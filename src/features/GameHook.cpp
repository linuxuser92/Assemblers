#include "GameHook.h"
#include "Features.h"
#include "sdk/Offsets.h"

#include <MinHook.h>

namespace
{
    void* originalTick = nullptr;
    void* installedAddress = nullptr;
    bool installed = false;

    void STDMETHODCALLTYPE TickHook(void* self, float deltaTime)
    {
        Features::ApplyTick();
        using Fn = void(STDMETHODCALLTYPE*)(void*, float);
        reinterpret_cast<Fn>(originalTick)(self, deltaTime);
        Features::ApplyTickPost();
    }
}

bool GameHook::Install()
{
    if (!g_offsets.tickFunction || installed) return false;

    const MH_STATUS status = MH_CreateHook(reinterpret_cast<void*>(g_offsets.tickFunction),
                                           &TickHook, &originalTick);
    if (status != MH_OK)
    {
        Log("GameHook: MH_CreateHook failed: %s", MH_StatusToString(status));
        return false;
    }
    if (MH_EnableHook(reinterpret_cast<void*>(g_offsets.tickFunction)) != MH_OK)
    {
        Log("GameHook: MH_EnableHook failed");
        MH_RemoveHook(reinterpret_cast<void*>(g_offsets.tickFunction));
        return false;
    }

    installed = true;
    installedAddress = reinterpret_cast<void*>(g_offsets.tickFunction);
    Log("GameHook installed at 0x%llX", static_cast<unsigned long long>(g_offsets.tickFunction));
    return true;
}

bool GameHook::IsInstalled()
{
    return installed;
}

void GameHook::Refresh()
{
    const void* target = reinterpret_cast<const void*>(g_offsets.tickFunction);
    if (installed && installedAddress != target) Remove();
    if (!installed) Install();
}

void GameHook::Remove()
{
    if (!installed) return;
    MH_DisableHook(installedAddress);
    MH_RemoveHook(installedAddress);
    originalTick = nullptr;
    installedAddress = nullptr;
    installed = false;
    Log("GameHook removed.");
}
