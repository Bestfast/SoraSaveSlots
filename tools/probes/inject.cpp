// Waits for sora_1st.exe and injects soraprobe.dll. Nothing is written to the game folder.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <tlhelp32.h>
#include <stdio.h>

static DWORD findPid(const wchar_t* name) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;
    PROCESSENTRY32W pe{ sizeof(pe) };
    DWORD pid = 0;
    if (Process32FirstW(snap, &pe)) {
        do {
            if (_wcsicmp(pe.szExeFile, name) == 0) { pid = pe.th32ProcessID; break; }
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return pid;
}

int wmain(int argc, wchar_t** argv) {
    if (argc < 2) { wprintf(L"usage: inject.exe <full path to soraprobe.dll>\n"); return 1; }
    const wchar_t* dll = argv[1];
    if (GetFileAttributesW(dll) == INVALID_FILE_ATTRIBUTES) { wprintf(L"dll not found: %s\n", dll); return 1; }

    wprintf(L"waiting for sora_1st.exe ... (launch the game now)\n");
    DWORD pid = 0;
    for (int i = 0; i < 3000 && !pid; ++i) { pid = findPid(L"sora_1st.exe"); if (!pid) Sleep(200); }
    if (!pid) { wprintf(L"timed out\n"); return 1; }
    wprintf(L"found pid %lu, waiting 8s for it to settle...\n", pid);
    Sleep(8000);

    HANDLE p = OpenProcess(PROCESS_CREATE_THREAD | PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_QUERY_INFORMATION, FALSE, pid);
    if (!p) { wprintf(L"OpenProcess failed: %lu\n", GetLastError()); return 1; }

    SIZE_T n = (wcslen(dll) + 1) * sizeof(wchar_t);
    void* mem = VirtualAllocEx(p, nullptr, n, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!mem) { wprintf(L"alloc failed\n"); return 1; }
    if (!WriteProcessMemory(p, mem, dll, n, nullptr)) { wprintf(L"write failed\n"); return 1; }

    auto ll = (LPTHREAD_START_ROUTINE)GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "LoadLibraryW");
    HANDLE t = CreateRemoteThread(p, nullptr, 0, ll, mem, 0, nullptr);
    if (!t) { wprintf(L"CreateRemoteThread failed: %lu\n", GetLastError()); return 1; }
    WaitForSingleObject(t, 10000);
    DWORD code = 0; GetExitCodeThread(t, &code);
    wprintf(L"injected (remote LoadLibraryW returned 0x%lx)\n", code);
    if (code == 0) wprintf(L"WARNING: LoadLibraryW returned NULL - dll failed to load\n");
    CloseHandle(t); CloseHandle(p);
    return 0;
}
