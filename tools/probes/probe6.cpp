// probe6 - whole-process value scanner. Finds every dword == 175 (and 170/34/35)
// in committed writable memory, and flags the ones that sit inside a struct that
// ALSO contains a nearby 300 or 170 (menu/save state signature). Runs on a timer,
// so the user just needs the save menu open during a scan pass. No restart needed.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <share.h>

#define LOGPATH L"C:\\Users\\SALVAT~1\\AppData\\Local\\Temp\\claude\\C--Users-Salvatore-Documents-Claudio\\60cdb7b6-2759-43d4-90ef-25c9f639e220\\scratchpad\\probe\\probe6.log"

static FILE* g_log;
static uintptr_t g_base;

static void L(const char* f, ...) {
    if (!g_log) return;
    va_list ap; va_start(ap, f); vfprintf(g_log, f, ap); va_end(ap);
    fflush(g_log);
}

// Does [center-window, center+window] contain value v?
static bool nearbyHas(unsigned* base, size_t nDwords, size_t idx, unsigned v, int window) {
    long lo = (long)idx - window, hi = (long)idx + window;
    if (lo < 0) lo = 0;
    if ((size_t)hi >= nDwords) hi = (long)nDwords - 1;
    for (long i = lo; i <= hi; ++i) if (base[i] == v) return true;
    return false;
}

static DWORD WINAPI worker(LPVOID) {
    g_base = (uintptr_t)GetModuleHandleW(nullptr);
    g_log = _wfsopen(LOGPATH, L"w", _SH_DENYNO);
    L("probe6 attached; base=0x%llx\n", (unsigned long long)g_base);
    L("scanning for 175 near {300|170|34|35}; open the SAVE MENU during a pass\n");

    for (int pass = 0; pass < 300; ++pass) {
        Sleep(3000);
        SYSTEM_INFO si; GetSystemInfo(&si);
        uintptr_t addr = (uintptr_t)si.lpMinimumApplicationAddress;
        uintptr_t maxa = (uintptr_t)si.lpMaximumApplicationAddress;
        MEMORY_BASIC_INFORMATION mbi;
        int strong = 0, total = 0;
        L("\n===== pass %d =====\n", pass);
        while (addr < maxa && VirtualQuery((void*)addr, &mbi, sizeof(mbi))) {
            uintptr_t next = (uintptr_t)mbi.BaseAddress + mbi.RegionSize;
            DWORD w = PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READWRITE;
            // only private (heap) writable committed memory, skip images/mapped
            if (mbi.State == MEM_COMMIT && mbi.Type == MEM_PRIVATE &&
                !(mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) && (mbi.Protect & w)) {
                unsigned* p = (unsigned*)mbi.BaseAddress;
                size_t n = mbi.RegionSize / 4;
                __try {
                    for (size_t i = 0; i < n; ++i) {
                        if (p[i] != 175) continue;
                        total++;
                        bool s300 = nearbyHas(p, n, i, 300, 0x100);
                        bool s170 = nearbyHas(p, n, i, 170, 0x40);
                        bool s34 = nearbyHas(p, n, i, 34, 0x40) || nearbyHas(p, n, i, 35, 0x40);
                        if (s300 || s170 || s34) {
                            strong++;
                            if (strong <= 60)
                                L("  0x%llx  =175  near:%s%s%s\n",
                                  (unsigned long long)(uintptr_t)&p[i],
                                  s300 ? " 300" : "", s170 ? " 170" : "", s34 ? " 34/35" : "");
                        }
                    }
                } __except (EXCEPTION_EXECUTE_HANDLER) {}
            }
            addr = next;
        }
        L("  pass %d: total 175-hits=%d  strong(near 300/170/34)=%d\n", pass, total, strong);
    }
    return 0;
}

BOOL APIENTRY DllMain(HMODULE h, DWORD r, LPVOID) {
    if (r == DLL_PROCESS_ATTACH) { DisableThreadLibraryCalls(h); CreateThread(0, 0, worker, 0, 0, 0); }
    return TRUE;
}
