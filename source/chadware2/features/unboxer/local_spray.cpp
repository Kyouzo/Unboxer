#include "local_spray.h"

#include <Windows.h>

#include <array>
#include <atomic>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "unboxer.h"
#include "../../../patterns.h"
#include "../../interfaces/interfaces.h"
#include "../../utils/memory/patternscan/patternscan.h"
#include "../../utils/schema/schema.h"

namespace
{
    // client.dll, 2026-10-08 build:
    // SprayCheck @+0x8D8970: GetSprayApplicationError's check - (pawn, CGameTrace* out, Vector* forward out, Vector* left
    // out): null = a spray can go where you look; the trace of where you look is copied out (0xC0 bytes).
    using SprayCheckFn = const char*(__fastcall*)(void* pawn, void* traceOut, float* forwardOut, float* leftOut);
    // The graffiti decal ("CPlayerSprayDecalRenderHelper", 0x30 bytes: kit, creation time, scene object, two resources,
    // the picture's async load, a "still loading" byte). C_PlayerSprayDecal has one; the spray wheel's preview is one.
    //   SetKit @+0xD089F0: (helper, sticker kit) - starts loading materials/decals/sprays/<kit>.vtex
    //   Place @+0xD0DB30: (helper, position, normal, -left, tint, preview) - makes / moves the decal in the world (the
    //     game calls it every frame; it waits while the picture loads)
    //   Release @+0xCE40C0: (helper) - the decal and its resources go (C_PlayerSprayDecal's destructor calls it)
    using SetKitFn = void(__fastcall*)(void* helper, int kit);
    using PlaceFn = bool(__fastcall*)(void* helper, const float* position, const float* normal, const float* negatedLeft, int tint, bool preview);
    using ReleaseFn = void(__fastcall*)(void* helper);

    constexpr std::size_t TRACE_BYTES = 0x100;          // the game's is 0xC0
    constexpr std::size_t TRACE_END = 0x84;             // m_vecEndPos
    constexpr std::size_t TRACE_NORMAL = 0x90;          // m_vecNormal
    constexpr std::size_t HELPER_BYTES = 0x40;          // the game's is 0x30
    constexpr std::size_t MAX_SPRAYS = 12;              // the oldest goes
    constexpr float COOLDOWN_SECONDS = 45.0f;           // CS2's (StoreAPI GetSprayCooldownSetting)

    // Place draws the decal by the game's clock against its creation time (helper +4): painted in over the first 1.5 s,
    // aged from 240 s, faded out between 320 and 360 s - CS2's own graffiti do the same. A creation time of -1 made a
    // decal invisible as soon as the map's clock passed 6 minutes (any official match).
    constexpr std::size_t HELPER_CREATION_TIME = 4;
    constexpr std::size_t HELPER_LOADING = 0x28;        // "its picture is still loading" (Place's first check, 14190)
    constexpr std::size_t GLOBALS_CLOCK = 0x30;         // [globals + 0x30], what Place compares with
    constexpr float GONE_AFTER_SECONDS = 361.0f;        // invisible from 360 s: then it's taken down

    SprayCheckFn g_check = nullptr;
    SetKitFn g_setKit = nullptr;
    PlaceFn g_place = nullptr;
    ReleaseFn g_release = nullptr;
    void* const* g_globals = nullptr;                   // the game's globals pointer (the variable Place reads)

    struct Spray
    {
        bool active = false;
        bool broken = false;                            // its Place crashed once: left alone
        alignas(16) std::uint8_t helper[HELPER_BYTES]{};
        float position[3]{};
        float normal[3]{};
        float negatedLeft[3]{};
        int kit = 0;
        int tint = 0;
        ULONGLONG placedAt = 0;
        float createdAt = -1.0f;                        // the game's clock when it was sprayed (-1: unknown)
    };

    // Main thread only. The helpers stay here for good (never freed): the picture's async load keeps a pointer to its helper.
    std::array<Spray, MAX_SPRAYS> g_sprays;
    ULONGLONG g_lastSprayAt = 0;

    std::atomic_bool g_shown{ true };
    std::atomic_bool g_clear{ false };
    std::atomic_bool g_shutdown{ false };
    std::atomic_bool g_shutdownDone{ false };
    std::atomic_int g_count{ 0 };

    void Log(const char* format, ...)
    {
        char line[256];
        va_list args;
        va_start(args, format);
        vsnprintf(line, sizeof(line), format, args);
        va_end(args);
        Unboxer::LogLine(line);
    }

    bool Finite(const float* v)
    {
        return std::isfinite(v[0]) && std::isfinite(v[1]) && std::isfinite(v[2]);
    }

    float Length(const float* v)
    {
        return std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    }

    // A helper as the game makes one: no kit (-1), creation time -1 (no paint-on animation), the rest empty.
    void ResetHelper(std::uint8_t* helper)
    {
        std::memset(helper, 0, HELPER_BYTES);
        const std::int32_t noKit = -1;
        const float never = -1.0f;
        std::memcpy(helper, &noKit, sizeof(noKit));
        std::memcpy(helper + 4, &never, sizeof(never));
    }

    // ── the game's functions, each behind SEH (no C++ objects in here) ──
    const char* CallSprayCheck(void* pawn, void* trace, float* forward, float* left, bool* crashed)
    {
        __try
        {
            *crashed = false;
            return g_check(pawn, trace, forward, left);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            *crashed = true;
            return nullptr;
        }
    }

    bool CallSetKit(void* helper, int kit)
    {
        __try
        {
            g_setKit(helper, kit);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    bool CallPlace(void* helper, const float* position, const float* normal, const float* negatedLeft, int tint)
    {
        __try
        {
            g_place(helper, position, normal, negatedLeft, tint, false);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    bool CallRelease(void* helper)
    {
        __try
        {
            g_release(helper);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    // The game's clock as Place reads it, false if it can't be read.
    bool ReadClock(float* now)
    {
        if (!g_globals)
            return false;

        __try
        {
            const auto* globals = static_cast<const std::uint8_t*>(*g_globals);

            if (!globals)
                return false;

            float value = 0.0f;
            std::memcpy(&value, globals + GLOBALS_CLOCK, sizeof(value));

            if (!std::isfinite(value) || value < 0.0f || value > 1.0e7f)
                return false;

            *now = value;
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    // Your pawn's simulation time: the fallback clock (the same time base, a tick or so apart).
    bool ReadSimulationTime(const std::uint8_t* pawn, float* now)
    {
        const std::uint32_t offset = SchemaFinder::GetCached<hash_32_fnv1a_const("C_BaseEntity->m_flSimulationTime")>();

        if (!pawn || !offset)
            return false;

        __try
        {
            float value = 0.0f;
            std::memcpy(&value, pawn + offset, sizeof(value));

            if (!std::isfinite(value) || value <= 0.0f || value > 1.0e7f)
                return false;

            *now = value;
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    // The game's own cooldown on your pawn (m_flNextSprayDecalTime), so the spray wheel shows the 45 s like after a real
    // spray. "Now" = your pawn's simulation time.
    void SetGameCooldown(std::uint8_t* pawn, std::uint32_t nextOffset, std::uint32_t simulationOffset)
    {
        __try
        {
            float now = 0.0f;
            std::memcpy(&now, pawn + simulationOffset, sizeof(now));

            if (now > 0.0f && now < 1.0e7f)
            {
                const float until = now + COOLDOWN_SECONDS;
                std::memcpy(pawn + nextOffset, &until, sizeof(until));
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
        }
    }

    void UpdateCount()
    {
        int count = 0;
        for (const Spray& spray : g_sprays)
            count += spray.active ? 1 : 0;
        g_count.store(count, std::memory_order_relaxed);
    }

    // Its decal comes down (the game's Release), and the slot is free again.
    void Remove(Spray& spray)
    {
        if (!spray.active)
            return;

        if (!CallRelease(spray.helper))
            Log("graffiti: taking one down crashed (signature?)");

        ResetHelper(spray.helper);
        spray.active = false;
        spray.broken = false;
    }

    void RemoveAll()
    {
        for (Spray& spray : g_sprays)
            Remove(spray);

        UpdateCount();
    }

    // Out of the match: the world (and the decals with it) is already gone - forget them without touching the game.
    void ForgetAll()
    {
        bool any = false;

        for (Spray& spray : g_sprays)
        {
            if (!spray.active)
                continue;

            ResetHelper(spray.helper);
            spray.active = false;
            spray.broken = false;
            any = true;
        }

        if (any)
            UpdateCount();
    }
}

bool LocalSpray::Initialize()
{
    g_check = reinterpret_cast<SprayCheckFn>(M::FindPattern("client.dll", Patterns::SprayCheck));
    g_setKit = reinterpret_cast<SetKitFn>(M::FindPattern("client.dll", Patterns::SpraySetKit));
    g_place = reinterpret_cast<PlaceFn>(M::FindPattern("client.dll", Patterns::SprayPlace));
    g_release = reinterpret_cast<ReleaseFn>(M::FindPattern("client.dll", Patterns::SprayRelease));

    // mov rax, [rip + disp32]: the variable holding the game's globals pointer
    if (const std::uint8_t* clock = M::FindPattern("client.dll", Patterns::SprayClock))
    {
        std::int32_t displacement = 0;
        std::memcpy(&displacement, clock + 3, sizeof(displacement));
        g_globals = reinterpret_cast<void* const*>(clock + 7 + displacement);
    }

    if (!g_globals)
        Log("graffiti: the game's clock wasn't found (SprayClock) - your pawn's time is used instead");

    for (Spray& spray : g_sprays)
        ResetHelper(spray.helper);

    if (!g_check || !g_setKit || !g_place || !g_release)
    {
        Log("graffiti: NOT found - check %s, kit %s, place %s, release %s", g_check ? "ok" : "missing", g_setKit ? "ok" : "missing",
            g_place ? "ok" : "missing", g_release ? "ok" : "missing");
        return false;
    }

    return true;
}

void LocalSpray::SetOptions(bool shown)
{
    g_shown.store(shown, std::memory_order_relaxed);
}

bool LocalSpray::OnSprayAction(int slot)
{
    int kit = 0, tint = 0;

    if (!Unboxer::EquippedGraffiti(slot, kit, tint))
        return false;   // a real graffiti (or none): the game's own spray

    // From here on it's a Lootline graffiti: the game's spray never runs for it.
    if (!g_shown.load(std::memory_order_relaxed) || !g_check || !g_setKit || !g_place || !g_release)
        return true;

    const ULONGLONG now = GetTickCount64();

    if (g_lastSprayAt != 0 && now - g_lastSprayAt < static_cast<ULONGLONG>(COOLDOWN_SECONDS * 1000.0f))
    {
        Log("graffiti: cooling down - %llu s left (45 s between sprays, like CS2)",
            static_cast<unsigned long long>((static_cast<ULONGLONG>(COOLDOWN_SECONDS * 1000.0f) - (now - g_lastSprayAt) + 999) / 1000));
        return true;
    }

    auto* pawn = reinterpret_cast<std::uint8_t*>(I::GetLocalPawn());

    if (!pawn)
        return true;

    alignas(16) std::uint8_t trace[TRACE_BYTES]{};
    float forward[3]{}, left[3]{};
    bool crashed = false;
    const char* error = CallSprayCheck(pawn, trace, forward, left, &crashed);

    if (crashed)
    {
        Log("graffiti: the game's spray check crashed (signature?) - nothing sprayed");
        return true;
    }

    if (error)
    {
        Log("graffiti: can't spray there (%s)", error);
        return true;
    }

    float position[3], normal[3];
    std::memcpy(position, trace + TRACE_END, sizeof(position));
    std::memcpy(normal, trace + TRACE_NORMAL, sizeof(normal));

    if (!Finite(position) || !Finite(normal) || !Finite(left) || std::fabs(Length(normal) - 1.0f) > 0.1f || Length(left) < 0.5f)
    {
        Log("graffiti: the spray check gave no wall to spray on - nothing sprayed");
        return true;
    }

    // a free slot, else the oldest decal comes down for it
    Spray* target = nullptr;

    for (Spray& spray : g_sprays)
    {
        if (!spray.active)
        {
            target = &spray;
            break;
        }
    }

    if (!target)
    {
        target = &g_sprays[0];

        for (Spray& spray : g_sprays)
            if (spray.placedAt < target->placedAt)
                target = &spray;

        Remove(*target);
    }

    ResetHelper(target->helper);

    if (!CallSetKit(target->helper, kit))
    {
        Log("graffiti: loading its picture crashed (signature?) - nothing sprayed");
        ResetHelper(target->helper);
        return true;
    }

    // sprayed now, by the game's clock: it paints in, and fades after ~6 minutes like CS2's own graffiti
    float createdAt = -1.0f;

    if (!ReadClock(&createdAt) && !ReadSimulationTime(pawn, &createdAt))
    {
        createdAt = -1.0f;
        Log("graffiti: the game's clock couldn't be read - the decal may not show");
    }

    std::memcpy(target->helper + HELPER_CREATION_TIME, &createdAt, sizeof(createdAt));
    target->createdAt = createdAt;

    std::memcpy(target->position, position, sizeof(position));
    std::memcpy(target->normal, normal, sizeof(normal));
    for (int k = 0; k < 3; ++k)
        target->negatedLeft[k] = -left[k];   // what the game hands Place (its preview and C_PlayerSprayDecal do the same)

    target->kit = kit;
    target->tint = tint;
    target->placedAt = now;
    target->broken = false;
    target->active = true;
    g_lastSprayAt = now;
    UpdateCount();

    // the spray wheel shows the cooldown like after a real spray
    const std::uint32_t nextOffset = SchemaFinder::GetCached<hash_32_fnv1a_const("C_CSPlayerPawn->m_flNextSprayDecalTime")>();
    const std::uint32_t simulationOffset = SchemaFinder::GetCached<hash_32_fnv1a_const("C_BaseEntity->m_flSimulationTime")>();

    if (nextOffset && simulationOffset)
        SetGameCooldown(pawn, nextOffset, simulationOffset);

    Log("graffiti: sprayed on your screen only (kit %d, colour %d) at %.0f %.0f %.0f, game clock %.1f", kit, tint, position[0], position[1],
        position[2], createdAt);

    Unboxer::OnGraffitiSprayed(slot);   // the spray sound, one charge used
    return true;
}

void LocalSpray::MainThreadTick()
{
    if (g_shutdown.load(std::memory_order_acquire))
    {
        if (!g_shutdownDone.load(std::memory_order_acquire))
        {
            const bool inGame = I::EngineClient && I::EngineClient->connected() && I::EngineClient->in_game();

            if (inGame)
                RemoveAll();
            else
                ForgetAll();

            g_shutdownDone.store(true, std::memory_order_release);
        }

        return;
    }

    if (!g_place)
        return;

    const bool inGame = I::EngineClient && I::EngineClient->connected() && I::EngineClient->in_game();

    if (!inGame)
    {
        ForgetAll();
        return;
    }

    if (g_clear.exchange(false) || !g_shown.load(std::memory_order_relaxed))
        RemoveAll();

    float now = 0.0f;
    const bool haveClock = ReadClock(&now);

    // every frame, like the game does for its own graffiti: the decal stays where it was sprayed
    for (Spray& spray : g_sprays)
    {
        if (!spray.active)
            continue;

        // faded out (CS2's graffiti are gone after 6 minutes), or the clock started over (a new map / match): taken down
        if (haveClock && spray.createdAt >= 0.0f && (now > spray.createdAt + GONE_AFTER_SECONDS || now + 5.0f < spray.createdAt))
        {
            Remove(spray);
            UpdateCount();
            continue;
        }

        if (spray.broken)
            continue;

        // still loading its picture (Place skips the decal while [helper + 0x28] is set): its creation time moves along
        // with the clock, so the paint-in (the first 1.5 s) starts when it's first drawn - not while it was loading unseen
        if (haveClock && spray.helper[HELPER_LOADING] != 0)
        {
            spray.createdAt = now;
            std::memcpy(spray.helper + HELPER_CREATION_TIME, &now, sizeof(now));
        }

        if (!CallPlace(spray.helper, spray.position, spray.normal, spray.negatedLeft, spray.tint))
        {
            spray.broken = true;
            Log("graffiti: placing the decal crashed (signature?) - that one is left alone");
        }
    }
}

void LocalSpray::LevelShutdown()
{
    if (g_release)
        RemoveAll();
}

void LocalSpray::Clear()
{
    g_clear.store(true);
}

int LocalSpray::Count()
{
    return g_count.load(std::memory_order_relaxed);
}

void LocalSpray::Shutdown(unsigned timeoutMs)
{
    g_shutdown.store(true, std::memory_order_release);

    if (g_count.load(std::memory_order_relaxed) == 0)
        return;

    for (unsigned waited = 0; waited < timeoutMs && !g_shutdownDone.load(std::memory_order_acquire); waited += 5)
        Sleep(5);
}
