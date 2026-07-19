// probe2 - locate the live fdk::savedata::AccessorWin32 object and find which
// field holds the 175 slot cap, by scanning the object for that value.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <share.h>

#define VTABLE_RVA   0x9d8750          // fdk::savedata::AccessorWin32::vftable
#define OBJ_SCAN_LEN 0x9000            // bytes of the object to scan
#define MAXOBJ       64
#define LOGPATH L"C:\\Users\\SALVAT~1\\AppData\\Local\\Temp\\claude\\C--Users-Salvatore-Documents-Claudio\\60cdb7b6-2759-43d4-90ef-25c9f639e220\\scratchpad\\probe\\probe2.log"

static FILE* g_log;
static uintptr_t g_base;

static void L(const char* f, ...) {
    if (!g_log) return;
    va_list ap; va_start(ap, f); vfprintf(g_log, f, ap); va_end(ap); fflush(g_log);
}

static bool readable(void* p, SIZE_T n) {
    MEMORY_BASIC_INFORMATION mbi;
    if (!VirtualQuery(p, &mbi, sizeof(mbi))) return false;
    if (mbi.State != MEM_COMMIT) return false;
    if (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return false;
    return (uintptr_t)p + n <= (uintptr_t)mbi.BaseAddress + mbi.RegionSize;
}

// plain C, SEH-safe (no C++ object unwinding in this frame)
static int scanRegion(uintptr_t* p, SIZE_T n, uintptr_t want, uintptr_t* out, int have) {
    __try {
        for (SIZE_T i = 0; i < n && have < MAXOBJ; ++i)
            if (p[i] == want) out[have++] = (uintptr_t)&p[i];
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return have;
}

static int findObjects(uintptr_t* out) {
    uintptr_t want = g_base + VTABLE_RVA;
    SYSTEM_INFO si; GetSystemInfo(&si);
    uintptr_t addr = (uintptr_t)si.lpMinimumApplicationAddress;
    uintptr_t maxa = (uintptr_t)si.lpMaximumApplicationAddress;
    MEMORY_BASIC_INFORMATION mbi;
    int n = 0;
    while (addr < maxa && n < MAXOBJ && VirtualQuery((void*)addr, &mbi, sizeof(mbi))) {
        DWORD ok = PAGE_READWRITE | PAGE_READONLY | PAGE_WRITECOPY | PAGE_EXECUTE_READWRITE;
        if (mbi.State == MEM_COMMIT && !(mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) && (mbi.Protect & ok))
            n = scanRegion((uintptr_t*)mbi.BaseAddress, mbi.RegionSize / sizeof(uintptr_t), want, out, n);
        addr = (uintptr_t)mbi.BaseAddress + mbi.RegionSize;
    }
    return n;
}

static DWORD WINAPI worker(LPVOID) {
    g_base = (uintptr_t)GetModuleHandleW(nullptr);
    g_log  = _wfsopen(LOGPATH, L"w", _SH_DENYNO);
    L("probe2 attached; base=0x%llx vtable=0x%llx\n",
      (unsigned long long)g_base, (unsigned long long)(g_base + VTABLE_RVA));

    uintptr_t objs[MAXOBJ];
    for (int pass = 0; pass < 900; ++pass) {
        Sleep(2000);
        int n = findObjects(objs);
        if (n == 0) { if (pass % 15 == 0) L("[pass %d] no object yet\n", pass); continue; }

        for (int k = 0; k < n; ++k) {
            uintptr_t o = objs[k];
            if (!readable((void*)o, OBJ_SCAN_LEN)) continue;
            auto dw = (unsigned int*)o;
            L("\n[pass %d] obj=0x%llx  count[+0x23a8]=%u  max[+0x37dc]=%u\n",
              pass, (unsigned long long)o,
              *(unsigned*)(o + 0x23a8), *(unsigned*)(o + 0x37dc));
            for (unsigned off = 0; off < OBJ_SCAN_LEN / 4; ++off) {
                unsigned v = dw[off];
                if (v == 175 || v == 174 || v == 170 || v == 300 || v == 35)
                    L("    +0x%-6x = %u\n", off * 4, v);
            }
        }
    }
    return 0;
}

BOOL APIENTRY DllMain(HMODULE h, DWORD r, LPVOID) {
    if (r == DLL_PROCESS_ATTACH) { DisableThreadLibraryCalls(h); CreateThread(0, 0, worker, 0, 0, 0); }
    return TRUE;
}
