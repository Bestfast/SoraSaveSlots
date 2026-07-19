// soraprobe - temporary instrumentation for sora_1st.exe
// IAT-hooks file APIs and logs every savedata path the game touches,
// together with the caller's RVA, so we can find the enumeration loop.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <wchar.h>
#include <wctype.h>
#include <intrin.h>

#define DEFAULT_LOG L"C:\\Users\\SALVAT~1\\AppData\\Local\\Temp\\claude\\C--Users-Salvatore-Documents-Claudio\\60cdb7b6-2759-43d4-90ef-25c9f639e220\\scratchpad\\probe\\soraprobe.log"

static FILE*             g_log = nullptr;
static CRITICAL_SECTION  g_cs;
static HMODULE           g_exe = nullptr;
static uintptr_t         g_base = 0;
static int               g_maxIdx = -1;

static void logf(const char* fmt, ...) {
    if (!g_log) return;
    EnterCriticalSection(&g_cs);
    va_list ap; va_start(ap, fmt);
    vfprintf(g_log, fmt, ap);
    va_end(ap);
    fflush(g_log);
    LeaveCriticalSection(&g_cs);
}

// Log only paths we care about, and track the highest saveNNN index seen.
static void note(const char* api, const wchar_t* path, void* ret) {
    if (!path) return;
    if (!wcsstr(path, L"save") && !wcsstr(path, L"sdmem")) return;

    uintptr_t rva = (uintptr_t)ret - g_base;

    const wchar_t* p = wcsstr(path, L"save");
    int idx = -1;
    if (p && wcslen(p) >= 7) {
        // "saveNNN"
        if (iswdigit(p[4]) && iswdigit(p[5]) && iswdigit(p[6]))
            idx = (p[4]-L'0')*100 + (p[5]-L'0')*10 + (p[6]-L'0');
    }
    if (idx > g_maxIdx) {
        g_maxIdx = idx;
        logf("[MAX] new highest slot index = %d\n", idx);
    }
    logf("%-22s ret=+0x%llx  idx=%4d  %ls\n", api, (unsigned long long)rva, idx, path);
}

// ---- originals
static HANDLE (WINAPI *o_CreateFileW)(LPCWSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
static DWORD  (WINAPI *o_GetFileAttributesW)(LPCWSTR);
static BOOL   (WINAPI *o_GetFileAttributesExW)(LPCWSTR, GET_FILEEX_INFO_LEVELS, LPVOID);
static HANDLE (WINAPI *o_FindFirstFileW)(LPCWSTR, LPWIN32_FIND_DATAW);
static HANDLE (WINAPI *o_FindFirstFileExW)(LPCWSTR, FINDEX_INFO_LEVELS, LPVOID, FINDEX_SEARCH_OPS, LPVOID, DWORD);
static BOOL   (WINAPI *o_CreateDirectoryW)(LPCWSTR, LPSECURITY_ATTRIBUTES);

static HANDLE WINAPI h_CreateFileW(LPCWSTR n, DWORD a, DWORD s, LPSECURITY_ATTRIBUTES sa, DWORD c, DWORD f, HANDLE t) {
    note("CreateFileW", n, _ReturnAddress());
    return o_CreateFileW(n, a, s, sa, c, f, t);
}
static DWORD WINAPI h_GetFileAttributesW(LPCWSTR n) {
    note("GetFileAttributesW", n, _ReturnAddress());
    return o_GetFileAttributesW(n);
}
static BOOL WINAPI h_GetFileAttributesExW(LPCWSTR n, GET_FILEEX_INFO_LEVELS l, LPVOID p) {
    note("GetFileAttributesExW", n, _ReturnAddress());
    return o_GetFileAttributesExW(n, l, p);
}
static HANDLE WINAPI h_FindFirstFileW(LPCWSTR n, LPWIN32_FIND_DATAW d) {
    note("FindFirstFileW", n, _ReturnAddress());
    return o_FindFirstFileW(n, d);
}
static HANDLE WINAPI h_FindFirstFileExW(LPCWSTR n, FINDEX_INFO_LEVELS l, LPVOID d, FINDEX_SEARCH_OPS o, LPVOID f, DWORD ff) {
    note("FindFirstFileExW", n, _ReturnAddress());
    return o_FindFirstFileExW(n, l, d, o, f, ff);
}
static BOOL WINAPI h_CreateDirectoryW(LPCWSTR n, LPSECURITY_ATTRIBUTES sa) {
    note("CreateDirectoryW", n, _ReturnAddress());
    return o_CreateDirectoryW(n, sa);
}

// ---- IAT patching over the main module
static bool patchIAT(const char* dllWanted, const char* fnWanted, void* hook, void** orig) {
    auto dos = (PIMAGE_DOS_HEADER)g_exe;
    auto nt  = (PIMAGE_NT_HEADERS)((BYTE*)g_exe + dos->e_lfanew);
    auto dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress) return false;
    auto imp = (PIMAGE_IMPORT_DESCRIPTOR)((BYTE*)g_exe + dir.VirtualAddress);

    for (; imp->Name; ++imp) {
        const char* dll = (const char*)((BYTE*)g_exe + imp->Name);
        if (_stricmp(dll, dllWanted) != 0) continue;
        auto oft = (PIMAGE_THUNK_DATA)((BYTE*)g_exe + (imp->OriginalFirstThunk ? imp->OriginalFirstThunk : imp->FirstThunk));
        auto ft  = (PIMAGE_THUNK_DATA)((BYTE*)g_exe + imp->FirstThunk);
        for (; oft->u1.AddressOfData; ++oft, ++ft) {
            if (IMAGE_SNAP_BY_ORDINAL(oft->u1.Ordinal)) continue;
            auto ibn = (PIMAGE_IMPORT_BY_NAME)((BYTE*)g_exe + oft->u1.AddressOfData);
            if (strcmp((const char*)ibn->Name, fnWanted) != 0) continue;
            DWORD old;
            if (!VirtualProtect(&ft->u1.Function, sizeof(void*), PAGE_READWRITE, &old)) return false;
            *orig = (void*)ft->u1.Function;
            ft->u1.Function = (ULONGLONG)hook;
            VirtualProtect(&ft->u1.Function, sizeof(void*), old, &old);
            return true;
        }
    }
    return false;
}

static DWORD WINAPI init(LPVOID) {
    g_exe  = GetModuleHandleW(nullptr);
    g_base = (uintptr_t)g_exe;

    wchar_t logpath[MAX_PATH] = { 0 };
    if (GetEnvironmentVariableW(L"SORAPROBE_LOG", logpath, MAX_PATH) == 0 || !logpath[0])
        wcscpy_s(logpath, DEFAULT_LOG);
    // share the handle so the log can be read while the game is still running
    g_log = _wfsopen(logpath, L"w", _SH_DENYNO);

    logf("=== soraprobe attached ===\nexe base = 0x%llx\n", (unsigned long long)g_base);

#define HOOK(f) do { if (patchIAT("KERNEL32.dll", #f, (void*)h_##f, (void**)&o_##f)) \
                        logf("hooked %s\n", #f); else logf("FAILED to hook %s\n", #f); } while(0)
    HOOK(CreateFileW);
    HOOK(GetFileAttributesW);
    HOOK(GetFileAttributesExW);
    HOOK(FindFirstFileW);
    HOOK(FindFirstFileExW);
    HOOK(CreateDirectoryW);
#undef HOOK
    logf("--- ready; open the save menu now ---\n");
    return 0;
}

BOOL APIENTRY DllMain(HMODULE h, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(h);
        InitializeCriticalSection(&g_cs);
        CreateThread(nullptr, 0, init, nullptr, 0, nullptr);
    }
    return TRUE;
}
