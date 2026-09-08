import sys
sys.path.insert(0,"tools")
from musyx_group import parse_midisetups, parse_pool, parse_sdir, macro_instrument
proj=open("dumps/snd/proj_SND.bin","rb").read()
pool=parse_pool(open("dumps/snd/pool_SND.bin","rb").read())
sdir=parse_sdir(open("dumps/snd/sdir_SND.bin","rb").read())
setups=parse_midisetups(proj)
print(f"=== {len(setups)} MIDISetups (group -> channel:program) ===")
for sid in sorted(setups):
    progs=setups[sid]
    used=[(c,p) for c,p in enumerate(progs) if p not in (0,0xFF)]
    print(f"\ngroup 0x{sid:02X}: " + " ".join(f"c{c}=0x{p:02X}" for c,p in used) if used else f"\ngroup 0x{sid:02X}: (empty)")
    # try to resolve each program's macro->instrument->sample for a couple channels
    for c,p in used[:4]:
        try:
            inst=macro_instrument(pool,sdir,p)
            if inst: print(f"     c{c} prog0x{p:02X} -> sample idx {inst.get('index')}")
        except Exception: pass
