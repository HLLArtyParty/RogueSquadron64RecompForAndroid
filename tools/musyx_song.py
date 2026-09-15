"""MusyX SONG (_SNG) sequence parser + offline renderer for Rogue Squadron 64.

Two-level amuse-style SONG structure (all big-endian), confirmed against the game's
sequencer (func_80094350 song-start, func_80095244 region walker):

Header, 6 u32:
  h0 trackTblOff   -> 64 u32 track pointers (0 = unused), each -> a TrackRegion list
  h1 regionTblOff  -> u32 region offsets (0-terminated), each -> a note-event stream
  h2 chanMapOff    -> per-track channel/program bytes (0xFF-terminated)
  h3 tempoTblOff   -> (tick:u32, tempo:u32) pairs, 0xFFFFFFFF-terminated
  h4 tempo (BPM-ish)
  h5 0

TrackRegion (12 bytes): startTick:u32, +0x4 u16, +0x6 u16, regionIdx:s16 @+0x8
  (0xFFFF=end, 0xFFFE=loop), loopTo:s16 @+0xA.

Region = 16-byte header then note events. Note event (8 bytes):
  duration:u16, note:u8, velocity:u8, absTick:u32.  Terminated by event word 0x0000FFFF.

Note timing within a region is absolute (region-local) ticks; a TrackRegion places the
region at startTick on the track's channel.
"""
from __future__ import annotations
import struct


def header(d: bytes):
    h = list(struct.unpack_from(">6I", d, 0))
    return {"trackTbl": h[0], "regionTbl": h[1], "chanMap": h[2],
            "tempoTbl": h[3], "tempo": h[4]}


def region_offsets(d: bytes):
    offs, o = [], header(d)["regionTbl"]
    while True:
        v = struct.unpack_from(">I", d, o)[0]; o += 4
        if v == 0:
            break
        offs.append(v)
    return offs


def parse_region(d: bytes, start: int, include_flagged: bool = False):
    """Note events in one region: list of {tick, note, vel, dur, flagged}. 16-byte header skipped.
    Events with note bit7 set (uniform vel=0x87, dur=0) are likely note-off/control; included
    (masked note&0x7f) only when include_flagged=True, for experimentation."""
    notes, o = [], start + 16
    while o + 8 <= len(d):
        ev, tick = struct.unpack_from(">II", d, o)
        if (ev & 0xFFFF) == 0xFFFF:      # 0x0000FFFF terminator
            break
        dur = ev >> 16
        note = (ev >> 8) & 0xFF
        vel = ev & 0xFF
        if note & 0x80:
            # Flagged events (bit7) add musical content but some decode to extreme octaves
            # (note&0x7f near 127 -> screech). Only include plausible instrument range.
            if include_flagged and 24 <= (note & 0x7F) <= 100:
                notes.append({"tick": tick, "note": note & 0x7F, "vel": vel & 0x7F,
                              "dur": dur or 96, "flagged": True})
        else:
            notes.append({"tick": tick, "note": note, "vel": vel, "dur": dur, "flagged": False})
        o += 8
    return notes


def track_regions(d: bytes, toff: int):
    """TrackRegion list for one track -> list of (startTick, regionIdx, loopTo)."""
    regs, o = [], toff
    while o + 12 <= len(d):
        startTick = struct.unpack_from(">I", d, o)[0]
        ridx = struct.unpack_from(">h", d, o + 8)[0]
        loopTo = struct.unpack_from(">h", d, o + 10)[0]
        if ridx == -1:                   # 0xFFFF end
            break
        regs.append((startTick, ridx, loopTo))
        o += 12
        if ridx == -2:                   # 0xFFFE loop terminator
            break
    return regs


def chan_map(d: bytes):
    o, out = header(d)["chanMap"], []
    while o < len(d) and d[o] != 0xFF:
        out.append(d[o]); o += 1
    return out


def parse_song(d: bytes, include_flagged: bool = False):
    hdr = header(d)
    regs = region_offsets(d)
    region_notes = [parse_region(d, off, include_flagged) for off in regs]
    chans = chan_map(d)
    base = hdr["trackTbl"]
    tracks = []
    for i in range(64):
        toff = struct.unpack_from(">I", d, base + i * 4)[0]
        if not toff:
            continue
        trs = track_regions(d, toff)
        # flatten: each TrackRegion places region's notes at startTick offset
        notes = []
        for startTick, ridx, _ in trs:
            if 0 <= ridx < len(region_notes):
                for n in region_notes[ridx]:
                    notes.append({**n, "tick": n["tick"] + startTick})
        if notes:
            ch = chans[len(tracks)] if len(tracks) < len(chans) else 0
            tracks.append({"index": i, "chan": ch, "regions": trs, "notes": notes})
    return hdr, tracks, region_notes


_NOTES = ["C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"]
def notename(n):
    return f"{_NOTES[n % 12]}{n // 12 - 1}"


if __name__ == "__main__":
    import sys
    for path in sys.argv[1:]:
        d = open(path, "rb").read()
        hdr, tracks, rn = parse_song(d)
        print(f"\n=== {path}  tempo={hdr['tempo']}  regions={len(rn)}  tracks={len(tracks)} ===")
        for t in tracks:
            ns = t["notes"]
            seq = " ".join(notename(x["note"]) for x in ns[:12])
            print(f"  trk{t['index']:2d} ch{t['chan']:2d}  {len(ns):3d} notes  "
                  f"regs={[r[1] for r in t['regions']]}  {seq}")
