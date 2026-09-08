rom = open("rogue_squadron.z64","rb").read()
def u16(o): return (rom[o]<<8)|rom[o+1]
# Find DMEM base: the low-opcode dispatch table is at DMEM 0xD6; op_03 handler 0x14D4 sits at
# table[0xD6 + 0x03*2] = 0xDC (opcode 3, halfword entries). So base = (rom addr of 0x14D4) - 0xDC.
import_targets=0x14D4
locs=[o for o in range(0x039900,0x039B00,2) if u16(o)==0x14D4]
print("0x14D4 occurrences near cluster:", [hex(x) for x in locs])
# Try each as the opcode-3 slot of the low table (DMEM 0xDC) → base candidate.
for loc in locs:
    base = loc - 0xDC
    # sanity: op_04 (0x15B4) should be at base+0xDE (opcode 4)
    if u16(base+0xDE)==0x15B4:
        print(f"\n*** DMEM base = rom 0x{base:06X} (op3@0xDC=0x14D4, op4@0xDE=0x15B4) ***")
        print("\n=== op_03 vertex-slot table candidate DMEM[0x40..0x64] (u16) ===")
        for off in range(0x40,0x64,2):
            v=u16(base+off); note=""
            if 0x600<=v<0xC00: note=f"  (cache slot? (v-0x670)/0x50={ (v-0x670)/0x50:.2f})"
            print(f"  DMEM[0x{off:03X}] = 0x{v:04X}{note}")
        print("\n=== low-opcode dispatch DMEM[0xD6..0xF6] (opcode->handler) ===")
        for off in range(0xD6,0xF6,2):
            op=(off-0xD6)//2
            print(f"  op_{op:02X}: DMEM[0x{off:03X}] = 0x{u16(base+off):04X}")
        break
else:
    print("no base matched op3/op4 adjacency; dumping raw around each loc")
    for loc in locs[:3]:
        print(f"\nraw @0x{loc-0x10:06X}:", " ".join(f"{u16(loc-0x10+2*i):04X}" for i in range(16)))

print("\n\n=== WIDE DMEM[0x46..0x1A0]: hunting cache-slot addresses (0x670 + k*0x50) ===")
base=0x039900
for off in range(0x46,0x1A0,2):
    v=u16(base+off)
    note=""
    if 0x640<=v<0xE00 and (v-0x670)%0x50==0: note=f"  <== SLOT {(v-0x670)//0x50}"
    elif 0x640<=v<0xE00: note=f"  (~slot region, (v-0x670)/0x50={(v-0x670)/0x50:.2f})"
    print(f"  DMEM[0x{off:03X}] = 0x{v:04X}{note}")
