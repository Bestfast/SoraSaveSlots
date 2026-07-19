// SoraSaveSlots - raises the save-slot cap in Trails in the Sky 1st Chapter (2025 remake).
//
// v3 - up to 999 total slots, reserved slots displayed at the bottom.
//
// Two independent patches:
//
// 1. RANGES. Every save/load dialog is described by a param block built in
//    savedata_saveload_state.cpp; it contains a list of {start,end,flag} slot
//    ranges (data ptr at param+0x70, count at param+0x78, inline capacity 8).
//    The accessor stores a pointer to it at this+0x37B8 and everything
//    (confirm at +0x56EDC0, enable pass at +0x56E080, enumeration at
//    +0x570200, display) validates against it. We hook the four vtable
//    setters that store the pointer and REBUILD the list as:
//        [manual ranges] [{200, MaxSlots-1}] [reserved ranges]
//    so the new slots continue directly after the manual ones and the
//    autosave / chapter-clear entries move to the bottom. The reserved
//    system dialog (its list is exactly {180,199}) is detected by content
//    and left untouched.
//
// 2. BACKING (only when MaxSlots > 300). The slot array (24-byte entries) is
//    INLINE in the AccessorWin32 object: the constructor points the data
//    field this+0x23A0 at this+0x780 and the 300 entries end exactly at
//    +0x23A0, so it cannot grow in place. But every consumer loads the data
//    pointer, so the ctor-tail hook redirects it to our own zeroed
//    allocation and raises count (+0x23A8) and max (+0x37DC). Three fixed
//    bounds must then be raised too:
//      +0x56DF34  icon-pass loop      cmp esi, 300      (imm32, in place)
//      +0x56E50B  enable-pass pages   cmp r13d, 60 / jl (cave)
//      +0x57354E  tile-create pages   cmp r12d, 60 / jb (cave)
//    The tile loop indexes slots up to pages*5-1, so the buffer is sized to
//    ceil(N/5)*5 entries; the spare entries stay zero, which the tile code
//    treats as "no icon".
//    These three are only patched once the ctor hook has actually run
//    (g_backingSlots), so if the accessor was built before we loaded, the
//    plugin degrades to the safe 300-slot behaviour instead of overflowing.
//
// Hard ceiling 999: the save path format is save%03d.
// Slot map: 0-169 manual, 170 autosave, 171-174 chapter clear, 175-179
// reserved presentation block, 180-199 reserved dialog window, 200+ free.
// See docs/FINDINGS.md.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <share.h>

// ---------------------------------------------------------------- config
#define DEFAULT_SLOTS   999
#define FILENAME_LIMIT  999     // save%03d
#define STOCK_BACKING   300     // inline slot array in the accessor object
#define NEW_START       200     // first slot past every reserved block

// ---------------------------------------------------------------- hook sites
#define SETTER_LEN 7
struct SetterSite {
    unsigned rva;
    unsigned char expect[SETTER_LEN];
    bool paramInRdx;            // param block reg: rdx instead of r8
};
// AccessorWin32 setters that store the dialog param block at this+0x37B8.
static const SetterSite kSetters[] = {
    { 0x56C29A, { 0x4C,0x89,0x81,0xB8,0x37,0x00,0x00 }, false },  // save dialog
    { 0x56C59A, { 0x4C,0x89,0x81,0xB8,0x37,0x00,0x00 }, false },  // load dialog
    { 0x56C68A, { 0x4C,0x89,0x81,0xB8,0x37,0x00,0x00 }, false },
    { 0x56C9E3, { 0x48,0x89,0x91,0xB8,0x37,0x00,0x00 }, true  },  // common dialog
};
#define NSETTERS (sizeof(kSetters)/sizeof(kSetters[0]))

// ctor tail: mov dword ptr [r14+37DCh], 12Ch   (accessor in r14)
// 11 bytes INCLUDING the 41 REX.B prefix. Matching from 0x56BF0B without the
// prefix also passes a memcmp but hooks mid-instruction and replays the store
// as [rsi+37DCh] - rsi is the slot-fill loop counter there. That was the v3
// startup crash.
#define CTOR_RVA  0x56BF0A
#define CTOR_LEN  11
static const unsigned char kCtorExpect[CTOR_LEN] =
    { 0x41,0xC7,0x86,0xDC,0x37,0x00,0x00,0x2C,0x01,0x00,0x00 };

// icon pass: cmp esi, 12Ch (81 FE imm32) - imm patched in place
#define ICON_RVA  0x56DF34
static const unsigned char kIconExpect[6] = { 0x81,0xFE,0x2C,0x01,0x00,0x00 };

// page loops: cmp rNNd, 3Ch / jcc rel32  (10 bytes each)
struct PageSite {
    unsigned rva;
    unsigned char expect[10];
    unsigned char cmpModRM;     // FD = r13d, FC = r12d
    unsigned char jcc;          // 8C = jl, 82 = jb (0F-prefixed)
    unsigned loopRva;           // jcc target (loop head)
    unsigned exitRva;           // fall-through
};
static const PageSite kPages[] = {
    { 0x56E50B, { 0x41,0x83,0xFD,0x3C,0x0F,0x8C,0x5D,0xFD,0xFF,0xFF },
      0xFD, 0x8C, 0x56E272, 0x56E515 },   // enable/grey-out pass
    { 0x57354E, { 0x41,0x83,0xFC,0x3C,0x0F,0x82,0xD8,0xFD,0xFF,0xFF },
      0xFC, 0x82, 0x573330, 0x573558 },   // tile creation
};
#define NPAGES (sizeof(kPages)/sizeof(kPages[0]))

// accessor object offsets
#define OFF_SLOTDATA  0x23A0    // slot array data pointer
#define OFF_SLOTCNT   0x23A8    // slot array count (u32)
#define OFF_SLOTMAX   0x37DC    // "max" field (write-only in the game)

// ---------------------------------------------------------------- state
static FILE* g_log;
static int   g_slots;                    // configured total
static uintptr_t g_base;
static volatile LONG g_backingSlots;     // !=0 once a ctor ran with our buffer
static volatile LONG g_boundsPatched;

static void L(const char* f, ...) {
    if (!g_log) return;
    va_list ap; va_start(ap, f); vfprintf(g_log, f, ap); va_end(ap);
    fflush(g_log);
}

static void siblingPath(wchar_t* out, size_t n, HMODULE self, const wchar_t* ext) {
    GetModuleFileNameW(self, out, (DWORD)n);
    wchar_t* dot = wcsrchr(out, L'.');
    if (dot) *dot = 0;
    wcscat_s(out, n, ext);
}

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

static bool patchBytes(unsigned char* site, const unsigned char* bytes, int len) {
    DWORD old;
    if (!VirtualProtect(site, len, PAGE_EXECUTE_READWRITE, &old)) return false;
    memcpy(site, bytes, len);
    VirtualProtect(site, len, old, &old);
    FlushInstructionCache(GetCurrentProcess(), site, len);
    return true;
}

// ---------------------------------------------------------------- C patch logic

struct Range { unsigned start, end, flag; };

// Called from the setter caves with the dialog param block.
// Rebuild the range list: manual ranges, then {NEW_START, hi}, then reserved.
static void __fastcall FixRanges(unsigned char* param) {
    if (!param) return;
    Range* r = *(Range**)(param + 0x70);
    unsigned n = *(unsigned*)(param + 0x78);
    if (!r || n == 0 || n > 7) return;
    for (unsigned i = 0; i < n; ++i)
        if (r[i].end >= 180) return;    // reserved system dialog, or already patched

    // Backing not expanded (accessor predates us, or MaxSlots<=300): cap at 299.
    int backing = g_backingSlots ? g_backingSlots : STOCK_BACKING;
    unsigned hi = (unsigned)((g_slots < backing ? g_slots : backing) - 1);
    if (hi < NEW_START) return;

    Range tmp[8]; unsigned m = 0;
    for (unsigned i = 0; i < n; ++i)          // manual slots first
        if (r[i].start < 170) tmp[m++] = r[i];
    tmp[m].start = NEW_START;                 // then the new block
    tmp[m].end = hi;
    tmp[m].flag = 1;
    ++m;
    for (unsigned i = 0; i < n; ++i)          // reserved slots at the bottom
        if (r[i].start >= 170) tmp[m++] = r[i];

    memcpy(r, tmp, m * sizeof(Range));
    *(unsigned*)(param + 0x78) = m;
}

static void patchFixedBounds();  // fwd

// Called from the ctor-tail cave with the accessor object (MaxSlots > 300 only).
// Redirect the inline slot array to a bigger zeroed buffer.
static void __fastcall FixBacking(unsigned char* accessor) {
    int slots = g_slots;
    unsigned entries = ((unsigned)(slots + 4) / 5) * 5;   // tile loop indexes pages*5
    void* buf = VirtualAlloc(nullptr, (size_t)entries * 0x18,
                             MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!buf) {
        L("FixBacking: allocation failed; this accessor stays at 300 slots.\n");
        return;
    }
    *(void**)   (accessor + OFF_SLOTDATA) = buf;
    *(unsigned*)(accessor + OFF_SLOTCNT)  = (unsigned)slots;
    *(unsigned*)(accessor + OFF_SLOTMAX)  = (unsigned)slots;
    InterlockedExchange(&g_backingSlots, slots);
    patchFixedBounds();   // safe only now that the backing store is big enough
    L("FixBacking: accessor %p -> %u-entry buffer %p, count/max = %d\n",
      accessor, entries, buf, slots);
}

// ---------------------------------------------------------------- cave emission

// Emit: <orig bytes>, save volatile regs + xmm0-5, call `target` with one
// argument, restore, jmp back to site+origLen.
// argMode: 0 = r8, 1 = rdx, 2 = r14  (value as of the hook site).
static unsigned char* emitCallThunk(unsigned char* p, unsigned char* site,
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
    if (argMode == 0)      { *p++ = 0x48; *p++ = 0x8B; *p++ = 0x4B; *p++ = 0x20; } // mov rcx,[rbx+20h] (r8)
    else if (argMode == 1) { *p++ = 0x48; *p++ = 0x8B; *p++ = 0x4B; *p++ = 0x28; } // mov rcx,[rbx+28h] (rdx)
    else                   { *p++ = 0x49; *p++ = 0x8B; *p++ = 0xCE; }              // mov rcx, r14
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

static bool hookSite(unsigned char* site, int siteLen, unsigned char* caveEntry) {
    unsigned char jmp[16];
    jmp[0] = 0xE9;
    *(int*)(jmp + 1) = (int)((uintptr_t)caveEntry - ((uintptr_t)site + 5));
    for (int i = 5; i < siteLen; ++i) jmp[i] = 0x90;
    return patchBytes(site, jmp, siteLen);
}

// ---------------------------------------------------------------- install

// icon-loop imm32 + the two page loops. Called from FixBacking (game thread),
// exactly once, only after a backing buffer exists.
static unsigned char* g_pageCave;   // pre-emitted at init, pre-verified sites
static void patchFixedBounds() {
    if (InterlockedExchange(&g_boundsPatched, 1)) return;
    int slots = g_slots;
    unsigned pages = ((unsigned)slots + 4) / 5;

    unsigned char iconImm[4];
    *(unsigned*)iconImm = (unsigned)slots;
    patchBytes((unsigned char*)(g_base + ICON_RVA + 2), iconImm, 4);
    L("OK: icon-pass bound +0x%X -> %d\n", ICON_RVA, slots);

    unsigned char* p = g_pageCave;
    for (size_t i = 0; i < NPAGES; ++i) {
        const PageSite& s = kPages[i];
        unsigned char* site = (unsigned char*)(g_base + s.rva);
        unsigned char* entry = p;
        *p++ = 0x41; *p++ = 0x81; *p++ = s.cmpModRM;          // cmp rNNd, pages
        *(unsigned*)p = pages; p += 4;
        *p++ = 0x0F; *p++ = s.jcc;                            // jcc loop head
        *(int*)p = (int)((g_base + s.loopRva) - ((uintptr_t)p + 4)); p += 4;
        *p++ = 0xE9;                                          // jmp exit
        *(int*)p = (int)((g_base + s.exitRva) - ((uintptr_t)p + 4)); p += 4;
        hookSite(site, 10, entry);
        L("OK: page bound +0x%X -> %u pages (cave 0x%llx)\n",
          s.rva, pages, (unsigned long long)(uintptr_t)entry);
    }
}

static bool installHooks() {
    bool backing = g_slots > STOCK_BACKING;

    // verify everything up front - fails closed on a game update
    for (size_t i = 0; i < NSETTERS; ++i)
        if (memcmp((void*)(g_base + kSetters[i].rva), kSetters[i].expect, SETTER_LEN) != 0) {
            L("ABORT: setter site +0x%X mismatch. Game updated? Not patching.\n", kSetters[i].rva);
            return false;
        }
    if (backing) {
        bool ok = memcmp((void*)(g_base + CTOR_RVA), kCtorExpect, CTOR_LEN) == 0
               && memcmp((void*)(g_base + ICON_RVA), kIconExpect, 6) == 0;
        for (size_t i = 0; i < NPAGES; ++i)
            ok = ok && memcmp((void*)(g_base + kPages[i].rva), kPages[i].expect, 10) == 0;
        if (!ok) {
            L("WARNING: a backing-expansion site mismatches; game updated?\n"
              "         Falling back to MaxSlots=300 (ranges only).\n");
            g_slots = STOCK_BACKING;
            backing = false;
        }
    }

    unsigned char* cave = (unsigned char*)allocNear((void*)(g_base + kSetters[0].rva), 1024);
    if (!cave) { L("ABORT: could not allocate a code cave within 2GB.\n"); return false; }
    unsigned char* p = cave;

    for (size_t i = 0; i < NSETTERS; ++i) {
        unsigned char* site = (unsigned char*)(g_base + kSetters[i].rva);
        unsigned char* entry = p;
        p = emitCallThunk(p, site, kSetters[i].expect, SETTER_LEN,
                          kSetters[i].paramInRdx ? 1 : 0, FixRanges);
        if (!hookSite(site, SETTER_LEN, entry)) { L("ABORT: hook +0x%X failed.\n", kSetters[i].rva); return false; }
        L("OK: ranges hook +0x%X -> cave 0x%llx\n", kSetters[i].rva, (unsigned long long)(uintptr_t)entry);
    }

    if (backing) {
        unsigned char* site = (unsigned char*)(g_base + CTOR_RVA);
        unsigned char* entry = p;
        p = emitCallThunk(p, site, kCtorExpect, CTOR_LEN, 2, FixBacking);
        if (!hookSite(site, CTOR_LEN, entry)) { L("ABORT: ctor hook failed.\n"); return false; }
        L("OK: backing hook +0x%X -> cave 0x%llx\n", CTOR_RVA, (unsigned long long)(uintptr_t)entry);
        g_pageCave = p;   // page-loop caves emitted lazily by patchFixedBounds
    }
    return true;
}

static DWORD WINAPI init(LPVOID param) {
    HMODULE self = (HMODULE)param;
    wchar_t logp[MAX_PATH], inip[MAX_PATH];
    siblingPath(logp, MAX_PATH, self, L".log");
    siblingPath(inip, MAX_PATH, self, L".ini");
    g_log = _wfsopen(logp, L"w", _SH_DENYNO);

    L("SoraSaveSlots v3\n");

    int slots = (int)GetPrivateProfileIntW(L"SoraSaveSlots", L"MaxSlots", DEFAULT_SLOTS, inip);
    if (slots > FILENAME_LIMIT) {
        L("WARNING: MaxSlots=%d exceeds the save%%03d filename limit; clamping to %d.\n",
          slots, FILENAME_LIMIT);
        slots = FILENAME_LIMIT;
    }
    if (slots <= NEW_START) {
        L("MaxSlots=%d adds nothing (new slots start at %d; 175-199 are the game's).\n"
          "Not patching.\n", slots, NEW_START);
        return 0;
    }
    g_slots = slots;

    HMODULE exe = GetModuleHandleW(nullptr);
    g_base = (uintptr_t)exe;
    L("MaxSlots = %d, sora_1st.exe base = 0x%llx\n", slots, (unsigned long long)g_base);

    installHooks();
    return 0;
}

BOOL APIENTRY DllMain(HMODULE h, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(h);
        CreateThread(nullptr, 0, init, h, 0, nullptr);
    }
    return TRUE;
}
