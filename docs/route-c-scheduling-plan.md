# Route C — Hardware-accurate scheduling / interrupt fidelity for frame pacing

**Status:** plan (2026-09-13). Prereq context: audio screech + stutter fixed (see
`project_audio_validation_harness_2026_09_09` memory). This is the *deep* pacing fix that could smooth
graphics frame pacing and harden the frame loop, and is the common root behind the VI-driven work.

## 1. Objective & scope

Make the recomp's thread/interrupt behavior faithful enough to hardware that **frame pacing is smooth and
the VI-driven frame loop is robust**, without the historical deadlock. Explicitly **out of scope**: audio
production cadence — already fixed by decoupling (free-running AI clock), no preemption needed.

Success = frames present at a steady hardware-matched cadence (gate rate ≈ PJ64 golden's, e.g. gate@120),
no long-run stalls (>2000 cinematic frames), audio stays 100%, and no new data races.

## 2. Verified model (2026-09-13)

- **Cooperative scheduler** (ultramodern `scheduling.cpp`/`threads.cpp`): each OSThread is a host thread,
  but only the token-holder runs; others block on a `running` semaphore. Priority is honored **only at
  scheduling points** (`check_running_queue`, hit on message-queue ops / yields). **A thread executing
  recompiled C is never preempted mid-run.**
- **Host "interrupt" threads** (`vi_thread_func` precise 60 Hz; `ai_thread_func` consumption deadlines;
  SP/DP completion) `enqueue_external_message` → mark the target game thread runnable → it runs at the
  **next yield of the currently-running game thread**.
- **Gfx is async**: SP gfx tasks go to `action_queue` → `gfx_thread_func` (own host thread) parses the DL
  and fires `sp_complete` (instant) + `dp_complete`. So DL parsing does **not** starve the cooperative
  game scheduler. Frame pacing is about *when the game submits + when VI presents*, not parse time.
- **Current pacing:** VI-driven is default-on (`rs64_vi_driven()` returns 1). A 38 s cinematic run advanced
  with no deadlock, so the historical 3-way deadlock is **not currently reproducing** (resolved or
  scene-specific). Route C is therefore *smoothness/robustness*, not unblocking a hang.

## 3. The core limitation

On hardware there are 3 CPU threads **plus interrupts that preempt**. Circular waits (e.g. retrace thread
waiting on "task done", SP scheduler waiting on "task requests", game thread waiting on "video buffer free")
are broken because an interrupt (DP-done, VI) **preempts** and delivers the unblocking message immediately.
In ultramodern's cooperative model, if all game threads are blocked on receives, **no handler runs to break
the cycle** → the historical VI-driven deadlock. Even without a hard deadlock, message delivery only at
yield points adds latency/jitter to the present cadence vs hardware's immediate interrupt delivery.

Why ultramodern is cooperative on purpose: N64 games rely on priority + single-CPU semantics and often
**don't lock shared data**. True preemption can introduce data races the game never guarded against. That
tension is the central risk of Route C.

## 4. Key insight from the audio fix

Audio was fixed **not** by adding preemption but by **decoupling a host-side clock** (free-running AI
consumption deadline) so the game is driven at the true rate regardless of cooperative round-trip latency.
**Implication:** try the decouple pattern for graphics pacing *before* committing to true preemption — it
may deliver hardware-matched present cadence at far lower risk.

## 5. Phased plan

### Phase 0 — Measure & reproduce (low risk, mostly tooled)
- Enable `ROGUESQ_LOG_MESG_TRACE=1` (recomp) + the PJ64 `pj64_rs64_dump.js` osSendMesg/osRecvMesg hooks;
  diff with `tools/validate/compare_mesg_trace.py` over the same cinematic frames.
- Deliverables: (a) is present cadence currently jittery? measure inter-present intervals like the audio
  `[audio-gap]` histogram (add a present-time histogram to the VI thread / `update_screen`). (b) Does a
  long run (>2000 frames) ever stall? (c) The exact message the recomp delivers late/reorders vs hw.
- **Gate:** if present cadence is already smooth and no stall in long runs → Route C may be unnecessary;
  stop and document. If jittery/stall → proceed.

### Phase 1 — Decouple present cadence (mirror the audio win; medium risk)
- Drive frame **present** on the host VI thread's steady 60 Hz (or the game's target), decoupled from the
  cooperative submit round-trip — analogous to the free-running AI clock. Deliver the "buffer free" /
  video-queue release on that fixed schedule so the game's frame loop isn't gated by cooperative latency.
- Files: `events.cpp` (`vi_thread_func`, the video-queue release path), `upstream_compat.cpp`
  (`rs64_vi_driven`, the restored BLOCK recv sites), `renderer_context.cpp` present path.
- **Gate:** if present cadence matches hw and no stall → done (preferred outcome). Else → Phase 2.

### Phase 2 — Bounded preemption at safe points (higher risk)
- Give the interrupt-handler threads (VI/DP retrace handlers) the ability to run **promptly** when they
  have a message for a blocked game thread, instead of only at yields. Since mid-C preemption is unsafe,
  add **cooperative safe-points**: a lightweight "poll for pending high-priority interrupt → yield" at
  function back-edges / entries (N64Recomp can emit these; opt-in, measured overhead). Only the specific
  handlers that break the circular wait need this, not all code.
- Risks: regen fragility (function-boundary hooks), overhead, and re-introducing races. Requires a data-
  race audit of the shared frame/queue state the game touches without locks.

### Phase 3 — Validate & harden
- No stall over >2000 cinematic frames; gate rate ≈ PJ64 golden; audio still 100% (`[audio-rate]`);
  `dl_diff` on the frame DL unchanged; a race check (TSan-style reasoning on the touched queues).
- Keep everything behind env gates (like `ROGUESQ_VI_DRIVEN_LOOP`) so it's revertible.

## 6. Recommendation & decision gates

1. **Do Phase 0 first** — it's cheap, mostly tooled, and may show Route C isn't needed (present cadence may
   already be fine now that VI-driven doesn't deadlock).
2. If needed, **prefer Phase 1 (decouple)** over Phase 2 (preemption) — the audio fix strongly suggests the
   decouple pattern can match hardware cadence without the race risk of true preemption.
3. **Phase 2 only if** decoupling can't deliver smooth cadence — and only with a data-race audit.

## 7. Risks & rollback
- **Data races** from preemption (why the model is cooperative) — the dominant risk; mitigated by keeping
  preemption to specific handlers + safe-points, and by an audit.
- **Regen fragility** if using function-boundary hooks (Phase 2) — prefer host-side / toml-hook forms.
- **Re-introducing the VI-driven deadlock** — every phase stays env-gated and long-run-tested before default.
- Rollback: all changes gated (`ROGUESQ_*`), default off until validated, mirroring the VI-driven rollout.
