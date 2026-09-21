// DPC bridge for RSP graphics ucodes that emit RDP commands directly via
// mtc0 to DPC_START/DPC_END (Factor 5 ucode does this). On DPC_END writes we
// forward [start..end) RDRAM bytes to RT64's RDP interpreter (LLE path,
// processDisplayLists with isHLE=false), matching what RT64 would consume from
// raw RDP command bytes.
//
// Reads of DPC_CURRENT/DPC_END return g_rsp_dpc_end so busy-wait loops in the
// ucode see "RDP completed instantly" and exit.
//
// This file is the LOAD-BEARING core: address masking, the incremental
// submit window, sync filtering, the OOB-CIMG mitigation, the cinematic
// fb-slot-ownership filter, and the final submit.

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include "os_compat.h"
#include <mutex>
#include <unordered_map>
#include "librecomp/rsp.hpp"
#include "ultramodern/events.hpp"
#include "dpc_bridge.h"

// upstream librecomp doesn't expose `ultramodern::submit_rdp_range` (a fork
// addition; defined in src/main/rt64_render_context.cpp) in any header, so
// forward-declare the one function the bridge needs.
namespace ultramodern {
    void submit_rdp_range(uint32_t lo_phys, uint32_t hi_phys);
}

// ---- DPC MMIO register state (read/written by the recompiled ucode) --------
uint32_t g_rsp_dpc_start = 0;
uint32_t g_rsp_dpc_end   = 0;

// Set when an RDP FULL_SYNC (op 0x29) is observed. The factor5_ucode dispatch
// loop polls this and force-returns Broke so task_thread_func can fire
// sp_complete() and the CPU's GFX_SCHED can advance to the next task (we don't
// model the hardware that halts the RSP externally between frames).
std::atomic<bool> g_rsp_full_sync_seen{false};

// RDRAM-relative address of the most recent real FULL_SYNC byte the bridge
// forwarded. rsp_force_fullsync() re-submits those 8 bytes so RT64 sees a
// fullSync at task-end.
static std::atomic<uint32_t> g_last_fullsync_addr{0xFFFFFFFFu};

// ---- Per-task / cumulative RDP counters ------------------------------------
// Per-task byte/command/fullsync counters; reset at the synthetic-halt site.
static std::atomic<uint32_t> g_task_rdp_bytes{0};
static std::atomic<uint32_t> g_task_rdp_cmds{0};
static std::atomic<uint32_t> g_task_rdp_fullsyncs{0};
// Per-task RDP opcode histogram (low 6 bits of first byte). Bridge and
// task_log_and_reset both run on the RSP task thread, so a plain array is fine.
uint32_t g_task_op_count[64] = {0};

extern "C" void rs64_dpc_drain_histogram(uint32_t out[64]) {
    for (int i = 0; i < 64; ++i) { out[i] = g_task_op_count[i]; g_task_op_count[i] = 0; }
}

// Cumulative (session-wide, never reset) opcode histogram + FULL_SYNC count.
// Dumped by the SEH crash handler in rt64_render_context.cpp.
static std::atomic<uint32_t> g_cumulative_op_count[64] = {};
extern "C" void rs64_dpc_get_cumulative_histogram(uint32_t out[64]) {
    for (int i = 0; i < 64; ++i)
        out[i] = g_cumulative_op_count[i].load(std::memory_order_relaxed);
}
static std::atomic<uint32_t> g_cumulative_fullsyncs{0};
extern "C" uint32_t rs64_dpc_get_cumulative_fullsyncs() {
    return g_cumulative_fullsyncs.load(std::memory_order_relaxed);
}

// Per-task GFX-opcode + G_MOVEMEM-index histograms, incremented directly by
// the factor5_ucode dispatch loop. Read by the per-task diagnostic report.
extern "C" uint32_t g_task_gfx_op_count[256] = {0};
extern "C" uint32_t g_task_movemem_idx_count[256] = {0};

// Currently-executing cinematic effect slot (0-5, or -1). Set by the
// recompiled slot dispatcher (funcs_8.c); read here to tag per-fb ownership.
extern "C" int g_cine_current_slot = -1;

// PHASE 21 engine-aware filter: per-fb slot ownership. Records, for each fb
// address, the slot index active when the most recent SET_COLOR_IMAGE targeting
// it was emitted. RT64's framebuffer_renderer queries rt64_cine_fb_is_slot_owned
// to suppress the cinematic's full-screen erasure fills (slot >= 0) while
// keeping legitimate clear-then-redraw fills (slot == -1).
static std::mutex g_cine_fb_owner_mutex;
static std::unordered_map<uint32_t, int> g_cine_fb_owner_slot;

extern "C" bool rt64_cine_fb_is_slot_owned(uint32_t addr) {
    std::lock_guard<std::mutex> lk(g_cine_fb_owner_mutex);
    auto it = g_cine_fb_owner_slot.find(addr & 0x00FFFFFFu);
    return (it != g_cine_fb_owner_slot.end()) && (it->second >= 0);
}

namespace {

inline uint32_t be_w(uint8_t* rdram, int64_t mips, int off) {
    return ((uint32_t)(uint8_t)MEM_B(off + 0, mips) << 24) |
           ((uint32_t)(uint8_t)MEM_B(off + 1, mips) << 16) |
           ((uint32_t)(uint8_t)MEM_B(off + 2, mips) <<  8) |
           ((uint32_t)(uint8_t)MEM_B(off + 3, mips));
}

// ROGUESQ_PRIM_FF: experimental override — force PRIM_COLOR RGB to FF FF FF
// (keep alpha) to test whether the warm off-white PRIM tint accounts for the
// residual "less saturated reds" gap. Skips the engine-glow outlier so orange
// flames don't go white. Writes the forwarded RDRAM bytes in place.
void dpc_apply_prim_override(uint8_t* rdram, uint32_t submit_lo) {
    static const bool prim_full = []{
        const char *e = recomp::os::getenv("ROGUESQ_PRIM_FF");
        return e && *e && *e != '0';
    }();
    if (!prim_full) return;
    int64_t mips = (int64_t)(int32_t)(submit_lo + 0x80000000);
    uint32_t w1 = be_w(rdram, mips, 4);
    bool is_glow = ((w1 >> 24) & 0xFF) < 0xC0 || ((w1 >> 8) & 0xFF) < 0x40;
    if (is_glow) return;
    rdram[(submit_lo + 4) ^ 3] = 0xFF;   // R
    rdram[(submit_lo + 5) ^ 3] = 0xFF;   // G
    rdram[(submit_lo + 6) ^ 3] = 0xFF;   // B (alpha at offset 7 preserved)
    static std::atomic<uint64_t> s_overridden{0};
    uint64_t v = ++s_overridden;
    if (v <= 3) {
        fprintf(stderr, "[dpc] PRIM override -> FF FF FF %02X (#%llu)\n",
            (w1 >> 0) & 0xFF, (unsigned long long)v);
        fflush(stderr);
    }
}

// PHASE 21: record which cinematic slot owns the fb a SET_COLOR_IMAGE targets.
void dpc_track_fb_ownership(uint8_t* rdram, uint32_t submit_lo) {
    int64_t mips = (int64_t)(int32_t)(submit_lo + 0x80000000);
    uint32_t addr24 = be_w(rdram, mips, 4) & 0x00FFFFFF;
    std::lock_guard<std::mutex> lk(g_cine_fb_owner_mutex);
    g_cine_fb_owner_slot[addr24] = g_cine_current_slot;
}

// Drop isolated 8-byte SET_COLOR_IMAGE commands whose address is outside
// legitimate framebuffer space. Returns true if the command was dropped.
//   HIGH (>= 0x800000)            : past 8MB RDRAM (0x00FFFFFF garbage)
//   MID  ([0x100000, 0x400000))   : matpool/heap region — Factor 5 cinematic
//                                    ucode lands garbage CIMGs here (verified
//                                    2026-05-20); no legitimate use.
//   LOW  (< 0x100000)             : asset/code region — Factor 5 legitimately
//                                    emits lowmem CIMGs for the 3D logo; the
//                                    rt64_state.cpp writeback guard filters
//                                    those, so let them through here.
// HIGH + LOW gated by ROGUESQ_SUPPRESS_OOB_CIMG; MID gated by ROGUESQ_DROP_MID_CIMG.
bool dpc_suppress_oob_cimg(uint8_t* rdram, uint32_t submit_lo, uint32_t submit_hi) {
    static const bool s_suppress = []{
        const char* v = recomp::os::getenv("ROGUESQ_SUPPRESS_OOB_CIMG");
        return v && *v && *v != '0';
    }();
    static const bool s_drop_mid = []{
        const char* v = recomp::os::getenv("ROGUESQ_DROP_MID_CIMG");
        return v && *v && *v != '0';
    }();
    if ((submit_hi - submit_lo) != 8) return false;
    int64_t mp = (int64_t)(int32_t)(submit_lo + 0x80000000);
    if ((uint8_t)MEM_B(0, mp) != 0xFF) return false;
    uint32_t addr24 = be_w(rdram, mp, 4) & 0x00FFFFFF;
    const bool drop_high = s_suppress && (addr24 >= 0x00800000u);
    const bool drop_mid  = s_drop_mid && (addr24 >= 0x00100000u && addr24 < 0x00400000u);
    const bool drop_low  = s_suppress && (addr24 != 0 && addr24 < 0x00100000u);
    if (!(drop_high || drop_mid || drop_low)) return false;
    static std::atomic<uint64_t> s_drop_h{0}, s_drop_m{0}, s_drop_l{0};
    const char* band = drop_high ? "HIGH" : (drop_mid ? "MID" : "LOW");
    std::atomic<uint64_t>& ctr = drop_high ? s_drop_h : (drop_mid ? s_drop_m : s_drop_l);
    uint64_t n = ++ctr;
    if (n == 1 || (n & (n - 1)) == 0) {
        fprintf(stderr, "[cimg-drop] %s #%llu submit_lo=0x%08X addr=0x%06X\n",
            band, (unsigned long long)n, submit_lo, addr24);
        fflush(stderr);
    }
    return true;
}

} // namespace

// ---------------------------------------------------------------------------
// Core: forward the unseen tail of the DPC byte range to RT64. The ucode emits
// one RDP command at a time and bumps DPC_END after each; we mirror real RDP by
// only forwarding bytes RT64 hasn't consumed yet, resetting on a new DPC_START.
// ---------------------------------------------------------------------------
void rsp_dpc_submit(uint8_t* rdram, uint32_t start, uint32_t end) {
    if (end <= start) return;

    // RT64 expects RDRAM-relative addresses; Factor 5 writes KSEG0 — mask to 24b.
    uint32_t start_phys = start & 0x3FFFFFF;
    uint32_t end_phys   = end   & 0x3FFFFFF;

    static uint32_t s_dl_base = 0;
    static uint32_t s_last_end = 0;
    if (start_phys != s_dl_base) { s_dl_base = start_phys; s_last_end = start_phys; }
    if (end_phys <= s_last_end) return;

    uint32_t submit_lo = s_last_end;
    uint32_t submit_hi = end_phys;
    s_last_end = end_phys;

    g_task_rdp_bytes.fetch_add(submit_hi - submit_lo, std::memory_order_relaxed);
    g_task_rdp_cmds.fetch_add(1, std::memory_order_relaxed);
    {
        int64_t mips_first = (int64_t)(int32_t)(submit_lo + 0x80000000);
        uint8_t op = (uint8_t)MEM_B(0, mips_first) & 0x3F;
        g_task_op_count[op]++;
        g_cumulative_op_count[op].fetch_add(1, std::memory_order_relaxed);
    }

    const uint32_t span = submit_hi - submit_lo;
    if (span == 8) {
        int64_t mips = (int64_t)(int32_t)(submit_lo + 0x80000000);
        uint8_t op6 = (uint8_t)MEM_B(0, mips) & 0x3F;
        // PIPESYNC (0x27): RT64 maintains pipeline coherence implicitly — drop
        // it so per-cmd PIPESYNC doesn't flood the action_queue. (Keep LOADSYNC
        // 0x26 / TILESYNC 0x28 / FULLSYNC 0x29.)
        if (op6 == 0x27) { (void)rdram; return; }
        // FULL_SYNC (0x29): end of a frame's RDP work — signal the ucode
        // dispatcher to break out, and cache the bytes for rsp_force_fullsync.
        if (op6 == 0x29) {
            g_rsp_full_sync_seen.store(true, std::memory_order_release);
            g_task_rdp_fullsyncs.fetch_add(1, std::memory_order_relaxed);
            g_cumulative_fullsyncs.fetch_add(1, std::memory_order_relaxed);
            g_last_fullsync_addr.store(submit_lo, std::memory_order_release);
        }
        if (op6 == 0x3A) dpc_apply_prim_override(rdram, submit_lo);   // PRIM_FF (LB)
        if (op6 == 0x3F) dpc_track_fb_ownership(rdram, submit_lo);    // fb-slot map (LB)
    }

    if (dpc_suppress_oob_cimg(rdram, submit_lo, submit_hi)) return;

    ultramodern::submit_rdp_range(submit_lo, submit_hi);
}

// Called from the factor5_ucode synthetic-halt site: drain the per-task
// counters, emit the diagnostic report, then reset the histograms for the
// next task.
extern "C" void rsp_task_log_and_reset(uint32_t iters, uint32_t data_size, uint32_t r17,
                                       uint32_t cmd_w0, uint32_t cmd_w1) {
    (void)iters; (void)data_size; (void)r17; (void)cmd_w0; (void)cmd_w1;
    g_task_rdp_bytes.store(0, std::memory_order_relaxed);
    g_task_rdp_cmds.store(0, std::memory_order_relaxed);
    g_task_rdp_fullsyncs.store(0, std::memory_order_relaxed);
    for (int i = 0; i < 64; i++)  g_task_op_count[i] = 0;
    for (int i = 0; i < 256; i++) g_task_gfx_op_count[i] = 0;
    for (int i = 0; i < 256; i++) g_task_movemem_idx_count[i] = 0;
}

// Submit a synthetic FULL_SYNC by re-using the bytes of the most recent real
// one, for cinematic-phase tasks that don't naturally emit op 0x29. The
// action_queue is FIFO, so all prior submit_rdp_range calls process first and
// this fullSync runs after the task's geometry. Paired with the LoadOperation
// validity check in rt64_state.cpp so partial tile state is skipped, not crashed.
// ROGUESQ_NO_SYNTH_FULLSYNC=1 disables this (reverts to the slow real-FULL_SYNC
// path, ~5 fps cinematic) for A/B comparison.
extern "C" void rsp_force_fullsync() {
    static const bool disabled = []{
        const char *e = recomp::os::getenv("ROGUESQ_NO_SYNTH_FULLSYNC");
        bool d = (e != nullptr && *e != '\0' && *e != '0');
        if (d) { fprintf(stderr, "[dpc] synthetic FULL_SYNC injection DISABLED via env\n"); fflush(stderr); }
        return d;
    }();
    if (disabled) return;
    uint32_t addr = g_last_fullsync_addr.load(std::memory_order_acquire);
    if (addr == 0xFFFFFFFFu) return;
    ultramodern::submit_rdp_range(addr, addr + 8);
}
