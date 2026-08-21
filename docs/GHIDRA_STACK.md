# Ghidra 12.1.2 RE Stack — usage guide

Static-analysis lane for `sora_1st.exe` (and any FDK-engine title). Complements
`tools/` (CE + probes + capstone scripts) with a real decompiler.

Ghidra install: `C:\Users\Salvatore\scoop\apps\ghidra\current` (12.1.2, scoop).

---

## What's installed

| Tool | Where | Purpose |
|---|---|---|
| PyGhidra (built-in) | Ghidra Feature + venv | Native Python 3 scripting in Ghidra |
| GhidraGo (built-in) | Ghidra Feature | Go binary analysis (unused for FDK, free) |
| Built-in Emulator | Ghidra Feature | pcode emulation of functions |
| Native RTTI/vftable analysis | Ghidra | MSVC RTTI, auto vftable |
| 0xdea scripts | `%USERPROFILE%\ghidra_scripts` | pseudocode extract, unsafe-call finder |
| oneiromancer + aidapal | `.cargo\bin\oneiromancer.exe` + ollama | LLM pseudocode analysis |

Deliberately NOT installed: Ghidrathon (archived → PyGhidra replaces), GhidraEmu
(stale, 11.0 max), ClassyGhidra (archived), UAssetGhidra (dead repo + FDK is not
UE), augur (IDA 9.x + idalib only — the Ghidra equivalent is Haruspex).

---

## 1. PyGhidra (Python 3 in Ghidra)

### GUI mode

```bat
:: activate the venv, then launch Ghidra via its PyGhidra wrapper
C:\Users\Salvatore\scoop\persist\ghidra-venv\Scripts\Activate.ps1
& C:\Users\Salvatore\scoop\apps\ghidra\current\support\pyghidraRun.bat
```

Venv: Python 3.12 + JPype1 + pyghidra 3.1.0. In the GUI Script Manager, `.py`
scripts now run under CPython 3; the interpreter drop-down lists `PyGhidra`.

### Headless mode

```bat
set GHIDRA_INSTALL_DIR=C:\Users\Salvatore\scoop\apps\ghidra\current
C:\Users\Salvatore\scoop\persist\ghidra-venv\Scripts\python.exe myscript.py
```

### Quick test

```python
import pyghidra
launcher = pyghidra.start(install_dir=r"C:\Users\Salvatore\scoop\apps\ghidra\current")
# launcher.open_program / get program, run GhidraScript, etc.
```

The repo's `tools/analysis/Decomp.java` / `DecompRange.java` still work via the
plain headless analyzer (see `tools/README.md`); PyGhidra is the interactive
alternative.

---

## 2. 0xdea scripts (`%USERPROFILE%\ghidra_scripts`)

| Script | What it does |
|---|---|
| `Haruspex.java` | Extracts every function's decompiler pseudocode to a file tree |
| `Rhabdomancer.java` | Locates all calls to potentially insecure functions |
| `FOX_alpha.java` | iOS/Objective-C xref repair (irrelevant here) |
| `ResolveMipsN32LinuxSyscallsScript.java` | MIPS N32 syscalls (irrelevant here) |

Install = drop into `%USERPROFILE%\ghidra_scripts` (done). Script Manager auto-
discovers on next launch; or run headless with `-scriptPath`.

### Useful for sora_1st.exe

```bat
set JAVA_HOME=%USERPROFILE%\scoop\apps\temurin21-jdk\current
analyzeHeadless.bat <proj> sora -process sora_1st.exe -noanalysis ^
  -scriptPath C:\Users\Salvatore\ghidra_scripts -postScript Haruspex.java
```

This dumps decompiled pseudocode for the whole binary — the same material the
live `tools/analysis` scripts found via CE, but with full decompiler output.

---

## 3. Built-in features (nothing to install)

- **Emulator / Debugger**: select function, Debugger → Emulate — trace pcode
  without running the game. Good for confirming what a candidate function
  computes (e.g. slot-array bounds).
- **RTTI + vftable**: MSVC RTTI analysis runs automatically; check the
  *VTables* window for FDK engine classes. Helps name `CGame*`/`CSaveData*`
  style structures by their vtable.

---

## 4. oneiromancer + aidapal (LLM pseudocode analysis)

Pipeline: decompiler pseudocode → aidapal (local LLM) → function name + comment
+ variable renames.

### aidapal is hosted in the ollama container

```bat
cd C:\Users\Salvatore\Documents\Claudio\windows-ollama
docker compose up -d
```

Model `aidapal:latest` (Mistral-7b fine-tune, 4.4 GB, 100% GPU on RTX 5070 Ti).
`qwen3:14b` also present (paperless). OLLAMA_MAX_LOADED_MODELS=1 so they don't
share 16 GB VRAM; they swap.

### Run oneiromancer

```bat
set OLLAMA_MODEL=aidapal
set OLLAMA_BASEURL=http://127.0.0.1:11434
C:\Users\Salvatore\.cargo\bin\oneiromancer.exe <pseudocode-file.c>
```

### Typical loop

1. `Haruspex.java` dumps pseudocode tree for `sora_1st.exe`.
2. Pick a function (e.g. the slot-range-list builder found by the live probes).
3. Feed its pseudocode file to oneiromancer → name + comment suggestions.
4. Verify against the runtime findings in `docs/FINDINGS.md` before accepting.

### Caveats

- aidapal answers only for *Hex-Rays-style* pseudocode. Ghidra's decompiler
  output is close enough to work; quality degrades on big functions.
- Output must be valid JSON `{function_name, comment, variables[]}` — tool
  parse fails otherwise. aidapal is tuned for that; stock models (Gemma) are
  not.
- If ollama container is down: `documents.lan` and other hosts do NOT run it.
  It lives in this repo's `windows-ollama` compose, on this Windows PC.

---

## Quick reference

| Task | Command |
|---|---|
| Launch Ghidra GUI with Py3 | `support\pyghidraRun.bat` (from venv) |
| Start ollama | `docker compose up -d` in `windows-ollama` |
| Decompile whole exe | headless `analyzeHeadless ... -postScript Haruspex.java` |
| LLM-analyze one function | `oneiromancer.exe file.c` |
| List models | `docker compose exec ollama ollama list` |
