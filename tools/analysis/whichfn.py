import sys
from disas import func_of
for a in sys.argv[1:]:
    rva=int(a,16)
    fn=func_of(rva)
    print(f"  {a} -> fn 0x{fn[0]:x}-0x{fn[1]:x}" if fn else f"  {a} -> no fn")
