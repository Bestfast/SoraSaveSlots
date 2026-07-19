# Ready-to-paste prompt for debugging v3

Fill in the SYMPTOMS line, paste the rest as-is into a new Claude Code session
started in `~/Documents/Claudio`.

---

In my sora-saveslots project, v3 of the plugin is misbehaving.

SYMPTOMS: crash at startup, nothing from Event Viewer (Windows Logs → Application)

My `SoraSaveSlots.ini` has `MaxSlots=999`. Attached/next to the .asi is
`SoraSaveSlots.log` which says:SoraSaveSlots v3
MaxSlots = 999, sora_1st.exe base = 0x7ff7c3590000
OK: ranges hook +0x56C29A -> cave 0x7ff7c3580000
OK: ranges hook +0x56C59A -> cave 0x7ff7c3580081
OK: ranges hook +0x56C68A -> cave 0x7ff7c3580102
OK: ranges hook +0x56C9E3 -> cave 0x7ff7c3580183
OK: backing hook +0x56BF0B -> cave 0x7ff7c3580204

it does not crash with maxslots=300.

Context (verify against `docs/FINDINGS.md` §9–12 and `src/dllmain.cpp`, which
document everything): v3 does two independent things —

1. RANGES (any MaxSlots): four 7-byte hooks on the AccessorWin32 setters at
   `+0x56C29A`, `+0x56C59A`, `+0x56C68A` (param in r8) and `+0x56C9E3` (param
   in rdx) call C `FixRanges` through a register/xmm-preserving thunk. It
   rebuilds the dialog param block's range list (data ptr at param+0x70,
   count at +0x78, cap 8) as [manual <170] [{200, N-1}] [reserved ≥170],
   skipping any list whose range end is ≥180 (the reserved system dialog —
   also the idempotency guard).
2. BACKING (MaxSlots > 300 only): a 10-byte ctor-tail hook at `+0x56BF0B`
   calls `FixBacking`, which redirects the accessor's inline slot array
   pointer at `+0x23A0` to a zeroed VirtualAlloc of ceil(N/5)*5 × 24-byte
   entries and sets count `+0x23A8` / max `+0x37DC` = N; it then (once,
   lazily) patches the icon-loop imm32 at `+0x56DF34+2` and re-encodes the
   two 60-page loop bounds at `+0x56E50B` (jl → `+0x56E272`, exit
   `+0x56E515`) and `+0x57354E` (jb → `+0x573330`, exit `+0x573558`) to
   ceil(N/5) pages in a cave.

Expected healthy log: 4× "ranges hook", 1× "backing hook", then once the game
builds its save system: "FixBacking: accessor …", "icon-pass bound", 2× "page
bound". Missing lines narrow the failure.

Debug in this order:

1. If a crash: map the faulting offset. Near `+0x56E2xx-0x56E5xx` = enable
   pass (suspect stale 60-page assumption or tile-child array); near
   `+0x5733xx` = tile creation (suspect slot-entry read past the redirected
   buffer); inside a cave (offset outside the module) = my emitted code —
   dump the cave bytes and disassemble with capstone.
2. Ranked suspects for the >300 mode, in order of my residual doubt:
   a. a consumer of the slot array or a page-count assumption I missed —
      rerun `tools/analysis/disp.py 0x23a0 / 0x23a8` and look for new
      readers, and search the enable/tile functions for other literals
      derived from 60/300;
   b. the UI scrollbar/paging visuals for >60 pages (cosmetic math that
      clamps or overflows);
   c. detail.json enumeration (`+0x570200`) misbehaving with ~900 extra
      slots per refresh (watch for freezes rather than crashes — try an
      intermediate MaxSlots like 400 to scale the load);
   d. the accessor being constructed before the plugin loads (log would
      show hooks but no "FixBacking" line — that path is designed to
      degrade to 300, verify it did).
3. Tooling: capstone venv at `tools/analysis/.venv` (run scripts from that
   dir); Ghidra headless per `tools/README.md` (project is regenerable,
   ~10 min import; JAVA_HOME = scoop temurin21-jdk). `soraprobe.cpp` in
   `tools/probes` IAT-logs every savedata file access with caller RVAs if
   you need runtime ground truth. Never run the probes and Cheat Engine
   together. Python3.14.exe exists.

Saves are at `%USERPROFILE%\Saved Games\FALCOM\Trails in the Sky 1st Chapter\`
— I have a backup from before testing. The game is on `Z:\SteamLibrary`.
