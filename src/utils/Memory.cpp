#include "Memory.h"

#include <cstring>
#include <psapi.h>

#include <libhat/scanner.hpp>
#include <libhat/signature.hpp>

namespace Memory
{
    bool LocateGameModule()
    {
        g_gameModule = GetModuleHandleW(L"Minecraft.Windows.exe");
        if (!g_gameModule)
        {
            // Some builds ship as Minecraft.exe / Minecraft.UWP.exe.
            g_gameModule = GetModuleHandleW(L"Minecraft.exe");
        }
        if (!g_gameModule) return false;

        MODULEINFO info = {};
        if (!GetModuleInformation(GetCurrentProcess(), g_gameModule, &info, sizeof(info))) return false;

        g_gameBase = reinterpret_cast<uintptr_t>(info.lpBaseOfDll);
        g_gameSize = info.SizeOfImage;
        return g_gameBase != 0 && g_gameSize != 0;
    }

    uintptr_t Scan(uintptr_t start, size_t length, std::string_view pattern)
    {
        if (!start || !length) return 0;

        auto parsed = hat::parse_signature(pattern);
        if (!parsed.has_value()) return 0;

        auto* begin = reinterpret_cast<std::byte*>(start);
        const hat::scan_result result = hat::find_pattern(
            begin, begin + length, parsed.value(), hat::scan_alignment::X1,
            hat::scan_hint::x86_64);
        return result.has_result() ? reinterpret_cast<uintptr_t>(result.get()) : 0;
    }

    uintptr_t ScanGame(std::string_view pattern)
    {
        if (!g_gameBase || !g_gameSize) return 0;
        return Scan(g_gameBase, g_gameSize, pattern);
    }

    uintptr_t FollowRipRelative(uintptr_t instructionAddress)
    {
        if (!CanRead(reinterpret_cast<const void*>(instructionAddress), 15)) return 0;
        const auto* bytes = reinterpret_cast<const uint8_t*>(instructionAddress);

        size_t i = 0;
        // Legacy prefixes and REX.
        while (i < 15 && (bytes[i] == 0x66 || bytes[i] == 0x67 || bytes[i] == 0xF0 ||
                          bytes[i] == 0xF2 || bytes[i] == 0xF3 ||
                          (bytes[i] >= 0x40 && bytes[i] <= 0x4F)))
            ++i;
        if (i >= 15) return 0;

        // Direct call/jmp rel32: `E8/E9 xx xx xx xx` (5-byte instruction).
        if (bytes[i] == 0xE8 || bytes[i] == 0xE9)
        {
            if (!CanRead(bytes + i + 1, sizeof(int32_t))) return 0;
            int32_t displacement = 0;
            std::memcpy(&displacement, bytes + i + 1, sizeof(displacement));
            return instructionAddress + i + 5 + static_cast<uintptr_t>(static_cast<int64_t>(displacement));
        }

        // x86-64 ModRM memory operand with mod=00, rm=101 => RIP-relative disp32.
        if (bytes[i] == 0x0F)
        {
            ++i;
            if (i < 15 && (bytes[i] == 0x38 || bytes[i] == 0x3A)) ++i;
            ++i; // opcode
        }
        else
        {
            ++i; // opcode
        }
        if (i >= 15) return 0;

        const uint8_t modrm = bytes[i++];
        if ((modrm >> 6) != 0 || (modrm & 7) != 5) return 0;
        if (!CanRead(bytes + i, sizeof(int32_t))) return 0;

        int32_t displacement = 0;
        std::memcpy(&displacement, bytes + i, sizeof(displacement));
        return instructionAddress + i + sizeof(int32_t) +
               static_cast<uintptr_t>(static_cast<int64_t>(displacement));
    }
}
