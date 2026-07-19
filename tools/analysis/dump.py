import sys
from disas import d, rva2off, sec_of
lo=int(sys.argv[1],16); n=int(sys.argv[2],16) if len(sys.argv)>2 else 0x100
off=rva2off(lo)
for i in range(0,n,16):
    chunk=d[off+i:off+i+16]
    if not chunk: break
    hx=' '.join(f'{b:02x}' for b in chunk)
    asc=''.join(chr(b) if 32<=b<127 else '.' for b in chunk)
    print(f"{lo+i:08x}  {hx:<48}  {asc}")
