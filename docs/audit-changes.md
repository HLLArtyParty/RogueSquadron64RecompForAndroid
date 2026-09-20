# Fork-modification inventory

A categorised snapshot of what this project adds on top of `lib/rt64`,
`lib/N64ModernRuntime`, and `src/`. Use it to tell integration glue from
defensive guards from debug cruft when deciding what a change is for.

Snapshot taken 2026-05; the funcs_*.c hand-edits it lists have since moved to
the regen-safe `patches/` build (see [AGENTS.md](../AGENTS.md) "Patches build").

## Categories

| Code | Meaning |
|---|---|
| **A** | Integration glue or necessary support — Factor 5 ucode handlers, DPC bridge, ROM-hash check, gamepad init, overlay loading. |
| **B** | Defensive guard over a bug whose root cause is unproven. Works empirically; each is a deferred investigation. |
| **C** | Pure debug instrumentation — log lines, env-gated traces, dump probes, watchdogs. Mostly removable or env-gated. |
| **D** | Forgotten / orphaned / superseded — disabled hypotheses, dead `if(false)` blocks, whitespace tweaks. |

## `lib/rt64` (fork at MikeSemicolonD/rt64)

### Cat A — Factor 5 ucode support

- `src/gbi/rt64_gbi_f3dfactor5.cpp` + `.h` — Factor 5 GBI backend. Custom opcodes (0x80, 0x02, 0xB0, 0xB2, 0xB4, 0xB5, 0xBF, 0xFF) need handlers absent from stock F3DEX; without it the parser misreads the stream as F3DEX and crashes.
- Bounds checks in `rt64_gbi_f3d.cpp` / `_f3dex.cpp` (G_DL / G_VTX) so drift into garbage doesn't AV.
- `include/rt64_extended_gbi.h` — `GBIUCode::F3DFACTOR5` enum + segment registration; CMakeLists registration.

### Cat B — Defensive guards

| Location | Guard | Unknown bug |
|---|---|---|
| `rt64_rdp.cpp` setColorImage/setDepthImage | Reject addr < 0x100000 or ≥ 0x800000; reject width > 1024 | Why the LLE pipeline sees CIMGs with garbage payloads at all. |
| `rt64_rdp.cpp` particle alpha rewrite | Detects cinematic-particle combiner with alpha-D=ZERO, rewrites to TEXEL0_ALPHA (`ROGUESQ_PARTICLE_FIX`) | Why that combiner+blender combo yields zero alpha. |
| `rt64_rdp.cpp` loadTile/loadBlock | RDRAM bounds checks | Why tile loads sometimes have wild source addresses. |
| `rt64_present_queue.cpp` | VI-follow override modes (mode 3 = pick freshest by timestamp) | Whether the Factor 5 cinematic violates RT64's VI heuristic assumptions. |
| `rt64_state.cpp` | Combiner-stack bounds check during late cinematic | Why the cinematic workload sometimes overflows the combiner stack. |
| `rt64_framebuffer_manager.cpp` | Skip zero-width / pixelSize==0 (G_IM_SIZ_4b) framebuffers | Factor 5 LLE registers such framebuffers during cinematic. |
| `rt64_native_target.cpp` | Null guards on D3D12 resources + 64-bit address arithmetic | Upstream feeding bad pointers; source unidentified. |
| `rt64_interpreter.cpp` | Iteration cap + drift detection on DL parse | Why the DL drifts into ASCII / pixel data. |
| Shade-fix (`ROGUESQ_SHADE_FIX`) | Rewrites SHADE values ("black model top") | Recompiled RSP produces SHADE with zeros at top vertices — RSP-translation bug or SHADE-compute mismatch. |

### Cat C / D

- Env-gated traces (`ROGUESQ_LOG_RDP_STATE` / `LOG_TEXBYTES` / `LOG_PIPELINE` / …) across `rt64_rdp`, `rt64_framebuffer`, `rt64_workload_queue`, `rt64_buffer_uploader`, `rt64_present_queue`.
- Per-mux color-key diagnostics in `RasterPS.hlsl` / `RasterVS.hlsl` / `VideoInterfacePS.hlsl`; env-gated thread-naming logs.
- Cat D: `if(false) fprintf(...)` dead prints in `rt64_gbi_*`; `#if 0` shader blocks.

## `lib/N64ModernRuntime` (fork at MikeSemicolonD/N64ModernRuntime)

### Cat A — Necessary runtime extension

- DPC bridge declarations in `librecomp/include/librecomp/rsp.hpp` (`g_rsp_dpc_start/end`, `rsp_dpc_submit`, `RSP_DPC_*`).
- RDP range submission API in `ultramodern/.../events.hpp` and `renderer_context.hpp` (`submit_rdp_range`, `submit_rdp_range_batch`, virtual `send_rdp_range`).
- Thread context magic sentinel in `ultramodern.hpp` + validation in `threads.cpp` (SEH-wrapped magic-load + VirtualQuery page-state check; detects corrupt OSThread context pointers).
- SEH exception propagation on Windows threads (`recomp.cpp`); earlier catch-and-exit lost minidumps.
- libultra stubs: `osViGetCurrentField`, pak/PFS no-pak, eep, cont rumble.
- VI mode deep-copy in `events.cpp` (Factor 5 reuses the OSViMode pointer); dummy VI mode init.

### Cat B — Defensive guards

| Location | Guard | Unknown bug |
|---|---|---|
| `rsp.hpp` DMA helpers | Clamp dram_addr to RDRAM bounds; skip IMEM-bit DMAs | Why the graphics ucode issues DMAs to dram_addr ≥ 0x800000 early in boot. |
| `dp.cpp` osDpGetCounters | 64-bit VA safety on the buffer pointer | Why uint32_t truncation AVs here. |
| `overlays.cpp` | Inverted-bounds guard on overlay section iteration | When/why overlay loads fail to match. |
| `overlays.cpp` get_function(0) | Returns a stub thunk + caller log instead of assert+exit | Why MIPS functions call `get_function(0)`; tail-return $ra leak suspected. |
| `ultra_translation.cpp` osYieldThread | `std::this_thread::yield()` instead of asserting | Cooperative-yield invariant not held. |
| `mesgqueue.cpp` do_send | Bails if `mq->msg` non-canonical or msgCount==0 | Why the message-queue struct becomes corrupt during cinematic. |

### Cat C / D

- mqdiag telemetry + CSV, mqfocus logging, `[trace]` lines (mostly `if(false)`), `ROGUESQ_LOG_*` env-gated traces, static counters, `[dma-7800]` DMA-trace logger.
- Cat D: cinematic-throttle scaffolding in `events.cpp` (disabled; superseded), whitespace tweak in `recomp.cpp`.

## `src/` and recompiled MIPS output

- `src/main/main.cpp` — Cat A: RSP microcode dispatch + audio task stub, Factor 5 boot ucode DMEM setup, SDL2 audio/window/gamepad, ROM hash check. Cat B: STL bounds-check abort suppression (`_CrtSetReportHook` returns 1). Cat C: F12 minidump hotkey, mqdiag watchdog, HWBP DR0 setup + VEH, DbgHelp symbolication, minidump writer, SEH + SIGABRT handlers.
- `src/rsp/dpc_bridge.cpp` — Cat A: DPC globals + FULL_SYNC detection, PIPESYNC filter (drops 0x27), task-end FULL_SYNC injection. Cat B: mid-frame FULL_SYNC, slot-dispatcher tracking, CIMG OOB detection, synthetic FULL_SYNC injection. Cat C: per-task RDP histogram + env-gated pretty-printers.
- `funcs_*.c` guards (KSEG0 pointer validation, `0xFFFFFFFF`→0 normalization, zero-init memory guard) are Cat B; the hand-editing they required is superseded by `patches/`. Debug macros in `funcs_27.c` were Cat C and are removable.

## Candidates to leave the fork

- The Factor 5 GBI backend, shade-fix / particle-alpha-fix / VI-follow logic — could live in our repo if RT64 grew a "register a custom GBI from outside" extension API.
- Overlay auto-load on PI DMA — Rogue-Squadron-specific policy; belongs in our repo as a runtime hook.
- Thread context magic — a defense any recomp wants; plausibly upstreamable.
- `if(false)` dead-code traces and cinematic-throttle scaffolding — deletable.
