import struct, sys
rom = open("rogue_squadron.z64","rb").read()
# Known IMEM dispatch targets that should appear in DMEM dispatch tables (big-endian u16).
known = {0x10D8,0x10E0,0x12A0,0x12B8,0x12C4,0x12E4,0x12EC,0x12FC,0x1304,0x1350,
         0x135C,0x1370,0x1390,0x146C,0x1484,0x14D4,0x14F0,0x1504,0x15A4,0x15AC,0x15B4,0x1DB0}
def u16(b,o): return (b[o]<<8)|b[o+1]
# Scan a window around the ucode text (0x025610) for a DMEM data image: a base where
# offset 0x64/0xD6 region holds many known IMEM targets.
lo, hi = 0x020000, 0x030000
best=[]
for base in range(lo, hi, 4):
    hits=0
    for off in range(0x60, 0xF0, 2):
        if base+off+2<=len(rom):
            if u16(rom, base+off) in known: hits+=1
    if hits>=4: best.append((hits, base))
best.sort(reverse=True)
print("Top DMEM-data-image candidates (hits, rom_off):")
for h,b in best[:6]: print(f"  hits={h} rom=0x{b:06X}")
if best:
    base=best[0][1]
    print(f"\n=== Using data image rom=0x{base:06X} ===")
    print("dispatch-ish region DMEM[0x60..0xF0] (u16):")
    for off in range(0x60,0xF0,2):
        v=u16(rom,base+off); mark=" <-IMEM" if v in known else ""
        print(f"  DMEM[0x{off:03X}] = 0x{v:04X}{mark}")
    print("\n=== op_03 table region DMEM[0x40..0x66] (u16) ===")
    for off in range(0x40,0x66,2):
        print(f"  DMEM[0x{off:03X}] = 0x{u16(rom,base+off):04X}")
