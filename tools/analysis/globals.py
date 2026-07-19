import struct
from disas import d, off2rva, rva2off, sec_of, secs, func_of
from xref import xrefs_to
# find dword==175 in .rdata/.data, then xref from .text
for secname in (".rdata", ".data"):
    s = next(x for x in secs if x[0]==secname)
    lo, hi = s[3], s[3]+s[4]
    i = lo; found=[]
    while True:
        i = d.find(b"\xaf\x00\x00\x00", i, hi)
        if i < 0: break
        if i % 4 == (rva2off(s[1]) % 4):   # aligned
            found.append(off2rva(i))
        i += 1
    print(f"=== {secname}: {len(found)} aligned dword-175 ===")
    hits=0
    for rva in found:
        xs = xrefs_to(rva)
        if xs:
            for x in xs:
                fn = func_of(x)
                print(f"   data 0x{rva:x}  referenced from 0x{x:x} (fn 0x{fn[0]:x})" if fn else f"   data 0x{rva:x} <- 0x{x:x}")
            hits+=1
    print(f"   referenced: {hits}")
