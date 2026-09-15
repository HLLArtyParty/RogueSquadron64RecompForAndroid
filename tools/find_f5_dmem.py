rom = open("rogue_squadron.z64","rb").read()
N=len(rom)
def u16(o): return (rom[o]<<8)|rom[o+1] if o+2<=N else -1
def u32(o): return int.from_bytes(rom[o:o+4],'big') if o+4<=N else -1
# op_03 handler 0x14D4, op_04 0x15B4 — search whole ROM.
for label,val,rd in [("u16 0x14D4",0x14D4,u16),("u16 0x15B4",0x15B4,u16),
                     ("u32 0x040014D4",0x040014D4,u32),("u32 0x040015B4",0x040015B4,u32)]:
    hits=[o for o in range(0,N-4,2) if rd(o)==val]
    print(f"{label}: {len(hits)} hits" + (f" first@0x{hits[0]:06X}..0x{hits[-1]:06X}" if hits else ""))
    if 0<len(hits)<=12:
        for h in hits: print(f"    0x{h:06X}")
# Cluster: windows where >=3 distinct known targets appear within 0x100 bytes (u16).
known={0x10D8,0x10E0,0x12A0,0x12B8,0x12C4,0x12E4,0x12EC,0x12FC,0x1304,0x1350,
       0x135C,0x1370,0x1390,0x146C,0x1484,0x14D4,0x14F0,0x1504,0x15A4,0x15AC,0x15B4,0x1DB0}
print("\n=== u16 clusters of known targets (>=5 in a 0x120 window) ===")
o=0
while o<N-2:
    if u16(o) in known:
        cnt=sum(1 for k in range(o,min(o+0x120,N-1),2) if u16(k) in known)
        if cnt>=5: print(f"  rom=0x{o:06X} cluster={cnt}"); o+=0x120; continue
    o+=2
