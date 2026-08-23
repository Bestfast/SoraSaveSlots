// SoraSaveSlots - shared core: types, constants and the engine API that the
// per-game modules (sora1.cpp, sora2.cpp, ...) plug into. See dllmain.cpp for
// the big-picture comments.
#pragma once
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <share.h>
#include <string.h>

// ---------------------------------------------------------------- config
static const int DEFAULT_SLOTS  = 999;
static const int FILENAME_LIMIT = 999;   // save%03d
static const int STOCK_BACKING  = 300;   // slot array the games allocate
static const unsigned NEW_START = 200;   // first slot past every reserved block
static const unsigned DYNAMIC_HEADROOM = 50;      // free tiles past last save on disk
static const unsigned DYNAMIC_FLOOR    = 250;     // never shrink the window below this

static const int MAX_SETTERS      = 8;
static const int MAX_PAGES        = 2;
static const int ENTRY_EXPECT_MAX = 16;
static const int MAX_INSTALLED    = 8;

// ---------------------------------------------------------------- AOB
struct AobPat {
    const char* name;
    const unsigned char* bytes;   // pattern
    const unsigned char* mask;    // 1 = must match, 0 = any
    int len;
    int want;                     // n = require exactly n hits
};
#define AOB(n, b, m, w) { n, b, m, (int)sizeof(b), w }

// ---------------------------------------------------------------- hook sites
struct SetterSite {                       // `mov [reg+offParam], reg` store sites
    unsigned rva;                         // resolved at load time
    unsigned char expect[7];              // original 7 bytes (SETTER_LEN)
    bool paramInRdx;                      // param block arrives in rdx (else r8)
    bool paramInRsi;                      // param block arrives in rsi
};
static const int SETTER_LEN = 7;

struct PageSite {                         // `cmp rNNd,60 / jcc rel32` page loops
    unsigned rva;
    unsigned char expect[10];
    unsigned char cmpModRM;               // modrm of the cmp register
    unsigned char jcc;                    // low byte of the 0F-prefixed branch
    unsigned loopRva;                     // branch target (loop head)
    unsigned exitRva;                     // fall-through
};

struct Range {                            // one dialog range entry (stride 12)
    unsigned start, end, flag;
};

// Everything a game module fills in / the core consumes.
struct GameContext {
    HMODULE    self;
    const char* label;
    uintptr_t  base;
    int        slots;                       // configured MaxSlots

    // accessor object layout (per game)
    unsigned offSlotData;                  // slot array data pointer
    unsigned offSlotCnt;                   // slot array count (u32)
    unsigned offParam;                     // dialog param-block pointer
    int      offSlotMax;                   // "max" field, -1 if none

    // %USERPROFILE%\Saved Games\FALCOM\<this>\savedata - for the dynamic window
    const wchar_t* savedGamesDir;
    int      dynamicWindow;                // cap visible slots to max-on-disk + headroom
    int      offTileContainer;             // accessor offset of the tile container
                                          // (-1 = ordered-mode experiment off)

    // resolved hook sites
    SetterSite setters[MAX_SETTERS];
    int        nSetters;
    unsigned   iconRva;                    // icon-pass loop bound (`cmp rNN,N`)
    unsigned   iconImmOff;                 // offset of its imm32 within the instr
    unsigned char iconExpect[6];           // original bytes at the icon bound
    unsigned   entryRva;                   // backing redirect site (function entry)
    unsigned   entryLen;                   // bytes to hijack there
    unsigned char entryExpect[ENTRY_EXPECT_MAX];
    unsigned   entryExpectLen;
    int        backingArgMode;             // thunk arg mode at entryRva (see core.cpp)
    PageSite   pages[MAX_PAGES];
    int        nPages;
};

// One per supported game.
struct GameModule {
    const wchar_t* exe;                    // host executable file name
    const char*    label;                  // log name
    bool (*resolve)(GameContext& ctx);     // fill offsets + sites; false = abort
};

// ---------------------------------------------------------------- core API
void     coreOpenLog(const wchar_t* path);
void     coreCloseLog();
void     L(const char* f, ...);

int      scanText(const GameContext& ctx, const AobPat& p, unsigned* out, int max);
unsigned findFunctionStart(uintptr_t base, unsigned rva);   // .pdata walk

bool     patchBytes(unsigned char* site, const unsigned char* bytes, int len);
bool     hookSite(unsigned char* site, int siteLen, unsigned char* caveEntry);
unsigned char* emitCallThunk(unsigned char* p, unsigned char* site,
                             const unsigned char* orig, int origLen,
                             int argMode, void* target);
unsigned char* emitCallThunkRsi(unsigned char* p, unsigned char* site,
                                const unsigned char* orig, int origLen,
                                void* target);

// Full install flow: resolve -> verify -> cave emission -> patch.
// Returns false if nothing was patched (fails closed).
bool     installGame(const GameModule& game, GameContext& ctx);
