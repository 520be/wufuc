#include <stdint.h>
#include <Windows.h>
#include <Psapi.h>
#include <TlHelp32.h>
#include <tchar.h>
#include <sddl.h>
#include "service.h"
#include "util.h"
#include "patternfind.h"
#include "core.h"

DWORD WINAPI NewThreadProc(LPVOID lpParam) {
    SC_HANDLE hSCManager = OpenSCManager(NULL, SERVICES_ACTIVE_DATABASE, SC_MANAGER_CONNECT);

    TCHAR lpBinaryPathName[0x8000];
    get_svcpath(hSCManager, _T("wuauserv"), lpBinaryPathName, _countof(lpBinaryPathName));

    BOOL result = _tcsicmp(GetCommandLine(), lpBinaryPathName);
    CloseServiceHandle(hSCManager);

    if (result) {
        return 0;
    }

    SECURITY_ATTRIBUTES sa;
    sa.nLength = sizeof(sa);
    ConvertStringSecurityDescriptorToSecurityDescriptor(_T("D:PAI(A;;FA;;;BA)"), SDDL_REVISION_1, &(sa.lpSecurityDescriptor), NULL);
    sa.bInheritHandle = FALSE;

    HANDLE hEvent = CreateEvent(&sa, TRUE, FALSE, _T("Global\\wufuc_UnloadEvent"));

    if (!hEvent) {
        return 0;
    }

    DWORD dwProcessId = GetCurrentProcessId();
    DWORD dwThreadId = GetCurrentThreadId();
    HANDLE lphThreads[0x1000];
    SIZE_T cb;

    SuspendProcessThreads(dwProcessId, dwThreadId, lphThreads, _countof(lphThreads), &cb);

    HMODULE hm = GetModuleHandle(NULL);
    DETOUR_IAT(hm, LoadLibraryExA);
    DETOUR_IAT(hm, LoadLibraryExW);

    TCHAR lpServiceDll[MAX_PATH + 1];
    get_svcdll(_T("wuauserv"), lpServiceDll, _countof(lpServiceDll));

    HMODULE hwu = GetModuleHandle(lpServiceDll);
    if (hwu && PatchWUAgentHMODULE(hwu)) {
        _tdbgprintf(_T("Patched previously loaded Windows Update module!"));
    }
    ResumeAndCloseThreads(lphThreads, cb);

    WaitForSingleObject(hEvent, INFINITE);

    _tdbgprintf(_T("Unload event was set."));

    SuspendProcessThreads(dwProcessId, dwThreadId, lphThreads, _countof(lphThreads), &cb);
    RESTORE_IAT(hm, LoadLibraryExA);
    RESTORE_IAT(hm, LoadLibraryExW);
    ResumeAndCloseThreads(lphThreads, cb);

    CloseHandle(hEvent);
    _tdbgprintf(_T("See ya!"));
    FreeLibraryAndExitThread(HINST_THISCOMPONENT, 0);
}

BOOL PatchWUAgentHMODULE(HMODULE hModule) {
    LPSTR pattern;
    SIZE_T offset00, offset01;
    if (Is64BitWindows()) {
        pattern = "FFF3 4883EC?? 33DB 391D???????? 7508 8B05????????";
        offset00 = 10;
        offset01 = 18;
    } else if (WindowsVersionCompare(VER_EQUAL, 6, 1, 0, 0, VER_MAJORVERSION | VER_MINORVERSION)) {
        pattern = "833D????????00 743E E8???????? A3????????";
        offset00 = 2;
        offset01 = 15;
    } else if (WindowsVersionCompare(VER_EQUAL, 6, 3, 0, 0, VER_MAJORVERSION | VER_MINORVERSION)) {
        pattern = "8BFF 51 833D????????00 7507 A1????????";
        offset00 = 5;
        offset01 = 13;
    } else {
        return FALSE;
    }

    MODULEINFO modinfo;
    GetModuleInformation(GetCurrentProcess(), hModule, &modinfo, sizeof(MODULEINFO));

    SIZE_T rva = patternfind(modinfo.lpBaseOfDll, modinfo.SizeOfImage, 0, pattern);
    if (rva == -1) {
        _tdbgprintf(_T("No pattern match!"));
        return FALSE;
    }
    uintptr_t baseAddress = (uintptr_t)modinfo.lpBaseOfDll;
    uintptr_t fpIsDeviceServiceable = baseAddress + rva;
    _tdbgprintf(_T("Found address of IsDeviceServiceable. (%p)"), fpIsDeviceServiceable);
    BOOL result = FALSE;
    LPBOOL lpbFirstRun, lpbIsCPUSupportedResult;
    if (Is64BitWindows()) {
        lpbFirstRun = (LPBOOL)(fpIsDeviceServiceable + offset00 + sizeof(uint32_t) + *(uint32_t *)(fpIsDeviceServiceable + offset00));
        lpbIsCPUSupportedResult = (LPBOOL)(fpIsDeviceServiceable + offset01 + sizeof(uint32_t) + *(uint32_t *)(fpIsDeviceServiceable + offset01));
    } else {
        lpbFirstRun = (LPBOOL)(*(uintptr_t *)(fpIsDeviceServiceable + offset00));
        lpbIsCPUSupportedResult = (LPBOOL)(*(uintptr_t *)(fpIsDeviceServiceable + offset01));
    }
    
    if (*lpbFirstRun) {
        *lpbFirstRun = FALSE;
        _tdbgprintf(_T("Changed first run to FALSE. (%p=%08x)"), lpbFirstRun, *lpbFirstRun);
        result = TRUE;
    }
    if (!*lpbIsCPUSupportedResult) {
        *lpbIsCPUSupportedResult = TRUE;
        _tdbgprintf(_T("Changed cached result to TRUE. (%p=%08x)."), 
            lpbIsCPUSupportedResult, *lpbIsCPUSupportedResult);
        result = TRUE;
    }
    return result;
}

HMODULE WINAPI _LoadLibraryExA(
    _In_       LPCSTR  lpFileName,
    _Reserved_ HANDLE  hFile,
    _In_       DWORD   dwFlags
) {
    HMODULE result = LoadLibraryExA(lpFileName, hFile, dwFlags);
    if (result) {
        _dbgprintf("Loaded %s.", lpFileName);
        CHAR path[MAX_PATH + 1];
        if (!get_svcdllA("wuauserv", path, _countof(path))) {
            return result;
        }

        if (!_stricmp(lpFileName, path) && PatchWUAgentHMODULE(result)) {
            _dbgprintf("Patched Windows Update module!");
        }
    }
    return result;
}

HMODULE WINAPI _LoadLibraryExW(
    _In_       LPCWSTR lpFileName,
    _Reserved_ HANDLE  hFile,
    _In_       DWORD   dwFlags
) {
    HMODULE result = LoadLibraryExW(lpFileName, hFile, dwFlags);
    if (result) {
        _wdbgprintf(L"Loaded library: %s.", lpFileName);
        WCHAR path[MAX_PATH + 1];
        if (!get_svcdllW(L"wuauserv", path, _countof(path))) {
            return result;
        }

        if (!_wcsicmp(lpFileName, path) && PatchWUAgentHMODULE(result)) {
            _wdbgprintf(L"Patched Windows Update module!");
        }
    }
    return result;
};
