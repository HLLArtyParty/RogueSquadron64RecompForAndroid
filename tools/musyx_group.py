"""Parse the MusyX AudioGroup files (proj/pool/sdir) to resolve program -> sample.

Formats per sister-repo docs/snd_files (snd_file_dump.c):
  sdir_SND: 0x18-byte entries {id:u16, +0x04 sampOff:u32, +0x0A rate:u16, +0x0C low24 numSamples}.
  proj_SND: sections [size:u32, id:u16, type:u16, subOffs...]. Common sub-segments 0..4 are arrays
            of u16 ids (0xFFFF-term); sub-seg 1 references sdir sample ids. type0=8 offs, type1=6.
  pool_SND: 4 sections (offsets in first 4 words); each = subsections [size:u32, id:u16, pad] then data.

We only need program -> sample-id(s): a proj section's id is the program; its sub-segment 1 lists
the sdir sample ids that program plays.
"""
from __future__ import annotations
import struct, os

D = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "dumps", "snd")


def _u16(b, o): return struct.unpack_from(">H", b, o)[0]
def _u32(b, o): return struct.unpack_from(">I", b, o)[0]


def parse_sdir(b: bytes):
    """id -> {off, rate, num, index}."""
    out, o, idx = {}, 0, 0
    while o + 0x18 <= len(b):
        sid = _u16(b, o)
        if sid == 0xFFFF:
            break
        out[sid] = {"off": _u32(b, o + 0x04), "root": b[o + 0x08],
                    "rate": _u16(b, o + 0x0A), "num": _u32(b, o + 0x0C) & 0xFFFFFF,
                    "loopStart": _u32(b, o + 0x10), "loopLen": _u32(b, o + 0x14),
                    "index": idx}
        o += 0x18; idx += 1
    return out


def _read_u16_array(b, o):
    out = []
    while o + 2 <= len(b):
        v = _u16(b, o); o += 2
        if v == 0xFFFF:
            break
        out.append(v)
    return out


def parse_proj(b: bytes):
    """program id -> {type, samples:[sdir ids], pool0:[...], pool1, pool2, pool3}."""
    progs, off = {}, 0
    while off + 8 <= len(b):
        size = _u32(b, off)
        if size == 0xFFFFFFFF or size == 0:
            break
        pid = _u16(b, off + 4)
        ptype = _u16(b, off + 6)
        seg_base = off + 8
        names = ["pool0", "samples", "pool1", "pool2", "pool3"]
        rec = {"type": ptype}
        for i in range(5):
            rel = _u32(b, seg_base + 4 * i)
            rec[names[i]] = _read_u16_array(b, seg_base + rel)
        progs[pid] = rec
        off += size
    return progs


def parse_pool(b: bytes):
    """4 sections (offsets in first 4 words). Each = subsections [size:u32, id:u16, pad] + data.
    Returns list of 4 sections, each a dict {id -> (offset, size, data_bytes)}."""
    sections = []
    for i in range(4):
        soff = _u32(b, i * 4)
        sub = {}
        o = soff
        while o + 8 <= len(b):
            size = _u32(b, o)
            if size == 0xFFFFFFFF or size == 0:
                break
            sid = _u16(b, o + 4)
            data = b[o + 8: o + size]
            sub[sid] = (o, size, data)
            o += size
        sections.append(sub)
    return sections


def parse_midisetups(b: bytes):
    """proj group id -> [16 channel programs] for the FIRST MIDIsetup (back-compat).
    Use parse_midisetups_all for the full per-song (sid) array."""
    return {g: ss[0] for g, ss in parse_midisetups_all(b).items() if ss}


def parse_midisetups_all(b: bytes):
    """proj GROUP id (sgid) -> LIST of MIDIsetups, one per song (sid) in the group.
    Each setup = [16 channel programs] (channel->program byte0). MusyX SongGroups hold one
    MIDIsetup per referenced song (manual: snd_play(sgid, sid); sid indexes this list). Setups
    are 132-byte stride at subseg7 + 4 + sid*132, 16 channels x 8 bytes (program at byte0)."""
    off, groups = 0, {}
    while off + 8 <= len(b):
        size = _u32(b, off)
        if size in (0, 0xFFFFFFFF):
            break
        sid, typ = _u16(b, off + 4), _u16(b, off + 6)
        if typ == 0:
            so = off + 8
            s7 = so + _u32(b, so + 4 * 7)
            o7 = s7 + 4
            # setups run from o7 to the segment end (132B each: 16ch*8B = 128 + 4 header/pad).
            n = max(1, (off + size - o7) // 132)
            groups[sid] = [[b[o7 + k * 132 + ch * 8] for ch in range(16)] for k in range(n)]
        off += size
    return groups


def parse_midisetups_full(b: bytes):
    """group -> [ per-song-setup [16 channels of {prog,vol,pan,reverb}] ]. The 8-byte channel
    entry is {byte0 program, byte1 volume 0..127, byte2 PAN (0x40=center, <left >right),
    byte3 REVERB send 0..127}. The render needs pan+reverb for the real stereo image + space."""
    off, groups = 0, {}
    while off + 8 <= len(b):
        size = _u32(b, off)
        if size in (0, 0xFFFFFFFF):
            break
        sid, typ = _u16(b, off + 4), _u16(b, off + 6)
        if typ == 0:
            so = off + 8
            o7 = so + _u32(b, so + 4 * 7) + 4
            n = max(1, (off + size - o7) // 132)
            groups[sid] = [[{"prog": b[o7 + k * 132 + ch * 8], "vol": b[o7 + k * 132 + ch * 8 + 1],
                             "pan": b[o7 + k * 132 + ch * 8 + 2], "reverb": b[o7 + k * 132 + ch * 8 + 3]}
                            for ch in range(16)] for k in range(n)]
        off += size
    return groups


def group_macrolist(proj_bytes: bytes, group_id: int):
    """A proj group's subseg0 = the pool0 (SoundMacro) ids it loads. The per-channel MIDISetup
    program byte may index into THIS list rather than being a direct macro id."""
    off = 0
    while off + 8 <= len(proj_bytes):
        size = _u32(proj_bytes, off)
        if size in (0, 0xFFFFFFFF):
            break
        if _u16(proj_bytes, off + 4) == group_id and _u16(proj_bytes, off + 6) == 0:
            so = off + 8
            return _read_u16_array(proj_bytes, so + _u32(proj_bytes, so))
        off += size
    return []


def group_subseg5(proj_bytes: bytes, group_id: int):
    """proj type0 sub-segment 5: program(byte5) -> unk00. For program instruments, unk00's high byte
    0x80 means it references a Layer (pool sec3 id = unk00>>16) that key-splits notes to macros."""
    off = 0
    while off + 8 <= len(proj_bytes):
        size = _u32(proj_bytes, off)
        if size in (0, 0xFFFFFFFF):
            break
        if _u16(proj_bytes, off + 4) == group_id and _u16(proj_bytes, off + 6) == 0:
            so = off + 8
            o = so + _u32(proj_bytes, so + 4 * 5)
            out = {}
            while o + 8 <= len(proj_bytes):
                unk00 = _u32(proj_bytes, o); unk05 = proj_bytes[o + 5]
                if unk00 == 0xFFFFFFFF and proj_bytes[o + 4] == 0xFF and unk05 == 0xFF:
                    break
                out[unk05] = unk00
                o += 8
            return out
        off += size
    return {}


def parse_layer(pool, layer_id: int):
    """Pool sec3 Layer: [u32 count][count entries of 12B]. Entry: [u16 macro][u8 keyLo][u8 keyHi]
    [s8 transpose][u8 volume]... Maps a note range to a SoundMacro (key-split / multisample);
    the per-entry transpose shifts the key (e.g. a high range reusing a low-root sample)."""
    if layer_id not in pool[3]:
        return []
    _, _, data = pool[3][layer_id]
    cnt = _u32(data, 0)
    out, p = [], 4
    for _ in range(cnt):
        if p + 12 > len(data):
            break
        macro = _u16(data, p); keyLo = data[p + 2]; keyHi = data[p + 3]
        tr = data[p + 4]
        if macro != 0xFFFF:
            out.append((macro, keyLo, keyHi, tr - 256 if tr > 127 else tr))
        p += 12
    return out


def resolve_program_macro(pool, proj_bytes, sub5, layer_cache, program, note):
    """program + note -> (macro id, layer transpose), via subseg5 (program->layer) then the layer's
    note-range split. Falls back to (program, 0) as a direct macro id if there's no layer mapping."""
    unk00 = sub5.get(program)
    if unk00 is None:
        return program, 0
    if (unk00 >> 24) == 0x80:                       # Layer reference
        lid = unk00 >> 16
        if lid not in layer_cache:
            layer_cache[lid] = parse_layer(pool, lid)
        entries = layer_cache[lid]
        for macro, lo, hi, tr in entries:
            if lo <= note <= hi:
                return macro, tr
        if entries:                                 # out of all ranges: clamp to nearest split
            macro, lo, hi, tr = min(
                entries, key=lambda e: 0 if e[1] <= note <= e[2] else min(abs(note - e[1]),
                                                                          abs(note - e[2])))
            return macro, tr
        return None, 0
    return program, 0                               # non-layer program: direct macro id


def parse_soundmacro(data: bytes):
    """Decode a SoundMacro command list. op = low byte of word0; each cmd 8 bytes.
    Returns (cmds, info) where info has the first StartSample + ADSR table + release."""
    cmds = []
    info = {"sample": None, "offset": 0, "key": 0, "adsr": None, "release": 0,
            "vib_depth": 0, "vib_period": 0}
    for i in range(0, len(data) - 7, 8):
        w0, w1 = struct.unpack_from(">II", data, i)
        op = w0 & 0xFF
        cmds.append({"op": op, "w0": w0, "w1": w1})
        if op == 0x10 and info["sample"] is None:   # StartSample: sample id + start offset
            info["sample"] = (w0 >> 8) & 0xFFFF
            info["offset"] = w1
            info["key"] = (w0 >> 24) & 0xFF
        elif op == 0x0C:                            # SetAdsr: ADSR table id
            info["adsr"] = (w0 >> 8) & 0xFFFF
        elif op == 0x0F:                            # Envelope: release-ish time in w1 high u16
            info["release"] = w1 >> 16
        elif op == 0x1C:                            # Vibrato: pitch LFO (strings). depth + period.
            info["vib_depth"] = (w0 >> 16) & 0xFF
            info["vib_period"] = (w1 >> 16) & 0xFFFF
        if op == 0x00:
            break
    return cmds, info


def adsr_table(pool, tid):
    """ADSR table (pool sec1, little-endian 4x u16): attack, decay, sustain, release.
    attack/decay/release = times in ms; sustain = level 0..0x1000 (0x1000 = full).
    release >= 0x8000 (e.g. the 0xDBDA sentinel) means 'no explicit release' -> 0."""
    if tid not in pool[1]:
        return None
    _, _, data = pool[1][tid]
    a, d, s, r = struct.unpack_from("<HHHH", data, 0)
    return {"attack": a, "decay": d, "sustain": min(1.0, s / 4096.0),
            "release": r if r < 0x8000 else 0}


def macro_instrument(pool, sdir, macro_id):
    """Resolve a SoundMacro to a playable instrument: sample + offset + loop + envelope."""
    if macro_id not in pool[0]:
        return None
    _, _, data = pool[0][macro_id]
    _, info = parse_soundmacro(data)
    sid = info["sample"]
    if sid not in sdir:
        return None
    sd = sdir[sid]
    adsr = adsr_table(pool, info["adsr"]) if info["adsr"] else None
    return {"sample_id": sid, "index": sd["index"], "root": sd["root"], "rate": sd["rate"],
            "start": info["offset"], "loopStart": sd["loopStart"], "loopLen": sd["loopLen"],
            "attack": adsr["attack"] if adsr else 0, "decay": adsr["decay"] if adsr else 0,
            "release": info["release"],
            "vib_depth": info["vib_depth"], "vib_period": info["vib_period"]}


def macro_program(pool, sdir, macro_id):
    """Interpret a SoundMacro as a timed voice program, the way the MusyX engine runs it.

    A macro is not one sample: it is a sequence of StartSample/StopSample/WaitTicks/AddNote/
    Envelope/Vibrato/SetAdsr commands. The first sustaining sample (held until a key-off wait,
    0xfffe) is the note's body; samples started AFTER the key-off wait are release tails that
    play once the key is let go. AddNote shifts pitch. We collect this so the renderer can
    reproduce 'brief attack sample -> sustained body -> release tail' instead of one flat sample.

    Returns {"phases": [ {inst, transpose, sustain:bool} ], "transpose": base} or None.
    """
    if macro_id not in pool[0]:
        return None
    _, _, data = pool[0][macro_id]
    phases, transpose, seen_keyoff = [], 0, False
    cur_adsr = None
    cur = None                                          # phase currently being filled
    vol = 1.0                                           # running voice volume (0..1)
    t = 0                                               # macro-tick clock for the current phase
    pending_target = None                               # Envelope ramp target awaiting its WaitTicks

    def close_phase():
        if cur is not None:
            cur["contour"].append((t, vol))             # final breakpoint
            cur["ticks"] = t
            phases.append(cur)

    for i in range(0, len(data) - 7, 8):
        w0, w1 = struct.unpack_from(">II", data, i)
        op = w0 & 0xFF
        if op == 0x18:                                  # AddNote: signed semitone transpose
            tr = (w0 >> 16) & 0xFF
            transpose += tr - 256 if tr > 127 else tr
        elif op == 0x0C:                                # SetAdsr table id (applies to next sample)
            cur_adsr = (w0 >> 8) & 0xFFFF
        elif op == 0x0D:                                # ScaleVol: step volume change
            vol = ((w0 >> 8) & 0xFF) / 127.0
            if cur is not None:
                cur["contour"].append((t, vol))
        elif op == 0x0F:                                # Envelope: ramp toward target over next wait
            pending_target = ((w0 >> 8) & 0xFF) / 127.0
        elif op == 0x04:                                # WaitTicks: advance clock; 0xfffe/ffff = key-off
            amt = w1 >> 16
            if amt >= 0xFFFE:
                seen_keyoff = True
                if cur is not None and cur["keyoff_tick"] is None:
                    cur["keyoff_tick"] = t
            else:
                t += amt
                if pending_target is not None:
                    vol = pending_target
                    pending_target = None
                if cur is not None:
                    cur["contour"].append((t, vol))
        elif op in (0x10, 0x11, 0x00):                  # StartSample / StopSample / End close a phase
            close_phase()
            cur, t = None, 0
            if op == 0x10:
                sid = (w0 >> 8) & 0xFFFF
                if sid in sdir:
                    sd = sdir[sid]
                    adsr = adsr_table(pool, cur_adsr) if cur_adsr else None
                    cur = {"sample_id": sid, "index": sd["index"], "root": sd["root"],
                           "rate": sd["rate"], "start": w1 if w1 < sd["num"] else 0,
                           "loopStart": sd["loopStart"], "loopLen": sd["loopLen"],
                           "attack": adsr["attack"] if adsr else 0,
                           "decay": adsr["decay"] if adsr else 0,
                           "transpose": transpose, "sustain": not seen_keyoff,
                           "contour": [(0, vol)], "keyoff_tick": None, "ticks": 0}
            if op == 0x00:
                break
    close_phase()
    return {"phases": phases, "transpose": transpose} if phases else None


def macro_samples(pool):
    """macro id -> first StartSample sample id (None if macro plays no sample)."""
    out = {}
    for mid, (_, _, data) in pool[0].items():
        _, st = parse_soundmacro(data)
        out[mid] = st["sample"] if st else None
    return out


if __name__ == "__main__":
    import sys
    if "--macros" in sys.argv:
        pool = parse_pool(open(os.path.join(D, "pool_SND.bin"), "rb").read())
        ms = macro_samples(pool)
        for mid in sorted(ms)[:24]:
            print(f"  macro {mid:#06x} -> sample {ms[mid]}")
        raise SystemExit(0)
    if "--pool" in sys.argv:
        pool = parse_pool(open(os.path.join(D, "pool_SND.bin"), "rb").read())
        for si, sec in enumerate(pool):
            ids = sorted(sec.keys())
            print(f"pool section {si}: {len(sec)} subsections, id range "
                  f"{min(ids):#06x}..{max(ids):#06x}")
            for sid in ids[:6]:
                off, size, data = sec[sid]
                print(f"    id={sid:#06x} size={size:#x} data[:24]={data[:24].hex()}")
        raise SystemExit(0)
    sdir = parse_sdir(open(os.path.join(D, "sdir_SND.bin"), "rb").read())
    proj = parse_proj(open(os.path.join(D, "proj_SND.bin"), "rb").read())
    print(f"sdir: {len(sdir)} samples (ids {min(sdir)}..{max(sdir)})")
    print(f"proj: {len(proj)} programs")
    for pid, r in sorted(proj.items()):
        smp = r["samples"]
        print(f"  prog {pid:#06x} type{r['type']}  samples={[hex(s) for s in smp[:6]]}"
              f"{'...' if len(smp) > 6 else ''}  (n={len(smp)})")
