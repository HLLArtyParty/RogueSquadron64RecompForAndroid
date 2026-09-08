#!/usr/bin/env python3
# One-shot: relocate the contiguous texrect/load/image rdpstate handler block
# (core lines 1459-2340) out of rt64_gbi_f3dfactor5.cpp into rt64_gbi_f5_rdpstate.cpp
# (before its closing GBI_F3DFACTOR5 namespace). Asserts the boundaries first;
# writes .bak backups. Build + smoke after to validate.
import shutil

core_path = r'e:\Projects\RogueSquadron64Recomp\lib\rt64\src\gbi\rt64_gbi_f3dfactor5.cpp'
rdp_path  = r'e:\Projects\RogueSquadron64Recomp\lib\rt64\src\gbi\rt64_gbi_f5_rdpstate.cpp'

shutil.copy(core_path, core_path + '.bak')
shutil.copy(rdp_path,  rdp_path  + '.bak')

raw = open(core_path, 'rb').read().decode('utf-8')
nl = '\r\n' if '\r\n' in raw else '\n'
core = raw.split(nl)

START, END = 1459, 2340  # 1-indexed inclusive
assert 'Texrect guard.' in core[START - 1], repr(core[START - 1])
assert core[END - 1].strip() == '}', repr(core[END - 1])
assert any('setup(GBI' in core[i] for i in (END, END + 1)), repr(core[END - 1:END + 3])

block = core[START - 1:END]
breadcrumb = [
    '        // texrect / fillRect_logged / texrectFlip / clamp_load_subscripts / load* /',
    '        // setTextureImage / setColorImage moved to rt64_gbi_f5_rdpstate.cpp',
    '        // (declared in the internal header).',
]
new_core = core[:START - 1] + breadcrumb + core[END:]
open(core_path, 'wb').write(nl.join(new_core).encode('utf-8'))

rraw = open(rdp_path, 'rb').read().decode('utf-8')
rnl = '\r\n' if '\r\n' in rraw else '\n'
rlines = rraw.split(rnl)
marker = '} // namespace GBI_F3DFACTOR5'
cand = [i for i, l in enumerate(rlines) if l.strip() == marker]
assert cand, 'closing-namespace marker not found in f5_rdpstate.cpp'
midx = cand[-1]
block_norm = [b.rstrip('\r') for b in block]
new_rlines = rlines[:midx] + block_norm + ['', ''] + rlines[midx:]
open(rdp_path, 'wb').write(rnl.join(new_rlines).encode('utf-8'))

print('OK: moved %d lines. core %d -> %d ; rdp %d -> %d'
      % (len(block), len(core), len(new_core), len(rlines), len(new_rlines)))
