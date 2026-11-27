#pragma once
#include "common.h"

#include <cstdint>

// All version-specific values. Addresses are converted to absolute pointers at load time;
// offsets stay relative to their owning object.
struct Offsets
{
    // Absolute addresses (game base + RVA, or resolved from a pattern).
    uintptr_t tickFunction = 0;       // void __fastcall Tick(void* self, float dt)
    uintptr_t localPlayerPointer = 0; // address holding LocalPlayer*
    uintptr_t gammaAddress = 0;       // address holding brightness float
    uintptr_t levelPointer = 0;       // address holding Level*
    uintptr_t cameraPointer = 0;      // address holding camera/render-context*

    // Offsets inside Level.
    uintptr_t levelEntityList = 0;
    uintptr_t levelTime = 0;
    int entityListStride = 8;

    // Offsets inside Entity / Player.
    uintptr_t entityPosition = 0; // Vec3 (3 x float)
    uintptr_t entityVelocity = 0; // Vec3 (3 x float)
    uintptr_t entityOnGround = 0; // bool
    uintptr_t entityHealth = 0;   // float (usually 20.0 when full)
    uintptr_t entityName = 0;     // char* or std::string (best effort)
    uintptr_t playerVTable = 0;   // RVA of the player vtable, used for filtering

    // Camera / render context.
    uintptr_t viewProj = 0;
    bool matrixTransposed = false;

    void Load();

    static uintptr_t Absolute(uintptr_t rva)
    {
        return (rva && g_gameBase) ? g_gameBase + rva : 0;
    }
};

extern Offsets g_offsets;

// Editable mirror of BedrockMenu.ini. All address fields are RVAs, not absolute pointers.
struct OffsetEditor
{
    uint64_t tick = 0;
    uint64_t localPlayer = 0;
    uint64_t gamma = 0;
    uint64_t level = 0;
    uint64_t camera = 0;

    uint64_t levelEntityList = 0;
    uint64_t levelTime = 0;
    uint64_t entityPosition = 0;
    uint64_t entityVelocity = 0;
    uint64_t entityOnGround = 0;
    uint64_t entityHealth = 0;
    uint64_t entityName = 0;
    uint64_t playerVTable = 0;
    uint64_t viewProj = 0;
    int entityListStride = 8;
    bool matrixTransposed = false;

    char levelPattern[256] = {};
    char cameraPattern[256] = {};

    void LoadFromIni();
    void SaveToIni() const;
};
