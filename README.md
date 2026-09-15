<div align="center">
  <img src="https://github.com/MikeSemicolonD/RogueSquadron64Recomp/blob/main/favicon.ico">

  Icon by [thedoctor45 on DeviantArt](https://www.deviantart.com/thedoctor45/art/Star-Wars-Rogue-Squadron-3D-Custom-Icon-535469296)

# Star Wars: Rogue Squadron 64 Recompiled

</div>

A static recompilation of **Star Wars: Rogue Squadron** (N64, USA v1.0) built with [N64Recomp](https://github.com/N64Recomp/N64Recomp) and [N64ModernRuntime](https://github.com/N64Recomp/N64ModernRuntime), rendering through a forked [RT64](https://github.com/MikeSemicolonD/rt64) that understands Factor 5's custom display-list format.

> **Work in progress, not yet playable end-to-end.** The port boots, renders the attribution text, plays the Factor 5 / N64-logo cinematic with textures and sound, and reaches the main menu on most runs. In-mission rendering has been seen in the attract-mode demo but is not stable. See [Status](#status) for details and [docs/game-architecture.md](docs/game-architecture.md) for the subsystem map.
>
> **Heavily AI-assisted.** Most of the debugging, architectural decisions, and code here (the F3DFACTOR5 GBI module, the runtime patches inside `lib/`, `src/main/`, the `patches/` pipeline, the diagnostic env vars) were produced with Claude. Many choices are pragmatic workarounds rather than principled fixes, and the architectural conclusions should be scrutinized rather than trusted. Issues, corrections, and second opinions are welcome.

---

'*first-attempt*' branch **is** the farthest I have gotten. Second attempt was a more higher level focus but ended up slowly drifting back to what first attempt was which is mostly LLE focused. I tried to go away from that because debugging and printing data from assembly gets very hairy. Since my experience with assembly is limited to just my comp. architecture class, I feel like I've done all that I can. Hopefully this project can be used as a good jumping off point for the whole recomp project. 

To anyone in the future willing to take this on: **May the force be with you**

---

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
  <tr>
    <td><img src="https://github.com/MikeSemicolonD/RogueSquadron64Recomp/blob/MikesBranch/screenshots/progress2/Capture5.PNG"></td>
    <td><img src="https://github.com/MikeSemicolonD/RogueSquadron64Recomp/blob/MikesBranch/screenshots/progress2/Capture6.PNG"></td>
  </tr>
  <tr>
    <td><img src="https://github.com/MikeSemicolonD/RogueSquadron64Recomp/blob/MikesBranch/screenshots/progress2/Capture7.PNG"></td>
    <td><img src="https://github.com/MikeSemicolonD/RogueSquadron64Recomp/blob/MikesBranch/screenshots/progress2/Capture8.PNG"></td>
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
| **N64Recomp output** | `RecompiledFuncs/`, generated locally from the companion [rogue_squadron64](https://github.com/MikeSemicolonD/rogue_squadron64) decomp (started by [Tmcg2](https://github.com/Tmcg2/rogue_squadron64)) via the `regen_funcs` target |
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

### 2. Produce the decomp ELF

The recompiler needs the ELF from the companion [rogue_squadron64](https://github.com/MikeSemicolonD/rogue_squadron64) decomp (checked out next to this repo):

```sh
# In the rogue_squadron64 repo:
splat split roguesquadron.yaml
python tools/make_elf.py
```

### 3. Configure, generate the recompiled C, and build

```sh
# Windows (Visual Studio + ClangCL). Debug is the tested configuration.
cmake -B build -T ClangCL
cmake --build build --config Debug --target regen_funcs   # generate RecompiledFuncs/ from your ROM
cmake --build build --config Debug

# Linux / macOS
cmake -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build --target regen_funcs
cmake --build build
```

`regen_funcs` runs the recompiler (built from the `lib/N64ModernRuntime/N64Recomp` submodule) on `rogue_squadron.toml`, producing the gitignored `RecompiledFuncs/`. Re-run it when `rogue_squadron.toml`, the symbols, or the decomp ELF change; the next build picks up the new sources automatically.

The binary is `build/Debug/RogueSquadron64Recomp.exe` (Windows) or `build/RogueSquadron64Recomp` (Linux/macOS). Host-side edits rebuild and link in well under a minute.

| CMake option | Default | Purpose |
|---|---|---|
| `-DMIPS_TOOLCHAIN_DIR=path` | `E:/mips-toolchain` | Location of `mips64-elf-gcc` for `patches/` |
| `-DROGUESQ_DX12_DEBUG=ON` | OFF | D3D12 debug layer (Debug builds only) |
| `-DROGUESQ_NO_ITER_DEBUG=ON` | OFF | Disable MSVC debug iterators in `lib/rt64` for faster Debug runs |

---

## The recompiler config (`rogue_squadron.toml`)

`rogue_squadron.toml` is the N64Recomp config: it names the input ROM/ELF and defines the override layer applied during `regen_funcs`. Three directives shape the generated output without hand-editing it:

| Directive | Effect |
|---|---|
| `stubs = [...]` | Replace a function body with an empty no-op (RSP blobs, cache-instruction leaves, splat fragments) |
| `[[patches.instruction]]` | Overwrite one instruction at a `vram` with a raw `value` (e.g. NOP a `cache` op or a busy-wait branch) |
| `[[patches.hook]]` | Inject C at a function's entry or before a `vram` — guards, pacing, logging. Host helpers live in `src/main/hook_helpers.cpp` |

Edit the toml, then re-run `regen_funcs` to apply. For larger game-logic overrides, write MIPS-side C in the [`patches/`](patches/README.md) build instead — see Patching below.

---

## Running

Put `rogue_squadron.z64` next to the executable and launch it. The ROM hash is checked at startup.

## Controls

Keyboard, mouse, and gamepad all work; no controller is required. Defaults:

| N64 | Action | Keyboard | Gamepad |
| --- | --- | --- | --- |
| Analog stick | Steer / bank | WASD / arrows | Left stick |
| A | Fire lasers | Space | A |
| B | Drop bombs | Left Shift | X |
| Z | Brake | Left Ctrl | Left trigger |
| R | Boost | E | Right shoulder |
| L | Targeting computer | Q | Left shoulder |
| D-Pad | Throttle / trim | Numpad 8/4/2/6 | D-Pad |
| C-Up / C-Down | Cycle views | I / K | Y / B |
| C-Left / C-Right | Roll | J / L | Back / Guide |
| Start | Pause | Enter | Start |

**Mouse flight steering:** press the backtick key (`` ` ``) to toggle mouse
capture — mouse motion then steers the craft, left/right click fire lasers /
drop bombs. `Esc` releases capture.

### Rebinding controls

Press **F6** to open the **Controls** window. Click **Rebind** on any action and
press the key, gamepad button, or mouse button to assign it; **Clear** removes a
binding. Adjust mouse sensitivity and invert there, then **Save** (or **Restore
defaults**). Bindings persist to `roguesq_input.json` next to the executable,
which you can also hand-edit.

> In Debug builds (developer mode on by default) the RT64 inspector owns the
> ImGui overlay, so press **F1** once before **F6**. Release builds open the
> Controls window with **F6** directly.

---

## Architecture

Recompiled game code (`RecompiledFuncs/`, generated) and hand-written overrides (`patches/`, linked first so their symbols win) compile into one static library, linked against forked builds of N64ModernRuntime (the libultra/runtime host) and RT64 (the renderer). `src/main/` wires it together.

| Path | Role |
|---|---|
| `src/main/main.cpp` | Entry point — SDL2 window/audio/input, RSP task dispatch |
| `src/main/rt64_render_context.cpp` | RT64 integration — `send_dl`, VI registers, framebuffer sanitizer |
| `src/main/register_overlays.cpp` | Boot-time overlay function-table registration |
| `src/main/upstream_compat.cpp` | libultra shims, overlay loader, HMT-load capture |
| `lib/rt64/src/gbi/rt64_gbi_f3dfactor5.cpp` | The Factor 5 GBI module (the render core) |
| `patches/` | Game-function overrides, cross-compiled to MIPS |
| `src/rsp/`, `*_rsp.toml` | RSPRecomp configs and DPC bridge for the opt-in LLE path |

**Graphics.** Rogue Squadron uses Factor 5's own display-list format, which stock RT64 cannot parse; the forked RT64 carries a GBI module for it. `M_GFXTASK` goes straight to RT64's HLE processor, which emits native RT64 geometry. The grammar is in [docs/f5-model-dl-spec.md](docs/f5-model-dl-spec.md) and validated offline against Project64 dumps with `tools/validate/f5_dl_walk.py`. The original LLE path (RSP ucode via RSPRecomp, RDP bytes through `dpc_bridge.cpp`) is kept behind `ROGUESQ_LLE_FORCE=1`.

**Frame pacing.** By default the game's own VI / SP / DP message protocol runs as on hardware, with SP-done delivered after RT64 parses the list. `--no-vi-driven-loop` (`ROGUESQ_VI_DRIVEN_LOOP=0`) restores the older host-paced loop.

**Audio.** MusyX drives SFX and music; samples stream from the cartridge via PI DMA as on hardware.

**Patching.** Overrides live in [`patches/`](patches/) and as `[[patches.hook]]` entries in `rogue_squadron.toml`, never as hand edits to the generated `RecompiledFuncs/`, so regeneration is safe. The pattern follows [Zelda64Recomp](https://github.com/Zelda64Recomp/Zelda64Recomp/tree/dev/patches) but uses `mips64-elf-gcc` instead of clang. See [patches/README.md](patches/README.md).

---

## Debugging

[docs/debugging-with-visual-studio.md](docs/debugging-with-visual-studio.md) covers attaching Visual Studio to the recompiled output and telling a recompile bug from a game-logic bug. Helpers under [tools/](tools/):

- `dump-game.ps1` writes a full-memory minidump of a running instance, even when the window is unresponsive. F12 in-game does the same.
- `inspect-dump.py` and `dump_stackscan.py` list and symbolize threads from a minidump.
- `run-stability.ps1` launches N timed runs and classifies each by stderr markers.
- `validate/` captures Project64 goldens, diffs RDRAM, compares message-order traces, and walks display lists offline.

A watchdog thread writes `mqdiag_NNN.txt` message-queue snapshots every 3 seconds.

### Command-line options

Run `RogueSquadron64Recomp.exe --help` for the full list. The common options:

| Option | Effect |
|---|---|
| `--gfx-api <vulkan\|d3d12>` | Force the graphics API (default auto) |
| `--[no-]hle-dev-mode` | RT64 ImGui inspector on F1 (default on in Debug, off in Release) |
| `--no-vi-driven-loop` | Old host-paced frame loop instead of the hardware protocol (default is VI-driven) |
| `--no-f5-native` | Parse F5 display lists without emitting geometry |
| `--no-audio-ucode` | Silent audio stub instead of the MusyX synth |
| `-m` / `--mute`, `--audio-gain <f>` | Silence output, or scale master gain |
| `--audio-latency-ms <n>` | Audio buffer latency in milliseconds |
| `--dump-pcm <path>` | Write the synth output to a 22050 Hz stereo WAV |
| `--render-song <key>` | Force a specific song (0 = the N64-logo music) |
| `--fake-controller`, `--auto-start <ms>` | Headless runs: fake a controller, pulse START |
| `--set NAME=VALUE` | Set any `ROGUESQ_*` variable directly |

Each option maps to a `ROGUESQ_*` environment variable, which still works (a bare `NAME=VALUE` argument does too). The full debug/trace/experiment catalog — logging categories, DL/texture dumps, message-order traces, and rendering A/B toggles — lives in [docs/debug-trace-env-vars.md](docs/debug-trace-env-vars.md); reach any of those from the command line with `--set NAME=VALUE`.

With the inspector enabled, F1 toggles RT64's ImGui overlay (configuration, texture dumping, per-call debugger, render-target view). F3 toggles ViewRDRAM mode and F4 toggles texture replacement, or pauses the debugger while an inspector window is focused.

---

## Acknowledgements

- **Dávid Pethes**: the [rerogue](https://github.com/dpethes/rerogue) tools and the [satd.sk write-up](https://satd.sk/pages/rs/) document the PC build's HOB, HMT, HMP, and MORT formats, which the N64 build shares. They directly inform the texture and model pipeline here.
- **[jrra](https://github.com/jrra/rerogue)**: a community fork of rerogue.
- **[Tmcg2](https://github.com/Tmcg2/rogue_squadron64)**: started the companion decomp project.

## License

See [LICENSE](LICENSE). This project contains no ROM data and requires a legally obtained copy of the game.
