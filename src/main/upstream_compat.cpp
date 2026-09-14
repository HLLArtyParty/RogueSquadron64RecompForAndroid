// Upstream-librecomp gap-filler: libultra _recomp bindings that the
// recompiled game calls but upstream librecomp doesn't provide. Stubbed
// to safe defaults — none of these primitives have meaningful semantics
// on a non-N64 host (cache management, controller pak access, etc.).
//
// All taken verbatim from MikeSemicolonD/N64ModernRuntime fork additions.
// Lifted into our repo so we don't have to maintain a librecomp fork.

#include <cstdio>
#include <cstdlib>
#include <thread>
#include <mutex>
#include <map>
#include <string>

#ifdef _WIN32
// For RtlCaptureStackBackTrace + GetCurrentThreadId in the task-submit
// stack-trace gate. Keep these out of broader code so we don't pull in
// windows.h elsewhere.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <dbghelp.h>
#include <timeapi.h>
#include <tlhelp32.h>
#pragma comment(lib, "dbghelp.lib")
#pragma comment(lib, "winmm.lib")
#endif

#include <atomic>

#include "recomp.h"
#include "librecomp/helpers.hpp"
#include "librecomp/overlays.hpp"
#include "ultramodern/ultramodern.hpp"

// Runtime overlay registration. RS64 ships three overlays that all load at
// VA 0x800A5130 and swap at runtime: .ovl.mission, .ovl.menu, .ovl.cinematic.
// librecomp's boot-time load_overlays(0x1000, entrypoint, 1MB) only covers
// ROM offsets below 0x101000 — that registers .ovl.mission (rom 0xA5D30) but
// NOT .ovl.menu (rom 0x10C2D0) or .ovl.cinematic (rom 0x137580). Without this
// hook, calls into the menu/cinematic overlay regions keep dispatching to the
// mission overlay's recompiled functions, so the menu overlay's distinct code
// (attribution draw, menu screens) never runs.
//
// Called from a [[patches.hook]] on the game's loadOverlay (0x80000B20) with
// the overlay id in $a0. We unload whatever currently occupies the shared VA,
// then register the requested overlay's functions into func_map so subsequent
// direct calls resolve correctly. VA is always 0x800A5130; per-overlay ROM
// offset + size are the section_table entries from recomp_overlays.inl.
// Which runtime overlay is currently mapped at 0x800A5130: 0=mission/gameplay,
// 1=menu, 2=cinematic, -1=none yet. The F3DFACTOR5 op_01 handler reads this to
// load matrices into RT64's stack ONLY in the menu (overlay 1), where the menu's
// inherited-triangle tiles need them; doing so during the cinematic (overlay 2,
// which renders via the custom op_04/op_13 path) corrupts RT64 state and crashes.
extern "C" volatile int g_active_overlay = -1;

extern "C" void rs64_load_overlay(unsigned int overlay_id) {
    // Largest overlay size (mission, 0x665A0) — unload_overlays cleanly drops
    // any smaller overlay fully contained in this range.
    unload_overlays(0x800A5130, 0x000665A0);
    switch (overlay_id) {
        case 0: load_overlays(0x000A5D30, 0x800A5130, 0x000665A0); break; // .ovl.mission
        case 1: load_overlays(0x0010C2D0, 0x800A5130, 0x000283F0); break; // .ovl.menu
        case 2: load_overlays(0x00137580, 0x800A5130, 0x0000B810); break; // .ovl.cinematic
        default: break;
    }
    if (overlay_id <= 2) g_active_overlay = (int)overlay_id;
    static int s_log = -1;
    if (s_log < 0) {
        const char* e = std::getenv("ROGUESQ_LOG_OVERLAY");
        s_log = (e && *e && *e != '0') ? 1 : 0;
    }
    if (s_log) {
        fprintf(stderr, "[overlay] loadOverlay(%u) -> registered functions in func_map\n", overlay_id);
        fflush(stderr);
    }
}

extern "C" void __osContRamRead_recomp(uint8_t* /*rdram*/, recomp_context* ctx) {
    _return<s32>(ctx, -1);  // no rumble pak
}

extern "C" void __osContRamWrite_recomp(uint8_t* /*rdram*/, recomp_context* ctx) {
    _return<s32>(ctx, -1);  // no rumble pak
}

extern "C" void __osPfsSelectBank_recomp(uint8_t* /*rdram*/, recomp_context* ctx) {
    _return<s32>(ctx, 1);  // PFS_ERR_NOPACK — no memory pak
}

extern "C" void osDpGetCounters_recomp(uint8_t* rdram, recomp_context* ctx) {
    // Zero out the 8 DP counters. buf_ptr stays gpr (64-bit) so the
    // sign-extended MIPS address survives MEM_W's KSEG0 unmasking.
    gpr buf_ptr = ctx->r4;
    for (int i = 0; i < 8; i++) {
        MEM_W(i * 4, buf_ptr) = 0;
    }
}

extern "C" void osViGetCurrentField_recomp(uint8_t* /*rdram*/, recomp_context* ctx) {
    ctx->r2 = 0;  // always field 0 (progressive / non-interlaced)
}

// Override upstream's `assert(false)` in ultra_translation.cpp:32. The real
// libultra osYieldThread voluntarily yields to the scheduler. On the host
// the closest equivalent is std::this_thread::yield(); blocking briefly is
// fine — the game only calls this from idle/wait paths.
//
// This symbol is defined in upstream librecomp too (with the assert), but
// the linker picks our definition first because /FORCE:MULTIPLE is enabled
// (already used for the patches/heap_guards.c override). Same trick.
// ROGUESQ_LOG_MESG_TRACE=1: thread/message order trace for cinematic frames 60-70 (game frame counter
// 0x8013889C) → logs/mesg_trace_recomp.csv, same columns as the PJ64 hook in tools/validate/pj64_rs64_dump.js:
// frame,event,threadId,queue,flags,ra. Compare the two to find where the recomp reorders or drops messages.
extern "C" OSId osGetThreadId(uint8_t* rdram, PTR(OSThread) t);
static const char* rs64_host_caller_name(void* addr) {
    static std::map<void*, std::string> cache;
    auto it = cache.find(addr);
    if (it != cache.end()) return it->second.c_str();
    static bool init = [](){ SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS); return SymInitialize(GetCurrentProcess(), nullptr, TRUE) != 0; }();
    char buf[sizeof(SYMBOL_INFO) + 256]; SYMBOL_INFO* sym = reinterpret_cast<SYMBOL_INFO*>(buf);
    sym->SizeOfStruct = sizeof(SYMBOL_INFO); sym->MaxNameLen = 255; DWORD64 disp = 0;
    char out[300];
    if (init && SymFromAddr(GetCurrentProcess(), (DWORD64)(uintptr_t)addr, &disp, sym)) snprintf(out, sizeof out, "%s+0x%llx", sym->Name, (unsigned long long)disp);
    else snprintf(out, sizeof out, "rva%llx", (unsigned long long)((uintptr_t)addr - (uintptr_t)GetModuleHandleW(nullptr)));
    return cache.emplace(addr, out).first->second.c_str();
}
// Window: ROGUESQ_MESG_TRACE_FRAMES=lo-hi (default 60-70). Extra columns after the hw-compatible six:
// effective flags (after the NOBLOCK override), host thread id, host caller (the generated function).
static void rs64_mesg_trace(uint8_t* rdram, const char* ev, uint32_t q, int flags, uint32_t ra, int eff_flags = -1) {
    static const bool on = [](){ const char* e = std::getenv("ROGUESQ_LOG_MESG_TRACE"); return e && *e && *e != '0'; }();
    if (!on) return;
    static uint32_t lo = 60, hi = 70;
    static const bool win = [](){ const char* e = std::getenv("ROGUESQ_MESG_TRACE_FRAMES"); if (e && *e) { unsigned a, b; if (sscanf(e, "%u-%u", &a, &b) == 2) { lo = a; hi = b; } } return true; }();
    (void)win;
    uint32_t frame = *reinterpret_cast<const uint32_t*>(rdram + 0x13889C);
    if (frame < lo || frame > hi) return;
    static std::mutex m; std::lock_guard<std::mutex> lk(m);
    static FILE* f = fopen("../../logs/mesg_trace_recomp.csv", "w");
    if (!f) return;
    void* frames[4]; USHORT n = RtlCaptureStackBackTrace(2, 4, frames, nullptr);
    const char* caller = n ? rs64_host_caller_name(frames[0]) : "?";
    if (ev[0] == 'y' && strncmp(caller, "waitForMusyXAudioTaskDone", 25) == 0) { static unsigned spin = 0; if (++spin % 1000 != 1) return; }
    PTR(OSThread) self = ultramodern::this_thread();
    int tid = (self >= 0x80000000u && self < 0x80800000u) ? (int)TO_PTR(OSThread, self)->id : -1;   // bootstrap thread has none
    fprintf(f, "%u,%s,%d,%x,%d,%x,%d,%lu,%s\n", frame, ev, tid, q, flags, ra,
            eff_flags < 0 ? flags : eff_flags, GetCurrentThreadId(), caller);
    fflush(f);
}

// Guard-fallback counter (2026-09-07): the KSEG0 "guard" hooks baked into RecompiledFuncs replace nop'd
// loads/stores with checked ones that silently fall back to 0/skip. On hardware those loads always
// succeed, so every fallback is a divergence. Each fallback calls this; ROGUESQ_LOG_GUARDS=1 prints
// the first hits per tag and a per-tag summary every 5 s.
extern "C" void rs64_guard_hit(const char* tag, unsigned a, unsigned b) {
    static const bool on = [](){ const char* e = std::getenv("ROGUESQ_LOG_GUARDS"); return e && *e && *e != '0'; }();
    static std::mutex m; std::lock_guard<std::mutex> lk(m);
    static std::map<std::string, unsigned> counts; static uint64_t last_summary = 0;
    unsigned n = ++counts[tag];
    if (!on) return;
    if (n <= 3) { fprintf(stderr, "[guard-hit] %s #%u a=0x%08X b=0x%08X\n", tag, n, a, b); fflush(stderr); }
    uint64_t now = GetTickCount64();
    if (now - last_summary > 5000) {
        last_summary = now;
        fprintf(stderr, "[guard-summary]");
        for (auto& kv : counts) fprintf(stderr, " %s=%u", kv.first.c_str(), kv.second);
        fprintf(stderr, "\n"); fflush(stderr);
    }
}

void dequeue_external_messages(uint8_t* rdram);   // ultramodern/src/mesgqueue.cpp, C++ linkage, not in the header
// (2026-09-07) Real libultra semantics: re-queue this thread at its priority and switch to the
// highest-priority runnable thread. The old std::this_thread::yield() kept the N64 scheduler slot,
// so any game spin-loop (e.g. the MusyX shutdown wait, the post-swap ack wait) starved every other
// N64 thread forever. ROGUESQ_HOST_YIELD_ONLY=1 restores the old behavior.
extern "C" void osYieldThread_recomp(uint8_t* rdram, recomp_context* ctx) {
    rs64_mesg_trace(rdram, "yield", 0, 0, (uint32_t)ctx->r31);
    static const bool s_old = [](){ const char* e = std::getenv("ROGUESQ_HOST_YIELD_ONLY"); return e && *e && *e != '0'; }();
    if (s_old) { std::this_thread::yield(); return; }
    // Service pending "interrupts" first: host VI/DP/AI events are queued as external messages and are
    // otherwise only drained inside osRecvMesg/osSendMesg or when the run queue is empty, so a spinning
    // thread would never let the VI thread wake. If nothing else is runnable, wait briefly for one.
    dequeue_external_messages(rdram);
    if (ultramodern::thread_queue_empty(rdram, ultramodern::running_queue)) {
        ultramodern::wait_for_external_message_timed(rdram, 1);
        dequeue_external_messages(rdram);
    }
    ultramodern::schedule_running_thread(rdram, ultramodern::this_thread());
    ultramodern::run_next_thread_and_wait(rdram);
}

// osStartThread pass-through with trace (upstream librecomp defines it too; /FORCE:MULTIPLE picks ours).
extern "C" void osStartThread(uint8_t* rdram, PTR(OSThread) t);
extern "C" void osStartThread_recomp(uint8_t* rdram, recomp_context* ctx) {
    rs64_mesg_trace(rdram, "start", (uint32_t)ctx->r4, 0, (uint32_t)ctx->r31);
    osStartThread(rdram, (int32_t)ctx->r4);
}

// RS64 fix: the per-frame gfx-barrier osRecvMesg in submitGfxFrame (funcs_3.c:13283,
// queues D_8011A7E8 / D_8011A818) is called with BLOCK. Once the game reaches the MENU
// (menuOverlayInit → menuControllerInput → submitGfxFrame), this barrier stalls: the
// VI-prime only delivers a frame token ~1/sec, so the menu loop crawls at ~1 FPS (looks
// like a freeze; watchdog caught it blocked here at iter 966). Forcing these two barrier
// receives to NOBLOCK lets the loop run free (as the boot NOBLOCK patches did) so the menu
// is responsive. Other queues are unaffected. ROGUESQ_GFX_BARRIER_NOBLOCK=0 to disable.
// ROGUESQ_VI_DRIVEN_LOOP=1 (opt-in, default OFF): run the game frame loop the hardware way — blocking
// frame-barrier receives, real post-swap ack wait, no host pacing sleep. Per-frame timing then matches
// PJ64 exactly, but a 3-way wait (retrace thread on 0x8011A800 <- SP scheduler on 0x8011A420 <- game
// thread on the video queue 0x80128CF0) still deadlocks the cinematic after ~100-140 frames (2026-09-07).
extern "C" int rs64_vi_driven(void) {
    static int s = -1;
    // DEFAULT ON since 2026-09-07: the VI-driven loop reproduces the hardware frame protocol
    // (validated by message-order trace + goldens) and reaches the menu; ROGUESQ_VI_DRIVEN_LOOP=0 restores
    // the old host-token/NOBLOCK pacing for A/B.
    if (s < 0) { const char* e = std::getenv("ROGUESQ_VI_DRIVEN_LOOP"); s = (e && *e && *e == '0') ? 0 : 1; }
    return s;
}

// ROGUESQ_FB_GUARDS=0 disables the host-side "framebuffer window" guards (matpool CIMG neutralizer,
// matpool repair, RT64 fb-registry sanitizer). They assume no game data lives in [0x400000,0x800000);
// that was only true because of the retired sample-bank hack. A/B them against the PJ64 goldens.
// Value is a bitmask: 1 = matpool CIMG neutralizer, 2 = matpool repair, 4 = fb-registry sanitizer; "0" = none, unset = all.
extern "C" int rs64_fb_guards_mask(void) {
    static int s = -1;
    if (s < 0) { const char* e = std::getenv("ROGUESQ_FB_GUARDS"); s = (e && *e) ? std::atoi(e) : 6; }   // 2026-09-07: neutralizer (bit 1) off by default: it rewrote payload words; repair + sanitizer stay
    return s;
}
extern "C" int rs64_fb_guards(void) { return rs64_fb_guards_mask() != 0; }

// Host-side symbolized backtrace for probes in generated code (regen-fragile hand edits call this).
extern void print_stack_with_symbols(void** frames, USHORT count);
extern "C" void rs64_dbg_backtrace(const char* tag) {
    void* frames[24];
    USHORT count = RtlCaptureStackBackTrace(1, 24, frames, nullptr);
    fprintf(stderr, "[bt] %s\n", tag);
    print_stack_with_symbols(frames, count);
    fflush(stderr);
}

extern "C" void rs64_wait_gfx_parse(void);   // ultramodern events.cpp: wait for the in-flight RT64 parse
extern "C" int rs64_gfx_parse_inflight_wait(int ms);
// Wait for the in-flight RT64 parse from an N64 thread: yield between short slices so higher-priority
// N64 threads (retrace thread: buffer swap + audio synth) run meanwhile, as hardware preemption would let them.
extern "C" volatile unsigned g_rs64_pw_calls = 0, g_rs64_pw_waited = 0, g_rs64_pw_ms = 0, g_rs64_pw_timeouts = 0;
extern "C" void rs64_wait_gfx_parse_yield(uint8_t* rdram, recomp_context* ctx) {
    ++g_rs64_pw_calls;
    const DWORD t0 = GetTickCount();
    for (int i = 0; i < 500; ++i) {
        if (!rs64_gfx_parse_inflight_wait(1)) { if (i) { ++g_rs64_pw_waited; g_rs64_pw_ms += GetTickCount() - t0; } return; }
        osYieldThread_recomp(rdram, ctx);
    }
    ++g_rs64_pw_timeouts; g_rs64_pw_ms += GetTickCount() - t0;
}
extern "C" void osRecvMesg_recomp(uint8_t* rdram, recomp_context* ctx) {
    s32 flags = (s32)ctx->r6;
    const uint32_t q = (uint32_t)ctx->r4;
    static int s_nb = -1;
    if (s_nb < 0) { const char* e = std::getenv("ROGUESQ_GFX_BARRIER_NOBLOCK"); s_nb = (e && e[0] == '0') ? 0 : 1; }
    if (s_nb && !rs64_vi_driven() && (q == 0x8011A7E8u || q == 0x8011A818u)) flags = 0;  // OS_MESG_NOBLOCK
    rs64_mesg_trace(rdram, "recv", q, (int)(s32)ctx->r6, (uint32_t)ctx->r31, (int)flags);
    { static int s_lg = 0; if (s_lg < 12) { ++s_lg; fprintf(stderr, "[osRecvMesg_ovr] q=0x%08X flags=%d->%d\n", q, (s32)ctx->r6, flags); fflush(stderr); } }
    // ROGUESQ_LOG_RECV_BLOCK=1: report BLOCK receives on an empty queue and how long they took (frame-barrier diagnosis).
    static const bool s_rb = [](){ const char* e = std::getenv("ROGUESQ_LOG_RECV_BLOCK"); return e && *e && *e != '0'; }();
    uint32_t t0 = 0; bool empty = false;
    if (s_rb && flags != 0 && q >= 0x80000000u && q < 0x80800000u) {
        empty = (*reinterpret_cast<uint32_t*>(rdram + ((q & 0x7FFFFFu) + 8)) == 0);   // validCount
        if (empty) { t0 = GetTickCount(); fprintf(stderr, "[recv-block] tid=%lu q=0x%08X waiting (empty)\n", GetCurrentThreadId(), q); fflush(stderr); }
    }
    ctx->r2 = osRecvMesg(rdram, (int32_t)ctx->r4, (int32_t)ctx->r5, flags);
    // Frame start (video queue 0x80128CF0, waitForPostSwapAck): the game is about to rebuild the frame
    // chunks of the buffer slot it just got back. On hardware the RSP finished reading them long ago;
    // RT64's parse can still be running (2026-09-08: snapshots walk clean, the live parse saw garbage).
    // Hold the game thread here until no graphics parse is in flight.
    if (q == 0x80128CF0u && ctx->r2 == 0 && rs64_vi_driven()) {
        static unsigned s_n = 0; ++s_n;
        const int before = rs64_gfx_parse_inflight_wait(0);
        rs64_wait_gfx_parse_yield(rdram, ctx);
        static const bool s_lg = [](){ const char* e = std::getenv("ROGUESQ_LOG_GFX_TASK"); return e && *e && *e != '0'; }();
        if (s_lg && (s_n <= 4 || (s_n & (s_n - 1)) == 0)) { fprintf(stderr, "[parse-wait] frame-recv #%u inflight-now=%d | all sites: calls=%u waited=%u timeouts=%u total=%u ms\n", s_n, before, g_rs64_pw_calls, g_rs64_pw_waited, g_rs64_pw_timeouts, g_rs64_pw_ms); fflush(stderr); }
    }
    // Trace the message TYPE the SP scheduler (0x8011A420) and DP handler (0x8011A408) dequeued: 1/2 = gfx/audio
    // request, 0x81 = SP-done event. Those event sends come from ultramodern, not osSendMesg, so they are invisible otherwise.
    if ((q == 0x8011A420u || q == 0x8011A408u) && ctx->r2 == 0) {
        uint32_t mp = (uint32_t)ctx->r5, mv = 0;
        if (mp >= 0x80000000u && mp < 0x80800000u) mv = *reinterpret_cast<uint32_t*>(rdram + (mp & 0x7FFFFFu));
        int type = (mv >= 0x80000000u && mv < 0x80800000u) ? rdram[(mv & 0x7FFFFFu) ^ 3] : -1;
        rs64_mesg_trace(rdram, "recvret", q, type, mv);
    }
    if (empty) { uint32_t dt = GetTickCount() - t0; if (dt > 200) { fprintf(stderr, "[recv-block] tid=%lu q=0x%08X resumed after %u ms\n", GetCurrentThreadId(), q, dt); fflush(stderr); } }
}

// RS64 fix: game calls osViBlack(1) twice during boot at funcs_0.c:2083 and
// :2142 and never osViBlack(0). That keeps ultramodern's VI_STATE_BLACK set
// forever → hStart forced to 0 → VI::visible() returns false in RT64 →
// PresentEarly matcher fails → no presents fire. Override the recomp wrapper
// so the game's calls are no-ops (visibility stays on by default).
extern "C" void osViBlack_recomp(uint8_t* /*rdram*/, recomp_context* ctx) {
    static int s_count = 0;
    int n = ++s_count;
    if (n <= 8) {
        fprintf(stderr, "[osViBlack #%d] active=%u (ignored — RS64 fix)\n",
                n, (uint32_t)ctx->r4);
        fflush(stderr);
    }
}

// Defined below; set by the menuOverlayInit hook (action 9 = attribution).
extern "C" volatile int g_current_scene;  // fwd decl (defined below); menu-screen id, 9 = attribution
extern "C" volatile unsigned g_last_swap_fb;  // defined below; set here per swap
extern "C" volatile unsigned g_op_bf_count;   // defined below; explosion-bloom tri counter (fb-dump trigger)
extern "C" volatile int g_explosion_hold;     // defined in rt64 GBI module; armed when fire tiles load

// ONE-FLAG bundle: ROGUESQ_EXPLOSION=1 enables the whole reliable-explosion config
// (HLE op_02, generic render, throttle, present-follow mode 4, CI4 de-swizzle,
// force-opaque, explosion-hold) instead of 7 separate env vars. Runs at program
// load (static init), before any of the lazy env checks read their vars.
static const int s_rogue_explosion_bundle = []() -> int {
    const char* e = std::getenv("ROGUESQ_EXPLOSION");
    if (e && *e && *e != '0') {
        auto set = [](const char* k, const char* v) { if (!std::getenv(k)) _putenv_s(k, v); };
        set("ROGUESQ_HLE_OP02_EXPERIMENTAL", "1");
        set("ROGUESQ_GENERIC_RENDER", "1");
        set("ROGUESQ_GR_THROTTLE", "4");
        set("ROGUESQ_VI_FOLLOW_DRAW", "4");
        set("ROGUESQ_CI4_DESWIZZLE", "1");
        set("ROGUESQ_HLE_FORCE_OPAQUE", "1");
        set("ROGUESQ_EXPLOSION_HOLD", "1");
        // FIRE_ADDITIVE removed: additive over the white bg washes to white.
        // Use ROGUESQ_FIRE_PROBE=1 to log the game's real blender/combiner.
        std::fprintf(stderr, "[rogue] ROGUESQ_EXPLOSION=1 -> enabled reliable-explosion bundle\n");
        std::fflush(stderr);
    }
    return 1;
}();

// Diagnostic: log osViSwapBuffer calls. The game tells VI which fb to display
// via this call. If the address doesn't match any prior SET_COLOR_IMAGE
// address, RT64 has no rendered content for it → black screen.
extern "C" void osViSwapBuffer_recomp(uint8_t* rdram, recomp_context* ctx) {
    static int s_count = 0;
    int n = ++s_count;
    uint32_t fb = (uint32_t)ctx->r4;

    // EXPERIMENT 2026-05-13: ROGUESQ_FB_REDIRECT_HIRES — if the game asks VI
    // to display a lo-res fb (0x806BA000 / 0x806DD000 are 320×224 buffers,
    // ~0x23000 apart), redirect to the hi-res scratch fb at 0x8062B800
    // instead. Tests the hypothesis that attribution text lives in the
    // hi-res scratch but VI is swapping to an empty lo-res target.
    //
    // Empirically: SET_CIMG fires for both fb classes per frame, but ALL
    // texrects target 0x62B800 — the lo-res buffers only receive fillRect
    // clears. So this redirect should pull the actual drawn content into
    // VI's sampling stream.
    static int s_redirect = -1;
    if (s_redirect < 0) {
        const char* v = std::getenv("ROGUESQ_FB_REDIRECT_HIRES");
        s_redirect = (v && *v && v[0] != '0') ? 1 : 0;
        if (s_redirect) {
            fprintf(stderr, "[fb-redirect] lo-res VI swaps will be redirected to 0x8062B800 (hi-res scratch)\n");
            fflush(stderr);
        }
    }
    if (s_redirect) {
        const uint32_t phys = fb & 0x00FFFFFF;
        // Lo-res-fb signature: addresses 0x6BA000 / 0x6DD000 / 0x7DD000
        // (under 0x800000, in the small-fb cluster). Hi-res scratch is at
        // 0x62B800 — leave that untouched; redirect anything else in the
        // "expected VI fb" range to the hi-res scratch.
        if ((phys == 0x6BA000) || (phys == 0x6DD000) || (phys == 0x7DD000)) {
            uint32_t orig = fb;
            fb = 0x8062B800;
            if (n <= 8 || (n & 63) == 0) {
                fprintf(stderr, "[fb-redirect #%d] 0x%08X -> 0x%08X\n", n, orig, fb);
                fflush(stderr);
            }
        }
    }

    // Attribution-screen VI redirect (see g_attribution_active). The handler
    // draws to 0x8076A000 but swaps a different pair; redirect so the
    // PresentEarly matcher (rendered colorImg.address == VI fbAddress) finds
    // the rendered attribution buffer instead of an empty display buffer.
    // 2026-09-08: the attribution VI redirect to 0x8076A000 is OFF by default. The game now renders the
    // attribution into its own pair (0x66A000/0x5D4000, identical to the hardware golden); redirecting the
    // VI to 0x76A000 showed a buffer nothing draws into (white page, no legal text). ROGUESQ_ATTRIB_VI_REDIRECT=1 restores.
    static const bool s_attrib_redirect = [](){ const char* e = std::getenv("ROGUESQ_ATTRIB_VI_REDIRECT"); return e && *e && *e != '0'; }();
    if (s_attrib_redirect && g_current_scene == 9 /* F5_SCENE_ATTRIBUTION */ && (fb & 0x00FFFFFFu) != 0x76A000u) {
        uint32_t orig = fb;
        fb = 0x8076A000u;
        if (n <= 200 || (n & 63) == 0) {
            fprintf(stderr, "[attrib-vi-redirect #%d] 0x%08X -> 0x%08X\n", n, orig, fb);
            fflush(stderr);
        }
    }

    // Record the buffer the game actually swapped to (post-redirect) so the VI
    // present can show THIS instead of the cycling intermediate-buffer VI_ORIGIN
    // writes the cinematic does (the flicker source). See g_last_swap_fb use in
    // update_screen (rt64_render_context.cpp).
    g_last_swap_fb = fb;
    if (n <= 200 || (n & 63) == 0) {
        fprintf(stderr, "[osViSwapBuffer #%d] fb=0x%08X\n", n, fb);
        fflush(stderr);
    }
    // ROGUESQ_DUMP_ARBITER=1: dump the buffer-arbiter framebuffer-pointer table
    // (D_80128E98[]) + per-slot state bytes (D_80128EAA[]) + counts. The present
    // logic (func_8001BE24) picks slot whose state matches "ready" and swaps
    // D_80128E98[slot]. Goal: see if the cinematic buffer 0x795C00 is in the table
    // and what state its slot holds (i.e. why it's never presented).
    {
        static int s_da = -1;
        if (s_da < 0) { const char* v = std::getenv("ROGUESQ_DUMP_ARBITER"); s_da = (v && *v && v[0] != '0') ? 0 : -2; }
        if (s_da >= 0) {
            // Compact per-swap dump of the 2 display-buffer table slots + states,
            // across the whole run, to catch when (if ever) a cinematic buffer
            // (0x795C00 / 0x290000) enters the display rotation.
            uint32_t p0 = (uint32_t)MEM_W(0, (gpr)(int32_t)(0x80128E98));
            uint32_t p1 = (uint32_t)MEM_W(0, (gpr)(int32_t)(0x80128E98 + 4));
            uint32_t s0 = (uint32_t)MEM_BU(0, (gpr)(int32_t)(0x80128EAA));
            uint32_t s1b = (uint32_t)MEM_BU(0, (gpr)(int32_t)(0x80128EAA + 1));
            // Only log when the table changes or every 32nd swap (avoid spam).
            static uint32_t s_lastp0 = 0, s_lastp1 = 0;
            if (p0 != s_lastp0 || p1 != s_lastp1 || (n & 31) == 0) {
                s_lastp0 = p0; s_lastp1 = p1; ++s_da;
                fprintf(stderr, "[arbiter #%d] disp[0]=0x%08X(st%u) disp[1]=0x%08X(st%u) curfb=0x%08X\n",
                    n, p0, s0, p1, s1b, fb);
                fflush(stderr);
            }
        }
    }
    // ROGUESQ_LOG_VI_FB_CONTENT=1: sample a few non-zero bytes from the fb
    // RDRAM to verify whether the game is writing attribution pixels into it
    // (vs the fb being empty and attribution rendering happening elsewhere).
    static int s_dump_fb = -1;
    if (s_dump_fb < 0) {
        const char* v = std::getenv("ROGUESQ_LOG_VI_FB_CONTENT");
        s_dump_fb = (v && *v && v[0] != '0') ? 1 : 0;
    }
    if (s_dump_fb && (n <= 64 || (n & 31) == 0)) {
        auto scan_fb = [&](uint32_t target_addr, const char* label) {
            const uint32_t phys = target_addr & 0x00FFFFFF;
            size_t nonzero = 0;
            uint32_t first_nz_off = 0xFFFFFFFFu;
            uint32_t first_nz_val = 0;
            const uint32_t scan = 320u * 224u * 2u;
            for (uint32_t i = 0; i < scan; i += 2) {
                uint32_t off = phys + i;
                uint8_t b0 = rdram[off ^ 3];
                uint8_t b1 = rdram[(off + 1) ^ 3];
                if (b0 || b1) {
                    if (first_nz_off == 0xFFFFFFFFu) {
                        first_nz_off = i;
                        first_nz_val = (uint32_t)((b0 << 8) | b1);
                    }
                    ++nonzero;
                }
            }
            fprintf(stderr, "[vi-fb-content #%d %s] addr=0x%08X nz_px=%zu/%u first@%u=0x%04X\n",
                    n, label, target_addr, nonzero, scan / 2u, first_nz_off, first_nz_val);
        };
        scan_fb(fb, "VI");
        // Also scan the hi-res scratch fb (0x8062B800) so we can see if
        // experimental op_02 output landed there.
        scan_fb(0x8062B800u, "hiRes");
        fflush(stderr);
    }
    // ROGUESQ_FB_DUMP=1: once op_bf (the explosion bloom) starts drawing, dump the cinematic
    // framebuffers from RDRAM to raw .bin files for the first few swaps, so the rendered bloom can
    // be decoded to PNG offline (bypasses the frozen visible present). RGBA5551, 640x340.
    {
        static int s_fbd = -1;
        if (s_fbd < 0) { const char* v = std::getenv("ROGUESQ_FB_DUMP"); s_fbd = (v && *v && v[0] != '0') ? 0 : -2; }
        if (s_fbd >= 0 && g_op_bf_count > 0 && s_fbd < 48) {
            const uint32_t addrs[2] = { 0x0062B800u, 0x00695C00u };
            for (int a = 0; a < 2; ++a) {
                char path[256];
                std::snprintf(path, sizeof(path), "logs/explosion/fbdump_%06X_%02d.bin", addrs[a], s_fbd);
                FILE* f = fopen(path, "wb");
                if (f) {
                    const uint32_t bytes = 640u * 340u * 2u;
                    for (uint32_t i = 0; i < bytes; ++i) fputc(rdram[(addrs[a] + i) ^ 3], f);
                    fclose(f);
                }
            }
            fprintf(stderr, "[fb-dump #%02d] bf_count=%u fb=0x%08X\n", s_fbd, g_op_bf_count, fb);
            fflush(stderr);
            ++s_fbd;
        }
        // One-shot: when the bloom is well underway (bf_count>=100), scan RDRAM in 64KB blocks and
        // report which blocks hold texture-like data (varied non-zero) — finds WHERE textures are
        // actually loaded, vs the empty addresses SETTIMG binds. Tells fixable-addressing from absent.
        static bool s_scanned = false;
        if (s_fbd >= 0 && !s_scanned && g_op_bf_count >= 100) {
            s_scanned = true;
            for (uint32_t base = 0x00100000u; base < 0x00800000u; base += 0x10000u) {
                uint32_t nz = 0; uint32_t distinct = 0; uint8_t seen[256] = {0};
                for (uint32_t i = 0; i < 0x10000u; i += 7) {
                    uint8_t v = rdram[(base + i) ^ 3];
                    if (v) nz++;
                    if (!seen[v]) { seen[v] = 1; distinct++; }
                }
                if (nz > 2000 && distinct > 20)  // varied, dense = texture/asset-like
                    fprintf(stderr, "[rdram-scan] 0x%08X nz=%u distinct=%u\n", 0x80000000u | base, nz, distinct);
            }
            fflush(stderr);
        }
    }
    // ROGUESQ_CART_FBDUMP=1: dump the attribution buffer 0x76A000 (where the cartridge draws) for a
    // few swaps during the attribution screen (g_current_scene==9), so the pak render is decodable
    // offline — the cartridge reaches render_one_model headless, so this is verifiable without a run.
    {
        static int s_cfd = -1;
        if (s_cfd < 0) { const char* v = std::getenv("ROGUESQ_CART_FBDUMP"); s_cfd = (v && *v && v[0] != '0') ? 0 : -2; }
        if (s_cfd >= 0 && g_current_scene == 9 && s_cfd < 12) {
            char path[256];
            std::snprintf(path, sizeof(path), "logs/explosion/cartfb_%02d.bin", s_cfd);
            FILE* f = fopen(path, "wb");
            if (f) { const uint32_t bytes = 640u * 480u * 2u;
                for (uint32_t i = 0; i < bytes; ++i) fputc(rdram[(0x0076A000u + i) ^ 3], f);
                fclose(f); }
            fprintf(stderr, "[cartfb #%02d] scene=%d fb=0x%08X\n", s_cfd, g_current_scene, fb);
            fflush(stderr);
            ++s_cfd;
        }
    }
    extern void osViSwapBuffer(uint8_t* rdram, int32_t frameBufPtr);
    osViSwapBuffer(rdram, (int32_t)fb);
}

// Diagnostic: log osViSetMode calls + decode the OS_VI_MODE struct contents.
// OS_VI_MODE layout (libultra):
//   type      u32 at +0x00
//   comRegs.ctrl/width/burst/vSync/hSync/leap/hStart/xScale/vCurrent (9 u32) +0x04
//     → width at +0x08, xScale at +0x20
//   fldRegs[0].origin/yScale/vStart/vBurst/vIntr (5 u32) at +0x28
//     → yScale at +0x2C
//   fldRegs[1] at +0x3C (same layout)
// Total 80 bytes. We dump the first 80 bytes raw + key fields decoded so we
// can tell which mode is lo-res (width=320, xScale~0x200) vs hi-res
// (width=640, xScale~0x400).
// __osInitialize_common is HLE'd by librecomp (calls host osInitialize) and never touches the
// game's libultra globals. Real libultra leaves osClockRate = OS_CPU_COUNTER (46875000) after boot
// (PJ64 golden: 0x80039100 = 46875000), but the ROM's static initializer is OS_CLOCK_RATE
// (62500000), so every osGetTime-derived delta ran at 3/4 speed (cinematic timeline 81 vs 123
// ticks at frame 120). Match hardware.
extern "C" void osInitialize(void);
static void rs64_os_initialize(uint8_t* rdram) {
    osInitialize();
    *reinterpret_cast<uint32_t*>(rdram + 0x39100) = 0u;          // osClockRate (u64) high word
    *reinterpret_cast<uint32_t*>(rdram + 0x39104) = 46875000u;   // osClockRate low word = OS_CPU_COUNTER
    fprintf(stderr, "[os-init] osClockRate set to 46875000 (hardware value)\n");
}
extern "C" void __osInitialize_common_recomp(uint8_t* rdram, recomp_context* ctx) { rs64_os_initialize(rdram); }
extern "C" void osInitialize_recomp(uint8_t* rdram, recomp_context* ctx) { rs64_os_initialize(rdram); }

extern "C" void osViSetMode_recomp(uint8_t* rdram, recomp_context* ctx) {
    static int s_count = 0;
    int n = ++s_count;
    uint32_t mode_ptr = (uint32_t)ctx->r4;
    uint32_t rdram_off = mode_ptr & 0x00FFFFFF;
    auto read_u32 = [&](uint32_t off) -> uint32_t {
        uint32_t v = 0;
        for (int i = 0; i < 4; ++i) {
            v = (v << 8) | rdram[(rdram_off + off + i) ^ 3];
        }
        return v;
    };
    uint32_t type    = read_u32(0x00);
    uint32_t ctrl    = read_u32(0x04);
    uint32_t width   = read_u32(0x08);
    uint32_t xScale  = read_u32(0x20);
    uint32_t yScale  = read_u32(0x2C);
    fprintf(stderr,
        "[osViSetMode #%d] mode_ptr=0x%08X  type=0x%08X ctrl=0x%08X "
        "width=%u xScale=0x%08X yScale=0x%08X\n",
        n, mode_ptr, type, ctrl, width, xScale, yScale);
    fflush(stderr);
    extern void osViSetMode(uint8_t* rdram, int32_t modePtr);
    osViSetMode(rdram, (int32_t)ctx->r4);
}

extern "C" void osViSetXScale_recomp(uint8_t* rdram, recomp_context* ctx) {
    static int s_count = 0;
    int n = ++s_count;
    if (n <= 4) {
        fprintf(stderr, "[osViSetXScale #%d] scale=%f\n", n, (double)ctx->f12.fl);
        fflush(stderr);
    }
    extern void osViSetXScale(float scale);
    osViSetXScale(ctx->f12.fl);
}

extern "C" void osViSetYScale_recomp(uint8_t* rdram, recomp_context* ctx) {
    static int s_count = 0;
    int n = ++s_count;
    if (n <= 4) {
        fprintf(stderr, "[osViSetYScale #%d] scale=%f\n", n, (double)ctx->f12.fl);
        fflush(stderr);
    }
    extern void osViSetYScale(float scale);
    osViSetYScale(ctx->f12.fl);
}

// osGetMemSize override — defaults to 8MB (Expansion Pak present, hi-res
// mode available). ROGUESQ_MEM_SIZE_MB=4 forces 4MB to test base-N64 lo-res
// path. 2026-05-13 test confirmed 4MB alone crashes boot in mainBootstrapWorker
// (game still goes hi-res because the mode flag is gated on more than memsize),
// so the env var is a knob, not a recommended setting.
extern "C" void osGetMemSize_recomp(uint8_t* /*rdram*/, recomp_context* ctx) {
    static int s_mb = -1;
    if (s_mb < 0) {
        const char* v = std::getenv("ROGUESQ_MEM_SIZE_MB");
        s_mb = (v && *v) ? std::atoi(v) : 8;
        if (s_mb != 8) {
            fprintf(stderr, "[osGetMemSize] reporting %d MB (env override)\n", s_mb);
            fflush(stderr);
        }
    }
    ctx->r2 = (gpr)(s_mb * 1024 * 1024);
}

// zmemcpy is stubbed in rogue_squadron.toml because the original MIPS code
// at 0x80018F4C contains a `cache 0x0D` instruction unsupported by N64Recomp.
// The toml comment claims "the runtime provides memcpy" but no such
// implementation existed — leaving zmemcpy as an empty body. That made
// zlib's inflate_flush a no-op (it calls zmemcpy to copy from the sliding
// window to the user's next_out buffer), so every decompressed asset stayed
// all-zero. This is the actual implementation: byte-by-byte copy honoring
// the recompile's XOR-3 byte swizzle on each access.
//
// Signature: void zmemcpy(Bytef *dest, const Bytef *source, uInt len);
//   a0/r4 = dest (MIPS virtual address)
//   a1/r5 = source (MIPS virtual address)
//   a2/r6 = len (byte count)
extern "C" void zmemcpy(uint8_t* rdram, recomp_context* ctx) {
    // Use the full 64-bit register values for the bounds check. The MEM_BU
    // macro does (reg + offset) ^ 3 - 0xFFFFFFFF80000000 in 64-bit, so
    // truncating to uint32_t can mask sign-extension issues.
    uint64_t dest_full = (uint64_t)ctx->r4;
    uint64_t src_full  = (uint64_t)ctx->r5;
    uint64_t len_full  = (uint64_t)ctx->r6;
    constexpr uint64_t KSEG0_BASE = 0xFFFFFFFF80000000ull;
    constexpr uint64_t KSEG0_END  = 0xFFFFFFFF80800000ull;
    constexpr uint32_t KSEG0_BASE_LO = 0x80000000u;
    constexpr uint32_t KSEG0_END_LO  = 0x80800000u;
    auto in_range = [](uint64_t addr_full, uint64_t n) {
        // Accept either sign-extended (0xFFFFFFFF80...) or zero-extended (0x80...)
        // as long as it lands within the 8MB rdram region.
        if (addr_full >= KSEG0_BASE && addr_full + n <= KSEG0_END) return true;
        uint32_t lo = (uint32_t)addr_full;
        if (lo >= KSEG0_BASE_LO && (uint64_t)lo + n <= KSEG0_END_LO &&
            (addr_full >> 32) == 0) return true;
        return false;
    };
    if (len_full > 0 && (!in_range(dest_full, len_full) || !in_range(src_full, len_full))) {
        static int s_warned = 0;
        if (s_warned++ < 8) {
            fprintf(stderr, "[zmemcpy] OOB skip dst=0x%016llX src=0x%016llX len=%llu\n",
                    (unsigned long long)dest_full,
                    (unsigned long long)src_full,
                    (unsigned long long)len_full);
            fflush(stderr);
        }
        ctx->r2 = ctx->r4;  // zlib's zmemcpy returns nothing meaningful
        return;
    }
    // Use full gpr (uint64_t) values for MEM_BU/MEM_B. Truncating to uint32_t
    // breaks the macro's `- 0xFFFFFFFF80000000` arithmetic under C integer
    // promotion (uint32 + int64 = the uint32 gets zero-extended, so subtracting
    // a sign-extended int64 produces a 4GB-offset host pointer → AV).
    gpr dest_reg = ctx->r4;
    gpr src_reg  = ctx->r5;
    uint32_t len = (uint32_t)len_full;
    for (uint32_t i = 0; i < len; ++i) {
        ctx->r2 = MEM_BU(i, src_reg);
        MEM_B(i, dest_reg) = ctx->r2;
    }
    ctx->r2 = ctx->r4;  // zlib's zmemcpy returns nothing meaningful
}

// heapWalker cycle-detection state. Updated by hooks in funcs_3.c (entry +
// loop-top). When the walk revisits its starting pointer, the hook breaks
// out of the loop instead of spinning forever.
extern "C" unsigned g_heapwalker_initial = 0;
extern "C" unsigned g_heapwalker_iter = 0;
// Second-loop iteration cap (loop at vram 0x80007AD4). Observed to freeze
// when the free-list has a cycle of kseg0-valid nodes that never reach null.
// Reset at function entry; loop-top hook bails the function early at 4096.
extern "C" unsigned g_heapwalker_iter2 = 0;

// tickTextureMaterialExpiry cycle-detection state. Inner walk over the
// material list can loop forever when a node's next ptr points back into
// the visited set. KSEG0 guards prevent AVs but not cycles. Reset at
// function entry; capped iter count + first-node-revisit triggers bailout.
extern "C" unsigned g_tick_walker_first = 0;
extern "C" unsigned g_tick_walker_iter  = 0;

// func_80062108 (slot-label string-walker) per-call iteration counter.
// Bounds the per-character state-machine loop so uninitialized string buffers
// in menu-phase slot labels don't spin forever. Reset at function entry; cap
// at 4096 chars in the loop-top hook.
extern "C" unsigned g_f80062108_iter = 0;

// drawSubtitleText per-character loop iteration counter. Bounds the per-glyph
// emit loop (L_80015AF8) so menu callers passing subtitle structs with
// uninitialized charCount don't spin forever. Reset at function entry; cap at
// 256 chars in the loop-top hook (bails to L_80016BE8 for clean ENDDL).
extern "C" unsigned g_drawSubtitleText_iter = 0;

// Attribution screen (func_800C28F0, menuOverlayInit action 9) active flag.
// The attribution handler renders its legal/logo text to an offscreen 640x480
// buffer at 0x8076A000, then tells VI to display a different pair
// (0x805D4000 / 0x8066A000) into which real HW copies the rendered buffer.
// HLE never reproduces that copy, so the PresentEarly matcher (which requires
// rendered colorImg.address == VI fbAddress) finds no content for the VI's
// buffer and presents black. While this flag is set, osViSwapBuffer redirects
// the VI origin to the attribution render buffer so the matcher succeeds.
// Set/cleared by the menuOverlayInit hook on action 9 / non-9.
// g_attribution_active removed (2026-06-02) — g_current_scene (below) is the sole screen-state global;
// the attribution VI redirect above now keys on g_current_scene == 9.
// menuOverlayInit's action id (ctx->r5) — the game's own menu-screen id (9 = attribution; other
// values = other menu sub-screens). The generalization of g_attribution_active: rendering can
// dispatch on the screen id instead of a per-screen bool, and the set of known ids grows as we map
// screens (see F5Scene in rt64_gbi_f3dfactor5_internal.h). Complements g_active_overlay
// (1=menu/2=cinematic/0=gameplay, the coarse overlay). -1 = unset.
extern "C" volatile int g_current_scene = -1;
// Last framebuffer the game swapped to (osViSwapBuffer, post-redirect). The VI
// present can force VI_ORIGIN to this so it shows the game's intended front buffer
// instead of the cycling intermediate-render-pass buffers the cinematic writes to
// VI_ORIGIN (the post-logo flicker). Gated by ROGUESQ_FORCE_SWAP_FB in update_screen.
extern "C" volatile unsigned g_last_swap_fb = 0;
extern "C" volatile unsigned g_op_bf_count = 0;  // explosion-bloom tri counter (set by f5_emit_tri_native); fb-dump trigger

// Base RAM address (KSEG0) of the loaded mempak (Expansion-Pak cartridge) HOB
// mesh, captured by load_hmt_and_hob when it loads 'frontend/mempak/mempak'.
// 0 until loaded. The F3DFACTOR5 op_02 HLE reads the cartridge mesh from here
// to render it on the attribution screen (the F5 vertex pipeline is no-op'd in
// HLE, so we transform+emit the loaded mesh ourselves).
extern "C" volatile unsigned g_mempak_hob_base = 0;

// Base RAM address (KSEG0) of the loaded mempak HMT buffer (textures + palettes).
// load_hmt_and_hob normally frees this temp buffer after registering textures;
// for mempak we SKIP that free and record the base here so the F3DFACTOR5 HLE can
// read the real CI4 cartridge textures (mempackfront @+0x120, topsmall @+0x880)
// and their palettes (@+0xF0 / +0x850) directly. One small one-time leak.
extern "C" volatile unsigned g_mempak_hmt_base = 0;

// Per-model HMT registry (foundational for GENERAL model texturing — see
// reference_hmt_format_general_texturing). load_hmt_and_hob records each loaded model's
// HOB address range + its HMT base here, so the F3DFACTOR5 general renderer can find a
// model's material table from its meshdef (md1 ∈ [hob_lo, hob_hi) → that load's HMT).
// Parallel arrays for clean C interop with the recompiled funcs. Additive/diagnostic
// only — nothing renders from this yet. g_lhh_hmt = the in-progress load's HMT base
// (set mid-load where ctx->r19 is valid, consumed at the HOB-capture site).
extern "C" volatile unsigned g_lhh_hmt = 0;
extern "C" unsigned g_model_hob_lo[64]   = {0};
extern "C" unsigned g_model_hob_hi[64]   = {0};
extern "C" unsigned g_model_hmt_base[64] = {0};
extern "C" volatile int g_model_hmt_count = 0;

// Load-time HMT staging: the HMT buffer is freed (and FB-clobbered) before render, so
// we copy each load's whole HMT into a host buffer at load (offsets are still file-relative
// there). g_lhh_hmt_size = the load's HMT byte size. The general F5 renderer parses the
// host copy (g_model_hmt_copy[idx]) instead of the freed guest HMT.
extern "C" volatile unsigned g_lhh_hmt_size = 0;
extern "C" unsigned char* g_lhh_hmt_copy = nullptr;       // pending (current load) copy
extern "C" volatile unsigned g_lhh_hmt_copy_size = 0;
extern "C" unsigned char* g_model_hmt_copy[64] = {nullptr};   // committed per-load copies
extern "C" volatile unsigned g_model_hmt_copy_size[64] = {0};

// Ensure the pending HMT-copy buffer holds at least `size` bytes; return it (funcs_12 fills
// it via MEM_BU). Returns nullptr for an insane size so the caller skips staging.
extern "C" unsigned char* rs64_hmt_pending_buffer(unsigned size) {
    if (size == 0 || size > 0x200000u) return nullptr;     // >2MB = not a real HMT, skip
    if (g_lhh_hmt_copy == nullptr || g_lhh_hmt_copy_size < size) {
        unsigned char* nb = (unsigned char*)realloc(g_lhh_hmt_copy, size);
        if (!nb) return nullptr;
        g_lhh_hmt_copy = nb;
    }
    g_lhh_hmt_copy_size = size;
    return g_lhh_hmt_copy;
}

// Transfer the pending copy into the per-load registry slot (ownership moves; next load
// allocates a fresh pending buffer). Old slot content is freed if overwritten.
extern "C" void rs64_hmt_commit(int idx) {
    if (idx < 0 || idx >= 64 || g_lhh_hmt_copy == nullptr) return;
    if (g_model_hmt_copy[idx]) free(g_model_hmt_copy[idx]);
    g_model_hmt_copy[idx]      = g_lhh_hmt_copy;
    g_model_hmt_copy_size[idx] = g_lhh_hmt_copy_size;
    g_lhh_hmt_copy      = nullptr;
    g_lhh_hmt_copy_size = 0;
}

// The mempak HMT buffer lands ON a framebuffer (0x5D4xxx), so the attribution
// rendering overwrites the CI4 texture data with framebuffer pixels before the
// cartridge draws. So we COPY the real CI4 pixels + 16-color palettes into these
// host buffers during load (before the clobber). The F3DFACTOR5 HLE re-stages
// them into safe guest RAM each frame for loadBlock. mempackfront=64x57 CI4
// (1824B pix + 32B pal), topsmall=48x38 CI4 (912B pix + 32B pal). N64 byte order.
// g_cart_front/top_pix/pal removed (2026-06-02) — the cartridge now textures from the general
// per-model HMT registry (g_model_hmt_copy) via f5_build_hmt_mats, not a hardcoded per-face copy.
extern "C" volatile int  g_cart_tex_ready = 0;  // still the "cartridge loaded" gate for render_cartridge_general

// Set by processSceneNode (funcs_4.c) to the meshdef1 ptr of the model it is about
// to draw (item+0x8), for cinematic multi-model rendering. The F3DFACTOR5 op_02
// handler reads this to render the correct model. Tests whether the game-thread
// scene-graph walk and the op_02 interpret are timely-sequential (vs decoupled).
extern "C" volatile unsigned g_current_meshdef = 0;
extern "C" volatile unsigned g_scene_node_count = 0;  // bumped per mesh node, frame-ish
// Accumulated scene objects for the frame: processSceneNode appends (meshdef ptr +
// its 16-float MVP); the F3DFACTOR5 fullSync handler renders them all LAST (after the
// game's DL content) so they aren't overwritten, then resets the count.
#define G_MAX_SCENE_OBJS 256
extern "C" unsigned g_scene_md[G_MAX_SCENE_OBJS] = {0};
extern "C" float    g_scene_mtx[G_MAX_SCENE_OBJS][16] = {{0}};
extern "C" volatile int g_scene_obj_count = 0;
// The model's projection-baked MVP (a1 in processSceneNode): row-major 3x4, rows
// 0/1/2 = clip x/y/w. Captured per mesh node so the op_02 handler renders with the
// exact matrix paired to g_current_meshdef. (Float content, not a guest ptr.)
extern "C" float g_current_matrix[16] = {0};

// findOrCreateMaterial (texture-material list walker) cycle-detection state. Its
// linked-list walk (next ptr at +0x0) spins forever when the list is cyclic;
// the KSEG0 guard catches bad pointers but not a cycle of valid ones. Reset
// at function entry; first-node-revisit or iter cap triggers bailout.
extern "C" unsigned g_matwalk_first = 0;
extern "C" unsigned g_matwalk_iter  = 0;

// func_800079A4 (free-list insert/coalesce) cycle-detection state. Its
// L_800079B8 loop walks a next-pointer chain (field +0x0) with no KSEG0
// check and no cycle guard — the unguarded walk that AVs in the
// tickTextureMaterialExpiry -> func_800079A4 path. Reset at function entry.
extern "C" unsigned g_f79a4_first = 0;
extern "C" unsigned g_f79a4_iter  = 0;

// Watchdog for the cinematic inner loop. cinematicLoopBody iterates many
// times per cutscene playback; if it stops iterating for more than 2 seconds
// while the rest of the runtime keeps ticking, the game thread is hung in
// some callee. We capture the game thread's stack to find which one.
#ifdef _WIN32
static std::atomic<DWORD> g_cine_tid{0};
static std::atomic<uint64_t> g_cine_iter{0};
static std::atomic<uint64_t> g_cine_last_ms{0};

extern "C" void rs64_cine_dump_if_stuck(void);
extern "C" void rs64_cine_progress_log(void);
extern "C" void rs64_cine_start_watchdog_thread(void);

extern "C" unsigned long long rs64_cine_iter_get(void) { return g_cine_iter.load(std::memory_order_relaxed); }
extern "C" void rs64_cine_iter_tick(unsigned iter) {
    g_cine_tid.store(GetCurrentThreadId(), std::memory_order_relaxed);
    g_cine_iter.store(iter, std::memory_order_relaxed);
    g_cine_last_ms.store(GetTickCount64(), std::memory_order_relaxed);
    // First tick spawns the dedicated watchdog thread.
    rs64_cine_start_watchdog_thread();
}

// Spawn a dedicated watchdog thread the first time iter_tick fires. The
// gfx_thread's update_screen path could itself be deadlocked behind the
// same mutex chain that's hanging the game thread (events_context.message_mutex
// in ultramodern, in particular), so polling from gfx_thread isn't reliable.
// A standalone thread that only uses Sleep + atomics is immune.
#ifdef _WIN32
static void rs64_dump_all_thread_stacks(DWORD game_tid);
#endif
extern "C" void rs64_cine_start_watchdog_thread(void) {
    static std::atomic<bool> s_started{false};
    bool was = s_started.exchange(true, std::memory_order_relaxed);
    if (was) return;
    std::thread([]() {
        // ROGUESQ_DUMP_RDRAM_ON_CINE_STALL=<ms>: when the cinematic FRAME counter (game 0x8013889C) stops
        // advancing for that long, write RDRAM (big-endian, PJ64 layout) to ROGUESQ_DUMP_RDRAM_PATH or
        // dumps/rdram_cine_stall.bin. Lives here because update_screen (the other dump hook) is only
        // reached while the game still presents.
        const char* stall_env = std::getenv("ROGUESQ_DUMP_RDRAM_ON_CINE_STALL");
        const uint64_t stall_ms = (stall_env && *stall_env) ? (uint64_t)std::atoll(stall_env) : 0;
        uint32_t last_frame = 0; uint64_t last_change = 0; bool dumped = false;
        extern volatile uint8_t* volatile g_recomp_rdram_for_wp_raw;
        for (;;) {
            ::Sleep(500);
            rs64_cine_progress_log();
            rs64_cine_dump_if_stuck();
            if (stall_ms && !dumped) {
                const uint8_t* rd = (const uint8_t*)g_recomp_rdram_for_wp_raw;
                if (rd) {
                    uint32_t frame = *reinterpret_cast<const uint32_t*>(rd + 0x13889C);
                    uint64_t now = GetTickCount64();
                    if (frame != last_frame) { last_frame = frame; last_change = now; }
                    else if (frame > 0 && last_change && now - last_change >= stall_ms) {
                        dumped = true;
                        const char* pe = std::getenv("ROGUESQ_DUMP_RDRAM_PATH");
                        const char* path = (pe && *pe) ? pe : "dumps/rdram_cine_stall.bin";
                        FILE* f = fopen(path, "wb");
                        if (f) {
                            static uint8_t buf[0x10000];
                            for (uint32_t base = 0; base < 0x800000u; base += sizeof(buf)) {
                                for (uint32_t i = 0; i < sizeof(buf); ++i) buf[i] = rd[(base + i) ^ 3];
                                fwrite(buf, 1, sizeof(buf), f);
                            }
                            fclose(f);
                        }
                        fprintf(stderr, "[rdram-dump] cinematic frame counter stuck at %u for %llu ms -> %s (%s)\n",
                                frame, (unsigned long long)(now - last_change), path, f ? "ok" : "OPEN FAILED");
                        fflush(stderr);
                    }
                }
            }
#ifdef _WIN32
            // ROGUESQ_DUMP_STACKS_ON_DEMO_STALL=<ms>: the demo-input cursor demoStep (0x80109AE0) is
            // advanced by the GAME THREAD every demo frame; it stalls exactly when the game thread
            // deadlocks (e.g. the jade-moon structure-explosion freeze). When it's unchanged that long,
            // suspend + StackWalk64 + symbolize EVERY thread (game thread first) so we can read the
            // game thread's actual blocked osRecvMesg/queue. See plans/jade-moon-demo-freeze-plan.md.
            {
                static const char* dse = std::getenv("ROGUESQ_DUMP_STACKS_ON_DEMO_STALL");
                static const uint64_t dsms = (dse && *dse) ? (uint64_t)std::atoll(dse) : 0;
                static uint32_t last_demo = 0xFFFFFFFFu; static uint64_t last_demo_change = 0; static bool demo_dumped = false;
                if (dsms && !demo_dumped) {
                    const uint8_t* rd = (const uint8_t*)g_recomp_rdram_for_wp_raw;
                    if (rd) {
                        uint32_t ds = *reinterpret_cast<const uint32_t*>(rd + 0x109AE0);
                        uint64_t now = GetTickCount64();
                        if (ds != last_demo) { last_demo = ds; last_demo_change = now; }
                        else if (last_demo != 0 && last_demo != 0xFFFFFFFFu && last_demo_change && now - last_demo_change >= dsms) {
                            demo_dumped = true;
                            fprintf(stderr, "[demo-stall] demoStep stuck at %u for %llu ms — dumping all thread stacks\n",
                                    ds, (unsigned long long)(now - last_demo_change));
                            fflush(stderr);
                            rs64_dump_all_thread_stacks(g_cine_tid.load(std::memory_order_relaxed));
                        }
                    }
                }
            }
#endif
        }
    }).detach();
}

// One-shot RDRAM dump (big-endian / PJ64 layout) triggered from anywhere with an
// rdram pointer. Diagnostic: walk with tools/validate/f5_dl_walk.py. Fires once.
extern "C" void rs64_dump_rdram_once(uint8_t* rd, const char* path) {
    static int done = 0; if (done || !rd) return; done = 1;
    FILE* f = fopen(path, "wb");
    if (f) {
        static uint8_t buf[0x10000];
        for (uint32_t base = 0; base < 0x800000u; base += (uint32_t)sizeof(buf)) {
            for (uint32_t i = 0; i < sizeof(buf); ++i) buf[i] = rd[(base + i) ^ 3];
            fwrite(buf, 1, sizeof(buf), f);
        }
        fclose(f);
    }
    fprintf(stderr, "[rdram-dump-once] -> %s (%s)\n", path, f ? "ok" : "OPEN FAILED");
    fflush(stderr);
}

// Periodic progress log so we can see whether the inner loop is iterating
// at all and at what rate. Called from update_screen on a coarse schedule.
extern "C" void rs64_dump_rt64_framebuffers(const char*);

extern "C" void rs64_cine_progress_log(void) {
    static uint64_t s_last_log_ms = 0;
    static uint64_t s_last_log_iter = 0;
    uint64_t now = GetTickCount64();
    if (now - s_last_log_ms < 2000) return;  // 2s cadence
    uint64_t cur = g_cine_iter.load(std::memory_order_relaxed);
    uint64_t delta = cur - s_last_log_iter;
    uint64_t age_ms = (g_cine_last_ms.load() ? now - g_cine_last_ms.load() : 0);
    fprintf(stderr, "[cine-progress] iter=%llu delta=%llu in %llums (idle=%llums)\n",
            (unsigned long long)cur, (unsigned long long)delta,
            (unsigned long long)(now - s_last_log_ms),
            (unsigned long long)age_ms);
    fflush(stderr);
    s_last_log_ms = now;
    s_last_log_iter = cur;
    // Periodic FB-registry snapshot so we see whether RT64 is tracking a
    // framebuffer that overlaps the matpool — the corruption-via-stale-FB
    // hypothesis. Called from RT64's update_screen thread (same thread that
    // mutates the map), so no torn-read race. Internal cap is 16 calls.
    rs64_dump_rt64_framebuffers("cine-progress");
}

extern void print_stack_with_symbols(void** frames, USHORT count);

// Suspend one thread, walk its stack and print it (stall diagnosis).
static void rs64_dump_thread_stack(DWORD tid, const char* label) {
    HANDLE hThread = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_LIMITED_INFORMATION, FALSE, tid);
    if (!hThread) { fprintf(stderr, "[stack] %s tid=%lu OpenThread failed err=%lu\n", label, tid, GetLastError()); return; }
    wchar_t* desc = nullptr; char name[128] = "";
    if (SUCCEEDED(GetThreadDescription(hThread, &desc)) && desc) { snprintf(name, sizeof name, "%ls", desc); LocalFree(desc); }
    if (SuspendThread(hThread) == (DWORD)-1) { fprintf(stderr, "[stack] %s tid=%lu SuspendThread failed\n", label, tid); CloseHandle(hThread); return; }
    CONTEXT ctx{}; ctx.ContextFlags = CONTEXT_ALL;
    if (!GetThreadContext(hThread, &ctx)) { fprintf(stderr, "[stack] %s tid=%lu GetThreadContext failed\n", label, tid); ResumeThread(hThread); CloseHandle(hThread); return; }
    STACKFRAME64 frame{};
    frame.AddrPC.Offset = ctx.Rip; frame.AddrPC.Mode = AddrModeFlat;
    frame.AddrFrame.Offset = ctx.Rbp; frame.AddrFrame.Mode = AddrModeFlat;
    frame.AddrStack.Offset = ctx.Rsp; frame.AddrStack.Mode = AddrModeFlat;
    void* frames[40]; USHORT count = 0; HANDLE hProcess = GetCurrentProcess();
    while (count < 40) {
        if (!StackWalk64(IMAGE_FILE_MACHINE_AMD64, hProcess, hThread, &frame, &ctx, NULL, SymFunctionTableAccess64, SymGetModuleBase64, NULL)) break;
        if (frame.AddrPC.Offset == 0) break;
        frames[count++] = (void*)(uintptr_t)frame.AddrPC.Offset;
    }
    ResumeThread(hThread); CloseHandle(hThread);
    fprintf(stderr, "[stack] %s tid=%lu \"%s\" (%u frames)\n", label, tid, name, count);
    print_stack_with_symbols(frames, count); fflush(stderr);
}

// Walk every thread of the process (except the caller), the game thread first.
static void rs64_dump_all_thread_stacks(DWORD game_tid) {
    SymInitialize(GetCurrentProcess(), NULL, TRUE);
    if (game_tid) rs64_dump_thread_stack(game_tid, "game");
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) return;
    THREADENTRY32 te{}; te.dwSize = sizeof te; DWORD pid = GetCurrentProcessId(), self = GetCurrentThreadId();
    for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
        if (te.th32OwnerProcessID != pid || te.th32ThreadID == self || te.th32ThreadID == game_tid) continue;
        rs64_dump_thread_stack(te.th32ThreadID, "thread");
    }
    CloseHandle(snap);
}

extern "C" void rs64_cine_dump_if_stuck(void) {
    static std::atomic<bool> s_dumped{false};
    if (s_dumped.load(std::memory_order_relaxed)) return;

    uint64_t last = g_cine_last_ms.load(std::memory_order_relaxed);
    if (last == 0) return;  // never started iterating

    uint64_t now = GetTickCount64();
    if (now - last < 2000) return;  // not stuck yet

    DWORD tid = g_cine_tid.load(std::memory_order_relaxed);
    if (tid == 0) return;

    bool expected = false;
    if (!s_dumped.compare_exchange_strong(expected, true)) return;  // single shot

    fprintf(stderr, "[cine-watchdog] freeze detected: tid=%lu iter=%llu idle=%llums — opening thread\n",
            tid, (unsigned long long)g_cine_iter.load(), (unsigned long long)(now - last));
    fflush(stderr);

    rs64_dump_all_thread_stacks(tid);
}
#else
extern "C" void rs64_cine_iter_tick(unsigned) {}
extern "C" unsigned long long rs64_cine_iter_get(void) { return 0; }
extern "C" void rs64_cine_dump_if_stuck(void) {}
#endif

// Yield the cinematic CPU thread every N iterations to break the iter-810
// OS-level thread starvation stall. Empirically a kernel I/O call (stderr
// flush) unsticks it; Win32 Sleep alone does not. SwitchToThread yields to
// any ready thread on the same CPU.
extern "C" void rs64_cine_yield(void) {
    static int s_count = 0;
    ++s_count;
    // ~Every 16 iters: yield + flush stderr. Cheap enough to do hot.
    if ((s_count & 0xF) == 0) {
#ifdef _WIN32
        ::SwitchToThread();
#else
        std::this_thread::yield();
#endif
        std::fflush(stderr);
    }
}

// Pace the 32-iteration attribution-display loop inside
// runIdleFramesAndLoadSaveData (each iter renders one attribution frame).
// On real N64, each iter waits for the previous frame to complete via
// waitForPrevFrameDone → waitForPostSwapAck → which we patched to NOBLOCK
// (boot deadlock workaround). Without the wait, all 32 frames flash by in
// <1ms and the attribution screen is never visible.
// Sleep here so attribution displays for ~5 seconds (configurable).
//
// ROGUESQ_ATTRIBUTION_SLEEP_MS=N (default 150ms × 32 iters ≈ 4.8 sec).
// Set to 0 to disable.
extern "C" void rs64_idle_pace(void) {
#ifdef _WIN32
    static int s_sleep_ms = -1;
    if (s_sleep_ms < 0) {
        const char* e = std::getenv("ROGUESQ_ATTRIBUTION_SLEEP_MS");
        s_sleep_ms = (e && *e) ? std::atoi(e) : 0;   // default 0 since 2026-09-08: the attribution is VI-paced once waitForPostSwapAck really waits
        if (s_sleep_ms < 0) s_sleep_ms = 0;
        if (s_sleep_ms > 5000) s_sleep_ms = 5000;
        fprintf(stderr, "[idle-pace] attribution sleep_ms=%d (32 iters ≈ %ds total)\n",
                s_sleep_ms, (s_sleep_ms * 32) / 1000);
        fflush(stderr);
    }
    if (s_sleep_ms == 0) return;
    ::Sleep((DWORD)s_sleep_ms);
#endif
}

// Pace cinematicLoopBody to a target frame rate. Without this, the loop
// runs at >1000 Hz on the host (no pacing from NOBLOCK-patched osRecvMesg
// waits), racing through the cinematic in <2 seconds and skipping past
// the attribution screen, fade, and N64-logo segments before they're
// visible. Default 30 Hz to match N64 native cinematic rate.
// Configure via ROGUESQ_CINE_TARGET_FPS env var: 0 = disabled, positive
// int = target FPS.
extern "C" void rs64_cine_pace(void) {
#ifdef _WIN32
    // ROGUESQ_CINE_HOLD_ITER=N (+ optional ROGUESQ_CINE_HOLD_MS, default 15000): when the cinematic
    // loop reaches iteration N, park the game thread once for HOLD_MS so the exact frame N stays on
    // screen long enough to screenshot, matched to the RDRAM dump captured at the same iter. Bounded
    // so it self-releases (watchdogs don't fire).
    {
        static long long s_hold_iter = -2; static uint32_t s_hold_ms = 15000; static bool s_held = false;
        if (s_hold_iter == -2) {
            const char* e = std::getenv("ROGUESQ_CINE_HOLD_ITER");
            s_hold_iter = (e && *e) ? std::atoll(e) : -1;
            const char* m = std::getenv("ROGUESQ_CINE_HOLD_MS");
            if (m && *m) s_hold_ms = (uint32_t)std::atoll(m);
        }
        if (s_hold_iter >= 0 && !s_held && (long long)rs64_cine_iter_get() >= s_hold_iter) {
            s_held = true;
            fprintf(stderr, "[cine-hold] freezing at iter %llu for %u ms\n", rs64_cine_iter_get(), s_hold_ms);
            fflush(stderr);
            uint64_t end = GetTickCount64() + s_hold_ms;
            while (GetTickCount64() < end) ::Sleep(100);
            fprintf(stderr, "[cine-hold] released\n"); fflush(stderr);
        }
    }
    static int s_target_fps = -1;
    if (s_target_fps < 0) {
        const char* e = std::getenv("ROGUESQ_CINE_TARGET_FPS");
        s_target_fps = (e && *e) ? std::atoi(e) : (rs64_vi_driven() ? 0 : 30);  // no host pacing when the loop is VI-driven
        if (s_target_fps < 0) s_target_fps = 0;
        if (s_target_fps > 240) s_target_fps = 240;
        fprintf(stderr, "[cine-pace] init target_fps=%d (env='%s')\n",
                s_target_fps, e ? e : "(null)");
        fflush(stderr);
        // Bump Windows timer resolution to 1ms so Sleep(33) actually sleeps
        // ~33ms instead of the default ~15.6ms quantum.
        timeBeginPeriod(1);
    }
    if (s_target_fps == 0) return;  // disabled
    uint32_t target_interval_ms = 1000u / (uint32_t)s_target_fps;
    // ROGUESQ_EXPLOSION_HOLD=1: when the GBI module flags the explosion active
    // (g_explosion_hold > 0, set when fire CI4 tiles load), CRAWL at ~3 fps so the
    // brief explosion lingers on screen and is reliably visible. Counts down.
    {
        static int s_hold_en = -1;
        if (s_hold_en < 0) { const char* e = std::getenv("ROGUESQ_EXPLOSION_HOLD"); s_hold_en = (e && *e && *e != '0') ? 1 : 0; }
        if (s_hold_en && g_explosion_hold > 0) {
            target_interval_ms = 330u;  // ~3 fps crawl during the explosion
            g_explosion_hold--;
        }
    }
    static uint64_t s_last_ms = 0;
    static uint32_t s_calls = 0;
    static uint32_t s_sleeps = 0;
    uint64_t now = GetTickCount64();
    if (s_last_ms == 0) {
        s_last_ms = now;
        return;
    }
    uint64_t elapsed = now - s_last_ms;
    if (elapsed < (uint64_t)target_interval_ms) {
        ::Sleep((DWORD)(target_interval_ms - elapsed));
        ++s_sleeps;
    }
    s_last_ms = GetTickCount64();
    ++s_calls;
    if (s_calls <= 5 || (s_calls & 0x3F) == 0) {
        fprintf(stderr, "[cine-pace] call=%u sleeps=%u elapsed=%llums interval=%ums\n",
                s_calls, s_sleeps, (unsigned long long)elapsed, target_interval_ms);
        fflush(stderr);
    }
#endif
}

// Diagnostic sleep helper for [[patches.hook]] entries (e.g. the attribution
// HOLD hook that stretches the screen so it can be screenshotted past the
// ~5s capture lag). Recompiled hook C can't include <windows.h>.
extern "C" void rs64_sleep_ms(unsigned ms) {
#ifdef _WIN32
    ::Sleep((DWORD)ms);
#endif
}

// Yield the CPU to other game threads (e.g. the DMA manager loading font
// tiles) without a fixed delay. Used to test whether the attribution screen's
// missing glyphs are a thread-starvation issue rather than a timing one.
extern "C" void rs64_yield(void) {
#ifdef _WIN32
    ::SwitchToThread();
#else
    std::this_thread::yield();
#endif
}

// Incremented once per host VI retrace by rs64_vi_callback (main.cpp), 60Hz.
extern "C" volatile unsigned g_vi_tick = 0;

// Pace the caller to the real VI retrace: block until the next host VI tick
// (or a safety cap). This is the proper fix for the attribution white/partial
// race — the attribution loop otherwise free-runs at ~135fps and starves the
// VI-driven producer thread that lays out the full glyph set, so only a partial
// set is emitted. Waiting one VI period (~16ms) of real time lets that thread
// run AND paces the screen to its correct ~60fps/2s, instead of the 50ms hold
// band-aid. Sleeps in 1ms steps so the wait yields real time (not a busy-spin
// that would re-starve the producer). Cap ~40ms so a stalled VI can't hang it.
extern "C" void rs64_attrib_wait_vi(void) {
#ifdef _WIN32
    unsigned start = g_vi_tick;
    for (int i = 0; i < 40; ++i) {
        if (g_vi_tick != start) break;
        ::Sleep(1);
    }
#endif
}

// Diagnostic helper for [[patches.hook]] entries that need to log to
// stderr — the hook code lives in funcs_*.c which doesn't include stdio.
// Format args: 4 unsigned 32-bit values + a tag string.
extern "C" void rs64_dbg_log4(const char* tag, unsigned a, unsigned b, unsigned c, unsigned d) {
    static int s_log = -1;
    if (s_log < 0) {
        const char* e = std::getenv("ROGUESQ_LOG_HOOKS");
        s_log = (e && *e && *e != '0') ? 1 : 0;
    }
    if (!s_log) return;
#ifdef _WIN32
    static uint64_t s_t0 = GetTickCount64();
    unsigned ms = (unsigned)(GetTickCount64() - s_t0);
#else
    unsigned ms = 0;
#endif
    fprintf(stderr, "[hook t=%ums] %s a=0x%08X b=0x%08X c=0x%08X d=0x%08X\n",
            ms, tag ? tag : "?", a, b, c, d);
    fflush(stderr);
}

// Float-valued variant of rs64_dbg_log4 for clock/dt diagnostics.
extern "C" void rs64_dbg_logf(const char* tag, float a, float b, float c, float d) {
    static int s_log = -1;
    if (s_log < 0) {
        const char* e = std::getenv("ROGUESQ_LOG_HOOKS");
        s_log = (e && *e && *e != '0') ? 1 : 0;
    }
    if (!s_log) return;
    fprintf(stderr, "[hookf] %s a=%.6f b=%.6f c=%.6f d=%.6f\n",
            tag ? tag : "?", a, b, c, d);
    fflush(stderr);
}

// Material node pool extent, recorded when createMaterialPool creates it. Used to
// detect whether another rs_malloc allocation overlaps the pool (a heap
// double-allocation would explain unrelated writes corrupting the free-list).
extern "C" unsigned g_matpool_lo = 0;
extern "C" unsigned g_matpool_hi = 0;
static uint8_t* g_rdram_base = nullptr;

// --- Software page-guard watchpoint on the material node pool ---
// Hardware debug registers AND the x86 single-step trap flag are both
// virtualized away in this environment, so a step-over-legit-writes watchpoint
// is impossible. Different tack: VirtualProtect the 4 KB page holding the
// matpool free-list nodes to read-only across exactly the submitGfxFrame->
// frameStartReset window. The matfreelist instrumentation proved that window
// contains NO legitimate matpool writer (no instrumented matpool function's
// check fires between those two points), so the FIRST write to the guarded
// page in that window is the corruptor. The handler logs its PC + stack
// immediately — no single-step, no value classification — then unprotects and
// lets the write proceed. Gated by ROGUESQ_MATPOOL_WP=1.
static void* g_pg_page = nullptr;          // 4 KB host page covering the nodes
static volatile LONG g_pg_protected = 0;
static volatile LONG g_pg_window = 0;      // armed-window counter, for correlation
static volatile LONG g_pg_logs = 0;        // faults logged so far (capped at 64)
static int g_pg_on = -1;

static LONG CALLBACK rs64_matpool_pg_veh(EXCEPTION_POINTERS* ep) {
    if (ep->ExceptionRecord->ExceptionCode != EXCEPTION_ACCESS_VIOLATION ||
        ep->ExceptionRecord->NumberParameters < 2 || g_pg_page == nullptr) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    uintptr_t fault = (uintptr_t)ep->ExceptionRecord->ExceptionInformation[1];
    uintptr_t pg = (uintptr_t)g_pg_page;
    if (fault < pg || fault >= pg + 0x1000) {
        return EXCEPTION_CONTINUE_SEARCH;            // not our guarded page
    }
    // A write into the guarded matpool page during the no-legit-writer window.
    // Unprotect so the write can complete, then log who did it.
    DWORD old;
    VirtualProtect(g_pg_page, 0x1000, PAGE_READWRITE, &old);
    InterlockedExchange(&g_pg_protected, 0);
    if (InterlockedIncrement(&g_pg_logs) <= 64) {
        uintptr_t modbase = (uintptr_t)GetModuleHandleW(NULL);
        uintptr_t pcrva = (uintptr_t)ep->ExceptionRecord->ExceptionAddress - modbase;
        uint32_t n64 = (g_rdram_base != nullptr)
            ? 0x80000000u + (uint32_t)(fault - (uintptr_t)g_rdram_base) : 0u;
        void* frames[24];
        USHORT nf = RtlCaptureStackBackTrace(0, 24, frames, NULL);
        fprintf(stderr, "[matpg] *** CORRUPTOR window=%ld pc_rva=0x%llX "
                "node_addr=0x%08X rw=%llu tid=%u ***\n",
                (long)g_pg_window, (unsigned long long)pcrva, n64,
                (unsigned long long)ep->ExceptionRecord->ExceptionInformation[0],
                (unsigned)GetCurrentThreadId());
        fprintf(stderr, "[matpg]   stack RVAs:");
        for (USHORT i = 0; i < nf; i++) {
            fprintf(stderr, " 0x%llX",
                    (unsigned long long)((uintptr_t)frames[i] - modbase));
        }
        fprintf(stderr, "\n");
        fflush(stderr);
    }
    return EXCEPTION_CONTINUE_EXECUTION;
}

// Called from the submitGfxFrame-exit hook: arm the page guard for the window.
extern "C" void rs64_matpool_protect(void) {
    if (g_pg_on != 1 || g_pg_page == nullptr) return;
    InterlockedIncrement(&g_pg_window);
    DWORD old;
    if (VirtualProtect(g_pg_page, 0x1000, PAGE_READONLY, &old)) {
        InterlockedExchange(&g_pg_protected, 1);
    }
}

// Called from the frameStartReset-entry hook: disarm (window closed).
extern "C" void rs64_matpool_unprotect(void) {
    if (!g_pg_protected) return;
    DWORD old;
    VirtualProtect(g_pg_page, 0x1000, PAGE_READWRITE, &old);
    InterlockedExchange(&g_pg_protected, 0);
}

extern "C" void rs64_matpool_set(uint8_t* rdram, unsigned pool) {
    g_rdram_base = rdram;
    g_matpool_lo = pool;
    g_matpool_hi = pool + 0x3000u;
    if (g_pg_on < 0) {
        const char* e = std::getenv("ROGUESQ_MATPOOL_WP");
        g_pg_on = (e && *e && *e != '0') ? 1 : 0;
        if (g_pg_on) {
            // 4 KB host page covering matpool nodes 82-87 (0x801605A8..0x160620).
            uintptr_t node83 = (uintptr_t)rdram + (uintptr_t)(pool - 0x80000000u) + 83u * 0x18u;
            g_pg_page = (void*)(node83 & ~(uintptr_t)0xFFF);
            AddVectoredExceptionHandler(1, rs64_matpool_pg_veh);
            fprintf(stderr, "[matpg] page guard ready — page host 0x%p (matpool @0x%08X)\n",
                    g_pg_page, pool);
            fflush(stderr);
        }
    }
}

extern "C" void rs64_matpool_alloc_check(unsigned addr, unsigned size) {
    static int s_on = -1;
    if (s_on < 0) {
        const char* e = std::getenv("ROGUESQ_LOG_MATFREELIST");
        s_on = (e && *e && *e != '0') ? 1 : 0;
    }
    if (!s_on || g_matpool_lo == 0 || addr == g_matpool_lo) return;
    // Overlap of [addr, addr+size) with [g_matpool_lo, g_matpool_hi)?
    if (addr < g_matpool_hi && (addr + size) > g_matpool_lo) {
        static int s_n = 0;
        if (s_n++ < 8) {
            fprintf(stderr, "[matpool] rs_malloc 0x%08X size 0x%X OVERLAPS material pool "
                    "[0x%08X,0x%08X)!\n", addr, size, g_matpool_lo, g_matpool_hi);
            fflush(stderr);
        }
    }
}

// Texture-material free-list integrity probe. Gated by ROGUESQ_LOG_MATFREELIST.
// Walks the node free-list (pool ptr @0x80128EF4, head @0x80128EF8) following
// each node's +0x0 'next'. Every node must lie in [pool, pool+0x3000) and be
// 0x18-aligned from the pool base. Reports the FIRST corruption seen, plus
// which texture-material function's entry detected it — so the corrupting
// function can be bisected from the call sequence.
// Phase 2 (RT64 per-object interpolation): level-scoped stable id map. Assigns a stable, unique id per
// persistent gameplay entity (meshInstance pointer) — the identity RT64 interpolation matching needs. The
// entity pointer is stable within a level (proven via the [lodsel] trace) but the heap reuses addresses
// across levels, so the map is reset on level change (level id @ 0x80130B70). ROGUESQ_F5_ID_LOG logs
// first-sightings so we can confirm the id set stabilizes (NEW lines stop after the first frame).
// See plans/rt64-f5-integration-plan.md.
static std::map<uint32_t, uint32_t> s_f5_id_map;
static uint32_t s_f5_id_next = 1;
static int      s_f5_id_level = -1;
extern "C" uint32_t rs64_f5_entity_id(uint8_t* rdram, uint32_t entity) {
    static int s_log = -1;
    if (s_log < 0) { const char* e = std::getenv("ROGUESQ_F5_ID_LOG"); s_log = (e && *e && *e != '0') ? 1 : 0; }
    int level = (int)MEM_BU(0, (gpr)(int32_t)0x80130B70);
    if (level != s_f5_id_level) {
        s_f5_id_level = level; s_f5_id_map.clear(); s_f5_id_next = 1;
        if (s_log) { fprintf(stderr, "[f5id] level=%d -> id map reset\n", level); fflush(stderr); }
    }
    if (entity < 0x80000000u || entity >= 0x80800000u) return 0;
    auto it = s_f5_id_map.find(entity);
    if (it != s_f5_id_map.end()) return it->second;
    uint32_t id = s_f5_id_next++;
    s_f5_id_map.emplace(entity, id);
    if (s_log) { fprintf(stderr, "[f5id] NEW entity=%08X -> id=%u (level=%d, total=%zu)\n",
                         entity, id, level, s_f5_id_map.size()); fflush(stderr); }
    return id;
}

// Broad-coverage variant: hook processSceneNode (the scene-graph per-node visitor). The scene NODE is
// transient, but node->object (+0x8) is the persistent entity (deduped per frame via object+0x30). Reads
// the object and assigns it a stable id via the shared level-scoped map. Skips transform/group nodes
// (object == 0). Measures how much of the ~71-object set the scene graph actually covers vs the
// per-NPC-type handlers. See plans/rt64-f5-integration-plan.md.
extern "C" unsigned int rs64_f5_entity_id(uint8_t*, unsigned int);
extern "C" void rs64_f5_scenenode_id(uint8_t* rdram, unsigned int node) {
    if (node < 0x80000000u || node >= 0x80800000u) return;
    unsigned int obj = (unsigned int)MEM_W(0, (gpr)(int32_t)(node + 0x8));
    if (obj >= 0x80000000u && obj < 0x80800000u) rs64_f5_entity_id(rdram, obj);
}

extern "C" void rs64_matfreelist_check(uint8_t* rdram, const char* where) {
    static int s_on = -1;
    if (s_on < 0) {
        const char* e = std::getenv("ROGUESQ_LOG_MATFREELIST");
        s_on = (e && *e && *e != '0') ? 1 : 0;
    }
    if (!s_on) return;
    static int s_last_ok = 1;             // was the list intact at the last check?
    static char s_last_where[80] = "(boot)";
    static int s_done = 0;
    if (s_done) return;

    uint32_t pool = (uint32_t)MEM_W(0, (gpr)(int32_t)0x80128EF4);
    if (pool < 0x80000000u || pool >= 0x80800000u) return; // pool not created yet
    uint32_t pool_end = pool + 0x3000u;
    uint32_t head = (uint32_t)MEM_W(0, (gpr)(int32_t)0x80128EF8);

    uint32_t node = head, prev = 0;
    int steps = 0, corrupt = 0;
    uint32_t bad = 0;
    while (node != 0) {
        if (node < pool || node >= pool_end || ((node - pool) % 0x18u) != 0) {
            corrupt = 1; bad = node; break;
        }
        prev = node;
        node = (uint32_t)MEM_W(0, (gpr)(int32_t)node);
        if (++steps > 600) { corrupt = 1; bad = 0xCCCCCCCCu; break; }
    }

    if (corrupt && s_last_ok) {
        // OK -> CORRUPT transition: the corruptor ran between these two checks.
        // Also dump the draw framebuffer + width/height globals
        // (buildAndRegisterDefaultMaterial writes them) to test whether the
        // matpool overlaps the framebuffer the renderer writes.
        uint32_t draw_fb = (uint32_t)MEM_W(0, (gpr)(int32_t)0x8011A84C);
        uint32_t fb_w    = (uint32_t)MEM_W(0, (gpr)(int32_t)0x8011A838);
        uint32_t fb_h    = (uint32_t)MEM_HU(0, (gpr)(int32_t)0x8011A848);
        fprintf(stderr, "[matfreelist] *** corruption appeared (matpg window=%ld) between [%s] and [%s] *** "
                "bad next=0x%08X from prev=0x%08X (prev pool idx %d) step=%d head=0x%08X pool=[0x%08X,0x%08X)\n"
                "[matfreelist]   draw_fb=0x%08X fb_w=0x%X fb_h=0x%X  (fb spans 0x%08X..0x%08X if w*h*2)\n",
                (long)g_pg_window, s_last_where, where, bad, prev,
                (prev >= pool && prev < pool_end) ? (int)((prev - pool) / 0x18u) : -1,
                steps, head, pool, pool_end,
                draw_fb, fb_w, fb_h, draw_fb, draw_fb + fb_w * fb_h * 2);
        fflush(stderr);
        s_done = 1;
        return;
    }
    s_last_ok = corrupt ? 0 : 1;
    s_last_where[0] = '\0';
    for (int i = 0; i < 79 && where[i]; i++) { s_last_where[i] = where[i]; s_last_where[i + 1] = '\0'; }
}

// --- Matpool free-list self-repair (freeze mitigation) ---
// Root cause (caught 2026-05-17 via the window-armed page guard): RT64's
// framebuffer writeback, RT64::Framebuffer::copyNativeToRAM, memcpy's a GPU
// framebuffer back into RDRAM at ~0x80160000 — which overlaps the texture-
// material node pool at 0x8015FDE0. A SET_COLOR_IMAGE in the F3DFACTOR5 HLE
// stream targets that region, so RT64 registers a framebuffer there and blits
// pixels over the free-list / bucket lists every RDP fullSync. The allocator
// (allocOrEvictMaterialNode) then follows a wild pointer and the cinematic thread dies.
//
// This is a MITIGATION, not the root fix: called at the entry of every matpool
// list-walking function, it detects a corrupt free-list and rebuilds the pool
// (clean 512-node free-list + empty bucket table — exactly what buildMaterialFreeList
// buildFreeList does). Materials in flight are lost and re-created on demand,
// but the game self-recovers instead of freezing. The proper fix is to stop
// RT64 registering a framebuffer over the matpool address range.
extern "C" void rs64_dump_rt64_framebuffers(const char*);

extern "C" void rs64_matpool_repair(uint8_t* rdram) {
    if (!(rs64_fb_guards_mask() & 2)) return;
    uint32_t pool = (uint32_t)MEM_W(0, (gpr)(int32_t)0x80128EF4);
    if (pool < 0x80000000u || pool >= 0x80800000u) return;   // not created yet
    uint32_t pool_end = pool + 0x3000u;

    // Walk the free-list: is any link out of [pool, pool_end) or misaligned?
    int corrupt = 0;
    uint32_t node = (uint32_t)MEM_W(0, (gpr)(int32_t)0x80128EF8);
    for (int steps = 0; node != 0 && steps < 600; steps++) {
        if (node < pool || node >= pool_end || ((node - pool) % 0x18u) != 0) {
            corrupt = 1; break;
        }
        node = (uint32_t)MEM_W(0, (gpr)(int32_t)node);
    }
    if (!corrupt) return;

    static int s_n = 0;
    if (s_n++ < 8) {
        fprintf(stderr, "[matpool-repair] free-list corrupt — rebuilding pool 0x%08X\n", pool);
        fflush(stderr);
        // Snapshot RT64's tracked framebuffer registry — if any FB overlaps
        // [pool, pool+0x3000), the corruption is RT64 writing into RDRAM
        // via a stale or current framebuffer registration.
        rs64_dump_rt64_framebuffers("matpool-repair-corruption");
    }
    // Rebuild: re-chain all 512 nodes, then empty the per-texture bucket table.
    for (uint32_t i = 0; i < 512u; i++) {
        uint32_t n = pool + i * 0x18u;
        MEM_W(0, (gpr)(int32_t)n) = (i < 511u) ? (n + 0x18u) : 0u;   // next
        MEM_W(4, (gpr)(int32_t)n) = (i > 0u)   ? (n - 0x18u) : 0u;   // prev
    }
    MEM_W(0, (gpr)(int32_t)0x80128EF8) = pool;                       // free-list head
    uint32_t cnt   = (uint32_t)MEM_HU(0, (gpr)(int32_t)0x80128EF0);
    uint32_t table = (uint32_t)MEM_W(0, (gpr)(int32_t)0x80128F00);
    if (table >= 0x80000000u && table < 0x80800000u) {
        for (uint32_t j = 0; j < cnt && j < 4096u; j++) {
            MEM_W(0, (gpr)(int32_t)(table + j * 4u)) = 0u;
        }
    }
}

// --- Bogus low-memory SET_COLOR_IMAGE neutralizer (the real fix) ---
// Verified 2026-05-20 via the fb-registry instrumentation: at the moment
// matpool corruption fires, RT64 has registered ~6 garbage framebuffers
// from Factor 5 ucode-emitted SET_COLOR_IMAGEs whose addresses land in the
// heap region (low RAM). Two examples from one run:
//   img=0x801410A6 w=195  h=332 siz=2 -> spans 0x801410A6..0x80160A6E
//                                        (covers matpool tail)
//   img=0x80040000 w=3863 h=332 siz=1 -> spans 0x80040000..0x801791D4
//                                        (covers entire low RAM)
// They don't *start* inside the matpool — they start far below it and
// span over it. The previous "start address contained in matpool" check
// missed both. The cleaner rule, cross-checked against three Project64
// RDRAM dumps: every legitimate framebuffer/render target on this game
// lives at >= 0x804B7800. Anything below that is garbage and should be
// stripped before RT64 sees it.
//
// This walks the F3DFACTOR5 HLE display list the GFX task points at (the
// same traversal the opcode-histogram walker in rt64_render_context.cpp
// uses — linear, following G_DL jumps, terminating on G_ENDDL 0xB8) and
// rewrites the opcode byte of any low-address G_SETCIMG to 0x00 (G_SPNOOP)
// so RT64 never sees it. Threshold 0x80400000 is safely below the lowest
// real fb (~0x80628000 in our runs, 0x804B7800 across the PJ64 dumps).
// Called per GFX task, before processDisplayLists.
static constexpr uint32_t RS64_FB_MIN_ADDR = 0x80400000u;
// Upper bound: real-hardware RDRAM is 8 MB so 0x80800000 is the absolute
// ceiling. Any CIMG addr above this writes past RDRAM into adjacent host
// memory (the recompile heap shares the same host allocation). Verified
// 2026-05-20 from registry dumps: garbage FBs at 0x80C00000, 0x80C90000,
// 0x80E90000, 0x80FA0000 all from Factor 5 ucode emitting CIMGs with
// W1 upper bytes that decode (post 24-bit mask) to addresses past 8 MB.
static constexpr uint32_t RS64_FB_MAX_ADDR = 0x80800000u;

extern "C" void rs64_neutralize_matpool_cimg(uint8_t* rdram, uint32_t dl_phys) {
    if (!(rs64_fb_guards_mask() & 1)) return;
    if (dl_phys == 0) return;

    // One-shot read-only dump of a whole frame DL's opcode stream. Set
    // ROGUESQ_DUMP_FRAME_DL=N to dump the Nth call (pick a menu frame, e.g.
    // 200) so we can see the 3 background-image quads + their SETTIMG sources
    // and whether the menu emits them at all. Logs SETTIMG/SETTILE/LOADBLOCK/
    // TEXRECT addresses inline. Read-only — does not modify the DL.
    {
        static int s_dump_target = -2;
        if (s_dump_target == -2) {
            const char* e = std::getenv("ROGUESQ_DUMP_FRAME_DL");
            s_dump_target = (e && *e) ? atoi(e) : -1;
        }
        static int s_dump_call = 0;
        int call = ++s_dump_call;
        // Dump a RANGE [target, target+16) so we can compare consecutive frame
        // DLs' TEXRECT rows + target FBs and learn the accumulation model.
        const bool in_range = (s_dump_target >= 0 && call >= s_dump_target && call < s_dump_target + 16);
        const bool full_detail = (call == s_dump_target);  // per-command detail only on first
        if (in_range) {
            uint32_t a = (dl_phys & 0x3FFFFFFu) | 0x80000000u;
            uint32_t dstack[16]; int dsp = 0; int djmp = 256;
            int tr_n = 0; int tr_minx = 99999, tr_miny = 99999, tr_maxx = -99999, tr_maxy = -99999;
            uint32_t last_cimg = 0;
            // Visited-set: the menu DL re-enters the same tile sub-DLs (a loop),
            // so without this the walker re-counts one strip forever and never
            // reaches the other rows. Skip any command address seen before.
            static uint8_t s_vis[0x800000 >> 3];
            memset(s_vis, 0, sizeof(s_vis));
            fprintf(stderr, "[frame-dl-dump call=%d] start=0x%08X\n", call, a);
            for (int i = 0; i < 16384; ++i) {
                if ((a & 0x3FFFFFFu) + 8u > 0x800000u) break;
                uint32_t vidx = (a & 0x3FFFFFFu) >> 3;
                if (vidx < (0x800000 >> 3)) { if (s_vis[vidx]) break; s_vis[vidx] = 1; }
                uint32_t w0 = (uint32_t)MEM_W(0, (gpr)(int32_t)a);
                uint32_t w1 = (uint32_t)MEM_W(4, (gpr)(int32_t)a);
                uint8_t op = (uint8_t)(w0 >> 24);
                // Model-DL spec probe (docs/f5-model-dl-spec.md): count 0xBD/0xBE/0xB4/0x14
                // + dump the next 4 words so 0xB4's true length and the BD/BE payloads can
                // be judged offline. BD/BE matter because the GBI inherits F3DEX
                // popMatrix/cullDl for them; B4 is consumed (geometry dropped).
                {
                    static int s_pcnt[4] = {0,0,0,0}, s_pdmp[4] = {0,0,0,0};
                    int pidx = (op == 0xBD) ? 0 : (op == 0xBE) ? 1 : (op == 0xB4) ? 2 : (op == 0x14) ? 3 : -1;
                    if (pidx >= 0) {
                        ++s_pcnt[pidx];
                        if (s_pdmp[pidx] < 6) { ++s_pdmp[pidx];
                            uint32_t n0 = (uint32_t)MEM_W(0, (gpr)(int32_t)(a + 8u)),  n1 = (uint32_t)MEM_W(4, (gpr)(int32_t)(a + 8u));
                            uint32_t n2 = (uint32_t)MEM_W(0, (gpr)(int32_t)(a + 16u)), n3 = (uint32_t)MEM_W(4, (gpr)(int32_t)(a + 16u));
                            fprintf(stderr, "  [probe-%02X] %08X: %08X %08X | next: %08X %08X %08X %08X\n",
                                    op, a, w0, w1, n0, n1, n2, n3);
                        }
                    }
                    if (i == 16383 || op == 0xB8)
                        fprintf(stderr, "  [probe-sum] BD=%d BE=%d B4=%d 14=%d\n",
                                s_pcnt[0], s_pcnt[1], s_pcnt[2], s_pcnt[3]);
                }
                // 0xB4: match the GBI's op_consume16 (16 bytes) so the walk reflects what
                // RT64 currently sees; the probe context above reveals the REAL length.
                if (op == 0xB4) { a += 16u; continue; }
                // 0xBE: 16-byte command (payload FFF7CF0F 00000000 — misparsed as a garbage
                // SETCIMG when walked at 8B; see docs/f5-model-dl-spec.md).
                if (op == 0xBE) { a += 16u; continue; }
                // 0xB5 chunk-link: follow w1 like op_b5_chunk_link (w1==0 → plain no-op).
                if (op == 0xB5 && (w1 & 0x00FFFFFFu) != 0u) {
                    a = (w1 & 0x00FFFFF8u) | 0x80000000u; continue;
                }
                // TEXRECT/TEXRECTFLIP (E4/E5) = 3 words (24B): cmd + texcoord +
                // step. Decode coords (10.2 fixed → px) and advance the walker
                // past all 3 words so it stays aligned (matches RT64's handler).
                if (op == 0xE4 || op == 0xE5) {
                    int lrx = ((w0 >> 12) & 0xFFF), lry = (w0 & 0xFFF);
                    int ulx = ((w1 >> 12) & 0xFFF), uly = (w1 & 0xFFF);
                    int tile = (w1 >> 24) & 0x7;
                    uint32_t t1w1 = (uint32_t)MEM_W(4, (gpr)(int32_t)(a + 8u));
                    uint32_t t2w1 = (uint32_t)MEM_W(4, (gpr)(int32_t)(a + 16u));
                    if (full_detail)
                        fprintf(stderr, "  %08X: TEXRECT tile=%d ul=(%d,%d) lr=(%d,%d) px=(%.1f,%.1f)-(%.1f,%.1f) "
                            "uls/t=0x%08X dsdx/y=0x%08X\n",
                            a, tile, ulx, uly, lrx, lry,
                            ulx/4.0, uly/4.0, lrx/4.0, lry/4.0, t1w1, t2w1);
                    ++tr_n;
                    if (ulx/4 < tr_minx) tr_minx = ulx/4; if (uly/4 < tr_miny) tr_miny = uly/4;
                    if (lrx/4 > tr_maxx) tr_maxx = lrx/4; if (lry/4 > tr_maxy) tr_maxy = lry/4;
                    a += 24u; continue;
                }
                // op_03 = 8B cmd + 16B inline vertex (24B total). Skip all 3 words.
                if (op == 0x03) { a += 24u; continue; }
                // SETTIMG: optionally dump the texture bytes for offline inspection.
                // ROGUESQ_DUMP_TEXTURES=1 → write dumps/tex/<addr>_fmt<f>_siz<s>.bin
                // (8 KB from the image address) + log fmt/siz. fmt: 0=RGBA 1=YUV
                // 2=CI 3=IA 4=I. siz: 0=4b 1=8b 2=16b 3=32b.
                if (op == 0xFD) {
                    static int s_dt = -1;
                    if (s_dt < 0) { const char* e = std::getenv("ROGUESQ_DUMP_TEXTURES"); s_dt = (e && e[0] && e[0]!='0') ? 1 : 0; }
                    uint32_t fmt = (w0 >> 21) & 0x7, siz = (w0 >> 19) & 0x3;
                    if (s_dt && full_detail) {
                        uint32_t timg = w1 & 0x00FFFFFFu;
                        char path[256];
                        snprintf(path, sizeof(path), "dumps/tex/%06X_fmt%u_siz%u.bin", timg, fmt, siz);
                        static int s_made = 0;
                        if (!s_made) { s_made = 1; system("if not exist dumps\\tex mkdir dumps\\tex"); }
                        FILE* tf = fopen(path, "wb");
                        if (tf) {
                            for (uint32_t k = 0; k < 0x2000u; ++k) {
                                uint32_t off = (timg + k);
                                uint8_t b = (off < 0x800000u) ? rdram[off ^ 3] : 0;
                                fputc(b, tf);
                            }
                            fclose(tf);
                        }
                        fprintf(stderr, "  %08X: SETTIMG addr=0x%06X fmt=%u siz=%u -> dumps/tex/%06X_fmt%u_siz%u.bin\n",
                                a, timg, fmt, siz, timg, fmt, siz);
                    }
                }
                if (op == 0xFF) last_cimg = w1;   // SETCIMG target FB
                const char* tag = "";
                if (op == 0xFD) tag = " <-SETTIMG";
                else if (op == 0xF5) tag = " <-SETTILE";
                else if (op == 0xF3) tag = " <-LOADBLOCK";
                else if (op == 0xFF) tag = " <-SETCIMG";
                else if (op == 0xFC) tag = " <-SETCOMBINE";
                if (full_detail && (*tag || (i < 64)))
                    fprintf(stderr, "  %08X: %08X %08X op=%02X%s\n", a, w0, w1, op, tag);
                if (op == 0xB8u || op == 0xDFu) { if (dsp > 0) { a = dstack[--dsp]; continue; } break; }
                bool is_dl = ((op == 0x06u) && ((w0 & 0x00FEFFFFu) == 0u)) || (op == 0xDEu);
                if (is_dl && djmp > 0) {
                    uint8_t br = (uint8_t)(w0 >> 16);
                    uint32_t tgt = (w1 & 0x00FFFFF8u) | 0x80000000u;
                    if (br == 0 && dsp < 16) dstack[dsp++] = a + 8u;
                    a = tgt; djmp--; continue;
                }
                a += 8u;
            }
            if (tr_n == 0) { tr_minx = tr_miny = tr_maxx = tr_maxy = 0; }
            fprintf(stderr, "[frame-dl-dump call=%d] TEXRECT count=%d bbox px=(%d,%d)-(%d,%d) cimg=0x%08X\n",
                    call, tr_n, tr_minx, tr_miny, tr_maxx, tr_maxy, last_cimg);
            fflush(stderr);
        }
    }

    uint32_t addr = (dl_phys & 0x3FFFFFFu) | 0x80000000u;
    uint32_t stack[16];
    int sp = 0;
    int jumps_left = 64;
    int cimg_seen = 0;
    int cimg_killed = 0;
    int cimg_passed = 0;
    static int s_logs = 0;
    static int s_summary_logs = 0;
    for (int i = 0; i < 4096; ++i) {
        if ((addr & 0x3FFFFFFu) + 8u > 0x800000u) break;
        uint32_t w0 = (uint32_t)MEM_W(0, (gpr)(int32_t)addr);
        uint32_t w1 = (uint32_t)MEM_W(4, (gpr)(int32_t)addr);
        uint8_t  op = (uint8_t)(w0 >> 24);
        if (op == 0xBEu) { addr += 16u; continue; }     // 16B F5 state cmd — payload is NOT a
                                                        // command (was misparsed/KILLED as a
                                                        // garbage FF SETCIMG, mutating the DL)
        if (op == 0xFFu) {                              // G_SETCIMG
            cimg_seen++;
            // Confirm it decodes as a color image before acting (guards
            // against a data word coincidentally matching during over-walk).
            uint32_t fmt = (w0 >> 21) & 0x7u;
            // RDP address is 24-bit (RDP_ADDRESS_MASK = 0xFFFFFF in
            // rt64_rdp.h), not 26-bit. Factor 5 ucode emits W1 values with
            // garbage in the upper byte (0xFFxxxxxx); RT64 strips it via
            // `address & 0xFFFFFF`. A 26-bit mask leaves those upper bits
            // intact and makes the address look high-mem and "legit", so
            // the < 0x80400000 filter misses the matpool corruptor at
            // W1=0xFF1410A6 (mask 24-bit = 0x1410A6 = matpool overlap).
            uint32_t img = (w1 & 0xFFFFFFu) | 0x80000000u;
            if (fmt <= 4u && w1 != 0u &&
                (img < RS64_FB_MIN_ADDR || img >= RS64_FB_MAX_ADDR)) {
                MEM_W(0, (gpr)(int32_t)addr) = w0 & 0x00FFFFFFu;   // -> G_SPNOOP
                cimg_killed++;
                if (s_logs++ < 16) {
                    uint32_t w = (w0 & 0xFFFu) + 1u;
                    const char* side = (img < RS64_FB_MIN_ADDR) ? "below" : "above";
                    fprintf(stderr, "[cimg-neutralize] killed G_SETCIMG @0x%08X "
                            "img=0x%08X w=%u fmt=%u (%s fb_range=[0x%08X,0x%08X))\n",
                            addr, img, w, fmt, side, RS64_FB_MIN_ADDR, RS64_FB_MAX_ADDR);
                    fflush(stderr);
                }
            } else if (fmt <= 4u && w1 != 0u) {
                cimg_passed++;
            }
        }
        if (op == 0xB8u || op == 0xDFu) {               // G_ENDDL
            if (sp > 0) { addr = stack[--sp]; continue; }
            break;
        }
        // Strict F3DFACTOR5 G_DL: Factor 5 reuses opcode 0x06 for non-DL
        // commands. RT64's op_06_strict_dl only treats 0x06 as G_DL when
        // w0 & 0x00FEFFFF == 0 (only the branch flag at bit 16 may be set).
        // Without this, my walker follows garbage targets into nowhere and
        // exits before reaching the real downstream CIMGs.
        bool is_f3d_dl  = (op == 0x06u) && ((w0 & 0x00FEFFFFu) == 0u);
        bool is_ex2_dl  = (op == 0xDEu);
        if ((is_f3d_dl || is_ex2_dl) && jumps_left > 0) {
            uint8_t branch = (uint8_t)(w0 >> 16);       // 0 = push, 1 = branch
            // 24-bit address mask + 8-byte alignment, matching RT64's
            // RSP::fromSegmentedMasked (0x00FFFFF8). Same fix as the CIMG
            // address mask above — Factor 5 emits W1 with 0xFFxxxxxx upper
            // bits that need stripping.
            uint32_t target = (w1 & 0x00FFFFF8u) | 0x80000000u;
            if (branch == 0 && sp < 16) stack[sp++] = addr + 8u;
            addr = target;
            jumps_left--;
            continue;
        }
        addr += 8u;
    }
    // Per-task summary: lets us spot DLs the walker either never visits (0
    // CIMGs seen) or where the FB-map later shows garbage we didn't kill.
    if (s_summary_logs++ < 32 || cimg_killed > 0) {
        fprintf(stderr, "[cimg-walker] dl=0x%08X visited cimg=%d killed=%d passed=%d "
                "jumps_used=%d stack_max=%d\n",
                (dl_phys & 0x3FFFFFFu) | 0x80000000u,
                cimg_seen, cimg_killed, cimg_passed, 64 - jumps_left, sp);
        fflush(stderr);
    }
}

// Optional task-submission diagnostic. Gated by ROGUESQ_LOG_TASKSUBMIT.
// When enabled, logs task type + data_ptr/size + first 32 bytes of the DL
// buffer for the first 4 tasks + every 64th. Originally added 2026-05-09
// to investigate why GFX tasks were submitted with data_size=0; finding:
// Factor 5's task protocol uses data_ptr but ignores data_size (the DL
// chains via G_DL jumps to static setup code in .text). data_size=0 is
// normal for Factor 5 cinematic tasks.
extern void print_stack_with_symbols(void** frames, USHORT count);

// ROGUESQ_CHECK_CHUNKLIST=1: walk the game's DL chunk free list (head 0x801163B0; a free chunk's
// first word is the next chunk, second the previous) on every SP task start. This runs on an N64
// thread, so no game code mutates the list concurrently. The first bad node dumps RDRAM to
// ROGUESQ_DUMP_RDRAM_PATH (or chunklist_corrupt.bin) for offline blame.
static void rs64_check_chunk_freelist(uint8_t* rdram) {
    static const bool s_chk = [](){ const char* e = std::getenv("ROGUESQ_CHECK_CHUNKLIST"); return e && *e && *e != '0'; }();
    static bool s_dumped = false;
    if (!s_chk || s_dumped) return;
    auto rw = [&](uint32_t off) { return *reinterpret_cast<const uint32_t*>(rdram + (off & 0x7FFFFCu)); };
    uint32_t node = rw(0x1163B0), prev = 0; int steps = 0; const char* why = nullptr;
    while (node != 0 && steps < 8192) {
        const uint32_t off = node & 0x00FFFFFFu;
        if ((node >> 24) != 0x80u || off < 0x400000u || off + 0x108u > 0x800000u) { why = "node outside RDRAM chunk range"; break; }
        if (steps > 0 && rw(off + 4) != prev) { why = "prev link mismatch"; break; }
        prev = node; node = rw(off); ++steps;
    }
    if (steps >= 8192) why = "list longer than 8192 (cycle)";
    { static int s_n = 0; if ((++s_n & 15) == 1) { fprintf(stderr, "[chunklist] task %d free=%d\n", s_n, steps); fflush(stderr); } }
    if (!why) return;
    s_dumped = true;
    fprintf(stderr, "[chunklist] CORRUPT at task start: %s: prev=0x%08X bad=0x%08X steps=%d\n", why, prev, node, steps);
    const char* path = std::getenv("ROGUESQ_DUMP_RDRAM_PATH");
    FILE* f = fopen(path && *path ? path : "chunklist_corrupt.bin", "wb");
    if (f) {
        static uint8_t buf[0x800000];
        for (uint32_t i = 0; i < sizeof(buf); ++i) buf[i] = rdram[i ^ 3];
        fwrite(buf, 1, sizeof(buf), f); fclose(f);
        fprintf(stderr, "[chunklist] dumped RDRAM\n");
    }
    fflush(stderr);
}

extern "C" void rs64_gfx_task_submitted(void);
extern "C" void osSpTaskStartGo_recomp(uint8_t* rdram, recomp_context* ctx) {
    rs64_check_chunk_freelist(rdram);

    static int s_log = -1;
    if (s_log < 0) {
        const char* e = std::getenv("ROGUESQ_LOG_TASKSUBMIT");
        s_log = (e && *e && *e != '0') ? 1 : 0;
    }
    // Separate gate: print host-side stack on first GFX submit so we can
    // identify which game thread / function chain is driving submission.
    // One-shot per task type — once we know who submits GFX vs audio, the
    // log is noise.
    {
        static int s_log_stk = -1;
        if (s_log_stk < 0) {
            const char* e = std::getenv("ROGUESQ_LOG_TASKSUBMIT_STACK");
            s_log_stk = (e && *e && *e != '0') ? 1 : 0;
        }
        if (s_log_stk) {
            OSTask* task = TO_PTR(OSTask, ctx->r4);
            static bool s_logged_gfx = false;
            static bool s_logged_audio = false;
            const bool is_gfx = (task->t.type == 1u);
            const bool is_audio = (task->t.type == 2u);
            if ((is_gfx && !s_logged_gfx) || (is_audio && !s_logged_audio)) {
                if (is_gfx) s_logged_gfx = true;
                if (is_audio) s_logged_audio = true;
                fprintf(stderr,
                    "[task-submit-stack] tid=%lu type=0x%X data_ptr=0x%08X ucode=0x%08X\n",
                    GetCurrentThreadId(),
                    (unsigned)task->t.type,
                    (unsigned)task->t.data_ptr,
                    (unsigned)task->t.ucode);
                void* frames[24];
                USHORT count = RtlCaptureStackBackTrace(0, 24, frames, nullptr);
                print_stack_with_symbols(frames, count);
                fflush(stderr);
            }
        }
    }
    if (s_log) {
        OSTask* task = TO_PTR(OSTask, ctx->r4);
        static int n = 0;
        ++n;
        // Log first 4, every 64th, AND every GFX task regardless of count
        // (audio tasks fire at high rate and would mask GFX in normal sampling).
        const bool is_gfx = (task->t.type == 1u);
        if (n <= 4 || (n & 63) == 0 || is_gfx) {
            fprintf(stderr,
                "[task-submit #%d] type=0x%X flags=0x%X data_ptr=0x%08X data_size=%u ucode=0x%08X ucode_size=%u\n",
                n,
                (unsigned)task->t.type,
                (unsigned)task->t.flags,
                (unsigned)task->t.data_ptr,
                (unsigned)task->t.data_size,
                (unsigned)task->t.ucode,
                (unsigned)task->t.ucode_size);
            uint32_t dp = (uint32_t)task->t.data_ptr;
            if (task->t.type == M_GFXTASK && dp >= 0x80000000u && dp < 0x80800000u) {
                uint32_t off = dp - 0x80000000u;
                fprintf(stderr, "  data_ptr[0..32]:");
                for (int i = 0; i < 32; ++i) {
                    fprintf(stderr, " %02X", (unsigned)rdram[(off + i) ^ 3]);
                }
                fprintf(stderr, "\n");
            }
            fflush(stderr);
        }
    }
    ultramodern::submit_rsp_task(rdram, ctx->r4);
}

// Optional sender-trace for osSendMesg. Captures host stack on the first
// send to each unique queue address — quickly maps which game functions
// drive which queues. Gated by ROGUESQ_LOG_SENDMESG_STACK=1.
//
// We override the librecomp default (link warning re: duplicate symbol is
// expected; lld-link picks our version). The behavior is identical otherwise.
extern "C" int32_t osSendMesg(uint8_t* rdram, int32_t mq_, OSMesg mesg, s32 flag);

extern "C" void rs64_gfx_task_submitted(void);
extern "C" void osSendMesg_recomp(uint8_t* rdram, recomp_context* ctx) {
    rs64_mesg_trace(rdram, "send", (uint32_t)ctx->r4, (int)(s32)ctx->r6, (uint32_t)ctx->r31);
    // A graphics task counts as in flight from the game's REQUEST (type-1 message to the RSP scheduler
    // queue), not from osSpTaskStartGo: with an audio task on the RSP the request sits pending while the
    // game already rebuilds chunks the pending list references (2026-09-08 ring: 0x752B98 E7 -> E9 mid-parse).
    if ((uint32_t)ctx->r4 == 0x8011A420u) {
        const uint32_t mp = (uint32_t)ctx->r5;
        { static const bool s_lg = [](){ const char* e = std::getenv("ROGUESQ_LOG_GFX_TASK"); return e && *e && *e != '0'; }();
          static unsigned s_hist[256] = {0}; static unsigned s_tot = 0;
          const int ty = (mp >= 0x80000000u && mp < 0x80800000u) ? rdram[(mp & 0x7FFFFFu) ^ 3] : 255;
          ++s_hist[ty]; if (s_lg && (++s_tot & 255) == 0) { fprintf(stderr, "[sched-send] total=%u type1=%u type2=%u type0=%u type81=%u other=%u mp=0x%08X tid=%lu" "\n", s_tot, s_hist[1], s_hist[2], s_hist[0], s_hist[0x81], s_tot - s_hist[1] - s_hist[2] - s_hist[0] - s_hist[0x81], mp, GetCurrentThreadId()); fflush(stderr); } }
        if (mp >= 0x80000000u && mp < 0x80800000u && rdram[(mp & 0x7FFFFFu) ^ 3] == 1u) {
            rs64_gfx_task_submitted();
            static const bool s_lg = [](){ const char* e = std::getenv("ROGUESQ_LOG_GFX_TASK"); return e && *e && *e != '0'; }();
            static unsigned s_n = 0;
            if (s_lg && ++s_n <= 6) { fprintf(stderr, "[gfx-request #%u] ra=0x%08X tid=%lu\n", s_n, (uint32_t)ctx->r31, GetCurrentThreadId()); fflush(stderr); }
        }
    }
    static int s_log_stk = -1;
    if (s_log_stk < 0) {
        const char* e = std::getenv("ROGUESQ_LOG_SENDMESG_STACK");
        s_log_stk = (e && *e && *e != '0') ? 1 : 0;
    }
    if (s_log_stk) {
        // Only trace sends to the graph thread's queue at 0x8011A420 — the
        // queue func_80019BF4 receives on. Other queues are noise for this
        // investigation. Easy to extend later by editing this list.
        const uint32_t target_mq = 0x8011A420u;
        const uint32_t mq_addr = (uint32_t)ctx->r4;
        if (mq_addr == target_mq) {
            static int s_count = 0;
            ++s_count;
            if (s_count <= 3) {
                fprintf(stderr,
                    "[sendmesg #%d] tid=%lu mq=0x%08X mesg_ptr=0x%08X flag=%d\n",
                    s_count,
                    GetCurrentThreadId(),
                    mq_addr,
                    (unsigned)ctx->r5,
                    (int)ctx->r6);
                void* frames[24];
                USHORT count = RtlCaptureStackBackTrace(0, 24, frames, nullptr);
                print_stack_with_symbols(frames, count);
                // Dump the message bytes (first 0x10) so we can see the tag/payload.
                uint32_t mesg_ptr = (uint32_t)ctx->r5;
                if (mesg_ptr >= 0x80000000u && mesg_ptr < 0x80800000u) {
                    uint32_t off = mesg_ptr - 0x80000000u;
                    fprintf(stderr, "  mesg[0..16]:");
                    for (int i = 0; i < 16; ++i) {
                        fprintf(stderr, " %02X", (unsigned)rdram[(off + i) ^ 3]);
                    }
                    fprintf(stderr, "\n");
                }
                fflush(stderr);
            }
        }
    }
    // ROGUESQ_LOG_RECV_BLOCK=1 also reports BLOCK sends to a full queue (and how long they took).
    static const bool s_sb = [](){ const char* e = std::getenv("ROGUESQ_LOG_RECV_BLOCK"); return e && *e && *e != '0'; }();
    const uint32_t sq = (uint32_t)ctx->r4; uint32_t st0 = 0; bool full = false;
    if (s_sb && (s32)ctx->r6 != 0 && sq >= 0x80000000u && sq < 0x80800000u) {
        const uint32_t* mq = reinterpret_cast<const uint32_t*>(rdram + (sq & 0x7FFFFFu));
        full = (mq[2] >= mq[4]);   // validCount >= msgCount
        if (full) { st0 = GetTickCount(); fprintf(stderr, "[send-block] tid=%lu q=0x%08X waiting (full)\n", GetCurrentThreadId(), sq); fflush(stderr); }
    }
    ctx->r2 = osSendMesg(rdram, (int32_t)ctx->r4, (OSMesg)ctx->r5, (s32)ctx->r6);
    if (full) { uint32_t dt = GetTickCount() - st0; if (dt > 200) { fprintf(stderr, "[send-block] tid=%lu q=0x%08X resumed after %u ms\n", GetCurrentThreadId(), sq, dt); fflush(stderr); } }
}

// osDestroyThread diagnostic + guard (2026-09-07): with the frame-barrier receives blocking again, the
// boot thread's osDestroyThread of a queued thread crashed in thread_queue_remove on a wild queue
// pointer (SEH-swallowed, game thread died = "stall"). Log the victim and, if its queue chain is
// not inside RDRAM, mark it STOPPED so ultramodern skips the corrupt walk.
extern "C" void osDestroyThread(uint8_t* rdram, PTR(OSThread) t_);
extern "C" void osDestroyThread_recomp(uint8_t* rdram, recomp_context* ctx) {
    PTR(OSThread) t_ = (PTR(OSThread))(int32_t)ctx->r4;
    if (t_ != NULLPTR) {
        OSThread* t = TO_PTR(OSThread, t_);
        auto in_ram = [](uint32_t p) { return p >= 0x80000000u && p < 0x80800000u; };
        // A non-STOPPED thread with a NULL queue is mid-transition between queues (popped, not yet
        // re-inserted); ultramodern would TO_PTR(NULL) and read wild. Treat as "in no queue".
        bool qok = (t->queue == ultramodern::running_queue) || in_ram((uint32_t)t->queue) ||
                   (t->queue == NULLPTR && t->state == (uint16_t)OSThreadState::STOPPED);
        bool chain_ok = true; int hops = 0;
        if (qok && t->queue != NULLPTR && t->queue != ultramodern::running_queue && t->state != (uint16_t)OSThreadState::STOPPED) {
            PTR(OSThread) cur = *TO_PTR(PTR(OSThread), t->queue);
            while (cur != NULLPTR && hops < 64) {
                if (!in_ram((uint32_t)cur)) { chain_ok = false; break; }
                cur = TO_PTR(OSThread, cur)->next; ++hops;
            }
        }
        fprintf(stderr, "[osDestroyThread] t=0x%08X id=%d pri=%d state=%u queue=0x%08X qok=%d chain_ok=%d hops=%d\n",
                (uint32_t)t_, (int)t->id, (int)t->priority, (unsigned)t->state, (uint32_t)t->queue, (int)qok, (int)chain_ok, hops);
        fflush(stderr);
        if (!qok || !chain_ok) t->state = (uint16_t)OSThreadState::STOPPED;
    }
    osDestroyThread(rdram, t_);
}

// Override upstream's osStopThread_recomp because upstream's osStopThread
// asserts(false) when called with a non-self thread handle (threads.cpp:278).
// RS64's func_80000C68 stops other threads. We replicate libultra's actual
// semantics: remove the target thread from any queue it's blocked on and
// mark it STOPPED so the scheduler doesn't try to schedule it. The host
// std::thread is parked in wait_for_resumed until a future osStartThread.
extern "C" void osStopThread_recomp(uint8_t* rdram, recomp_context* ctx) {
    PTR(OSThread) t_ = (PTR(OSThread))(int32_t)ctx->r4;
    if (t_ == NULLPTR) {
        // Self-stop: same path upstream takes — yield to scheduler.
        ultramodern::run_next_thread_and_wait(PASS_RDRAM1);
        return;
    }
    OSThread* t = TO_PTR(OSThread, t_);
    if (t->state != OSThreadState::STOPPED) {
        if (t->queue != NULLPTR) {
            ultramodern::thread_queue_remove(PASS_RDRAM t->queue, t_);
        }
        t->state = OSThreadState::STOPPED;
    }
}

