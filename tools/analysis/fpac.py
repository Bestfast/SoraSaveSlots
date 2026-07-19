import struct, sys, os
def entries(path):
    f=open(path,'rb'); d=f.read()
    magic,n,first,unk = struct.unpack_from("<4sIII", d, 0)
    assert magic==b'FPAC', magic
    out=[]
    for i in range(n):
        o=0x10+i*32
        crc,pad,name_a,size,addr = struct.unpack_from("<IIQQQ", d, o)
        end=d.find(b'\0', name_a)
        name=d[name_a:end].decode('utf-8','replace')
        out.append((name,addr,size,crc))
    return d,out
if __name__=="__main__":
    p=sys.argv[1]
    d,es=entries(p)
    print(f"{os.path.basename(p)}: {len(es)} files")
    pat=sys.argv[2] if len(sys.argv)>2 else None
    for name,addr,size,crc in es:
        if pat is None or pat.lower() in name.lower():
            print(f"  {name:<50} off=0x{addr:<10x} size={size}")
