# Ghidra GUI walkthrough — analyzing `sora_1st.exe`

Interactive GUI tutorial. Every click is against your real install and game.
Use with `docs/FINDINGS.md` — it has the RVAs you'll jump to.

Target: `Z:\SteamLibrary\steamapps\common\Trails in the Sky 1st Chapter\sora_1st.exe`
(11.3 MB, x64 PE, image base `0x140000000`).

> RVA → address: `image base + RVA`. The hooks from the plugin live at
> `+0x56C29A` etc., i.e. **`0x14056C29A`** in Ghidra. FINDINGS.md RVAs all
> translate the same way.

---

## 0. Launch

```bat
:: activate the venv FIRST so Ghidra boots with Python 3
C:\Users\Salvatore\scoop\persist\ghidra-venv\Scripts\Activate.ps1
& C:\Users\Salvatore\scoop\apps\ghidra\current\support\pyghidraRun.bat
```

If it's already running you can skip; PyGhidra console just won't be there.

---

## 1. First run: create a project + import

1. **File → New Project** → `Non-Shared Project`, name it `sora`, pick a folder
   (e.g. `C:\Users\Salvatore\Documents\Claudio\sora-saveslots\ghidra\`).
2. **File → Import File** → navigate to `sora_1st.exe` → **Select**.
   Leave format detection alone (auto-detects PE). **OK**.
3. Import dialog shows the binary; click **Yes** on "Analyze?".
4. **Analysis Options** dialog:
   - Keep defaults on. For this binary the important ones:
     - **Decompiler Parameter ID** (enabled by default)
     - **MSVC RTTI Analysis** (yes — FINDINGS says full RTTI present)
     - **Aggressive Instruction Finder**, **Windows PE** analysis
   - Click **Analyze**. Wait. ~11 MB, x64, pure .text — takes a minute or two.
5. Result: **CodeBrowser** tool opens with Listing (top), Decompiler (bottom
   right), Symbol Tree (top left), Program Trees (bottom left).

---

## 2. Orient yourself

- **Image base**: check **Window → Memory Map** — `0x140000000` as expected.
- **Symbols**: Symbol Tree → **Functions** → find classes. Search:
  - **Search → Program Text** for `savedata_manager` — FINDINGS says source
    paths are retained, so `sora::savedata::Manager` should be findable.
  - RTTI should name `fdk::savedata::AccessorWin32` etc. If functions look like
    `FUN_14056c000`, RTTI didn't fire — see §7.

---

## 3. Jump to a known hook site (the key move)

The plugin hooks four setters. Look at the first one:

1. **Navigate → Go To** (`G`) → type `14056c29a` → **Enter**.
   (Or `sora_1st.exe+56C29A` if you want the symbol-flavoured form.)
2. Listing shows code around the hook. In the Decompiler window, the enclosing
   function's C decompiles below. This is the `AccessorWin32` setter that the
   plugin re-routes — confirm you see the range-list fields the FINDINGS
   describe (data ptr at `param+0x70`, count at `+0x78`).

**Why this matters**: in Ghidra the *view* is static. The plugin's live-probe
findings (CE/probes) give you "what address wrote what" — Ghidra gives you the
surrounding logic. Cross-checking both = confidence.

---

## 4. Follow the vtable

1. **Navigate → Go To** → `1409d8750` (the `AccessorWin32` vtable from FINDINGS).
2. Listing shows a table of pointers. Right-click a pointer → **References →
   Show References To Address** (or **F** with cursor on it) to find every call
   through the vtable.
3. In Decompiler, double-click any function call to jump into it.

This is how you walk "who owns the slot array" from FINDINGS §3.

---

## 5. Find references (the bread and butter)

From any address:
- **References → Show References To Address** (`Ctrl+Shift+F`) — who reads/writes this.
- **References → Find References to** (right-click a function name) — callers.

Practical use: pick the `resize(300)` slot-array site from FINDINGS, find its
references, and you see every consumer the >300 patch redirects.

---

## 6. Emulate a function (no game running)

Ghidra 12 has a built-in pcode emulator (the Debugger tool's **Emulator**
window — same engine as the `SystemEmulation` library).

1. Launch the **Debugger** tool: from CodeBrowser, **Window → Debugger** (or
   the bug icon in the tool bar). This opens the Debugger in its own tool
   window.
2. In the Debugger, find the **Emulator** window (Debugger → **Window → Emulator**
   if it isn't already a tab; the Emulator uses the current program and lets you
   set a start address, run, and step pcode).
3. Set RIP/start to the function entry (e.g. the setter at `14056c29a`), set
   arguments in the registers window, then **Step** / **Run** and watch memory.
4. Simpler alternative: **Window → Script Manager**, type `Emu` — run the
   shipped templates `DebuggerEmuExampleScript.java` / `EmuDeskCheckScript.java`
   (they show the emulator API in Java) or `StandAloneEmuExampleScript.java`
   for a headless-style loop.

Great for confirming "does this setter actually write count at `+0x78`"
without launching the game.

---

## 7. PyGhidra console (Python 3 in the GUI)

1. **Window → Script Manager**.
2. **Manage Script Directories** (folder icon) → add `C:\Users\Salvatore\ghidra_scripts`.
   The 0xdea scripts now appear.
3. In Script Manager:
   - **Haruspex** (decompile everything to a file tree), **Rhabdomancer** (find
     calls to insecure functions) — select, press **Run**.
   - They're `.java` — Ghidra compiles+stays in the GUI. Or select them and
     **View Source** to read what they do.
4. For ad-hoc Python 3: Ghidra 12's PyGhidra console appears as an extra
   interpreter option in Script Manager's interpreter dropdown, or run scripts
   from the venv shell (headless) with `pyghidra.start(...)`.

---

## 8. RTTI didn't name everything? Fallbacks

If functions stay `FUN_140xxxxx`:

1. **Analysis → Auto Analyze** — re-run with **MSVC RTTI** ticked (sometimes
   the first pass skips it if a data-analysis option was off).
2. **Search → For Strings...** — FINDINGS says source paths are embedded.
   Search for `savedata_accessor_win32.cpp` → its xrefs often land near the
   matching functions; rename them by hand.
3. PDB: **Symbols → Load PDB File** only helps if `sora_1st.pdb` exists next to
   the exe (Steam build usually ships it). Check the game folder first.

---

## 9. Save your work

- **File → Save Project** — analysis state persists in the `.gpr`.
- If you rename/retag functions, they're in the project, not the exe. Nothing
  touches the game on disk.

---

## Cheat sheet (GUI shortcuts)

| Action | Shortcut |
|---|---|
| Go To address | `G` |
| References to address | `Ctrl+Shift+F` |
| Decompiler | default in CodeBrowser |
| Function callers | right-click → References → Find References To |
| Rename function/variable | `L` |
| Show cross-references (listing) | `Ctrl+Shift+F` |
| Byte edit | `Ctrl+Shift+E` |
| Memory map | Window → Memory Map |
| Script Manager | `S` (or Window → Script Manager) |

---

## Suggested first session

1. Launch (§0), project + import + analyze (§1).
2. `G` → `14056c29a` — look at the setter the plugin hooks (§3).
3. `G` → `1409d8750` — the vtable, follow two calls (§4).
4. `Ctrl+Shift+F` on the 300-slot `resize` site (§5).
5. Try one **Emulate Function** (§6).
6. Run **Haruspex** from Script Manager (§7).

That covers the whole loop: locate → understand → verify.
