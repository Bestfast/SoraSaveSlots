import struct, sys
from capstone import Cs, CS_ARCH_X86, CS_MODE_64
from disas import d, off2rva, func_of, secs, IMAGEBASE
target=int(sys.argv[1],0)
text=next(s for s in secs if s[0]=='.text')
LO,HI=text[3],text[3]+text[4]
md=Cs(CS_ARCH_X86,CS_MODE_64)
needle=struct.pack("<i",target)
print(f"=== lea/mov reg,[reg+0x{target:x}] (address-taking or access) ===")
i=LO; seen=set()
while True:
    i=d.find(needle,i,HI)
    if i<0: break
    for back in range(3,10):
        s=i-back
        try: ins=next(md.disasm(d[s:s+16], IMAGEBASE+off2rva(s), count=1))
        except StopIteration: continue
        if ins.size>=back+4 and f"0x{target:x}" in ins.op_str and ins.mnemonic in ("lea","mov"):
            rva=off2rva(s); fn=func_of(rva)
            key=(rva,ins.mnemonic)
            if key not in seen:
                seen.add(key)
                print(f"  0x{rva:08x} fn 0x{fn[0]:x}  {ins.mnemonic} {ins.op_str}")
            break
    i+=1
