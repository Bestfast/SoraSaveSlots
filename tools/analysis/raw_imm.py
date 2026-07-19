"""Exhaustively list raw imm32 occurrences of a value inside .text, with byte context."""
import struct, sys
from capstone import Cs, CS_ARCH_X86, CS_MODE_64
from disas import d, rva2off, off2rva, func_of, secs, IMAGEBASE

val = int(sys.argv[1], 0) if len(sys.argv) > 1 else 175
text = next(s for s in secs if s[0] == ".text")
LO, HI = text[3], text[3] + text[4]
needle = struct.pack("<I", val)
md = Cs(CS_ARCH_X86, CS_MODE_64)

print(f"=== raw dword {val} (0x{val:x}) inside .text ===")
i = LO
hits = 0
while True:
    i = d.find(needle, i, HI)
    if i < 0:
        break
    rva = off2rva(i)
    fn = func_of(rva)
    decoded = ""
    for back in range(1, 11):
        start = i - back
        try:
            ins = next(md.disasm(d[start:start + 16], IMAGEBASE + off2rva(start), count=1))
        except StopIteration:
            continue
        if ins.size >= back + 4 and (hex(val) in ins.op_str or str(val) in ins.op_str):
            decoded = f"{ins.mnemonic} {ins.op_str}"
            break
    ctx = d[i - 8:i + 4].hex()
    fs = f"fn 0x{fn[0]:x}" if fn else "fn ?"
    print(f"  rva 0x{rva:08x}  ctx {ctx}  {fs:<14} {decoded}")
    hits += 1
    i += 1
print(f"total {hits}")
