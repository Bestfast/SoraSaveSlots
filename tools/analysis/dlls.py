import struct
from disas import d, rva2off
pe = struct.unpack_from("<I", d, 0x3C)[0]
dd = pe+24+112
imp_rva, _ = struct.unpack_from("<II", d, dd + 8)
o = rva2off(imp_rva)
def cstr(rva):
    off=rva2off(rva); e=d.find(b"\0",off); return d[off:e].decode()
while True:
    olt, ts, fc, name_rva, first = struct.unpack_from("<IIIII", d, o)
    if name_rva == 0: break
    print("  ", cstr(name_rva))
    o += 20
