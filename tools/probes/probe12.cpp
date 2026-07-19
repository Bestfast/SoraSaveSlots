// probe12 - self-contained: scan for the item-count field, then hardware-breakpoint it.
// 1) Scan private heap for dword==175 that has BOTH 300 and 170 within a tight window
//    (the save-state struct signature: display=175, capacity=300, manual-cap=170).
// 2) Rotate hardware read+write breakpoints (DR0-3) through the found addresses across
//    all threads, logging the instruction (RVA) that touches each. That instruction is
//    where 175 is computed/consumed -> the ASI patch target.
// Survives relaunch (finds fresh addresses itself). Just keep the save menu reachable
// and open/scroll it while probe12 rotates.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <tlhelp32.h>
#include <stdio.h>
#include <share.h>

#define LOG L"C:\\Users\\SALVAT~1\\AppData\\Local\\Temp\\claude\\C--Users-Salvatore-Documents-Claudio\\60cdb7b6-2759-43d4-90ef-25c9f639e220\\scratchpad\\probe\\probe12.log"
#define MAXC 64

static FILE* g_log;
static uintptr_t g_base, g_end;
static uintptr_t g_cand[MAXC];
static int g_ncand = 0;
static uintptr_t g_armed[4];
static volatile LONG g_hits = 0;

static void L(const char* f, ...) {
    if (!g_log) return;
    va_list ap; va_start(ap, f); vfprintf(g_log, f, ap); va_end(ap); fflush(g_log);
}
static const char* where(uintptr_t rip) {
    static char b[80];
    if (rip >= g_base && rip < g_end) sprintf(b, "sora_1st.exe+0x%llx", (unsigned long long)(rip - g_base));
    else sprintf(b, "extern:0x%llx", (unsigned long long)rip);
    return b;
}
static bool nearHas(unsigned* base, size_t n, size_t idx, unsigned v, int win) {
    long lo = (long)idx - win, hi = (long)idx + win;
    if (lo < 0) lo = 0; if ((size_t)hi >= n) hi = (long)n - 1;
    for (long i = lo; i <= hi; ++i) if (base[i] == v) return true;
    return false;
}

static void scan() {
    SYSTEM_INFO si; GetSystemInfo(&si);
    uintptr_t a = (uintptr_t)si.lpMinimumApplicationAddress, mx = (uintptr_t)si.lpMaximumApplicationAddress;
    MEMORY_BASIC_INFORMATION mbi;
    while (a < mx && g_ncand < MAXC && VirtualQuery((void*)a, &mbi, sizeof(mbi))) {
        uintptr_t next = (uintptr_t)mbi.BaseAddress + mbi.RegionSize;
        DWORD w = PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READWRITE;
        if (mbi.State == MEM_COMMIT && mbi.Type == MEM_PRIVATE &&
            !(mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) && (mbi.Protect & w)) {
            unsigned* p = (unsigned*)mbi.BaseAddress; size_t n = mbi.RegionSize / 4;
            __try {
                for (size_t i = 0; i < n && g_ncand < MAXC; ++i) {
                    if (p[i] != 175) continue;
                    // tight signature: 300 AND 170 both within 0x20 dwords
                    if (nearHas(p, n, i, 300, 0x100) && nearHas(p, n, i, 170, 0x80))
                        g_cand[g_ncand++] = (uintptr_t)&p[i];
                }
            } __except (EXCEPTION_EXECUTE_HANDLER) {}
        }
        a = next;
    }
}

static LONG CALLBACK veh(EXCEPTION_POINTERS* ep) {
    if (ep->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP) return EXCEPTION_CONTINUE_SEARCH;
    DWORD64 dr6 = ep->ContextRecord->Dr6;
    uintptr_t rip = (uintptr_t)ep->ContextRecord->Rip;
    bool inGame = (rip >= g_base && rip < g_end);   // ignore reads from our own injected DLLs
    for (int i = 0; i < 4; ++i)
        if ((dr6 & (1ull << i)) && g_armed[i] && inGame) {
            if (InterlockedIncrement(&g_hits) <= 60)
                L("HIT  addr=0x%llx  by %s\n", (unsigned long long)g_armed[i], where(rip));
        }
    ep->ContextRecord->Dr6 = 0;
    ep->ContextRecord->EFlags |= 0x10000;
    return EXCEPTION_CONTINUE_EXECUTION;
}

static void armThreads() {
    DWORD me = GetCurrentProcessId();
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) return;
    THREADENTRY32 te{ sizeof(te) };
    DWORD64 dr7 = 0;
    for (int i = 0; i < 4; ++i) if (g_armed[i]) {
        dr7 |= (1ull << (i * 2)) | (0b11ull << (16 + i * 4)) | (0b11ull << (18 + i * 4));
    }
    DWORD self = GetCurrentThreadId();
    if (Thread32First(snap, &te)) do {
        if (te.th32OwnerProcessID != me) continue;
        if (te.th32ThreadID == self) continue;   // never suspend our own worker (deadlock)
        HANDLE th = OpenThread(THREAD_GET_CONTEXT | THREAD_SET_CONTEXT | THREAD_SUSPEND_RESUME, FALSE, te.th32ThreadID);
        if (!th) continue;
        SuspendThread(th);
        CONTEXT c; c.ContextFlags = CONTEXT_DEBUG_REGISTERS;
        if (GetThreadContext(th, &c)) {
            c.Dr0 = g_armed[0]; c.Dr1 = g_armed[1]; c.Dr2 = g_armed[2]; c.Dr3 = g_armed[3];
            c.Dr7 = dr7; c.ContextFlags = CONTEXT_DEBUG_REGISTERS;
            SetThreadContext(th, &c);
        }
        ResumeThread(th); CloseHandle(th);
    } while (Thread32Next(snap, &te));
    CloseHandle(snap);
}

static DWORD WINAPI worker(LPVOID) {
    g_base = (uintptr_t)GetModuleHandleW(nullptr); g_end = g_base + 0xb67000;
    g_log = _wfsopen(LOG, L"w", _SH_DENYNO);
    L("probe12 attached; base=0x%llx\n", (unsigned long long)g_base);
    L("Open the save menu, THEN it scans (so the struct exists). Rotating HW BPs.\n");
    AddVectoredExceptionHandler(1, veh);

    // scan repeatedly until the save-state struct exists (menu open) - no timing dependency
    for (int tries = 0; g_ncand == 0 && tries < 120; ++tries) {
        Sleep(3000);
        g_ncand = 0;
        scan();
        if (tries % 5 == 0) L("[scan try %d] candidates=%d (open the save menu)\n", tries, g_ncand);
    }
    L("found %d candidates (175 near 300 AND 170):\n", g_ncand);
    for (int i = 0; i < g_ncand; ++i) L("   [%d] 0x%llx\n", i, (unsigned long long)g_cand[i]);
    if (g_ncand == 0) { L("no candidates after retries.\n"); return 0; }

    // rotate groups of 4 through the candidates; user scrolls/reopens menu during each window
    for (int pass = 0; pass < 300; ++pass) {
        int group = pass % ((g_ncand + 3) / 4);
        for (int i = 0; i < 4; ++i) {
            int idx = group * 4 + i;
            g_armed[i] = (idx < g_ncand) ? g_cand[idx] : 0;
        }
        L("\n[window %d] armed group %d: %llx %llx %llx %llx  (scroll/reopen the menu now)\n",
          pass, group,
          (unsigned long long)g_armed[0], (unsigned long long)g_armed[1],
          (unsigned long long)g_armed[2], (unsigned long long)g_armed[3]);
        for (int t = 0; t < 6; ++t) { armThreads(); Sleep(2500); }
    }
    return 0;
}

BOOL APIENTRY DllMain(HMODULE h, DWORD r, LPVOID) {
    if (r == DLL_PROCESS_ATTACH) { DisableThreadLibraryCalls(h); CreateThread(0, 0, worker, 0, 0, 0); }
    return TRUE;
}
