#include "Config.h"
#include "common.h"

namespace Config
{
    Settings g_settings;

    std::wstring IniPath()
    {
        return (DataDir() / L"BedrockMenu.ini").wstring();
    }

    static void ReadBool(const wchar_t* section, const wchar_t* key, bool& value)
    {
        value = GetPrivateProfileIntW(section, key, value ? 1 : 0, IniPath().c_str()) != 0;
    }

    static void ReadFloat(const wchar_t* section, const wchar_t* key, float& value)
    {
        wchar_t defaultValue[32] = {};
        swprintf_s(defaultValue, L"%.9g", value);
        wchar_t buf[64] = {};
        GetPrivateProfileStringW(section, key, defaultValue, buf, 64, IniPath().c_str());
        value = std::stof(buf);
    }

    static void WriteValue(const wchar_t* section, const wchar_t* key, const std::wstring& value)
    {
        WritePrivateProfileStringW(section, key, value.c_str(), IniPath().c_str());
    }

    static std::wstring FloatString(float value)
    {
        return std::to_wstring(value);
    }

    void Load()
    {
        auto& s = g_settings;
        ReadBool(L"ESP", L"Enabled", s.espEnabled);
        ReadBool(L"ESP", L"Boxes", s.espBoxes);
        ReadBool(L"ESP", L"Names", s.espNames);
        ReadBool(L"ESP", L"Distance", s.espDistance);
        ReadBool(L"ESP", L"Snaplines", s.espSnaplines);
        ReadFloat(L"ESP", L"MaxDistance", s.espMaxDistance);

        ReadBool(L"Movement", L"Fly", s.fly);
        ReadFloat(L"Movement", L"FlySpeed", s.flySpeed);
        ReadFloat(L"Movement", L"SpeedMultiplier", s.speedMultiplier);
        ReadFloat(L"Movement", L"StepHeight", s.stepHeight);
        ReadBool(L"Movement", L"NoFallDamage", s.noFallDamage);

        ReadBool(L"World", L"Fullbright", s.fullbright);
        ReadFloat(L"World", L"Brightness", s.brightness);
        ReadBool(L"World", L"SetTime", s.setTime);
        ReadFloat(L"World", L"TimeOfDay", s.timeOfDay);
        ReadBool(L"World", L"ShowCoords", s.showCoords);

        ReadBool(L"UI", L"MenuOpen", s.menuOpen);
        ReadBool(L"HUD", L"Enabled", s.hudEnabled);
        ReadBool(L"HUD", L"ShowFps", s.hudShowFps);
        ReadBool(L"HUD", L"ShowCoords", s.hudShowCoords);
        ReadBool(L"HUD", L"ShowSpeed", s.hudShowSpeed);
        ReadBool(L"HUD", L"ShowStatus", s.hudShowStatus);
        ReadFloat(L"HUD", L"X", s.hudX);
        ReadFloat(L"HUD", L"Y", s.hudY);
        ReadBool(L"HUD", L"Notifications", s.notificationsEnabled);

        ReadBool(L"General", L"ShowConsole", s.showConsole);
        ReadBool(L"General", L"DisableImGuiInput", s.disableImGuiInput);
        ReadBool(L"General", L"DisableLateHook", s.disableLateHook);
        ReadBool(L"General", L"DisableD3DHooks", s.disableD3DHooks);
        Log("Config loaded from %s", PathToString(IniPath()).c_str());
    }

    void Save()
    {
        auto& s = g_settings;
        WriteValue(L"ESP", L"Enabled", s.espEnabled ? L"1" : L"0");
        WriteValue(L"ESP", L"Boxes", s.espBoxes ? L"1" : L"0");
        WriteValue(L"ESP", L"Names", s.espNames ? L"1" : L"0");
        WriteValue(L"ESP", L"Distance", s.espDistance ? L"1" : L"0");
        WriteValue(L"ESP", L"Snaplines", s.espSnaplines ? L"1" : L"0");
        WriteValue(L"ESP", L"MaxDistance", FloatString(s.espMaxDistance));

        WriteValue(L"Movement", L"Fly", s.fly ? L"1" : L"0");
        WriteValue(L"Movement", L"FlySpeed", FloatString(s.flySpeed));
        WriteValue(L"Movement", L"SpeedMultiplier", FloatString(s.speedMultiplier));
        WriteValue(L"Movement", L"StepHeight", FloatString(s.stepHeight));
        WriteValue(L"Movement", L"NoFallDamage", s.noFallDamage ? L"1" : L"0");

        WriteValue(L"World", L"Fullbright", s.fullbright ? L"1" : L"0");
        WriteValue(L"World", L"Brightness", FloatString(s.brightness));
        WriteValue(L"World", L"SetTime", s.setTime ? L"1" : L"0");
        WriteValue(L"World", L"TimeOfDay", FloatString(s.timeOfDay));
        WriteValue(L"World", L"ShowCoords", s.showCoords ? L"1" : L"0");

        WriteValue(L"UI", L"MenuOpen", s.menuOpen ? L"1" : L"0");
        WriteValue(L"HUD", L"Enabled", s.hudEnabled ? L"1" : L"0");
        WriteValue(L"HUD", L"ShowFps", s.hudShowFps ? L"1" : L"0");
        WriteValue(L"HUD", L"ShowCoords", s.hudShowCoords ? L"1" : L"0");
        WriteValue(L"HUD", L"ShowSpeed", s.hudShowSpeed ? L"1" : L"0");
        WriteValue(L"HUD", L"ShowStatus", s.hudShowStatus ? L"1" : L"0");
        WriteValue(L"HUD", L"X", FloatString(s.hudX));
        WriteValue(L"HUD", L"Y", FloatString(s.hudY));
        WriteValue(L"HUD", L"Notifications", s.notificationsEnabled ? L"1" : L"0");

        WriteValue(L"General", L"ShowConsole", s.showConsole ? L"1" : L"0");
        WriteValue(L"General", L"DisableImGuiInput", s.disableImGuiInput ? L"1" : L"0");
        WriteValue(L"General", L"DisableLateHook", s.disableLateHook ? L"1" : L"0");
        WriteValue(L"General", L"DisableD3DHooks", s.disableD3DHooks ? L"1" : L"0");
        Log("Config saved.");
    }
}
