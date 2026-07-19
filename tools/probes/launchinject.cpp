// Launches sora_1st.exe and injects a DLL as early as possible (before the
// save system is built), so the ctor-tail hook can be tested without an ASI
// loader in the game folder. Handles the Steam relaunch dance: if the first
// process exits quickly (SteamAPI restart), waits for the replacement
// sora_1st.exe and injects that one immediately.
//
//   launchinject.exe <full path to dll> [game exe path]
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <tlhelp32.h>
#include <stdio.h>

static const wchar_t* kDefaultExe =
    L"Z:\\SteamLibrary\\steamapps\\common\\Trails in the Sky 1st Chapter\\sora_1st.exe";

static DWORD findPid(const wchar_t* name, DWORD skip) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;
    PROCESSENTRY32W pe{ sizeof(pe) };
    DWORD pid = 0;
    if (Process32FirstW(snap, &pe)) {
        do {
            if (_wcsicmp(pe.szExeFile, name) == 0 && pe.th32ProcessID != skip) {
                pid = pe.th32ProcessID; break;
            }
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return pid;
}

static bool inject(HANDLE p, const wchar_t* dll) {
    SIZE_T n = (wcslen(dll) + 1) * sizeof(wchar_t);
    void* mem = VirtualAllocEx(p, nullptr, n, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!mem || !WriteProcessMemory(p, mem, dll, n, nullptr)) { wprintf(L"write failed\n"); return false; }
    auto ll = (LPTHREAD_START_ROUTINE)GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "LoadLibraryW");
    HANDLE t = CreateRemoteThread(p, nullptr, 0, ll, mem, 0, nullptr);
    if (!t) { wprintf(L"CreateRemoteThread failed: %lu\n", GetLastError()); return false; }
    WaitForSingleObject(t, 10000);
    DWORD code = 0; GetExitCodeThread(t, &code);
    CloseHandle(t);
    wprintf(L"remote LoadLibraryW returned 0x%lx\n", code);
    return code != 0;
}

int wmain(int argc, wchar_t** argv) {
    if (argc < 2) { wprintf(L"usage: launchinject.exe <dll> [game exe]\n"); return 1; }
    const wchar_t* dll = argv[1];
    const wchar_t* exe = argc > 2 ? argv[2] : kDefaultExe;
    if (GetFileAttributesW(dll) == INVALID_FILE_ATTRIBUTES) { wprintf(L"dll not found: %s\n", dll); return 1; }

    wchar_t dir[MAX_PATH]; wcscpy_s(dir, exe);
    wchar_t* sl = wcsrchr(dir, L'\\'); if (sl) *sl = 0;

    STARTUPINFOW si{ sizeof(si) }; PROCESS_INFORMATION pi{};
    wchar_t cmd[MAX_PATH]; swprintf_s(cmd, L"\"%s\"", exe);
    if (!CreateProcessW(exe, cmd, nullptr, nullptr, FALSE, 0, nullptr, dir, &si, &pi)) {
        wprintf(L"CreateProcess failed: %lu\n", GetLastError()); return 1;
    }
    wprintf(L"launched pid %lu\n", pi.dwProcessId);
    Sleep(400);   // let the loader initialise

    DWORD ec = STILL_ACTIVE;
    GetExitCodeProcess(pi.hProcess, &ec);
    if (ec == STILL_ACTIVE && inject(pi.hProcess, dll)) {
        // watch for a Steam relaunch: original dies within a few seconds
        for (int i = 0; i < 25; ++i) { Sleep(200); GetExitCodeProcess(pi.hProcess, &ec); if (ec != STILL_ACTIVE) break; }
        if (ec == STILL_ACTIVE) { wprintf(L"injected into original process; it is still running\n"); return 0; }
        wprintf(L"original exited (0x%lx) - Steam relaunch? waiting for new process\n", ec);
    } else if (ec != STILL_ACTIVE) {
        wprintf(L"original exited immediately (0x%lx) - waiting for Steam relaunch\n", ec);
    }
    CloseHandle(pi.hThread); CloseHandle(pi.hProcess);

    DWORD pid = 0;
    for (int i = 0; i < 150 && !pid; ++i) { Sleep(200); pid = findPid(L"sora_1st.exe", pi.dwProcessId); }
    if (!pid) { wprintf(L"no relaunched sora_1st.exe appeared\n"); return 1; }
    wprintf(L"relaunched pid %lu, injecting now\n", pid);
    Sleep(400);
    HANDLE p = OpenProcess(PROCESS_CREATE_THREAD | PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_QUERY_INFORMATION, FALSE, pid);
    if (!p) { wprintf(L"OpenProcess failed: %lu\n", GetLastError()); return 1; }
    bool ok = inject(p, dll);
    CloseHandle(p);
    return ok ? 0 : 1;
}
