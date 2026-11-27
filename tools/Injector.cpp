#include <windows.h>
#include <psapi.h>

#include <cstdio>
#include <string>
#include <vector>

namespace
{
    constexpr DWORD kAccess =
        PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION |
        PROCESS_VM_OPERATION | PROCESS_VM_READ | PROCESS_VM_WRITE;

    HANDLE OpenProcessWithRetry(DWORD processId, int attempts)
    {
        for (int i = 0; i < attempts; ++i)
        {
            HANDLE handle = OpenProcess(kAccess, FALSE, processId);
            if (handle) return handle;
            Sleep(100);
        }
        return nullptr;
    }

    void PrintError(const wchar_t* action, DWORD error)
    {
        wprintf(L"[injector] %s failed (Win32 error %lu)\n", action, error);
    }

    bool ModuleLoaded(HANDLE process, const wchar_t* moduleName)
    {
        std::vector<HMODULE> modules(1024);
        DWORD needed = 0;
        if (!EnumProcessModulesEx(process, modules.data(),
                                  static_cast<DWORD>(modules.size() * sizeof(HMODULE)),
                                  &needed, LIST_MODULES_ALL))
        {
            return false;
        }

        const DWORD count = needed / sizeof(HMODULE);
        if (count > modules.size()) return false;

        wchar_t name[MAX_PATH] = {};
        for (DWORD i = 0; i < count; ++i)
        {
            if (GetModuleBaseNameW(process, modules[i], name, MAX_PATH) == 0) continue;
            if (_wcsicmp(name, moduleName) == 0) return true;
        }
        return false;
    }

    int Inject(DWORD processId, const wchar_t* dllPath)
    {
        if (GetFileAttributesW(dllPath) == INVALID_FILE_ATTRIBUTES)
        {
            wprintf(L"[injector] DLL not found: %s\n", dllPath);
            return 1;
        }

        HANDLE process = OpenProcessWithRetry(processId, 30);
        if (!process)
        {
            PrintError(L"OpenProcess", GetLastError());
            return 1;
        }

        HANDLE thread = nullptr;
        LPVOID remoteMemory = nullptr;

        const DWORD bytes = static_cast<DWORD>((wcslen(dllPath) + 1) * sizeof(wchar_t));
        remoteMemory = VirtualAllocEx(process, nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        if (!remoteMemory)
        {
            PrintError(L"VirtualAllocEx", GetLastError());
            CloseHandle(process);
            return 1;
        }

        SIZE_T written = 0;
        if (!WriteProcessMemory(process, remoteMemory, dllPath, bytes, &written) || written != bytes)
        {
            PrintError(L"WriteProcessMemory", GetLastError());
            VirtualFreeEx(process, remoteMemory, 0, MEM_RELEASE);
            CloseHandle(process);
            return 1;
        }

        HMODULE kernel32 = GetModuleHandleW(L"kernel32.dll");
        FARPROC loadLibrary = kernel32 ? GetProcAddress(kernel32, "LoadLibraryW") : nullptr;
        if (!loadLibrary)
        {
            PrintError(L"GetProcAddress(LoadLibraryW)", GetLastError());
            VirtualFreeEx(process, remoteMemory, 0, MEM_RELEASE);
            CloseHandle(process);
            return 1;
        }

        DWORD threadId = 0;
        thread = CreateRemoteThread(process, nullptr, 0,
                                    reinterpret_cast<LPTHREAD_START_ROUTINE>(loadLibrary),
                                    remoteMemory, 0, &threadId);
        if (!thread)
        {
            PrintError(L"CreateRemoteThread", GetLastError());
            VirtualFreeEx(process, remoteMemory, 0, MEM_RELEASE);
            CloseHandle(process);
            return 1;
        }

        const DWORD waitResult = WaitForSingleObject(thread, 10000);
        if (waitResult != WAIT_OBJECT_0)
        {
            wprintf(L"[injector] LoadLibraryW thread did not finish (wait=%lu)\n", waitResult);
            CloseHandle(thread);
            VirtualFreeEx(process, remoteMemory, 0, MEM_RELEASE);
            CloseHandle(process);
            return 1;
        }

        DWORD exitCode = 0;
        if (!GetExitCodeThread(thread, &exitCode) || exitCode == 0)
        {
            wprintf(L"[injector] LoadLibraryW returned 0; the DLL could not be loaded.\n");
            CloseHandle(thread);
            VirtualFreeEx(process, remoteMemory, 0, MEM_RELEASE);
            CloseHandle(process);
            return 1;
        }

        wprintf(L"[injector] LoadLibraryW succeeded (module=0x%llX)\n",
                static_cast<unsigned long long>(exitCode));

        Sleep(500);
        const bool visible = ModuleLoaded(process, L"BedrockMenu.dll");
        wprintf(L"[injector] Module verification: %s\n", visible ? L"loaded" : L"not visible");

        CloseHandle(thread);
        VirtualFreeEx(process, remoteMemory, 0, MEM_RELEASE);
        CloseHandle(process);
        return visible ? 0 : 1;
    }
}

int wmain(int argc, wchar_t** argv)
{
    if (argc < 2)
    {
        wprintf(L"usage: Injector.exe <probe|inject> <pid> [dll]\n");
        return 2;
    }

    if (_wcsicmp(argv[1], L"probe") == 0)
    {
        if (argc < 3) return 2;
        const DWORD pid = static_cast<DWORD>(_wcstoui64(argv[2], nullptr, 10));
        HANDLE process = OpenProcessWithRetry(pid, 10);
        if (!process) return 1;
        CloseHandle(process);
        return 0;
    }

    if (_wcsicmp(argv[1], L"inject") == 0)
    {
        if (argc < 4) return 2;
        const DWORD pid = static_cast<DWORD>(_wcstoui64(argv[2], nullptr, 10));
        return Inject(pid, argv[3]);
    }

    wprintf(L"[injector] unknown command: %s\n", argv[1]);
    return 2;
}
