#!/usr/bin/env python3
"""Map the Factor 5 RSP DMEM data layout by static analysis of the ucode.

Disassembles dumps/f5_ucode.imem.bin (8KB RSP IMEM), then:
  1. Tracks register immediate-constant loads (addiu/ori/lui/li) so we can
     resolve register-relative DMEM accesses to absolute DMEM offsets.
  2. Records every scalar + COP2-vector load/store, with base reg + offset
     and (when the base reg holds a known constant) the absolute DMEM offset.
  3. Buckets accesses by DMEM region and prints a layout map.

Run: python tools/map_dmem_layout.py
"""
import sys, struct
import rabbitizer

IMEM = open("dumps/f5_ucode.imem.bin", "rb").read()
N = len(IMEM) // 4

# Disassemble all words as RSP instructions.
insns = []
for i in range(N):
    w = struct.unpack(">I", IMEM[i*4:i*4+4])[0]
    ins = rabbitizer.Instruction(w, vram=0x04001000 + i*4, category=rabbitizer.InstrCategory.RSP)
    insns.append((0x1000 + i*4, w, ins))

# Memory-access mnemonics (scalar + RSP COP2 vector).
LOAD_OPS  = {"lb","lbu","lh","lhu","lw","lwu","ldv","lqv","lrv","lsv","llv","lpv","luv","lhv","lfv","ltv","lbv"}
STORE_OPS = {"sb","sh","sw","sdv","sqv","srv","ssv","slv","spv","suv","shv","sfv","stv","sbv"}

def reg_name(ins, want):
    # Try to pull register operands by disassembled text (robust across rabbitizer versions).
    return None

# Track register constants via a simple linear pass. RSP regs $0..$31.
# We reset knowledge conservatively on jumps/branches targets isn't tracked;
# constants we care about (0xB70/0xCB4/0x170/0x1C0) are set close to use.
def analyze():
    regs = {}  # reg -> constant value (best-effort)
    accesses = []  # (imem, mnem, base_reg, offset, abs_or_None, text)
    consts = []    # (imem, reg, value, text)
    for (addr, w, ins) in insns:
        m = ins.getOpcodeName()
        txt = ins.disassemble(immOverride=None)
        # Constant tracking: addiu rt, rs, imm  / ori rt, rs, imm / lui rt, imm
        try:
            if m == "lui":
                rt = ins.rt.value if hasattr(ins.rt,'value') else None
                imm = ins.getProcessedImmediate()
                if rt is not None:
                    regs[rt] = (imm & 0xFFFF) << 16
                    consts.append((addr, rt, regs[rt], txt))
            elif m in ("addiu","addi"):
                rt = ins.rt.value if hasattr(ins.rt,'value') else None
                rs = ins.rs.value if hasattr(ins.rs,'value') else None
                imm = ins.getProcessedImmediate()
                if rt is not None and rs is not None:
                    base = regs.get(rs, None)
                    if rs == 0:
                        regs[rt] = imm & 0xFFFFFFFF
                        consts.append((addr, rt, regs[rt], txt))
                    elif base is not None:
                        regs[rt] = (base + imm) & 0xFFFFFFFF
                        consts.append((addr, rt, regs[rt], txt))
                    else:
                        regs.pop(rt, None)
            elif m == "ori":
                rt = ins.rt.value if hasattr(ins.rt,'value') else None
                rs = ins.rs.value if hasattr(ins.rs,'value') else None
                imm = ins.getProcessedImmediate()
                if rt is not None and rs is not None:
                    base = regs.get(rs, 0 if rs==0 else None)
                    if base is not None:
                        regs[rt] = (base | (imm & 0xFFFF)) & 0xFFFFFFFF
                        consts.append((addr, rt, regs[rt], txt))
                    else:
                        regs.pop(rt, None)
        except Exception:
            pass
        # Memory access extraction.
        if m in LOAD_OPS or m in STORE_OPS:
            base = None; off = None
            try:
                base = ins.rs.value if hasattr(ins,'rs') and hasattr(ins.rs,'value') else None
                off  = ins.getProcessedImmediate()
            except Exception:
                pass
            absoff = None
            if base is not None and base in regs and off is not None:
                absoff = (regs[base] + off) & 0xFFFFFFFF
            accesses.append((addr, m, base, off, absoff, txt))
    return accesses, consts

accesses, consts = analyze()

print("=== DMEM-offset constants loaded into registers (val < 0x1000) ===")
seen=set()
for (addr, reg, val, txt) in consts:
    if val < 0x1000 and (val,) not in seen:
        seen.add((val,))
        print(f"  IMEM 0x{addr:04X}  ${reg:<2} = 0x{val:03X}   {txt}")

print("\n=== Memory accesses with RESOLVED absolute DMEM offset ===")
for (addr, m, base, off, absoff, txt) in accesses:
    if absoff is not None and absoff < 0x1000:
        print(f"  IMEM 0x{addr:04X}  {m:5} -> DMEM 0x{absoff:03X}   (${base}+{off})   {txt}")

print("\n=== All memory accesses (raw base+offset) in key handler ranges ===")
RANGES = [(0x12A0,0x1300,"op06 setup"),(0x14B0,0x1520,"op01/02/03 entry+loop"),
          (0x1F14,0x1F70,"func_4001F14 mtx*vec"),(0x2F00,0x2FA0,"func_4001F60 persp/emit")]
for (lo,hi,name) in RANGES:
    print(f"\n  --- {name}  IMEM 0x{lo:04X}-0x{hi:04X} ---")
    for (addr, m, base, off, absoff, txt) in accesses:
        if lo <= addr < hi:
            a = f" = DMEM 0x{absoff:03X}" if absoff is not None and absoff<0x1000 else ""
            print(f"    0x{addr:04X}  {txt}{a}")
