"""Rename playVoice_0xNN / playVoice2_0xNN wrappers in
e:/Projects/rogue_squadron64/symbol_files/main_overlay.txt using the
subtitle (or user-note fallback) from speech_table_v2_annotated.csv.

Run from e:/Projects/RogueSquadron64Recomp.
"""
from __future__ import annotations

import argparse
import csv
import re
import struct
import sys
from pathlib import Path

ROM_PATH = Path("rs64.n64.us.1.0.z64")
CSV_PATH = Path("speech_table_v2_annotated.csv")
SYM_PATH = Path("e:/Projects/rogue_squadron64/symbol_files/main_overlay.txt")

STOPWORDS = {
    "a", "the", "an", "i", "my", "your", "our", "their", "his", "her", "its",
    "to", "of", "for", "on", "at", "by", "in", "is", "are", "was", "were",
    "be", "been", "will", "would", "should", "do", "have", "has", "had",
    "this", "that", "these", "those", "any", "one", "all", "they", "them",
    "it", "me", "you", "we", "us", "am", "as", "so", "if", "but", "and",
    "or", "not", "no", "got", "get",
}


def derive_name(text: str, gvid: int) -> str:
    """Build a C-safe meaningful name from a subtitle (or note) + voiceId."""
    if not text or text.strip() in ("", " "):
        return f"playSpeechUnknown_v0x{gvid:X}"

    is_sfx = text.lower().startswith((
        "wooky", "wookiee", "radio", "aaah", "yarr", "yaaa", "aarr", "romf",
        "raowr", "maybe a ui", "explosion sfx"
    ))

    cleaned = re.sub(r"[^a-zA-Z0-9'\s]", " ", text).lower()
    words = [w.replace("'", "") for w in cleaned.split()]
    words = [w for w in words if w and w not in STOPWORDS]
    pick = words[:5]
    if not pick:
        return f"playSpeechBlank_v0x{gvid:X}"

    camel = "".join(w.capitalize() for w in pick)
    camel = re.sub(r"[^A-Za-z0-9]", "", camel)  # drop any remaining non-alnum
    if not camel:
        return f"playSpeechBlank_v0x{gvid:X}"

    prefix = "play" if is_sfx else "say"
    return f"{prefix}{camel}_v0x{gvid:X}"


def load_voice_map() -> list[int]:
    rom = ROM_PATH.read_bytes()
    off = 0x8009FFE0 - 0x80000400 + 0x1000
    return list(struct.unpack_from(">756H", rom, off))


def load_samples() -> dict[int, dict]:
    out: dict[int, dict] = {}
    with CSV_PATH.open(encoding="utf-8") as f:
        for r in csv.DictReader(f):
            out[int(r["sample_idx"])] = r
    return out


def build_renames() -> list[tuple[str, str, str, str]]:
    """Returns list of (old_name, new_name, source_text, kind)."""
    vm = load_voice_map()
    samples = load_samples()
    txt = SYM_PATH.read_text(encoding="utf-8")
    # Match the symbol line AND its trailing comment so we can disambiguate
    # collisions using the second arg (e.g. 1.0f vs 0).
    pat = re.compile(
        r"^(playVoice2?_0x[0-9A-Fa-f]+)\s*=\s*0x([0-9A-Fa-f]+)\s*;\s*//(.*)$",
        re.M,
    )
    candidates = []
    for m in pat.finditer(txt):
        old = m.group(1)
        comment = m.group(3)
        gvid = int(old.split("_0x")[1], 16)
        if gvid >= len(vm):
            continue
        sid = vm[gvid]
        s = samples.get(sid)
        text = ""
        kind = "blank"
        if s:
            eng = s["english"].strip()
            note = s["user_notes"].strip()
            if eng and eng != " ":
                text = eng
                kind = "english"
            elif note:
                text = note
                kind = "note"
        new = derive_name(text, gvid)
        if old.startswith("playVoice2_"):
            new = re.sub(r"^(say|play)2?", lambda mm: mm.group(0).rstrip("2") + "2", new)
        # extract second arg from comment for collision disambiguation
        ma = re.search(r"playObjectiveVoiceLine[12]\([^,]+,\s*([^)]+)\)", comment)
        second_arg = ma.group(1).strip() if ma else ""
        candidates.append((old, new, text, kind, second_arg))

    # Disambiguate collisions using the second_arg
    by_new: dict[str, list] = {}
    for c in candidates:
        by_new.setdefault(c[1], []).append(c)
    out = []
    for new, group in by_new.items():
        if len(group) == 1:
            old, _, text, kind, _ = group[0]
            out.append((old, new, text, kind))
            continue
        for old, _, text, kind, sec in group:
            # Convert e.g. "1.0f" -> "f1", "0" -> "f0", "0.5f" -> "f0p5"
            tag = sec.replace(".", "p").replace("f", "").strip()
            tag = re.sub(r"[^A-Za-z0-9]", "", tag) or "x"
            disamb = f"{new}_f{tag}"
            out.append((old, disamb, text, kind))
    return out


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--apply", action="store_true", help="write changes to symbol_files")
    ap.add_argument("--limit", type=int, default=None)
    args = ap.parse_args()

    renames = build_renames()
    if args.limit:
        renames = renames[: args.limit]
    print(f"renames: {len(renames)}")

    # Check uniqueness
    seen = {}
    for old, new, _, _ in renames:
        seen.setdefault(new, []).append(old)
    dupes = {k: v for k, v in seen.items() if len(v) > 1}
    if dupes:
        print(f"WARNING: {len(dupes)} duplicate new-names:")
        for n, ol in dupes.items():
            print(f"  {n}  <-  {ol}")

    # Print preview
    width = max(len(o) for o, _, _, _ in renames) + 1
    for old, new, text, kind in renames:
        snip = (text[:48] + "...") if len(text) > 50 else text
        print(f"  {old:<{width}} -> {new:<55}  [{kind}]  {snip!r}")

    if not args.apply:
        print("\n(dry-run; re-run with --apply to write)")
        return 0

    # Apply: per-line edit of symbol file
    txt = SYM_PATH.read_text(encoding="utf-8")
    replaced = 0
    for old, new, _, _ in renames:
        # Match the symbol at start-of-line with whitespace then '='
        pat = re.compile(rf"^{re.escape(old)}(\s*=)", re.M)
        new_txt, n = pat.subn(rf"{new}\1", txt)
        if n == 1:
            txt = new_txt
            replaced += 1
        else:
            print(f"  WARN: {old}: {n} matches (expected 1)")
    SYM_PATH.write_text(txt, encoding="utf-8")
    print(f"\nwrote {replaced} renames to {SYM_PATH}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
