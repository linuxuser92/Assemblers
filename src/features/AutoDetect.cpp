#include "AutoDetect.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <unordered_set>
#include <utility>

namespace
{
    bool ReadVec3(uintptr_t address, Vec3& out)
    {
        if (!CanRead(reinterpret_cast<const void*>(address), sizeof(Vec3))) return false;
        std::memcpy(&out, reinterpret_cast<const void*>(address), sizeof(Vec3));
        return true;
    }

    template <typename T>
    bool ReadValue(uintptr_t address, T& out)
    {
        if (!CanRead(reinterpret_cast<const void*>(address), sizeof(T))) return false;
        std::memcpy(&out, reinterpret_cast<const void*>(address), sizeof(T));
        return true;
    }

    bool Finite(float value)
    {
        return std::isfinite(value);
    }

    bool PlausiblePlayerPosition(const Vec3& p)
    {
        if (!Finite(p.x) || !Finite(p.y) || !Finite(p.z)) return false;
        if (std::fabs(p.x) > 1000000.0f || std::fabs(p.z) > 1000000.0f) return false;
        if (p.y < -300.0f || p.y > 1200.0f) return false;
        // A real player is rarely exactly at the origin.
        if (std::fabs(p.x) < 0.01f && std::fabs(p.y) < 0.01f && std::fabs(p.z) < 0.01f)
            return false;
        return true;
    }

    bool SameVec3(const Vec3& a, const Vec3& b)
    {
        return std::fabs(a.x - b.x) < 0.001f &&
               std::fabs(a.y - b.y) < 0.001f &&
               std::fabs(a.z - b.z) < 0.001f;
    }

    bool PlausibleVelocity(const Vec3& v)
    {
        if (!Finite(v.x) || !Finite(v.y) || !Finite(v.z)) return false;
        return std::fabs(v.x) < 500.0f &&
               std::fabs(v.y) < 500.0f &&
               std::fabs(v.z) < 500.0f;
    }
}

std::vector<AutoDetect::PlayerCandidate> AutoDetect::FindPlayerCandidates()
{
    std::vector<PlayerCandidate> results;
    if (!g_gameBase || !g_gameSize) return results;

    std::unordered_set<uintptr_t> seen;
    const uintptr_t end = g_gameBase + g_gameSize;

    // Walk committed readable regions instead of calling VirtualQuery per qword.
    for (uintptr_t region = g_gameBase; region < end; )
    {
        MEMORY_BASIC_INFORMATION mbi = {};
        if (!VirtualQuery(reinterpret_cast<const void*>(region), &mbi, sizeof(mbi)) ||
            mbi.RegionSize == 0)
        {
            region += 0x1000;
            continue;
        }

        const uintptr_t base = reinterpret_cast<uintptr_t>(mbi.BaseAddress);
        const DWORD readable = PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY |
                               PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE;
        if (mbi.State == MEM_COMMIT && (mbi.Protect & readable) != 0)
        {
            const uintptr_t regionEnd = base + mbi.RegionSize;

            for (uintptr_t address = base; address + sizeof(uintptr_t) <= regionEnd;
                 address += sizeof(uintptr_t))
            {
                uintptr_t pointer = 0;
                std::memcpy(&pointer, reinterpret_cast<const void*>(address),
                            sizeof(pointer));
                if (pointer == 0 || (pointer & 0x3) != 0) continue;
                if (!seen.insert(pointer).second) continue;

                Vec3 position{};
                if (!ReadVec3(pointer, position)) continue;
                if (!PlausiblePlayerPosition(position)) continue;

                results.push_back({ static_cast<uint64_t>(address - g_gameBase),
                                    pointer, position });
                if (results.size() >= 64) return results;
            }
        }

        region = base + mbi.RegionSize;
    }

    return results;
}

bool AutoDetect::DetectEntityPosition(uintptr_t player, const Vec3& expected, uint64_t& offset)
{
    offset = 0;
    if (!player) return false;

    const bool haveExpected = PlausiblePlayerPosition(expected);

    // Pass 1: match the known coordinates closely. This is the most reliable path when the
    // user has Show Coordinates enabled and typed the current X/Y/Z into the offset tab.
    // Pass 2: otherwise pick a stable, plausible position vector with a large magnitude,
    // which distinguishes it from near-zero velocity vectors.
    for (int pass = 0; pass < 2; ++pass)
    {
        if (pass == 1 && haveExpected)
        {
            Log("AutoDetect: position did not match coordinates; using stable-vector fallback");
        }

        double bestScore = -1.0;
        uint64_t bestOffset = 0;

        for (uint64_t candidate = 0; candidate <= 0x800; candidate += 4)
        {
            Vec3 first{};
            if (!ReadVec3(player + candidate, first)) continue;
            if (!PlausiblePlayerPosition(first)) continue;

            // Require two identical reads so transient/uninitialized floats are skipped.
            Vec3 second{};
            if (!ReadVec3(player + candidate, second) || !SameVec3(first, second)) continue;

            double score = 0.0;
            if (pass == 0 && haveExpected)
            {
                const double dx = first.x - expected.x;
                const double dy = first.y - expected.y;
                const double dz = first.z - expected.z;
                const double distance = std::sqrt(dx * dx + dy * dy + dz * dz);
                if (distance > 8.0) continue; // coordinates can tick between scan and apply
                score = 1000.0 - distance;
            }
            else
            {
                const double magnitude = (std::max)({ std::fabs(first.x),
                                                      std::fabs(first.y),
                                                      std::fabs(first.z) });
                if (magnitude < 2.0) continue; // velocity-like values are usually near zero
                score = magnitude;
            }

            if (score > bestScore)
            {
                bestScore = score;
                bestOffset = candidate;
            }
        }

        if (bestOffset)
        {
            offset = bestOffset;
            Log("AutoDetect: position offset 0x%llX (pass %d, score %.2f)",
                static_cast<unsigned long long>(bestOffset), pass + 1, bestScore);
            return true;
        }
    }

    Log("AutoDetect: no stable position vector found in the first 0x800 bytes of the player");
    return false;
}

bool AutoDetect::DetectEntityHealth(uintptr_t player, uint64_t& offset)
{
    offset = 0;
    if (!player) return false;

    double bestScore = -1.0;
    uint64_t bestOffset = 0;

    for (uint64_t candidate = 0; candidate <= 0x800; candidate += 4)
    {
        float first = 0.0f;
        if (!ReadValue(player + candidate, first)) continue;
        if (!Finite(first) || first <= 0.0f || first > 1000.0f) continue;

        float second = 0.0f;
        if (!ReadValue(player + candidate, second) || !Finite(second)) continue;
        if (std::fabs(first - second) > 0.001f) continue;

        // Bedrock entities normally have 20.0 health when full. Prefer values close to
        // that, then fall back to any small stable positive float.
        const double distanceFromFull = std::fabs(static_cast<double>(first) - 20.0);
        const double score = 100.0 - distanceFromFull;
        if (score > bestScore)
        {
            bestScore = score;
            bestOffset = candidate;
        }
    }

    if (!bestOffset)
    {
        Log("AutoDetect: no stable health-like float found in the player object");
        return false;
    }

    offset = bestOffset;
    Log("AutoDetect: health offset 0x%llX (score %.2f)",
        static_cast<unsigned long long>(bestOffset), bestScore);
    return true;
}

std::vector<AutoDetect::VelocityCandidate> AutoDetect::FindVelocityCandidates(
    uintptr_t player, const Vec3& excludePosition)
{
    std::vector<VelocityCandidate> results;
    if (!player) return results;

    // Entity velocity is normally within the first few hundred bytes of the object.
    for (uint64_t offset = 0; offset <= 0x600; offset += 4)
    {
        Vec3 value{};
        if (!ReadVec3(player + offset, value)) continue;
        if (!PlausibleVelocity(value)) continue;
        if (SameVec3(value, excludePosition)) continue;

        results.push_back({ offset, value });
        if (results.size() >= 128) break;
    }

    return results;
}

void AutoDetect::StartVelocityScan(VelocitySession& session, uintptr_t player,
                                   const Vec3& excludePosition)
{
    session.active = true;
    session.stage = 0; // wait for Space to be released before sampling the press
    session.player = player;
    session.candidates = FindVelocityCandidates(player, excludePosition);
    session.before.clear();
    session.before.reserve(session.candidates.size());
    for (const auto& candidate : session.candidates)
        session.before.push_back(candidate.value);

    Log("AutoDetect: velocity scan started (%zu candidates); release then hold Space",
        session.candidates.size());
}

bool AutoDetect::UpdateVelocityScan(VelocitySession& session, bool jumpHeld, uint64_t& offset)
{
    if (!session.active) return false;

    if (session.stage == 0)
    {
        if (!jumpHeld) session.stage = 1;
        return false;
    }

    if (!jumpHeld) return false;

    double bestScore = 0.0;
    uint64_t bestOffset = 0;
    std::vector<std::pair<double, uint64_t>> scores;

    for (size_t i = 0; i < session.candidates.size(); ++i)
    {
        Vec3 after{};
        if (!ReadVec3(session.player + session.candidates[i].offset, after)) continue;

        const Vec3& before = session.before[i];
        const double score = std::fabs(after.x - before.x) +
                             std::fabs(after.y - before.y) * 2.0 +
                             std::fabs(after.z - before.z);
        scores.emplace_back(score, session.candidates[i].offset);
        if (score > bestScore)
        {
            bestScore = score;
            bestOffset = session.candidates[i].offset;
        }
    }

    session.active = false;
    if (bestScore > 0.01 && bestOffset != 0)
    {
        offset = bestOffset;
        Log("AutoDetect: velocity offset 0x%llX (score %.3f)",
            static_cast<unsigned long long>(bestOffset), bestScore);
        return true;
    }

    std::sort(scores.begin(), scores.end(), [](const auto& a, const auto& b) {
        return a.first > b.first;
    });
    char detail[192] = {};
    std::snprintf(detail, sizeof(detail), "top: 0x%llX=%.3f 0x%llX=%.3f 0x%llX=%.3f",
                  static_cast<unsigned long long>(scores.size() > 0 ? scores[0].second : 0),
                  scores.size() > 0 ? scores[0].first : 0.0,
                  static_cast<unsigned long long>(scores.size() > 1 ? scores[1].second : 0),
                  scores.size() > 1 ? scores[1].first : 0.0,
                  static_cast<unsigned long long>(scores.size() > 2 ? scores[2].second : 0),
                  scores.size() > 2 ? scores[2].first : 0.0);
    Log("AutoDetect: no velocity value changed (%zu candidates); %s; try moving/jumping",
        session.candidates.size(), detail);
    return false;
}

void AutoDetect::CancelVelocityScan(VelocitySession& session)
{
    session.active = false;
    session.candidates.clear();
    session.before.clear();
}

const char* AutoDetect::VelocityScanStatus(const VelocitySession& session)
{
    if (!session.active) return "idle";
    if (session.stage == 0) return "Release Space...";
    return "Hold Space now...";
}

std::vector<AutoDetect::GlobalPointer> AutoDetect::FindGlobalPointers(size_t limit)
{
    std::vector<GlobalPointer> results;
    if (!g_gameBase || !g_gameSize) return results;

    std::unordered_set<uintptr_t> seen;
    const uintptr_t end = g_gameBase + g_gameSize;

    for (uintptr_t region = g_gameBase; region < end; )
    {
        MEMORY_BASIC_INFORMATION mbi = {};
        if (!VirtualQuery(reinterpret_cast<const void*>(region), &mbi, sizeof(mbi)) ||
            mbi.RegionSize == 0)
        {
            region += 0x1000;
            continue;
        }

        const uintptr_t base = reinterpret_cast<uintptr_t>(mbi.BaseAddress);
        const DWORD readable = PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY |
                               PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE;
        if (mbi.State == MEM_COMMIT && (mbi.Protect & readable) != 0)
        {
            const uintptr_t regionEnd = base + mbi.RegionSize;
            for (uintptr_t address = base; address + sizeof(uintptr_t) <= regionEnd;
                 address += sizeof(uintptr_t))
            {
                uintptr_t pointer = 0;
                std::memcpy(&pointer, reinterpret_cast<const void*>(address),
                            sizeof(pointer));
                if (pointer == 0 || (pointer & 0x3) != 0) continue;
                if (!CanRead(reinterpret_cast<const void*>(pointer), 8)) continue;
                if (!seen.insert(pointer).second) continue;

                results.push_back({ static_cast<uint64_t>(address - g_gameBase), pointer });
                if (results.size() >= limit) return results;
            }
        }

        region = base + mbi.RegionSize;
    }

    return results;
}

namespace
{
    uint64_t FindPositionOffset(uintptr_t player, const Vec3& position)
    {
        for (uint64_t offset = 0; offset <= 0x600; offset += 4)
        {
            Vec3 value{};
            if (!ReadVec3(player + offset, value)) continue;
            if (SameVec3(value, position)) return offset;
        }
        return 0;
    }

    struct LevelHit
    {
        uint64_t rva = 0;
        uint64_t listOffset = 0;
        int stride = 8;
        int score = 0;
    };

    LevelHit DetectLevel(const std::vector<AutoDetect::GlobalPointer>& globals,
                         uintptr_t player, uint64_t entityPositionOffset)
    {
        LevelHit best;
        if (!entityPositionOffset) return best;

        for (const auto& global : globals)
        {
            for (uint64_t offset = 0; offset <= 0x1000; offset += 8)
            {
                uintptr_t listAddress = global.value + offset;
                uintptr_t begin = 0;
                uintptr_t end = 0;
                if (!CanRead(reinterpret_cast<const void*>(listAddress), sizeof(uintptr_t) * 2))
                    continue;
                std::memcpy(&begin, reinterpret_cast<const void*>(listAddress),
                            sizeof(begin));
                std::memcpy(&end, reinterpret_cast<const void*>(listAddress + sizeof(uintptr_t)),
                            sizeof(end));
                if (end <= begin || (end - begin) > 0x100000 || (end - begin) < 8) continue;
                if (!CanRead(reinterpret_cast<const void*>(begin), 8)) continue;

                for (const int stride : { 8, 16 })
                {
                    int count = 0;
                    bool containsPlayer = false;
                    for (size_t i = 0; i < 64; ++i)
                    {
                        const uintptr_t entry = begin + i * static_cast<size_t>(stride);
                        if (!CanRead(reinterpret_cast<const void*>(entry), sizeof(uintptr_t)))
                            break;
                        uintptr_t entity = 0;
                        std::memcpy(&entity, reinterpret_cast<const void*>(entry),
                                    sizeof(entity));
                        if (!entity || (entity & 0x3)) continue;

                        Vec3 pos{};
                        if (!ReadVec3(entity + entityPositionOffset, pos)) continue;
                        if (!PlausiblePlayerPosition(pos)) continue;
                        ++count;
                        if (entity == player) containsPlayer = true;
                    }

                    const int score = count + (containsPlayer ? 100 : 0);
                    if (score > best.score)
                        best = { global.rva, offset, stride, score };
                }
            }
        }

        return best;
    }

    struct CameraHit
    {
        uint64_t rva = 0;
        uint64_t viewProj = 0;
        bool transposed = false;
        double score = 0.0;
    };

    bool ProjectMatrix(const float* values, bool transposed, const Vec3& point,
                       float viewportX, float viewportY, ImVec2& screen)
    {
        for (int i = 0; i < 16; ++i)
            if (!Finite(values[i]) || std::fabs(values[i]) > 1000000.0f) return false;

        DirectX::XMMATRIX raw = DirectX::XMMatrixIdentity();
        std::memcpy(&raw, values, sizeof(raw));
        const DirectX::XMMATRIX matrix =
            transposed ? DirectX::XMMatrixTranspose(raw) : raw;

        const DirectX::XMVECTOR clip = DirectX::XMVector4Transform(
            DirectX::XMVectorSet(point.x, point.y, point.z, 1.0f), matrix);
        const float w = DirectX::XMVectorGetW(clip);
        if (!Finite(w) || w <= 0.0001f) return false;

        const float ndcX = DirectX::XMVectorGetX(clip) / w;
        const float ndcY = DirectX::XMVectorGetY(clip) / w;
        if (!Finite(ndcX) || !Finite(ndcY)) return false;
        if (ndcX < -1.5f || ndcX > 1.5f || ndcY < -1.5f || ndcY > 1.5f) return false;

        screen.x = (ndcX * 0.5f + 0.5f) * viewportX;
        screen.y = (0.5f - ndcY * 0.5f) * viewportY;
        return true;
    }

    CameraHit DetectCamera(const std::vector<AutoDetect::GlobalPointer>& globals,
                           const Vec3& playerPosition, float viewportX, float viewportY)
    {
        CameraHit best;
        if (viewportX <= 0.0f || viewportY <= 0.0f) return best;

        for (const auto& global : globals)
        {
            for (uint64_t offset = 0; offset <= 0x1000; offset += 16)
            {
                const uintptr_t matrixAddress = global.value + offset;
                if (!CanRead(reinterpret_cast<const void*>(matrixAddress), 64)) continue;

                float values[16] = {};
                std::memcpy(values, reinterpret_cast<const void*>(matrixAddress),
                            sizeof(values));

                for (const bool transposed : { false, true })
                {
                    ImVec2 screen{};
                    if (!ProjectMatrix(values, transposed, playerPosition,
                                       viewportX, viewportY, screen)) continue;

                    const double dx = (screen.x - viewportX * 0.5f) / viewportX;
                    const double dy = (screen.y - viewportY * 0.5f) / viewportY;
                    const double distance = std::sqrt(dx * dx + dy * dy);
                    const double score = 1.0 / (0.01 + distance);
                    if (score > best.score)
                        best = { global.rva, offset, transposed, score };
                }
            }
        }

        return best;
    }
}

bool AutoDetect::DetectEsp(const PlayerCandidate& player, float viewportX, float viewportY,
                           EspDetection& out)
{
    out = {};
    out.localPlayer = player.rva;

    const uint64_t positionOffset = FindPositionOffset(player.player, player.position);
    out.entityPosition = positionOffset;

    const auto globals = FindGlobalPointers();

    const LevelHit level = DetectLevel(globals, player.player, positionOffset);
    if (level.score > 0)
    {
        out.level = level.rva;
        out.levelEntityList = level.listOffset;
        out.entityListStride = level.stride;
    }

    const CameraHit camera = DetectCamera(globals, player.position, viewportX, viewportY);
    if (camera.score > 0.0)
    {
        out.camera = camera.rva;
        out.viewProj = camera.viewProj;
        out.matrixTransposed = camera.transposed;
    }

    Log("AutoDetect ESP: pos=0x%llX level=0x%llX list=0x%llX stride=%d camera=0x%llX view=0x%llX transposed=%d",
        static_cast<unsigned long long>(out.entityPosition),
        static_cast<unsigned long long>(out.level),
        static_cast<unsigned long long>(out.levelEntityList),
        out.entityListStride,
        static_cast<unsigned long long>(out.camera),
        static_cast<unsigned long long>(out.viewProj),
        out.matrixTransposed ? 1 : 0);

    return out.entityPosition != 0 && out.level != 0 &&
           out.levelEntityList != 0 && out.camera != 0 && out.viewProj != 0;
}
