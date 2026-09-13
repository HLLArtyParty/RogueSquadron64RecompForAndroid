# AGENTS.md

Guidance for AI agents working on the Rogue Squadron 64 Recompiled project — a native port of *Star Wars: Rogue Squadron* (N64, USA v1.0) built with N64Recomp + RT64. The game boots, and content renders: attribution text, the textured Factor 5 / N64-logo cinematic, the main menu, and the attract demo all come up. The current frontier is display-list fidelity (texturing/UV bugs and per-frame DL desyncs), the MORT voice codec, and symbol renaming. See the [Status](README.md#status) table in the README for the authoritative what-works snapshot before starting anything — this file assumes it.

## Project layout

```
src/main/main.cpp                       Game registration + RSP/audio/input/gfx callbacks
src/main/register_overlays.cpp          Boot-time overlay registration (all 3 .ovl.* at once)
src/main/rt64_render_context.cpp        RT64 host context; voice-unstick watchdog
src/main/upstream_compat.cpp            Host shims (DMA, VI-driven loop plumbing)
src/rsp/dpc_bridge.cpp                  DPC_START/DPC_END bridge into RT64
src/rsp/dpc_bridge_diag.cpp             DL stream tracing/diagnostics
src/rsp/aspMain.cpp                     Audio RSP microcode stub
lib/N64ModernRuntime/                   Submodule — fork at MikeSemicolonD/N64ModernRuntime
  ├── librecomp/                        Recompiler runtime (overlay loading, get_function, SEH)
  └── ultramodern/                      libultra emulation (threads, mesgqueue, events)
lib/rt64/                               Submodule — fork at MikeSemicolonD/rt64
  └── src/gbi/rt64_gbi_f3dfactor5.cpp   Native Factor 5 GBI profile (the render core)
docs/                                   Project notes (GBI, DL spec, data structures, env vars)
tools/                                  PowerShell + Python diagnostic and validation helpers
build/                                  CMake out-of-source build dir
```

The recompiled MIPS code lives **outside** the project at `E:\Projects\N64Recomp\RecompiledFuncs\` — `funcs_*.c` plus `funcs.h` and `recomp_overlays.inl`. These are auto-generated; hand-instrumenting them with diagnostic `fprintf` probes is routine, but load-bearing logic belongs in the `patches/` build (see below), not here — regeneration silently discards inline edits.

The forked submodules under `lib/` carry intentional `if(false) fprintf(...)` debug-toggle cruft and game-specific defensive guards. **Do not propose stripping these** as cleanup; they are intentional. (The KSEG0 pointer guards are a separate, tracked retirement — see Open work.)

## Build & run

```powershell
# Build (Debug is the tested configuration)
cmake --build build --config Debug --target RogueSquadron64Recomp

# Run with stdout/stderr capture (PowerShell — bash redirects don't flush before SIGTERM)
$job = Start-Job -ScriptBlock {
    Set-Location E:\Projects\RogueSquadron64Recomp\build\Debug
    & ".\RogueSquadron64Recomp.exe" *>&1
}
Start-Sleep -Seconds 30
Stop-Job $job
Receive-Job $job -Keep | Out-File -Encoding utf8 ..\probe_run.txt
Remove-Job $job
```

Ignore the `lld-link : warning : found both wmain and main; using latter` — benign (SDL2main provides wmain, our main wins).

ROM lives at `build/Debug/rogue_squadron.z64` (USA v1.0, xxHash3-64 = `0x6B66A44153594DEA`).

For a headless run that drives past the title without a controller, use `ROGUESQ_FAKE_CONTROLLER=1` and `ROGUESQ_AUTO_START=<ms>`.

## Environment variables

All trace categories are off by default. The full catalog is in
[docs/debug-trace-env-vars.md](docs/debug-trace-env-vars.md) — **read it before
adding a new `fprintf` or asking the user to enable logs.** The switches you are
most likely to need:

| Env var | Effect |
|---|---|
| `ROGUESQ_GFX_API=vulkan\|d3d12` | Force the graphics API (default auto) |
| `ROGUESQ_HLE_DEV_MODE=0\|1` | RT64 ImGui inspector on F1 (default on in Debug, off in Release) |
| `ROGUESQ_VI_DRIVEN_LOOP=0` | Old host-paced frame loop instead of the hardware VI protocol (default on). The VI-driven loop matches hardware message order and is the current stability baseline |
| `ROGUESQ_F5_NATIVE=0` | Parse F5 display lists without emitting geometry |
| `ROGUESQ_F5_CHUNK_BOUND=0` / `ROGUESQ_F5_B5_CHUNKEND=0` / `ROGUESQ_F5_DL_SKIP16=0` | Disable individual DL grammar rules (all default on) |
| `ROGUESQ_LLE_FORCE=1` | Run graphics through the recompiled RSP ucode instead of HLE (diagnostic only — see dead ends) |
| `ROGUESQ_FB_GUARDS=0` | Disable the host framebuffer-window guards for A/B against goldens |
| `ROGUESQ_BUFSTUCK_RESCUE=0` | Disable the video-buffer arbiter rescue hook |
| `ROGUESQ_VOICE_UNSTICK` | Host watchdog (in `rt64_render_context.cpp`) that clears the streamed-voice active byte a stuck cutscene waits on — the MORT codec never fires the completion itself. See the demo-freeze note under Architectural quirks |
| `ROGUESQ_NO_AUDIO_UCODE=1` | Silent audio stub instead of the MusyX synth |
| `ROGUESQ_DUMP_PCM=<path>` | Write the synth output to a 22050 Hz stereo WAV |
| `ROGUESQ_RENDER_SONG=<key>` | Force a specific song (0 = the N64-logo music) |
| `ROGUESQ_FAKE_CONTROLLER=1` / `ROGUESQ_AUTO_START=<ms>` | Headless runs: fake a controller, pulse START |
| `ROGUESQ_SUPPRESS_OOB_CIMG` | **Default OFF.** Drops F5 ucode SET_COLOR_IMAGE emissions at HIGH (≥ 0x800000) / LOW (< 0x100000) addresses. Reduces a memory spike but drops legitimate lowmem CIMGs too (regresses the 3D logo). Leak-experiment only |
| `ROGUESQ_LOG_ALL=1` | Every trace category |
| `ROGUESQ_LOG_GBI=1` / `ROGUESQ_LOG_GFX_TASK=1` | Per-handler GBI logs (high volume) / one line per graphics task |
| `ROGUESQ_LOG_MESG_TRACE=1` (+ `ROGUESQ_MESG_TRACE_FRAMES=lo-hi`) | Thread/message-order trace for `tools/validate/compare_mesg_trace.py` |
| `ROGUESQ_DUMP_FRAME_DL=N` / `ROGUESQ_DUMP_TEXTURES=1` | One-shot display-list and texture dumps |

With the inspector on, F1 toggles RT64's ImGui overlay, F3 toggles ViewRDRAM mode, F4 toggles texture replacement, and F12 writes a crash dump.

## Diagnostic artifacts

Files written to `logs/` and `dumps/crash-dumps/` during a run:

- `logs/stability/<tag>/run_N.log` — per-run stderr captured by `tools/run-stability.ps1`. Companion `summary.csv` classifies outcomes.
- `logs/stability/<tag>/memory.csv` — per-second WS / private / VM samples from `tools/measure-leak.ps1`.
- `mqdiag_NNN.txt` — per-queue counters dumped every 3s by the watchdog thread (`start_mqdiag_watchdog` in [src/main/main.cpp](src/main/main.cpp)). Each file is a point-in-time snapshot; diff two to see which queues are still moving.
- `dumps/crash-dumps/crash_YYYYMMDD_HHMMSS.dmp` — full-memory minidump from the SEH handler, the SIGABRT handler, the F12 hotkey, or external `tools/dump-game.ps1`.

## Tooling (under `tools/`)

### Crash / hang triage

1. **Crash**: a `crash_*.dmp` is written automatically by the handlers in `src/main/main.cpp`. Open in VS (File → Open → File → .dmp → Debug with Native Only).
2. **Hang**: from another shell, `pwsh tools/dump-game.ps1` captures the running process even when the window is "Not Responding". Don't use F12 if the message pump is starved — it won't fire.
3. **Triage threads**: `python tools/inspect-dump.py` lists threads in the most recent dump and tags ones running our exe code as `in_exe`. Those 2–4 are the only ones worth opening in VS; the rest are runtime workers parked in ntdll.

Note: the Windows debugger CLI (`cdb.exe`) is currently broken on this machine (`STATUS_DLL_INIT_FAILED`). Don't try to drive it from PowerShell — use VS interactively or the `minidump` Python package via `inspect-dump.py`.

### Stability / leak harness

- `tools/run-stability.ps1 -Runs N -Timeout S -Tag <label>` — N timed launches, per-run stderr, outcome classification by marker grep. `-EnvVars "A=1;B=1"` forwards debug vars. `-Runs 1` for data capture, `-Runs 3` for stability-rate.
- `tools/measure-leak.ps1 -Timeout S -Tag <label>` — single run, per-second WS/Private/VM to `memory.csv`.

### Validation harness (`tools/validate/`)

The primary correctness workflow — diff a live run against a Project64 golden rather than eyeballing screenshots:

- `capture_pj64_golden.ps1` / `capture_menu_rdram.ps1` — scripted PJ64 goldens.
- `f5_dl_walk.py` — walks a Factor 5 display list offline; `--json` for machine diff, `--tex` for texture/UV inspection. `f5_dl_ndc.py` adds NDC projection.
- `dl_diff.py` — layer-by-layer DL diff vs golden.
- `rdram_golden_diff.py` — RDRAM diff vs PJ64.
- `audio_cmd_walk.py` / `audio_diff.py` — MusyX voice-command walk and diff (perception-free audio validation).
- `compare_mesg_trace.py` / `symbolize_mesg_trace.py` — message-order trace comparison (pair with `ROGUESQ_LOG_MESG_TRACE`).
- `checkpoint.ps1` — orchestrates a capture + diff checkpoint.

### Renaming (`tools/rename/`) and cross-refs (`tools/rz/`)

- `tools/rename/` — the symbol-renaming pipeline. **Run `tools/rename/lint_toml_syms.py` after every rename batch** to catch symbol drift before a regen.
- `tools/rz/rzq.py <cmd> <sym|0xADDR>` — rizin queries against the symbolized `rogue_squadron64` ELF: `xrefs`, `callees`, `disasm`, `strrefs`, `funcs`, `raw`. `--overlay mission|menu|cinematic` selects which overlay sits at 0x800A5130; `--project <file>` caches the ~25s analysis. See [tools/rz/README.md](tools/rz/README.md).

## Patches build (overriding auto-generated functions)

**Do not hand-edit `E:/Projects/N64Recomp/RecompiledFuncs/funcs_*.c` for defensive guards or game-logic overrides.** Those edits are regeneration-hostile and made the codebase brittle for months. Use the `patches/` build — the Zelda64Recompiled pattern, with one difference:

**We use `mips64-elf-gcc`, not `clang -target mips`.** The official LLVM Windows installer ships without the MIPS backend (`clang -print-targets | findstr mips` returns nothing on 19/20/22-rc). Linux LLVM packages do include MIPS; on Linux just `apt install clang lld make` and override `MIPS_GCC` / `MIPS_LD`. On Windows, install the [n64-tools/gcc-toolchain-mips64](https://github.com/n64-tools/gcc-toolchain-mips64/releases) prebuilt MIPS GCC 12.2.0 — the CMake config expects `E:/mips-toolchain` (override with `-DMIPS_TOOLCHAIN_DIR=...`).

### Pipeline at a glance

```
patches/heap_guards.c           ← MIPS-side C (game pointers, externs)
  ↓ mips64-elf-gcc -mips2 -mabi=32 -nostdinc
patches/heap_guards.o
  ↓ mips64-elf-ld -T patches.ld -T syms.ld
patches/patches.elf
  ↓ N64Recomp.exe ../patches.toml  (single_file_output, strict_patch_mode)
RecompiledPatches/patches.c     ← host C with recomp_func_t signatures
  ↓ clang-cl
PatchesLib.lib
  ↓ linked FIRST in target_link_libraries (before RecompiledFuncs)
RogueSquadron64Recomp.exe       ← /FORCE:MULTIPLE picks our overrides at link
```

### Adding a new override

1. Write the function in `patches/somefile.c`, named the same as the game function (`func_80007D74`), annotated `RECOMP_PATCH`:
   ```c
   #define RECOMP_PATCH __attribute__((section(".recomp_patch")))
   RECOMP_PATCH void* func_80007D74(void) { /* ... */ }
   ```
2. Add game symbols the patch references to `patches/syms.ld`:
   ```ld
   func_8002221C  = 0x8002221C;
   heap_free_head = 0x801163B0;
   ```
   Look up addresses in `syms/rogue_squadron.syms.toml` (produced by `N64Recomp.exe rogue_squadron.toml --dump-context`).
3. Declare them `extern` in your patch C — the linker resolves the symbol; the C declaration only adds type info.
4. Build: `cmake --build build --config Debug --target PatchesLib` builds just the lib; the full build relinks everything.

### Constraints (gotchas)

- **`-nostdinc` means no libc headers.** Inline the typedefs you need:
  ```c
  typedef unsigned char uint8_t;
  typedef unsigned int  uint32_t;
  typedef unsigned long uintptr_t;   // mips32 ABI: long is 32-bit
  #define NULL ((void*)0)
  ```
- **No `printf` / `stderr`.** To call host code from a patch, declare a runtime stub at a fake `0x8FXXXXXX` address in `syms.ld` and provide the host impl in `src/main/`.
- **Signatures match the game, not the recomp.** Write the original MIPS-style signature (no `recomp_context*` arg); N64Recomp produces the host `void(uint8_t*, recomp_context*)` automatically.
- **Duplicate symbols are EXPECTED** when an override exists in both PatchesLib and RecompiledFuncs. We pass `/FORCE:MULTIPLE`; the linker takes PatchesLib's (it comes first). One warning per duplicate — fine.

### Toolchain quick-reference

| Component | Path |
|---|---|
| MIPS GCC | `E:/mips-toolchain/bin/mips64-elf-gcc.exe` |
| MIPS LD | `E:/mips-toolchain/bin/mips64-elf-ld.exe` |
| `make` | `mingw32-make` (any MinGW install) |
| N64Recomp (patches) | `build/Debug/N64Recomp.exe` — built from the submodule via `N64RecompCLI`; supports `--dump-context` + `func_reference_syms_file`. Use ONLY for the patches pipeline |
| N64Recomp (main regen) | `E:/Projects/N64Recomp/Debug/N64Recomp.exe` — the older binary; use for full main-tree regeneration (the newer one truncates output — see dead ends) |

See [patches/README.md](patches/README.md) for the full how-to.

## Architectural quirks worth knowing

### Overlays register at boot

librecomp's section table covers all three `.ovl.*` overlays (mission / menu / cinematic), which share `ram_addr 0x800A5130`. They are all registered at boot in [src/main/register_overlays.cpp](src/main/register_overlays.cpp) via `recomp::overlays::register_overlays` — the Zelda64Recomp pattern. The per-DMA `load_overlays` callback that earlier builds patched into librecomp is **non-canonical** and was removed. If runtime DMA-driven overlay switching ever proves necessary, the correct place is a thin wrapper inside our own `load_overlays`, not a librecomp modification.

### Factor 5 GBI — custom opcodes

This game uses a Factor 5-customized F3DEX-derived ucode. The RT64 profile `GBI_F3DFACTOR5` lives in [lib/rt64/src/gbi/rt64_gbi_f3dfactor5.cpp](lib/rt64/src/gbi/rt64_gbi_f3dfactor5.cpp) and inherits from `GBI_F3DEX`. The canonical opcode map (with observed w0/w1 patterns and disproven interpretations) is in [docs/factor5-gbi.md](docs/factor5-gbi.md); the DL grammar itself is in [docs/f5-model-dl-spec.md](docs/f5-model-dl-spec.md). Confirmed Factor 5-specific behaviors:

| Opcode | Standard meaning | Factor 5 behavior | Handler |
|-------:|------------------|-------------------|---------|
| `0xB5` | F3DEX `G_QUAD` | **Chunk/DL terminator** at chunk offset 0x100 (returns to parent DL) | `OP_B5_ENDDL` / `op_b5_next_chunk` |
| `0xE4` | F3DEX `G_TEXRECT` | **LLE format (16 bytes)**, not HLE 24-byte | `texrectLLE_guarded` |
| `0xE5` | F3DEX `G_TEXRECTFLIP` | LLE format | `texrectFlipLLE_guarded` |
| `0xFF` | `G_SETCIMG` | Frequently emitted with bogus payloads (w1=0, fmt>4, out-of-FB addresses); rejected by `setColorImage_filtered` | `setColorImage_filtered` |
| `0x80` | unused | Chunk metadata header (next-chunk pointer in 24-bit w0); walked as a no-op | `op80_unknown` |
| `0x02` | F3D `G_RDPHALF_2` | Constant payload `0x028001C0 / 0x01FF0000`; fixed setup, no-op | `op02_unknown` |

Chunks are contiguous 0x108-byte blocks; the interpreter walks chunk content linearly and the `0xB5` terminator at offset 0x100 returns control to the parent DL. Models render as CI4 textures at palette bank 15. **The current model texturing/UV bug is RT64-side** — the DL stream, UVs, wrap/mask, and texture data have been proven byte-identical to a PJ64 golden, so the defect is in RT64's rendering of that faithful stream, not in what we submit. Use `f5_dl_walk.py --tex` to compare.

### Audio — MusyX synth (working) vs MORT voice (not)

Rogue Squadron drives audio through Factor 5's **MusyX** engine, and **SFX and music work**: the CPU-side MusyX sequencer submits `M_AUDTASK`s, and a host-side synth path produces PCM that flows through `queue_samples` → SDL ([main.cpp](src/main/main.cpp)). The RSP synth microcode itself is stubbed (`aspMain` on MusyX task data hangs — no shared format with stock ucode), so the M_AUDTASK RSP call returns `RspExitReason::Broke` and the synthesis happens host-side instead. `ROGUESQ_NO_AUDIO_UCODE=1` reverts to a silent stub; `ROGUESQ_DUMP_PCM` / `ROGUESQ_RENDER_SONG` drive offline capture.

Subtitled **dialogue uses a separate codec, MORT** (per the [rerogue](https://github.com/dpethes/rerogue) PC-version RE), which is **not implemented**. This is the direct cause of the demo / FrontEnd cutscene freeze: the cutscene thread waits on a streamed-voice active byte that the MORT decode would clear, and it never does. `ROGUESQ_VOICE_UNSTICK` is a host watchdog in [rt64_render_context.cpp](src/main/rt64_render_context.cpp) that clears that byte so the cutscene advances. `tools/extract_speech_table.py` extracts the voiceId→text table; in-engine MORT decode is the missing piece (`tools/MORTDecoder.cpp` / `tools/mort_decode.py` are the RE scratch).

### Cooperative-scheduler queue plumbing

DP (`OS_EVENT_DP`) events arrive on a non-game thread → `enqueue_external_message_src` → drained on the next game-thread `osSendMesg`/`osRecvMesg`/`osJamMesg` via `dequeue_external_messages`. Queue 0x8011A408 (gate-thread DP queue, count=1) and 0x8011A7E8 (consumer) are the DP-pacing pair. Default `MessageQueueControl{}` has `requeue_dp = true`. The `mqdiag` instrumentation in [ultramodern/src/mesgqueue.cpp](lib/N64ModernRuntime/ultramodern/src/mesgqueue.cpp) tracks per-queue send/recv/external/delivered/blocked/lost/requeued counts; dump via `mqdiag_dump(path)`.

### Exception handling pipeline

- **C++ exceptions** (SEH `0xE06D7363`): the `__except` filter in [librecomp/src/recomp.cpp](lib/N64ModernRuntime/librecomp/src/recomp.cpp) **must** let these propagate (`EXCEPTION_CONTINUE_SEARCH`). Catching them with `std::exit(1)` kills the game on any throw; the outer `try/catch` in [ultramodern/src/threads.cpp](lib/N64ModernRuntime/ultramodern/src/threads.cpp) recovers.
- **Hardware SEH** (AVs, illegal instructions): caught, the thread terminates, the process continues.
- **STL bounds checks** ("vector subscript out of range"): the `_CrtSetReportHook` in [main.cpp](src/main/main.cpp) returns 1 to suppress the abort. Trade-off: occasional visual glitch over a hard crash.
- **`get_function(0)`** (NULL fn-ptr call): stubbed to a no-op in [librecomp/src/overlays.cpp](lib/N64ModernRuntime/librecomp/src/overlays.cpp) so the recompiled MIPS continues; logs the host return address for later mapping.

### RT64 interpreter safety

The interpreter loop in [rt64_interpreter.cpp](lib/rt64/src/hle/rt64_interpreter.cpp) has a 5-million-iter safety limit — without it a missing DL terminator marches `dl++` past RDRAM and AVs. It also guards against `hleGBI` going NULL mid-task (F3DEX `0xAF` `loadUCode` can fail to match and zero it out).

## When investigating a new crash

1. **Capture full output to a file.** Bash redirects don't flush before SIGTERM on Windows.
2. **Read the last few hundred lines** before the crash marker. The crash type is the bug class:
   - `Access violation reading address 0x1F8` (small) — NULL+offset deref, usually `hleGBI->map[opcode]` with NULL `hleGBI`. Guarded.
   - `Access violation reading address 0x1F2_xxxxxxxx` (huge) — stale GPU resource handle; renderer use-after-free.
   - `Assertion failed: ... rt64_gbi_f3d.cpp` — F3D handler hit an unimplemented case. Convert to log+skip.
   - `Assertion failed: ... rt64_native_target.cpp` — RT64 hit an unimplemented readback format. Convert to skip.
   - `vector subscript out of range` — STL bounds check, suppressed via `_CrtSetReportHook`.
   - `Failed to find function at 0x...` — recompiled MIPS called via NULL fn-ptr. Stubbed.
   - `Unable to find a matching GBI in the current database` — unrecognized ucode task; the mid-task NULL guard catches the resulting deref, that geometry doesn't render.
3. **Check recent `processDisplayLists ENTER` logs** — the latest `dlStart` names the DL; `NEW DL @` / `NEW sub-DL @` dumps show the first commands.
4. **Check `submit_rsp_task` counts.** `n_gfx` = M_GFXTASK enqueues, `n_other` = audio. Compare with `dp_complete` on 0x8011A408 (mqdiag `Dp` column) to find tasks stuck in RT64.
5. **Use `mqdiag_NNN.txt`** to validate queue-level theories before instrumenting.

For a hang specifically: if it's a cutscene/demo, first suspect the MORT voice freeze (see Audio quirk) before anything else — that's the known one.

## Avoid these dead ends (already disproven)

### Rendering / GBI

- **`op_80` as a sub-DL call** — treating its 24-bit w0 as a call target infinite-loops and hangs after ~200 DLs. It's a state/param load.
- **Y-flip / component swap on model textures** — retired. The stream is byte-faithful to golden; the texturing bug is inside RT64's render of a correct DL, not in our submission or a coordinate transform. Fix RT64, not the stream.
- **`ROGUESQ_SUPPRESS_OOB_CIMG` LOW-region filter as default-on** — Factor 5 LLE legitimately emits some lowmem CIMGs; keep it env-gated.
- **Synthetic per-halt FULL_SYNC injection in dpc_bridge** — corrupts RT64 tile state mid-frame; white-bounding-box artifacts and AVs in `loadTileOperation`.
- **Running LLE + HLE in parallel (`ROGUESQ_LLE_FORCE=1` without solo)** — LLE hits `Unhandled jump target 0xFEDB` on cinematic tasks, dpc_bridge SEH-catches an AV inside `processDisplayLists`, and a downstream `fullSync` then asserts on "Unimplemented 4 bits Readback mode". The 4-bit assertion is a *symptom* of LLE state corruption, not a real game-side issue.
- **A `cv.wait` rewrite of RT64's present-queue busy-wait** (`rt64_present_queue.cpp:38-46`) — regressed natural-exit rate. Reverted.

### libultra / scheduler

- **13-way contention on `0x8011A7E8`** — only one thread calls `func_8000C07C`.
- **cE/cF bytes at `0x80128EAE/F` as a frame-sync counter** — they're slot-type bytes in a scheduler table.
- **10× `dp_complete` to fix DP throughput** — producer side is fine.
- **Cooperative scheduler losing DP messages** — `mqdiag` shows 0 lost/requeued for the DP queue.
- **"iter 3 hangs" in `func_8000C07C`** — counter misread; the loop runs 30+ iters normally.
- **"iter ~810 cinematic freeze"** — was a symptom of the old LLE pipeline. The current VI-driven HLE path runs steady; not observable. Don't chase it.

### Boot flow / state machine

- **Force-menu bypass via `ROGUESQ_FORCE_MENU_AT_SEC`** — skipping cinematic init crashes downstream. Don't jump the state machine.
- **Re-investigating a "missing state-1 writer" in the 5-slot table at `D_80154620`** — that's the speech/streamed-voice playback slots, not cinematic stages. Related to the MORT freeze (the active byte lives here); the fix is the codec/watchdog, not a scheduler writer.

### Build / regeneration

- **Hand-editing `funcs_*.c` for game-logic overrides** — next regen silently strips it. Use `patches/`. Diagnostic `fprintf` probes are fine.
- **Regenerating `funcs_*.c` with the newer N64Recomp** (`build/Debug/N64Recomp.exe`) — it errors on the unsupported `cache 0x0D` instruction in `func_8000040C` / `func_80018D80` and **truncates `funcs.h` and the affected `funcs_N.c` mid-write**. Restore via `cd E:/Projects/N64Recomp && git checkout -- RecompiledFuncs/`. Use the *older* `E:/Projects/N64Recomp/Debug/N64Recomp.exe` for full regeneration.
- **Never write repo files via Python `open(...,'w')`** — a bad Python write once truncated the entire GBI core and it had to be rebuilt from goldens. Use the editor tools.

### Miscellaneous

- **The `wmain/main` link warning** — benign.
- **Shadowing `osPiStartDma_recomp` in `upstream_compat.cpp` with only the ROM-read branch** — boot needs the SRAM-read path too. Replicate `do_dma` in full or it regresses.

## Style conventions

- **No emojis** in code, comments, or docs unless explicitly requested.
- **No trailing summary blocks** in chat responses — one-line wrap-up max.
- Default to **no comments**. Add one only when the WHY is non-obvious (a workaround for a specific bug, a hidden invariant, a non-visible constraint).
- Don't reference the current task or session in comments — they rot.
- Prefer **editing existing files** over creating new ones. The runtime is already large; new files attract drift.
- For probe instrumentation in `funcs_*.c`, rate-limit:
  ```c
  { static int n=0; ++n; if (n<=10 || (n%50)==0) { fprintf(stderr, "..."); fflush(stderr); } }
  ```
- When renaming symbols, avoid address-encoded names (`clearByteAt801128CC`) — they add nothing over `func_HHHHHHHH`. If you can't see the semantics, leave it as `func_*`.

## Open work

Priorities, per the README's [Open work](README.md#open-work):

1. **Display-list desyncs** — about a dozen per run, typically garbage right after a material sub-DL returns. Root-cause with [docs/f5-model-dl-spec.md](docs/f5-model-dl-spec.md) and `tools/validate/f5_dl_walk.py`.
2. **Model texturing beyond the cinematic** — the RT64-side texture/UV bug (see the F5 GBI quirk). Byte-faithful stream confirmed; the defect is in RT64.
3. **MORT voice codec** — the demo/cutscene freeze fix. `tools/extract_speech_table.py` extracts the table; in-engine decode is unwritten.
4. **Retire the defensive KSEG0 pointer guards** — proven inert against hardware-golden runs; slated for removal via the TOML plus a regen. See [docs/plan-kseg-guard-migration.md](docs/plan-kseg-guard-migration.md).
5. **Function renaming** of `func_8XXXXXXX` symbols — pick a memory-map region or subsystem, pattern-match against the `rogue_squadron64` decomp's m2c output and string refs, propose meaningful names. Run `tools/rename/lint_toml_syms.py` after every batch. See [tools/rename/README.md](tools/rename/README.md) and [docs/game-architecture.md](docs/game-architecture.md).
6. **Keyboard input** — port Zelda64Recompiled's bind/rebind UI.

The team explicitly chose NOT to pursue full HLE for Factor 5 — see [docs/lle-spike-report.md](docs/lle-spike-report.md).
