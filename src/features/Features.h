#pragma once

namespace Features
{
    // Called from the game tick hook, before the original tick runs.
    void ApplyTick();

    // Called after the original tick, so position writes are not overwritten by physics.
    void ApplyTickPost();

    // Called once per rendered frame (gamma, world time).
    void ApplyWorld();

    // Restores anything we patched before unloading.
    void Restore();
}
