"""Extract MusyX sound assets (music/SFX/sample bank) from the RS64 ROM, offline.

Phase 1 (this): walk the data blob, list every audio-ish asset (SND banks + song
files), and dump sdir_SND/samp_SND (+ proj/pool) raw (zlib-decompressed) to
dumps/snd/. Reuses the speech extractor's manifest walk + zlib loader.

Phase 2 (next): parse sdir_SND entries -> sample {offset,len,rate,loop,ADPCM book},
decode MusyX ADPCM (algorithm read from build/factor5_ucode/musyx_audio_recompiled.c)
-> WAV per sample. Like extract_speech_table.py but with working decode.

Usage: python extract_sounds.py <rom.z64> [--dump]
"""
import sys, os, struct, wave
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from extract_speech_table import walk_data_blob, load_asset_bytes
from musyx_n64_decode import parse_sdir, decode_sample


def write_wav(path, pcm, rate):
    with wave.open(path, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(rate or 22050)
        w.writeframes(struct.pack(f"<{len(pcm)}h", *pcm))


def decode_all(outdir):
    """Decode every samp_SND entry (per sdir_SND) to a WAV. Returns count."""
    samp = open(os.path.join(outdir, "samp_SND.bin"), "rb").read()
    sdir = open(os.path.join(outdir, "sdir_SND.bin"), "rb").read()
    wavdir = os.path.join(outdir, "wav")
    os.makedirs(wavdir, exist_ok=True)
    n = 0
    for i, off, rate, ns in parse_sdir(sdir):
        if ns == 0 or off + 256 > len(samp):
            continue
        pcm = decode_sample(samp, off, ns)
        if not pcm:
            continue
        fn = os.path.join(wavdir, f"s{i:03d}_{rate}hz.wav")
        write_wav(fn, pcm, rate)
        n += 1
    print(f"  decoded {n} samples -> {wavdir}")
    return n

AUDIO_HINT = ("SND", "song", "jing", "theme", "logo", "title", "credit", "cut_",
              "seq", "trns", "spc", "prob", "act", "mus", "sfx", "voice", "amb")

def looks_audio(name: str) -> bool:
    n = name.lower()
    return any(h.lower() in n for h in AUDIO_HINT)

def main() -> int:
    if len(sys.argv) < 2:
        print("usage: extract_sounds.py <rom.z64> [--dump]"); return 1
    rom = open(sys.argv[1], "rb").read()
    do_dump = "--dump" in sys.argv
    outdir = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "dumps", "snd")
    if do_dump: os.makedirs(outdir, exist_ok=True)

    audio = []
    for seg, path, ent in walk_data_blob(rom):
        if looks_audio(path):
            comp = ent["compressed_size"]
            audio.append((seg, path, ent["decompressed_size"], comp))
    print(f"=== {len(audio)} audio-ish assets in ROM ===")
    for seg, path, dsz, comp in audio:
        c = "raw" if comp == 0xFFFFFFFF else f"zlib({comp})"
        print(f"  [{seg}] {path:32s} decompressed={dsz:#x} {c}")

    if do_dump:
        for seg, path, ent in walk_data_blob(rom):
            base = path.rsplit("/", 1)[-1]
            if base in ("sdir_SND", "samp_SND", "proj_SND", "pool_SND"):
                data = load_asset_bytes(rom, ent)
                fn = os.path.join(outdir, base + ".bin")
                open(fn, "wb").write(data)
                print(f"  wrote {fn} ({len(data):#x} bytes)")

    if "--wav" in sys.argv:
        decode_all(outdir)
    return 0

if __name__ == "__main__":
    raise SystemExit(main())
