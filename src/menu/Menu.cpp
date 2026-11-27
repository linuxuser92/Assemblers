#include "Menu.h"
#include "common.h"
#include "Config.h"
#include "Game.h"
#include "Features.h"
#include "GameHook.h"
#include "AutoDetect.h"
#include "sdk/Offsets.h"

#include <imgui_internal.h>

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>

static bool s_debugConsoleOpen = false;
static ImVec2 s_menuPos(80.0f, 80.0f);
static ImVec2 s_consolePos(200.0f, 160.0f);
static OffsetEditor s_offsetEditor;
static bool s_offsetEditorLoaded = false;

static std::vector<AutoDetect::PlayerCandidate> s_playerCandidates;
static AutoDetect::VelocitySession s_velocitySession;
static bool s_velocityAutoPrompted = false;
static uintptr_t s_selectedPlayer = 0;
static Vec3 s_selectedPosition{};
static bool s_filterByCoordinates = false;
static float s_detectX = 0.0f;
static float s_detectY = 0.0f;
static float s_detectZ = 0.0f;
static std::string s_setupStatus = "idle";

namespace
{
    enum class MenuSection
    {
        Esp,
        Movement,
        World,
        Offsets,
        Settings,
    };
    static MenuSection s_menuSection = MenuSection::Esp;

    void HandleWindowMovement()
    {
        const auto down = [](int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; };
        if (!down(VK_CONTROL)) return;

        // Ctrl + arrows moves the visible window. Ctrl + Shift + arrows always
        // moves the debug console, so both windows can be placed independently.
        ImVec2* target = nullptr;
        if (down(VK_SHIFT))
        {
            if (s_debugConsoleOpen) target = &s_consolePos;
        }
        else if (Config::g_settings.menuOpen)
        {
            target = &s_menuPos;
        }
        else if (s_debugConsoleOpen)
        {
            target = &s_consolePos;
        }
        if (!target) return;

        constexpr float step = 14.0f;
        if (down(VK_LEFT)) target->x -= step;
        if (down(VK_RIGHT)) target->x += step;
        if (down(VK_UP)) target->y -= step;
        if (down(VK_DOWN)) target->y += step;
    }

    void Requirement(const char* label, bool ok)
    {
        ImGui::TextColored(ok ? ImVec4(0.30f, 1.0f, 0.40f, 1.0f)
                              : ImVec4(1.0f, 0.40f, 0.35f, 1.0f),
                           "%s: %s", label, ok ? "OK" : "MISSING");
    }

    void ApplyEditorOffsets()
    {
        s_offsetEditor.SaveToIni();
        g_offsets.Load();
        GameHook::Refresh();
        Log("Offsets applied; tick hook refreshed");
    }

    void FilterPlayerCandidates(std::vector<AutoDetect::PlayerCandidate>& candidates,
                                bool filter, float x, float y, float z)
    {
        if (!filter) return;
        const Vec3 target{ x, y, z };
        candidates.erase(
            std::remove_if(candidates.begin(), candidates.end(),
                [&](const AutoDetect::PlayerCandidate& c) {
                    const Vec3& p = c.position;
                    return std::fabs(p.x - target.x) >= 3.0f ||
                           std::fabs(p.y - target.y) >= 3.0f ||
                           std::fabs(p.z - target.z) >= 3.0f;
                }),
            candidates.end());
    }

    struct Toast
    {
        std::string text;
        ImU32 color = IM_COL32(210, 230, 255, 255);
        ULONGLONG created = 0;
        ULONGLONG duration = 2600;
    };
    std::vector<Toast> s_toasts;

    void NotifyColored(ImU32 color, const char* fmt, ...)
    {
        if (!Config::g_settings.notificationsEnabled) return;

        char buffer[256] = {};
        va_list args = {};
        va_start(args, fmt);
        std::vsnprintf(buffer, sizeof(buffer), fmt, args);
        va_end(args);

        s_toasts.push_back({ buffer, color, GetTickCount64(), 2600 });
        if (s_toasts.size() > 6) s_toasts.erase(s_toasts.begin());
    }

    void Notify(const char* fmt, ...)
    {
        char buffer[256] = {};
        va_list args = {};
        va_start(args, fmt);
        std::vsnprintf(buffer, sizeof(buffer), fmt, args);
        va_end(args);
        NotifyColored(IM_COL32(210, 230, 255, 255), "%s", buffer);
    }

    ImU32 WithAlpha(ImU32 color, float alpha)
    {
        ImVec4 value = ImGui::ColorConvertU32ToFloat4(color);
        value.w *= (std::max)(0.0f, (std::min)(1.0f, alpha));
        return ImGui::ColorConvertFloat4ToU32(value);
    }

    void StartFlySetupIfNeeded()
    {
        if (g_offsets.entityVelocity || g_offsets.entityPosition ||
            s_velocitySession.active || s_velocityAutoPrompted)
            return;

        uintptr_t player = 0;
        if (g_offsets.localPlayerPointer &&
            CanRead(reinterpret_cast<const void*>(g_offsets.localPlayerPointer),
                    sizeof(uintptr_t)))
        {
            std::memcpy(&player,
                        reinterpret_cast<const void*>(g_offsets.localPlayerPointer),
                        sizeof(player));
        }
        if (!player) player = s_selectedPlayer;
        if (!player)
        {
            NotifyColored(IM_COL32(255, 120, 110, 255),
                          "Fly: LocalPlayer is missing - run auto-setup first");
            return;
        }

        // Try the position fallback first: it needs no jumping and works with just
        // LocalPlayer + EntityPosition.
        uint64_t positionOffset = 0;
        if (AutoDetect::DetectEntityPosition(player, s_selectedPosition, positionOffset))
        {
            s_offsetEditor.entityPosition = positionOffset;
            ApplyEditorOffsets();
            NotifyColored(IM_COL32(110, 255, 140, 255), "Fly ready (position mode)");
            return;
        }

        s_velocityAutoPrompted = true;
        AutoDetect::StartVelocityScan(s_velocitySession, player, s_selectedPosition);
        NotifyColored(IM_COL32(255, 200, 110, 255),
                      "Fly needs velocity - release then hold Space");
    }

    struct HudLine
    {
        std::string text;
        ImU32 color = IM_COL32(235, 240, 250, 255);
    };

    void DrawHud(ImDrawList* drawList, const ImVec2& viewport)
    {
        static ULONGLONG lastTick = 0;
        static float smoothedFps = 0.0f;

        const ULONGLONG now = GetTickCount64();
        if (lastTick != 0)
        {
            const float delta = static_cast<float>(now - lastTick) / 1000.0f;
            if (delta > 0.0f && delta < 1.0f)
            {
                const float instant = 1.0f / delta;
                smoothedFps = smoothedFps <= 0.0f ? instant
                                                  : smoothedFps * 0.9f + instant * 0.1f;
            }
        }
        lastTick = now;

        const auto& s = Config::g_settings;
        std::vector<HudLine> lines;

        if (s.hudShowFps)
            lines.push_back({ "FPS " + std::to_string(static_cast<int>(smoothedFps)),
                              IM_COL32(210, 230, 255, 255) });

        if (s.hudShowCoords)
        {
            if (g_offsets.localPlayerPointer && g_offsets.entityPosition)
            {
                const Vec3 p = g_game.LocalPosition();
                char text[96] = {};
                std::snprintf(text, sizeof(text), "XYZ  %.1f / %.1f / %.1f",
                              p.x, p.y, p.z);
                lines.push_back({ text, IM_COL32(180, 255, 210, 255) });
            }
            else
            {
                lines.push_back({ "XYZ  unavailable", IM_COL32(150, 150, 160, 255) });
            }
        }

        if (s.hudShowSpeed)
        {
            if (g_offsets.localPlayerPointer && g_offsets.entityVelocity)
            {
                const Vec3 v = g_game.LocalVelocity();
                const float speed = std::sqrt(v.x * v.x + v.z * v.z);
                char text[64] = {};
                std::snprintf(text, sizeof(text), "SPEED  %.1f b/s", speed);
                lines.push_back({ text, IM_COL32(255, 210, 150, 255) });
            }
            else
            {
                lines.push_back({ "SPEED  unavailable", IM_COL32(150, 150, 160, 255) });
            }
        }

        if (s.hudShowStatus)
        {
            const auto statusColor = [](bool on) {
                return on ? IM_COL32(110, 255, 140, 255) : IM_COL32(140, 140, 150, 255);
            };
            char text[96] = {};
            std::snprintf(text, sizeof(text), "FLY %s  ESP %s  FULLBRIGHT %s",
                          s.fly ? "ON" : "OFF",
                          s.espEnabled ? "ON" : "OFF",
                          s.fullbright ? "ON" : "OFF");
            lines.push_back({ text, statusColor(s.fly || s.espEnabled || s.fullbright) });

            if (s.speedMultiplier > 1.01f)
            {
                std::snprintf(text, sizeof(text), "SPEED MULTIPLIER  %.2fx",
                              s.speedMultiplier);
                lines.push_back({ text, IM_COL32(255, 210, 150, 255) });
            }
        }

        if (lines.empty()) return;

        float width = 0.0f;
        float height = 0.0f;
        for (const auto& line : lines)
        {
            const ImVec2 size = ImGui::CalcTextSize(line.text.c_str());
            width = (std::max)(width, size.x);
            height += size.y + 2.0f;
        }

        const ImVec2 pos((std::max)(0.0f, (std::min)(s.hudX, viewport.x - width - 20.0f)),
                         (std::max)(0.0f, (std::min)(s.hudY, viewport.y - height - 20.0f)));
        const ImVec2 size(width + 18.0f, height + 12.0f);

        drawList->AddRectFilled(pos, ImVec2(pos.x + size.x, pos.y + size.y), IM_COL32(10, 14, 22, 190), 4.0f);
        drawList->AddRect(pos, ImVec2(pos.x + size.x, pos.y + size.y), IM_COL32(110, 170, 255, 110), 4.0f, 0, 1.0f);

        float y = pos.y + 6.0f;
        for (const auto& line : lines)
        {
            drawList->AddText({ pos.x + 9.0f, y }, line.color, line.text.c_str());
            y += ImGui::CalcTextSize(line.text.c_str()).y + 2.0f;
        }
    }

    void DrawNotifications(ImDrawList* drawList, const ImVec2& viewport)
    {
        const ULONGLONG now = GetTickCount64();
        s_toasts.erase(
            std::remove_if(s_toasts.begin(), s_toasts.end(),
                [now](const Toast& toast) {
                    return now >= toast.created + toast.duration;
                }),
            s_toasts.end());

        float y = 12.0f;
        for (const auto& toast : s_toasts)
        {
            const ImVec2 textSize = ImGui::CalcTextSize(toast.text.c_str());
            const ImVec2 size(textSize.x + 18.0f, textSize.y + 12.0f);
            const ImVec2 pos(viewport.x - size.x - 12.0f, y);

            const ULONGLONG remaining = toast.created + toast.duration - now;
            const float alpha = remaining < 400 ? static_cast<float>(remaining) / 400.0f : 1.0f;

            drawList->AddRectFilled(pos, ImVec2(pos.x + size.x, pos.y + size.y),
                                    WithAlpha(IM_COL32(12, 16, 24, 230), alpha), 4.0f);
            drawList->AddRect(pos, ImVec2(pos.x + size.x, pos.y + size.y),
                              WithAlpha(toast.color, alpha), 4.0f, 0, 1.0f);
            drawList->AddText({ pos.x + 9.0f, pos.y + 6.0f },
                              WithAlpha(toast.color, alpha), toast.text.c_str());
            y += size.y + 6.0f;
        }
    }

    void DrawDebugConsole(bool* open)
    {
        ImGui::SetNextWindowSize({ 760.0f, 420.0f }, ImGuiCond_FirstUseEver);
        ImGui::SetWindowPos("BedrockMenu Debug Console", s_consolePos, ImGuiCond_Always);
        if (!ImGui::Begin("BedrockMenu Debug Console", open))
        {
            s_consolePos = ImGui::GetWindowPos();
            return;
        }
        s_consolePos = ImGui::GetWindowPos();

        const std::string logPath = PathToString(DataDir() / "BedrockMenu.log");
        ImGui::TextUnformatted(logPath.c_str());
        ImGui::TextDisabled("F9: show/hide   |   latest 200 log lines");
        ImGui::Separator();

        const auto lines = LogLines(200);
        ImGui::BeginChild("##LogLines", { 0.0f, -ImGui::GetFrameHeightWithSpacing() * 2.0f },
                          true, ImGuiWindowFlags_HorizontalScrollbar);
        const bool atBottom = ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 4.0f;
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, { 4.0f, 2.0f });
        for (const auto& line : lines)
            ImGui::TextUnformatted(line.c_str());
        ImGui::PopStyleVar();
        if (atBottom) ImGui::SetScrollHereY(1.0f);
        ImGui::EndChild();

        ImGui::End();
    }

    void DrawRadar(ImDrawList* drawList, const ImVec2& viewport)
    {
        const auto& s = Config::g_settings;
        const float radius = 86.0f;
        const ImVec2 center(viewport.x - radius - 18.0f, viewport.y - radius - 18.0f);

        drawList->AddCircleFilled(center, radius, IM_COL32(8, 12, 20, 190), 48);
        drawList->AddCircle(center, radius, IM_COL32(110, 170, 255, 150), 48, 1.0f);
        drawList->AddLine({ center.x - radius, center.y }, { center.x + radius, center.y },
                          IM_COL32(110, 170, 255, 60), 1.0f);
        drawList->AddLine({ center.x, center.y - radius }, { center.x, center.y + radius },
                          IM_COL32(110, 170, 255, 60), 1.0f);

        const Vec3 origin = g_game.LocalPosition();
        const float range = (std::max)(1.0f, s.espRadarRange);
        const float scale = radius / range;
        for (const auto& entity : g_game.Entities(range))
        {
            if (entity.isPlayer ? !s.espFilterPlayers : !s.espFilterMobs) continue;

            const float dx = (entity.position.x - origin.x) * scale;
            const float dz = (entity.position.z - origin.z) * scale;
            if (dx * dx + dz * dz > radius * radius) continue;

            const ImU32 color = ImGui::ColorConvertFloat4ToU32(
                entity.isPlayer ? s.espColorPlayer : s.espColorOther);
            drawList->AddCircleFilled({ center.x + dx, center.y + dz },
                                      entity.isPlayer ? 4.0f : 3.0f, color, 12);
        }

        drawList->AddCircleFilled(center, 3.0f, IM_COL32(255, 255, 255, 255), 12);
        drawList->AddText({ center.x - 17.0f, center.y + radius + 4.0f },
                          IM_COL32(200, 220, 255, 200), "RADAR");
    }

    void DrawEsp(ImDrawList* drawList, const ImVec2& viewport)
    {
        const auto& s = Config::g_settings;
        auto entities = g_game.Entities(s.espMaxDistance);

        entities.erase(
            std::remove_if(entities.begin(), entities.end(), [&](const EntityInfo& entity) {
                return entity.isPlayer ? !s.espFilterPlayers : !s.espFilterMobs;
            }),
            entities.end());

        // Draw farther entities first so the nearest ones stay on top.
        if (s.espSortByDistance)
        {
            std::sort(entities.begin(), entities.end(),
                      [](const EntityInfo& a, const EntityInfo& b) {
                          return a.distance > b.distance;
                      });
        }

        for (const auto& entity : entities)
        {
            ImVec2 feet;
            ImVec2 head;
            if (!g_game.WorldToScreen(entity.position, feet)) continue;

            Vec3 headPosition = entity.position;
            headPosition.y += 1.8f; // approx. player height in blocks
            if (!g_game.WorldToScreen(headPosition, head)) continue;

            if (feet.x < -100 || feet.x > viewport.x + 100 ||
                feet.y < -100 || feet.y > viewport.y + 100) continue;

            const ImU32 color = ImGui::ColorConvertFloat4ToU32(
                entity.isPlayer ? s.espColorPlayer : s.espColorOther);
            const float height = feet.y - head.y;
            if (height <= 2.0f || height > viewport.y * 2.0f) continue;
            const float width = (std::max)(6.0f, height * 0.32f);

            const ImVec2 boxMin(head.x - width * 0.5f, head.y);
            const ImVec2 boxMax(head.x + width * 0.5f, feet.y);

            if (s.espBoxes)
                drawList->AddRect(boxMin, boxMax, color, 0.0f, 0, 1.5f);

            if (s.espHealth && entity.hasHealth)
            {
                const float ratio = (std::max)(0.0f, (std::min)(1.0f, entity.health / 20.0f));
                const ImVec2 barMin(boxMin.x, boxMax.y + 3.0f);
                const ImVec2 barMax(boxMax.x, boxMax.y + 7.0f);
                const ImU32 healthColor = ratio > 0.5f
                    ? IM_COL32(90, 235, 120, 255)
                    : (ratio > 0.25f ? IM_COL32(250, 200, 80, 255)
                                     : IM_COL32(250, 90, 80, 255));
                drawList->AddRectFilled(barMin, barMax, IM_COL32(0, 0, 0, 190), 1.0f);
                drawList->AddRectFilled(barMin,
                                        { barMin.x + (barMax.x - barMin.x) * ratio, barMax.y },
                                        healthColor, 1.0f);
                drawList->AddRect(barMin, barMax, IM_COL32(255, 255, 255, 90), 1.0f, 0, 1.0f);
            }

            if (s.espSnaplines)
                drawList->AddLine({ viewport.x * 0.5f, viewport.y }, feet, color, 1.0f);

            const char* name = entity.name.empty()
                ? (entity.isPlayer ? "Player" : "Entity") : entity.name.c_str();
            char label[128] = {};

            if (s.espNames && s.espDistance)
                std::snprintf(label, sizeof(label), "%s  %.0fm", name, entity.distance);
            else if (s.espNames)
                std::snprintf(label, sizeof(label), "%s", name);
            else if (s.espDistance)
                std::snprintf(label, sizeof(label), "%.0fm", entity.distance);

            if (label[0])
            {
                const ImVec2 textSize = ImGui::CalcTextSize(label);
                const ImVec2 textPos(boxMin.x, boxMin.y - textSize.y - 2.0f);
                drawList->AddRectFilled(textPos, { textPos.x + textSize.x + 4.0f,
                                                   textPos.y + textSize.y + 2.0f },
                                        IM_COL32(0, 0, 0, 150));
                drawList->AddText({ textPos.x + 2.0f, textPos.y + 1.0f }, color, label);
            }
        }

        if (s.espRadar) DrawRadar(drawList, viewport);
    }

    void DrawMainWindow()
    {
        auto& s = Config::g_settings;
        ImGui::SetNextWindowSize({ 700.0f, 520.0f }, ImGuiCond_FirstUseEver);
        ImGui::SetWindowPos("BedrockMenu", s_menuPos, ImGuiCond_Always);
        if (!ImGui::Begin("BedrockMenu", &s.menuOpen, ImGuiWindowFlags_NoCollapse))
        {
            s_menuPos = ImGui::GetWindowPos();
            return;
        }
        s_menuPos = ImGui::GetWindowPos();

        // Column layout: fixed navigation column on the left, content on the right.
        ImGui::BeginChild("##NavColumn", ImVec2(140.0f, 0.0f), true,
                          ImGuiWindowFlags_NoScrollbar);
        {
            const auto navButton = [](const char* label, MenuSection section) {
                const bool selected = s_menuSection == section;
                if (selected)
                    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.16f, 0.34f, 0.62f, 1.0f));
                if (ImGui::Button(label, ImVec2(-1.0f, 0.0f))) s_menuSection = section;
                if (selected) ImGui::PopStyleColor();
            };
            navButton("ESP", MenuSection::Esp);
            navButton("Movement", MenuSection::Movement);
            navButton("World", MenuSection::World);
            navButton("Offsets", MenuSection::Offsets);
            navButton("Settings", MenuSection::Settings);
        }
        ImGui::EndChild();

        ImGui::SameLine();
        ImGui::BeginChild("##ContentColumn", ImVec2(0.0f, 0.0f), true);

            if (s_menuSection == MenuSection::Esp)
            {
                ImGui::Checkbox("Enable ESP", &s.espEnabled);

                ImGui::Columns(2, "##EspRequirements", false);
                Requirement("LocalPlayer", g_offsets.localPlayerPointer != 0);
                ImGui::NextColumn();
                Requirement("Level", g_offsets.levelPointer != 0);
                ImGui::NextColumn();
                Requirement("LevelEntityList", g_offsets.levelEntityList != 0);
                ImGui::NextColumn();
                Requirement("EntityPosition", g_offsets.entityPosition != 0);
                ImGui::NextColumn();
                Requirement("Camera", g_offsets.cameraPointer != 0);
                ImGui::NextColumn();
                Requirement("ViewProj", g_offsets.viewProj != 0);
                ImGui::NextColumn();
                Requirement("Health (bars)", g_offsets.entityHealth != 0);
                ImGui::Columns(1);

                ImGui::Separator();
                ImGui::Columns(2, "##EspOptions", false);
                ImGui::Checkbox("Boxes", &s.espBoxes);
                ImGui::NextColumn();
                ImGui::Checkbox("Names", &s.espNames);
                ImGui::NextColumn();
                ImGui::Checkbox("Distance", &s.espDistance);
                ImGui::NextColumn();
                ImGui::Checkbox("Snaplines", &s.espSnaplines);
                ImGui::NextColumn();
                ImGui::Checkbox("Health bars", &s.espHealth);
                ImGui::NextColumn();
                ImGui::Checkbox("Show players", &s.espFilterPlayers);
                ImGui::NextColumn();
                ImGui::Checkbox("Show entities", &s.espFilterMobs);
                ImGui::NextColumn();
                ImGui::Checkbox("Sort by distance", &s.espSortByDistance);
                ImGui::NextColumn();
                ImGui::Checkbox("Radar", &s.espRadar);
                ImGui::Columns(1);

                ImGui::SliderFloat("Max distance", &s.espMaxDistance, 5.0f, 200.0f, "%.0f m");
                if (s.espRadar)
                    ImGui::SliderFloat("Radar range", &s.espRadarRange, 10.0f, 160.0f, "%.0f m");

                ImGui::Columns(2, "##EspColors", false);
                ImGui::ColorEdit4("Player color", reinterpret_cast<float*>(&s.espColorPlayer));
                ImGui::NextColumn();
                ImGui::ColorEdit4("Entity color", reinterpret_cast<float*>(&s.espColorOther));
                ImGui::Columns(1);
            }

            if (s_menuSection == MenuSection::Movement)
            {
                ImGui::Columns(2, "##MoveRequirements", false);
                Requirement("LocalPlayer", g_offsets.localPlayerPointer != 0);
                ImGui::NextColumn();
                Requirement("Velocity offset", g_offsets.entityVelocity != 0);
                ImGui::NextColumn();
                Requirement("Position (fly fallback)", g_offsets.entityPosition != 0);
                ImGui::NextColumn();
                Requirement("OnGround (no fall)", g_offsets.entityOnGround != 0);
                ImGui::NextColumn();
                if (GameHook::IsInstalled())
                    ImGui::TextColored(ImVec4(0.30f, 1.0f, 0.40f, 1.0f), "Tick hook: active");
                else
                    ImGui::TextColored(ImVec4(1.0f, 0.80f, 0.30f, 1.0f),
                                       "Tick hook: off (frame fallback)");
                ImGui::Columns(1);

                ImGui::Separator();
                ImGui::Checkbox("Fly (hold jump)", &s.fly);
                if (s.fly) StartFlySetupIfNeeded();
                ImGui::Columns(2, "##MoveOptions", false);
                ImGui::SliderFloat("Fly speed", &s.flySpeed, -1.0f, 2.0f, "%.2f");
                ImGui::NextColumn();
                ImGui::SliderFloat("Speed multiplier", &s.speedMultiplier, 0.1f, 5.0f, "%.2fx");
                ImGui::NextColumn();
                ImGui::SliderFloat("Step height", &s.stepHeight, 0.0f, 2.0f, "%.2f");
                ImGui::NextColumn();
                ImGui::Checkbox("No fall damage", &s.noFallDamage);
                ImGui::Columns(1);

                const bool flyReady = g_offsets.localPlayerPointer &&
                                      (g_offsets.entityVelocity || g_offsets.entityPosition);
                const bool speedReady = g_offsets.localPlayerPointer && g_offsets.entityVelocity;
                if (s.fly && !flyReady)
                    ImGui::TextColored(ImVec4(1.0f, 0.40f, 0.35f, 1.0f),
                        "Fly inactive: needs LocalPlayer + Velocity, or LocalPlayer + Position.");
                if (s.speedMultiplier != 1.0f && !speedReady)
                    ImGui::TextColored(ImVec4(1.0f, 0.40f, 0.35f, 1.0f),
                        "Speed multiplier needs LocalPlayer + EntityVelocity.");
            }

            if (s_menuSection == MenuSection::World)
            {
                ImGui::Columns(2, "##WorldRequirements", false);
                Requirement("Gamma address", g_offsets.gammaAddress != 0);
                ImGui::NextColumn();
                Requirement("Level", g_offsets.levelPointer != 0);
                ImGui::NextColumn();
                Requirement("LevelTime", g_offsets.levelTime != 0);
                ImGui::Columns(1);

                ImGui::Separator();
                ImGui::Columns(2, "##WorldOptions", false);
                ImGui::Checkbox("Fullbright", &s.fullbright);
                ImGui::NextColumn();
                ImGui::SliderFloat("Brightness", &s.brightness, 0.5f, 5.0f, "%.2f");
                ImGui::NextColumn();
                ImGui::Checkbox("Set time", &s.setTime);
                ImGui::NextColumn();
                ImGui::SliderFloat("Time of day", &s.timeOfDay, 0.0f, 1.0f, "%.2f");
                ImGui::Columns(1);
                ImGui::Checkbox("Show coordinates", &s.showCoords);
            }

            if (s_menuSection == MenuSection::Offsets)
            {
                if (!s_offsetEditorLoaded)
                {
                    s_offsetEditor.LoadFromIni();
                    s_offsetEditorLoaded = true;
                }

                ImGui::TextWrapped(
                    "All addresses are RVAs relative to Minecraft.Windows.exe. "
                    "Save & apply writes BedrockMenu.ini and reloads live.");

                if (ImGui::Button("Reload from INI")) s_offsetEditor.LoadFromIni();
                ImGui::SameLine();
                if (ImGui::Button("Save & apply")) ApplyEditorOffsets();
                ImGui::Separator();

                const auto Hex = [](const char* label, uint64_t& value) {
                    ImGui::InputScalar(label, ImGuiDataType_U64, &value, nullptr, nullptr,
                                       "0x%llX", ImGuiInputTextFlags_CharsHexadecimal);
                };

                ImGui::Columns(2, "##OffsetFields", false);
                ImGui::Text("Addresses");
                Hex("Tick", s_offsetEditor.tick);
                Hex("LocalPlayer", s_offsetEditor.localPlayer);
                Hex("Gamma", s_offsetEditor.gamma);
                Hex("Level", s_offsetEditor.level);
                Hex("Camera", s_offsetEditor.camera);

                ImGui::NextColumn();
                ImGui::Text("Level / Entity offsets");
                Hex("LevelEntityList", s_offsetEditor.levelEntityList);
                Hex("LevelTime", s_offsetEditor.levelTime);
                Hex("EntityPosition", s_offsetEditor.entityPosition);
                Hex("EntityVelocity", s_offsetEditor.entityVelocity);
                Hex("EntityOnGround", s_offsetEditor.entityOnGround);
                Hex("EntityHealth", s_offsetEditor.entityHealth);
                Hex("EntityName", s_offsetEditor.entityName);
                Hex("PlayerVTable", s_offsetEditor.playerVTable);
                Hex("ViewProj", s_offsetEditor.viewProj);
                ImGui::InputInt("EntityListStride", &s_offsetEditor.entityListStride);
                ImGui::Checkbox("MatrixTransposed", &s_offsetEditor.matrixTransposed);
                ImGui::Columns(1);

                ImGui::Separator();
                ImGui::Text("Patterns (alternative to Level/Camera RVAs)");
                ImGui::InputText("Level pattern", s_offsetEditor.levelPattern,
                                 static_cast<int>(sizeof(s_offsetEditor.levelPattern)));
                ImGui::InputText("Camera pattern", s_offsetEditor.cameraPattern,
                                 static_cast<int>(sizeof(s_offsetEditor.cameraPattern)));
                ImGui::TextWrapped("Format: 48 8B 05 ?? ?? ?? ??");

                ImGui::Separator();
                ImGui::Text("Auto-detect (read-only scan)");
                ImGui::TextWrapped(
                    "Scans game memory for a player-like position. Filter by your current "
                    "in-game coordinates to avoid false positives, then pick the best match.");
                ImGui::Checkbox("Filter by coordinates", &s_filterByCoordinates);
                ImGui::Columns(3, "##DetectCoords", false);
                ImGui::InputFloat("Detect X", &s_detectX, 0.0f, 0.0f, "%.1f");
                ImGui::NextColumn();
                ImGui::InputFloat("Detect Y", &s_detectY, 0.0f, 0.0f, "%.1f");
                ImGui::NextColumn();
                ImGui::InputFloat("Detect Z", &s_detectZ, 0.0f, 0.0f, "%.1f");
                ImGui::Columns(1);

                if (ImGui::Button("One-click full setup"))
                {
                    AutoDetect::PlayerCandidate candidate{};
                    bool havePlayer = false;

                    if (s_selectedPlayer && s_offsetEditor.localPlayer)
                    {
                        candidate = { s_offsetEditor.localPlayer, s_selectedPlayer,
                                      s_selectedPosition };
                        havePlayer = true;
                    }
                    else
                    {
                        auto candidates = AutoDetect::FindPlayerCandidates();
                        FilterPlayerCandidates(candidates, s_filterByCoordinates,
                                               s_detectX, s_detectY, s_detectZ);
                        if (!candidates.empty())
                        {
                            candidate = candidates.front();
                            s_selectedPlayer = candidate.player;
                            s_selectedPosition = candidate.position;
                            s_offsetEditor.localPlayer = candidate.rva;
                            havePlayer = true;
                        }
                    }

                    if (!havePlayer)
                    {
                        s_setupStatus = "No player candidate found. Enable coordinates and rescan.";
                        Log("AutoDetect full setup: no player candidate");
                        NotifyColored(IM_COL32(255, 120, 110, 255),
                                      "Setup failed: no player candidate");
                    }
                    else
                    {
                        const ImVec2 viewport = g_game.Viewport();
                        uint64_t detectedPosition = 0;
                        if (AutoDetect::DetectEntityPosition(candidate.player,
                                                             candidate.position,
                                                             detectedPosition))
                        {
                            s_offsetEditor.entityPosition = detectedPosition;
                        }

                        uint64_t detectedHealth = 0;
                        if (AutoDetect::DetectEntityHealth(candidate.player, detectedHealth))
                            s_offsetEditor.entityHealth = detectedHealth;

                        AutoDetect::EspDetection detected{};
                        const bool espOk = AutoDetect::DetectEsp(candidate,
                                                                viewport.x, viewport.y,
                                                                detected);
                        if (espOk)
                        {
                            if (detected.entityPosition)
                                s_offsetEditor.entityPosition = detected.entityPosition;
                            s_offsetEditor.level = detected.level;
                            s_offsetEditor.levelEntityList = detected.levelEntityList;
                            s_offsetEditor.entityListStride = detected.entityListStride;
                            s_offsetEditor.camera = detected.camera;
                            s_offsetEditor.viewProj = detected.viewProj;
                            s_offsetEditor.matrixTransposed = detected.matrixTransposed;
                        }
                        ApplyEditorOffsets();

                        const bool flyReady = s_offsetEditor.entityVelocity ||
                                              s_offsetEditor.entityPosition;
                        if (flyReady)
                        {
                            s_setupStatus = espOk
                                ? "Done: fly + ESP offsets applied."
                                : "Done: fly ready (position mode). ESP incomplete (see F9).";
                            NotifyColored(espOk ? IM_COL32(110, 255, 140, 255)
                                                : IM_COL32(255, 190, 100, 255),
                                          espOk ? "Full setup complete"
                                                : "Fly ready; ESP incomplete");
                        }
                        else
                        {
                            s_velocityAutoPrompted = true;
                            AutoDetect::StartVelocityScan(s_velocitySession,
                                                          candidate.player,
                                                          candidate.position);
                            NotifyColored(IM_COL32(255, 190, 100, 255),
                                          "ESP detection incomplete; see F9");
                            s_setupStatus = "LocalPlayer applied; ESP incomplete (see F9). "
                                            "Release then hold Space.";
                        }
                    }
                }
                ImGui::TextWrapped("Setup: %s", s_setupStatus.c_str());

                if (ImGui::Button("Scan for LocalPlayer"))
                {
                    s_playerCandidates = AutoDetect::FindPlayerCandidates();
                    FilterPlayerCandidates(s_playerCandidates, s_filterByCoordinates,
                                           s_detectX, s_detectY, s_detectZ);
                    Log("AutoDetect: %zu LocalPlayer candidates found",
                        s_playerCandidates.size());
                }

                const size_t shown = (std::min)(s_playerCandidates.size(), size_t(12));
                for (size_t i = 0; i < shown; ++i)
                {
                    const auto& c = s_playerCandidates[i];
                    ImGui::PushID(static_cast<int>(i));
                    ImGui::BulletText("0x%llX  pos %.1f, %.1f, %.1f",
                                      static_cast<unsigned long long>(c.rva),
                                      c.position.x, c.position.y, c.position.z);
                    ImGui::SameLine();
                    if (ImGui::Button("Use"))
                    {
                        s_offsetEditor.localPlayer = c.rva;
                        s_selectedPlayer = c.player;
                        s_selectedPosition = c.position;
                        ApplyEditorOffsets();
                    }
                    ImGui::PopID();
                }
                if (s_playerCandidates.empty())
                    ImGui::TextDisabled("No candidates yet. Enable coordinates in Minecraft and rescan.");

                ImGui::Separator();
                uintptr_t velocityPlayer = s_selectedPlayer;
                if (!velocityPlayer && s_offsetEditor.localPlayer && g_gameBase)
                {
                    const uintptr_t slot = g_gameBase + s_offsetEditor.localPlayer;
                    if (CanRead(reinterpret_cast<const void*>(slot), sizeof(uintptr_t)))
                        std::memcpy(&velocityPlayer,
                                    reinterpret_cast<const void*>(slot),
                                    sizeof(velocityPlayer));
                }

                if (ImGui::Button("Detect position (fly)") && velocityPlayer)
                {
                    uint64_t positionOffset = 0;
                    if (AutoDetect::DetectEntityPosition(velocityPlayer, s_selectedPosition,
                                                         positionOffset))
                    {
                        s_offsetEditor.entityPosition = positionOffset;
                        ApplyEditorOffsets();
                        NotifyColored(IM_COL32(110, 255, 140, 255),
                                      "Fly ready (position mode)");
                    }
                    else
                    {
                        NotifyColored(IM_COL32(255, 120, 110, 255),
                                      "Position detect failed - see F9");
                    }
                }
                ImGui::SameLine();
                if (ImGui::Button("Detect health") && velocityPlayer)
                {
                    uint64_t healthOffset = 0;
                    if (AutoDetect::DetectEntityHealth(velocityPlayer, healthOffset))
                    {
                        s_offsetEditor.entityHealth = healthOffset;
                        ApplyEditorOffsets();
                        NotifyColored(IM_COL32(110, 255, 140, 255),
                                      "Health offset detected");
                    }
                    else
                    {
                        NotifyColored(IM_COL32(255, 120, 110, 255),
                                      "Health detect failed - see F9");
                    }
                }
                ImGui::SameLine();
                ImGui::TextDisabled("Position/health need no Space press");
                ImGui::Text("Velocity scan player: %s",
                            velocityPlayer ? "selected" : "none");
                ImGui::Text("Status: %s", AutoDetect::VelocityScanStatus(s_velocitySession));
                if (ImGui::Button("Detect velocity (hold Space)") && velocityPlayer)
                {
                    s_velocityAutoPrompted = false;
                    AutoDetect::StartVelocityScan(s_velocitySession, velocityPlayer,
                                                  s_selectedPosition);
                }
                ImGui::SameLine();
                if (ImGui::Button("Cancel"))
                    AutoDetect::CancelVelocityScan(s_velocitySession);

                uint64_t detectedVelocity = 0;
                bool jumpHeld = (GetAsyncKeyState(VK_SPACE) & 0x8000) != 0;
                if (AutoDetect::UpdateVelocityScan(s_velocitySession, jumpHeld,
                                                   detectedVelocity))
                {
                    s_offsetEditor.entityVelocity = detectedVelocity;
                    s_velocityAutoPrompted = false;
                    ApplyEditorOffsets();
                    s_setupStatus = "Done: fly + ESP offsets applied.";
                    NotifyColored(IM_COL32(110, 255, 140, 255), "Full setup complete");
                }

                ImGui::Separator();
                ImGui::Button("Detect all ESP offsets");
                if (ImGui::IsItemClicked() && s_selectedPlayer)
                {
                    const ImVec2 viewport = g_game.Viewport();
                    AutoDetect::PlayerCandidate candidate{
                        s_offsetEditor.localPlayer, s_selectedPlayer, s_selectedPosition };
                    uint64_t detectedHealth = 0;
                    if (AutoDetect::DetectEntityHealth(s_selectedPlayer, detectedHealth))
                        s_offsetEditor.entityHealth = detectedHealth;

                    AutoDetect::EspDetection detected{};
                    if (AutoDetect::DetectEsp(candidate, viewport.x, viewport.y, detected))
                    {
                        s_offsetEditor.localPlayer = detected.localPlayer;
                        s_offsetEditor.entityPosition = detected.entityPosition;
                        s_offsetEditor.level = detected.level;
                        s_offsetEditor.levelEntityList = detected.levelEntityList;
                        s_offsetEditor.entityListStride = detected.entityListStride;
                        s_offsetEditor.camera = detected.camera;
                        s_offsetEditor.viewProj = detected.viewProj;
                        s_offsetEditor.matrixTransposed = detected.matrixTransposed;
                        ApplyEditorOffsets();
                        Log("AutoDetect: all ESP offsets applied");
                    }
                    else
                    {
                        Log("AutoDetect: ESP detection incomplete; check the log for details");
                    }
                }
                if (!s_selectedPlayer)
                    ImGui::TextDisabled("Pick a LocalPlayer candidate first.");

            }

            if (s_menuSection == MenuSection::Settings)
            {
                ImGui::Columns(2, "##SettingsCols", false);
                ImGui::Text("Insert: menu   F1-F5: toggles   F9: log   End: unload");
                ImGui::Text("Ctrl+Arrows: move window");
                ImGui::Text("Ctrl+Shift+Arrows: move log window");
                ImGui::NextColumn();
                ImGui::Text("HUD");
                ImGui::Checkbox("Enable HUD", &s.hudEnabled);
                ImGui::Checkbox("Show FPS", &s.hudShowFps);
                ImGui::Checkbox("Show coordinates", &s.hudShowCoords);
                ImGui::Checkbox("Show speed", &s.hudShowSpeed);
                ImGui::Checkbox("Show status", &s.hudShowStatus);
                ImGui::InputFloat("HUD X", &s.hudX, 1.0f, 10.0f, "%.0f");
                ImGui::InputFloat("HUD Y", &s.hudY, 1.0f, 10.0f, "%.0f");
                ImGui::Checkbox("Notifications", &s.notificationsEnabled);
                ImGui::Columns(1);
                ImGui::Separator();
                if (ImGui::Button("Save config")) Config::Save();
                ImGui::SameLine();
                if (ImGui::Button("Load config")) Config::Load();
                if (ImGui::Button("Reload offsets"))
                {
                    s_offsetEditor.LoadFromIni();
                    g_offsets.Load();
                    GameHook::Refresh();
                }
                ImGui::Separator();
                if (ImGui::Button("Unload"))
                {
                    Config::Save();
                    g_shouldUnload = true;
                }
                ImGui::TextWrapped("Single-player use only. Offsets are in BedrockMenu.ini.");
            }
        ImGui::EndChild();
        ImGui::End();
    }
}

bool Menu::AnyWindowOpen()
{
    return Config::g_settings.menuOpen || s_debugConsoleOpen;
}

void Menu::PollHotkeys()
{
    // Polling works even when the game's UWP/CoreWindow input never reaches ImGui.
    static bool insertHeld = false;
    static bool endHeld = false;
    static bool f9Held = false;
    static bool f1Held = false;
    static bool f2Held = false;
    static bool f3Held = false;
    static bool f4Held = false;
    static bool f5Held = false;
    static float savedSpeedMultiplier = 1.0f;

    const bool insertDown = (GetAsyncKeyState(VK_INSERT) & 0x8000) != 0;
    if (insertDown && !insertHeld)
    {
        Config::g_settings.menuOpen = !Config::g_settings.menuOpen;
        Log("Hotkey: Insert -> menu %s", Config::g_settings.menuOpen ? "open" : "closed");
    }
    insertHeld = insertDown;

    const bool f9Down = (GetAsyncKeyState(VK_F9) & 0x8000) != 0;
    if (f9Down && !f9Held)
    {
        s_debugConsoleOpen = !s_debugConsoleOpen;
        Log("Hotkey: F9 -> debug console %s", s_debugConsoleOpen ? "open" : "closed");
    }
    f9Held = f9Down;

    const bool endDown = (GetAsyncKeyState(VK_END) & 0x8000) != 0;
    if (endDown && !endHeld)
    {
        Log("Hotkey: End -> unload requested");
        g_shouldUnload = true;
    }
    endHeld = endDown;

    auto& settings = Config::g_settings;
    if (settings.speedMultiplier > 1.01f)
        savedSpeedMultiplier = settings.speedMultiplier;

    const bool f1Down = (GetAsyncKeyState(VK_F1) & 0x8000) != 0;
    if (f1Down && !f1Held)
    {
        settings.fly = !settings.fly;
        Log("Hotkey: F1 -> fly %s", settings.fly ? "ON" : "OFF");
        Notify("Fly %s", settings.fly ? "ON" : "OFF");
        if (settings.fly) StartFlySetupIfNeeded();
        Config::Save();
    }
    f1Held = f1Down;

    const bool f2Down = (GetAsyncKeyState(VK_F2) & 0x8000) != 0;
    if (f2Down && !f2Held)
    {
        if (settings.speedMultiplier > 1.01f)
        {
            settings.speedMultiplier = 1.0f;
            Log("Hotkey: F2 -> speed OFF");
            Notify("Speed OFF");
        }
        else
        {
            if (savedSpeedMultiplier <= 1.01f) savedSpeedMultiplier = 2.0f;
            settings.speedMultiplier = savedSpeedMultiplier;
            Log("Hotkey: F2 -> speed ON (%.2fx)", settings.speedMultiplier);
            Notify("Speed ON  %.2fx", settings.speedMultiplier);
        }
        Config::Save();
    }
    f2Held = f2Down;

    const bool f3Down = (GetAsyncKeyState(VK_F3) & 0x8000) != 0;
    if (f3Down && !f3Held)
    {
        settings.espEnabled = !settings.espEnabled;
        Log("Hotkey: F3 -> ESP %s", settings.espEnabled ? "ON" : "OFF");
        Notify("ESP %s", settings.espEnabled ? "ON" : "OFF");
        Config::Save();
    }
    f3Held = f3Down;

    const bool f4Down = (GetAsyncKeyState(VK_F4) & 0x8000) != 0;
    if (f4Down && !f4Held)
    {
        settings.fullbright = !settings.fullbright;
        Log("Hotkey: F4 -> fullbright %s", settings.fullbright ? "ON" : "OFF");
        Notify("Fullbright %s", settings.fullbright ? "ON" : "OFF");
        Config::Save();
    }
    f4Held = f4Down;

    const bool f5Down = (GetAsyncKeyState(VK_F5) & 0x8000) != 0;
    if (f5Down && !f5Held)
    {
        settings.showCoords = !settings.showCoords;
        Log("Hotkey: F5 -> coordinates %s", settings.showCoords ? "ON" : "OFF");
        Notify("Coordinates %s", settings.showCoords ? "ON" : "OFF");
        Config::Save();
    }
    f5Held = f5Down;
}

void Menu::Draw()
{
    Features::ApplyWorld();

    // If no Tick RVA is configured, apply movement from the render loop instead.
    // GameHook owns this when installed, so never apply both in the same frame.
    if (!GameHook::IsInstalled())
    {
        Features::ApplyTick();
        Features::ApplyTickPost();
    }

    static bool loggedFirstFrame = false;
    if (!loggedFirstFrame)
    {
        loggedFirstFrame = true;
        Log("Overlay rendering started (viewport %.0fx%.0f)",
            g_game.Viewport().x, g_game.Viewport().y);
    }

    // Keyboard window movement works even when mouse input is disabled.
    HandleWindowMovement();

    // Draw to the foreground draw list instead of a fullscreen transparent window.
    // This avoids UWP/CoreWindow window-visibility issues and always lands on top.
    const ImVec2 viewport = g_game.Viewport();
    ImDrawList* drawList = ImGui::GetForegroundDrawList();

    // Gate on our own flag: ImGui::Begin() does not hide a window just because
    // *p_open is false, so F9 / the X button need this check to actually close it.
    if (s_debugConsoleOpen) DrawDebugConsole(&s_debugConsoleOpen);

    if (Config::g_settings.espEnabled) DrawEsp(drawList, viewport);

    if (Config::g_settings.hudEnabled) DrawHud(drawList, viewport);
    DrawNotifications(drawList, viewport);

    if (Config::g_settings.showCoords)
    {
        const Vec3 pos = g_game.LocalPosition();
        char text[96] = {};
        std::snprintf(text, sizeof(text), "X: %.2f  Y: %.2f  Z: %.2f", pos.x, pos.y, pos.z);
        drawList->AddText({ 12.0f, 12.0f }, IM_COL32(255, 255, 255, 255), text);
    }

    if (Config::g_settings.menuOpen)
    {
        // Small always-visible marker: proves ImGui rendering + hotkey state even if
        // the main window is off-screen or hidden by the game.
        drawList->AddRectFilled({ 12.0f, 30.0f }, { 250.0f, 52.0f }, IM_COL32(0, 0, 0, 180));
        drawList->AddText({ 16.0f, 33.0f }, IM_COL32(0, 255, 170, 255),
                          "BedrockMenu  [INSERT]  [F9 LOG]");
        DrawMainWindow();
    }
}
