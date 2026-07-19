import struct, sys
from capstone import Cs, CS_ARCH_X86, CS_MODE_64
from disas import d, off2rva, func_of, secs, IMAGEBASE
target = int(sys.argv[1], 0)
text = next(s for s in secs if s[0]==".text")
LO,HI = text[3], text[3]+text[4]
md = Cs(CS_ARCH_X86, CS_MODE_64)
needle = struct.pack("<i", target)
print(f"=== instructions with disp 0x{target:x} ===")
i=LO; n=0
while True:
    i = d.find(needle, i, HI)
    if i<0: break
    for back in range(2, 12):
        s = i-back
        try: ins = next(md.disasm(d[s:s+16], IMAGEBASE+off2rva(s), count=1))
        except StopIteration: continue
        if ins.size >= back+4 and (f"0x{target:x}" in ins.op_str) and "[" in ins.op_str:
            fn = func_of(off2rva(s))
            fs = f"fn 0x{fn[0]:x}" if fn else "fn ?"
            print(f"  0x{off2rva(s):08x} {fs:<14} {ins.mnemonic} {ins.op_str}")
            n+=1; break
    i+=1
print("total", n)
