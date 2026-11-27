#pragma once
#include <imgui.h>

#include <string>

namespace Config
{
    struct Settings
    {
        // ESP
        bool espEnabled = true;
        bool espBoxes = true;
        bool espNames = true;
        bool espDistance = true;
        bool espSnaplines = false;
        bool espHealth = true;
        bool espFilterPlayers = true;
        bool espFilterMobs = true;
        bool espSortByDistance = false;
        bool espRadar = false;
        float espRadarRange = 40.0f;
        float espMaxDistance = 80.0f;
        ImVec4 espColorPlayer = ImVec4(0.20f, 0.85f, 1.0f, 1.0f);
        ImVec4 espColorOther = ImVec4(1.0f, 0.45f, 0.30f, 1.0f);

        // Movement
        bool fly = false;
        float flySpeed = 0.6f;
        float speedMultiplier = 1.0f;
        float stepHeight = 0.6f;
        bool noFallDamage = false;

        // World
        bool fullbright = false;
        float brightness = 1.0f;
        bool setTime = false;
        float timeOfDay = 0.5f;
        bool showCoords = true;

        // UI
        bool menuOpen = true;
        bool hudEnabled = true;
        bool hudShowFps = true;
        bool hudShowCoords = true;
        bool hudShowSpeed = true;
        bool hudShowStatus = true;
        float hudX = 12.0f;
        float hudY = 12.0f;
        bool notificationsEnabled = true;

        // Diagnostics / safe mode — all risky features are OFF by default.
        bool showConsole = false;
        bool disableImGuiInput = false;  // 1 = keyboard only; 0 = safe event-based mouse input
        bool disableLateHook = true;     // skip dummy-swapchain Present hook
        bool disableD3DHooks = true;     // no graphics hooks at all until explicitly enabled
    };

    extern Settings g_settings;

    void Load();
    void Save();
    std::wstring IniPath();
}
