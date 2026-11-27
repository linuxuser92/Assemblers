#include "Memory.h"

#include <cctype>
#include <cstring>
#include <psapi.h>
#include <vector>

namespace
{
    bool ParseByte(std::string_view token, uint8_t& value, bool& wildcard)
    {
        if (token.size() != 2) return false;
        if (token[0] == '?' && token[1] == '?')
        {
            wildcard = true;
            value = 0;
            return true;
        }
        auto nibble = [](char c) -> int {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'a' && c <= 'f') return c - 'a' + 10;
            if (c >= 'A' && c <= 'F') return c - 'A' + 10;
            return -1;
        };
        const int hi = nibble(token[0]);
        const int lo = nibble(token[1]);
        if (hi < 0 || lo < 0) return false;
        wildcard = false;
        value = static_cast<uint8_t>((hi << 4) | lo);
        return true;
    }

    bool CompilePattern(std::string_view pattern, std::vector<uint8_t>& bytes, std::vector<bool>& wildcards)
    {
        size_t i = 0;
        while (i < pattern.size())
        {
            while (i < pattern.size() && std::isspace(static_cast<unsigned char>(pattern[i]))) ++i;
            if (i >= pattern.size()) break;

            const size_t end = pattern.find_first_of(" \t\r\n", i);
            const auto token = pattern.substr(i, end == std::string_view::npos ? end : end - i);
            uint8_t byte = 0;
            bool wildcard = false;
            if (!ParseByte(token, byte, wildcard)) return false;
            bytes.push_back(byte);
            wildcards.push_back(wildcard);
            i = end == std::string_view::npos ? pattern.size() : end;
        }
        return !bytes.empty();
    }
}

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
        std::vector<uint8_t> bytes;
        std::vector<bool> wildcards;
        if (!CompilePattern(pattern, bytes, wildcards)) return 0;
        if (length < bytes.size()) return 0;

        const auto* data = reinterpret_cast<const uint8_t*>(start);
        for (size_t i = 0; i <= length - bytes.size(); ++i)
        {
            bool match = true;
            for (size_t j = 0; j < bytes.size(); ++j)
            {
                if (!wildcards[j] && data[i + j] != bytes[j])
                {
                    match = false;
                    break;
                }
            }
            if (match) return start + i;
        }
        return 0;
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
