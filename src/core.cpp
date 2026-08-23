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

// Called from the setter caves with the dialog param block.
// Rebuild as an ASCENDING, GAP-FREE sequence ending in the new block:
//   [manual <170] [{170,170}] [{171,174}] [{175,198} filler] [{199..hi}]
// Three constraints drive this layout (2026-08 game update):
//  1. The save-menu renderer needs display_position == slot_number, so the
//     list may not skip slots.
//  2. The UI labels tiles 1-based, so a tile showing folder saveNNN sits at
//     position NNN and is labelled NNN+1. Starting the new block at slot 199
//     puts folder save199 on the tile labelled "200" — what the player reads
//     matches the folder number for every new slot.
//  3. The tile-content pass no longer derives its tile count from the range
//     list. It enumerates positions 0 .. param[+0xC]-1 (hardcoded: save=170,
//     load=180, title-load=200, system=20) and looks each slot up in the
//     enumerated-saves tree. So param[+0xC] must be raised alongside the
//     ranges or the extra slots never get tiles.
static void __fastcall FixRanges(unsigned char* param) {
    if (!param) return;
    Range* r = *(Range**)(param + 0x70);
    unsigned n = *(unsigned*)(param + 0x78);
    if (!r || n == 0 || n > 7) return;

    const unsigned APP_START = NEW_START - 1;      // 199: label == folder no.

    int backing = g_backingSlots ? g_backingSlots : STOCK_BACKING;
    unsigned hi = (unsigned)((g_ctx->slots < backing ? g_ctx->slots : backing) - 1);
    if (hi < APP_START) return;

    // Our previously appended block? Extend it in place.
    for (unsigned i = 0; i < n; ++i) {
        if (r[i].start == APP_START) {
            *(unsigned*)(param + 0xC) = hi + 1;
            if (r[i].end != hi) {
                r[i].end = hi;
                *(unsigned*)(param + 0x78) = n;
                L("FixRanges: extended new block to %u\n", hi);
            }
            return;
        }
    }
    // Reserved system dialog ({180,199}) or unknown high-slot list: untouched.
    // (Our own lists never reach here - the extend-loop above returns first.)
    for (unsigned i = 0; i < n; ++i)
        if (r[i].end >= 180) return;

    const Range* src170 = nullptr;
    const Range* src171 = nullptr;
    for (unsigned i = 0; i < n; ++i) {
        if      (r[i].start == 170) src170 = &r[i];
        else if (r[i].start == 171) src171 = &r[i];
    }

    Range tmp[8]; unsigned m = 0;
    for (unsigned i = 0; i < n && m < 8; ++i)      // manual slots, stock order
        if (r[i].start < 170) tmp[m++] = r[i];
    if (src170) { if (m < 8) tmp[m++] = *src170; } // autosave, verbatim
    else { tmp[m].start = 170; tmp[m].end = 170; tmp[m].flag = 1; ++m; }
    if (src171) { if (m < 8) tmp[m++] = *src171; } // chapter-clears, verbatim
    else { tmp[m].start = 171; tmp[m].end = 174; tmp[m].flag = 1; ++m; }
    // filler over the rest of the reserved window (New Game+ slots!). The
    // enable-flag byte is cleared so the UI treats these tiles as inactive;
    // they still OCCUPY positions, which keeps position == slot continuity
    // for everything after them.
    unsigned fs = 175;
    if (src171 && src171->end >= fs) fs = src171->end + 1;
    if (fs <= APP_START - 1 && m < 8) {
        tmp[m].start = fs; tmp[m].end = APP_START - 1; tmp[m].flag = 0; ++m;
    }
    if (m < 8) { tmp[m].start = APP_START; tmp[m].end = hi; tmp[m].flag = 1; ++m; }

    memcpy(r, tmp, m * sizeof(Range));
    *(unsigned*)(param + 0x78) = m;
    *(unsigned*)(param + 0xC) = hi + 1;   // tile-count bound (see note 3 above)
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
    if (param) FixRanges(param);
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

    unsigned char* cave = (unsigned char*)allocNear((void*)(ctx.base + ctx.setters[0].rva), 1024);
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
