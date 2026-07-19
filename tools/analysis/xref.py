"""Find rip-relative xrefs to a string literal, report containing functions."""
import struct, re, sys
from disas import d, rva2off, off2rva, sec_of, func_of, secs, disas_func

text = next(s for s in secs if s[0] == ".text")
TEXT_LO, TEXT_HI = text[3], text[3] + text[4]


def find_str(lit: bytes):
    out = []
    for m in re.finditer(re.escape(lit), d):
        r = off2rva(m.start())
        if r is not None and sec_of(r) in (".rdata", ".data"):
            out.append(r)
    return out


def xrefs_to(target_rva):
    out = []
    for rex in (0x48, 0x4C):
        for modrm in (0x05, 0x0D, 0x15, 0x1D, 0x25, 0x2D, 0x35, 0x3D):
            pat = bytes([rex, 0x8D, modrm])
            start = TEXT_LO
            while True:
                i = d.find(pat, start, TEXT_HI)
                if i < 0:
                    break
                start = i + 1
                disp = struct.unpack_from("<i", d, i + 3)[0]
                nxt = off2rva(i + 7)
                if nxt is not None and nxt + disp == target_rva:
                    out.append(off2rva(i))
    return sorted(out)


if __name__ == "__main__":
    lit = sys.argv[1].encode() + b"\x00"
    dis = "-d" in sys.argv
    for rva in find_str(lit):
        xs = xrefs_to(rva)
        print(f"\n### {lit!r} @0x{rva:x}  ({len(xs)} xrefs)")
        for x in xs:
            fn = func_of(x)
            print(f"    lea @0x{x:x}   in func 0x{fn[0]:x}-0x{fn[1]:x}" if fn else f"    lea @0x{x:x}  (no fn)")
        if dis:
            seen = set()
            for x in xs:
                fn = func_of(x)
                if fn and fn[0] not in seen:
                    seen.add(fn[0])
                    disas_func(x, label=f"xref {lit!r}", highlight={x})
