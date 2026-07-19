# SoraSaveSlots

> **⚠️ AI-generated project.** The reverse engineering, the plugin code, and
> these docs were produced by an AI coding assistant (Claude), working from a
> human's runtime observations and steering. It has been tested against real
> saves but has **not** had independent human code review. Use at your own
> risk, and back up your saves before trying it — see [Safety](#safety).

Raises the save-slot limit in **The Legend of Heroes: Trails in the Sky 1st Chapter**
(2025 remake, `sora_1st.exe`) from 175 to up to **999 total slots**, and moves
the reserved slots (autosave, chapter clears) to the bottom of the list so the
manual slots run uninterrupted.

Ships as an **ASI plugin**. No game files are modified; the patch is applied to
the running process and disappears when the game closes.

---

## What it does

The stock game shows 175 save slots. That number isn't a constant anywhere in
the executable — every save/load dialog is described by a list of slot ranges
built at runtime, over a 300-slot backing store laid out like this:

| Slots | Use |
|---|---|
| 0–169 | manual saves |
| 170 | autosave |
| 171–174 | chapter-clear saves |
| 175–199 | **reserved** (a hidden dialog mode owns 180–199; 175–179 border it) |
| 200–299 | free — never referenced by anything |

This plugin rebuilds each dialog's range list as
**`[manual 0–169] [new 200–N] [reserved]`** — so the save, load, copy and
delete menus gain the new slots right after the manual block, with autosave and
chapter-clear entries moved to the bottom. The reserved regions keep their
exact stock behaviour.

With `MaxSlots` above 300 it additionally **replaces the game's 300-entry slot
array** with a bigger one (the array is inline in the accessor object, but
every consumer reads it through a pointer, so it can be redirected) and raises
the three hardcoded 300-slot loop bounds (icon pass, slot-enable pass, tile
creation). That unlocks slots up to 998 — the `save%03d` folder-name format is
the true ceiling.

> **Version history.** v1 hooked only the menu *display* builder: slots
> rendered but save/load confirm was silently ignored — the confirm handler,
> slot-enable pass and disk enumeration validate against the *source* range
> list. v2 patched the source (and moved the new block to 200+, off the
> reserved window). v3 adds the reorder and the >300 backing expansion.
> Details in [docs/FINDINGS.md](docs/FINDINGS.md) §9–12.

---

## Install

1. You need an ASI loader. Any of these work, because the plugin does its work in
   `DllMain`:
   - **Ultimate ASI Loader** (rename its dll to `xinput1_4.dll` or `version.dll`)
   - **Special K** — add to its config:
     ```ini
     [Import.SoraSaveSlots]
     Architecture=x64
     Role=ThirdParty
     When=PlugIn
     Filename=SoraSaveSlots.asi
     ```
2. Copy `SoraSaveSlots.asi` and `SoraSaveSlots.ini` into the game folder:
   `...\steamapps\common\Trails in the Sky 1st Chapter\`
3. Launch the game. Check `SoraSaveSlots.log` next to the `.asi` — it should
   list four `OK: hooked` lines.

**Back up your saves first**, at
`%USERPROFILE%\Saved Games\FALCOM\Trails in the Sky 1st Chapter\`.

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
  match or if the game built its save system before the plugin loaded.

At 999 expect slower menu opening: the game reads `detail.json` for every
listed slot on each dialog refresh, loads up to 999 `icon0.png`s at startup,
and builds 1000 slot tiles.

## Why slots 175–199 are skipped

They aren't free. The game has an alternate dialog mode whose slot window is
exactly `{180,199}` (20 slots), and the title-screen load dialog reserves
`{171,179}`. Saving ordinary games there could collide with those features, so
the plugin starts at 200. The hook detects the `{180,199}` dialog by content
and leaves it untouched.

---

## Safety

- **Fails closed.** On startup it verifies the exact bytes at every hook site.
  If the game updates and the range-list sites don't match, nothing is
  patched; if only the backing-expansion sites mismatch, it falls back to
  `MaxSlots=300`.
- **No game files touched.** Runtime patch only.
- **Reserved slots preserved.** Autosave (170), chapter-clear (171–174) and the
  reserved window (175–199) keep their original behaviour — they're only
  *displayed* after the manual/new slots now.

## Known limitations

- If you create saves in slots 200+ and later remove the plugin, those saves
  become invisible in-game until it's reinstalled. The files are untouched on
  disk.
- The >300 mode replaces a game-owned structure at runtime. It has been
  reasoned through carefully (see FINDINGS §12) but is inherently riskier
  than the ≤300 mode — **back up your saves**.
- Steam Cloud: each save is ~95 KB (mostly `icon0.png`). 999 slots ≈ 93 MB. If
  cloud sync starts failing, disable Steam Cloud for the game — local saves are
  unaffected.

---

## Repo layout

```
src/            ASI plugin source + build script (MSVC)
dist/           built SoraSaveSlots.asi + .ini
cheatengine/    CE Auto Assemble prototypes (v1 display hook, v2 source hook)
tools/          reverse-engineering tooling used to find all this
docs/           full findings writeup
```

Build with `src\build.bat` (needs VS 2022 with the C++ workload).
