"""Disassemble functions of interest in sora_1st.exe using .pdata bounds."""
import struct, re, sys
from capstone import Cs, CS_ARCH_X86, CS_MODE_64

EXE = r"Z:\SteamLibrary\steamapps\common\Trails in the Sky 1st Chapter\sora_1st.exe"
d = open(EXE, "rb").read()

pe = struct.unpack_from("<I", d, 0x3C)[0]
nsec = struct.unpack_from("<H", d, pe + 6)[0]
optsz = struct.unpack_from("<H", d, pe + 20)[0]
IMAGEBASE = struct.unpack_from("<Q", d, pe + 24 + 24)[0]
secs = []
for i in range(nsec):
    o = pe + 24 + optsz + i * 40
    name = d[o:o + 8].rstrip(b"\0").decode()
    vs, va, rs, ro = struct.unpack_from("<IIII", d, o + 8)
    secs.append((name, va, vs, ro, rs))


def rva2off(rva):
    for name, va, vs, ro, rs in secs:
        if va <= rva < va + max(vs, rs):
            return ro + (rva - va)
    return None


def off2rva(off):
    for name, va, vs, ro, rs in secs:
        if ro <= off < ro + rs:
            return va + (off - ro)
    return None


def sec_of(rva):
    for name, va, vs, ro, rs in secs:
        if va <= rva < va + max(vs, rs):
            return name
    return "?"


# ---- .pdata function table
pd = next(s for s in secs if s[0] == ".pdata")
FUNCS = []  # (begin, end)
for o in range(pd[3], pd[3] + pd[4], 12):
    b, e, u = struct.unpack_from("<III", d, o)
    if b == 0 and e == 0:
        continue
    FUNCS.append((b, e))
FUNCS.sort()
print(f"[.pdata] {len(FUNCS)} runtime functions", file=sys.stderr)


def func_of(rva):
    lo, hi = 0, len(FUNCS) - 1
    best = None
    while lo <= hi:
        mid = (lo + hi) // 2
        b, e = FUNCS[mid]
        if b <= rva < e:
            return (b, e)
        if b > rva:
            hi = mid - 1
        else:
            best = FUNCS[mid]
            lo = mid + 1
    return best


md = Cs(CS_ARCH_X86, CS_MODE_64)
md.detail = False

STR_CACHE = {}


def read_cstr(rva, maxlen=64):
    off = rva2off(rva)
    if off is None:
        return None
    end = d.find(b"\0", off, off + maxlen)
    if end < 0:
        return None
    try:
        return d[off:end].decode("utf-8", "replace")
    except Exception:
        return None


def disas_func(rva, label="", highlight=()):
    fn = func_of(rva)
    if not fn:
        print(f"!! no .pdata entry for 0x{rva:x}")
        return
    b, e = fn
    off = rva2off(b)
    code = d[off:off + (e - b)]
    print(f"\n{'='*78}\n== {label}  func 0x{b:x}-0x{e:x}  (len {e-b})  [contains 0x{rva:x}]\n{'='*78}")
    for ins in md.disasm(code, IMAGEBASE + b):
        r = ins.address - IMAGEBASE
        mark = " <<<" if r in highlight else ""
        note = ""
        # annotate rip-relative string refs
        m = re.search(r"\[rip \+ (0x[0-9a-f]+)\]|\[rip - (0x[0-9a-f]+)\]", ins.op_str)
        if m:
            disp = int(m.group(1), 16) if m.group(1) else -int(m.group(2), 16)
            tgt = r + ins.size + disp
            if sec_of(tgt) == ".rdata":
                s = read_cstr(tgt)
                if s and s.isprintable() and len(s) > 1:
                    note = f"   ; \"{s}\""
            if not note:
                note = f"   ; ->0x{tgt:x} ({sec_of(tgt)})"
        print(f"  {r:08x}  {ins.bytes.hex():<20} {ins.mnemonic:<8} {ins.op_str}{note}{mark}")


if __name__ == "__main__":
    for arg in sys.argv[1:]:
        disas_func(int(arg, 16), label=f"@{arg}")
