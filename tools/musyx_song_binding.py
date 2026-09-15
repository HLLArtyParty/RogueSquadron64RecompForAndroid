"""MusyX song -> (sgid, sid) binding for Rogue Squadron 64.

Hunts the SONG ARCHIVE (the data blob 'sound/' directory) for the canonical _SNG order,
then assigns each song to a proj GROUP by walking groups in sgid order and consuming each
group's per-sid MIDIsetup count (parse_midisetups_all). See
memory reference_musyx_song_group_binding_2026_05_29.

STATUS: HYPOTHESIS. Validated by the cutscene cluster (all *_cut1/cut_seq*/title -> group 0x1E),
but total setups=98 vs 97 songs (off by one) and some theme boundaries look shifted. Confirm the
exact boundaries via runtime capture of func_80097AD0 (sgid@struct+0x14, sid@+0x16, sngPtr@+0x10)
or func_8009C5BC (packed command: sgid=byte2>>3, sid=((byte2&7)<<2)|(byte3>>6)).

Usage: python tools/musyx_song_binding.py [rom.z64]   (default rogue_squadron.z64)
"""
import sys, os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from extract_speech_table import walk_data_blob
from musyx_group import parse_midisetups_all


def canonical_songs(rom: bytes):
    """_SNG names in data-blob 'sound/' order (the canonical song archive order)."""
    return [path.split("/")[-1].replace("_SNG", "")
            for seg, path, ent in walk_data_blob(rom) if path.lower().endswith("_sng")]


def build_binding(rom: bytes, proj: bytes):
    """-> {song_name: (sgid, sid)} by group-order × per-group setup-count walk (HYPOTHESIS)."""
    songs = canonical_songs(rom)
    counts = {g: len(s) for g, s in sorted(parse_midisetups_all(proj).items())}
    out, i = {}, 0
    for g in sorted(counts):
        for sid in range(counts[g]):
            if i < len(songs):
                out[songs[i]] = (g, sid); i += 1
    return out, songs, counts


if __name__ == "__main__":
    rompath = sys.argv[1] if len(sys.argv) > 1 else "rogue_squadron.z64"
    rom = open(rompath, "rb").read()
    proj = open("dumps/snd/proj_SND.bin", "rb").read()
    binding, songs, counts = build_binding(rom, proj)
    print(f"{len(songs)} songs, {sum(counts.values())} setups across {len(counts)} groups")
    for name, (g, sid) in binding.items():
        print(f"  {name:18s} -> sgid=0x{g:02X} sid={sid}")
