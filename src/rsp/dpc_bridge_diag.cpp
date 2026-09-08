// dpc_bridge_diag.cpp — DPC bridge diagnostics (relocated from dpc_bridge.cpp).
//
// All command-stream tracing the bridge used to inline into rsp_dpc_submit /
// rsp_task_log_and_reset lives here. None of it affects the forwarded RDP
// command stream — every hook is gated by ROGUESQ_LOG_DPC (or a one-shot /
// ROGUESQ_LOG_SUBMIT_SHAPE probe) and the diagnostic-support state (combiner
// trackers, RDP-state shadows, per-task combine histogram) is file-local here
// because nothing outside the bridge reads it.
//
// The core (dpc_bridge.cpp) calls the dpc_diag_* hooks declared in
// dpc_bridge.h at the matching points in the submission path.

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include "librecomp/rsp.hpp"
#include "dpc_bridge.h"

extern uint8_t dmem[];  // defined in librecomp/rsp.cpp

namespace {

// Cached env-var read for dpc_bridge logging. The bridge emits ~1000+ lines
// per second of [task-brief]/[task#N gfx]/[dpc-pak]/[dpc-cine]/[dpc-64tri]
// during cinematic playback — extremely noisy. Default off; enable with
// ROGUESQ_LOG_DPC=1 (or ROGUESQ_LOG_ALL=1).
bool log_dpc() {
    static const bool v = []{
        const char *a = std::getenv("ROGUESQ_LOG_ALL");
        if (a && *a && *a != '0') return true;
        const char *e = std::getenv("ROGUESQ_LOG_DPC");
        return e && *e && *e != '0';
    }();
    return v;
}

// --- Diagnostic-support state (file-local; nothing outside the bridge uses it) ---

// True while the most recently-set combiner mux equals the Pak's
// (TEXEL × SHADE = 0xFC127E24/0xFFFFFFFF). Texture-command logging gates on
// this so we only see the Pak's loads, not the 159 text loads.
bool s_in_pak_combiner = false;
// Track the most recent SETTIMG address while in Pak combiner block —
// LOADTLUT consumes whatever SETTIMG address is current (RGBA16 source).
uint32_t s_pak_last_tlut_addr = 0;

// Cinematic / particle combiner instrumentation. Mux 0xFC11FE23/0xFFFFFFFF
// covers the cinematic's TEXRECT-rendered particles (shockwaves, fireballs).
bool s_in_cine_combiner = false;
uint32_t s_cine_last_settimg_fmt = 0;
uint32_t s_cine_last_settimg_siz = 0;
uint32_t s_cine_last_settimg_addr = 0;
uint32_t s_cine_last_cimg_addr = 0;   // current SET_COLOR_IMAGE target fb

// Shadow of the most recent RDP state-set commands, so the 64-tri particle
// mux entry can dump the full draw context at the moment it is activated.
uint32_t s_last_othermode_l = 0, s_last_othermode_h = 0;
uint32_t s_last_settimg_w0 = 0, s_last_settimg_w1 = 0;
uint32_t s_last_setcimg_w0 = 0, s_last_setcimg_w1 = 0;
uint32_t s_last_settile_w0 = 0, s_last_settile_w1 = 0;
uint32_t s_last_fogcolor = 0, s_last_primcolor = 0;
uint32_t s_last_envcolor = 0, s_last_blendcolor = 0;

// Per-task distinct G_SETCOMBINE mux tracker. Multi-pass rendering produces
// different (w0,w1) pairs per task; single-pass repeats one mux. Capped at 16.
struct TaskCombineEntry { uint32_t w0; uint32_t w1; uint32_t count; };
TaskCombineEntry s_task_combine[16] = {};
uint32_t s_task_combine_uniq = 0;
uint32_t s_task_combine_total = 0;
uint32_t s_task_combine_overflow = 0;

// Read a big-endian 32-bit word from a submission at byte offset `off`.
inline uint32_t be_w(uint8_t* rdram, int64_t mips, int off) {
    return ((uint32_t)(uint8_t)MEM_B(off + 0, mips) << 24) |
           ((uint32_t)(uint8_t)MEM_B(off + 1, mips) << 16) |
           ((uint32_t)(uint8_t)MEM_B(off + 2, mips) <<  8) |
           ((uint32_t)(uint8_t)MEM_B(off + 3, mips));
}

} // namespace

// ---------------------------------------------------------------------------
// Early per-submission inspection: SET_COLOR_IMAGE address tracking, the boot→
// late-cinematic RDRAM/DMEM EARLY_DUMP probe, and out-of-bounds CIMG logging.
// (No effect on the forwarded stream — logging + one-shot memory dumps only.)
// ---------------------------------------------------------------------------
void dpc_diag_inspect_early(uint8_t* rdram, uint32_t submit_lo, uint32_t submit_hi) {
    int64_t mips_first = (int64_t)(int32_t)(submit_lo + 0x80000000);
    uint8_t op = (uint8_t)MEM_B(0, mips_first) & 0x3F;
    // Track all SET_COLOR_IMAGE addresses (op 0x3F) so we can compare them
    // against VI_ORIGIN to diagnose visual gaps.
    if (op == 0x3F) {
        uint32_t w1 = be_w(rdram, mips_first, 4);
        static std::atomic<int> s_set_count{0};
        int n = ++s_set_count;
        if (n <= 16 || (n & 63) == 0) {
            fprintf(stderr, "[set-cimg #%d] addr=0x%08X (phys=0x%06X)\n",
                    n, w1, w1 & 0x00FFFFFF);
            fflush(stderr);
        }
    }

    // EARLY DUMP: capture RDRAM at MIPS 0x4B7800 + DMEM at the first few submit
    // calls. Lets us compare to PJ64's first-explosion screenshot.
    {
        static std::atomic<uint32_t> s_early_dump{0};
        uint32_t n = ++s_early_dump;
        if (n == 1 || n == 1000 || n == 3000 || n == 6000 || n == 10000 ||
            n == 15000 || n == 20000 || n == 25000 || n == 30000) {
            fprintf(stderr, "[early-dump #%u] RDRAM 0x4B7800-0x4B7900 host order:\n", n);
            for (uint32_t row = 0; row < 16; ++row) {
                uint32_t base = 0x4B7800u + row * 16;
                fprintf(stderr, "  %08X:", base);
                for (uint32_t col = 0; col < 16; ++col)
                    fprintf(stderr, " %02X", (unsigned)rdram[base + col]);
                fprintf(stderr, "\n");
            }
            fprintf(stderr, "[early-dump #%u] DMEM 0x170-0x1C0 host order:\n", n);
            for (uint32_t row = 0; row < 5; ++row) {
                uint32_t base = 0x170u + row * 16;
                fprintf(stderr, "  %03X:", base);
                for (uint32_t col = 0; col < 16; ++col)
                    fprintf(stderr, " %02X", (unsigned)dmem[base + col]);
                fprintf(stderr, "\n");
            }
            fflush(stderr);
        }
    }

    // OOB-CIMG: catch G_SETCIMG (byte 0 == 0xFF) with a bogus addr OR width.
    //   addr >= 0x800000  : past 8MB RDRAM (typical 0x00FFFFFF garbage)
    //   addr <  0x100000  : asset/code region, not framebuffer space
    //   width >  640      : impossible (lo-res 320, hi-res 640)
    if ((submit_hi - submit_lo) >= 8) {
        int64_t mips = (int64_t)(int32_t)(submit_lo + 0x80000000);
        uint8_t b0w = (uint8_t)MEM_B(0, mips);
        if (b0w == 0xFF) {  // strict G_SETCIMG opcode match
            uint32_t w0w = be_w(rdram, mips, 0);
            uint32_t w1w = be_w(rdram, mips, 4);
            uint32_t addr24 = w1w & 0x00FFFFFF;
            uint8_t  segHigh = (uint8_t)((w1w >> 24) & 0xFF);
            uint16_t cimg_width = (uint16_t)((w0w & 0xFFFu) + 1);
            const bool oob_high  = (addr24 >= 0x00800000u);
            const bool oob_low   = (addr24 < 0x00100000u && addr24 != 0);
            const bool oob_width = (cimg_width > 640u);
            (void)segHigh;
            if (oob_high || oob_low || oob_width) {
                static std::atomic<uint64_t> s_oob{0};
                static std::atomic<uint32_t> s_oob_low{0};
                static std::atomic<uint32_t> s_oob_width{0};
                uint64_t n = ++s_oob;
                bool log_low   = oob_low && (++s_oob_low <= 30);
                bool log_high  = oob_high && (n == 1 || (n & (n - 1)) == 0);
                bool log_width = oob_width && (++s_oob_width <= 40);
                if (log_low || log_high || log_width) {
                    const char *region = oob_width ? "WIDTH" : oob_high ? "HIGH" : "LOW";
                    uint32_t submit_size = submit_hi - submit_lo;
                    fprintf(stderr,
                        "[cimg-oob-1] #%llu region=%s slot=%d dl=0x%08X size=%u w0=0x%08X w1=0x%08X addr24=0x%06X w=%u segHigh=0x%02X\n",
                        (unsigned long long)n, region, g_cine_current_slot,
                        (unsigned)submit_lo | 0x80000000u, submit_size, w0w, w1w, addr24,
                        (unsigned)cimg_width, (unsigned)segHigh);
                    if (log_low || log_width) {
                        int64_t pre  = (int64_t)(int32_t)((submit_lo - 32) + 0x80000000);
                        int64_t post = (int64_t)(int32_t)((submit_lo +  8) + 0x80000000);
                        fprintf(stderr, "  [ctx] pre32:");
                        for (int i = 0; i < 32; ++i) fprintf(stderr, " %02X", (unsigned)(uint8_t)MEM_B(i, pre));
                        fprintf(stderr, "  cmd:");
                        for (int i = 0; i < 8;  ++i) fprintf(stderr, " %02X", (unsigned)(uint8_t)MEM_B(i, mips));
                        fprintf(stderr, "  post32:");
                        for (int i = 0; i < 32; ++i) fprintf(stderr, " %02X", (unsigned)(uint8_t)MEM_B(i, post));
                        fprintf(stderr, "\n");
                    }
                    fflush(stderr);
                }
                // One-shot RDRAM + DMEM dump at first OOB (compare to PJ64).
                {
                    static std::atomic<bool> s_dumped{false};
                    bool expected = false;
                    if (s_dumped.compare_exchange_strong(expected, true)) {
                        fprintf(stderr, "[mem-dump-4B7800] 256 bytes (host order, MIPS 0x4B7800-0x4B7900):\n");
                        for (uint32_t row = 0; row < 16; ++row) {
                            uint32_t base = 0x4B7800u + row * 16;
                            fprintf(stderr, "  %08X:", base);
                            for (uint32_t col = 0; col < 16; ++col)
                                fprintf(stderr, " %02X", (unsigned)rdram[base + col]);
                            fprintf(stderr, "\n");
                        }
                        fprintf(stderr, "[mem-dump-DMEM] 1KB (host order, DMEM 0x000-0x400):\n");
                        for (uint32_t row = 0; row < 64; ++row) {
                            uint32_t base = row * 16;
                            fprintf(stderr, "  %03X:", base);
                            for (uint32_t col = 0; col < 16; ++col)
                                fprintf(stderr, " %02X", (unsigned)dmem[base + col]);
                            fprintf(stderr, "\n");
                        }
                        fflush(stderr);
                    }
                }
            }
        }
    }
}

// FULL_SYNC byte count log (the detection itself is load-bearing, in the core).
void dpc_diag_fullsync_log() {
    static std::atomic<uint64_t> s_fs{0};
    uint64_t n = ++s_fs;
    if (log_dpc() && (n <= 8 || (n & 31) == 0)) {
        fprintf(stderr, "[dpc] FULL_SYNC byte sent #%llu\n", (unsigned long long)n);
        fflush(stderr);
    }
}

// ---------------------------------------------------------------------------
// Per-8-byte-command tracking + logging: RDP-state shadows, PRIM/ENV colors,
// G_SETCOMBINE mux tracking (Pak / cinematic / 64-tri), in-block texture loads,
// SETTIMG/SETCIMG tracking, OOB-CIMG correlation, and cinematic TEXRECT logs.
// (The load-bearing PRIM override + fb-slot-ownership write stay in the core.)
// ---------------------------------------------------------------------------
void dpc_diag_track_8b(uint8_t* rdram, uint32_t submit_lo) {
    int64_t mips = (int64_t)(int32_t)(submit_lo + 0x80000000);
    uint8_t op6 = (uint8_t)MEM_B(0, mips) & 0x3F;

    // Shadow the most recent state-set commands for the 64-tri context dump.
    if (op6 == 0x22 || op6 == 0x23 || op6 == 0x2F || op6 == 0x3D || op6 == 0x3F ||
        op6 == 0x35 || op6 == 0x38 || op6 == 0x39 || op6 == 0x3A || op6 == 0x3B) {
        uint32_t w0_now = be_w(rdram, mips, 0);
        uint32_t w1_now = be_w(rdram, mips, 4);
        switch (op6) {
            case 0x22: s_last_othermode_l = w1_now; break;
            case 0x23: s_last_othermode_h = w1_now; break;
            case 0x2F: s_last_othermode_h = w0_now; s_last_othermode_l = w1_now; break;
            case 0x3D: s_last_settimg_w0 = w0_now; s_last_settimg_w1 = w1_now; break;
            case 0x3F: s_last_setcimg_w0 = w0_now; s_last_setcimg_w1 = w1_now; break;
            case 0x35: s_last_settile_w0 = w0_now; s_last_settile_w1 = w1_now; break;
            case 0x38: s_last_fogcolor = w1_now; break;
            case 0x39: s_last_blendcolor = w1_now; break;
            case 0x3A: s_last_primcolor = w1_now; break;
            case 0x3B: s_last_envcolor = w1_now; break;
        }
    }

    // G_SETPRIMCOLOR / G_SETENVCOLOR: log the RGBA values being loaded.
    if (op6 == 0x3A || op6 == 0x3B) {
        static std::atomic<uint64_t> s_prim{0}, s_env{0};
        uint32_t w1 = be_w(rdram, mips, 4);
        uint64_t n = (op6 == 0x3A) ? ++s_prim : ++s_env;
        if (log_dpc() && (n <= 12 || n == 100 || n == 1000)) {
            const char *name = (op6 == 0x3A) ? "PRIM" : "ENV";
            fprintf(stderr, "[dpc] G_SET%s_COLOR #%llu RGBA=%02X %02X %02X %02X (w1=0x%08X)\n",
                name, (unsigned long long)n,
                (w1 >> 24) & 0xFF, (w1 >> 16) & 0xFF, (w1 >> 8) & 0xFF, (w1 >> 0) & 0xFF, w1);
            fflush(stderr);
        }
    }

    // G_SETCOMBINE: track distinct muxes + detect the Pak / cinematic / 64-tri
    // combiner blocks (gate subsequent texture-command logging).
    if (op6 == 0x3C) {
        uint32_t w0 = be_w(rdram, mips, 0);
        uint32_t w1 = be_w(rdram, mips, 4);
        s_task_combine_total++;
        bool found = false;
        for (uint32_t i = 0; i < s_task_combine_uniq; i++) {
            if (s_task_combine[i].w0 == w0 && s_task_combine[i].w1 == w1) {
                s_task_combine[i].count++; found = true; break;
            }
        }
        if (!found) {
            if (s_task_combine_uniq < 16) {
                s_task_combine[s_task_combine_uniq] = { w0, w1, 1 };
                s_task_combine_uniq++;
            } else {
                s_task_combine_overflow++;
            }
        }
        // Pak combiner = TEXEL × SHADE (0xFC127E24/0xFFFFFFFF).
        s_in_pak_combiner = (w0 == 0xFC127E24u && w1 == 0xFFFFFFFFu);
        if (s_in_pak_combiner && log_dpc()) {
            fprintf(stderr, "[dpc-pak] >>> ENTER Pak combiner block\n"); fflush(stderr);
        }
        // Cinematic-particle combiner (0xFC11FE23/0xFFFFFFFF).
        bool was_cine = s_in_cine_combiner;
        s_in_cine_combiner = (w0 == 0xFC11FE23u && w1 == 0xFFFFFFFFu);
        if (s_in_cine_combiner && !was_cine && log_dpc()) {
            static std::atomic<uint64_t> s_first{0};
            uint64_t v = ++s_first;
            if (v <= 4) {
                fprintf(stderr, "[dpc-cine] >>> ENTER cinematic particle combiner block #%llu\n",
                    (unsigned long long)v);
                fflush(stderr);
            }
        }
        // 64-triangle particle mux (0xFC127FFF/0xFFFFFE3F): dump full draw context.
        static bool s_in_64tri = false;
        const bool now_64tri = (w0 == 0xFC127FFFu && w1 == 0xFFFFFE3Fu);
        if (now_64tri && !s_in_64tri && log_dpc()) {
            static std::atomic<uint64_t> s_first{0};
            uint64_t v = ++s_first;
            if (v <= 8 || (v & 63) == 0) {
                fprintf(stderr,
                    "[dpc-64tri] ENTER #%llu  L=0x%08X H=0x%08X  TIMG=[%08X %08X]  CIMG=[%08X %08X]  TILE=[%08X %08X]  FOG=%08X PRIM=%08X ENV=%08X BLEND=%08X\n",
                    (unsigned long long)v, s_last_othermode_l, s_last_othermode_h,
                    s_last_settimg_w0, s_last_settimg_w1, s_last_setcimg_w0, s_last_setcimg_w1,
                    s_last_settile_w0, s_last_settile_w1, s_last_fogcolor, s_last_primcolor,
                    s_last_envcolor, s_last_blendcolor);
                fflush(stderr);
            }
        }
        s_in_64tri = now_64tri;
    }

    // Inside the Pak combiner block, log every texture-loading command.
    if (s_in_pak_combiner && log_dpc()) {
        const char *name = nullptr;
        switch (op6) {
            case 0x30: name = "G_LOADTLUT";    break;
            case 0x32: name = "G_SETTILESIZE"; break;
            case 0x33: name = "G_LOADBLOCK";   break;
            case 0x34: name = "G_LOADTILE";    break;
            case 0x35: name = "G_SETTILE";     break;
            case 0x3D: name = "G_SETTIMG";     break;
            case 0x3F: name = "G_SETCIMG";     break;
            case 0x05: case 0x07: case 0x08: case 0x09:
            case 0x0A: case 0x0B: case 0x0C: case 0x0D:
            case 0x0E: case 0x0F: name = "G_TRI"; break;
            default: break;
        }
        if (name) {
            uint32_t w0 = be_w(rdram, mips, 0);
            uint32_t w1 = be_w(rdram, mips, 4);
            if (op6 == 0x3D) {
                uint32_t fmt = (w0 >> 21) & 0x7, siz = (w0 >> 19) & 0x3;
                uint32_t width = (w0 & 0xFFF) + 1, addr = w1 & 0x00FFFFFF;
                const char *fmt_name = "?";
                switch (fmt) {
                    case 0: fmt_name = "RGBA"; break;  case 1: fmt_name = "YUV"; break;
                    case 2: fmt_name = "CI";   break;  case 3: fmt_name = "IA";  break;
                    case 4: fmt_name = "I";    break;
                }
                const char *siz_name = (siz == 0) ? "4b" : (siz == 1) ? "8b" : (siz == 2) ? "16b" : "32b";
                fprintf(stderr, "[dpc-pak] %s fmt=%s siz=%s width=%u addr=0x%08X\n",
                    name, fmt_name, siz_name, width, addr);
                if (fmt == 0 /* RGBA */) s_pak_last_tlut_addr = addr;
            } else if (op6 == 0x35) {
                uint32_t fmt = (w0 >> 21) & 0x7, siz = (w0 >> 19) & 0x3;
                uint32_t line = (w0 >> 9) & 0x1FF, tmem = w0 & 0x1FF;
                uint32_t tile = (w1 >> 24) & 0x7, palette = (w1 >> 20) & 0xF;
                fprintf(stderr, "[dpc-pak] %s tile=%u fmt=%u siz=%u line=%u tmem=0x%X palette=%u\n",
                    name, tile, fmt, siz, line, tmem, palette);
            } else if (op6 == 0x33) {
                uint32_t uls = (w0 >> 12) & 0xFFF, ult = w0 & 0xFFF;
                uint32_t tile = (w1 >> 24) & 0x7, lrs = (w1 >> 12) & 0xFFF, dxt = w1 & 0xFFF;
                fprintf(stderr, "[dpc-pak] %s tile=%u uls=%u ult=%u texels=%u dxt=%u\n",
                    name, tile, uls, ult, lrs + 1, dxt);
            } else if (op6 == 0x30) {
                uint32_t tile = (w1 >> 24) & 0x7;
                fprintf(stderr, "[dpc-pak] %s tile=%u w0=0x%08X w1=0x%08X\n", name, tile, w0, w1);
                if (s_pak_last_tlut_addr != 0) {
                    fprintf(stderr, "[dpc-pak] TLUT @ 0x%08X (RGBA16):\n", s_pak_last_tlut_addr);
                    for (int i = 0; i < 16; i++) {
                        uint32_t off = s_pak_last_tlut_addr + i * 2;
                        uint8_t hi = rdram[(off + 0) ^ 3], lo = rdram[(off + 1) ^ 3];
                        uint16_t rgba16 = ((uint16_t)hi << 8) | lo;
                        int r = (rgba16 >> 11) & 0x1F, g = (rgba16 >> 6) & 0x1F;
                        int b = (rgba16 >> 1) & 0x1F, a = rgba16 & 0x1;
                        fprintf(stderr, "  [%2d] rgba16=0x%04X  R=%2d G=%2d B=%2d A=%d  (R8=%3d G8=%3d B8=%3d)\n",
                            i, rgba16, r, g, b, a, (r << 3) | (r >> 2), (g << 3) | (g >> 2), (b << 3) | (b >> 2));
                    }
                }
            } else {
                fprintf(stderr, "[dpc-pak] %s w0=0x%08X w1=0x%08X\n", name, w0, w1);
            }
            fflush(stderr);
        }
    }

    // Track latest SETTIMG / SETCIMG globally for texrect correlation context.
    if (op6 == 0x3D) {
        uint32_t w0 = be_w(rdram, mips, 0);
        uint32_t w1 = be_w(rdram, mips, 4);
        s_cine_last_settimg_fmt = (w0 >> 21) & 0x7;
        s_cine_last_settimg_siz = (w0 >> 19) & 0x3;
        s_cine_last_settimg_addr = w1 & 0x00FFFFFF;
    }
    if (op6 == 0x3F) {
        uint32_t w1 = be_w(rdram, mips, 4);
        s_cine_last_cimg_addr = w1 & 0x00FFFFFF;
        // Always log when the CIMG address is OOB (>= 8MB), throttled.
        if (s_cine_last_cimg_addr >= 0x00800000u) {
            static std::atomic<uint64_t> s_oob{0};
            uint64_t n = ++s_oob;
            if (n == 1 || (n & (n - 1)) == 0) {
                uint32_t w0 = be_w(rdram, mips, 0);
                fprintf(stderr, "[cimg-oob] #%llu slot=%d dl=0x%08X w0=0x%08X w1=0x%08X addr24=0x%08X\n",
                    (unsigned long long)n, g_cine_current_slot, (unsigned)mips, w0, w1, s_cine_last_cimg_addr);
                fflush(stderr);
            }
        }
        // PHASE 21 correlation log (the slot-ownership map write is in the core).
        if (log_dpc()) {
            static std::atomic<uint64_t> s_seq{0};
            uint64_t v = ++s_seq;
            if (v <= 200 || (v % 100) == 0) {
                fprintf(stderr, "[cimg-corr] seq=%llu slot=%d addr=0x%08X\n",
                    (unsigned long long)v, g_cine_current_slot, s_cine_last_cimg_addr);
                fflush(stderr);
            }
        }
    }

    // Cinematic particle combiner — log first ~32 8-byte TEXRECTs + context.
    if (s_in_cine_combiner && op6 == 0x24) {
        static std::atomic<uint64_t> s_rect{0};
        uint64_t v = ++s_rect;
        if (v <= 32) {
            uint32_t w0 = be_w(rdram, mips, 0);
            uint32_t w1 = be_w(rdram, mips, 4);
            uint32_t lrx = (w0 >> 12) & 0xFFF, lry = w0 & 0xFFF;
            uint32_t tile = (w1 >> 24) & 0x7, ulx = (w1 >> 12) & 0xFFF, uly = w1 & 0xFFF;
            fprintf(stderr,
                "[dpc-cine] TEXRECT #%llu ul=(%u,%u) lr=(%u,%u) size=(%u,%u) tile=%u  fb=0x%08X texSrc=0x%08X fmt=%u siz=%u\n",
                (unsigned long long)v, ulx >> 2, uly >> 2, lrx >> 2, lry >> 2,
                (lrx - ulx) >> 2, (lry - uly) >> 2, tile,
                s_cine_last_cimg_addr, s_cine_last_settimg_addr,
                s_cine_last_settimg_fmt, s_cine_last_settimg_siz);
            fflush(stderr);
        }
    }
    if (!s_in_pak_combiner && (op6 == 0x3D || op6 == 0x3E || op6 == 0x3F)) {
        static std::atomic<uint64_t> s_seen{0};
        uint64_t n = ++s_seen;
        if (log_dpc() && n <= 8) {
            uint32_t w0 = be_w(rdram, mips, 0);
            uint32_t w1 = be_w(rdram, mips, 4);
            const char *name = (op6 == 0x3F) ? "SET_COLOR_IMAGE"
                             : (op6 == 0x3E) ? "SET_DEPTH_IMAGE" : "SET_TEXTURE_IMAGE";
            fprintf(stderr, "[dpc] %s #%llu w0=0x%08X w1=0x%08X (addr=0x%08X)\n",
                name, (unsigned long long)n, w0, w1, w1 & 0x00FFFFFF);
            fflush(stderr);
        }
    }
}

// 16-byte LLE TEXRECT chunk (particle billboards) — geometry + texture context.
void dpc_diag_track_16b(uint8_t* rdram, uint32_t submit_lo) {
    if (!s_in_cine_combiner) return;
    int64_t mips = (int64_t)(int32_t)(submit_lo + 0x80000000);
    uint8_t op6 = (uint8_t)MEM_B(0, mips) & 0x3F;
    if (op6 != 0x24 && op6 != 0x25) return;  // TEXRECT / TEXRECTFLIP
    static std::atomic<uint64_t> s_rect{0};
    uint64_t v = ++s_rect;
    if (v > 32) return;
    uint32_t w0 = be_w(rdram, mips, 0);
    uint32_t w1 = be_w(rdram, mips, 4);
    uint32_t w2 = be_w(rdram, mips, 8);
    uint32_t w3 = be_w(rdram, mips, 12);
    uint32_t lrx = (w0 >> 12) & 0xFFF, lry = w0 & 0xFFF;
    uint32_t tile = (w1 >> 24) & 0x7, ulx = (w1 >> 12) & 0xFFF, uly = w1 & 0xFFF;
    fprintf(stderr,
        "[dpc-cine] TEXRECT%s #%llu ul=(%u,%u) lr=(%u,%u) size=(%u,%u) tile=%u  fb=0x%08X tex=0x%08X fmt=%u siz=%u stuv=0x%08X dxdy=0x%08X\n",
        (op6 == 0x25) ? "_FLIP" : "", (unsigned long long)v,
        ulx >> 2, uly >> 2, lrx >> 2, lry >> 2,
        (lrx > ulx) ? ((lrx - ulx) >> 2) : 0, (lry > uly) ? ((lry - uly) >> 2) : 0,
        tile, s_cine_last_cimg_addr, s_cine_last_settimg_addr,
        s_cine_last_settimg_fmt, s_cine_last_settimg_siz, w2, w3);
    fflush(stderr);
}

// Submission-size distribution log (ROGUESQ_LOG_SUBMIT_SHAPE): whether Factor 5
// bumps DPC_END per command (size 8) or batches multiple commands per bump.
void dpc_diag_submit_shape(uint8_t* rdram, uint32_t submit_lo, uint32_t submit_hi) {
    static const bool s_log = []{
        const char* v = std::getenv("ROGUESQ_LOG_SUBMIT_SHAPE");
        return v && *v && *v != '0';
    }();
    if (!s_log) return;
    uint32_t span = submit_hi - submit_lo;
    int bucket = (span <= 8)    ? 0 : (span <= 16)  ? 1 : (span <= 24)  ? 2
               : (span <= 32)   ? 3 : (span <= 64)  ? 4 : (span <= 128) ? 5
               : (span <= 256)  ? 6 : (span <= 512) ? 7 : (span <= 1024) ? 8 : 9;
    static std::atomic<uint64_t> s_count[10] = {};
    static std::atomic<uint64_t> s_total{0};
    uint64_t b = ++s_count[bucket];
    uint64_t t = ++s_total;
    if (b == 1 || (b & (b - 1)) == 0) {
        uint8_t bytes[16] = {};
        int64_t mp = (int64_t)(int32_t)(submit_lo + 0x80000000);
        int cap = (int)std::min<uint32_t>(span, 16);
        for (int i = 0; i < cap; ++i) bytes[i] = (uint8_t)MEM_B(i, mp);
        fprintf(stderr, "[submit-shape] span=%u bucket=%d count=%llu total=%llu lo=0x%08X "
                "first16=%02X%02X%02X%02X%02X%02X%02X%02X %02X%02X%02X%02X%02X%02X%02X%02X\n",
                (unsigned)span, bucket, (unsigned long long)b, (unsigned long long)t, submit_lo,
                bytes[0], bytes[1], bytes[2], bytes[3], bytes[4], bytes[5], bytes[6], bytes[7],
                bytes[8], bytes[9], bytes[10], bytes[11], bytes[12], bytes[13], bytes[14], bytes[15]);
        fflush(stderr);
    }
}

// ---------------------------------------------------------------------------
// Per-task synthetic-halt report (logging only; the counter exchange + the
// shared-histogram reset stay in the core). ALWAYS resets the file-local
// SETCOMBINE tracker at the end so the next task starts clean.
// ---------------------------------------------------------------------------
void dpc_diag_task_report(uint32_t iters, uint32_t data_size, uint32_t r17,
                          uint32_t cmd_w0, uint32_t cmd_w1,
                          uint32_t bytes, uint32_t cmds, uint32_t fs) {
    static std::atomic<uint64_t> s_n{0};
    uint64_t n = ++s_n;
    bool capped_no_fs = (iters > 16000 && fs == 0);
    static std::atomic<uint64_t> s_capped_count{0};
    bool log_capped = false;
    if (capped_no_fs) {
        uint64_t cn = ++s_capped_count;
        log_capped = (cn <= 8 || (cn & 4095) == 0);
    }
    const bool target_task_outer = (n == 256 || n == 384 || n == 512 || n == 640 ||
                                    n == 768 || n == 896 || n == 290 || n == 320 || n == 350);
    if (log_dpc()) {
        const uint32_t tri_count = g_task_gfx_op_count[0x05] + g_task_gfx_op_count[0x07];
        const uint32_t texrect_count = g_task_gfx_op_count[0xE4] + g_task_gfx_op_count[0xE5];
        fprintf(stderr, "[task-brief] #%llu tri=%u texrect=%u fs=%u iters=%u\n",
            (unsigned long long)n, tri_count, texrect_count, fs, iters);
    }
    if (log_dpc() && (n <= 16 || (n & 255) == 0 || target_task_outer || log_capped)) {
        fprintf(stderr,
            "[task] #%llu iters=%u data_size=%u r17=0x%X cmd=[%08X %08X] rdp_bytes=%u cmds=%u fullsyncs=%u%s\n",
            (unsigned long long)n, iters, data_size, r17, cmd_w0, cmd_w1, bytes, cmds, fs,
            capped_no_fs ? " CAPPED-NO-FS" : "");
        {
            uint32_t copy[256];
            for (int i = 0; i < 256; i++) copy[i] = g_task_gfx_op_count[i];
            for (int slot = 0; slot < 8; slot++) {
                int max_idx = 0;
                for (int i = 0; i < 256; i++) if (copy[i] > copy[max_idx]) max_idx = i;
                if (copy[max_idx] == 0) break;
                fprintf(stderr, "  [task#%llu gfx] op 0x%02X = %u\n",
                    (unsigned long long)n, (unsigned)max_idx, copy[max_idx]);
                copy[max_idx] = 0;
            }
            static bool s_first_capped_dumped = false;
            static bool s_first_substantial_dumped = false;
            const bool substantial = (iters > 1000 && !s_first_substantial_dumped);
            if (substantial) s_first_substantial_dumped = true;
            const bool target_task = (n == 512 || n == 768 || n == 256 || n == 384 ||
                                      n == 640 || n == 896 || n == 290 || n == 320 || n == 350);
            const bool dump_full = (n <= 4) || substantial || target_task ||
                                   (capped_no_fs && !s_first_capped_dumped);
            if (dump_full) {
                if (capped_no_fs) s_first_capped_dumped = true;
                fprintf(stderr, "  [task#%llu gfx-FULL] all non-zero ops:\n", (unsigned long long)n);
                for (int i = 0; i < 256; i++)
                    if (g_task_gfx_op_count[i] > 0)
                        fprintf(stderr, "    op 0x%02X = %u\n", (unsigned)i, g_task_gfx_op_count[i]);
                fprintf(stderr, "  [task#%llu setcombine] total=%u uniq=%u overflow=%u\n",
                    (unsigned long long)n, s_task_combine_total, s_task_combine_uniq, s_task_combine_overflow);
                for (uint32_t i = 0; i < s_task_combine_uniq; i++)
                    fprintf(stderr, "    mux #%u w0=0x%08X w1=0x%08X count=%u\n",
                        i, s_task_combine[i].w0, s_task_combine[i].w1, s_task_combine[i].count);
                fprintf(stderr, "  [task#%llu movemem-idx]\n", (unsigned long long)n);
                for (int i = 0; i < 256; i++) {
                    if (g_task_movemem_idx_count[i] > 0) {
                        const char *name = "?";
                        switch (i) {
                            case 0x80: name = "VIEWPORT"; break; case 0x82: name = "LOOKAT_Y"; break;
                            case 0x84: name = "LOOKAT_X"; break; case 0x86: name = "L0"; break;
                            case 0x88: name = "L1"; break; case 0x8A: name = "L2"; break;
                            case 0x8C: name = "L3"; break; case 0x8E: name = "L4"; break;
                            case 0x90: name = "L5"; break; case 0x92: name = "L6"; break;
                            case 0x94: name = "L7"; break; case 0x96: name = "TXTATT"; break;
                            case 0x98: name = "MTX_2"; break; case 0x9A: name = "MTX_3"; break;
                            case 0x9C: name = "MTX_4"; break; case 0x9E: name = "MTX_1"; break;
                        }
                        fprintf(stderr, "    idx 0x%02X (%s) = %u\n", (unsigned)i, name, g_task_movemem_idx_count[i]);
                    }
                }
            }
            fflush(stderr);
        }
        fflush(stderr);
        // For cap-without-fullSync tasks, dump the top-8 RDP opcodes — if one
        // opcode dominates, that's the stuck command (g_task_op_count is reset
        // by the core right after this report).
        if (log_capped) {
            uint32_t copy[64];
            for (int i = 0; i < 64; i++) copy[i] = g_task_op_count[i];
            for (int slot = 0; slot < 8; slot++) {
                int max_idx = 0;
                for (int i = 0; i < 64; i++) if (copy[i] > copy[max_idx]) max_idx = i;
                if (copy[max_idx] == 0) break;
                fprintf(stderr, "  [task#%llu hist] op 0x%02X = %u\n",
                    (unsigned long long)n, (unsigned)max_idx, copy[max_idx]);
                copy[max_idx] = 0;
            }
            fflush(stderr);
        }
    }
    // Always reset the per-task SETCOMBINE tracker for the next task.
    for (int i = 0; i < 16; i++) s_task_combine[i] = { 0, 0, 0 };
    s_task_combine_uniq = 0;
    s_task_combine_total = 0;
    s_task_combine_overflow = 0;
}
