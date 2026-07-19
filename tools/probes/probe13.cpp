// probe13 - breakpoint the KNOWN cursor address (found via Cheat Engine) and
// log the game instructions that read/write it. Also dumps the struct around it
// (the scroll limit is often stored a few fields away from the cursor).
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <tlhelp32.h>
#include <stdio.h>
#include <share.h>

#define TARGET   0x1D1D2D841B4ull        // cursor address (this session)
#define LOG      L"C:\\Users\\SALVAT~1\\AppData\\Local\\Temp\\claude\\C--Users-Salvatore-Documents-Claudio\\60cdb7b6-2759-43d4-90ef-25c9f639e220\\scratchpad\\probe\\probe13.log"

static FILE* g_log;
static uintptr_t g_base, g_end;
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

static LONG CALLBACK veh(EXCEPTION_POINTERS* ep) {
    if (ep->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP) return EXCEPTION_CONTINUE_SEARCH;
    if (ep->ContextRecord->Dr6 & 1) {
        uintptr_t rip = (uintptr_t)ep->ContextRecord->Rip;
        bool inGame = (rip >= g_base && rip < g_end);
        if (inGame && InterlockedIncrement(&g_hits) <= 80)
            L("HIT  by %s\n", where(rip));
    }
    ep->ContextRecord->Dr6 = 0;
    ep->ContextRecord->EFlags |= 0x10000;
    return EXCEPTION_CONTINUE_EXECUTION;
}

static void armAll() {
    DWORD me = GetCurrentProcessId(), self = GetCurrentThreadId();
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) return;
    THREADENTRY32 te{ sizeof(te) };
    DWORD64 dr7 = 1ull | (0b11ull << 16) | (0b11ull << 18); // L0 on, RW=readwrite, LEN=4
    if (Thread32First(snap, &te)) do {
        if (te.th32OwnerProcessID != me || te.th32ThreadID == self) continue;
        HANDLE th = OpenThread(THREAD_GET_CONTEXT | THREAD_SET_CONTEXT | THREAD_SUSPEND_RESUME, FALSE, te.th32ThreadID);
        if (!th) continue;
        SuspendThread(th);
        CONTEXT c; c.ContextFlags = CONTEXT_DEBUG_REGISTERS;
        if (GetThreadContext(th, &c)) { c.Dr0 = TARGET; c.Dr7 = dr7; c.ContextFlags = CONTEXT_DEBUG_REGISTERS; SetThreadContext(th, &c); }
        ResumeThread(th); CloseHandle(th);
    } while (Thread32Next(snap, &te));
    CloseHandle(snap);
}

static DWORD WINAPI worker(LPVOID) {
    g_base = (uintptr_t)GetModuleHandleW(nullptr); g_end = g_base + 0xb67000;
    g_log = _wfsopen(LOG, L"w", _SH_DENYNO);
    L("probe13 attached; base=0x%llx  target(cursor)=0x%llx\n", (unsigned long long)g_base, (unsigned long long)TARGET);

    // dump the struct around the cursor: 0x80 bytes before .. 0x80 after, as dwords
    unsigned* p = (unsigned*)(TARGET - 0x80);
    L("--- struct dump (cursor at offset +0x80) ---\n");
    for (int off = -0x80; off < 0x84; off += 4) {
        unsigned v = *(unsigned*)(TARGET + off);
        L("  cursor%+4d = %-10u (0x%x)%s\n", off, v, v, off == 0 ? "  <== CURSOR" : "");
    }
    L("--- watching (scroll the menu; only in-game readers logged) ---\n");

    AddVectoredExceptionHandler(1, veh);
    for (int pass = 0; pass < 600; ++pass) { armAll(); Sleep(2000); if (pass % 15 == 0) L("[t=%ds] hits=%ld\n", pass*2, g_hits); }
    return 0;
}

BOOL APIENTRY DllMain(HMODULE h, DWORD r, LPVOID) {
    if (r == DLL_PROCESS_ATTACH) { DisableThreadLibraryCalls(h); CreateThread(0, 0, worker, 0, 0, 0); }
    return TRUE;
}
