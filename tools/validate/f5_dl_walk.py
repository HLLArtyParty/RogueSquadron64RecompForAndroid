#!/usr/bin/env python3
"""Offline walk of a Factor 5 display list with the ucode's own rules (IMEM 0x1088..0x12F8, see
rt64_gbi_f3dfactor5.cpp): chunks are 0x108 bytes fetched whole, executed from +8; at +0x108 or on
B5/0x12 the walk continues in the chunk named by the current chunk's first word; 06 = call w1,
07 = branch w1, B8 = return / end. Lengths: 03 = 24, BD/BE/14 = 16, `05 05` = 40, textured BF/13/B4
= 32, untextured = 16.

--json emits a normalized, diff-friendly record stream (one record per flow op / primitive) with
absolute chunk addresses replaced by first-seen ordinals, so the same logical DL from two different
dumps (recomp vs PJ64 golden, different allocation addresses) compares equal. Consumed by dl_diff.py.
Usage: f5_dl_walk.py dump.bin [addr|task] [--max N] [--verbose] [--tail N] [--json]"""
import sys, struct, argparse, json
ap = argparse.ArgumentParser()
ap.add_argument('dump'); ap.add_argument('addr', default='task', nargs='?')
ap.add_argument('--max', type=int, default=20000); ap.add_argument('--verbose', action='store_true')
ap.add_argument('--tail', type=int, default=24)
ap.add_argument('--json', action='store_true', help='emit normalized diff-friendly records as JSON')
ap.add_argument('--b4', choices=('auto', '16', '32'), default='auto',
                help="0xB4 stride rule: auto=32 if w0&2 else 16 (hardware); 32=live op_b4_quad's unconditional rule (for A/B)")
a = ap.parse_args()
d = open(a.dump, 'rb').read()
def W(x): return struct.unpack('>I', d[x:x+4])[0]
start = (W(0x377C8 + 0x30) if a.addr == 'task' else int(a.addr, 16)) & 0xFFFFFF
KNOWN = set([0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F, 0x10,
             0x11, 0x12, 0x13, 0x14, 0x80, 0xAF, 0xB0, 0xB1, 0xB2, 0xB3, 0xB4, 0xB5, 0xB6, 0xB7, 0xB8, 0xB9, 0xBA,
             0xBB, 0xBC, 0xBD, 0xBE, 0xBF] + list(range(0xE4, 0x100)))
def valid_chunk(p): return (p >> 24) == 0x80 and (p & 0xFFFFFF) != 0 and (p & 0xFFFFFF) + 0x108 <= 0x800000
def linked_next(base):
    nxt = W(base)
    if not valid_chunk(nxt): return None
    if W((nxt & 0xFFFFFF) + 4) != (0x80000000 | base): return None   # target's prev must point back (allocated list)
    return nxt
stack = []          # (return pc, caller chunk base)
base = start        # chunk being executed at this level
pc = start + 8
steps = 0; hist = []; unknown = 0; first_unknown = None; faces = 0; rects = 0; ended = False
# --json: normalize absolute chunk offsets to first-seen ordinals so two dumps of the same logical DL
# (different allocation addresses) produce identical record streams; real structural differences remain.
records = []; chunk_ords = {}
def cord(off):
    off &= 0xFFFFFF
    if off not in chunk_ords: chunk_ords[off] = len(chunk_ords)
    return chunk_ords[off]
def rec(**kw):
    kw['i'] = len(records); records.append(kw)
def out(line):
    if not a.json: print(line)
def flow(line):
    hist.append(line)
    out('%s  depth=%d' % (line, len(stack)))
def enter(chunk):
    global base, pc
    base = chunk; pc = chunk + 8
while steps < a.max and not ended:
    if pc + 8 > 0x800000:
        out('OOB pc %08X' % pc); rec(kind='oob', depth=len(stack), chunk=cord(base)); break
    if pc >= base + 0x108:
        nxt = linked_next(base); steps += 1
        if nxt is not None:
            flow('%08X: (chunk end) -> next %08X' % (0x80000000 + pc, nxt))
            rec(kind='chunk_next', depth=len(stack), chunk=cord(base), to=cord(nxt))
            enter(nxt & 0xFFFFFF); continue
        nxt = W(base)
        flow('%08X: (chunk end, no next %08X)' % (0x80000000 + pc, nxt))
        rec(kind='chunk_end', depth=len(stack), chunk=cord(base))
        if stack: pc, base = stack.pop(); continue
        ended = True; break
    w0, w1 = W(pc), W(pc + 4); op = w0 >> 24; ln = 8; note = ''
    if op == 0x06 and (w0 & 0x00FEFFFF) == 0:
        t = w1 & 0xFFFFFF; steps += 1
        if ((w0 >> 16) & 1) == 0:
            stack.append((pc + 8, base)); flow('%08X: %08X %08X call' % (0x80000000 + pc, w0, w1))
            rec(kind='call', op=op, depth=len(stack), chunk=cord(base), to=cord(t))
        else:
            flow('%08X: %08X %08X branch' % (0x80000000 + pc, w0, w1))
            rec(kind='branch', op=op, depth=len(stack), chunk=cord(base), to=cord(t))
        enter(t); continue
    elif op == 0x07:
        t = w1 & 0xFFFFFF; steps += 1
        flow('%08X: %08X %08X branch' % (0x80000000 + pc, w0, w1))
        bad = (t == 0 or t + 0x108 > 0x800000)
        rec(kind='branch07', op=op, depth=len(stack), chunk=cord(base), to=('bad' if bad else cord(t)))
        if bad: out('   bad target'); ended = True; break
        enter(t); continue
    elif op in (0xB5, 0x12):
        nxt = linked_next(base); steps += 1
        flow('%08X: %08X %08X next -> %s' % (0x80000000 + pc, w0, w1, ('%08X' % nxt) if nxt is not None else 'unlinked %08X' % W(base)))
        rec(kind='next', op=op, depth=len(stack), chunk=cord(base), to=(cord(nxt) if nxt is not None else 'unlinked'))
        if nxt is not None: enter(nxt & 0xFFFFFF); continue
        if stack: pc, base = stack.pop(); continue
        ended = True; break
    elif op in (0xB8, 0x0F):
        steps += 1; flow('%08X: %08X %08X ret' % (0x80000000 + pc, w0, w1))
        rec(kind='ret', op=op, depth=len(stack), chunk=cord(base))
        if stack: pc, base = stack.pop(); continue
        ended = True; break
    elif op == 0x05: ln = 40 if ((w0 >> 16) & 0xFF) == 5 else 8
    elif op in (0xBE, 0xBD, 0x14, 0x09, 0x0A): ln = 16
    elif op == 0x03: ln = 24
    elif op in (0xBF, 0x08, 0x13): ln = 32 if (w0 & 2) else 16
    elif op == 0xB4: ln = 32 if a.b4 == '32' else (16 if a.b4 == '16' else (32 if (w0 & 2) else 16))
    elif op not in KNOWN:
        unknown += 1; note = 'UNKNOWN'
        if first_unknown is None: first_unknown = (steps, pc)
    if op in (0xBF, 0xB4, 0x13, 0x08):
        faces += 1
        rec(kind='face', op=op, depth=len(stack), chunk=cord(base),
            verts=(3 if op in (0xBF, 0x08) else 4), tex=bool(w0 & 2), stride=ln)
    elif op in (0xE4, 0xE5):
        rects += 1; rec(kind='rect', op=op, depth=len(stack), chunk=cord(base), stride=ln)
    elif op not in KNOWN:
        rec(kind='unknown', op=op, depth=len(stack), chunk=cord(base))
    else:
        rec(kind='state', op=op, depth=len(stack), chunk=cord(base), stride=ln)
    hist.append('%08X: %08X %08X %s' % (0x80000000 + pc, w0, w1, note))
    if a.verbose: out(hist[-1] + ('  depth=%d' % len(stack)))
    steps += 1; pc += ln
if a.json:
    print(json.dumps({
        'summary': {'steps': steps, 'faces': faces, 'rects': rects, 'unknown': unknown,
                    'chunks': len(chunk_ords), 'end': 'clean' if ended else 'runaway'},
        'records': records,
    }))
else:
    print('steps=%d faces=%d rects=%d unknown=%d first_unknown=%s end=%s' % (
        steps, faces, rects, unknown, ('step %d @%08X' % (first_unknown[0], 0x80000000 + first_unknown[1])) if first_unknown else None,
        'clean' if ended else 'RUNAWAY (max steps)'))
    if first_unknown:
        lo = max(0, first_unknown[0] - a.tail)
        print('--- history before first unknown ---'); print('\n'.join(hist[lo:first_unknown[0] + 4]))
