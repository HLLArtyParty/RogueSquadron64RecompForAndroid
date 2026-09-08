"""N64 MusyX ADPCM decoder — faithful port of amuse lib/N64MusyXCodec.cpp.

Confirmed correct by ear (2026-05-26): decoded samp_SND entries sound like the
game's instrument samples. Each sample in samp_SND is laid out as:
  [coefs: 8*2*8 = 128 s16 big-endian = 256 bytes][ADPCM frames: 0x28 bytes each]
Each 0x28-byte frame yields 64 PCM samples as two self-contained 32-sample
sub-frames (raw 16-bit seeds restart per sub-frame -> no cross-frame history).

sdir_SND entry (0x18 bytes): +0x04 sample offset, +0x0A rate (u16), +0x0C low24 = sample count.
"""
from __future__ import annotations
import struct


def _s16(x: int) -> int:
    x &= 0xFFFF
    return x - 0x10000 if x & 0x8000 else x


def _clamp16(v: int) -> int:
    return -32768 if v < -32768 else (32767 if v > 32767 else v)


def _pred_sample(byte: int, mask: int, lshift: int, rshift: int) -> int:
    v = _s16((byte & mask) << lshift)
    return v >> rshift


def _predicted_frame(raw4: bytes, nib16: bytes, rshift: int):
    """16 nibble-pairs -> 32 predicted samples; first two are raw 16-bit seeds."""
    f = [0] * 32
    f[0] = _s16((raw4[0] << 8) | raw4[1])
    f[1] = _s16((raw4[2] << 8) | raw4[3])
    for i in range(1, 16):
        b = nib16[i]
        f[2 * i] = _pred_sample(b, 0xF0, 8, rshift)
        f[2 * i + 1] = _pred_sample(b, 0x0F, 12, rshift)
    return f


def _rdot(n: int, book2, src):
    return sum(book2[k] * src[n - 1 - k] for k in range(n))


def _decode_upto(src, book16, hist, size: int):
    b1 = book16[0:8]
    b2 = book16[8:16]
    l1, l2 = hist
    out = []
    for i in range(size):
        accu = (src[i] << 11) + b1[i] * l1 + b2[i] * l2 + _rdot(i, b2, src)
        out.append(_clamp16(accu >> 11))
    return out


def decompress_frame(d: bytes, coefs, last_sample: int):
    """Decode one 0x28-byte frame, capped at last_sample total samples."""
    out = [0] * 64
    rem = last_sample
    samples = 0
    for rawo, nibo, outbase in ((0, 0x8, 0), (4, 0x18, 32)):
        c2 = d[nibo] % 0x80
        book = coefs[(c2 & 0xF0) >> 4]
        b16 = book[0] + book[1]
        rshift = c2 & 0x0F
        frame = _predicted_frame(d[rawo:rawo + 4], d[nibo:nibo + 16], rshift)
        # first two samples are the raw seeds (history bootstrap)
        ps = max(0, min(rem, 2))
        for k in range(ps):
            out[outbase + k] = frame[k]
        samples += ps
        rem -= ps
        if rem <= 0:
            return out[:samples]
        for off, hoff, size in ((2, 0, 6), (8, 6, 8), (16, 14, 8), (24, 22, 8)):
            ps = min(rem, size)
            if ps <= 0:
                break
            seg = _decode_upto(frame[off:off + ps], b16,
                               (out[outbase + hoff], out[outbase + hoff + 1]), ps)
            for k in range(ps):
                out[outbase + off + k] = seg[k]
            samples += ps
            rem -= ps
            if rem <= 0:
                return out[:samples]
    return out[:samples]


def read_coefs(samp: bytes, off: int):
    """Read the inline 8x2x8 s16 big-endian coefficient book at sample offset."""
    coefs = []
    p = off
    for _ in range(8):
        pair = []
        for _ in range(2):
            row = list(struct.unpack_from(">8h", samp, p))
            p += 16
            pair.append(row)
        coefs.append(pair)
    return coefs  # p == off + 256


def decode_sample(samp: bytes, off: int, num_samples: int):
    """Decode one MusyX ADPCM sample to a list of int16 PCM samples."""
    coefs = read_coefs(samp, off)
    frames = off + 256
    out = []
    rem = num_samples
    fp = frames
    while rem > 0 and fp + 0x28 <= len(samp):
        got = decompress_frame(samp[fp:fp + 0x28], coefs, min(rem, 64))
        out.extend(got)
        rem -= len(got)
        fp += 0x28
        if len(got) < 64:
            break
    return out


def parse_sdir(sdir: bytes):
    """Yield (index, offset, rate, num_samples) for each 0x18-byte directory entry."""
    n = len(sdir) // 0x18
    for i in range(n):
        e = i * 0x18
        if struct.unpack_from(">I", sdir, e)[0] == 0xFFFFFFFF:
            break
        off = struct.unpack_from(">I", sdir, e + 0x04)[0]
        rate = struct.unpack_from(">H", sdir, e + 0x0A)[0]
        ns = struct.unpack_from(">I", sdir, e + 0x0C)[0] & 0xFFFFFF
        yield i, off, rate, ns
