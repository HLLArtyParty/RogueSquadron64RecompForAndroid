"""Faithful MusyX SoundMacro VM (stepping interpreter), ported from the recomp's

tickAudioChannel dispatch (funcs_23: op = cmdword0 & 0x7F, 8 bytes/cmd, 81-case switch).

Unlike the old linear `macro_program` walk, this is a real stepping VM with a program
counter and CONTROL FLOW: op 0x06 (GoSub/Goto -> macro=(w0>>16)&0xFFFF, step=w1) follows
into the referenced macro the way the engine does, so shared/looping instrument code is
expanded instead of truncated at the first key-off.

Command byte layout (big-endian word0,word1 per 8-byte cmd):
  byte0=(w0>>24)&0xFF  byte1=(w0>>16)&0xFF  byte2=(w0>>8)&0xFF  op=w0&0x7F   w1=second word

Output: a list of "phases" (same shape the renderer's render_phase consumes):
  {sample_id,index,root,rate,start,loopStart,loopLen,attack,decay,transpose,sustain,
   contour:[(tick,vol)...],keyoff_tick,ticks}
plus a base transpose. A phase is one StartSample..(StopSample/StartSample/End) span with
its volume contour (ScaleVol/Envelope over WaitTicks/WaitMs) and note transpose accumulated
from AddNote/SetNote. This expands GoSub targets inline (bounded depth) for faithful timbre.
"""
from __future__ import annotations
import struct
from musyx_group import adsr_table

# WaitMs is in milliseconds; convert to macro ticks using the same MACRO_TICK clock the
# renderer uses (passed in), so 0x04 (ticks) and 0x07 (ms) share one timeline.
S8 = lambda v: v - 256 if v > 127 else v


def _cmds(data: bytes):
    out = []
    for i in range(0, len(data) - 7, 8):
        w0, w1 = struct.unpack_from(">II", data, i)
        out.append((w0 & 0x7F, w0, w1))
    return out


def run_macro(pool, sdir, macro_id, mtick_s, max_steps=20000):
    """Step the SoundMacro VM for macro_id. Returns (phases, base_transpose) or (None,0).

    mtick_s = seconds per macro tick (WaitTicks unit); WaitMs uses real ms / mtick_s so both
    waits land on one tick timeline (the renderer scales tick->samples by mtick_s)."""
    if macro_id not in pool[0]:
        return None, 0
    phases = []
    transpose = 0          # accumulated AddNote/SetNote semitone shift (relative to MIDI note)
    set_note = None        # SetNote: absolute pitch override (fixed-pitch / percussion)
    cur = None             # phase being filled
    vol = 1.0              # running voice volume 0..1
    t = 0.0                # macro-tick clock (float ticks) for the current phase
    pending_env = None     # (target_vol) Envelope ramp awaiting its next wait to interpolate
    cur_adsr = None
    vib_depth = vib_period = 0

    # Program counter as (macro_id, cmd_index); a small call stack for GoSub.
    pc = (macro_id, 0)
    stack = []
    prog = {macro_id: _cmds(pool[0][macro_id][2])}
    steps = 0
    visited = set()        # (macro,idx) guard against infinite self-loops

    def get_cmds(mid):
        if mid not in prog:
            prog[mid] = _cmds(pool[0][mid][2]) if mid in pool[0] else []
        return prog[mid]

    def close_phase():
        nonlocal cur
        if cur is not None:
            cur["contour"].append((t, vol))
            cur["ticks"] = t
            phases.append(cur)
            cur = None

    while steps < max_steps:
        steps += 1
        mid, idx = pc
        cmds = get_cmds(mid)
        if idx >= len(cmds):
            op = 0x00
        else:
            op, w0, w1 = cmds[idx]
        b0, b1, b2 = (w0 >> 24) & 0xFF, (w0 >> 16) & 0xFF, (w0 >> 8) & 0xFF
        nxt = (mid, idx + 1)

        if op == 0x00 or op == 0x01:                 # End / Stop
            close_phase()
            if op == 0x00 and stack:                 # End returns from a GoSub
                pc = stack.pop()
                continue
            break
        elif op == 0x10:                             # StartSample
            close_phase()
            t = 0.0
            sid = b1 << 8 | b2
            if sid in sdir:
                sd = sdir[sid]
                adsr = adsr_table(pool, cur_adsr) if cur_adsr else None
                cur = {"sample_id": sid, "index": sd["index"], "root": sd["root"],
                       "rate": sd["rate"], "start": w1 if w1 < sd["num"] else 0,
                       "loopStart": sd["loopStart"], "loopLen": sd["loopLen"],
                       "attack": adsr["attack"] if adsr else 0,
                       "decay": adsr["decay"] if adsr else 0,
                       "sus_level": adsr["sustain"] if adsr else 1.0,
                       "release": adsr["release"] if adsr else 0,
                       "transpose": (set_note - 60 if set_note is not None else transpose),
                       "sustain": True, "contour": [(0.0, vol)], "keyoff_tick": None,
                       "ticks": 0, "vib_depth": vib_depth, "vib_period": vib_period}
            pc = nxt
        elif op == 0x11:                             # StopSample (start release / end body)
            if cur is not None and cur["keyoff_tick"] is None:
                cur["keyoff_tick"] = t
            close_phase()
            t = 0.0
            pc = nxt
        elif op == 0x04:                             # WaitTicks (w1>>16; 0xFFFE/F = key-off)
            amt = w1 >> 16
            if amt >= 0xFFFE:
                if cur is not None and cur["keyoff_tick"] is None:
                    cur["keyoff_tick"] = t
            else:
                t += amt
                if pending_env is not None:
                    vol = pending_env; pending_env = None
                if cur is not None:
                    cur["contour"].append((t, vol))
            pc = nxt
        elif op == 0x07:                             # WaitMs (ms in w1>>16)
            ms = w1 >> 16
            t += (ms / 1000.0) / mtick_s
            if pending_env is not None:
                vol = pending_env; pending_env = None
            if cur is not None:
                cur["contour"].append((t, vol))
            pc = nxt
        elif op == 0x06:                             # GoSub/Goto: macro=(w0>>16), step=w1
            tgt = (w0 >> 16) & 0xFFFF
            key = (tgt, w1)
            if tgt in pool[0] and key not in visited:
                visited.add(key)
                stack.append(nxt)
                pc = (tgt, w1)
                if len(stack) > 16:                  # runaway guard
                    break
            else:
                pc = nxt                             # already taken / missing -> don't loop forever
        elif op == 0x0C:                             # SetAdsr (table id)
            cur_adsr = b1 << 8 | b2
            pc = nxt
        elif op == 0x0D:                             # ScaleVolume: vol = byte2/127
            vol = b2 / 127.0
            if cur is not None:
                cur["contour"].append((t, vol))
            pc = nxt
        elif op == 0x0F:                             # Envelope: ramp to byte2/127 over next wait
            pending_env = b2 / 127.0
            pc = nxt
        elif op == 0x18:                             # AddNote: signed semitone (byte2)
            transpose += S8(b2)
            pc = nxt
        elif op == 0x19:                             # SetNote: absolute note (byte2)
            set_note = b2
            pc = nxt
        elif op == 0x17:                             # RndNote: center (byte0) deterministic
            set_note = b0
            pc = nxt
        elif op == 0x1C:                             # Vibrato: depth byte1, period w1>>16
            vib_depth = b1; vib_period = w1 >> 16
            pc = nxt
        elif op == 0x12:                             # KeyOff
            if cur is not None and cur["keyoff_tick"] is None:
                cur["keyoff_tick"] = t
            pc = nxt
        else:                                        # 0x03 split, 0x0e pan, 0x28, 0x30+ events
            pc = nxt                                 # no audio effect for mono render -> skip
        if pc[0] not in pool[0]:
            break
    close_phase()
    base = set_note - 60 if set_note is not None else transpose
    return (phases, base) if phases else (None, 0)


if __name__ == "__main__":
    import os, sys
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    from musyx_group import parse_pool, parse_sdir
    D = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "dumps", "snd")
    pool = parse_pool(open(os.path.join(D, "pool_SND.bin"), "rb").read())
    sdir = parse_sdir(open(os.path.join(D, "sdir_SND.bin"), "rb").read())
    mid = int(sys.argv[1], 0) if len(sys.argv) > 1 else 0x07
    phases, base = run_macro(pool, sdir, mid, 0.005)
    print(f"macro {mid:#06x}: base_transpose={base}, {len(phases or [])} phases")
    for p in phases or []:
        print(f"  sample {p['sample_id']:#06x} idx{p['index']} root{p['root']} "
              f"tr{p['transpose']} sustain={p['sustain']} keyoff={p['keyoff_tick']} "
              f"ticks={p['ticks']:.0f} contour={[(round(x,1),round(y,2)) for x,y in p['contour'][:6]]}")
