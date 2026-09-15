// Host entry points for the [[patches.hook]] entries in rogue_squadron.toml. Hook code is
// generated MIPS C with no host headers, so anything it needs from the host is extern "C" here.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <atomic>
#include <thread>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <dbghelp.h>
#include <timeapi.h>
#include <tlhelp32.h>
#pragma comment(lib, "dbghelp.lib")
#pragma comment(lib, "winmm.lib")
#endif

#include "recomp.h"
#include "debug_logs.h"

using recomp::dbg::env_str;
using recomp::dbg::env_on;
using recomp::dbg::env_int;

extern "C" int rs64_vi_driven(void);                                   // upstream_compat.cpp
extern "C" int rs64_fb_guards_mask(void);                              // upstream_compat.cpp
extern void print_stack_with_symbols(void** frames, unsigned short count);  // main.cpp
extern "C" volatile uint8_t* volatile g_recomp_rdram_for_wp_raw;       // main.cpp

// ---- Boot target ----

// Set by the present hook while the attract title is up; get_n64_input injects START while set.
extern "C" volatile int g_boot_pulse_start = 0;

// ROGUESQ_BOOT_TARGET=level:<id>[,craft]: level id (0..0x14) or -1; craft (0..8) or -1 via craft_out.
// Consumed by the runIdleFramesAndLoadSaveData epilogue hook.
extern "C" int rs64_boot_target_level(int* craft_out) {
    static int s_lvl = -2, s_craft = -1;
    if (s_lvl == -2) {
        s_lvl = -1;
        const char* v = env_str("ROGUESQ_BOOT_TARGET");
        if (v && strncmp(v, "level", 5) == 0) {
            const char* c = strchr(v, ':');
            int n = (c && c[1]) ? std::atoi(c + 1) : 0;
            if (n >= 0 && n <= 0x14) s_lvl = n;
            const char* comma = strchr(v, ',');
            if (comma && comma[1]) { int cc = std::atoi(comma + 1); if (cc >= 0 && cc <= 8) s_craft = cc; }
        }
    }
    if (craft_out) *craft_out = s_craft;
    return s_lvl;
}

// ---- State published for the GBI ----

extern "C" volatile int g_current_scene = -1;      // menuOverlayInit action id; 9 = attribution
extern "C" volatile unsigned g_op_bf_count = 0;    // bumped by the F5 GBI per explosion-bloom tri

// ROGUESQ_LOG_HOOKS=1: stderr for hook code.
extern "C" void rs64_dbg_log4(const char* tag, unsigned a, unsigned b, unsigned c, unsigned d) {
    static const bool on = env_on("ROGUESQ_LOG_HOOKS");
    if (!on) return;
#ifdef _WIN32
    static uint64_t s_t0 = GetTickCount64();
    unsigned ms = (unsigned)(GetTickCount64() - s_t0);
#else
    unsigned ms = 0;
#endif
    fprintf(stderr, "[hook t=%ums] %s a=0x%08X b=0x%08X c=0x%08X d=0x%08X\n", ms, tag ? tag : "?", a, b, c, d);
    fflush(stderr);
}

// ---- Cinematic watchdog ----

#ifdef _WIN32
static std::atomic<DWORD> g_cine_tid{0};
static std::atomic<uint64_t> g_cine_iter{0};
static std::atomic<uint64_t> g_cine_last_ms{0};

static void rs64_cine_start_watchdog_thread(void);
static void rs64_dump_all_thread_stacks(DWORD game_tid);

extern "C" unsigned long long rs64_cine_iter_get(void) { return g_cine_iter.load(std::memory_order_relaxed); }

// Called from the cinematic loop-top hook.
extern "C" void rs64_cine_iter_tick(unsigned iter) {
    g_cine_tid.store(GetCurrentThreadId(), std::memory_order_relaxed);
    g_cine_iter.store(iter, std::memory_order_relaxed);
    g_cine_last_ms.store(GetTickCount64(), std::memory_order_relaxed);
    rs64_cine_start_watchdog_thread();
}

static void rs64_cine_progress_log(void) {
    static uint64_t s_last_log_ms = 0, s_last_log_iter = 0;
    uint64_t now = GetTickCount64();
    if (now - s_last_log_ms < 2000) return;
    uint64_t cur = g_cine_iter.load(std::memory_order_relaxed);
    uint64_t age_ms = g_cine_last_ms.load() ? now - g_cine_last_ms.load() : 0;
    fprintf(stderr, "[cine-progress] iter=%llu delta=%llu in %llums (idle=%llums)\n",
            (unsigned long long)cur, (unsigned long long)(cur - s_last_log_iter),
            (unsigned long long)(now - s_last_log_ms), (unsigned long long)age_ms);
    fflush(stderr);
    s_last_log_ms = now;
    s_last_log_iter = cur;
}

// Suspend one thread, walk its stack, print it.
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

// Every thread except the caller, game thread first.
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

// One-shot: cinematic loop idle for 2 s -> dump every thread's stack.
static void rs64_cine_dump_if_stuck(void) {
    static std::atomic<bool> s_dumped{false};
    if (s_dumped.load(std::memory_order_relaxed)) return;
    uint64_t last = g_cine_last_ms.load(std::memory_order_relaxed);
    if (last == 0) return;
    uint64_t now = GetTickCount64();
    if (now - last < 2000) return;
    DWORD tid = g_cine_tid.load(std::memory_order_relaxed);
    if (tid == 0) return;
    bool expected = false;
    if (!s_dumped.compare_exchange_strong(expected, true)) return;
    fprintf(stderr, "[cine-watchdog] freeze detected: tid=%lu iter=%llu idle=%llums\n",
            tid, (unsigned long long)g_cine_iter.load(), (unsigned long long)(now - last));
    fflush(stderr);
    rs64_dump_all_thread_stacks(tid);
}

static void rs64_write_rdram_be(const uint8_t* rd, const char* path, const char* why) {
    FILE* f = fopen(path, "wb");
    if (f) {
        static uint8_t buf[0x10000];
        for (uint32_t base = 0; base < 0x800000u; base += sizeof buf) {
            for (uint32_t i = 0; i < sizeof buf; ++i) buf[i] = rd[(base + i) ^ 3];
            fwrite(buf, 1, sizeof buf, f);
        }
        fclose(f);
    }
    fprintf(stderr, "[rdram-dump] %s -> %s (%s)\n", why, path, f ? "ok" : "OPEN FAILED");
    fflush(stderr);
}

// Own thread: the gfx thread can be parked on the same mutex chain that hangs the game thread.
// ROGUESQ_DUMP_RDRAM_ON_CINE_STALL=<ms>: cinematic frame counter (0x8013889C) unchanged that long ->
// RDRAM to ROGUESQ_DUMP_RDRAM_PATH (default dumps/rdram_cine_stall.bin).
// ROGUESQ_DUMP_STACKS_ON_DEMO_STALL=<ms>: demo cursor (0x80109AE0) unchanged that long -> all stacks.
static void rs64_cine_start_watchdog_thread(void) {
    static std::atomic<bool> s_started{false};
    if (s_started.exchange(true, std::memory_order_relaxed)) return;
    std::thread([]() {
        const uint64_t stall_ms = (uint64_t)std::atoll(env_str("ROGUESQ_DUMP_RDRAM_ON_CINE_STALL") ? env_str("ROGUESQ_DUMP_RDRAM_ON_CINE_STALL") : "0");
        const uint64_t demo_ms = (uint64_t)std::atoll(env_str("ROGUESQ_DUMP_STACKS_ON_DEMO_STALL") ? env_str("ROGUESQ_DUMP_STACKS_ON_DEMO_STALL") : "0");
        uint32_t last_frame = 0; uint64_t last_frame_change = 0; bool frame_dumped = false;
        uint32_t last_demo = 0xFFFFFFFFu; uint64_t last_demo_change = 0; bool demo_dumped = false;
        for (;;) {
            ::Sleep(500);
            rs64_cine_progress_log();
            rs64_cine_dump_if_stuck();
            const uint8_t* rd = (const uint8_t*)g_recomp_rdram_for_wp_raw;
            if (!rd) continue;
            const uint64_t now = GetTickCount64();
            if (stall_ms && !frame_dumped) {
                uint32_t frame = *reinterpret_cast<const uint32_t*>(rd + 0x13889C);
                if (frame != last_frame) { last_frame = frame; last_frame_change = now; }
                else if (frame > 0 && last_frame_change && now - last_frame_change >= stall_ms) {
                    frame_dumped = true;
                    const char* pe = env_str("ROGUESQ_DUMP_RDRAM_PATH");
                    char why[96];
                    snprintf(why, sizeof why, "cinematic frame counter stuck at %u for %llu ms", frame, (unsigned long long)(now - last_frame_change));
                    rs64_write_rdram_be(rd, pe ? pe : "dumps/rdram_cine_stall.bin", why);
                }
            }
            if (demo_ms && !demo_dumped) {
                uint32_t ds = *reinterpret_cast<const uint32_t*>(rd + 0x109AE0);
                if (ds != last_demo) { last_demo = ds; last_demo_change = now; }
                else if (ds != 0 && ds != 0xFFFFFFFFu && last_demo_change && now - last_demo_change >= demo_ms) {
                    demo_dumped = true;
                    fprintf(stderr, "[demo-stall] demoStep stuck at %u for %llu ms; dumping all thread stacks\n",
                            ds, (unsigned long long)(now - last_demo_change));
                    fflush(stderr);
                    rs64_dump_all_thread_stacks(g_cine_tid.load(std::memory_order_relaxed));
                }
            }
        }
    }).detach();
}
#else
extern "C" void rs64_cine_iter_tick(unsigned) {}
extern "C" unsigned long long rs64_cine_iter_get(void) { return 0; }
#endif

// ---- Pacing ----

// Every 16 cinematic iterations: yield and flush. A kernel call is what unsticks the loop.
extern "C" void rs64_cine_yield(void) {
    static int s_count = 0;
    if ((++s_count & 0xF) == 0) {
#ifdef _WIN32
        ::SwitchToThread();
#else
        std::this_thread::yield();
#endif
        std::fflush(stderr);
    }
}

// ROGUESQ_ATTRIBUTION_SLEEP_MS=N per attribution iteration (default 0: VI-paced).
extern "C" void rs64_idle_pace(void) {
#ifdef _WIN32
    static const int s_ms = [](){ int v = env_int("ROGUESQ_ATTRIBUTION_SLEEP_MS"); return v < 0 ? 0 : v > 5000 ? 5000 : v; }();
    if (s_ms) ::Sleep((DWORD)s_ms);
#endif
}

// ROGUESQ_CINE_TARGET_FPS=N paces cinematicLoopBody (default 0 when VI-driven, else 30).
// ROGUESQ_CINE_HOLD_ITER=N [+ ROGUESQ_CINE_HOLD_MS, default 15000]: park once at iteration N.
extern "C" void rs64_cine_pace(void) {
#ifdef _WIN32
    static const long long s_hold_iter = env_str("ROGUESQ_CINE_HOLD_ITER") ? std::atoll(env_str("ROGUESQ_CINE_HOLD_ITER")) : -1;
    static const uint32_t s_hold_ms = (uint32_t)env_int("ROGUESQ_CINE_HOLD_MS", 15000);
    static bool s_held = false;
    if (s_hold_iter >= 0 && !s_held && (long long)rs64_cine_iter_get() >= s_hold_iter) {
        s_held = true;
        fprintf(stderr, "[cine-hold] freezing at iter %llu for %u ms\n", rs64_cine_iter_get(), s_hold_ms);
        fflush(stderr);
        uint64_t end = GetTickCount64() + s_hold_ms;
        while (GetTickCount64() < end) ::Sleep(100);
        fprintf(stderr, "[cine-hold] released\n"); fflush(stderr);
    }
    static const int s_target_fps = [](){
        int v = env_int("ROGUESQ_CINE_TARGET_FPS", rs64_vi_driven() ? 0 : 30);
        v = v < 0 ? 0 : v > 240 ? 240 : v;
        if (v) timeBeginPeriod(1);   // so Sleep(33) is not quantized to 15.6 ms
        return v;
    }();
    if (s_target_fps == 0) return;
    const uint32_t interval_ms = 1000u / (uint32_t)s_target_fps;
    static uint64_t s_last_ms = 0;
    uint64_t now = GetTickCount64();
    if (s_last_ms == 0) { s_last_ms = now; return; }
    uint64_t elapsed = now - s_last_ms;
    if (elapsed < interval_ms) ::Sleep((DWORD)(interval_ms - elapsed));
    s_last_ms = GetTickCount64();
#endif
}

extern "C" void rs64_sleep_ms(unsigned ms) {
#ifdef _WIN32
    ::Sleep((DWORD)ms);
#endif
}

// Bumped once per host VI retrace (main.cpp).
extern "C" volatile unsigned g_vi_tick = 0;

// Block until the next VI tick (cap ~40 ms). Paces the attribution loop to real time so the
// VI-driven producer thread that lays out the glyphs gets to run.
extern "C" void rs64_attrib_wait_vi(void) {
#ifdef _WIN32
    unsigned start = g_vi_tick;
    for (int i = 0; i < 40 && g_vi_tick == start; ++i) ::Sleep(1);
#endif
}

// ---- Display-list walkers ----

// ROGUESQ_DUMP_FRAME_DL=N: dump the opcode stream of gfx tasks N..N+15 (per-command detail on N;
// TEXRECT count and bbox for all). ROGUESQ_DUMP_TEXTURES=1 also writes each SETTIMG source to
// dumps/tex/. Read-only.
extern "C" void rs64_dump_frame_dl(uint8_t* rdram, uint32_t dl_phys) {
    static const int s_target = env_int("ROGUESQ_DUMP_FRAME_DL", -1);
    static const bool s_dump_tex = env_on("ROGUESQ_DUMP_TEXTURES");
    static int s_call = 0;
    const int call = ++s_call;
    if (s_target < 0 || call < s_target || call >= s_target + 16 || dl_phys == 0) return;
    const bool full_detail = (call == s_target);

    uint32_t a = (dl_phys & 0x3FFFFFFu) | 0x80000000u;
    uint32_t dstack[16]; int dsp = 0; int djmp = 256;
    int tr_n = 0; int tr_minx = 99999, tr_miny = 99999, tr_maxx = -99999, tr_maxy = -99999;
    uint32_t last_cimg = 0;
    // The menu DL re-enters the same tile sub-DLs; skip any command address seen before.
    static uint8_t s_vis[0x800000 >> 3];
    memset(s_vis, 0, sizeof(s_vis));
    fprintf(stderr, "[frame-dl-dump call=%d] start=0x%08X\n", call, a);
    for (int i = 0; i < 16384; ++i) {
        if ((a & 0x3FFFFFFu) + 8u > 0x800000u) break;
        uint32_t vidx = (a & 0x3FFFFFFu) >> 3;
        if (s_vis[vidx]) break;
        s_vis[vidx] = 1;
        uint32_t w0 = (uint32_t)MEM_W(0, (gpr)(int32_t)a);
        uint32_t w1 = (uint32_t)MEM_W(4, (gpr)(int32_t)a);
        uint8_t op = (uint8_t)(w0 >> 24);
        // 16-byte F5 commands (0xB4 as the GBI consumes it; 0xBE payload is not a command).
        if (op == 0xB4 || op == 0xBE) { a += 16u; continue; }
        // 0xB5 chunk link: follow w1 (0 = plain no-op).
        if (op == 0xB5 && (w1 & 0x00FFFFFFu) != 0u) { a = (w1 & 0x00FFFFF8u) | 0x80000000u; continue; }
        if (op == 0xE4 || op == 0xE5) {   // TEXRECT, 24 bytes
            int lrx = ((w0 >> 12) & 0xFFF), lry = (w0 & 0xFFF);
            int ulx = ((w1 >> 12) & 0xFFF), uly = (w1 & 0xFFF);
            int tile = (w1 >> 24) & 0x7;
            uint32_t t1w1 = (uint32_t)MEM_W(4, (gpr)(int32_t)(a + 8u));
            uint32_t t2w1 = (uint32_t)MEM_W(4, (gpr)(int32_t)(a + 16u));
            if (full_detail)
                fprintf(stderr, "  %08X: TEXRECT tile=%d ul=(%d,%d) lr=(%d,%d) px=(%.1f,%.1f)-(%.1f,%.1f) uls/t=0x%08X dsdx/y=0x%08X\n",
                        a, tile, ulx, uly, lrx, lry, ulx/4.0, uly/4.0, lrx/4.0, lry/4.0, t1w1, t2w1);
            ++tr_n;
            if (ulx/4 < tr_minx) tr_minx = ulx/4; if (uly/4 < tr_miny) tr_miny = uly/4;
            if (lrx/4 > tr_maxx) tr_maxx = lrx/4; if (lry/4 > tr_maxy) tr_maxy = lry/4;
            a += 24u; continue;
        }
        if (op == 0x03) { a += 24u; continue; }   // 8B cmd + 16B inline vertex
        if (op == 0xFD && s_dump_tex && full_detail) {
            uint32_t fmt = (w0 >> 21) & 0x7, siz = (w0 >> 19) & 0x3;
            uint32_t timg = w1 & 0x00FFFFFFu;
            char path[256];
            snprintf(path, sizeof(path), "dumps/tex/%06X_fmt%u_siz%u.bin", timg, fmt, siz);
            static int s_made = 0;
            if (!s_made) { s_made = 1; system("if not exist dumps\\tex mkdir dumps\\tex"); }
            FILE* tf = fopen(path, "wb");
            if (tf) {
                for (uint32_t k = 0; k < 0x2000u; ++k) { uint32_t off = timg + k; fputc(off < 0x800000u ? rdram[off ^ 3] : 0, tf); }
                fclose(tf);
            }
            fprintf(stderr, "  %08X: SETTIMG addr=0x%06X fmt=%u siz=%u -> %s\n", a, timg, fmt, siz, path);
        }
        if (op == 0xFF) last_cimg = w1;
        const char* tag = "";
        if (op == 0xFD) tag = " <-SETTIMG";
        else if (op == 0xF5) tag = " <-SETTILE";
        else if (op == 0xF3) tag = " <-LOADBLOCK";
        else if (op == 0xFF) tag = " <-SETCIMG";
        else if (op == 0xFC) tag = " <-SETCOMBINE";
        if (full_detail && (*tag || i < 64)) fprintf(stderr, "  %08X: %08X %08X op=%02X%s\n", a, w0, w1, op, tag);
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
    if (tr_n == 0) tr_minx = tr_miny = tr_maxx = tr_maxy = 0;
    fprintf(stderr, "[frame-dl-dump call=%d] TEXRECT count=%d bbox px=(%d,%d)-(%d,%d) cimg=0x%08X\n",
            call, tr_n, tr_minx, tr_miny, tr_maxx, tr_maxy, last_cimg);
    fflush(stderr);
}

// FB-guards bit 1 (default off): rewrite any G_SETCIMG whose 24-bit address falls outside the real
// framebuffer range to G_SPNOOP before RT64 parses the task. Real framebuffers all live at or above
// 0x804B7800; RDRAM ends at 0x80800000. Mutates the DL in RDRAM.
static constexpr uint32_t RS64_FB_MIN_ADDR = 0x80400000u;
static constexpr uint32_t RS64_FB_MAX_ADDR = 0x80800000u;

extern "C" void rs64_neutralize_matpool_cimg(uint8_t* rdram, uint32_t dl_phys) {
    rs64_dump_frame_dl(rdram, dl_phys);
    if (!(rs64_fb_guards_mask() & 1) || dl_phys == 0) return;

    uint32_t addr = (dl_phys & 0x3FFFFFFu) | 0x80000000u;
    uint32_t stack[16];
    int sp = 0;
    int jumps_left = 64;
    int cimg_seen = 0, cimg_killed = 0, cimg_passed = 0;
    static int s_logs = 0;
    static int s_summary_logs = 0;
    for (int i = 0; i < 4096; ++i) {
        if ((addr & 0x3FFFFFFu) + 8u > 0x800000u) break;
        uint32_t w0 = (uint32_t)MEM_W(0, (gpr)(int32_t)addr);
        uint32_t w1 = (uint32_t)MEM_W(4, (gpr)(int32_t)addr);
        uint8_t  op = (uint8_t)(w0 >> 24);
        if (op == 0xBEu) { addr += 16u; continue; }   // 16B state cmd; payload is not a command
        if (op == 0xFFu) {
            cimg_seen++;
            uint32_t fmt = (w0 >> 21) & 0x7u;
            uint32_t img = (w1 & 0xFFFFFFu) | 0x80000000u;   // RDP addresses are 24-bit; F5 leaves garbage above
            if (fmt <= 4u && w1 != 0u && (img < RS64_FB_MIN_ADDR || img >= RS64_FB_MAX_ADDR)) {
                MEM_W(0, (gpr)(int32_t)addr) = w0 & 0x00FFFFFFu;
                cimg_killed++;
                if (s_logs++ < 16) {
                    fprintf(stderr, "[cimg-neutralize] killed G_SETCIMG @0x%08X img=0x%08X w=%u fmt=%u (%s fb_range=[0x%08X,0x%08X))\n",
                            addr, img, (w0 & 0xFFFu) + 1u, fmt, img < RS64_FB_MIN_ADDR ? "below" : "above",
                            RS64_FB_MIN_ADDR, RS64_FB_MAX_ADDR);
                    fflush(stderr);
                }
            } else if (fmt <= 4u && w1 != 0u) {
                cimg_passed++;
            }
        }
        if (op == 0xB8u || op == 0xDFu) {
            if (sp > 0) { addr = stack[--sp]; continue; }
            break;
        }
        // F5 reuses 0x06; it is G_DL only when w0 & 0x00FEFFFF == 0 (matches op_06_strict_dl).
        bool is_dl = ((op == 0x06u) && ((w0 & 0x00FEFFFFu) == 0u)) || (op == 0xDEu);
        if (is_dl && jumps_left > 0) {
            uint8_t branch = (uint8_t)(w0 >> 16);
            uint32_t target = (w1 & 0x00FFFFF8u) | 0x80000000u;
            if (branch == 0 && sp < 16) stack[sp++] = addr + 8u;
            addr = target;
            jumps_left--;
            continue;
        }
        addr += 8u;
    }
    if (s_summary_logs++ < 32 || cimg_killed > 0) {
        fprintf(stderr, "[cimg-walker] dl=0x%08X visited cimg=%d killed=%d passed=%d jumps_used=%d stack_max=%d\n",
                (dl_phys & 0x3FFFFFFu) | 0x80000000u, cimg_seen, cimg_killed, cimg_passed, 64 - jumps_left, sp);
        fflush(stderr);
    }
}
