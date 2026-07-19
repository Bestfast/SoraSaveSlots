import sys
from xref import xrefs_to
from disas import func_of
t=int(sys.argv[1],16)
for x in xrefs_to(t):
    fn=func_of(x)
    print(f"  lea @0x{x:x}  fn 0x{fn[0]:x}-0x{fn[1]:x}" if fn else f"  lea @0x{x:x} (no fn)")
