// SoraSaveSlots - Trails in the Sky 2nd Chapter (sora_2nd.exe, 2025 remake).
//
// Accessor layout (fdk::savedata::AccessorWin32, reworked vs sora_1st):
//   +0x23B0  slot array data pointer (HEAP array, 24-byte entries)
//   +0x23B8  slot array count
//   +0x37D8  dialog param-block pointer   {+0x70 range array, +0x78 count}
//   (no separate max field; the executor validates through the ranges)
//
// Backing redirect point: the icon-pass function entry (FUN_140694530,
// `mov [rsp+18h],rbx`, 5 bytes, rcx = accessor). The entry RVA is derived
// from the already-unique icon-loop bound via the .pdata function table -
// a naive prologue AOB matched an unrelated function in the wild.
#include "games.h"

namespace {

// 3x setter: mov [rcx+37D8h], r8
const unsigned char s_r8b[] = { 0x4C,0x89,0x81,0xD8,0x37,0x00,0x00 };
const unsigned char s_r8m[] = { 0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF };
// 1x setter: mov [r14+37D8h], rsi
const unsigned char s_rsib[] = { 0x49,0x89,0xB6,0xD8,0x37,0x00,0x00 };
const unsigned char s_rsim[] = { 0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF };
// 1x setter: mov [rcx+37D8h], rdx
const unsigned char s_rdxb[] = { 0x48,0x89,0x91,0xD8,0x37,0x00,0x00 };
const unsigned char s_rdxm[] = { 0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF };
// icon-pass entry prologue (verified before hooking, never scanned for)
const unsigned char s_entryOrig[5] = { 0x48,0x89,0x5C,0x24,0x18 }; // mov [rsp+18h],rbx
// icon-pass loop bound: cmp r14d, 12Ch (REX.B + 81 /7 id)
const unsigned char s_iconb[] = { 0x41,0x81,0xFE,0x2C,0x01,0x00,0x00 };
const unsigned char s_iconm[] = { 0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF };
// page loops: cmp rNNd, 3Ch / jcc rel32
const unsigned char s_page1b[] = { 0x41,0x83,0xFD,0x3C,0x0F,0x8C,0x00,0x00,0x00,0x00 }; // r13d / jl
const unsigned char s_page1m[] = { 0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0x00,0x00,0x00,0x00 };
const unsigned char s_page2b[] = { 0x41,0x83,0xFC,0x3C,0x0F,0x82,0x00,0x00,0x00,0x00 }; // r12d / jc
const unsigned char s_page2m[] = { 0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0x00,0x00,0x00,0x00 };

bool resolve(GameContext& c) {
    c.offSlotData = 0x23B0;
    c.offSlotCnt  = 0x23B8;
    c.offParam    = 0x37D8;
    c.offSlotMax  = -1;
    c.iconImmOff     = 3;      // `41 81 FE imm32` - REX prefix shifts the imm
    c.backingArgMode = 4;      // accessor arrives in rcx at the icon entry

    unsigned hits[8];
    int n;

    // 3 r8 setters
    n = scanText(c, AOB("s2_setter_r8", s_r8b, s_r8m, 3), hits, 3);
    if (n != 3) { L("ABORT: sora2 setter_r8 found %d (want 3)\n", n); return false; }
    for (int i = 0; i < 3; ++i) {
        c.setters[c.nSetters].rva = hits[i];
        memcpy(c.setters[c.nSetters].expect, s_r8b, SETTER_LEN);
        c.setters[c.nSetters].paramInRdx = false;
        c.setters[c.nSetters].paramInRsi = false;
        ++c.nSetters;
    }
    // 1 rsi setter
    n = scanText(c, AOB("s2_setter_rsi", s_rsib, s_rsim, 1), hits, 1);
    if (n != 1) { L("ABORT: sora2 setter_rsi found %d (want 1)\n", n); return false; }
    c.setters[c.nSetters].rva = hits[0];
    memcpy(c.setters[c.nSetters].expect, s_rsib, SETTER_LEN);
    c.setters[c.nSetters].paramInRdx = false;
    c.setters[c.nSetters].paramInRsi = true;
    ++c.nSetters;
    // 1 rdx setter
    n = scanText(c, AOB("s2_setter_rdx", s_rdxb, s_rdxm, 1), hits, 1);
    if (n != 1) { L("ABORT: sora2 setter_rdx found %d (want 1)\n", n); return false; }
    c.setters[c.nSetters].rva = hits[0];
    memcpy(c.setters[c.nSetters].expect, s_rdxb, SETTER_LEN);
    c.setters[c.nSetters].paramInRdx = true;
    c.setters[c.nSetters].paramInRsi = false;
    ++c.nSetters;

    if (c.slots <= STOCK_BACKING) {
        L("MaxSlots<=%d: backing sites not needed, skipping.\n", STOCK_BACKING);
        return true;
    }

    // icon-pass loop bound (unique anchor), then the containing function's
    // entry via .pdata
    n = scanText(c, AOB("s2_icon", s_iconb, s_iconm, 1), hits, 1);
    if (n != 1) { L("ABORT: sora2 icon found %d (want 1)\n", n); return false; }
    c.iconRva = hits[0];
    memcpy(c.iconExpect, s_iconb, 6);

    c.entryRva = findFunctionStart(c.base, c.iconRva);
    if (!c.entryRva) { L("ABORT: sora2 icon entry not found in .pdata\n"); return false; }
    c.entryLen = sizeof(s_entryOrig);
    memcpy(c.entryExpect, s_entryOrig, sizeof(s_entryOrig));
    c.entryExpectLen = sizeof(s_entryOrig);

    // page loops
    struct PRef { const unsigned char* b; unsigned char modRM; unsigned char jcc; };
    const PRef refs[2] = { { s_page1b, 0xFD, 0x8C }, { s_page2b, 0xFC, 0x82 } };
    for (int i = 0; i < 2; ++i) {
        AobPat p = AOB(i ? "s2_page2" : "s2_page1",
                       i ? s_page2b : s_page1b,
                       i ? s_page2m : s_page1m, 1);
        n = scanText(c, p, hits, 1);
        if (n != 1) { L("ABORT: sora2 page%d found %d (want 1)\n", i + 1, n); return false; }
        unsigned rva = hits[0];
        const unsigned char* site = (const unsigned char*)c.base + rva;
        c.pages[i].rva = rva;
        memcpy(c.pages[i].expect, site, 10);
        c.pages[i].cmpModRM = refs[i].modRM;
        c.pages[i].jcc      = refs[i].jcc;
        int disp = *(const int*)(site + 6);
        c.pages[i].loopRva = rva + 10 + (unsigned)disp;
        c.pages[i].exitRva = rva + 10;
    }
    c.nPages = 2;

    L("sites resolved (sora_2nd): entry +0x%X, icon +0x%X, page1 +0x%X, page2 +0x%X\n",
      c.entryRva, c.iconRva, c.pages[0].rva, c.pages[1].rva);
    for (int i = 0; i < c.nSetters; ++i)
        L("  setter%d +0x%X (%s)\n", i, c.setters[i].rva,
          c.setters[i].paramInRsi ? "rsi" : (c.setters[i].paramInRdx ? "rdx" : "r8"));
    return true;
}

} // namespace

const GameModule game_sora2 = { L"sora_2nd.exe", "sora_2nd", resolve };
