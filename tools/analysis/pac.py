import struct, sys, os
p = sys.argv[1]
d = open(p,'rb').read(0x4000)
size = os.path.getsize(p)
magic, count, f8, fc = struct.unpack_from("<4sIII", d, 0)
print(f"{os.path.basename(p)}  size={size}  magic={magic}  count={count}  f8=0x{f8:x}  fc={fc}")
for esz in (16,20,24,32,40,48):
    ents=[]
    ok=True
    for i in range(min(count,8)):
        o=0x10+i*esz
        vals=struct.unpack_from("<"+"Q"*(esz//8), d, o) if esz%8==0 else None
        if vals is None: ok=False; break
        ents.append(vals)
    if not ok: continue
    # plausible if some column is monotonic increasing and < size
    ncol=esz//8
    for c in range(ncol):
        col=[e[c] for e in ents]
        if all(0 <= v < size for v in col) and all(col[i]<col[i+1] for i in range(len(col)-1)):
            print(f"  esz={esz} col{c} monotonic<size: {[hex(v) for v in col]}")
