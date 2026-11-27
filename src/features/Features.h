#pragma once

namespace Features
{
    // Called from the game tick hook.
    void ApplyTick();

    // Called once per rendered frame (gamma, world time).
    void ApplyWorld();

    // Restores anything we patched before unloading.
    void Restore();
}
