#include "common.h"
#include "features/AutoDetect.h"
#include "utils/Memory.h"

#include <iostream>

namespace
{
    int failures = 0;

    void Check(bool condition, const char* name)
    {
        std::cout << (condition ? "[PASS] " : "[FAIL] ") << name << "\n";
        if (!condition) ++failures;
    }
}

int main()
{
    g_thisModule = GetModuleHandle(nullptr);

    // 48 8B 05 10 00 00 00 -> mov rax, [rip+0x10]
    const unsigned char code[] = { 0x48, 0x8B, 0x05, 0x10, 0x00, 0x00, 0x00, 0x90, 0xC3 };
    const uintptr_t found = Memory::Scan(reinterpret_cast<uintptr_t>(code),
                                         sizeof(code), "48 8B 05 ?? ?? ?? ??");
    Check(found == reinterpret_cast<uintptr_t>(code), "wildcard pattern matches at offset 0");
    Check(Memory::Scan(reinterpret_cast<uintptr_t>(code), sizeof(code), "48 8B 05 FF FF FF FF") == 0,
          "non-matching pattern returns 0");
    Check(Memory::Scan(reinterpret_cast<uintptr_t>(code), sizeof(code),
                       "48 8B 05 ?0 ?0 ?0 ?0") == reinterpret_cast<uintptr_t>(code),
          "libhat partial-nibble mask matches");
    Check(Memory::Scan(reinterpret_cast<uintptr_t>(code), sizeof(code), "not a pattern") == 0,
          "invalid pattern returns 0");
    // `48 8B 05` is a 7-byte instruction: REX + opcode + ModRM + disp32.
    Check(Memory::FollowRipRelative(found) == found + 7 + 0x10, "RIP-relative resolution (7-byte)");
    const unsigned char callCode[] = { 0xE8, 0x10, 0x00, 0x00, 0x00 };
    Check(Memory::FollowRipRelative(reinterpret_cast<uintptr_t>(callCode)) ==
          reinterpret_cast<uintptr_t>(callCode) + 5 + 0x10, "RIP-relative resolution (5-byte call)");
    Check(ParseInt("0x1234") == 0x1234, "hex parsing");
    Check(ParseInt("42") == 42, "decimal parsing");
    Check(CanRead(code, sizeof(code)), "CanRead on own memory");
    Check(!CanRead(reinterpret_cast<const void*>(0x1000), 4), "CanRead rejects invalid address");

    // Guided CanFly detection: only bytes that changed 0 -> 1 between snapshots qualify.
    unsigned char playerBytes[16] = {};
    AutoDetect::BoolSnapshot survival;
    AutoDetect::BoolSnapshot creative;
    Check(AutoDetect::CaptureBoolSnapshot(reinterpret_cast<uintptr_t>(playerBytes),
                                          sizeof(playerBytes), survival),
          "CanFly survival snapshot captured");
    playerBytes[3] = 1;
    playerBytes[7] = 1;
    Check(AutoDetect::CaptureBoolSnapshot(reinterpret_cast<uintptr_t>(playerBytes),
                                          sizeof(playerBytes), creative),
          "CanFly creative snapshot captured");
    const auto canFlyCandidates = AutoDetect::FindCanFlyCandidates(
        survival, creative, { 7 }, 24);
    Check(canFlyCandidates.size() == 1 && canFlyCandidates.front() == 3,
          "CanFly candidate ignores excluded offset");
    Check(AutoDetect::RecommendedCanFlySnapshotSize() > 0,
          "CanFly snapshot size is configured");

    std::cout << (failures ? "UNIT TESTS FAILED\n" : "UNIT TESTS PASSED\n");
    return failures ? 1 : 0;
}
