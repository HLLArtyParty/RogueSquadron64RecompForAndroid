"""Render a parsed MusyX SONG to WAV offline.

Two instrument modes:
  --tone   : synthesized tone per note (saw+sine with AD envelope). Fast melody validator.
  --sample : pitch-shift a decoded instrument WAV (dumps/snd/wav/) per note (closer to game).

Timing: MIDI ticks -> seconds via --tps (ticks per second; tune by ear). Notes whose
decoded value is out of musical range (>0x7F) are dropped (control/percussion artifacts).

Usage:
  python tools/musyx_render.py dumps/snd/cut_jing1_SNG.bin out.wav [--tps 150] [--tone|--sample s004]
"""
from __future__ import annotations
import sys, struct, wave, math, os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from musyx_song import parse_song, header

SR = 22050
APPLY_TRANSPOSE = "--transpose" in sys.argv   # apply AddNote semitone shift (off: detunes on-key lines)


def env(n_total, atk, rel):
    """Linear attack/release envelope over n_total samples."""
    e = [1.0] * n_total
    for i in range(min(atk, n_total)):
        e[i] = i / atk
    for i in range(min(rel, n_total)):
        e[n_total - 1 - i] *= i / rel
    return e


def tone(note, dur_s, vel):
    n = max(1, int(dur_s * SR))
    f = 440.0 * 2 ** ((note - 69) / 12.0)
    a = vel / 127.0 * 0.25
    e = env(n, int(0.005 * SR), int(min(0.08, dur_s * 0.5) * SR))
    out = [0.0] * n
    for i in range(n):
        t = i / SR
        ph = f * t
        saw = 2.0 * (ph - math.floor(ph + 0.5))
        s = 0.6 * saw + 0.4 * math.sin(2 * math.pi * ph)
        out[i] = s * a * e[i]
    return out


def load_wav(path):
    w = wave.open(path, "rb"); n = w.getnframes()
    pcm = struct.unpack(f"<{n}h", w.readframes(n)); w.close()
    return [s / 32768.0 for s in pcm]


def sample_note(samp, note, dur_s, vel, base=60):
    ratio = 2 ** ((note - base) / 12.0)
    n = max(1, int(dur_s * SR))
    a = (vel / 127.0) ** VEL_POW * 0.9
    e = env(n, int(0.004 * SR), int(min(0.05, dur_s * 0.5) * SR))
    out = [0.0] * n
    L = len(samp)
    for i in range(n):
        sp = i * ratio
        si = int(sp)
        if si + 1 >= L:
            break
        frac = sp - si
        out[i] = (samp[si] * (1 - frac) + samp[si + 1] * frac) * a * e[i]
    return out


def adsr_env(n, attack_n, decay_n, sustain, release_n):
    """Attack -> decay-to-sustain -> sustain -> release envelope over n samples."""
    e = [sustain] * n
    for i in range(min(attack_n, n)):
        e[i] = i / attack_n
    for i in range(min(decay_n, max(0, n - attack_n))):
        e[attack_n + i] = 1.0 - (1.0 - sustain) * (i / decay_n)
    rs = min(release_n, n)
    for i in range(rs):
        e[n - 1 - i] *= i / rs
    return e


def real_note(inst, samp, note, dur_s, vel, drum=False):
    """Render a note from a macro instrument: start-offset + root pitch + loop + ADSR.
    GM: drum channel (9) is NOT melodically pitched — play near native pitch so the rhythm
    section reads as percussion instead of a pitched loop."""
    n = max(1, int(dur_s * SR))
    if drum:
        step = (inst["rate"] / SR)                  # native pitch, no melodic note shift
    else:
        step = (inst["rate"] / SR) * 2 ** ((note - inst["root"]) / 12.0)
    a = (vel / 127.0) ** VEL_POW * 0.9
    # Sample-natural envelope: fast attack (preserve the sample's own attack transient),
    # sustain ~1.0 (the sample's amplitude carries the shape), short release to avoid clicks.
    # The ADSR table's attack only lengthens attack for genuinely slow patches (value capped low).
    atk = max(2, min(25, inst["attack"] // 12)) * SR // 1000
    rel = max(30, min(180, inst["release"] // 16 or 60)) * SR // 1000
    e = adsr_env(n, int(atk), 1, 1.0, int(rel))
    out = [0.0] * n
    L = len(samp)
    ls, ll = inst["loopStart"], inst["loopLen"]
    looped = ll > 16 and ls + ll <= L
    le = ls + ll
    xf = min(ll // 4, 128) if looped and ls - 128 >= 0 else 0   # loop-boundary crossfade width
    pos = float(inst["start"]) if inst["start"] < L else 0.0
    # Vibrato (op 0x1c): pitch LFO. Without it a sustained sample reads as a static brass stab;
    # with it, it sings like a string/violin. depth ~ semitone fraction, rate ~5.5 Hz (musical).
    vib = (not drum) and inst.get("vib_depth", 0) > 0
    vib_semi = min(0.5, inst.get("vib_depth", 0) / 48.0) if vib else 0.0  # depth 0x0f -> ~0.31 semitone
    vib_w = 2 * math.pi * 5.5 / SR
    for i in range(n):
        if vib:
            step_i = step * 2 ** (vib_semi * math.sin(vib_w * i) / 12.0)
        else:
            step_i = step
        si = int(pos)
        if si + 1 >= L:
            if looped:
                pos = ls + ((pos - ls) % ll); si = int(pos)
                if si + 1 >= L:
                    break
            else:
                break
        frac = pos - si
        s0 = samp[si] * (1 - frac) + samp[si + 1] * frac
        # Loop-boundary crossfade: in the loop's tail, blend toward the pre-loop region so the
        # wrap to loopStart is seamless (kills the buzzy click that makes a string read as brass).
        if xf and ls <= si < le and si >= le - xf:
            w = (si - (le - xf)) / xf
            b = si - ll
            s0 = s0 * (1 - w) + (samp[b] * (1 - frac) + samp[b + 1] * frac) * w
        out[i] = s0 * a * e[i]
        pos += step_i
    return out


MACRO_TICK_S = float(next((sys.argv[i + 1] for i, a in enumerate(sys.argv) if a == "--mtick"),
                          0.0007))   # seconds per macro WaitTick; scales Envelope/decay timing
VEL_POW = float(next((sys.argv[i + 1] for i, a in enumerate(sys.argv) if a == "--velpow"), 2.0))
# velocity->amplitude curve (MusyX is not linear): a soft tremolo (vel 24) should be much quieter
# than its loud peak (vel 127). Linear (1.0) made the dense ch9 roll too pronounced.


def vol_at(contour, tick):
    """Linear-interpolate the macro volume contour [(tick, vol), ...] at `tick`; hold ends."""
    if not contour:
        return 1.0
    if tick <= contour[0][0]:
        return contour[0][1]
    for (t0, v0), (t1, v1) in zip(contour, contour[1:]):
        if tick <= t1:
            return v0 if t1 == t0 else v0 + (v1 - v0) * (tick - t0) / (t1 - t0)
    return contour[-1][1]


def render_phase(samp, phase, note, n, vel, drum, hold, ltrans=0):
    """Render one macro sample phase into `n` output samples, applying the phase's volume
    contour (Envelope/ScaleVol over WaitTicks). `hold` loops the sample to fill the window."""
    L = len(samp)
    if drum:
        step = phase["rate"] / SR
    else:
        # NOTE: macro SetNote (0x19) is a DEFAULT note the sequencer's note-on overrides for
        # note-triggered (melodic) voices, so it must NOT pin the pitch (ch1 of roguetheme has 31
        # distinct pitches). The MIDI note wins; SetNote/fixed_note is intentionally ignored here.
        # Layer transpose IS applied (a high key-range reusing a low-root sample needs it, e.g.
        # ch14's -48 keeps high notes from screeching). The macro AddNote transpose is NOT applied
        # by default (it detuned on-key lines); enable with --transpose to A/B.
        key = note + ltrans + (phase["transpose"] if APPLY_TRANSPOSE else 0)
        step = (phase["rate"] / SR) * 2 ** ((key - phase["root"]) / 12.0)
    a = (vel / 127.0) ** VEL_POW * 0.9
    # Full ADSR amplitude envelope from the SetAdsr table (ms times, sustain 0..1), applied on top
    # of the macro volume contour. attack ramps in, decay falls to the sustain level, then hold.
    adsr_scale = float(next((sys.argv[i + 1] for i, a in enumerate(sys.argv) if a == "--adsr"), 1.0))
    atk_n = max(2, int(phase.get("attack", 0) * adsr_scale * SR / 1000))
    dec_n = int(phase.get("decay", 0) * adsr_scale * SR / 1000)
    sus = phase.get("sus_level", 1.0)
    contour = phase["contour"]
    ls, ll = phase["loopStart"], phase["loopLen"]
    looped = hold and ll > 16 and ls + ll <= L
    le = ls + ll
    xf = min(ll // 4, 128) if looped and ls - 128 >= 0 else 0
    pos = float(phase["start"]) if phase["start"] < L else 0.0
    out = [0.0] * n
    for i in range(n):
        si = int(pos)
        if si + 1 >= L:
            if looped:
                pos = ls + ((pos - ls) % ll); si = int(pos)
                if si + 1 >= L:
                    break
            else:
                break
        frac = pos - si
        s0 = samp[si] * (1 - frac) + samp[si + 1] * frac
        if xf and ls <= si < le and si >= le - xf:
            w = (si - (le - xf)) / xf
            b = si - ll
            s0 = s0 * (1 - w) + (samp[b] * (1 - frac) + samp[b + 1] * frac) * w
        g = vol_at(contour, (i / SR) / MACRO_TICK_S)    # macro volume envelope at this time
        if i < atk_n:                                   # ADSR attack
            g *= i / atk_n
        elif dec_n and i < atk_n + dec_n:               # ADSR decay -> sustain
            g *= 1.0 - (1.0 - sus) * ((i - atk_n) / dec_n)
        else:                                           # ADSR sustain hold
            g *= sus
        out[i] = s0 * a * g
        pos += step
    return out


def render_macro_note(prog, getwav, note, dur_s, vel, drum=False, ltrans=0):
    """Play a SoundMacro program: each sample phase rendered with its volume contour. A phase
    that hit a key-off wait is held for the MIDI-note duration; a fixed-timeline phase plays for
    its own contour length (its Envelope shapes the decay); tails play once after the body."""
    n_body = max(1, int(dur_s * SR))
    # Release fade at note-off: use the body phase's decoded ADSR release time (ms) when present,
    # clamped so a long ring-out can't muddy fast passages; fall back to a short anti-click fade.
    body_rel = next((p.get("release", 0) for p in prog["phases"]
                     if p["keyoff_tick"] is not None or (p["sustain"] and p["loopLen"] > 16)), 0)
    rel = int(max(0.03, min(0.6, body_rel / 1000.0 if body_rel else 0.10)) * SR)
    out = [0.0] * n_body
    cursor = 0                                          # where the next non-held phase starts
    for p in prog["phases"]:
        samp = getwav(p["index"])
        if not samp:
            continue
        # Held = a sustaining voice: either an explicit key-off wait, or a looping sample on a
        # pre-key-off phase (its huge "wait indefinitely" tick count means "hold until released").
        loops = p["loopLen"] > 16
        held = p["keyoff_tick"] is not None or (p["sustain"] and loops)
        full = max(1, int(max(p["ticks"], 1) * MACRO_TICK_S * SR))
        gated = False
        if held:                                        # sustaining body: note duration + release ring-out
            n, base = n_body + rel, 0
        elif p["sustain"]:                              # fixed one-shot BODY: gate to note duration
            # Without this a fast roll (e.g. a timpani E/F tremolo) overlaps each note's full 2 s
            # decay into a sustained pitched drone. Note-off cuts it; long notes still ring.
            n, base, gated = min(full, n_body + rel), 0, True
        else:                                           # release tail: plays after body, full decay
            n, base = full, cursor
        v = render_phase(samp, p, note, n, vel, drum, hold=held, ltrans=ltrans)
        if (gated or held) and rel and len(v) > rel:    # release fade at the (gated/held) note-off
            for k in range(rel):
                v[len(v) - 1 - k] *= k / rel
        if base + len(v) > len(out):
            out += [0.0] * (base + len(v) - len(out))
        for i, s in enumerate(v):
            out[base + i] += s
        if not p["sustain"]:
            cursor = max(cursor, base + len(v))
        else:
            cursor = max(cursor, n_body, len(v))
    return out


def add_reverb(buf, wet=0.32):
    """Schroeder-ish reverb: parallel comb filters + series allpass. Adds orchestral space."""
    out = list(buf)
    combs = [(int(0.0297 * SR), 0.78), (int(0.0371 * SR), 0.80),
             (int(0.0411 * SR), 0.82), (int(0.0437 * SR), 0.84)]
    rev = [0.0] * len(buf)
    for delay, fb in combs:
        cb = [0.0] * delay
        idx = 0
        for i in range(len(buf)):
            y = cb[idx]
            cb[idx] = buf[i] + y * fb
            rev[i] += y
            idx += 1
            if idx >= delay:
                idx = 0
    for delay, g in [(int(0.005 * SR), 0.7), (int(0.0017 * SR), 0.7)]:
        ab = [0.0] * delay
        idx = 0
        for i in range(len(rev)):
            x = rev[i]; y = ab[idx]
            ab[idx] = x + y * g
            rev[i] = y - x * g
            idx += 1
            if idx >= delay:
                idx = 0
    for i in range(len(out)):
        out[i] += rev[i] * (wet / len(combs))
    return out


def reverb_tail(buf):
    """The WET reverb signal of buf (no dry) for stereo aux sends: parallel combs + series allpass."""
    combs = [(int(0.0297 * SR), 0.78), (int(0.0371 * SR), 0.80),
             (int(0.0411 * SR), 0.82), (int(0.0437 * SR), 0.84)]
    rev = [0.0] * len(buf)
    for delay, fb in combs:
        cb = [0.0] * delay; idx = 0
        for i in range(len(buf)):
            y = cb[idx]; cb[idx] = buf[i] + y * fb; rev[i] += y
            idx += 1
            if idx >= delay:
                idx = 0
    for delay, g in [(int(0.005 * SR), 0.7), (int(0.0017 * SR), 0.7)]:
        ab = [0.0] * delay; idx = 0
        for i in range(len(rev)):
            x = rev[i]; y = ab[idx]; ab[idx] = x + y * g; rev[i] = y - x * g
            idx += 1
            if idx >= delay:
                idx = 0
    return [r / len(combs) for r in rev]


def build_tick2sec(d, base_tps):
    """tick -> seconds following the song's TEMPO TABLE (variable tempo). The song header's tempo
    table is (tick:u32, bpm:u32) pairs (0xFFFFFFFF-term). base_tps (the --tps value, tuned by ear)
    is the tick rate at the FIRST tempo; other tempos scale proportionally, so the song speeds up /
    slows down where the game does instead of running at one fixed rate."""
    import struct as _s
    o = header(d)["tempoTbl"]
    tbl = []
    while o + 8 <= len(d):
        tk, bpm = _s.unpack_from(">II", d, o); o += 8
        if tk == 0xFFFFFFFF or bpm == 0:
            break
        tbl.append((tk, bpm))
    if not tbl:
        return lambda tk: tk / base_tps
    ref = tbl[0][1]
    segs, sec = [], 0.0                          # (tick, sec_at_tick, tps_in_seg)
    for i, (tk, bpm) in enumerate(tbl):
        if i > 0:
            sec += (tk - tbl[i - 1][0]) / segs[-1][2]
        segs.append((tk, sec, base_tps * bpm / ref))

    def t2s(tk):
        seg = segs[0]
        for s in segs:
            if s[0] <= tk:
                seg = s
            else:
                break
        return seg[1] + (tk - seg[0]) / seg[2]
    return t2s


def render_real(path, outwav, tps):
    """Faithful render: each track -> SoundMacro (via chanMap program) -> its real sample,
    root-note pitched. Falls back to a distinct tonal sample if the macro binding is missing."""
    import glob
    from musyx_group import (parse_sdir, parse_pool, macro_instrument, macro_program,
                             parse_midisetups, parse_midisetups_all, group_macrolist,
                             group_subseg5, resolve_program_macro)
    sdir = parse_sdir(open("dumps/snd/sdir_SND.bin", "rb").read())
    by_index = {v["index"]: (sid, v) for sid, v in sdir.items()}
    pool = parse_pool(open("dumps/snd/pool_SND.bin", "rb").read())
    proj_bytes = open("dumps/snd/proj_SND.bin", "rb").read()
    all_setups = parse_midisetups_all(proj_bytes)   # group -> [setup0, setup1, ...] (per song)
    # Resolve THIS song's (group, setup-index): each song uses a specific MIDISetup, not just the
    # group's first one. Auto-resolve from the SNG filename via the song->binding (so logo1 gets
    # group 0x20 sid 1, not the wrong default). Override with setup=0xNN and/or sid=N.
    song_name = os.path.basename(path).replace("_SNG.bin", "").replace("_SNG", "")
    setup_grp = setup_sid = None
    if not any(a.startswith("setup=") for a in sys.argv):
        try:
            from musyx_song_binding import build_binding
            binding, _, _ = build_binding(open("rogue_squadron.z64", "rb").read(), proj_bytes)
            if song_name in binding:
                setup_grp, setup_sid = binding[song_name]
        except Exception as e:
            print(f"(binding unavailable: {e})")
    if setup_grp is None:
        setup_grp = int(next((a for a in sys.argv if a.startswith("setup=")), "setup=0x20")[6:], 0)
    if setup_sid is None:
        setup_sid = int(next((a for a in sys.argv if a.startswith("sid=")), "sid=0")[4:], 0)
    grp_setups = all_setups.get(setup_grp, [[0] * 16])
    setup = list(grp_setups[setup_sid] if setup_sid < len(grp_setups) else grp_setups[0])
    setup_id = setup_grp                             # group id used for subseg5/Layer resolution
    print(f"song '{song_name}': MIDISetup group {setup_grp:#x} sid {setup_sid}")
    # --idxmode: treat the MIDISetup program byte as an INDEX into the group's macro list
    # (subseg0), not a direct macro id. (A/B test of the resolution semantics.)
    if "--idxmode" in sys.argv:
        ml = group_macrolist(proj_bytes, setup_id)
        setup = [ml[p] if p < len(ml) else 0 for p in setup]
        print(f"idxmode: resolved via group {setup_id:#x} macrolist (len {len(ml)})")
    if "--flip" in sys.argv:                         # reverse channel->setup index (ch -> setup[15-ch])
        setup = setup[::-1]
        print("flip: channel->setup index reversed")
    # Per-channel macro override: CLI args like "ch12=0x50" remap one channel's instrument
    # (e.g. fix ch12 from the buzzy-brass macro 0x70 to a long-loop string macro). Repeatable.
    for a in sys.argv:
        if a.startswith("ch") and "=" in a and a[2:a.index("=")].isdigit():
            c = int(a[2:a.index("=")]); m = int(a[a.index("=") + 1:], 0)
            if 0 <= c < 16:
                setup[c] = m
    print(f"using MIDISetup proj#{setup_id:#x}: {[hex(m) for m in setup]}")
    wavs = {}
    for f in glob.glob("dumps/snd/wav/s[0-9][0-9][0-9]_*.wav"):
        idx = int(os.path.basename(f)[1:4]); wavs[idx] = f
    _cache = {}
    def getwav(idx):
        if idx not in _cache:
            _cache[idx] = load_wav(wavs[idx]) if idx in wavs else None
        return _cache[idx]

    # Per-note instrument resolution: program (MIDISetup) -> subseg5 -> Layer -> note-range -> macro.
    # This key-split is the real MusyX resolution (multisampled instruments); a single macro per
    # channel was the core bug behind "wrong instruments". --nolayer falls back to direct macro.
    sub5 = group_subseg5(proj_bytes, setup_id)
    layer_cache, inst_cache, prog_cache = {}, {}, {}
    use_layer = "--nolayer" not in sys.argv
    # --firstsample: old behavior (first StartSample only). Default now walks the macro program
    # (sustaining body + release tails + AddNote transpose) for faithful per-note timbre.
    use_prog = "--firstsample" not in sys.argv
    def get_inst(macro):
        if macro not in inst_cache:
            inst = macro_instrument(pool, sdir, macro)
            inst_cache[macro] = (inst, getwav(inst["index"]) if inst else None)
        return inst_cache[macro]
    use_vm = "--vm" in sys.argv      # faithful stepping VM (control flow / GoSub) vs old linear walk
    if use_vm:
        from musyx_vm import run_macro
    def get_prog(macro):
        if macro not in prog_cache:
            if use_vm:
                ph, base = run_macro(pool, sdir, macro, MACRO_TICK_S)
                prog_cache[macro] = {"phases": ph, "transpose": base} if ph else None
            else:
                prog_cache[macro] = macro_program(pool, sdir, macro)
        return prog_cache[macro]

    d = open(path, "rb").read()
    hdr, tracks, _ = parse_song(d, include_flagged="--withflagged" in sys.argv)
    events = []
    end_tick = 0
    # --mute 5,8 = drop those channels; --only 8 = render only that channel (diagnostic isolation).
    mute = {int(x) for a in sys.argv if a == "--mute"
            for x in sys.argv[sys.argv.index(a) + 1].split(",")} \
        if "--mute" in sys.argv else set()
    only = {int(x) for a in sys.argv if a == "--only"
            for x in sys.argv[sys.argv.index(a) + 1].split(",")} \
        if "--only" in sys.argv else None
    # Per-channel sample override "ch9samp=157": force a channel to play a specific sample index
    # (bypasses macro resolution) — to audition a different instrument when the data's choice is
    # judged wrong. Built as a synthetic 1-phase held program at the sample's root pitch.
    samp_override = {}
    for a in sys.argv:
        if a.startswith("ch") and "samp=" in a and a[2:a.index("samp=")].isdigit():
            samp_override[int(a[2:a.index("samp=")])] = int(a[a.index("samp=") + 5:])

    def synth_prog(samp_idx):
        if samp_idx not in by_index:
            return None
        _, sd = by_index[samp_idx]
        return {"transpose": 0, "phases": [{
            "sample_id": -1, "index": samp_idx, "root": sd["root"], "rate": sd["rate"],
            "start": 0, "loopStart": sd["loopStart"], "loopLen": sd["loopLen"],
            "attack": 0, "decay": 0, "transpose": 0, "sustain": True,
            "contour": [(0, 1.0)], "keyoff_tick": 0, "ticks": 0}]}

    for ti, t in enumerate(tracks):
        ch = t["chan"]                              # chanMap value = MIDI channel
        if ch in mute or (only is not None and ch not in only):
            continue
        program = setup[ch] if ch < 16 else 0       # MIDISetup: channel -> program byte
        # MusyX is NOT General MIDI: there is no fixed percussion channel. ch9 is a normal pitched
        # instrument (treating it as GM drums played every note at one pitch = a repeated "piano"
        # drone). Percussion comes through ordinary macros/keymaps. --gmdrums forces the old behavior.
        is_drum = (ch == 9) and "--gmdrums" in sys.argv
        ov_prog = synth_prog(samp_override[ch]) if ch in samp_override else None
        for nrec in t["notes"]:
            note = nrec["note"]
            if not (0 < note <= 0x7F):
                continue
            if ov_prog is not None:                     # channel forced to a specific sample
                events.append((nrec, ov_prog, 0, is_drum, ch))
                end_tick = max(end_tick, nrec["tick"] + nrec["dur"])
                continue
            if use_layer:
                macro, ltrans = resolve_program_macro(pool, proj_bytes, sub5, layer_cache,
                                                       program, note)
            else:
                macro, ltrans = program, 0
            if macro is None:
                continue
            if use_prog:
                prog = get_prog(macro)
                if prog is None:
                    continue
                events.append((nrec, prog, ltrans, is_drum, ch))
            else:
                inst, samp = get_inst(macro)
                if inst is None or not samp:
                    continue
                events.append((nrec, samp, inst, is_drum, ch))
            end_tick = max(end_tick, nrec["tick"] + nrec["dur"])
    t2s = build_tick2sec(d, tps)                        # variable-tempo tick -> seconds
    total = int(t2s(end_tick) * SR) + 2 * SR
    print(f"{len(events)} notes over {len(tracks)} tracks, end {end_tick} -> {total/SR:.1f}s "
          f"(tempo-table timing)")
    # Render each note to its own buffer first; placement/voice-stealing happens after.
    voices = []                                         # (ch, t0, [samples])
    for nrec, a2, a3, is_drum, ch in events:
        t0 = int(t2s(nrec["tick"]) * SR)
        dur_s = max(0.05, t2s(nrec["tick"] + nrec["dur"]) - t2s(nrec["tick"]))
        if use_prog:
            v = render_macro_note(a2, getwav, nrec["note"], dur_s, nrec["vel"],
                                  drum=is_drum, ltrans=a3)
        else:
            v = real_note(a3, a2, nrec["note"], dur_s, nrec["vel"], drum=is_drum)
        voices.append([ch, t0, v])
    # Per-channel voice cap: MusyX has a finite voice pool, so rapidly re-triggered overlapping
    # notes on a channel steal the oldest voice (keeps a fast staccato bass from washing into a
    # sustained "piano" drone). Spaced-out hits (drums) never hit the cap, so they ring freely.
    cap = int(next((sys.argv[i + 1] for i, a in enumerate(sys.argv) if a == "--voices"), 3))
    xfade = int(0.012 * SR)
    by_ch = {}
    for v in voices:
        by_ch.setdefault(v[0], []).append(v)
    for ch, vs in by_ch.items():
        vs.sort(key=lambda x: x[1])
        active = []                                     # (end, voice) currently sounding
        for v in vs:
            s = v[1]
            active = [(e, av) for (e, av) in active if e > s]
            if len(active) >= cap:                      # steal oldest: fade its tail from s
                active.sort(key=lambda x: x[1][1])
                _, victim = active.pop(0)
                cut = s - victim[1]                     # offset within victim where the new note starts
                buf_v = victim[2]
                for k in range(xfade):
                    if cut + k < len(buf_v):
                        buf_v[cut + k] *= 1 - k / xfade
                del buf_v[cut + xfade:]
            active.append((v[1] + len(v[2]), v))
    # Per-channel mix from the MIDISetup (byte1 vol, byte2 PAN, byte3 REVERB send): the render
    # was mono + dry; the game pans instruments across the field and sends each to a reverb bus.
    from musyx_group import parse_midisetups_full
    fs_all = parse_midisetups_full(proj_bytes).get(setup_grp, [])
    fs = fs_all[setup_sid] if setup_sid < len(fs_all) else [{} for _ in range(16)]
    def chmix(c):
        e = fs[c] if c < len(fs) else {}
        return e.get("pan", 64), e.get("reverb", 0), e.get("vol", 127)
    L = [0.0] * total; R = [0.0] * total; rev_bus = [0.0] * total
    for ch, t0, v in voices:
        pan, rev, vol = chmix(ch)
        pa = max(0, min(127, pan)) / 127.0 * (math.pi / 2)        # equal-power pan law
        lg = math.cos(pa) * (vol / 127.0); rg = math.sin(pa) * (vol / 127.0)
        rs = rev / 127.0
        for i, smp in enumerate(v):
            j = t0 + i
            if 0 <= j < total:
                L[j] += smp * lg; R[j] += smp * rg
                if rs:
                    rev_bus[j] += smp * rs
    if any(rev_bus):                                              # send-bus reverb -> stereo space
        wet = reverb_tail(rev_bus)
        d = int(0.013 * SR)                                       # small L/R decorrelation for width
        for i in range(total):
            L[i] += wet[i]
            R[i] += wet[i - d] if i - d >= 0 else 0.0
    peak = max(1e-6, max(max((abs(s) for s in L), default=0), max((abs(s) for s in R), default=0)))
    norm = min(1.0, 0.95 / peak)
    inter = []
    for i in range(total):
        inter.append(int(max(-32768, min(32767, L[i] * norm * 32767))))
        inter.append(int(max(-32768, min(32767, R[i] * norm * 32767))))
    pcm = struct.pack(f"<{2 * total}h", *inter)
    with wave.open(outwav, "wb") as w:
        w.setnchannels(2); w.setsampwidth(2); w.setframerate(SR); w.writeframes(pcm)
    print(f"wrote {outwav} (stereo + per-channel pan/reverb)")


def main():
    if "--real" in sys.argv:
        tps = float(next((sys.argv[i + 1] for i, a in enumerate(sys.argv) if a == "--tps"), 1024))
        render_real(sys.argv[1], sys.argv[2], tps)
        return
    path, outwav = sys.argv[1], sys.argv[2]
    tps = float(next((sys.argv[i + 1] for i, a in enumerate(sys.argv) if a == "--tps"), 150))
    use_tone = "--tone" in sys.argv
    samp_id = next((a for a in sys.argv if a.startswith("s") and a[1:4].isdigit()), "s004")

    d = open(path, "rb").read()
    hdr, tracks, _ = parse_song(d)

    samp = None
    if not use_tone:
        import glob
        cand = glob.glob(f"dumps/snd/wav/{samp_id}_*.wav")
        samp = load_wav(cand[0]) if cand else None
        if samp is None:
            print("no sample, falling back to tone"); use_tone = True

    # gather notes
    events = []
    for t in tracks:
        for nrec in t["notes"]:
            if 0 < nrec["note"] <= 0x7F:
                events.append(nrec)
    if not events:
        print("no playable notes"); return
    end_tick = max(e["tick"] + e["dur"] for e in events)
    total = int((end_tick / tps + 0.5) * SR) + SR
    buf = [0.0] * total
    print(f"{len(events)} notes, end_tick={end_tick}, tps={tps} -> {total/SR:.1f}s")

    for e in events:
        t0 = int(e["tick"] / tps * SR)
        dur_s = max(0.05, e["dur"] / tps)
        v = tone(e["note"], dur_s, e["vel"]) if use_tone else sample_note(samp, e["note"], dur_s, e["vel"])
        for i, s in enumerate(v):
            if t0 + i < total:
                buf[t0 + i] += s

    peak = max(1e-6, max(abs(s) for s in buf))
    norm = min(1.0, 0.95 / peak)
    pcm = struct.pack(f"<{total}h", *[int(max(-32768, min(32767, s * norm * 32767))) for s in buf])
    with wave.open(outwav, "wb") as w:
        w.setnchannels(1); w.setsampwidth(2); w.setframerate(SR); w.writeframes(pcm)
    print(f"wrote {outwav} ({total/SR:.1f}s, peak {peak:.2f})")


if __name__ == "__main__":
    main()
