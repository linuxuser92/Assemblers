#include "Offsets.h"
#include "Config.h"
#include "utils/Memory.h"

#include <cstring>

Offsets g_offsets;

namespace
{
    std::string Narrow(const std::wstring& wide)
    {
        const int size = WideCharToMultiByte(CP_UTF8, 0, wide.c_str(),
            static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr);
        if (size <= 0) return {};
        std::string result(static_cast<size_t>(size), '\0');
        WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), static_cast<int>(wide.size()),
                            result.data(), size, nullptr, nullptr);
        return result;
    }

    uintptr_t ReadRva(const wchar_t* section, const wchar_t* key)
    {
        wchar_t buf[128] = {};
        GetPrivateProfileStringW(section, key, L"0", buf, 128, Config::IniPath().c_str());
        const uintptr_t rva = ParseInt(Narrow(std::wstring(buf)));
        // Write back a normalized 0x-prefixed value so the user sees a template.
        wchar_t normalized[32] = {};
        swprintf_s(normalized, L"0x%llX", static_cast<unsigned long long>(rva));
        WritePrivateProfileStringW(section, key, normalized, Config::IniPath().c_str());
        return rva;
    }

    std::string ReadPattern(const wchar_t* key)
    {
        wchar_t buf[256] = {};
        GetPrivateProfileStringW(L"Patterns", key, L"", buf, 256, Config::IniPath().c_str());
        return Narrow(std::wstring(buf));
    }

    // Patterns usually point at an instruction like `48 8B 05 ?? ?? ?? ??` (mov rax, [rip+X]).
    // Resolve the RIP-relative operand when present; otherwise use the match address itself.
    uintptr_t ResolvePattern(const std::string& pattern)
    {
        if (pattern.empty()) return 0;
        const uintptr_t match = Memory::ScanGame(pattern);
        if (!match) return 0;

        if (!CanRead(reinterpret_cast<const void*>(match), 6)) return match;
        const auto* bytes = reinterpret_cast<const uint8_t*>(match);
        const bool isCall = bytes[0] == 0xE8 || bytes[0] == 0xE9;
        const bool isRipMov = (bytes[0] == 0x48 || bytes[0] == 0x4C) &&
                              (bytes[1] == 0x8B || bytes[1] == 0x8D || bytes[1] == 0x89) &&
                              ((bytes[2] & 0xC7) == 0x05);
        return (isRipMov || isCall) ? Memory::FollowRipRelative(match) : match;
    }

    std::wstring Widen(const std::string& narrow)
    {
        const int size = MultiByteToWideChar(CP_UTF8, 0, narrow.c_str(),
            static_cast<int>(narrow.size()), nullptr, 0);
        if (size <= 0) return {};
        std::wstring result(static_cast<size_t>(size), L'\0');
        MultiByteToWideChar(CP_UTF8, 0, narrow.c_str(), static_cast<int>(narrow.size()),
                            result.data(), size);
        return result;
    }

    void WriteRva(const wchar_t* section, const wchar_t* key, uint64_t value)
    {
        wchar_t normalized[32] = {};
        swprintf_s(normalized, L"0x%llX", static_cast<unsigned long long>(value));
        WritePrivateProfileStringW(section, key, normalized, Config::IniPath().c_str());
    }

    void CopyPattern(char* destination, size_t size, const std::string& value)
    {
        std::strncpy(destination, value.c_str(), size - 1);
        destination[size - 1] = '\0';
    }
}

void OffsetEditor::LoadFromIni()
{
    tick = static_cast<uint64_t>(ReadRva(L"Addresses", L"Tick"));
    localPlayer = static_cast<uint64_t>(ReadRva(L"Addresses", L"LocalPlayer"));
    gamma = static_cast<uint64_t>(ReadRva(L"Addresses", L"Gamma"));
    level = static_cast<uint64_t>(ReadRva(L"Addresses", L"Level"));
    camera = static_cast<uint64_t>(ReadRva(L"Addresses", L"Camera"));

    levelEntityList = static_cast<uint64_t>(ReadRva(L"Offsets", L"LevelEntityList"));
    levelTime = static_cast<uint64_t>(ReadRva(L"Offsets", L"LevelTime"));
    entityListStride = static_cast<int>(ReadRva(L"Offsets", L"EntityListStride"));
    if (entityListStride != 4 && entityListStride != 8) entityListStride = 8;

    entityPosition = static_cast<uint64_t>(ReadRva(L"Offsets", L"EntityPosition"));
    entityVelocity = static_cast<uint64_t>(ReadRva(L"Offsets", L"EntityVelocity"));
    entityOnGround = static_cast<uint64_t>(ReadRva(L"Offsets", L"EntityOnGround"));
    entityHealth = static_cast<uint64_t>(ReadRva(L"Offsets", L"EntityHealth"));
    entityName = static_cast<uint64_t>(ReadRva(L"Offsets", L"EntityName"));
    playerVTable = static_cast<uint64_t>(ReadRva(L"Offsets", L"PlayerVTable"));
    viewProj = static_cast<uint64_t>(ReadRva(L"Offsets", L"ViewProj"));
    matrixTransposed = ReadRva(L"Offsets", L"MatrixTransposed") != 0;

    CopyPattern(levelPattern, sizeof(levelPattern), ReadPattern(L"Level"));
    CopyPattern(cameraPattern, sizeof(cameraPattern), ReadPattern(L"Camera"));
}

void OffsetEditor::SaveToIni() const
{
    const std::wstring path = Config::IniPath();

    WriteRva(L"Addresses", L"Tick", tick);
    WriteRva(L"Addresses", L"LocalPlayer", localPlayer);
    WriteRva(L"Addresses", L"Gamma", gamma);
    WriteRva(L"Addresses", L"Level", level);
    WriteRva(L"Addresses", L"Camera", camera);

    WriteRva(L"Offsets", L"LevelEntityList", levelEntityList);
    WriteRva(L"Offsets", L"LevelTime", levelTime);
    WriteRva(L"Offsets", L"EntityPosition", entityPosition);
    WriteRva(L"Offsets", L"EntityVelocity", entityVelocity);
    WriteRva(L"Offsets", L"EntityOnGround", entityOnGround);
    WriteRva(L"Offsets", L"EntityHealth", entityHealth);
    WriteRva(L"Offsets", L"EntityName", entityName);
    WriteRva(L"Offsets", L"PlayerVTable", playerVTable);
    WriteRva(L"Offsets", L"ViewProj", viewProj);

    wchar_t number[32] = {};
    swprintf_s(number, L"%d", entityListStride);
    WritePrivateProfileStringW(L"Offsets", L"EntityListStride", number, path.c_str());
    WritePrivateProfileStringW(L"Offsets", L"MatrixTransposed", matrixTransposed ? L"1" : L"0", path.c_str());

    WritePrivateProfileStringW(L"Patterns", L"Level", Widen(levelPattern).c_str(), path.c_str());
    WritePrivateProfileStringW(L"Patterns", L"Camera", Widen(cameraPattern).c_str(), path.c_str());
}

void Offsets::Load()
{
    OffsetEditor editor;
    editor.LoadFromIni();

    const uintptr_t tickRva = static_cast<uintptr_t>(editor.tick);
    const uintptr_t localPlayerRva = static_cast<uintptr_t>(editor.localPlayer);
    const uintptr_t gammaRva = static_cast<uintptr_t>(editor.gamma);
    const uintptr_t levelRva = static_cast<uintptr_t>(editor.level);
    const uintptr_t cameraRva = static_cast<uintptr_t>(editor.camera);

    levelEntityList = static_cast<uintptr_t>(editor.levelEntityList);
    levelTime = static_cast<uintptr_t>(editor.levelTime);
    entityListStride = editor.entityListStride;
    if (entityListStride != 4 && entityListStride != 8) entityListStride = 8;

    entityPosition = static_cast<uintptr_t>(editor.entityPosition);
    entityVelocity = static_cast<uintptr_t>(editor.entityVelocity);
    entityOnGround = static_cast<uintptr_t>(editor.entityOnGround);
    entityHealth = static_cast<uintptr_t>(editor.entityHealth);
    entityName = static_cast<uintptr_t>(editor.entityName);
    playerVTable = static_cast<uintptr_t>(editor.playerVTable);
    viewProj = static_cast<uintptr_t>(editor.viewProj);
    matrixTransposed = editor.matrixTransposed;

    tickFunction = Absolute(tickRva);
    localPlayerPointer = Absolute(localPlayerRva);
    gammaAddress = Absolute(gammaRva);
    levelPointer = Absolute(levelRva);
    cameraPointer = Absolute(cameraRva);

    const uintptr_t levelFromPattern = ResolvePattern(editor.levelPattern);
    const uintptr_t cameraFromPattern = ResolvePattern(editor.cameraPattern);
    if (levelPointer == 0 && levelFromPattern) levelPointer = levelFromPattern;
    if (cameraPointer == 0 && cameraFromPattern) cameraPointer = cameraFromPattern;

    Log("Offsets: base=0x%llX tick=0x%llX localPlayer=0x%llX level=0x%llX camera=0x%llX",
        static_cast<unsigned long long>(g_gameBase),
        static_cast<unsigned long long>(tickFunction),
        static_cast<unsigned long long>(localPlayerPointer),
        static_cast<unsigned long long>(levelPointer),
        static_cast<unsigned long long>(cameraPointer));
}
