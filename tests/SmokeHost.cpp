#include <windows.h>
#include <iostream>

// Loads the DLL into a normal process. Since Minecraft isn't running, the DLL should:
//   1. create BedrockMenu.log
//   2. fail to locate the game module
//   3. save a default BedrockMenu.ini
//   4. self-unload via FreeLibraryAndExitThread
int wmain(int argc, wchar_t** argv)
{
    const std::wstring dll = (argc > 1) ? argv[1] : L"BedrockMenu.dll";

    HMODULE handle = LoadLibraryW(dll.c_str());
    if (!handle)
    {
        std::wcerr << L"LoadLibrary failed, error " << GetLastError() << L"\n";
        return 1;
    }
    std::wcout << L"Loaded: " << dll << L" at " << handle << L"\n";

    Sleep(2000);

    // The DLL intentionally stays loaded now (no auto-unload during LoadLibrary).
    HMODULE stillLoaded = GetModuleHandleW(L"BedrockMenu.dll");
    std::wcout << L"After init thread: "
               << (stillLoaded ? L"loaded (expected)" : L"unloaded")
               << L"\n";
    return 0;
}
