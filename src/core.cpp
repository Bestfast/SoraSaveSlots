// SoraSaveSlots - shared engine implementation. The per-game modules only
// describe WHERE things are (GameContext); everything that patches memory,
// rebuilds range lists or expands the slot array lives here.
#include "core.h"
#include <string.h>

// ---------------------------------------------------------------- logging
static FILE* g_log = nullptr;

void coreOpenLog(const wchar_t* path) { g_log = _wfsopen(path, L"w", _SH_DENYNO); }
void coreCloseLog() { if (g_log) { fclose(g_log); g_log = nullptr; } }

void L(const char* f, ...) {
    if (!g_log) return;
    va_list ap; va_start(ap, f); vfprintf(g_log, f, ap); va_end(ap);
    fflush(g_log);
}

// ---------------------------------------------------------------- runtime state
static GameContext*   g_ctx;                     // active context (set by installGame)
static volatile LONG  g_backingSlots  = 0;       // !=0 once a backing buffer exists
static volatile LONG  g_boundsPatched = 0;       // icon/page bounds raised once
static void*          g_installed[MAX_INSTALLED]; // every buffer we handed out
static unsigned char* g_pageCave;                // lazy page-loop caves

// autosave-history state (see AutosaveHistoryTick)
static volatile LONG     g_autoCopyBusy  = 0;    // re-entry guard
static unsigned long long g_autoLastMtime = 0;   // last live-save mtime we mirrored
static unsigned          g_autoLastSize  = 0;    // ...and its size

// ---------------------------------------------------------------- basics
static void* allocNear(void* anchor, size_t size) {
    SYSTEM_INFO si; GetSystemInfo(&si);
    uintptr_t base = (uintptr_t)anchor;
    uintptr_t gran = si.dwAllocationGranularity;
    for (uintptr_t delta = gran; delta < 0x7F000000ull; delta += gran) {
        for (int dir = 0; dir < 2; ++dir) {
            uintptr_t addr = dir ? base + delta : base - delta;
            addr &= ~(uintptr_t)(gran - 1);
            void* p = VirtualAlloc((void*)addr, size, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
            if (p) return p;
        }
    }
    return nullptr;
}

bool patchBytes(unsigned char* site, const unsigned char* bytes, int len) {
    DWORD old;
    if (!VirtualProtect(site, len, PAGE_EXECUTE_READWRITE, &old)) return false;
    memcpy(site, bytes, len);
    VirtualProtect(site, len, old, &old);
    FlushInstructionCache(GetCurrentProcess(), site, len);
    return true;
}

bool hookSite(unsigned char* site, int siteLen, unsigned char* caveEntry) {
    unsigned char jmp[16];
    jmp[0] = 0xE9;
    *(int*)(jmp + 1) = (int)((uintptr_t)caveEntry - ((uintptr_t)site + 5));
    for (int i = 5; i < siteLen; ++i) jmp[i] = 0x90;
    return patchBytes(site, jmp, siteLen);
}

// ---------------------------------------------------------------- scanning
int scanText(const GameContext& ctx, const AobPat& p, unsigned* out, int max) {
    IMAGE_DOS_HEADER* dos = (IMAGE_DOS_HEADER*)ctx.base;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) { L("scan: bad DOS sig\n"); return -1; }
    IMAGE_NT_HEADERS* nt = (IMAGE_NT_HEADERS*)(ctx.base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) { L("scan: bad NT sig\n"); return -1; }
    IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(nt);
    DWORD textRva = 0, textSize = 0;
    for (int i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
        if (memcmp(sec[i].Name, ".text", 5) == 0) { textRva = sec[i].VirtualAddress; textSize = sec[i].Misc.VirtualSize; break; }
    }
    if (!textSize) { L("scan: .text not found\n"); return -1; }

    const unsigned char* base = (const unsigned char*)(ctx.base + textRva);
    int found = 0;
    for (DWORD off = 0; off + (DWORD)p.len <= textSize && found < max; ++off) {
        bool ok = true;
        for (int j = 0; j < p.len; ++j)
            if (p.mask[j] && base[off + j] != p.bytes[j]) { ok = false; break; }
        if (ok) out[found++] = textRva + off;
    }
    return found;
}

// Start RVA of the function containing `rva`, via the .pdata exception table
// (RUNTIME_FUNCTION = {BeginAddress, EndAddress, UnwindData}, sorted).
unsigned findFunctionStart(uintptr_t base, unsigned rva) {
    IMAGE_DOS_HEADER* dos = (IMAGE_DOS_HEADER*)base;
    IMAGE_NT_HEADERS* nt = (IMAGE_NT_HEADERS*)(base + dos->e_lfanew);
    IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(nt);
    for (int i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
        if (memcmp(sec[i].Name, ".pdata", 6) != 0) continue;
        const DWORD* e = (const DWORD*)(base + sec[i].VirtualAddress);
        DWORD count = sec[i].Misc.VirtualSize / 12;
        for (DWORD k = 0; k < count; ++k)
            if (e[k * 3] <= rva && rva < e[k * 3 + 1]) return e[k * 3];
        break;
    }
    return 0;
}

// ---------------------------------------------------------------- patch logic

// %USERPROFILE%\Saved Games\FALCOM\<game>\savedata, or false if unavailable.
static bool SavedataRoot(wchar_t* out, size_t n) {
    if (!g_ctx || !g_ctx->savedGamesDir) return false;
    wchar_t up[MAX_PATH];
    DWORD len = GetEnvironmentVariableW(L"USERPROFILE", up, MAX_PATH);
    if (!len || len >= MAX_PATH) return false;
    return _snwprintf_s(out, n, _TRUNCATE,
                        L"%s\\Saved Games\\FALCOM\\%s\\savedata",
                        up, g_ctx->savedGamesDir) != -1;
}

// Highest saveNNN directory in the game's Saved Games folder, or 0xFFFFFFFF
// if the scan is unavailable (unknown path / nothing found / error). The
// autosave-history ring is excluded so it does not inflate the window.
static unsigned ScanMaxSaveSlot() {
    wchar_t root[MAX_PATH];
    if (!SavedataRoot(root, MAX_PATH)) return 0xFFFFFFFFu;

    wchar_t pat[MAX_PATH];
    if (_snwprintf_s(pat, MAX_PATH, _TRUNCATE, L"%s\\save*", root) == -1)
        return 0xFFFFFFFFu;

    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileExW(pat, FindExInfoBasic, &fd,
                                FindExSearchLimitToDirectories, nullptr,
                                FIND_FIRST_EX_LARGE_FETCH);
    if (h == INVALID_HANDLE_VALUE) return 0xFFFFFFFFu;
    unsigned max = 0; bool any = false;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
        const wchar_t* s = fd.cFileName + 4;          // past "save"
        unsigned v = 0; bool dig = false;
        while (*s >= L'0' && *s <= L'9') { v = v * 10 + (unsigned)(*s - L'0'); ++s; ++dig; }
        if (!dig || *s != L'\0' || v >= FILENAME_LIMIT) continue;
        if (g_ctx->autoRva && g_ctx->autoHistCount > 0 &&
            v >= g_ctx->autoHistStart &&
            v <  g_ctx->autoHistStart + (unsigned)g_ctx->autoHistCount)
            continue;                                  // history ring: ignore
        if (!any || v > max) { max = v; any = true; }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return any ? max : 0xFFFFFFFFu;
}

// Called from the autosave cave just BEFORE the game overwrites the live
// autosave slot. Mirrors the *current* (previous) autosave folder into the
// oldest/empty ring slot, so the live slot keeps behaving exactly as stock
// ("load latest" etc.) while the ring keeps a rolling history.
static void AutosaveHistoryTick(void) {
    const GameContext& c = *g_ctx;
    if (!c.autoRva || c.autoHistCount <= 0) return;
    if (InterlockedExchange(&g_autoCopyBusy, 1)) return;

    wchar_t root[MAX_PATH];
    if (!SavedataRoot(root, MAX_PATH)) { InterlockedExchange(&g_autoCopyBusy, 0); return; }

    wchar_t live[MAX_PATH], liveDir[MAX_PATH];
    _snwprintf_s(liveDir, MAX_PATH, _TRUNCATE, L"%s\\save%03u", root, c.autoSlot);
    _snwprintf_s(live,    MAX_PATH, _TRUNCATE, L"%s\\savedata", liveDir);

    WIN32_FILE_ATTRIBUTE_DATA fad;
    if (!GetFileAttributesExW(live, GetFileExInfoStandard, &fad) || !fad.nFileSizeLow) {
        InterlockedExchange(&g_autoCopyBusy, 0);
        return;                                      // no live autosave yet
    }
    unsigned long long mt = ((unsigned long long)fad.ftLastWriteTime.dwHighDateTime << 32)
                          |  (unsigned long long)fad.ftLastWriteTime.dwLowDateTime;
    L("autosave-hist: hook fired (live save%03u mtime=%llu size=%u)\n",
      c.autoSlot, mt, fad.nFileSizeLow);

    if (mt == g_autoLastMtime && fad.nFileSizeLow == g_autoLastSize) {
        InterlockedExchange(&g_autoCopyBusy, 0);
        return;                                      // same autosave, twin hook
    }

    // Pick the ring target: first empty slot, else the oldest.
    int best = -1; unsigned long long bestMt = 0;
    for (int i = 0; i < c.autoHistCount; ++i) {
        unsigned slot = c.autoHistStart + (unsigned)i;
        wchar_t f[MAX_PATH];
        _snwprintf_s(f, MAX_PATH, _TRUNCATE, L"%s\\save%03u\\savedata", root, slot);
        WIN32_FILE_ATTRIBUTE_DATA a;
        if (!GetFileAttributesExW(f, GetFileExInfoStandard, &a)) { best = (int)slot; break; }
        unsigned long long m = ((unsigned long long)a.ftLastWriteTime.dwHighDateTime << 32)
                             |  (unsigned long long)a.ftLastWriteTime.dwLowDateTime;
        if (best < 0 || m < bestMt) { bestMt = m; best = (int)slot; }
    }
    if (best < 0) { InterlockedExchange(&g_autoCopyBusy, 0); return; }

    wchar_t tgtDir[MAX_PATH], pat[MAX_PATH];
    _snwprintf_s(tgtDir, MAX_PATH, _TRUNCATE, L"%s\\save%03d", root, best);
    CreateDirectoryW(tgtDir, nullptr);
    _snwprintf_s(pat, MAX_PATH, _TRUNCATE, L"%s\\*", liveDir);

    int copied = 0;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pat, &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            wchar_t src[MAX_PATH], dst[MAX_PATH];
            _snwprintf_s(src, MAX_PATH, _TRUNCATE, L"%s\\%s", liveDir, fd.cFileName);
            _snwprintf_s(dst, MAX_PATH, _TRUNCATE, L"%s\\%s", tgtDir,  fd.cFileName);
            if (CopyFileW(src, dst, FALSE)) ++copied;
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    if (copied) {
        g_autoLastMtime = mt;
        g_autoLastSize  = fad.nFileSizeLow;
        L("autosave-hist: mirrored save%03u -> save%03d (%d file%s)\n",
          c.autoSlot, best, copied, copied == 1 ? "" : "s");
    } else {
        L("autosave-hist: mirror to save%03d failed (%lu)\n", best, GetLastError());
    }
    InterlockedExchange(&g_autoCopyBusy, 0);
}

// Called from the setter caves with the dialog param block (arg2 = accessor).
// Rebuilt layouts:
//   load-style: [manual <170] [{200..headEnd}] [{histEnd+1..hi}] [{ring}]
//               [{170,170}] [{171,179}] [{180,199} filler], ordered-mode flag=1
//               so the autosave/backup tiles sit at the BOTTOM of the menu.
//   save-style: [manual <170] [{200..headEnd}] [{histEnd+1..hi}] with the ring
//               and 170-199 left UNCOVERED.
// headEnd is raised past 300 (split around the ring) once the backing redirect
// has lifted the ceiling, so MaxSlots>300 actually adds slots.
// Constraints driving this layout (2026-08 game update):
//  1. The sequential tile renderer locks tile position == slot_number; gaps
//     are only allowed where nothing should ever render or be selectable.
//  2. Slots 180-199 are the reserved system window (the game treats an
//     occupied slot there as clear data and switches the title menu to
//     New Game+), so the new block starts at 200. Tiles are labelled
//     position+1, so folder saveNNN sits on the tile labelled NNN+1.
//  3. The tile-content pass derives its count from param[+0xC], not from the
//     range list, so param[+0xC] must be raised alongside the ranges.
//  4. A list that arrives WITHOUT reserved entries is the save dialog (stock:
//     {0,169} only). Keeping 170-199 uncovered means the enumerator/confirm/
//     enable passes never touch the autosave or backups there - stock
//     behaviour restored for the save menu.
static void __fastcall FixRanges(unsigned char* param, unsigned char* accessor) {
    if (!param) return;
    Range* r = *(Range**)(param + 0x70);
    unsigned n = *(unsigned*)(param + 0x78);
    if (!r || n == 0 || n > 7) return;

    const unsigned APP_START = NEW_START;          // 200: first free slot

    int backing = g_backingSlots ? g_backingSlots : STOCK_BACKING;
    unsigned hi = (unsigned)((g_ctx->slots < backing ? g_ctx->slots : backing) - 1);

    // Dynamic window (ini DynamicWindow, default on): show at most
    // last-save-on-disk + headroom, floored so the new block stays reachable.
    if (g_ctx->dynamicWindow) {
        unsigned m = ScanMaxSaveSlot();
        if (m != 0xFFFFFFFFu) {
            unsigned want = m + DYNAMIC_HEADROOM;
            if (want < DYNAMIC_FLOOR) want = DYNAMIC_FLOOR;
            if (want < hi) {
                hi = want;
                L("FixRanges: dynamic window -> %u (last save on disk %u)\n", hi, m);
            }
        }
    }
    if (hi < APP_START) return;

    // Recognise our own rebuilt list: it is the only normal dialog whose ranges
    // reach APP_START (n>=2 so the game's single-range {200,203} system list is
    // not mistaken for ours). Ours is regenerated from scratch every call, which
    // is also what lets a later call pick up a raised ceiling once the >300
    // backing redirect has kicked in (setters run before the icon pass).
    bool ours = false;
    for (unsigned i = 0; i < n; ++i)
        if (r[i].start >= APP_START) { ours = (n >= 2); break; }

    if (!ours) {
        // Reserved system dialog ({180,199}) or unknown high-slot list: untouched.
        for (unsigned i = 0; i < n; ++i)
            if (r[i].end >= 180) return;
    }

    // Autosave-history ring sits at the top of the stock 300 window. When the
    // backing redirect raises the ceiling above it, the manual/new block is
    // split around the ring so slots beyond 300 stay usable.
    const unsigned histStart = g_ctx->autoHistStart;
    const bool hist = g_ctx->autoRva && g_ctx->autoHistCount > 0 && histStart > APP_START;
    const unsigned histEnd = hist ? histStart + (unsigned)g_ctx->autoHistCount - 1 : 0;
    const bool split = hist && hi >= histStart;
    const unsigned headEnd = split ? histStart - 1 : hi;

    const Range* src170 = nullptr;
    const Range* src171 = nullptr;
    bool saveStyle = true;                         // no reserved entries?
    Range manual[8]; unsigned nm = 0;
    for (unsigned i = 0; i < n; ++i) {
        if (r[i].start >= APP_START) continue;     // ours: drop, regenerate below
        if      (r[i].start == 170) { src170 = &r[i]; saveStyle = false; }
        else if (r[i].start == 171) { src171 = &r[i]; saveStyle = false; }
        else if (r[i].start <  170) { if (nm < 8) manual[nm++] = r[i]; }
    }

    Range tmp[8]; unsigned m = 0;
    for (unsigned i = 0; i < nm && m < 8; ++i)     // stock manual lead (e.g. {0,159})
        tmp[m++] = manual[i];
    if (m < 8) { tmp[m].start = APP_START; tmp[m].end = headEnd; tmp[m].flag = 1; ++m; }
    if (split && hi > histEnd && m < 8) {          // manual tail above the ring
        tmp[m].start = histEnd + 1; tmp[m].end = hi; tmp[m].flag = 1; ++m;
    }
    if (!saveStyle) {
        // Ordered load menu: ring next, then the reserved tiles verbatim.
        if (hist && m < 8) {                       // autosave-history ring block
            tmp[m].start = histStart; tmp[m].end = histEnd; tmp[m].flag = 1; ++m;
        }
        if (src170) { if (m < 8) tmp[m++] = *src170; }   // autosave, verbatim
        else { tmp[m].start = 170; tmp[m].end = 170; tmp[m].flag = 1; ++m; }
        if (src171) { if (m < 8) tmp[m++] = *src171; }   // chapter-clears, verbatim
        else { tmp[m].start = 171; tmp[m].end = 174; tmp[m].flag = 1; ++m; }
        // Inactive filler closes the list; flag=0 marks those tiles dead.
        unsigned fs = 175;
        if (src171 && src171->end >= fs) fs = src171->end + 1;
        if (fs <= APP_START - 1 && m < 8) {
            tmp[m].start = fs; tmp[m].end = APP_START - 1; tmp[m].flag = 0; ++m;
        }
        if (g_ctx->offTileContainer >= 0 && accessor)
            *(unsigned*)(accessor + (unsigned)g_ctx->offTileContainer + 0xDE0) = 1;
    } else {
        // saveStyle: the ring and 170-199 stay uncovered. The enumerator/
        // confirm/enable passes all test ranges, so an uncovered slot is never
        // enumerated, selectable or savable - exactly like stock.
        if (g_ctx->offTileContainer >= 0 && accessor)
            *(unsigned*)(accessor + (unsigned)g_ctx->offTileContainer + 0xDE0) = 0;
    }

    unsigned tileBound = hi + 1;
    if (hist && histEnd + 1 > tileBound) tileBound = histEnd + 1;

    memcpy(r, tmp, m * sizeof(Range));
    *(unsigned*)(param + 0x78) = m;
    *(unsigned*)(param + 0xC) = tileBound;   // tile-count bound (see note 3 above)
    L("FixRanges: hi=%u head=%u split=%d tileBound=%u style=%s ours=%d n=%u\n",
      hi, headEnd, split ? 1 : 0, tileBound, saveStyle ? "save" : "load", ours ? 1 : 0, m);
    for (unsigned i = 0; i < m; ++i)
        L("  range[%u] = {%u,%u,f%u}\n", i, tmp[i].start, tmp[i].end, tmp[i].flag);
}

static void patchFixedBounds();

// Called from the backing cave with the accessor object (only when MaxSlots > 300).
// Point the accessor's slot array at a larger zeroed buffer and raise the
// hardcoded bounds. Idempotent: an accessor already running on one of our
// buffers is left alone.
static void __fastcall FixBacking(unsigned char* accessor) {
    const GameContext& c = *g_ctx;
    int slots = c.slots;
    unsigned entries = ((unsigned)(slots + 4) / 5) * 5;   // tile loop indexes pages*5

    void* cur = *(void**)(accessor + c.offSlotData);
    for (int i = 0; i < MAX_INSTALLED; ++i) {
        if (g_installed[i] && g_installed[i] == cur) {
            InterlockedExchange(&g_backingSlots, slots);
            patchFixedBounds();
            return;
        }
    }

    void* buf = VirtualAlloc(nullptr, (size_t)entries * 0x18,
                             MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!buf) {
        L("FixBacking: allocation failed; accessor stays at %d slots.\n", STOCK_BACKING);
        return;
    }
    for (int i = 0; i < MAX_INSTALLED; ++i)
        if (!g_installed[i]) { g_installed[i] = buf; break; }

    *(void**)   (accessor + c.offSlotData) = buf;
    *(unsigned*)(accessor + c.offSlotCnt)  = (unsigned)slots;
    if (c.offSlotMax >= 0)
        *(unsigned*)(accessor + (unsigned)c.offSlotMax) = (unsigned)slots;
    InterlockedExchange(&g_backingSlots, slots);
    patchFixedBounds();   // safe only now that the backing store is big enough
    // The current dialog's ranges were capped before the backing existed
    // (setters fire before the icon pass). Extend them right away.
    unsigned char* param = *(unsigned char**)(accessor + c.offParam);
    if (param) FixRanges(param, accessor);
    L("FixBacking: accessor %p -> %u-entry buffer %p, count/max = %d\n",
      accessor, entries, buf, slots);
}

// ---------------------------------------------------------------- cave emission

// Emit: <orig bytes>, save volatile regs + xmm0-5, call `target` with one
// argument, restore, jmp back to site+origLen.
// argMode selects where the argument comes from as of the hook site:
//   0 = r8   1 = rdx   2 = r14   3 = rdx (alias)   4 = rcx
unsigned char* emitCallThunk(unsigned char* p, unsigned char* site,
                             const unsigned char* orig, int origLen,
                             int argMode, void* target) {
    memcpy(p, orig, origLen); p += origLen;
    *p++ = 0x50;                                  // push rax
    *p++ = 0x51;                                  // push rcx
    *p++ = 0x52;                                  // push rdx
    *p++ = 0x41; *p++ = 0x50;                     // push r8
    *p++ = 0x41; *p++ = 0x51;                     // push r9
    *p++ = 0x41; *p++ = 0x52;                     // push r10
    *p++ = 0x41; *p++ = 0x53;                     // push r11
    *p++ = 0x53;                                  // push rbx
    *p++ = 0x48; *p++ = 0x8B; *p++ = 0xDC;        // mov rbx, rsp
    *p++ = 0x48; *p++ = 0x81; *p++ = 0xEC;        // sub rsp, 0A0h
    *(unsigned*)p = 0xA0; p += 4;
    *p++ = 0x48; *p++ = 0x83; *p++ = 0xE4; *p++ = 0xF0;  // and rsp, -16
    static const unsigned char xr[6] = { 0x44,0x4C,0x54,0x5C,0x64,0x6C };
    for (int i = 0; i < 6; ++i) {                 // movups [rsp+20h+i*10h], xmmN
        *p++ = 0x0F; *p++ = 0x11; *p++ = xr[i]; *p++ = 0x24;
        *p++ = (unsigned char)(0x20 + i * 0x10);
    }
    // saved vol regs on stack: 0=rbx,8=r11,10=r10,18=r9,20=r8,28=rdx,30=rcx,38=rax
    if      (argMode == 0) { *p++ = 0x48; *p++ = 0x8B; *p++ = 0x4B; *p++ = 0x20; } // rcx=[rbx+20h]
    else if (argMode == 1) { *p++ = 0x48; *p++ = 0x8B; *p++ = 0x4B; *p++ = 0x28; } // rcx=[rbx+28h]
    else if (argMode == 3) { *p++ = 0x48; *p++ = 0x8B; *p++ = 0x4B; *p++ = 0x28; } // rcx=[rbx+28h]
    else if (argMode == 4) { *p++ = 0x48; *p++ = 0x8B; *p++ = 0x4B; *p++ = 0x30; } // rcx=[rbx+30h]
    else                   { *p++ = 0x49; *p++ = 0x8B; *p++ = 0xCE; }              // mov rcx, r14
    // arg2 (rdx) = the accessor: for every store site hooked here the
    // accessor lives in rcx on entry (saved at [rbx+30h]).
    *p++ = 0x48; *p++ = 0x8B; *p++ = 0x53; *p++ = 0x30;   // mov rdx, [rbx+30h]
    *p++ = 0x48; *p++ = 0xB8;                     // mov rax, imm64
    *(unsigned long long*)p = (unsigned long long)(uintptr_t)target; p += 8;
    *p++ = 0xFF; *p++ = 0xD0;                     // call rax
    for (int i = 0; i < 6; ++i) {                 // movups xmmN, [rsp+20h+i*10h]
        *p++ = 0x0F; *p++ = 0x10; *p++ = xr[i]; *p++ = 0x24;
        *p++ = (unsigned char)(0x20 + i * 0x10);
    }
    *p++ = 0x48; *p++ = 0x8B; *p++ = 0xE3;        // mov rsp, rbx
    *p++ = 0x5B;                                  // pop rbx
    *p++ = 0x41; *p++ = 0x5B;                     // pop r11
    *p++ = 0x41; *p++ = 0x5A;                     // pop r10
    *p++ = 0x41; *p++ = 0x59;                     // pop r9
    *p++ = 0x41; *p++ = 0x58;                     // pop r8
    *p++ = 0x5A;                                  // pop rdx
    *p++ = 0x59;                                  // pop rcx
    *p++ = 0x58;                                  // pop rax
    uintptr_t ret = (uintptr_t)(site + origLen);
    *p++ = 0xE9;                                  // jmp back
    *(int*)p = (int)(ret - ((uintptr_t)p + 4)); p += 4;
    return p;
}

// Thunk variant for setters whose param block arrives in RSI (non-volatile):
// pushes/pops RSI around the call.
unsigned char* emitCallThunkRsi(unsigned char* p, unsigned char* site,
                                const unsigned char* orig, int origLen,
                                void* target) {
    memcpy(p, orig, origLen); p += origLen;
    *p++ = 0x56;                                  // push rsi
    *p++ = 0x50;                                  // push rax
    *p++ = 0x51;                                  // push rcx
    *p++ = 0x52;                                  // push rdx
    *p++ = 0x41; *p++ = 0x50;                     // push r8
    *p++ = 0x41; *p++ = 0x51;                     // push r9
    *p++ = 0x41; *p++ = 0x52;                     // push r10
    *p++ = 0x41; *p++ = 0x53;                     // push r11
    *p++ = 0x53;                                  // push rbx
    *p++ = 0x48; *p++ = 0x8B; *p++ = 0xDC;        // mov rbx, rsp
    *p++ = 0x48; *p++ = 0x81; *p++ = 0xEC;        // sub rsp, 0A0h
    *(unsigned*)p = 0xA0; p += 4;
    *p++ = 0x48; *p++ = 0x83; *p++ = 0xE4; *p++ = 0xF0;  // and rsp, -16
    static const unsigned char xr[6] = { 0x44,0x4C,0x54,0x5C,0x64,0x6C };
    for (int i = 0; i < 6; ++i) {
        *p++ = 0x0F; *p++ = 0x11; *p++ = xr[i]; *p++ = 0x24;
        *p++ = (unsigned char)(0x20 + i * 0x10);
    }
    // saved: 0=rsi,8=rax,10=rcx,18=rdx,20=r8,28=r9,30=r10,38=r11,40=rbx
    *p++ = 0x48; *p++ = 0x8B; *p++ = 0x4B; *p++ = 0x00;  // rcx = [rbx+0] (rsi)
    *p++ = 0x48; *p++ = 0xB8;                     // mov rax, imm64
    *(unsigned long long*)p = (unsigned long long)(uintptr_t)target; p += 8;
    *p++ = 0xFF; *p++ = 0xD0;                     // call rax
    for (int i = 0; i < 6; ++i) {
        *p++ = 0x0F; *p++ = 0x10; *p++ = xr[i]; *p++ = 0x24;
        *p++ = (unsigned char)(0x20 + i * 0x10);
    }
    *p++ = 0x48; *p++ = 0x8B; *p++ = 0xE3;        // mov rsp, rbx
    *p++ = 0x5B;                                  // pop rbx
    *p++ = 0x41; *p++ = 0x5B;                     // pop r11
    *p++ = 0x41; *p++ = 0x5A;                     // pop r10
    *p++ = 0x41; *p++ = 0x59;                     // pop r9
    *p++ = 0x41; *p++ = 0x58;                     // pop r8
    *p++ = 0x5A;                                  // pop rdx
    *p++ = 0x59;                                  // pop rcx
    *p++ = 0x58;                                  // pop rax
    *p++ = 0x5E;                                  // pop rsi
    uintptr_t ret = (uintptr_t)(site + origLen);
    *p++ = 0xE9;                                  // jmp back
    *(int*)p = (int)(ret - ((uintptr_t)p + 4)); p += 4;
    return p;
}

// ---------------------------------------------------------------- install

// Icon-loop imm32 + the page loops. Called from FixBacking (game thread),
// exactly once, only after a backing buffer exists.
static void patchFixedBounds() {
    if (InterlockedExchange(&g_boundsPatched, 1)) return;
    const GameContext& c = *g_ctx;
    int slots = c.slots;
    unsigned pages = ((unsigned)slots + 4) / 5;

    unsigned char iconImm[4];
    *(unsigned*)iconImm = (unsigned)slots;
    patchBytes((unsigned char*)(c.base + c.iconRva + c.iconImmOff), iconImm, 4);
    L("OK: icon-pass bound +0x%X -> %d\n", c.iconRva, slots);

    unsigned char* p = g_pageCave;
    for (int i = 0; i < c.nPages; ++i) {
        const PageSite& s = c.pages[i];
        unsigned char* site = (unsigned char*)(c.base + s.rva);
        unsigned char* entry = p;
        *p++ = 0x41; *p++ = 0x81; *p++ = s.cmpModRM;          // cmp rNNd, pages
        *(unsigned*)p = pages; p += 4;
        *p++ = 0x0F; *p++ = s.jcc;                            // jcc loop head
        *(int*)p = (int)((c.base + s.loopRva) - ((uintptr_t)p + 4)); p += 4;
        *p++ = 0xE9;                                          // jmp exit
        *(int*)p = (int)((c.base + s.exitRva) - ((uintptr_t)p + 4)); p += 4;
        hookSite(site, 10, entry);
        L("OK: page bound +0x%X -> %u pages (cave 0x%llx)\n",
          s.rva, pages, (unsigned long long)(uintptr_t)entry);
    }
}

bool installGame(const GameModule& game, GameContext& ctx) {
    g_ctx = &ctx;
    if (!game.resolve(ctx)) { L("ABORT: site resolution failed.\n"); return false; }

    bool backing = ctx.slots > STOCK_BACKING;

    // verify everything up front - fails closed on a game update
    for (int i = 0; i < ctx.nSetters; ++i)
        if (memcmp((void*)(ctx.base + ctx.setters[i].rva), ctx.setters[i].expect, SETTER_LEN) != 0) {
            L("ABORT: setter site +0x%X mismatch. Game updated? Not patching.\n", ctx.setters[i].rva);
            return false;
        }
    if (backing) {
        bool ok = ctx.entryExpectLen > 0 && ctx.nPages == MAX_PAGES
               && memcmp((void*)(ctx.base + ctx.entryRva), ctx.entryExpect, ctx.entryExpectLen) == 0
               && memcmp((void*)(ctx.base + ctx.iconRva), ctx.iconExpect, 6) == 0;
        for (int i = 0; i < ctx.nPages; ++i)
            ok = ok && memcmp((void*)(ctx.base + ctx.pages[i].rva), ctx.pages[i].expect, 10) == 0;
        if (!ok) {
            L("WARNING: a backing-expansion site mismatches; game updated?\n"
              "         Falling back to MaxSlots=%d (ranges only).\n", STOCK_BACKING);
            ctx.slots = STOCK_BACKING;
            backing = false;
        }
    }

    unsigned char* cave = (unsigned char*)allocNear((void*)(ctx.base + ctx.setters[0].rva), 4096);
    if (!cave) { L("ABORT: could not allocate a code cave within 2GB.\n"); return false; }
    unsigned char* p = cave;

    for (int i = 0; i < ctx.nSetters; ++i) {
        unsigned char* site = (unsigned char*)(ctx.base + ctx.setters[i].rva);
        unsigned char* entry = p;
        if (ctx.setters[i].paramInRsi)
            p = emitCallThunkRsi(p, site, ctx.setters[i].expect, SETTER_LEN, FixRanges);
        else
            p = emitCallThunk(p, site, ctx.setters[i].expect, SETTER_LEN,
                              ctx.setters[i].paramInRdx ? 1 : 0, FixRanges);
        if (!hookSite(site, SETTER_LEN, entry)) { L("ABORT: hook +0x%X failed.\n", ctx.setters[i].rva); return false; }
        L("OK: ranges hook +0x%X -> cave 0x%llx\n", ctx.setters[i].rva, (unsigned long long)(uintptr_t)entry);
    }

    // Autosave history: mirror the live autosave into a ring of extra slots.
    // The hook replays the game's `mov edx,<slot>` and calls our helper before
    // the write, so the live slot keeps its stock meaning ("load latest").
    if (ctx.autoHistCount > 0 && ctx.autoRva && ctx.autoLen) {
        unsigned char* site = (unsigned char*)(ctx.base + ctx.autoRva);
        if (memcmp(site, ctx.autoExpect, ctx.autoLen) != 0) {
            L("WARNING: autosave site +0x%X mismatch; autosave history disabled.\n", ctx.autoRva);
        } else {
            unsigned char* entry = p;
            p = emitCallThunk(p, site, ctx.autoExpect, ctx.autoLen, 4, (void*)AutosaveHistoryTick);
            if (hookSite(site, ctx.autoLen, entry))
                L("OK: autosave-history hook +0x%X -> cave 0x%llx (ring %u..%u)\n",
                  ctx.autoRva, (unsigned long long)(uintptr_t)entry,
                  ctx.autoHistStart,
                  ctx.autoHistStart + (unsigned)ctx.autoHistCount - 1);
            else
                L("ABORT: autosave-history hook +0x%X failed.\n", ctx.autoRva);
        }
    }

    if (backing) {
        unsigned char* site = (unsigned char*)(ctx.base + ctx.entryRva);
        unsigned char* entry = p;
        p = emitCallThunk(p, site, ctx.entryExpect, ctx.entryExpectLen, ctx.backingArgMode, FixBacking);
        if (!hookSite(site, ctx.entryExpectLen, entry)) { L("ABORT: backing hook +0x%X failed.\n", ctx.entryRva); return false; }
        L("OK: backing hook +0x%X -> cave 0x%llx\n", ctx.entryRva, (unsigned long long)(uintptr_t)entry);
        g_pageCave = p;   // page-loop caves emitted lazily by patchFixedBounds
    }
    return true;
}
