"""Scan a code region for immediates of interest, with correct x86 encodings."""
import struct, re, sys
from disas import d, rva2off, off2rva, sec_of, FUNCS, func_of, secs

LO = int(sys.argv[1], 16) if len(sys.argv) > 1 else 0x560000
HI = int(sys.argv[2], 16) if len(sys.argv) > 2 else 0x580000
VALUES = [int(v) for v in (sys.argv[3].split(",") if len(sys.argv) > 3 else ["175", "170", "35", "34", "174"])]

lo_off, hi_off = rva2off(LO), rva2off(HI)
print(f"region rva 0x{LO:x}-0x{HI:x}  file 0x{lo_off:x}-0x{hi_off:x}")

for val in VALUES:
    imm32 = struct.pack("<I", val)
    pats = [
        (re.compile(b"[\xb8-\xbf]" + imm32), "mov r32, imm32"),
        (re.compile(b"\x41[\xb8-\xbf]" + imm32), "mov r8-15d, imm32"),
        (re.compile(b"\x3d" + imm32), "cmp eax, imm32"),
        (re.compile(b"\x81[\xf8-\xff]" + imm32), "cmp r32, imm32"),
        (re.compile(b"\xc7[\x80-\x87].{4}" + imm32, re.S), "mov [r+d32], imm32"),
        (re.compile(b"\xc7[\x40-\x47]." + imm32, re.S), "mov [r+d8], imm32"),
        (re.compile(b"\xc7\x05.{4}" + imm32, re.S), "mov [rip+d32], imm32"),
        (re.compile(b"\x68" + imm32), "push imm32"),
    ]
    # imm8 forms are sign-extended: only valid for val < 0x80
    if val < 0x80:
        pats += [
            (re.compile(b"\x83[\xf8-\xff]" + bytes([val])), "cmp r32, imm8"),
            (re.compile(b"\x6a" + bytes([val])), "push imm8"),
            (re.compile(b"\xc7[\x40-\x47]." + imm32, re.S), "mov [r+d8], imm32"),
        ]
    print(f"\n--- value {val} (0x{val:x}) ---")
    n = 0
    for rx, desc in pats:
        for m in rx.finditer(d, lo_off, hi_off):
            rva = off2rva(m.start())
            fn = func_of(rva)
            fs = f"fn 0x{fn[0]:x}" if fn else "fn ?"
            print(f"   rva 0x{rva:08x}  {desc:<20} {m.group().hex():<24} {fs}")
            n += 1
    print(f"   total {n}")
