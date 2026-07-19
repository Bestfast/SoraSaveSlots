import sys, struct
from fpac import entries
d,es=entries(sys.argv[1])
for name,addr,size,crc in es:
    if sys.argv[2].lower() in name.lower():
        out=name.split('/')[-1]
        open(out,'wb').write(d[addr:addr+size])
        print(f"wrote {out} ({size} bytes)")
