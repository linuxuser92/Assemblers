#pragma once
#include "common.h"

#include <cstdint>
#include <string_view>

namespace Memory
{
    // Finds a byte signature inside [start, start+length) using libhat's SIMD scanner.
    // Pattern format: "48 8B 05 ?? ?? ?? ??" — "??" is a wildcard. libhat also
    // accepts "?" and partial-nibble masks such as "?3".
    // Returns the absolute address of the first match, or 0.
    uintptr_t Scan(uintptr_t start, size_t length, std::string_view pattern);

    // Scans the game module. Returns absolute address or 0.
    uintptr_t ScanGame(std::string_view pattern);

    // Converts an address found by ScanGame to an RVA.
    inline uintptr_t ToRva(uintptr_t address) { return address ? address - g_gameBase : 0; }

    // Resolves a RIP-relative displacement at `instructionAddress + 1` to its absolute target.
    uintptr_t FollowRipRelative(uintptr_t instructionAddress);

    // Finds the game module and caches base/size in common.h globals.
    bool LocateGameModule();
}
