# Trails in the Sky 1st Chapter — save-slot cap: reverse-engineering notes

Target: `sora_1st.exe`, 2025 remake, x64, 11.8 MB (Steam build, May 2025).
All RVAs below are relative to the module base.

---

## Summary

The 175-slot cap is **not a constant**. It is the sum of a list of
`{start, end}` slot ranges that the save menu rebuilds every time it opens.
Raising the cap means changing those ranges; the ceiling is 300 because the
backing slot array is `resize(300)`'d.

---

## 1. The binary is friendly to analysis

- No DRM wrapper; plain 6-section PE, statically analysable.
- **Full MSVC RTTI and retained source paths**, e.g.
  `D:\sora_r_fc\src\sora\src\savedata\savedata_manager.cpp`,
  `D:\sora_r_fc\src\fdk\src\core\savedata\win32\savedata_accessor_win32.cpp`.
- Relevant classes: `sora::savedata::Manager`, `fdk::savedata::AccessorWin32`
  (vtable RVA `0x9d8750`), `fdk::savedata::AccessorCommonDialog`.
- UI layouts are discrete files: `layout/savemenu_base.lay`, `savemenu_item.lay`,
  `savemenu_yesno.lay`, `auto_save.lay`.

## 2. Dead ends (recorded so nobody repeats them)

**There is no `175` or `170` anywhere.** Exhaustively checked:

- every valid x86 encoding of `cmp`/`mov`/`push` against 175/170 in `.text`
  (the only hits are orbment sprite IDs and a `std::string` allocator threshold);
- 357 decompiled functions across the savedata accessor, save-menu, and manager modules;
- aligned dword `175` in `.rdata`/`.data` referenced by any code — **zero**;
- `savemenu_base.lay`, `asset_config.json`, all 237 `.tbl` files;
- the live `AccessorWin32` object (36 KB scanned at runtime) — no field holds 175.

> ⚠️ Encoding gotcha that cost real time: `83 F8 AF` is **`cmp eax, -81`**, not
> `cmp eax, 175`. `83 /7 ib` sign-extends its imm8, so any comparison against
> 175 or 170 *must* use the imm32 form (`81 /7 id` or `3D id`). A naive byte scan
> produces a flood of false positives.

Also ruled out by direct runtime experiment:
- Field `+0x33A8` (which holds `35`, tempting because 35 × 5 = 175) — poked to 60,
  value stuck, cap unchanged. Not the row count.
- Field `+0x1F4454` (holds `170` in save mode) — write-only, values 20/170/180/200
  by menu mode. A UI layout parameter, not a slot count.
- "Rolling window" theory (`used + 35`): disproved by adding 21 contiguous saves
  (highest manual index 139 → 160) and observing the cap stay at exactly 174.

## 3. Structure (the real mechanism)

### Backing store — 300 slots
`FUN_14056BBB0`, the `AccessorWin32` constructor:

```
mov edx, 0x12C                ; 300
...resize the slot vector to 300 entries (24-byte elements)...
mov [rbp+0x1C28], 0x12C       ; count = 300
mov [r14+0x37DC], 0x12C       ; max   = 300
```

Confirmed at runtime: `count[+0x23A8] = 300`, `max[+0x37DC] = 300`.
`FUN_14056DCC0` loops `while (i < 300)` loading `icon0.png` for every slot —
observed live via an API hook (the game opens `save000`…`save299`).

### Display list — 175 entries
Built by **`FUN_140591020`**. The menu's cursor and item count live in the
accessor object:

| Offset | Meaning |
|---|---|
| `+0x4728` | display vector: data pointer |
| `+0x4730` | display vector: element count (**175**) |
| `+0x4744` | cursor / current slot |

Scroll clamping computes `max_index = count - 1 = 174`
(seen in `FUN_14056EC50`, cursor write at `+0x56ECE9`).

### The range descriptors — where 175 comes from
`FUN_140591020` calls **`FUN_1405952F0`** (a plain copy loop) to copy an array of
12-byte `{start, end, float}` range descriptors into a local buffer, then walks
them, pushing one display entry per index in each range:

```
0x591390   mov ebx, [r14]        ; ebx = range.start
0x591393   cmp ebx, [r14+4]      ; range.end
...
0x591480   inc ebx
0x591482   cmp ebx, [r14+4]
0x591486   jbe  <inner>          ; inclusive
0x59148C   add r14, 0xC          ; next range
0x591490   cmp r14, r12
0x591493   jne  <outer>
```

Read live from memory (3 ranges, count at `+0xDE0`, array at `+0xD78`):

| # | `{start, end}` | Meaning | Entries |
|---|---|---|---|
| 0 | `{170, 170}` | autosave | 1 |
| 1 | `{0, 169}` | manual saves | 170 |
| 2 | `{171, 174}` | chapter-clear / reserved | 4 |
| | | **total** | **175** |

The descriptors are **re-copied on every menu open** (that copy is
`FUN_1405952F0`, store at `+0x59532D`), which is why editing them in memory does
not persist.

## 4. The v1 patch (superseded — display only)

> ⚠️ **This hook turned out to be insufficient.** It makes the slots *render*,
> but save and load silently ignore them — see §9 for why and §10 for the v2
> patch that replaces it. Kept for the record.

Hook `FUN_140591020` immediately after the copy, at **`+0x591370`**, where the
builder reads the local range array:

```
+0x591370   8B 45 F8       mov eax,[rbp-08]      ; range count
+0x591373   48 8D 0C 40    lea rcx,[rax+rax*2]
+0x591377   4C 8B 75 F0    mov r14,[rbp-10]      ; range array
+0x59137B   4D 8D 24 8E    lea r12,[r14+rcx*4]   ; end = array + count*12
```

Replace the 7 bytes at `+0x591370` with a `jmp rel32` + 2 `nop`, and in the cave
**append** a range, leaving the game's own three untouched:

```asm
mov eax, [rbp-08]                  ; existing range count (3)
cmp eax, 7                         ; copy routine handles at most 8 ranges
jae  skip
lea rcx, [rbp-70]                  ; rcx = range array
lea rdx, [rax+rax*2]               ; count*3
lea rcx, [rcx+rdx*4]               ; &range[count]  (stride 12)
mov dword ptr [rcx],    175        ; new.start (first slot past reserved)
mov dword ptr [rcx+04], 299        ; new.end   = MaxSlots-1
mov dword ptr [rcx+08], 3F800001h  ; third field, matching the game's ranges
inc eax
mov [rbp-08], eax                  ; count = 4
skip:
mov eax, [rbp-08]                  ; (original)
lea rcx, [rax+rax*2]               ; (original)
jmp +0x591377
```

`[rbp-0x70]` is the range array (small-buffer optimisation: the container's
element pointer at `+0x60` points back at its own storage) and `[rbp-0x08]` is
its count.

### ⚠️ Do not flatten the ranges

The first working prototype replaced all three ranges with a single `{0, 299}`.
That **does** produce 300 visible slots — but **save and load then fail**: saving
writes nothing to disk and the slot list stops rendering afterwards.

The ranges are not merely a display list; their grouping carries the slot
*categorisation* (autosave / manual / reserved) that the save path depends on,
and the display order is `[170] [0..169] [171..174]`, not ascending. Collapsing
them loses that. **Append a fourth range instead** — the stock three keep their
exact contents, order, and meaning.

Per-slot, the builder initialises `r8d = -1` and looks the slot index up in a
red-black tree of enumerated saves (`[rsp+0x78]`), emitting a
`{value, slot_index}` pair — `value = -1` for an empty slot. So empty slots in an
appended range are represented correctly; the categorisation, not the emptiness,
was the problem.

## 5. Ceiling

**300 without further surgery** — the display list may only reference slots that
exist in the 300-element backing array. §12 documents how v3 breaks through
this to the true hard ceiling of **999** (imposed by the `save%03d` filename
format).

## 6. Reserved slots

Indices **170–174** are reserved: 170 = autosave, 171–174 = chapter-clear and
related (confirmed by reading `detail.json` — "Prologue End", "Chapter 1 End").
Flattening the ranges to `{0, N-1}` makes them display as ordinary slots.
(§9 later showed the reservation goes further: 175–179 and 180–199 are also
spoken for, so appended slots must start at 200.)

## 7. Method notes

What actually cracked this, after static analysis failed:

1. **API-level instrumentation** — IAT-hooking `CreateFileW` etc. and logging every
   `savedata` path revealed the 300-slot icon pass and the exact 0–174 enumeration.
2. **Cheat Engine** for the interactive work: find the cursor by scanning for the
   highlighted slot number, then *"find out what writes this address"* to reach the
   builder, then read the range array in Memory Viewer.
3. **CE Auto Assemble** to prototype the code patch live before writing any DLL.

Two mistakes worth avoiding: a self-written hardware-breakpoint probe re-armed the
debug registers every 2.5 s and silently clobbered Cheat Engine's breakpoints
(never run both); and enumerating threads to set DR registers must **skip the
calling thread**, or it suspends itself and deadlocks.

## 9. Why the v1 patch failed: the range list has a *source*, and everything validates against it

The list the display builder copies is itself a copy. The authoritative
range list lives in a **dialog param block** built by the save/load state
machine (`savedata_saveload_state.cpp`) and handed to the accessor, which
stores a pointer to it at **`accessor+0x37B8`**. Inside the param block:

| Offset | Meaning |
|---|---|
| `+0x70` | range array data pointer (points at an inline buffer, capacity 8) |
| `+0x78` | range count |

Element stride is 12: `{u32 start, u32 end, u8 enabled_flag + 3 pad}`. The
"float" third field observed live (`0x3F800001`) is an artifact: the builders
write `CONCAT31(stack_garbage, 1)` — only the low byte (a bool) is meaningful,
and the garbage happened to be the upper bytes of a stale `1.0f`.

**Consumers of the source list** (all walk `[+0x37B8]→{+0x70,+0x78}`):

- **`FUN_14056EDC0` — the confirm handler. This is the clamp.** It computes the
  selected slot, walks the ranges, and if the slot is in none of them it just
  `return`s — the confirm press is silently dropped, for save (mode 1), load
  (mode 2), and mode 3 alike. This is exactly the "save/load does nothing on
  slots ≥175" symptom.
- `FUN_14056E080` — per-slot enable/grey-out pass (60 pages × 5 slots), and the
  detail-icon attach, both skip out-of-range slots.
- `FUN_140570200` — the detail.json enumeration pass (called via `FUN_140575EE0`
  / `FUN_140576030` with the `+0x37B8` block). This is what soraprobe saw as
  the "exact 0–174 enumeration": it enumerates *the ranges*, nothing else.

The v1 hook appended the range to the display builder's stack-local copy —
downstream of all of the above. Hence: slots render, everything else refuses.

### Where the ranges are born

The dialog-open states in `savedata_saveload_state.cpp` hard-code them
(as imm32 `169/171/174` etc. — the earlier exhaustive search missed them by
only looking for `175`/`170`):

| Function | Dialog | Ranges built |
|---|---|---|
| `FUN_14036E430` | save | `{0,169}` |
| `FUN_14036EB70` | load | `{170,170} {0,169} {171,174}` |
| `FUN_140370870` | title-screen load | `{0,169} {171,179} {170,170}` |
| `FUN_14036E430` / `FUN_14036EB70`, alternate mode | system | `{180,199}` |
| `FUN_14036F680` | copy/delete | `{0,169} {171,174}` |

Each writes into its state object's inline buffer (`state[0x3e897]` ptr /
`state+0x1F44C0` count, capacity 8, always reset to 0 first), then calls an
accessor vtable setter that stores the pointer at `+0x37B8`.

### The real slot map of the 300-slot backing store

| Slots | Use |
|---|---|
| 0–169 | manual saves |
| 170 | autosave |
| 171–174 | chapter-clear |
| 175–179 | unused, but inside the title-load dialog's reserved block `{171,179}` |
| **180–199** | **reserved 20-slot window for the alternate "system" dialog mode** |
| 200–299 | genuinely free |

So new slots must be **`{200, 299}`** — appending `{175,299}` (as v1 did)
would have let ordinary saves land inside the reserved 180–199 window.

## 10. The v2 patch

Hook the four `AccessorWin32` vtable setters at the instruction that stores the
param pointer (each exactly 7 bytes, ideal for `jmp rel32` + 2 `nop`):

| Site | Bytes | Store |
|---|---|---|
| `+0x56C29A` | `4C 89 81 B8 37 00 00` | `mov [rcx+37B8h], r8` (save dialog) |
| `+0x56C59A` | `4C 89 81 B8 37 00 00` | `mov [rcx+37B8h], r8` (load dialog) |
| `+0x56C68A` | `4C 89 81 B8 37 00 00` | `mov [rcx+37B8h], r8` |
| `+0x56C9E3` | `48 89 91 B8 37 00 00` | `mov [rcx+37B8h], rdx` (common dialog) |

The cave redoes the store, then appends `{200, MaxSlots-1, 1}` to the param's
range list **unless** the list is empty, already has 8 entries, or any range
end is ≥ 180 (that condition identifies the reserved system dialog — leave it
alone). The builders re-run and reset the list on every dialog open, so the
append never accumulates.

The old `+0x591370` display hook must be **removed** at the same time: the
display builder copies from the source, so with the source patched it inherits
the new range — keeping both would show slots 200–299 twice.

## 12. The v3 patch: 999 slots + reserved slots at the bottom

### Reordering the display

The range list order *is* the display order, and v3 owns the list rebuild (the
four setter hooks of §10 now call a C helper through a register-preserving
thunk). The list becomes `[manual ranges] [{200, N-1}] [reserved ranges]`, so
new slots continue straight after the manual block and autosave/chapter-clear
move to the bottom. Partition rule: `start < 170` = manual, `start >= 170` =
reserved; the `{180,199}` system dialog is still detected by content
(`any end >= 180`) and skipped — which doubles as the idempotency guard.

### Breaking the 300 ceiling

`FUN_14056BBB0` (the accessor ctor) revealed why a naive `resize` patch can't
work: **the slot array is inline in the object.** The ctor sets the data
pointer `this+0x23A0` to `this+0x780`, and 300 × 24-byte entries end at
exactly `+0x23A0` — the array is wedged right up against its own bookkeeping
fields. Growing it in place would overwrite the entire rest of the object.

But an exhaustive scan showed **every consumer loads the data pointer** —
`+0x56CDA0`, `+0x56D0E0`, icon pass `+0x56DCC0`, release `+0x56DF70`,
enable pass `+0x56E080`, `+0x5724C0`, `+0x5726C0`, tile creation in
`+0x572E20`, `+0x573D50` — and the only code taking the inline buffer's
address directly is the ctor itself. The `max` field `+0x37DC` is write-only
(never read anywhere). So the redirect is safe:

- **Hook the ctor tail** at `+0x56BF0B` (`C7 86 DC 37 00 00 2C 01 00 00`,
  `mov [rsi+37DCh], 12Ch`, 10 bytes): point `+0x23A0` at a fresh zeroed
  VirtualAlloc'd buffer, set count `+0x23A8` and max `+0x37DC` to N.
- **Icon pass bound** `+0x56DF34`: `81 FE 2C 01 00 00` (`cmp esi, 300`) —
  imm32 patched in place to N.
- **Page loops** (the UI is 60 pages × 5 tiles = 300):
  - `+0x56E50B` `41 83 FD 3C / 0F 8C 5D FD FF FF` (`cmp r13d,60 / jl`) —
    enable/grey-out pass in `FUN_14056E080`;
  - `+0x57354E` `41 83 FC 3C / 0F 82 D8 FD FF FF` (`cmp r12d,60 / jb`) —
    tile creation in the dispatcher `FUN_140572E20`.
  Both re-encoded in a cave as `cmp reg, ceil(N/5) / jcc loop / jmp exit`.

Two subtleties:

1. **The tile loop indexes slots up to `pages*5 - 1`**, reading
   `slot_entry[0]` (the icon texture) for each — for N=999 that's index 999,
   one past a 999-entry buffer. The buffer is therefore sized `ceil(N/5)*5`
   entries; the spare entries stay zero and the tile code's null check takes
   its "no icon" fallback. The release loop iterates `count` (= N) and never
   touches them.
2. **Ordering/race safety.** The icon/page bounds are only patched from
   inside the ctor hook, after the bigger buffer actually exists
   (`patchFixedBounds`, once). If the accessor was constructed before the
   plugin loaded, the range append also caps itself at 299
   (`g_backingSlots` gate) — the plugin degrades to v2 behaviour instead of
   letting the game index past the inline array.

Remaining scaling facts, verified: the tile sprites are added as *children* of
the layout container (`call +0x4655E0`) — a dynamic list, it just grows; the
display vector, enumeration tree and scroll clamp are all count-driven; the
dialog result returns the slot as a `saveNNN` folder-name string (`atoi` after
skipping `"save"`), so 3-digit indices up to 998 parse fine.

Costs at 999: the icon pass opens 999 `icon0.png`s at startup, every dialog
open stats/reads up to ~900 extra `detail.json`s, and the dialog builds 1000
sprite objects (~0x470 bytes each). Noticeably slower menu open; nothing
structural.

## 13. File format notes (incidental)

- Saves: `%USERPROFILE%\Saved Games\FALCOM\Trails in the Sky 1st Chapter\savedata\saveNNN\`
  containing `detail.json` (plaintext metadata), `icon0.png`, `user.dat` (zstd, magic `28 B5 2F FD`).
- Archives: `.pac` = **FPAC** — 16-byte header (`FPAC`, u32 count, u32 first-file
  offset, u32 unknown), then 32-byte entries:
  `u32 name_crc32, u32 pad, u64 name_offset, u64 size, u64 data_offset`.
  Extractor in `tools/analysis/fpac.py`.
- `.lay` UI layouts are a binary-JSON format (magic `JSON`, CRC-hashed keys).
