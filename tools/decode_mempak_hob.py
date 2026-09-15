#!/usr/bin/env python3
"""Decode the attribution Expansion-Pak cartridge mesh (mempak_HOB) offline.

Reads the zlib-compressed asset from the ROM at its data-blob offset, inflates
it, parses the HOB per docs/hob_files/extract_hob.py, and prints ground-truth
object/vertex/face/material counts so the runtime RAM read can be validated.

Run: python tools/decode_mempak_hob.py
"""
import os, sys, struct, zlib, io

ROM = r"e:/Projects/rogue_squadron64/rogue_squadron.z64"
MEMPAK_HOB_OFF = 0x00e55bd8  # ROM offset of the zlib stream (data_subsegments.yaml)

be16 = struct.Struct(">H").unpack
be32 = struct.Struct(">I").unpack
vtx  = struct.Struct(">hhhh").iter_unpack
idx4 = struct.Struct(">HHHH").unpack

EXTRA=1<<6; COLOR=1<<5; VCOLOR=1<<4; QUAD=1<<3; UV=1<<2; EXTRA2=1<<1

def inflate_at(rom, off):
    # zlib streams in the blob are oversized; just feed a generous window.
    d = zlib.decompressobj()
    out = d.decompress(rom[off:off+0x20000])
    return out

class Face:
    def __init__(self, b, i):
        self.i = i
        (self.flags,) = be32(b.read(4))
        self.secondary = self.flags & (EXTRA|EXTRA2)
        self.has_color = self.flags & COLOR
        self.vcolor    = self.flags & VCOLOR
        self.is_quad   = self.flags & QUAD
        self.has_uv    = self.flags & UV
        (self.unknown,) = be32(b.read(4))
        (self.material,) = be16(b.read(2)); b.seek(2, os.SEEK_CUR)
        n = 4 if self.is_quad else 3
        self.prim = idx4(b.read(8))[:n]
        self.sec = None; self.uv = None; self.fcolor=None; self.vcolors=None
        if self.secondary:
            self.sec = idx4(b.read(8))[:n]
        if self.has_color:
            if self.vcolor:
                self.vcolors = struct.unpack(f">{n}I", b.read(4*n))
            else:
                (self.fcolor,) = be32(b.read(4))
        if self.has_uv:
            self.uv = tuple((x/4096, 1-y/4096) for (x,y) in struct.iter_unpack(">hh", b.read(4*n)))

class FaceGroup:
    def __init__(self, b, off):
        b.seek(off); b.seek(8, os.SEEK_CUR)
        (self.flo,)=be32(b.read(4)); (self.fc,)=be32(b.read(4))
        b.seek(self.flo)
        self.faces=[Face(b,i) for i in range(self.fc)]

class Mesh1:
    def __init__(self, b, off):
        self.off=off; b.seek(off); b.seek(20, os.SEEK_CUR)
        (self.pvc,)=be32(b.read(4)); (self.svc,)=be32(b.read(4)); b.seek(4,os.SEEK_CUR)
        (self.fgo,)=be32(b.read(4)); (self.pvo,)=be32(b.read(4)); (self.svo,)=be32(b.read(4))
        self.fg=FaceGroup(b,self.fgo)
        b.seek(self.pvo)
        self.pv=list((x,y,z) for (x,y,z,_) in vtx(b.read(8*self.pvc)))
        if self.svo:
            b.seek(self.svo); self.sv=list((x,y,z) for (x,y,z,_) in vtx(b.read(8*self.svc)))
        else: self.sv=None

class Obj:
    def __init__(self, b, off):
        b.seek(off)
        (nm,)=struct.unpack(">16s", b.read(16)); self.name=nm.decode(errors="replace").rstrip("\x00")
        (self.md0_l,)=be32(b.read(4)); (self.md0_p,)=be32(b.read(4))
        (self.md1_p1,)=be32(b.read(4)); (self.md1_p2,)=be32(b.read(4))
        b.seek(self.md0_p)
        (self.md0c,)=be16(b.read(2)); (self.md1c,)=be16(b.read(2))
        self.md0_offs=[struct.unpack(">II",b.read(8)) for _ in range(self.md0c)]
        b.seek(self.md1_p1); b.seek(4,os.SEEK_CUR)
        self.md1_offs=[]; found=0
        while found!=self.md1c:
            (t,)=be32(b.read(4))
            if t: self.md1_offs.append(t); found+=1
        self.m1=[Mesh1(b,o) for o in self.md1_offs]

def main():
    rom=open(ROM,"rb").read()
    print(f"ROM size 0x{len(rom):X}; mempak_HOB zlib @ 0x{MEMPAK_HOB_OFF:X}")
    raw=inflate_at(rom, MEMPAK_HOB_OFF)
    print(f"inflated HOB = {len(raw)} bytes")
    b=io.BytesIO(raw)
    (objc,)=be16(b.read(2)); b.seek(6,os.SEEK_CUR)
    print(f"object_count = {objc}")
    objs=[Obj(b,0x74*i+8) for i in range(objc)]
    for o in objs:
        print(f"\nOBJECT '{o.name}'  md0c={o.md0c} md1c={o.md1c}")
        for mi,m in enumerate(o.m1):
            tris=sum(1 for f in m.fg.faces if not f.is_quad)
            quads=sum(1 for f in m.fg.faces if f.is_quad)
            uvs=sum(1 for f in m.fg.faces if f.has_uv)
            mats=sorted(set(f.material for f in m.fg.faces))
            print(f"  mesh[{mi}] pverts={m.pvc} sverts={m.svc} faces={m.fg.fc} (tri={tris} quad={quads}) uv_faces={uvs} mats={mats}")
            if m.pv:
                xs=[v[0] for v in m.pv]; ys=[v[1] for v in m.pv]; zs=[v[2] for v in m.pv]
                print(f"            vtx range x[{min(xs)},{max(xs)}] y[{min(ys)},{max(ys)}] z[{min(zs)},{max(zs)}]")
        # sample first mesh first 3 faces
        if o.m1:
            f0=o.m1[0].fg.faces[:3]
            for f in f0:
                print(f"            face[{f.i}] flags=0x{f.flags:08X} mat={f.material} prim={f.prim} uv={f.uv}")

if __name__=="__main__":
    main()
