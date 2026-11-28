#include "common.h"
#include "Config.h"
#include "utils/Memory.h"
#include "sdk/Game.h"
#include "render/Renderer.h"
#include "render/D3D12Hook.h"
#include "features/Features.h"
#include "features/GameHook.h"
#include "menu/Menu.h"

#include <cstdio>

static DWORD WINAPI InitializeThread(LPVOID)
{
    Config::Load();

    if (Config::g_settings.showConsole && AllocConsole())
    {
        FILE* dummy = nullptr;
        freopen_s(&dummy, "CONOUT$", "w", stdout);
        freopen_s(&dummy, "CONOUT$", "w", stderr);
        SetConsoleTitleA("Assemblers - debug console");
    }

    Log("Assemblers starting...");
    Log("Config: console=%d d3dHooks=%d lateHook=%d imguiInput=%d",
        Config::g_settings.showConsole ? 1 : 0,
        Config::g_settings.disableD3DHooks ? 0 : 1,
        Config::g_settings.disableLateHook ? 0 : 1,
        Config::g_settings.disableImGuiInput ? 0 : 1);

    if (!Memory::LocateGameModule())
    {
        // Do NOT self-unload here: some injectors are still inside LoadLibrary(), and
        // unloading the module from another thread at that moment can crash the process.
        Log("FATAL: could not locate Minecraft module. DLL stays loaded; press End to unload.");
    }
    else
    {
        g_offsets.Load();
        g_game.Init();

        const auto warnMissing = [](bool ok, const char* message) {
            if (!ok) Log("CONFIG WARNING: %s", message);
        };
        warnMissing(g_offsets.localPlayerPointer != 0,
                    "[Addresses] LocalPlayer is 0x0; fly/speed and ESP cannot work.");
        warnMissing(g_offsets.tickFunction != 0,
                    "[Addresses] Tick is 0x0; fly/speed will use the frame fallback.");
        warnMissing(g_offsets.entityVelocity != 0,
                    "[Offsets] EntityVelocity is 0x0; fly/speed cannot work.");
        warnMissing(g_offsets.entityPosition != 0,
                    "[Offsets] EntityPosition is 0x0; ESP cannot work.");
        warnMissing(g_offsets.levelPointer != 0 && g_offsets.levelEntityList != 0,
                    "[Addresses] Level/[Offsets] LevelEntityList missing; ESP cannot enumerate entities.");
        warnMissing(g_offsets.cameraPointer != 0 && g_offsets.viewProj != 0,
                    "[Patterns] Camera and [Offsets] ViewProj missing; ESP cannot project entities.");
        warnMissing(g_offsets.gammaAddress != 0,
                    "[Addresses] Gamma is 0x0; fullbright cannot work.");
        warnMissing(g_offsets.levelTime != 0,
                    "[Offsets] LevelTime is 0x0; set-time cannot work.");
        if (!Config::g_settings.disableD3DHooks)
        {
            D3D12Hook::Install();

            // If swap-chain creation was missed (late injection), try the deferred fallback
            // after the game has settled. Doing this immediately can collide with game startup.
            Sleep(1000);
            if (!Config::g_settings.disableLateHook && !D3D12Hook::HasPresentHook())
            {
                Log("Present hook missing after 1s; attempting late-injection fallback...");
                D3D12Hook::InstallLatePresentHook();
            }
        }
        else
        {
            Log("D3D hooks disabled by config (no overlay).");
        }
        GameHook::Install();

        Log("Initialization complete. Insert = menu, End = unload.");
    }

    // Poll hotkeys on our own thread so they work even if the D3D12 hooks fail to install.
    while (!g_shouldUnload)
    {
        Menu::PollHotkeys();
        Sleep(20);
    }

    Log("Unloading...");
    Features::Restore();
    GameHook::Remove();
    D3D12Hook::Remove();
    Config::Save();

    if (g_thisModule)
        FreeLibraryAndExitThread(g_thisModule, 0);
    return 0;
}

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        g_thisModule = module;
        DisableThreadLibraryCalls(module);
        HANDLE thread = CreateThread(nullptr, 0, &InitializeThread, nullptr, 0, nullptr);
        if (thread) CloseHandle(thread);
    }
    else if (reason == DLL_PROCESS_DETACH)
    {
        g_shouldUnload = true;
    }
    return TRUE;
}
