# SoraSaveSlots

> **⚠️ AI-generated project.** The reverse engineering, the plugin code, and
> these docs were produced by an AI coding assistant (Claude), working from a
> human's runtime observations and steering. It has been tested against real
> saves but has **not** had independent human code review. Use at your own
> risk, and back up your saves before trying it — see [Safety](#safety).

Raises the save-slot limit in the **Trails in the Sky remakes** from stock
(175 in 1st Chapter, 170 in 2nd Chapter) to up to **999 total slots**, and
moves the reserved slots (autosave, chapter clears) to the bottom of the list
so the manual slots run uninterrupted.

One ASI plugin serves both games — the host executable is auto-detected at
load:

| Game | Host exe | Stock slots |
|---|---|---|
| Trails in the Sky 1st Chapter | `sora_1st.exe` | 175 |
| Trails in the Sky 2nd Chapter (demo) | `sora_2nd.exe` | 170 |

No game files are modified; the patch is applied to the running process and
disappears when the game closes.

---

## What it does

The slot cap isn't a constant anywhere in the executables. Every save/load
dialog carries a *param block* containing a list of `{start, end}` slot
ranges, and everything — display, confirm handler, disk enumeration, the
save/load executor — validates against that list. The list lives behind a
pointer stored inside the savedata accessor object.

The plugin hooks the instructions that store that pointer and rebuilds each
dialog's range list as **`[manual] [new 200–N] [reserved]`** — new slots
continue directly after the manual block, autosave/chapter-clear entries move
to the bottom, and the reserved regions keep their exact stock behaviour.

With `MaxSlots` above 300 it additionally **replaces the game's slot array**
with a bigger one (every consumer reads it through a pointer, so it can be
redirected) and raises the hardcoded icon/page loop bounds. That unlocks
slots up to 998 — the `save%03d` folder-name format is the true ceiling.

Per-game differences handled internally: accessor offsets (`+0x37B8` vs
`+0x37D8` param pointer, inline vs heap slot array), setter encodings, and
the backing-redirect site (ctor tail vs icon-pass entry via `.pdata`).

---

## Install

1. You need an ASI loader. Any of these work, because the plugin does its work
   in `DllMain`:
   - **Ultimate ASI Loader** (rename its dll to `xinput1_4.dll` or `version.dll`)
   - **Special K** — add to the game's profile `SpecialK.ini`:
     ```ini
     [Import.SoraSaveSlots]
     Architecture=x64
     Role=ThirdParty
     When=PlugIn
     Filename=C:\path\to\Plugins\SoraSaveSlots.asi
     Mode=
     ```
2. Copy `SoraSaveSlots.asi` and `SoraSaveSlots.ini` into a folder of your
   choice (per-game Special K `Plugins\` folders work well).
3. Launch the game. Check `SoraSaveSlots.log` next to the `.asi` — it should
   list the resolved sites and one `OK:` line per hook.

**Back up your saves first**, under
`%USERPROFILE%\Saved Games\FALCOM\<game name>\`.

---

## Configuration

`SoraSaveSlots.ini`:

```ini
[SoraSaveSlots]
MaxSlots=999
```

New slots run from 200 to `MaxSlots-1`. Values above **999** are clamped
(`save%03d` folder names); values of 200 or less add nothing.

- **`MaxSlots` ≤ 300** — only the dialog range lists are touched. Safest.
- **`MaxSlots` > 300** — the slot-array replacement and loop-bound patches
  are applied too. This is the riskier half: it redirects a structure the
  game normally owns. It degrades safely (to 300) if any patch site doesn't
  match.

At 999 expect slower menu opening: the game reads `detail.json` for every
listed slot on each dialog refresh, loads up to 999 `icon0.png`s at startup,
and builds 1000 slot tiles.

## Why slots 175–199 are skipped

They aren't free. The games have alternate dialog modes whose slot windows
reach into 180–199 (system-data dialog `{180,199}`, title-screen load
`{171,179}`), and 170–174 belong to autosave/chapter clears. The hook detects
those dialogs by content and leaves them untouched.

---

## Safety

- **Fails closed.** On startup it verifies the exact bytes at every hook site.
  If the game updates and the range-list sites don't match, nothing is
  patched; if only the backing-expansion sites mismatch, it falls back to
  `MaxSlots=300`.
- **No game files touched.** Runtime patch only.
- **Reserved slots preserved.** Autosave and chapter-clear slots keep their
  original behaviour — they're only *displayed* after the manual/new slots.

## Known limitations

- Saves created in slots 200+ become invisible in-game if you remove the
  plugin. The files are untouched on disk.
- The >300 mode replaces a game-owned structure at runtime. Reasoned through
  carefully (see docs) but inherently riskier than ≤300 — **back up your
  saves**.
- Steam Cloud: each save is ~95 KB (mostly `icon0.png`). 999 slots ≈ 93 MB.
  If cloud sync starts failing, disable Steam Cloud for the game — local
  saves are unaffected.

---

## Repo layout

```
src/
  core.h/.cpp    shared engine: AOB scan, .pdata lookup, code caves,
                 register-preserving thunks, FixRanges / FixBacking,
                 install + verify flow
  sora1.cpp      sora_1st patterns + accessor offsets (resolve only)
  sora2.cpp      sora_2nd patterns + accessor offsets (resolve only)
  games.h        GameModule registry externs
  dllmain.cpp    entry point: log/ini bootstrap, exe detection, dispatch
  build.bat      MSVC build -> dist\SoraSaveSlots.asi
dist/            built SoraSaveSlots.asi + .ini
cheatengine/     CE Auto Assemble prototypes (v1/v2 era)
tools/           reverse-engineering tooling used to find all this
docs/            full findings writeups per game
```

### Adding a game

1. Copy `src/sora2.cpp` to `src/game_x.cpp`; swap the AOB patterns and the
   accessor offsets in its `resolve()`.
2. Add `extern const GameModule game_x;` to `src/games.h`.
3. Add `&game_x` to `kGames` in `src/dllmain.cpp`.
4. Rebuild with `src\build.bat`.

Everything else (range rebuild, backing expansion, verification, logging) is
shared in `core.cpp`.

Build with `src\build.bat` (needs VS 2022 with the C++ workload).
