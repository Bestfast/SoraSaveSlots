"""Disassemble a raw RVA window (not whole function) - for inspecting hook vicinities."""
import sys
from disas import d, rva2off, IMAGEBASE
from capstone import Cs, CS_ARCH_X86, CS_MODE_64

start = int(sys.argv[1], 16)
length = int(sys.argv[2], 16) if len(sys.argv) > 2 else 0x100
md = Cs(CS_ARCH_X86, CS_MODE_64)
code = d[rva2off(start):rva2off(start) + length]
for ins in md.disasm(code, IMAGEBASE + start):
    r = ins.address - IMAGEBASE
    print(f"  {r:08x}  {ins.bytes.hex():<22} {ins.mnemonic:<8} {ins.op_str}")
