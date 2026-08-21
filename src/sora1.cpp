// SoraSaveSlots - Trails in the Sky 1st Chapter (sora_1st.exe, 2025 remake).
//
// Accessor layout (fdk::savedata::AccessorWin32):
//   +0x23A0  slot array data pointer (INLINE array at this+0x780, 300 entries)
//   +0x23A8  slot array count
//   +0x37B8  dialog param-block pointer   {+0x70 range array, +0x78 count}
//   +0x37DC  "max" field (write-only)
//
// Backing redirect point: the accessor constructor tail
// (`mov [r14+37DCh], 12Ch`, 11 bytes) - runs once per accessor, r14 = this.
#include "games.h"

namespace {

// 3x setter: mov [rcx+37B8h], r8
const unsigned char s_r8b[] = { 0x4C,0x89,0x81,0xB8,0x37,0x00,0x00 };
const unsigned char s_r8m[] = { 0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF };
// 1x setter: mov [rcx+37B8h], rdx
const unsigned char s_rdxb[] = { 0x48,0x89,0x91,0xB8,0x37,0x00,0x00 };
const unsigned char s_rdxm[] = { 0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF };
// ctor tail: mov dword ptr [r14+37DCh], 12Ch (REX.B prefix is part of the
// pattern: matching without it hooks mid-instruction - v3 startup crash)
const unsigned char s_ctorb[] = { 0x41,0xC7,0x86,0xDC,0x37,0x00,0x00,0x2C,0x01,0x00,0x00 };
const unsigned char s_ctorm[] = { 0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF };
// icon pass: cmp esi, 12Ch
const unsigned char s_iconb[] = { 0x81,0xFE,0x2C,0x01,0x00,0x00 };
const unsigned char s_iconm[] = { 0xFF,0xFF,0xFF,0xFF,0xFF,0xFF };
// page loops: cmp rNNd, 3Ch / jcc rel32 (branch disp resolved from bytes)
const unsigned char s_page1b[] = { 0x41,0x83,0xFD,0x3C,0x0F,0x8C,0x00,0x00,0x00,0x00 }; // r13d / jl
const unsigned char s_page1m[] = { 0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0x00,0x00,0x00,0x00 };
const unsigned char s_page2b[] = { 0x41,0x83,0xFC,0x3C,0x0F,0x82,0x00,0x00,0x00,0x00 }; // r12d / jb
const unsigned char s_page2m[] = { 0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0x00,0x00,0x00,0x00 };

bool resolve(GameContext& c) {
    c.offSlotData = 0x23A0;
    c.offSlotCnt  = 0x23A8;
    c.offParam    = 0x37B8;
    c.offSlotMax  = 0x37DC;
    c.iconImmOff     = 2;      // `81 FE imm32` - no REX prefix
    c.backingArgMode = 2;      // accessor arrives in r14 at the ctor tail

    unsigned hits[8];
    int n;

    // 3 r8 setters (order-preserving: save, load, other)
    n = scanText(c, AOB("s1_setter_r8", s_r8b, s_r8m, 3), hits, 3);
    if (n != 3) { L("ABORT: sora1 setter_r8 found %d (want 3). Game updated?\n", n); return false; }
    for (int i = 0; i < 3; ++i) {
        c.setters[c.nSetters].rva = hits[i];
        memcpy(c.setters[c.nSetters].expect, s_r8b, SETTER_LEN);
        c.setters[c.nSetters].paramInRdx = false;
        c.setters[c.nSetters].paramInRsi = false;
        ++c.nSetters;
    }
    // 1 rdx setter (common dialog)
    n = scanText(c, AOB("s1_setter_rdx", s_rdxb, s_rdxm, 1), hits, 1);
    if (n != 1) { L("ABORT: sora1 setter_rdx found %d (want 1)\n", n); return false; }
    c.setters[c.nSetters].rva = hits[0];
    memcpy(c.setters[c.nSetters].expect, s_rdxb, SETTER_LEN);
    c.setters[c.nSetters].paramInRdx = true;
    c.setters[c.nSetters].paramInRsi = false;
    ++c.nSetters;

    // ctor tail = backing redirect site
    n = scanText(c, AOB("s1_ctor", s_ctorb, s_ctorm, 1), hits, 1);
    if (n != 1) { L("ABORT: sora1 ctor found %d (want 1)\n", n); return false; }
    c.entryRva = hits[0];
    c.entryLen = sizeof(s_ctorb);
    memcpy(c.entryExpect, s_ctorb, sizeof(s_ctorb));
    c.entryExpectLen = sizeof(s_ctorb);

    // icon-pass bound
    n = scanText(c, AOB("s1_icon", s_iconb, s_iconm, 1), hits, 1);
    if (n != 1) { L("ABORT: sora1 icon found %d (want 1)\n", n); return false; }
    c.iconRva = hits[0];
    memcpy(c.iconExpect, s_iconb, 6);

    // page loops: fixed prefix + read branch disp from the resolved bytes
    struct PRef { const unsigned char* b; unsigned char modRM; unsigned char jcc; };
    const PRef refs[2] = { { s_page1b, 0xFD, 0x8C }, { s_page2b, 0xFC, 0x82 } };
    for (int i = 0; i < 2; ++i) {
        AobPat p = AOB(i ? "s1_page2" : "s1_page1",
                       i ? s_page2b : s_page1b,
                       i ? s_page2m : s_page1m, 1);
        n = scanText(c, p, hits, 1);
        if (n != 1) { L("ABORT: sora1 page%d found %d (want 1)\n", i + 1, n); return false; }
        unsigned rva = hits[0];
        const unsigned char* site = (const unsigned char*)c.base + rva;
        c.pages[i].rva = rva;
        memcpy(c.pages[i].expect, site, 10);
        c.pages[i].cmpModRM = refs[i].modRM;
        c.pages[i].jcc      = refs[i].jcc;
        int disp = *(const int*)(site + 6);          // disp32 = last 4 bytes
        c.pages[i].loopRva = rva + 10 + (unsigned)disp;
        c.pages[i].exitRva = rva + 10;
    }
    c.nPages = 2;

    L("sites resolved (sora_1st): entry +0x%X, icon +0x%X, page1 +0x%X, page2 +0x%X\n",
      c.entryRva, c.iconRva, c.pages[0].rva, c.pages[1].rva);
    for (int i = 0; i < c.nSetters; ++i)
        L("  setter%d +0x%X (%s)\n", i, c.setters[i].rva,
          c.setters[i].paramInRdx ? "rdx" : "r8");
    return true;
}

} // namespace

const GameModule game_sora1 = { L"sora_1st.exe", "sora_1st", resolve };
