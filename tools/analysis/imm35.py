import struct
from capstone import Cs, CS_ARCH_X86, CS_MODE_64
from disas import d, off2rva, func_of, secs, IMAGEBASE
text = next(s for s in secs if s[0]==".text")
LO,HI=text[3],text[3]+text[4]
md=Cs(CS_ARCH_X86,CS_MODE_64)
print("=== 'mov [mem], 35' stores ===")
n=0
for pat in (b"\xc7", ):
    i=LO
    while True:
        i=d.find(pat,i,HI)
        if i<0: break
        try: ins=next(md.disasm(d[i:i+16], IMAGEBASE+off2rva(i), count=1))
        except StopIteration: i+=1; continue
        if ins.mnemonic=="mov" and ins.op_str.endswith(", 0x23") and "[" in ins.op_str:
            fn=func_of(off2rva(i))
            fs=f"fn 0x{fn[0]:x}" if fn else "fn ?"
            print(f"  0x{off2rva(i):08x} {fs:<14} {ins.mnemonic} {ins.op_str}")
            n+=1
        i+=1
print("total",n)
