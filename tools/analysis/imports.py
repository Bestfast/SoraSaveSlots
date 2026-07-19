import struct
from disas import d, rva2off, secs
pe = struct.unpack_from("<I", d, 0x3C)[0]
optsz = struct.unpack_from("<H", d, pe+20)[0]
opt = pe+24
# PE32+ data directory 1 = import
dd = opt + 112
imp_rva, imp_sz = struct.unpack_from("<II", d, dd + 1*8)
o = rva2off(imp_rva)
def cstr(rva):
    off=rva2off(rva); e=d.find(b"\0",off); return d[off:e].decode()
while True:
    olt, ts, fc, name_rva, first = struct.unpack_from("<IIIII", d, o)
    if name_rva == 0: break
    dll = cstr(name_rva).lower()
    if "kernel32" in dll or "shlwapi" in dll:
        print(f"--- {dll} ---")
        t = rva2off(olt or first)
        while True:
            v = struct.unpack_from("<Q", d, t)[0]
            if v == 0: break
            if not (v >> 63):
                nm = cstr((v & 0x7fffffff) + 2)
                if any(k in nm for k in ("File","Find","Directory","Path")):
                    print("   ", nm)
            t += 8
    o += 20
