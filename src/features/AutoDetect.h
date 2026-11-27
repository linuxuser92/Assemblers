#pragma once

#include "common.h"
#include "sdk/Game.h"

#include <cstdint>
#include <vector>

namespace AutoDetect
{
    struct PlayerCandidate
    {
        uint64_t rva = 0;       // RVA of the global slot that holds the player pointer
        uintptr_t player = 0;  // pointer value read from that slot
        Vec3 position{};        // position read from the player object
    };

    struct VelocityCandidate
    {
        uint64_t offset = 0;    // offset from the player object
        Vec3 value{};           // Vec3 at that offset
    };

    struct VelocitySession
    {
        bool active = false;
        int stage = 0;          // 0 = wait for Space release, 1 = wait for Space press
        uintptr_t player = 0;
        std::vector<VelocityCandidate> candidates;
        std::vector<Vec3> before;
    };

    // Read-only scans. Safe to call from the render thread on a button click.
    std::vector<PlayerCandidate> FindPlayerCandidates();
    std::vector<VelocityCandidate> FindVelocityCandidates(uintptr_t player,
                                                          const Vec3& excludePosition);

    // Finds the entity position offset inside the player object. `expected` may be all zero,
    // in which case the scan falls back to scoring stable, plausible position vectors.
    bool DetectEntityPosition(uintptr_t player, const Vec3& expected, uint64_t& offset);

    void StartVelocityScan(VelocitySession& session, uintptr_t player,
                           const Vec3& excludePosition);

    // Call every frame. Returns true once a velocity offset was detected.
    bool UpdateVelocityScan(VelocitySession& session, bool jumpHeld, uint64_t& offset);

    void CancelVelocityScan(VelocitySession& session);
    const char* VelocityScanStatus(const VelocitySession& session);

    struct GlobalPointer
    {
        uint64_t rva = 0;
        uintptr_t value = 0;
    };

    struct EspDetection
    {
        uint64_t localPlayer = 0;
        uint64_t entityPosition = 0;
        uint64_t level = 0;
        uint64_t levelEntityList = 0;
        int entityListStride = 8;
        uint64_t camera = 0;
        uint64_t viewProj = 0;
        bool matrixTransposed = false;
    };

    std::vector<GlobalPointer> FindGlobalPointers(size_t limit = 256);

    // Fills every ESP-related field using the already selected player.
    bool DetectEsp(const PlayerCandidate& player, float viewportX, float viewportY,
                   EspDetection& out);
}
