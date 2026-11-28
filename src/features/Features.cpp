#include "Features.h"
#include "common.h"
#include "Config.h"
#include "Game.h"

#include <cmath>
#include <cstring>

namespace
{
    bool CanWrite(uintptr_t address, size_t size);

    template <typename T>
    bool ReadValue(uintptr_t address, T& out)
    {
        if (!CanRead(reinterpret_cast<const void*>(address), sizeof(T))) return false;
        std::memcpy(&out, reinterpret_cast<const void*>(address), sizeof(T));
        return true;
    }

    template <typename T>
    bool WriteValue(uintptr_t address, const T& value)
    {
        if (!CanWrite(address, sizeof(T))) return false;
        std::memcpy(reinterpret_cast<void*>(address), &value, sizeof(T));
        return true;
    }

    bool CanWrite(uintptr_t address, size_t size)
    {
        MEMORY_BASIC_INFORMATION mbi = {};
        if (!VirtualQuery(reinterpret_cast<const void*>(address), &mbi, sizeof(mbi))) return false;
        if (mbi.State != MEM_COMMIT) return false;
        const DWORD writable = PAGE_READWRITE | PAGE_WRITECOPY |
                               PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
        return (mbi.Protect & writable) != 0 && address + size <=
               reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
    }

    // Position-mode fly state. We track our own Y instead of adding to the physics-updated
    // position, so a held Space jump impulse cannot push the player above the target.
    bool s_positionFlyActive = false;
    uintptr_t s_positionFlyPlayer = 0;
    float s_positionFlyY = 0.0f;
    ULONGLONG s_positionFlyLast = 0;

    // Vanilla/CanFly mode: write the game's own fly flag instead of velocity. We save the
    // original byte so disabling Fly or unloading does not leave the flag changed.
    bool s_canFlyActive = false;
    uintptr_t s_canFlyPlayer = 0;
    uint64_t s_canFlyOffset = 0;
    uint8_t s_canFlyOriginal = 0;

    void RestoreCanFly()
    {
        if (s_canFlyActive && s_canFlyPlayer && s_canFlyOffset)
        {
            WriteValue(s_canFlyPlayer + s_canFlyOffset, s_canFlyOriginal);
            Log("Fly CanFly mode disabled (offset 0x%llX restored)",
                static_cast<unsigned long long>(s_canFlyOffset));
        }
        s_canFlyActive = false;
        s_canFlyPlayer = 0;
        s_canFlyOffset = 0;
    }
}

void Features::ApplyTick()
{
    const auto& s = Config::g_settings;
    if (!s.fly && s.speedMultiplier == 1.0f && !s.noFallDamage)
    {
        RestoreCanFly();
        return;
    }

    const uintptr_t player = g_game.LocalPlayer();
    if (!player) return;

    // Horion-style fly: use the game's own CanFly bool when configured. This avoids all
    // velocity/position detection and keeps single-player vanilla flight semantics.
    const bool canFlyMode = s.fly && g_offsets.entityCanFly != 0;
    if (canFlyMode)
    {
        const uintptr_t address = player + g_offsets.entityCanFly;
        if (!s_canFlyActive || s_canFlyPlayer != player ||
            s_canFlyOffset != g_offsets.entityCanFly)
        {
            RestoreCanFly();
            uint8_t original = 0;
            if (!ReadValue(address, original)) return;
            s_canFlyActive = true;
            s_canFlyPlayer = player;
            s_canFlyOffset = g_offsets.entityCanFly;
            s_canFlyOriginal = original;
            Log("Fly CanFly mode engaged at offset 0x%llX (no velocity needed)",
                static_cast<unsigned long long>(g_offsets.entityCanFly));
        }
        const uint8_t enabled = 1;
        WriteValue(address, enabled);
    }
    else
    {
        RestoreCanFly();
    }

    const bool jumpHeld = (GetAsyncKeyState(VK_SPACE) & 0x8000) != 0;

    // Velocity is still used for the speed multiplier, but not for fly in CanFly mode.
    if (g_offsets.entityVelocity &&
        (s.speedMultiplier != 1.0f || (s.fly && !canFlyMode)))
    {
        Vec3 velocity{};
        const uintptr_t address = player + g_offsets.entityVelocity;
        if (ReadValue(address, velocity))
        {
            // UI says "hold jump": only override vertical velocity while Space is down.
            // Use the magnitude so a negative configured speed still climbs.
            if (s.fly && !canFlyMode && jumpHeld) velocity.y = std::fabs(s.flySpeed);
            velocity.x *= s.speedMultiplier;
            velocity.z *= s.speedMultiplier;
            if (std::isfinite(velocity.x) && std::isfinite(velocity.y) && std::isfinite(velocity.z))
                WriteValue(address, velocity);
        }
    }
    if (s.noFallDamage && g_offsets.entityOnGround)
    {
        const bool grounded = true;
        WriteValue(player + g_offsets.entityOnGround, grounded);
    }
}

void Features::ApplyTickPost()
{
    const auto& s = Config::g_settings;

    // Position mode is only used when velocity mode is unavailable. Velocity writes must stay
    // in ApplyTick (before physics); position writes must happen after physics.
    if (!s.fly || g_offsets.entityCanFly || g_offsets.entityVelocity || !g_offsets.entityPosition)
    {
        s_positionFlyActive = false;
        s_positionFlyLast = 0;
        return;
    }

    const uintptr_t player = g_game.LocalPlayer();
    if (!player) return;

    Vec3 position{};
    const uintptr_t address = player + g_offsets.entityPosition;
    if (!ReadValue(address, position) || !std::isfinite(position.y)) return;

    const ULONGLONG now = GetTickCount64();
    float delta = 0.0f;
    if (s_positionFlyActive && s_positionFlyPlayer == player && s_positionFlyLast)
    {
        delta = static_cast<float>(now - s_positionFlyLast) / 1000.0f;
        if (delta > 0.1f) delta = 0.1f; // clamp after a stall / tab switch
    }
    else
    {
        s_positionFlyY = position.y;
        Log("Fly position mode engaged at Y=%.2f (offset 0x%llX)",
            position.y, static_cast<unsigned long long>(g_offsets.entityPosition));
    }

    if ((GetAsyncKeyState(VK_SPACE) & 0x8000) != 0)
    {
        // Use the magnitude so a negative configured speed still climbs while Space is held.
        s_positionFlyY += std::fabs(s.flySpeed) * delta * 20.0f;
    }

    position.y = s_positionFlyY;
    if (std::isfinite(position.y)) WriteValue(address, position);

    s_positionFlyActive = true;
    s_positionFlyPlayer = player;
    s_positionFlyLast = now;
}

void Features::ApplyWorld()
{
    const auto& s = Config::g_settings;

    if (g_offsets.gammaAddress && s.fullbright)
    {
        const float value = s.brightness;
        WriteValue(g_offsets.gammaAddress, value);
    }

    const uintptr_t level = g_game.Level();
    if (level && g_offsets.levelTime && s.setTime)
    {
        const float value = s.timeOfDay;
        WriteValue(level + g_offsets.levelTime, value);
    }
}

void Features::Restore()
{
    RestoreCanFly();
    s_positionFlyActive = false;
    s_positionFlyLast = 0;
}
