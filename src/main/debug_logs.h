// Centralized debug-log gates for the Rogue Squadron recompile.
//
// The project accumulated many fprintf/printf trace points during boot
// debugging — thread setup, GFX dispatch, framebuffer routing, RDP
// commands. Most of them are silent on a healthy run but still emit
// hundreds of lines per second when active, drowning out the few
// messages you actually want to see.
//
// Each category below is read once on first call (cached in a function-
// local static), so check overhead is a single load per call site after
// startup. Set the env var to "1" (or any non-zero value) to enable.
// Errors and crashes (SIGABRT/SEH/etc.) are always printed unconditionally;
// don't gate those.
//
// Usage:
//   if (recomp::dbg::log_dpc()) {
//       fprintf(stderr, "[dpc-pak] %s w0=0x%08X\n", name, w0);
//   }
//
// To enable everything quickly:
//   set ROGUESQ_LOG_ALL=1   (Windows)
//   export ROGUESQ_LOG_ALL=1   (POSIX)

#pragma once

#include <cstdlib>
#include <cstring>

namespace recomp::dbg {

// Cached env-var read. Returns true iff the variable is set to a non-zero
// non-"false"/"no" value, OR the catch-all ROGUESQ_LOG_ALL is enabled.
inline bool env_flag(const char *name) {
    const char *all = std::getenv("ROGUESQ_LOG_ALL");
    if (all && *all && *all != '0') return true;
    const char *e = std::getenv(name);
    if (!e || !*e) return false;
    if (*e == '0') return false;
    if (std::strcmp(e, "false") == 0) return false;
    if (std::strcmp(e, "no") == 0) return false;
    return true;
}

// Legacy LLE DPC trace gate. The dpc_bridge.cpp pretty-printer
// (`[dpc-pak] ...` etc.) was retired 2026-05-09 alongside the LLE pipeline;
// the gate is preserved for any future LLE bring-up but currently has no
// callers. ROGUESQ_LOG_DPC=1.
inline bool log_dpc() {
    static const bool v = env_flag("ROGUESQ_LOG_DPC");
    return v;
}

// rt64 setCombine / setOtherMode traces (`[trace] setCombine #N ...`,
// `[trace] setOtherMode #N ...`). One line per state change. Useful when
// tracking which combiner/mode is active at a specific draw.
// ROGUESQ_LOG_RDP_STATE=1.
inline bool log_rdp_state() {
    static const bool v = env_flag("ROGUESQ_LOG_RDP_STATE");
    return v;
}

// VI present-queue traces (`[trace] PresentQueue::frame ...`,
// `[trace] RT64::Present #N ...`, `[trace] Present::lookup ...`,
// `[trace] PresentQ::regfb ...`, `[trace] fbReg #N ...`). Per-frame
// noise. Useful when investigating framebuffer routing / VI origin.
// ROGUESQ_LOG_PRESENT=1.
inline bool log_present() {
    static const bool v = env_flag("ROGUESQ_LOG_PRESENT");
    return v;
}

// SP task dispatch (`[sp] osSpTaskStartGo #N kind=GFX ...`,
// `[trace] DISPATCH slot=N ...`). One line per submitted GFX task.
// Useful when tracking task scheduling rate. ROGUESQ_LOG_SP_TASKS=1.
inline bool log_sp_tasks() {
    static const bool v = env_flag("ROGUESQ_LOG_SP_TASKS");
    return v;
}

// libultra VI shims: osViSetMode / osViSwapBuffer / osViSetXScale / osViSetYScale / osViBlack.
// ROGUESQ_LOG_VI=1.
inline bool log_vi() {
    static const bool v = env_flag("ROGUESQ_LOG_VI");
    return v;
}

// Thread lifecycle shims: osDestroyThread victim + queue check. ROGUESQ_LOG_THREADS=1.
inline bool log_threads() {
    static const bool v = env_flag("ROGUESQ_LOG_THREADS");
    return v;
}

// Behaviour knobs, read at the call site. Unlike env_flag, ROGUESQ_LOG_ALL does not turn these on.
inline const char* env_str(const char* name) { const char* v = std::getenv(name); return (v && *v) ? v : nullptr; }
inline bool env_on(const char* name, bool def = false) { const char* v = env_str(name); return v ? (*v != '0') : def; }
inline int env_int(const char* name, int def = 0) { const char* v = env_str(name); return v ? std::atoi(v) : def; }
inline unsigned env_u32(const char* name) { const char* v = env_str(name); return v ? (unsigned)std::strtoul(v, nullptr, 0) : 0u; }

} // namespace recomp::dbg
