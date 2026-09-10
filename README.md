<div align="center">
  <img src="https://github.com/MikeSemicolonD/RogueSquadron64Recomp/blob/main/favicon.ico">

  Icon by [thedoctor45 on DeviantArt](https://www.deviantart.com/thedoctor45/art/Star-Wars-Rogue-Squadron-3D-Custom-Icon-535469296)

# Star Wars: Rogue Squadron 64 Recompiled

</div>

A static recompilation of **Star Wars: Rogue Squadron** (N64, USA v1.0) built with [N64Recomp](https://github.com/N64Recomp/N64Recomp) and [N64ModernRuntime](https://github.com/N64Recomp/N64ModernRuntime), rendering through a forked [RT64](https://github.com/MikeSemicolonD/rt64) that understands Factor 5's custom display-list format.

> **Work in progress, not yet playable end-to-end.** The port boots, renders the attribution text, plays the Factor 5 / N64-logo cinematic with textures and sound, and reaches the main menu on most runs. In-mission rendering has been seen in the attract-mode demo but is not stable. See [Status](#status) for details and [docs/game-architecture.md](docs/game-architecture.md) for the subsystem map.
>
> **Heavily AI-assisted.** Most of the debugging, architectural decisions, and code here (the F3DFACTOR5 GBI module, the runtime patches inside `lib/`, `src/main/`, the `patches/` pipeline, the diagnostic env vars) were produced with Claude. Many choices are pragmatic workarounds rather than principled fixes, and the architectural conclusions should be scrutinized rather than trusted. Issues, corrections, and second opinions are welcome.

<div align="center">

### Screenshots

<table>
  <tr>
    <td><img src="https://github.com/MikeSemicolonD/RogueSquadron64Recomp/blob/MikesBranch/screenshots/progress2/Capture1.PNG"></td>
    <td><img src="https://github.com/MikeSemicolonD/RogueSquadron64Recomp/blob/MikesBranch/screenshots/progress2/Capture2.PNG"></td>
  </tr>
  <tr>
    <td><img src="https://github.com/MikeSemicolonD/RogueSquadron64Recomp/blob/MikesBranch/screenshots/progress2/Capture3.PNG"></td>
    <td><img src="https://github.com/MikeSemicolonD/RogueSquadron64Recomp/blob/MikesBranch/screenshots/progress2/Capture4.PNG"></td>
  </tr>
</table>
</div>

---

## Requirements

| Requirement | Notes |
|---|---|
| **ROM** | `rogue_squadron.z64`, USA v1.0 (16 MB, xxHash3-64 `0x6B66A44153594DEA`) |
| **OS / GPU** | Windows 10+, Linux, or macOS 11+ with a D3D12, Vulkan, or Metal capable GPU |
| **CMake** | 3.20+ |
| **Compiler** | MSVC with the ClangCL toolset (Windows), Clang or GCC (Linux/macOS) |
| **N64Recomp output** | `RecompiledFuncs/` generated from the companion [rogue_squadron64](https://github.com/MikeSemicolonD/rogue_squadron64) decomp (started by [Tmcg2](https://github.com/Tmcg2/rogue_squadron64)) |
| **MIPS cross-compiler** *(optional)* | `mips64-elf-gcc` for the [`patches/` build](patches/README.md). Windows builds are at [n64-tools](https://github.com/n64-tools/gcc-toolchain-mips64/releases); the official LLVM Windows installers lack the MIPS backend. Default path `E:/mips-toolchain` (override with `-DMIPS_TOOLCHAIN_DIR`). Without it CMake warns and skips the patches build. |
| **GNU make** *(optional)* | For `patches/Makefile`. `mingw32-make` works. |

---

## Building

### 1. Clone with submodules

```sh
git clone --recurse-submodules https://github.com/MikeSemicolonD/RogueSquadron64Recomp.git
cd RogueSquadron64Recomp
```

`lib/` holds forks of [N64ModernRuntime](https://github.com/MikeSemicolonD/N64ModernRuntime) and [rt64](https://github.com/MikeSemicolonD/rt64). They are forked because Factor 5's custom microcode needs changes stock upstream would not take.

### 2. Generate the recompiled C output

```sh
# In the rogue_squadron64 repo:
splat split roguesquadron.yaml
python tools/make_elf.py
# In the N64Recomp repo:
N64Recomp rogue_squadron.toml
```

Output lands in `../N64Recomp/RecompiledFuncs/` relative to this repo. Only redo this step when the TOML or symbol names change.

### 3. Configure and build

```sh
# Windows (Visual Studio + ClangCL). Debug is the tested configuration.
cmake -B build -T ClangCL
cmake --build build --config Debug

# Linux / macOS
cmake -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build
```

The binary is `build/Debug/RogueSquadron64Recomp.exe` (Windows) or `build/RogueSquadron64Recomp` (Linux/macOS). Host-side edits rebuild and link in well under a minute.

| CMake option | Default | Purpose |
|---|---|---|
| `-DMIPS_TOOLCHAIN_DIR=path` | `E:/mips-toolchain` | Location of `mips64-elf-gcc` for `patches/` |
| `-DROGUESQ_DX12_DEBUG=ON` | OFF | D3D12 debug layer (Debug builds only) |
| `-DROGUESQ_NO_ITER_DEBUG=ON` | OFF | Disable MSVC debug iterators in `lib/rt64` for faster Debug runs |

---

## Running

Put `rogue_squadron.z64` next to the executable and launch it. The ROM hash is checked at startup.

## Controls (gamepad)

| N64 | Action | Gamepad |
|---|---|---|
| Analog stick | Steer / bank | Left stick |
| A | Fire lasers | A |
| B | Drop bombs | X |
| Z | Brake | Left trigger |
| R | Boost | Right shoulder |
| L | Targeting computer | Left shoulder |
| D-Pad | Throttle / trim | D-Pad |
| C-Up / C-Down | Cycle views | Y / B |
| C-Left / C-Right | Roll | Back / Guide |
| Start | Pause | Start |

Keyboard input is not implemented.

---

## How it fits together

```
src/main/main.cpp                Entry point: SDL2 window/audio/input, RSP task dispatch
src/main/rt64_render_context.cpp RT64 integration: send_dl, VI registers, framebuffer sanitizer
src/main/register_overlays.cpp   Boot-time overlay function-table registration
src/main/upstream_compat.cpp     libultra shim overrides, overlay loader, HMT-load capture
src/rsp/dpc_bridge.cpp           DPC_START/DPC_END bridge for the opt-in LLE graphics path
src/rsp/aspMain.cpp              Legacy silent audio stub (unused by default)
lib/rt64/src/gbi/rt64_gbi_f3dfactor5.cpp   The F3DFACTOR5 GBI module (plus f5_rdpstate / f5_diag)
patches/                         Hand-written game-function overrides, cross-compiled to MIPS
factor5_rsp.toml                 RSPRecomp config for the graphics ucode (LLE path)
factor5_boot_rsp.toml            RSPRecomp config for the boot ucode
musyx_rsp.toml                   RSPRecomp config for the MusyX audio synth
```

Build pipeline: `RecompiledFuncs/` (game code as C) and `patches/` (overrides, linked first so their symbols win) are compiled into a static library, linked with the forked runtime and renderer, and wired up by `src/main/`.

**Graphics.** Rogue Squadron uses Factor 5's own display-list format, which stock RT64 cannot parse. The forked RT64 carries a GBI module for it. `M_GFXTASK` is handed straight to RT64's HLE display-list processor, and the module emits native RT64 vertices and triangles. The grammar is documented in [docs/f5-model-dl-spec.md](docs/f5-model-dl-spec.md) and validated offline against Project64 memory dumps with `tools/validate/f5_dl_walk.py`. The original LLE path (RSP ucode recompiled with RSPRecomp, RDP bytes forwarded through `dpc_bridge.cpp`) is kept behind `ROGUESQ_LLE_FORCE=1`.

**Frame pacing.** By default the game's own VI / SP / DP message protocol runs as it does on hardware, and SP-done is delivered after RT64 has parsed the list. `ROGUESQ_VI_DRIVEN_LOOP=0` restores the older host-paced loop.

**Audio.** The MusyX RSP synth is recompiled with RSPRecomp (`musyx_rsp.toml`) and runs by default. Samples stream from the cartridge via PI DMA as on hardware.

**Patching game functions.** Overrides live in [`patches/`](patches/) and as `[[patches.hook]]` entries in the N64Recomp `rogue_squadron.toml`, never as hand edits to the generated `RecompiledFuncs/funcs_*.c`, so regeneration is safe. The pattern follows [Zelda64Recomp/patches](https://github.com/Zelda64Recomp/Zelda64Recomp/tree/dev/patches) but uses `mips64-elf-gcc` instead of clang. See [patches/README.md](patches/README.md) for the pipeline, the `syms.ld` pattern, and the gotchas.

---

## Status

| System | Status |
|---|---|
| Boot → attribution → cinematic → menu | ✅ Reached on most runs without host intervention |
| Attribution screen | ✅ Legal text renders on black |
| Factor 5 / N64-logo cinematic | ✅ Textured, with camera motion and explosion. The white backdrop is intended content |
| Main menu | ✅ Photographic background and title text. The mispositioned background tiles were untextured `0xB4` quads mis-parsed by the GBI (stride bug, fixed 2026-09-09); they now parse correctly — with some misplaced UVs |
| In-mission gameplay | ⏳ The attract demo has rendered (untextured terrain, flat models). Not stable |
| Audio | ✅ SFX and music via the recompiled MusyX synth. The MORT voice codec for subtitled dialogue is not implemented |
| Input | SDL2 gamepad only |
| Save data | EEPROM 4K via librecomp |
| Memory pak | Stubbed (reports no pak) |

### Open work

1. **Display-list desyncs.** About a dozen per run, typically garbage right after a material sub-DL returns. Root-cause with the DL spec and `f5_dl_walk.py`.
2. **Model texturing** beyond the cinematic. (see the F5 GBI module)
3. **MORT voice codec** (per [rerogue](https://github.com/dpethes/rerogue)). `tools/extract_speech_table.py` extracts the table; in-engine decode is the missing piece.
4. **Retire the defensive KSEG0 pointer guards.** They never fire against hardware-golden runs and are slated for removal via the TOML plus a regen.
5. **Function renaming** of `func_8XXXXXXX` symbols (see [tools/rename/README.md](tools/rename/README.md)). Run `tools/rename/lint_toml_syms.py` after every batch.
6. **Keyboard input.**

---

## Debugging

[docs/debugging-with-visual-studio.md](docs/debugging-with-visual-studio.md) covers attaching Visual Studio to the recompiled output and telling a recompile bug from a game-logic bug. Helpers under [tools/](tools/):

- `dump-game.ps1` writes a full-memory minidump of a running instance, even when the window is unresponsive. F12 in-game does the same.
- `inspect-dump.py` and `dump_stackscan.py` list and symbolize threads from a minidump.
- `run-stability.ps1` launches N timed runs and classifies each by stderr markers.
- `validate/` captures Project64 goldens, diffs RDRAM, compares message-order traces, and walks display lists offline.

A watchdog thread writes `mqdiag_NNN.txt` message-queue snapshots every 3 seconds.

### Environment variables

All traces are off by default. The full list is in [docs/debug-trace-env-vars.md](docs/debug-trace-env-vars.md); read it before adding a new `fprintf`. The ones you are most likely to need:

| Env var | Effect |
|---|---|
| `ROGUESQ_GFX_API=vulkan\|d3d12` | Force the graphics API (default auto) |
| `ROGUESQ_HLE_DEV_MODE=0\|1` | RT64 ImGui inspector on F1 (default on in Debug, off in Release) |
| `ROGUESQ_VI_DRIVEN_LOOP=0` | Old host-paced frame loop instead of the hardware protocol |
| `ROGUESQ_F5_NATIVE=0` | Parse F5 display lists without emitting geometry |
| `ROGUESQ_F5_CHUNK_BOUND=0` / `ROGUESQ_F5_B5_CHUNKEND=0` / `ROGUESQ_F5_DL_SKIP16=0` | Disable individual DL grammar rules (all default on) |
| `ROGUESQ_LLE_FORCE=1` | Run graphics through the recompiled RSP ucode instead of HLE |
| `ROGUESQ_FB_GUARDS=0` | Disable the host framebuffer-window guards for A/B against goldens |
| `ROGUESQ_BUFSTUCK_RESCUE=0` | Disable the video-buffer arbiter rescue hook |
| `ROGUESQ_NO_AUDIO_UCODE=1` | Silent audio stub instead of the MusyX synth |
| `ROGUESQ_DUMP_PCM=<path>` | Write the synth output to a 22050 Hz stereo WAV |
| `ROGUESQ_RENDER_SONG=<key>` | Force a specific song (0 = the N64-logo music) |
| `ROGUESQ_FAKE_CONTROLLER=1` / `ROGUESQ_AUTO_START=<ms>` | Headless runs: fake a controller, pulse START |
| `ROGUESQ_LOG_ALL=1` | Every trace category |
| `ROGUESQ_LOG_GBI=1` | Per-handler GBI logs (very high volume) |
| `ROGUESQ_LOG_GFX_TASK=1` | One line per graphics task |
| `ROGUESQ_LOG_MESG_TRACE=1` (+ `ROGUESQ_MESG_TRACE_FRAMES=lo-hi`) | Thread/message-order trace for `tools/validate/compare_mesg_trace.py` |
| `ROGUESQ_DUMP_FRAME_DL=N` / `ROGUESQ_DUMP_TEXTURES=1` | One-shot display-list and texture dumps |

With the inspector enabled, F1 toggles RT64's ImGui overlay (configuration, texture dumping, per-call debugger, render-target view). F3 toggles ViewRDRAM mode and F4 toggles texture replacement, or pauses the debugger while an inspector window is focused.

---

## Acknowledgements

- **Dávid Pethes**: the [rerogue](https://github.com/dpethes/rerogue) tools and the [satd.sk write-up](https://satd.sk/pages/rs/) document the PC build's HOB, HMT, HMP, and MORT formats, which the N64 build shares. They directly inform the texture and model pipeline here.
- **[jrra](https://github.com/jrra/rerogue)**: a community fork of rerogue.
- **[Tmcg2](https://github.com/Tmcg2/rogue_squadron64)**: started the companion decomp project.

## License

See [LICENSE](LICENSE). This project contains no ROM data and requires a legally obtained copy of the game.
