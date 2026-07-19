"""Static recon on sora_1st.exe: locate save-slot constants and string xrefs."""
import struct, re, sys

EXE = r"Z:\SteamLibrary\steamapps\common\Trails in the Sky 1st Chapter\sora_1st.exe"
d = open(EXE, "rb").read()

pe = struct.unpack_from("<I", d, 0x3C)[0]
nsec = struct.unpack_from("<H", d, pe + 6)[0]
optsz = struct.unpack_from("<H", d, pe + 20)[0]
imagebase = struct.unpack_from("<Q", d, pe + 24 + 24)[0]
secs = []
base = pe + 24 + optsz
for i in range(nsec):
    o = base + i * 40
    name = d[o:o + 8].rstrip(b"\0").decode()
    vs, va, rs, ro = struct.unpack_from("<IIII", d, o + 8)
    secs.append((name, va, vs, ro, rs))

print(f"imagebase=0x{imagebase:x}")
for s in secs:
    print(f"  {s[0]:<8} rva=0x{s[1]:08x} vsize=0x{s[2]:08x} raw={s[3]} rsize={s[4]}")


def off2rva(off):
    for name, va, vs, ro, rs in secs:
        if ro <= off < ro + rs:
            return va + (off - ro)
    return None


def rva2off(rva):
    for name, va, vs, ro, rs in secs:
        if va <= rva < va + max(vs, rs):
            return ro + (rva - va)
    return None


def sec_of(rva):
    for name, va, vs, ro, rs in secs:
        if va <= rva < va + max(vs, rs):
            return name
    return "?"


text = next(s for s in secs if s[0] == ".text")
TEXT_LO, TEXT_HI = text[3], text[3] + text[4]

# ---------------------------------------------------------------- strings
print("\n=== target strings ===")
targets = {}
for lit in [b"%s%03d\x00", b"savedata\x00", b"sdmem\x00", b"user.dat\x00",
            b"detail.json\x00", b"icon0.png\x00", b"save\x00",
            b".?AVManager@savedata@sora@@\x00",
            b".?AVAccessorWin32@savedata@fdk@@\x00",
            b"savedata_manager.cpp", b"savedata_saveload_state.cpp"]:
    for m in re.finditer(re.escape(lit), d):
        rva = off2rva(m.start())
        if rva is None:
            continue
        targets.setdefault(lit, []).append(rva)
    got = targets.get(lit, [])
    print(f"  {lit!r:<40} -> {[hex(x) for x in got][:6]} ({sec_of(got[0]) if got else '-'})")

# ------------------------------------------------- rip-relative xref scan
# LEA r64, [rip+d32] encodings: REX.W 8d /r with mod=00 rm=101
LEA_PREFIXES = {}
for rex in (0x48, 0x4C):
    for modrm in (0x05, 0x0D, 0x15, 0x1D, 0x25, 0x2D, 0x35, 0x3D):
        LEA_PREFIXES[bytes([rex, 0x8D, modrm])] = None


def xrefs_to(target_rva, limit=40):
    """Find LEA rip-relative references into .text pointing at target_rva."""
    out = []
    for pat in LEA_PREFIXES:
        start = TEXT_LO
        while True:
            i = d.find(pat, start, TEXT_HI)
            if i < 0:
                break
            start = i + 1
            disp = struct.unpack_from("<i", d, i + 3)[0]
            nxt = off2rva(i + 7)
            if nxt is None:
                continue
            if nxt + disp == target_rva:
                out.append((off2rva(i), pat.hex()))
                if len(out) >= limit:
                    return out
    return out


for lit in [b"%s%03d\x00", b"savedata\x00", b"sdmem\x00"]:
    for rva in targets.get(lit, [])[:3]:
        xs = xrefs_to(rva)
        print(f"\n=== xrefs to {lit!r} @0x{rva:x} ({len(xs)}) ===")
        for r, p in xs:
            print(f"   lea @ rva 0x{r:x}  (file 0x{rva2off(r):x})  enc {p}")

# ------------------------------------------------- immediate constant scan
print("\n=== immediate constant sites in .text ===")
for val, label in ((175, "175 MAX?"), (170, "170 RESERVED_BASE?"), (174, "174"), (176, "176")):
    pats = []
    pats.append((re.compile(re.escape(bytes([0x83])) + b"[\xf8-\xff]" + bytes([val])), "cmp r32,imm8"))
    pats.append((re.compile(b"[\xb8-\xbf]" + struct.pack("<I", val)), "mov r32,imm32"))
    pats.append((re.compile(b"\x3d" + struct.pack("<I", val)), "cmp eax,imm32"))
    pats.append((re.compile(b"\x6a" + bytes([val])), "push imm8"))
    pats.append((re.compile(b"\x41[\xb8-\xbf]" + struct.pack("<I", val)), "mov r8-r15d,imm32"))
    print(f"\n--- value {val} ({label}) ---")
    n = 0
    for rx, desc in pats:
        for m in rx.finditer(d, TEXT_LO, TEXT_HI):
            rva = off2rva(m.start())
            print(f"   rva 0x{rva:08x}  file 0x{m.start():08x}  {desc:<18} {m.group().hex()}")
            n += 1
    print(f"   total: {n}")
