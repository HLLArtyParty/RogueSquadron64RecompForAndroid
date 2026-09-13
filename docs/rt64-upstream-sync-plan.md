# RT64 / N64ModernRuntime upstream-sync plan

Goal: pull independent upstream fixes into our heavily-modified forks — primarily the
RT64 **tile-sampling synchronization** and **F3D far-plane clipping** fixes, which land
directly on the subsystem behind our open "model texture bug is RT64-side" issue — and
close the small set of places where we ignore an API the library already offers.

Status: **awaiting go.** Michael preps (section 1); Claude executes (sections 3+) on the word.

---

## 0. Current state (measured 2026-09-13)

- `lib/rt64` — fork branch `HLE-F5-Attempt`, forked at `1bcbda3` (2026-05-03), **15 commits behind** `upstream/main`.
  Fork history is messy: ~25 commits with many add→revert pairs. **Do not linear-rebase** — merge or cherry-pick.
- `lib/N64ModernRuntime` — local symlink to `E:\Projects\N64ModernRuntime`, branch off `0bb76b0`, **6 commits behind** upstream. Lower priority (no fix targets a current bug).
- Both submodules have `upstream` remotes already configured.

Files our F5 fork changed **and** pending upstream also changes (the real conflict points):
`CMakeLists.txt`, `src/hle/rt64_present_queue.cpp`, `src/hle/rt64_rsp.cpp`,
`src/hle/rt64_state.cpp`, `src/shaders/RasterPS.hlsl`.

---

## 1. Prep checklist (Michael — before "go")

- [ ] Working tree clean enough to branch: current changes are `src/main/*.cpp`, `tools/validate/*`. Decide whether they stay or get stashed; the rt64 work is isolated in the submodule so this mostly won't collide.
- [ ] Confirm the **texture golden** is on disk and current: the frame900 model-texture golden and the `f5_dl_walk.py --tex` reference used to prove byte-identity. This is the pass/fail gate for the whole effort — if it's stale, the merge can't be judged.
- [ ] Confirm a known-good **Debug** build works *right now* on `HLE-F5-Attempt` (baseline screenshot of the affected model + a golden-diff run that passes). We need the "before" to compare against.
- [ ] Decide scope for this pass: **(A) tile-sync + far-plane only** (surgical, recommended first) or **(B) full merge of upstream/main** (all 15 commits). Plan covers both; A is the low-risk lead.
- [ ] Note the RT64 uprev also moves the bundled `plume` (render backend), `nlohmann/json` 3.12.0, and an SDL2 build fix — a full merge (option B) may shift the build toolchain surface. Have the build environment ready to rebuild RT64 from scratch.

---

## 2. Target commits (upstream `lib/rt64`)

Net-effective fixes at `upstream/main` HEAD (revert churn already resolved upstream):

| Fix | Final commit | Files | Why we want it |
|-----|--------------|-------|----------------|
| Tile-sampling sync (#254 finalized as #259) | `f0933d2` | `rt64_state.cpp`, `rt64_framebuffer_manager.cpp/.h`, `rt64_tmem_hasher.h` | **Primary** — texture-tile sampling sync; same subsystem as our texture bug |
| Far-plane clip on pixel shader, F3D bug (#244) | `6f1c2d9` (reapply) | `RasterPS.hlsl` | Same shader file our fork edits in the UV-derivative/LOD path |
| Viewport clip-rect draw-area detection (#262) | `5473732` | state | Correctness for triangle draw-area |
| VI inverted-region + RDNA4 workaround (#264/#265) | `4337374`, `8be17e7` | app | Stability, low risk |

Do **not** cherry-pick the intermediate `Revert …` commits — upstream HEAD is already the resolved state.

N64ModernRuntime (defer unless something below needs it): CLI game/gamemode selection (#153/#149),
`is_entrypoint` API (#147), stack args ≥4 in `_arg` (#138). None fixes a current bug.

---

## 3. Execution — RT64 sync (Claude, on "go")

Work on a **scratch branch in the submodule** so the parent repo's submodule pointer is untouched until validated.

1. `cd lib/rt64`, `git fetch upstream`, create branch: `git switch -c f5-upstream-sync-2026-09` from `HLE-F5-Attempt`.
2. **Option A (surgical, default):** cherry-pick in order: `f0933d2` (tile sync), `6f1c2d9` (far-plane), then optionally `5473732`, `4337374`, `8be17e7`.
   **Option B (full):** `git merge upstream/main`.
3. Resolve conflicts, expected in the 5 overlap files. Guiding rules:
   - `RasterPS.hlsl` — our edit swapped `ddx/ddyVertexUV` handling in the LOD path; upstream #244 adds far-plane clip logic. Keep **both**; they're in different regions. Re-derive our `ddxuvx`/`ddyuvy` change on top of upstream's version rather than discarding either.
   - `rt64_state.cpp` — our F5 additions (fullSync/fbPair handling) vs upstream tile-sync + clip-rect. Merge by hand; verify our `state->fullSync`/fbPair paths still compile against any changed signatures.
   - `rt64_present_queue.cpp`, `rt64_rsp.cpp`, `CMakeLists.txt` — reconcile; CMake likely gains the json/plume/SDL changes.
4. Build RT64 + the recomp (`cmake` per project flow, Debug). Fix compile breaks from any changed RT64 internal signatures our host code reaches into (`app->state->…` pokes flagged in the audit are fragile here — expect touch-ups in `src/main/rt64_render_context.cpp`).
5. **Validation gate (must pass to keep the merge):**
   - Run the model-texture golden diff (`tools/validate/f5_dl_walk.py --tex` + `dl_diff.py` / `checkpoint.ps1`) — byte-identity of DL/UV/texture stream must still hold.
   - Boot the app, capture the affected model, compare to the baseline screenshot from prep.
   - Watch specifically whether the tile-sync fix changes the rendered texture output (the whole point).
6. Record result: does the texture bug improve, change, or stay identical? Either way it's signal — if the stream was already byte-faithful and the sync fix changes the picture, that corroborates the RT64-side diagnosis.
7. If good: fast-forward `HLE-F5-Attempt` (or keep the dated branch), bump the parent-repo submodule pointer, note the new upstream base. If bad: branch is disposable, submodule pointer never moved.

---

## 4. Secondary track — `update_config` (independent, low risk)

Separable from the RT64 sync; can be done before or after.

- `src/main/rt64_render_context.cpp:269` — `update_config` currently returns `true` and drops everything.
- Wire it to RT64's existing `Application::updateUserConfig(bool discardFBs)` and `updateEnhancementConfig()` so runtime resolution / AA / aspect / buffering changes from the host config actually apply.
- Gate: change a setting at runtime, confirm it takes effect and nothing regresses in the F5 present path.

---

## 5. Optional cleanups (only if time; not blockers)

- Move the log-only `os*_recomp` overrides (`osViSetMode`, `osViSetXScale/YScale`, `osStartThread`, and the forward-only halves of `osSpTaskStartGo`/`osSendMesg`) to `[[patches.hook]]` and delete the duplicates, shrinking the `/FORCE:MULTIPLE` surface. See [feedback: load-bearing inline edits → hooks].
- Confirm whether `rt64_cine_fb_is_slot_owned` (`src/rsp/dpc_bridge.cpp:91`) is dead — no caller found in either tree. Remove the ownership-map writes if so, else wire its call site.

---

## 6. Risks / rollback

- **Fork revert churn** makes cherry-pick order matter — take upstream's *final* commits only, never the intermediate reverts.
- **Internal-API coupling**: our host code reaches into private `app->state->…` members (`fullSync`, framebuffer registry). An RT64 uprev can silently change these; budget for `rt64_render_context.cpp` touch-ups and re-run validation.
- **Full merge (option B)** drags in plume/json/SDL build changes — larger blast radius. Prefer option A first.
- **Rollback is cheap**: all work is on a scratch submodule branch; the parent repo's submodule commit is not moved until the golden gate passes. Nothing to undo if it fails.

---

## 7. Definition of done

- RT64 fork carries the tile-sync + far-plane fixes (min: option A), builds clean, texture golden diff still passes.
- Documented before/after on the model-texture bug (improved / changed / unchanged — all are useful signal).
- Parent-repo submodule pointer bumped only after the gate passes.
