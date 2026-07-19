"""Find direct call sites (E8 rel32) of a function RVA."""
import struct, sys
from disas import d, off2rva, func_of, secs

text = next(s for s in secs if s[0] == ".text")
LO, HI = text[3], text[3] + text[4]

for arg in sys.argv[1:]:
    target = int(arg, 16)
    print(f"=== callers of 0x{target:x} ===")
    n = 0
    i = LO
    while True:
        i = d.find(b"\xe8", i, HI)
        if i < 0:
            break
        rel = struct.unpack_from("<i", d, i + 1)[0]
        r = off2rva(i)
        if r is not None and r + 5 + rel == target:
            fn = func_of(r)
            fs = f"fn 0x{fn[0]:x}-0x{fn[1]:x}" if fn else "fn ?"
            print(f"  call @0x{r:08x}   {fs}")
            n += 1
        i += 1
    print("total", n)
