import struct,glob,os
def u32(b,o): return struct.unpack(">I",b[o:o+4])[0]
def u16(b,o): return struct.unpack(">H",b[o:o+2])[0]
proj=open("dumps/snd/proj_SND.bin","rb").read()
print("=== proj segments (off, id, type, size) ===")
off=0; groups=[]
while off+8<=len(proj):
    size=u32(proj,off)
    if size in (0,0xFFFFFFFF): break
    sid=u16(proj,off+4); typ=u16(proj,off+6)
    print(f"  off=0x{off:06X} id=0x{sid:04X} type={typ} size=0x{size:X}")
    groups.append((off,sid,typ,size))
    off+=size
print(f"\ntotal proj size=0x{len(proj):X}, parsed to 0x{off:X}")
# Peek at song headers: do they carry a group/setup id?
print("\n=== _SNG headers (first 16 bytes) ===")
for f in sorted(glob.glob("dumps/snd/*_SNG.bin"))[:8]:
    d=open(f,"rb").read()
    print(f"  {os.path.basename(f):22s} hdr={' '.join(f'{x:02X}' for x in d[:16])}")
