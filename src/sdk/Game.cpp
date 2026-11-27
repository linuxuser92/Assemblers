#include "Game.h"
#include "common.h"

#include <algorithm>
#include <cmath>
#include <cstring>

Game g_game;

bool Game::Init()
{
    if (!g_gameBase || !g_gameSize)
    {
        Log("Game::Init failed: game module not located.");
        return false;
    }
    Log("Game::Init OK (base 0x%llX, size 0x%llX)",
        static_cast<unsigned long long>(g_gameBase), static_cast<unsigned long long>(g_gameSize));
    return true;
}

template <typename T>
static bool Read(uintptr_t address, T& out)
{
    if (!CanRead(reinterpret_cast<const void*>(address), sizeof(T))) return false;
    std::memcpy(&out, reinterpret_cast<const void*>(address), sizeof(T));
    return true;
}

uintptr_t Game::Level() const
{
    uintptr_t level = 0;
    return (g_offsets.levelPointer && Read(g_offsets.levelPointer, level)) ? level : 0;
}

uintptr_t Game::LocalPlayer() const
{
    uintptr_t player = 0;
    return (g_offsets.localPlayerPointer && Read(g_offsets.localPlayerPointer, player)) ? player : 0;
}

Vec3 Game::LocalPosition() const
{
    Vec3 result{};
    const uintptr_t player = LocalPlayer();
    if (!player || !g_offsets.entityPosition) return result;
    Read(player + g_offsets.entityPosition, result);
    return result;
}

Vec3 Game::LocalVelocity() const
{
    Vec3 result{};
    const uintptr_t player = LocalPlayer();
    if (!player || !g_offsets.entityVelocity) return result;
    Read(player + g_offsets.entityVelocity, result);
    return result;
}

bool Game::ReadEntity(uintptr_t address, EntityInfo& out, Vec3 origin) const
{
    if (!address || !g_offsets.entityPosition) return false;
    if (!CanRead(reinterpret_cast<const void*>(address), 0x1000)) return false;

    Vec3 pos{};
    if (!Read(address + g_offsets.entityPosition, pos)) return false;
    if (!std::isfinite(pos.x) || !std::isfinite(pos.y) || !std::isfinite(pos.z)) return false;

    out.address = address;
    out.position = pos;
    out.distance = std::sqrt((pos.x - origin.x) * (pos.x - origin.x) +
                             (pos.y - origin.y) * (pos.y - origin.y) +
                             (pos.z - origin.z) * (pos.z - origin.z));

    // Player filtering: compare the entity's vtable pointer with the configured RVA.
    if (g_offsets.playerVTable)
    {
        uintptr_t vtable = 0;
        out.isPlayer = Read(address, vtable) && vtable == Offsets::Absolute(g_offsets.playerVTable);
    }
    else
    {
        out.isPlayer = true;
    }

    // Name: follow a char* if possible, otherwise treat the offset as an inline buffer.
    if (g_offsets.entityName)
    {
        uintptr_t namePtr = 0;
        char inlineName[32] = {};
        if (Read(address + g_offsets.entityName, namePtr) &&
            CanRead(reinterpret_cast<const void*>(namePtr), 32))
        {
            namePtr += 0; // MSVC std::string long layout stores data at the pointer itself.
            std::memcpy(inlineName, reinterpret_cast<const void*>(namePtr), 31);
        }
        else if (CanRead(reinterpret_cast<const void*>(address + g_offsets.entityName), 32))
        {
            std::memcpy(inlineName, reinterpret_cast<const void*>(address + g_offsets.entityName), 31);
        }
        inlineName[31] = '\0';
        if (inlineName[0] && std::isprint(static_cast<unsigned char>(inlineName[0])))
            out.name = inlineName;
    }
    return true;
}

std::vector<EntityInfo> Game::Entities(float maxDistance) const
{
    std::vector<EntityInfo> result;
    const uintptr_t level = Level();
    if (!level || !g_offsets.levelEntityList)
    {
        // ESP works without a level pointer only if the user configured one.
        return result;
    }

    const uintptr_t listAddress = level + g_offsets.levelEntityList;
    uintptr_t begin = 0;
    uintptr_t end = 0;
    if (!Read(listAddress, begin) || !CanRead(reinterpret_cast<const void*>(begin), 8)) return result;
    Read(listAddress + sizeof(uintptr_t), end);

    size_t count = 0;
    constexpr size_t kMaxEntities = 512;
    if (end > begin && (end - begin) < 0x100000)
        count = static_cast<size_t>((end - begin) / g_offsets.entityListStride);

    count = (std::min)(count, kMaxEntities);
    if (count == 0) count = kMaxEntities; // fallback: bounded scan

    const Vec3 origin = LocalPosition();
    for (size_t i = 0; i < count; ++i)
    {
        uintptr_t entity = 0;
        if (!Read(begin + i * g_offsets.entityListStride, entity)) break;
        EntityInfo info;
        if (ReadEntity(entity, info, origin) && info.distance <= maxDistance)
            result.push_back(info);
    }
    return result;
}

bool Game::ReadMatrix(DirectX::XMMATRIX& out) const
{
    uintptr_t camera = 0;
    if (g_offsets.cameraPointer && !Read(g_offsets.cameraPointer, camera) && !camera)
        camera = g_offsets.cameraPointer; // some builds store the struct inline

    uintptr_t matrixAddress = 0;
    if (camera && g_offsets.viewProj)
        matrixAddress = camera + g_offsets.viewProj;

    if (!matrixAddress || !CanRead(reinterpret_cast<const void*>(matrixAddress), 64)) return false;

    DirectX::XMMATRIX raw = DirectX::XMMatrixIdentity();
    std::memcpy(&raw, reinterpret_cast<const void*>(matrixAddress), 64);
    out = g_offsets.matrixTransposed ? DirectX::XMMatrixTranspose(raw) : raw;
    return true;
}

bool Game::WorldToScreen(Vec3 world, ImVec2& out) const
{
    DirectX::XMMATRIX viewProj;
    if (!ReadMatrix(viewProj)) return false;

    const DirectX::XMVECTOR clip = DirectX::XMVector4Transform(
        DirectX::XMVectorSet(world.x, world.y, world.z, 1.0f), viewProj);

    const float w = DirectX::XMVectorGetW(clip);
    if (w <= 0.0001f || !std::isfinite(w)) return false;

    const float ndcX = DirectX::XMVectorGetX(clip) / w;
    const float ndcY = DirectX::XMVectorGetY(clip) / w;
    if (!std::isfinite(ndcX) || !std::isfinite(ndcY)) return false;

    out.x = (ndcX * 0.5f + 0.5f) * m_viewportX;
    out.y = (0.5f - ndcY * 0.5f) * m_viewportY;
    return true;
}
