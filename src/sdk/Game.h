#pragma once
#include "Offsets.h"

#include <string>
#include <vector>

struct Vec3
{
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
};

struct EntityInfo
{
    uintptr_t address = 0;
    Vec3 position{};
    std::string name;
    bool isPlayer = false;
    float distance = 0.0f;
};

class Game
{
public:
    bool Init();

    uintptr_t Level() const;
    uintptr_t LocalPlayer() const;
    Vec3 LocalPosition() const;
    Vec3 LocalVelocity() const;

    // Returns entities found in the level list, filtered by readability and distance.
    std::vector<EntityInfo> Entities(float maxDistance) const;

    // Projects a world position into screen pixels. Returns false if behind camera / off-screen.
    bool WorldToScreen(Vec3 world, ImVec2& out) const;

    void SetViewport(float width, float height) { m_viewportX = width; m_viewportY = height; }
    ImVec2 Viewport() const { return { m_viewportX, m_viewportY }; }

private:
    bool ReadEntity(uintptr_t address, EntityInfo& out, Vec3 origin) const;
    bool ReadMatrix(DirectX::XMMATRIX& out) const;

    float m_viewportX = 1920.0f;
    float m_viewportY = 1080.0f;
};

extern Game g_game;
