// SoraSaveSlots - raises the save-slot cap in Trails in the Sky remakes
// (sora_1st.exe / sora_2nd.exe, auto-detected).
//
// Layout:
//   core.h/.cpp   shared engine: AOB scanning, .pdata lookup, code caves,
//                 register-preserving thunks, the range-list rebuild
//                 (FixRanges) and slot-array expansion (FixBacking)
//   sora1.cpp     sora_1st-specific AOB patterns + accessor offsets
//   sora2.cpp     sora_2nd-specific AOB patterns + accessor offsets
//   dllmain.cpp   entry point: log/ini bootstrap, game detection, dispatch
//
// Adding a game = write game_x.cpp defining a GameModule::resolve that fills
// a GameContext (accessor offsets + hook-site RVAs), and registering it in
// dllmain.cpp. Everything else is shared.
//
// Mechanism (identical model in both games so far):
//   RANGES  Every save/load dialog carries a param block whose {start,end,flag}
//           slot-range list (block+0x70 array / block+0x78 count) gates every
//           consumer: display, confirm, enumeration and the save/load executor.
//           The block pointer lives in the savedata AccessorWin32 object
//           (offset differs per game). We hook the instructions that store the
//           pointer and rebuild the list as [manual][{NEW_START..hi}][reserved].
//   BACKING When MaxSlots > 300 the accessor's slot array is redirected to a
//           larger zeroed buffer and the hardcoded icon/page-loop bounds are
//           raised (per-game sites).

#include "core.h"
#include "games.h"

// Supported games - first match on the host exe name wins.
const GameModule* const kGames[] = { &game_sora1, &game_sora2 };

static void siblingPath(wchar_t* out, size_t n, HMODULE self, const wchar_t* ext) {
    GetModuleFileNameW(self, out, (DWORD)n);
    wchar_t* dot = wcsrchr(out, L'.');
    if (dot) *dot = 0;
    wcscat_s(out, n, ext);
}

static DWORD WINAPI init(LPVOID param) {
    HMODULE self = (HMODULE)param;
    wchar_t logp[MAX_PATH], inip[MAX_PATH];
    siblingPath(logp, MAX_PATH, self, L".log");
    siblingPath(inip, MAX_PATH, self, L".ini");
    coreOpenLog(logp);

    L("SoraSaveSlots v6.0.0\n");

    // ---- detect host exe --------------------------------------------------
    wchar_t exeName[MAX_PATH];
    GetModuleFileNameW(nullptr, exeName, MAX_PATH);
    const wchar_t* leaf = wcsrchr(exeName, L'\\');
    leaf = leaf ? leaf + 1 : exeName;

    const GameModule* game = nullptr;
    for (size_t i = 0; i < sizeof(kGames) / sizeof(kGames[0]); ++i)
        if (_wcsicmp(leaf, kGames[i]->exe) == 0) { game = kGames[i]; break; }
    if (!game) {
        L("WARNING: unrecognized host exe \"%ls\". Not patching.\n", leaf);
        coreCloseLog();
        return 0;
    }
    L("game detected: %s (%ls)\n", game->label, leaf);

    // ---- configuration ----------------------------------------------------
    // static: g_ctx (in core.cpp) keeps a pointer to this beyond the lifetime
    // of the init thread - FixRanges/FixBacking read it on every dialog open.
    static GameContext ctx{};
    ctx.self  = self;
    ctx.label = game->label;
    ctx.base  = (uintptr_t)GetModuleHandleW(nullptr);

    int slots = (int)GetPrivateProfileIntW(L"SoraSaveSlots", L"MaxSlots", DEFAULT_SLOTS, inip);
    if (slots > FILENAME_LIMIT) {
        L("WARNING: MaxSlots=%d exceeds the save%%03d filename limit; clamping to %d.\n",
          slots, FILENAME_LIMIT);
        slots = FILENAME_LIMIT;
    }
    if (slots <= NEW_START) {
        L("MaxSlots=%d adds nothing (new slots start at %d).\nNot patching.\n", slots, NEW_START);
        coreCloseLog();
        return 0;
    }
    ctx.slots = slots;
    ctx.dynamicWindow = GetPrivateProfileIntW(L"SoraSaveSlots", L"DynamicWindow", 1, inip) != 0;
    L("MaxSlots = %d, DynamicWindow = %d, base = 0x%llx\n",
      slots, ctx.dynamicWindow ? 1 : 0, (unsigned long long)ctx.base);

    // Autosave history ring (sora_2nd only; ignored by games without the site).
    int autoSlots = (int)GetPrivateProfileIntW(L"SoraSaveSlots", L"AutosaveSlots",
                                               DEFAULT_AUTOSAVE_SLOTS, inip);
    if (autoSlots < 0) autoSlots = 0;
    if (autoSlots > MAX_AUTOSAVE_SLOTS) autoSlots = MAX_AUTOSAVE_SLOTS;
    if (autoSlots > STOCK_BACKING - (int)NEW_START) autoSlots = STOCK_BACKING - (int)NEW_START;
    ctx.autoHistCount = autoSlots;
    ctx.autoHistStart = (unsigned)(STOCK_BACKING - autoSlots);
    L("AutosaveSlots = %d (ring %u..%u)\n", autoSlots, ctx.autoHistStart,
      autoSlots > 0 ? ctx.autoHistStart + (unsigned)autoSlots - 1 : 0);

    installGame(*game, ctx);
    // The log is deliberately kept open: FixRanges/FixBacking/AutosaveHistoryTick
    // run long after init and their diagnostics are the point of the file.
    return 0;
}

BOOL APIENTRY DllMain(HMODULE h, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(h);
        CreateThread(nullptr, 0, init, h, 0, nullptr);
    }
    return TRUE;
}
