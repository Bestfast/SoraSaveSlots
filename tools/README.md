# Tooling

The reverse-engineering tools used to find the save-slot cap. Kept because the
same tooling applies to any Falcom FDK-engine title.

## `analysis/` — static analysis (Python)

Requires `capstone` (`uv venv && uv pip install capstone`). Most scripts import
helpers from `disas.py`, so run them from this directory.

| Script | Purpose |
|---|---|
| `disas.py` | PE parser + capstone disassembler. Uses `.pdata` to get exact function bounds. Import target: `disas_func`, `func_of`, `rva2off`. |
| `whichfn.py` | Map an RVA to its containing function. |
| `xref.py` / `xa.py` | Find rip-relative references to a string literal / address. |
| `disp.py` | Find every instruction using a given struct displacement (e.g. `+0x4730`). |
| `lea_scan.py` | Find code taking the *address* of a field (`lea reg,[reg+off]`). |
| `find_alloc.py` | Narrower displacement scan. |
| `scan_region.py` / `raw_imm.py` | Scan a range for immediates, **with correct x86 encodings** (see the sign-extension warning in FINDINGS.md). |
| `globals.py` | Find `.data`/`.rdata` globals holding a value *and* referenced by code. |
| `imports.py` / `dlls.py` | Import table dump (used to pick IAT hook targets). |
| `dump.py` | Hex dump by RVA. |
| `fpac.py` / `ext.py` / `pac.py` | **FPAC archive** parser and extractor for the game's `.pac` files. |
| `Decomp.java` / `DecompRange.java` | Ghidra headless scripts: decompile an address, or a whole RVA range, to C. |

Ghidra headless example:

```bat
set JAVA_HOME=%USERPROFILE%\scoop\apps\temurin21-jdk\current
analyzeHeadless.bat <proj> sora -process sora_1st.exe -noanalysis ^
  -scriptPath . -postScript Decomp.java 0x591020
```

## `probes/` — runtime instrumentation (C++)

Injected DLLs. Build with MSVC (`cl /O2 /MT /EHa /LD probeN.cpp`).
`inject.exe` waits for `sora_1st.exe` and injects a DLL — nothing is written to
the game folder.

| Probe | Purpose |
|---|---|
| `soraprobe.cpp` | IAT-hooks `CreateFileW`/`GetFileAttributes*`/`FindFirstFile*` and logs every `savedata` path with the caller's RVA. **This is what revealed the 300-slot icon pass and the 0–174 enumeration.** |
| `probe2.cpp` | Finds a live object by vtable and dumps every field matching given values. |
| `probe6.cpp` | Whole-process value scan with co-location filtering. |
| `probe12.cpp` | Hardware breakpoints (DR0–3) via VEH — the "find what accesses" equivalent. |
| `probe13.cpp` | Hardware breakpoint on one known address + struct dump. |

### Two hard-won warnings

1. **Never run these alongside Cheat Engine.** Both use the CPU's 4 debug
   registers. A probe re-arming them on a timer silently wipes CE's breakpoints,
   which looks exactly like "the breakpoint never fires".
2. When enumerating threads to set debug registers, **skip
   `GetCurrentThreadId()`** — otherwise the worker suspends itself and deadlocks.

For this particular job, Cheat Engine alone (cursor scan → "find out what writes"
→ Memory Viewer → Auto Assemble) was faster than any of the custom probes. The
probes earn their keep for the API-level tracing in `soraprobe.cpp`.
