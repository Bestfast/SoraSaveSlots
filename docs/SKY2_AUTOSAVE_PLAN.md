# Trails in the Sky 2nd Chapter — multi-slot autosave: investigation + plan

Status: **feasible, not implemented.** All facts below are from static analysis of the
shipping build; no game file was modified, nothing was injected (the game was running).

Target binary: `Z:\SteamLibrary\steamapps\common\Trails in the Sky 2nd Chapter\sora_2nd.exe`
- file version `1.3.2.0`, size 13,464,576, SHA256 `D8B2911D…F8AAF`
- PE image base `0x140000000`. `.text` RVA `0x1000`, raw `0x400`, so **rva = fileoff + 0xC00**.

Reference demo build (for the existing mod): `…\Trails in the Sky 2nd Chapter Demo\sora_2nd.exe`
(SHA256 `BF435021…887A91`) — this is what `src/sora2.cpp` was written against.

---

## 1. Answer

Yes. The autosave target slot is **not** baked into the disk/save code — it is a single
runtime value (a slot index) handed to one accessor call, and the save/load menus are
driven entirely by an editable range list the mod already intercepts. So the autosave
can be spread over N slots without touching the save format.

The cost is menu-wiring + one writer hook, plus relocating/avoiding the chapter-clear
slots (171–179), which currently sit exactly where a naive autosave run would go.

---

## 2. Slot map of the shipping build (measured from the save folder + the range builders)

| Slots | Use | Source |
|---|---|---|
| 0–159 | manual saves | load/save dialog ranges `{0,159}` |
| 170 | **autosave (1 slot)** | range `{170,170}` + hardcoded write target |
| 171–179 | chapter-clear saves (index 0–8) | `0x4B6370` computes `171 + chapterIndex` |
| 180–199 | reserved "system" dialog window | load dialog mode 1 → `{180,199}` |
| 200–299 | free (stock backing is 300 entries) | mod's `{200,N-1}` block |
| 300–998 | free only after the mod's backing redirect | `MaxSlots>300` |

User's current disk state: `save000`–`save062`, `save170`, `save171`, `save172`,
`sdmem000`. Each save is a **single `savedata` blob** in its folder (no `detail.json` /
`icon0.png`, unlike the 1st game). Save ≈ 130 KB.

---

## 3. Mechanism (verified)

### 3.1 Where the autosave write slot comes from
Both autosave paths build a 0x6B0-byte dialog param block, copy it into `accessor+8`,
then call **`AccessorWin32` vtable slot +0x30** with the target slot in `EDX`.

- **State 6 — plain autosave, hardcoded slot 170** (`sora::savedata` state machine,
  RVA range `0x444990–0x444DA3`):
  ```
  0x444C0D   BA AA 00 00 00          mov edx, 0AAh        ; slot 170  <-- single autosave
  0x444C12   FF 50 30                call qword ptr [rax+30h]
  ```
- **State 8 — explicit-slot autosave** (`0x444DB0–0x4450FD`):
  ```
  0x444FCA   8B 93 98 0F 40 00       mov edx, [rbx+400F98h]   ; slot from state field
  ...        FF 50 30                call qword ptr [rax+30h]
  ```
  `state+0x400F98` is written only by the chapter-clear helper:
  ```
  0x4B63D8   8D 88 AB 00 00 00       lea ecx, [rax+0ABh]      ; 171 + index
  0x4B63E5   B9 AB 00 00 00          mov ecx, 0ABh            ; fallback = 171
  → 0x4B63F1 89 8F 98 0F 40 00       mov [rdi+400F98h], ecx   ; then enter state 8
  ```
  So 171–179 are chapter-end saves, not extra autosaves.

### 3.2 Where the range lists are built (display/enable/confirm)
State machine dialog builders, all emitting `{start,end,flag}` at stride 12:

| RVA | Accessor method | Default (mode 0) ranges |
|---|---|---|
| `0x443DC0` | vtable +0x20 | `{170,170} {0,159} {171,179}` (load) |
| `0x444550` | vtable +0x28 | `{170,170} {0,159} {171,179}` (save) |
| `0x445370` | vtable +0x50 | `{0,159} {171,179} {170,170}` |

Other modes on the same builders: `{180,199}` (system), `{200,203}` (a 4-slot system
list), and cross-save entries that read the 1st game's folder.

The mod already hooks the five `mov [reg+37D8h], rN` accessor stores and rewrites these
lists to `[manual <170][{200..hi}][reserved]`. Existing demo AOBs from `src/sora2.cpp`
still match the shipping exe **uniquely**:

| Pattern (bytes) | Shipping hits |
|---|---|
| `4C 89 81 D8 37 00 00` (×3) | 3 |
| `49 89 B6 D8 37 00 00` | 1 |
| `48 89 91 D8 37 00 00` | 1 |
| `41 81 FE 2C 01 00 00` (icon bound `cmp r14d,300`) | 1 |
| `41 83 FD 3C 0F 8C` (page1) | 1 |
| `41 83 FC 3C 0F 82` (page2) | 1 |

So the current `SoraSaveSlots.asi` (v5.1) should attach to the shipping exe as-is; it is
simply **not installed** (`plugins\` only contains `OptiPatcher.asi`).

### 3.3 Key AOBs for the new work (all currently unique)

| Name | Bytes | RVA |
|---|---|---|
| autosave state6 slot imm | `BA AA 00 00 00 FF 50 30` | `0x444C0D` |
| autosave state8 slot load | `8B 93 98 0F 40 00` | `0x444FCA` |
| chapter slot calc | `8D 88 AB 00 00 00 83 F8 08` | `0x4B63D8` |
| chapter slot fallback | `B9 AB 00 00 00` | `0x4B63E5` |
| `{170,170}` range literal (load/save) | `C7 44 24 40 AA 00 00 00 C7 44 24 44 AA 00 00 00` | `0x443F37` / `0x4446A8` |
| state+0x400ECC store (load/save) | `C7 83 CC 0E 40 00 AA 00 00 00` | `0x443F2F` / `0x44469E` |

Not yet located statically: the **vtable+0x30 function** (the actual "save to slot N"
implementation). Resolve it at runtime (break at `0x444C12`, read `RAX` → `[RAX+0x30]`)
or statically via the AccessorWin32 RTTI/COL. Needed only for the "hook the writer once"
variant below.

---

## 4. Design options

**A — Native rotation.** Hook `0x444C0D` (and the `==170` case of state 8) so the game
writes each autosave to `170 + (k mod N)`. Add a range `{170, 170+N-1}` to the rebuild.
- Pro: no file copying; the game owns everything.
- Con: slot 170 stops always being the newest → breaks anything that hardcodes 170 as
  "latest" (game-over *Load latest*, title *Continue*). Must verify those first;
  chapter-clear 171–179 must be relocated or the autosave history collides with them.

**B — Mirror history (recommended).** Leave the native writer untouched (still always
writes slot 170), and after each successful autosave copy `save170\savedata` →
`save<HIST+k mod N>\savedata`; expose the history block via the range rebuild.
- Pro: "latest"/"Continue" behaviour is bit-for-bit unchanged; zero risk to the write
  path or save format; a copy is one small file.
- Con: needs one post-save trigger (hook `0x444C0D` to set a pending flag, then act when
  the state function returns; or hook the vtable+0x30 saver once) and a rotation counter
  persisted across runs. History slots are ordinary saves to the game (icon will be the
  manual icon unless the icon picker is also patched).

**C — Relocate chapter clears, then rotate natively into 171–179.** Patch `0x4B63D8`/
`0x4B63E5` so chapter clears go to e.g. 220–228, change the reserved range accordingly,
and give 171–179 to autosave history (they already render with the `clear/*` icon family,
which may need a small picker patch to show `auto/*`). More sites touched, best-looking
result, most risk.

---

## 5. Recommended plan (phased, all offline until the user stops playing)

**Phase 0 — baseline (2 min, requires a game restart, so do it when idle)**
1. Verify `%USERPROFILE%\Saved Games\FALCOM\Trails in the Sky 2nd Chapter\savedata`
   is backed up (they have a `backup-…` convention from the 1st game).
2. Copy the current `dist\SoraSaveSlots.asi` + `.ini` into the shipping `plugins\`,
   set `MaxSlots=200` (ranges only, no backing), launch once, confirm the log shows the
   5 setter hooks + that the menu still lists 0–159 / autosave / chapter clears.
   This proves the AOBs on v1.3.2.0 and gives a rollback point. Then remove the .asi.

**Phase 1 — resolve remaining addresses**
- Confirm the `0x444C0D` / `0x444FCA` sites fire on a real autosave (CE hardware BP;
  watch `EDX`). This also tells you whether plain autosaves are state 6 or state 8.
- Resolve the vtable+0x30 saver function (needed only for option B/C).
- Find the icon-category picker that chooses `auto/*` vs `clear/*` vs `save/*` by slot.

**Phase 2 — implement (option B unless Phase 1 shows "latest" is range-driven)**
1. `sora2.cpp`: add the new AOBs to `GameContext` (autosave-site, optional saver).
2. `core.cpp`: add `FixAutosaveRanges()` — append `{HIST, HIST+N-1, 1}` to the load/save
   range lists alongside the existing `{200..hi}` block. Pick `HIST` to avoid
   171–199 and the `{200,203}` system list (suggest `HIST = 200` with the manual block
   moved to `200+N`, **or** `HIST` in the 900s once backing is active). Add `AutosaveSlots`
   to the ini.
3. Rotation + copy: hook `0x444C0D`; on the save returning, copy `save170` to the next
   history slot. Persist the counter (oldest of `saveHIST..` by mtime, or a sibling file).
4. Guard `FailRanges`' existing "extend block starting at 200" branch so it can't swallow
   the game's `{200,203}` system list (latent bug: that branch runs before the
   `end>=180` skip). Needed if the new 200-based block is introduced.

**Phase 3 — verify (CE + a scratch save profile)**
- 3 autosaves → 3 history folders, all loadable, newest still in 170.
- Chapter clears still land on 171–179 and are loadable.
- Game-over *Load latest* and title *Continue* load the same save as before the mod.
- Save menu can't overwrite history slots (stock save dialog does not list 171+; keep
  parity).
- Stress: autosave in a map-transition-heavy area; confirm no per-open slowdown beyond
  the existing `MaxSlots` cost.

---

## 6. Risks / unknowns

- **"Latest save" hardcoding.** Unverified whether *Continue* / *Load latest* use slot 170
  or pick by mtime. If hardcoded, option A is unsafe and option B is required.
- **`{200,203}` system list collision** with the mod's `NEW_START=200` (see 2.4 above).
- **Icon routing** is by slot number; history slots outside 170 will render with the
  wrong icon unless the picker is patched or 171–179 are repurposed (option C).
- **Autosave frequency.** If autosave fires on every area change, N=10 keeps 10
  transitions only; a large N bloats the folder. Keep N small (5–10) and document it.
- **Game updates** shift every RVA; all sites must stay AOB-resolved (the codebase
  already fails closed on mismatch — keep that).
- **Steam Cloud** may sync the extra slots; harmless but expect the folder to grow.

## 7. Bottom line

The hook surface is small (one 5-byte immediate + one range entry), the save format is
untouched, and the mod already owns the menu range lists — so multi-slot autosave is a
contained change. The only real engineering risk is not the writer but the two
"newest autosave" shortcuts and the chapter-clear slot collision; both are resolvable
once Phase 1 confirms where `load latest` reads from.

---

# 8. Implementation (branch `feat/sky2-autosave-history`)

Chosen approach: **B (mirror history)** — the game's live autosave slot, its writer
and its "load latest"/Continue paths are all left exactly as stock; the plugin only
adds a rolling ring of copies and exposes it in the menu.

## What was added

- `core.h`: `AUTOSAVE_LIVE_SLOT=170`, `DEFAULT_AUTOSAVE_SLOTS=10`,
  `MAX_AUTOSAVE_SLOTS=64`; new `GameContext` fields `autoRva/autoLen/autoExpect/
  autoSlot/autoHistCount/autoHistStart`.
- `sora2.cpp`: resolves the autosave executor site by AOB
  `BA AA 00 00 00 FF 50 30` (unique → shipping RVA `0x444C0D`, file `0x44400D`,
  the `mov edx,0AAh` before `call [rax+30h]` in state 6). Resolved independently of
  the `>300` backing path, so it works with `backing` inactive.
- `core.cpp`:
  - `AutosaveHistoryTick()` — called from the cave just **before** the write;
    mirrors the *current* `save170` folder into the oldest/empty ring slot. De-dups
    by mtime+size (guards against twin hooks), re-entry guarded, copies every file
    in the folder. Never touches the live slot.
  - `FixRanges()` — caps the manual block below the ring (`hi = histStart-1`) and
    inserts a `{histStart..histStart+N-1}` range before the reserved tiles; raises
    the tile-content bound (`param+0xC`) to cover the ring. Also tightened the
    "extend our block" idempotency test to `n>=2` so it can never swallow the game's
    single-range `{200,203}` system list.
  - `ScanMaxSaveSlot()` — excludes the ring so the dynamic window is not inflated.
  - `installGame()` — installs the autosave hook (fails closed on AOB mismatch);
    code cave bumped 1024 → 4096.
- `dllmain.cpp`: `AutosaveSlots` ini key (default 10), ring start = `300 - N`,
  clamped to `[0,64]`; version bumped to v5.2.
- `dist/SoraSaveSlots.ini`: documents `AutosaveSlots`.

## Layout produced (ordered dialogs)

`[manual <170] [{200..hi}] [{ring}] [{170,170}] [{171,179}] [{180,199} dead]`

With `AutosaveSlots=10`, ring = `{290,299}` and the manual new block is capped at
`289`. Menu labels are position-based, so the ring tiles sit immediately above the
live autosave / chapter-clear tiles at the bottom.

## Still to verify at runtime (needs the game)

1. Hook fires (`autosave-hist: hook fired`) on a real field autosave, and the AOB
   resolves on v1.3.2.0 (log line `s2_autosave +0x444C0D`).
2. `save290`..`save299` fill and are loadable; visual icon of ring tiles.
3. Backing stays inactive (expected); `MaxSlots` still effectively 300.
4. No hitch at autosave from the folder copy (~130 KB, game thread).

Rollback: set `AutosaveSlots=0` (ranges + hook installed but ring never populated),
or restore the previous `SoraSaveSlots.asi`.
