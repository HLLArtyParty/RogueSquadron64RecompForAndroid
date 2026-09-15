# Debug Trace Environment Variables

The recompile accumulates many `fprintf(stderr, "[tag] ...")` trace points
during bug investigation. Most fire dozens or hundreds of lines per second
and are silent on a healthy run. To keep the terminal usable they are
gated behind environment-variable flags. This file lists every flag, what
trace tags it controls, where the code lives, and when you'd want to turn
it on.

## How to enable

Set the env var to a non-zero value before launching the executable:

**PowerShell / cmd**
```
set ROGUESQ_LOG_DPC=1
build\Debug\RogueSquadron64Recomp.exe
```

**bash / git-bash**
```
ROGUESQ_LOG_DPC=1 ./build/Debug/RogueSquadron64Recomp.exe
```

**Catch-all**: `ROGUESQ_LOG_ALL=1` enables every category at once.

To disable: unset, set to `0`, or leave undefined.

**From the command line**: every variable here can also be set without touching
the environment, by passing it to the executable as `--set NAME=VALUE` (or a
bare `NAME=VALUE`). These are equivalent:

```
RogueSquadron64Recomp.exe --set ROGUESQ_LOG_DPC=1
ROGUESQ_LOG_DPC=1 RogueSquadron64Recomp.exe
```

The common *user-facing* runtime options (graphics API, frame loop, audio,
headless helpers) have dedicated `--flags` instead — see `--help` and the README.
Everything below is debug/diagnostic and stays variable-only.

## Categories

| Env Var | Default | Tags Enabled | When To Enable |
|---|---|---|---|
| `ROGUESQ_LOG_ALL` | off | everything below | Quick "show me everything" — heavy output, OK for one-shot diagnostic. |
| `ROGUESQ_LOG_DPC` | off | `[task-brief]`, `[task]`, `[task#N gfx]`, `[task#N gfx-FULL]`, `[task#N setcombine]`, `[task#N movemem-idx]`, `[task#N hist]`, `[dpc] FULL_SYNC byte sent`, `[dpc] G_SETPRIM/ENV_COLOR`, `[dpc] PRIM override`, `[dpc] SET_COLOR/DEPTH/TEXTURE_IMAGE`, `[dpc-pak] *`, `[dpc-cine] ENTER`, `[dpc-64tri] ENTER`, `[trace] cinematic_drv ENTRY/BEFORE/AFTER` | Tracing what GFX commands the Factor5 LLE ucode emits. **Highest-volume category** — easily 1000+ lines/sec during cinematic. Use when investigating combiner muxes, texture loads, task pacing, or per-task tri/texrect counts. |
| `ROGUESQ_LOG_RDP_STATE` | off | `[trace] setCombine #N mux=...`, `[trace] setOtherMode #N cycleType=...`, `[texfilt] tf=...`, `[particle-mux] otherMode...` | RDP-state diagnosis — which combiner/cycleType is active when a specific draw fires. Useful when correlating "what mux runs at frame X" or "did texfilt change between expected and actual". |
| `ROGUESQ_LOG_PRESENT` | off | `[trace] RT64::Present #N swapIdx=...`, `[trace] PresentQueue::frame #N`, `[trace] Present::lookup`, `[trace] PresentQ::regfb/scratch`, `[trace] fbReg`, `[vi-status] word=...` | VI presentation / framebuffer routing investigations. The buffer-arbiter bug was diagnosed via these traces. Fires ~60 lines/sec. |
| `ROGUESQ_LOG_SP_TASKS` | off | `[sp] osSpTaskStartGo #N kind=GFX...` | Task-scheduling rate (cinematic ~30/s, normal play 1-2/frame). Useful when tracking how often the game submits GFX tasks to the RSP. |
| `ROGUESQ_LOG_VI` | off | `[osViSetMode #N ...]`, `[osViSwapBuffer #N fb=...]`, `[osViSetXScale/YScale]`, `[osViBlack]` | The game's VI mode and swap calls as they reach the libultra shims. |
| `ROGUESQ_LOG_THREADS` | off | `[osDestroyThread] t=... queue=... qok=... chain_ok=...` | Thread teardown with the queue-chain check. A failed check is always printed. |
| `ROGUESQ_LOG_PIPELINE` | off | `[pipe-1]`, `[pipe-2]`, `[pipe-3]` stage counters in RT64 framebuffer renderer | Per-stage TEXRECT pipeline counters (push → GPU draw). Used to verify the pipeline isn't dropping cinematic content between submission and rasterization. |
| `ROGUESQ_LOG_GBI` | off | per-handler GBI command logs | Every Factor 5 GBI handler as it fires. **Very high volume.** Use when auditing which opcodes run in a phase. |
| `ROGUESQ_LOG_GFX_TASK` | off | one line per graphics task | Low-volume task-submission trace; pair with queue diagnostics. |
| `ROGUESQ_LOG_MESG_TRACE` | off | thread/message-order trace | Message-order trace for `tools/validate/compare_mesg_trace.py`. Scope to frames with `ROGUESQ_MESG_TRACE_FRAMES=lo-hi`. |
| `ROGUESQ_MESG_TRACE_FRAMES` | all | (companion to `ROGUESQ_LOG_MESG_TRACE`) | Frame window `lo-hi` to limit the message-order trace. |
| `ROGUESQ_DUMP_FRAME_DL` | off | one-shot display-list dump | Dump the display list for frame `N` to disk for offline `f5_dl_walk.py` inspection. |
| `ROGUESQ_DUMP_TEXTURES` | off | one-shot texture dump | Write loaded textures to disk once, for asset diffing. |

## Workaround / experiment env vars (separate from logging)

These control *behavior*, not just logging. Listed here for convenience —
they're orthogonal to the log gates above but commonly co-used.

| Env Var | Default | Effect |
|---|---|---|
| `ROGUESQ_VI_FOLLOW_DRAW` | `1` | VI-presentation override picker. `0` = original VI/0x66A000 lookup. `1` (default) = override only when VI's fb is stale (not in recently-written set). `2` = aggressive — always pick most-recent color fb. `3` = freshness mode — pick whichever Framebuffer in the manager has the highest `lastWriteTimestamp`, decoupled from `colorImageAddressVector` (use when modes 1/2 don't keep VI on a fresh fb because the workload's pairs aren't `interpolationCandidate` and so the vector stays empty). Workaround for the cinematic buffer-arbiter bug. |
| `ROGUESQ_PRIM_FF` | off | Force PRIM_COLOR RGB to (FF,FF,FF) keeping alpha. Tests whether the warm off-white tint accounts for "less saturated reds" gap from ideal. |
| `ROGUESQ_NO_SYNTH_FULLSYNC` | off | Disables the synthetic-fullsync injection in dpc_bridge.cpp. |
| `ROGUESQ_VI_FORCE_FB` | off | Diagnostic. Forces VI to present a specific RDRAM fb regardless of VI_ORIGIN. Use as `ROGUESQ_VI_FORCE_FB=0x80695C00`. Bypasses the cinematic buffer-arbiter bug to test "explosion sprites land in fb X but VI never shows X" hypotheses. Strips upper-half virtual prefix automatically. |
| `ROGUESQ_NO_FULLSCREEN_FILLRECT` | off | Diagnostic. Suppresses full-screen FillRect (rect covers entire color target). Modes: `1`/`all` = skip every full-screen FillRect (causes Memory Pak attribution to ghost-trail since clears are needed there); `cinematic` = skip only when target is cinematic color fb 0x0062B800 / 0x00695C00 (preserves attribution clears, exposes cinematic content). Tests sub-frame-overwrite hypothesis for cinematic explosions. Partial FillRects always execute. |
| `ROGUESQ_FULL_DUMP` | off | When a crash dump is written (SEH handler, SIGABRT, F12), `=1` produces a full-memory minidump (~5 GB) instead of the default lite dump. Use only when you need RDRAM contents for postmortem. |
| `ROGUESQ_SUPPRESS_OOB_CIMG` | off | When set, drops Factor 5 ucode emissions of bogus SET_COLOR_IMAGE commands at HIGH (≥ 0x800000) and LOW (< 0x100000) addresses before they reach RT64. Reduces the iter-810 memory spike but causes a visual regression — the 3D Factor 5 logo no longer renders, since some legitimate Factor 5 lowmem CIMGs are dropped along with the garbage. |
| `ROGUESQ_FB_GUARDS` | on | Host framebuffer-window guards. Set `0` to disable for A/B comparison against hardware goldens. |
| `ROGUESQ_F5_CHUNK_BOUND` | on | Factor 5 DL chunk-bounded fetch grammar rule. Set `0` to disable when diagnosing a DL desync. |
| `ROGUESQ_F5_NON` | off | Force `NoN` (No-Near-clipping) for the Factor 5 ucode: `1` disables the GPU hard near-plane clip and uses the far-plane manual clamp instead. F5 uniquely ships `NoN=false`, so `depthClipEnabled=true` discards large geometry as it approaches the camera (objects "culled / lower-detail up close"). A/B fix for that symptom; matches every other `.NoN` ucode. Applied in `RSP::setGBI` (`lib/rt64/src/hle/rt64_rsp.cpp`). |
| `ROGUESQ_LLE_FORCE` | off | Route `M_GFXTASK` through the recompiled RSP ucode + `dpc_bridge.cpp` instead of RT64 HLE. Diagnostic only — semi-broken (cinematic tasks hit an unhandled jump). Companions `ROGUESQ_LLE_UNGATED` (skip the attribution-page gate) and `ROGUESQ_LLE_SOLO` (skip the HLE fallthrough). |

### Boot / attract-demo navigation

Levers to reach a specific attract-mode demo fast (instead of the ~5-min boot → intro → menu → demo-1 →
transition → demo-2 gauntlet). All live in `update_screen` (`src/main/rt64_render_context.cpp`) and poke
game RDRAM each present, same pattern as the other pokes.

| Env Var | Default | Effect |
|---|---|---|
| `ROGUESQ_CINE_FASTFWD` | `0` | Adds N extra ticks to the VI-retrace count (`0x8011A890`) each present while the intro cutscene is active and its `gateCtr` (`0x800B0B28`) is below the end threshold (`cutscene[0x44]-0xA`). Inflates the cinematic dt so the intro completes NATURALLY in seconds instead of ~6 min headless. Not a skip — the game still sets its own done bits. Use e.g. `=200`. |
| `ROGUESQ_SKIP_DEMO` | (unset) | Pins `gGameSettings.demoId` (`0x80130B54`) = N (0-5) every present, and pins the wrap counter `unk15` (`0x80130B55`) = 0 so `cycleIdleDemoId` (`0x8006F044`) can't drift the selector — so the attract mode plays (and LOOPS) that demo directly, skipping earlier demos + transitions. `demoId`→level: **0**=Mos Eisley/Tatooine, **1**=Jade Moon, **2**=Kile II, **3**=Taloraan, **4**=Fest, **5**=Trench Run (`dLevelByDemoId` `0x800CD404` = `00 05 07 0A 0B 11`; `gDemoFilenames` `0x80109AE4`). Combine with `ROGUESQ_CINE_FASTFWD` to blast the intro. Example: `ROGUESQ_SKIP_DEMO=1 ROGUESQ_CINE_FASTFWD=200` → Jade Moon in ~2 min. Selector RE in [plans/jade-moon-demo-freeze-plan.md](../plans/jade-moon-demo-freeze-plan.md). |
| `ROGUESQ_SKIP_TO_JADEMOON` | off | Convenience alias for `ROGUESQ_SKIP_DEMO=1` (the Jade Moon demo — has the structure-explosion freeze under investigation). |

### RT64 raster enhancements (Phase 0)

Opt-in `UserConfiguration` knobs applied in `create_render_context` (`src/main/rt64_render_context.cpp`)
before `app->setup()`. Raster-only (this build has `RT_ENABLED` off — no RT/DLSS/FSR/XeSS). See
[plans/rt64-f5-integration-plan.md](../plans/rt64-f5-integration-plan.md). Default baseline is unchanged
unless set.

| Env Var | Default | Effect |
|---|---|---|
| `ROGUESQ_MSAA` | off | Multisample anti-aliasing: `2`/`4`/`8` → MSAA 2x/4x/8x (rounds down; `<2` = None). |
| `ROGUESQ_RES_SCALE` | (RT64 2x) | Internal render-resolution multiplier; sets `resolution=Manual` + `resolutionMultiplier=<mult>` (e.g. `3.0`). Clamped by RT64's limit in `validate()`. |
| `ROGUESQ_SSAA` | `1` | Supersample downsample factor (`>=2` renders higher then downsamples — sharper, costlier). |
| `ROGUESQ_WIDESCREEN` | off | `1` → `aspectRatio=Expand` (fills the window aspect). F5 HUD/2D may need alignment work at non-4:3 (plan Phase 1b). |
| `ROGUESQ_ASPECT` | (unset) | Force a specific aspect ratio as a decimal `w/h` (e.g. `1.7777`); sets `aspectRatio=Manual`. Takes precedence over `ROGUESQ_WIDESCREEN`. |
| `ROGUESQ_HDR` | off | `1` → `internalColorFormat=High` (higher-precision internal color target). |
| `ROGUESQ_TEX_FILTER` | `aa` | Texture filtering: `nearest` / `linear` / `aa` (AntiAliasedPixelScaling). |
| `ROGUESQ_RT_INTERP` | off | Enable RT64 frame interpolation to `<hz>` (default 60): `refreshRate=Manual`, `refreshRateTarget=hz`. Gives smooth 60fps motion via RT64's built-in AUTO geometric matcher (validated on the Factor 5 path: smooth, nearly flicker-free) with no per-object id stamping needed. Off by default (`refreshRate=Original` = no interpolation). |
| `ROGUESQ_TEXTURE_PACK` | (unset) | Load an RT64 texture-replacement pack (a directory or `.zip` containing `rt64.json` + DDS/PNG) and enable replacements. F5 loads textures through the normal RDP TMEM path so RT64's content hashes are stable — packs resolve exactly as for a stock-ucode game. Applied after `app->setup()` via `textureCache->loadReplacementDirectories`. Authoring a pack uses RT64's developer dump workflow (see Zelda64Recomp's texture-pack tooling); the loader here is the runtime consumer. |

## How to add a new debug trace

1. Pick the right category from the table above (or invent a new one and add it here).
2. At the call site, gate the `fprintf(stderr, ...)` behind a static-cached env-var check:

   **C++ (lambda + static)**
   ```cpp
   static const bool log_x = []{
       const char *a = std::getenv("ROGUESQ_LOG_ALL");
       if (a && *a && *a != '0') return true;
       const char *e = std::getenv("ROGUESQ_LOG_DPC");  // your category
       return e && *e && *e != '0';
   }();
   if (log_x) {
       fprintf(stderr, "[your-tag] ...\n");
       fflush(stderr);
   }
   ```

   **C (manual init flag)**
   ```c
   static int log_x_init = 0, log_x = 0;
   if (!log_x_init) {
       const char *a = getenv("ROGUESQ_LOG_ALL");
       const char *e = getenv("ROGUESQ_LOG_DPC");
       log_x = ((a && *a && *a != '0') || (e && *e && *e != '0')) ? 1 : 0;
       log_x_init = 1;
   }
   if (log_x) { fprintf(stderr, "[your-tag] ...\n"); fflush(stderr); }
   ```

3. Add a comment above the gate stating *what* the trace shows and *when* to enable it.
4. If you introduce a new env-var category (rare — prefer reusing existing ones), update both this file and `src/main/debug_logs.h`.

## What is NOT gated (intentional)

These always print regardless of env vars — don't gate them:

- **`[CRASH]`, `[ABORT]`** — crash handler / SIGABRT path. Always need these for postmortem.
- **`[CRT_REPORT]`, `[INVALID_PARAM]`** — CRT debug-report hooks; rare and serious.
- **`[Audio] SDL_OpenAudioDevice failed`, `[ROM] Imported`, `[F12] manual minidump requested`** — one-shot user-facing events.
- **`[vi-follow-draw mode=N]`** — workaround/experiment confirmation message, throttled to first 3-5 hits. Telemetry that an opt-in workaround actually fired.
- **`[RT64] setCurrentThreadName threw`** — exception that the gated success path was supposed to avoid; you want to see this.
