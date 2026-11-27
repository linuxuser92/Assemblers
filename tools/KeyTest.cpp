// Small diagnostic helper: focuses the Minecraft window and taps a virtual key.
// Compiled instead of PowerShell Add-Type because antivirus blocks dynamic P/Invoke.
#include <windows.h>
#include <tlhelp32.h>

#include <cstdio>
#include <cstdlib>

namespace
{
    DWORD FindMinecraftPid()
    {
        HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (snapshot == INVALID_HANDLE_VALUE) return 0;
        PROCESSENTRY32W entry = {};
        entry.dwSize = sizeof(entry);
        DWORD pid = 0;
        if (Process32FirstW(snapshot, &entry))
        {
            do
            {
                if (_wcsicmp(entry.szExeFile, L"Minecraft.Windows.exe") == 0)
                {
                    pid = entry.th32ProcessID;
                    break;
                }
            } while (Process32NextW(snapshot, &entry));
        }
        CloseHandle(snapshot);
        return pid;
    }

    static HWND g_found = nullptr;

    HWND FindWindowForPid(DWORD pid)
    {
        g_found = nullptr;
        EnumWindows([](HWND hwnd, LPARAM param) -> BOOL {
            DWORD pid = 0;
            GetWindowThreadProcessId(hwnd, &pid);
            if (pid == static_cast<DWORD>(param) && IsWindowVisible(hwnd))
            {
                g_found = hwnd;
                return FALSE;
            }
            return TRUE;
        }, static_cast<LPARAM>(pid));
        return g_found;
    }
}

int wmain(int argc, wchar_t** argv)
{
    if (argc < 2)
    {
        wprintf(L"usage: KeyTest.exe <vk> [times] [delayMs]\n");
        return 2;
    }

    const int vk = _wtoi(argv[1]);
    const int times = argc > 2 ? _wtoi(argv[2]) : 1;
    const int delay = argc > 3 ? _wtoi(argv[3]) : 150;

    const DWORD pid = FindMinecraftPid();
    const HWND hwnd = pid ? FindWindowForPid(pid) : nullptr;
    wprintf(L"[keytest] pid=%lu hwnd=%p\n", pid, hwnd);

    if (hwnd)
    {
        ShowWindow(hwnd, SW_RESTORE);
        SetForegroundWindow(hwnd);
        SwitchToThisWindow(hwnd, TRUE);
        Sleep(300);
    }

    for (int i = 0; i < times; ++i)
    {
        keybd_event(static_cast<BYTE>(vk), 0, 0, 0);
        Sleep(80);
        keybd_event(static_cast<BYTE>(vk), 0, KEYEVENTF_KEYUP, 0);
        Sleep(delay);
    }
    return 0;
}
