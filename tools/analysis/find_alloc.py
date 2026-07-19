import struct, re, sys
from capstone import Cs, CS_ARCH_X86, CS_MODE_64
from disas import d, off2rva, func_of, secs, IMAGEBASE
text = next(s for s in secs if s[0]==".text")
LO,HI = text[3], text[3]+text[4]
md = Cs(CS_ARCH_X86, CS_MODE_64)
disp = struct.pack("<I", int(sys.argv[1],0))
print(f"=== instructions touching displacement {sys.argv[1]} ===")
i=LO; n=0
while True:
    i = d.find(disp, i, HI)
    if i<0: break
    for back in range(2,9):
        s=i-back
        try: ins=next(md.disasm(d[s:s+16], IMAGEBASE+off2rva(s), count=1))
        except StopIteration: continue
        if ins.size>=back+4 and sys.argv[1].lstrip('0x') in ins.op_str.replace('0x',''):
            fn=func_of(off2rva(s))
            print(f"  0x{off2rva(s):08x} fn 0x{fn[0]:x}  {ins.mnemonic} {ins.op_str}")
            n+=1; break
    i+=1
print("total",n)
