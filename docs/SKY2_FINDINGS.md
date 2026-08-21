# Trails in the Sky 2nd Chapter — save-slot cap: RE notes

Status: **SOLVED (demo build).** Plugin v4 supports both games from one ASI
(auto-detects `sora_1st.exe` / `sora_2nd.exe`). Range rebuild works up to 300;
the >300 backing expansion is implemented for sora_2nd as well (icon-entry
redirect + icon/page bound raises), giving the same 999-slot ceiling.

Target binary: `Z:\SteamLibrary\steamapps\common\Trails in the Sky 2nd Chapter Demo\sora_2nd.exe`
Image base 0x140000000 (static) / ASLR at runtime. Offsets below are demo-specific
and will shift at full-game launch — all hook sites are AOB-resolved at load,
so only the patterns (not RVAs) need to survive.

---

## The mechanism (confirmed live + static)

Same range-list model as Sky 1st, different storage offset:

- Param block pointer stored at **`accessor+0x37D8`** (Sky1: `+0x37B8`).
- Inside the block: `+0x70` = range array ptr, `+0x78` = count.
- Range element stride 12: `{u32 start, u32 end, u32 flag}` (flag is a clean 1,
  unlike Sky1's bool-in-float artifact).
- Live ranges in the demo save dialog: `{170,170}`, `{0,159}`, `{171,179}`
  = **170 slots** (menu shows 1–170).
- Consumers (all walk `[+0x37D8]→{+0x70,+0x78}`): enable/grey-out pass
  `FUN_140694aa0`, tile grid `FUN_14069abf0`, per-slot detail loaders
  `FUN_140699bb0/d90`, and the save/load executor `FUN_140696d80`
  (called via `FUN_14069e140` with `[+0x37D8]`/`[+0x37E0]`).

### Setter hooks (5 sites, each 7 bytes)

| RVA | Bytes | Store |
|---|---|---|
| `0x692597` | `49 89 B6 D8 37 00 00` | `mov [r14+37D8h], rsi` |
| `0x692B1A` | `4C 89 81 D8 37 00 00` | `mov [rcx+37D8h], r8` |
| `0x692E1A` | `4C 89 81 D8 37 00 00` | `mov [rcx+37D8h], r8` |
| `0x692F0A` | `4C 89 81 D8 37 00 00` | `mov [rcx+37D8h], r8` |
| `0x693263` | `48 89 91 D8 37 00 00` | `mov [rcx+37D8h], rdx` |

The plugin hooks all five; the cave calls a C helper that rebuilds the list as
`[manual <170] [{200, N-1}] [reserved >=170]`. The RSI setter gets its own
thunk variant that push/pops RSI (non-volatile).

### Backing (>300)

Unlike Sky1's inline array, sora_2nd keeps the slot array on the heap:
data ptr `accessor+0x23B0`, count `accessor+0x23B8` (24-byte entries, entry+0x10
= "has data" byte). Every consumer loads the pointer, so redirecting it is safe:

- **Redirect point**: icon-pass entry `FUN_140694530` (`48 89 5C 24 18 ...`,
  RCX = accessor on entry). The thunk replays the first 5 bytes, calls
  `FixBacking(rcx)`, jumps back. Idempotent: accessors already running on one
  of our buffers are skipped (up to 8 tracked).
- **Icon-pass bound** `0x69498D`: `41 81 FE 2C 01 00 00` (`cmp r14d, 300`),
  imm32 at +3 patched in place to N.
- **Enable pass** `0x694F2B`: `41 83 FD 3C / 0F 8C` (`cmp r13d,60 / jl`) —
  re-encoded in a cave as `cmp r13d, ceil(N/5)`.
- **Tile create** `0x69B32E`: `41 83 FC 3C / 0F 82` (`cmp r12d,60 / jc`) —
  same cave treatment.

Buffer sized `ceil(N/5)*5` entries like Sky1 (tile loop indexes pages*5);
spare entries stay zero ("no icon"). No separate max field needed — the
executor validates through the ranges, and cleanup iterates count.

## Method

Ghidra MCP (static: decompiles of the accessor driver `FUN_140693310`, enable
pass, tile grid, executor) + Cheat Engine MCP (runtime: hardware breakpoints on
the driver/icon pass captured the live accessor `RCX`; dumps of `+0x23B0`
array, `+0x3120` param copy and the `+0x37D8` range block confirmed every
offset before any patch was written).

---

# Historical scouting notes (pre-solution)

Older partial findings from the first demo pass; kept for context.

---

## What survives vs Sky 1st (same codebase `sora_r_sc`)

- Engine classes identical, RTTI intact:
  - `.?AVAccessorWin32@savedata@fdk@@` (RTTI name @ `0x140c48c20`)
  - Source paths `E:\sora_r_sc\...\savedata_accessor_win32.cpp`,
    `...\savedata_saveload_state.cpp`
- Slot array: **24-byte entries, 300 default** (`if (299 < uVar3) return` in the
  icon pass).
- Page-loop + icon encodings **unchanged** (found by AOB in demo):
  - icon `cmp esi,300` `81 FE 2C 01 00 00` — RVA `0x69498E`
  - page1 `41 83 FD 3C 0F 8C` (enable/grey-out, r13d) — RVA `0x694F2B`
  - page2 `41 83 FC 3C 0F 82` (tile-create, r12d) — RVA `0x69B32E`
  - loop targets rel32 identical to Sky1 pattern.

## What changed (the rework)

- **Slot-array data ptr moved** `+0x23A0` → **`+0x23B0`** (accessor-relative).
  Verified live: accessor object +0x23B0 holds a valid heap pointer to
  24-byte-entry slot array.
- **Param-store offset `0x37B8` gone.** Sky 1st setters were `mov [rcx+37B8h],r8`;
  the only `0x37B8` byte hits in demo `.text` are `mov eax,37` false positives.
- **Range building refactored into a state machine.**
  `StateDialogOpen` (in `savedata_saveload_state.cpp`) builds a ~`0x6b0`-byte
  param struct and hands it to an **accessor vtable method at vtable+0x40**
  (`(**(code **)(*this + 0x40))(this, 0, &param_struct)`). Not the old direct
  `mov [rcx+37B8],r8` stores.

## AccessorWin32 (from decompile of driver FUN_140693310, param_1 = accessor)

Key offsets (accessor-relative), partially confirmed live:

| Offset | Meaning | Confirmed |
|---|---|---|
| +0x0000 | vtable | ✓ (0x7FF6DD120D68 = base+0xB20D68) |
| +0x08   | (member passed as param_2 to icon pass) | ✓ |
| +0x23B0 | slot-array data ptr (24B entries) | ✓ |
| +0x23E0/+0x23E8 | a vector (ptr,size) — suspected range/enum list | partial |
| +0x30A0 | cache of hash-of-vector (FUN_1406666d0) | |
| +0x30F4/+0x30F8 | cursor `%5`/`/5` page math (computed from count) | |
| +0x38A0 | uVar4 = *count?* — page math source | **ambiguous** |
| +0x38A8 | 2 at save-menu time → else path taken, count NOT via +0x38A0 | |
| +0x37E8 | state/lock flag | |
| +0x3110 | dirty/id tracking | |

`FUN_140693310` = accessor update/driver. No direct callers found (vtable-dispatched).
Icon pass `FUN_140694530` called with `(accessor, accessor+8)`, walks slot array
0..299 with 0x18 stride, stores per-slot texture handle at `[offset + [accessor+0x23B0]]`.

## Why it's NOT a quick port

1. Real slot-count field unidentified (driver's `+0x38a0` reads 0; `+0x38a8`=2
   diverts to an alternate setup path).
2. The param/range list + its 4 setters need re-discovery (new vtable+0x40
   mechanism).
3. The accessor ctor (which sets slot-array ptr, count, max — needed for the
   >300 backing redirect) not yet pinned; no `mov [reg+23B0]` matching the
   Sky1 pattern found by scan.
4. >300 backing redirect (count/max + icon/page-loop bounds) is the risky half;
   without exact count/max fields it's a corruption risk, not "just inert".

## Recommended path

**Defer to full-game release.** Offsets here are demo-specific and will shift at
launch (Sky 1st's own did between May-2025 and 1.7.0.0). When the full game is
out and stable:
1. Re-open `sora_2nd.exe` in Ghidra (project `Sora`).
2. Find accessor ctor (initializes +0x23B0, count, max).
3. Find the 4 param-list setters (new vtable+0x40 path).
4. Reuse Sky1 plugin structure (range rebuild + optional backing redirect) with
   the new offsets; add a second AOB table / variant const table for sky_2nd.

Sky 1st and Sky 2nd both ship from the single `SoraSaveSlots.asi` (v4).
