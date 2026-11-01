#include "movement.h"

#include <Windows.h>
#include <timeapi.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <mutex>

#include "../../../patterns.h"
#include "../../chadware2.h"
#include "../../config/config.h"
#include "../../interfaces/interfaces.h"
#include "../../menu/menu.h"
#include "../../menu/notify.h"
#include "../../utils/memory/patternscan/patternscan.h"
#include "../../utils/offsets/offsets.h"
#include "../../utils/schema/schema.h"
#include "../../../cs2/entity/C_CSPlayerPawn/C_CSPlayerPawn.h"

#pragma comment(lib, "winmm.lib")   // timeBeginPeriod: 1 ms sleeps for the movement thread

namespace
{
    // client.dll button state values: 65537 = +jump (down + "pressed this frame"), 256 = -jump.
    constexpr std::uint32_t JUMP_PRESS = 65537;
    constexpr std::uint32_t JUMP_RELEASE = 256;
    constexpr std::uint32_t FL_ONGROUND = 1u << 0;

    // Offset of the state the game reads inside an input button (what cs2-dumper lists as buttons.*).
    constexpr std::ptrdiff_t BUTTON_STATE = 0x30;

    // A perfect hop jumps on the first ground tick, so ground friction never runs and no speed is lost. One late
    // tick costs ~8% (sv_friction 5.2 at 64 tick), so keeping 97%+ of the landing speed counts as perfect.
    constexpr float PERFECT_KEEP = 0.97f;
    constexpr float SLOW_SPEED = 30.0f;         // below this friction barely shows: judge by time instead
    constexpr double PERFECT_MS = 24.0;         // ~1.5 ticks at 64 tick
    constexpr double CHAIN_BREAK_MS = 250.0;    // longer on the ground = you stopped hopping

    constexpr int FOV_MIN = 60;
    constexpr int FOV_MAX = 150;

    constexpr std::int32_t WEAPONTYPE_SNIPER_RIFLE = 5;   // CCSWeaponBaseVData::m_WeaponType: AWP, SSG 08, SCAR-20, G3SG1

    struct Player
    {
        std::uint8_t* pawn = nullptr;
        bool alive = false;
        std::uint32_t flags = 0;
        float velocity[3]{};
        std::uint8_t* camera = nullptr;     // CCSPlayer_CameraServices*
        std::uint32_t fov = 0;
        bool scoped = false;
    };

    // ── state ──
    // The bunnyhop and the speed meter run on their own 1 ms thread (like the standalone bhop): once per frame is too
    // late, the jump lands a frame after touching the ground and friction eats the speed. The game thread only
    // publishes the pawn and does the FOV.

    std::atomic_bool g_ShutdownRequested{ false };
    std::atomic_bool g_ShutdownDone{ false };
    std::atomic_int g_JumpLookup{ static_cast<int>(Movement::ButtonLookup::Pending) };
    std::atomic_bool g_ResetStats{ false };
    std::atomic<std::uint8_t*> g_Pawn{ nullptr };      // local pawn while in game (game thread -> 1 ms thread)
    std::atomic_bool g_ThreadStarted{ false };
    HANDLE g_Thread = nullptr;

    // 1 ms thread only
    std::uint32_t* g_JumpState = nullptr;
    unsigned long long g_NextJumpLookup = 0;
    bool g_Holding = false;                 // we pressed +jump and haven't released it yet

    // FOV hook (H::hkGetFov): your camera services and whether you're scoped, published at FRAME_RENDER_START /
    // END (the view is set up right after RENDER_START). The hook only compares the pointer, never reads through it.
    std::atomic_bool g_FovHookActive{ false };
    std::atomic<void*> g_FovLocalCamera{ nullptr };
    std::atomic_bool g_FovLocalScoped{ false };

    // Crosshair on snipers (H::hkShouldDrawCrosshair): your active sniper while it should show, published by the game
    // thread every frame (null = none). The hook only compares the pointer, never reads through it.
    std::atomic_bool g_SniperHookActive{ false };
    std::atomic<const void*> g_SniperCrosshairWeapon{ nullptr };

    int DesiredFov()
    {
        return std::clamp(static_cast<int>(std::lround(Config::menu.fov)), FOV_MIN, FOV_MAX);
    }

    // game thread only
    bool g_FovApplied = false;
    std::uint8_t* g_FovCamera = nullptr;
    std::uint32_t g_FovOriginal = 0;        // what the game had before we touched it (usually 0 = default)
    std::uint32_t g_FovWritten = 0;

    std::mutex g_StatsMutex;
    Movement::Stats g_Stats;                // guarded by g_StatsMutex

    struct HopTracker
    {
        bool tracking = false;
        bool wasGround = false;
        bool landed = false;                // came down from the air, a hop can follow
        double landMs = 0.0;
        float landSpeed = 0.0f;             // speed on the last air frame before landing
        float lastAirSpeed = 0.0f;
        float prevTakeoff = 0.0f;
        bool haveTakeoff = false;
    } g_Hop;

    // ── helpers ──

    bool SafeCopy(void* destination, const void* source, std::size_t size)
    {
        __try
        {
            std::memcpy(destination, source, size);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    double NowMs()
    {
        static LARGE_INTEGER frequency{};
        LARGE_INTEGER now{};
        if (!frequency.QuadPart)
            QueryPerformanceFrequency(&frequency);
        QueryPerformanceCounter(&now);
        return static_cast<double>(now.QuadPart) * 1000.0 / static_cast<double>(frequency.QuadPart);
    }

    bool GameFocused()
    {
        DWORD pid = 0;
        GetWindowThreadProcessId(GetForegroundWindow(), &pid);
        return pid == GetCurrentProcessId();
    }

    // You're typing in CS2 (the chat, the console, any text box): CS2 runs on SDL3, which only delivers typed text
    // while "text input" is switched on - CS2 switches it on when a text box gets the keyboard and off again after.
    // SDL3.dll's own exports, any thread (it reads a flag). Not found: never "typing".
    using SdlGetKeyboardFocusFn = void* (*)();
    using SdlTextInputActiveFn = bool (*)(void*);
    std::atomic<SdlGetKeyboardFocusFn> g_SdlKeyboardFocus{ nullptr };
    std::atomic<SdlTextInputActiveFn> g_SdlTextInputActive{ nullptr };
    std::atomic_bool g_SdlResolved{ false };

    bool TypingInGame()
    {
        if (!g_SdlResolved.load(std::memory_order_acquire))
        {
            if (const HMODULE sdl = GetModuleHandleW(L"SDL3.dll"))
            {
                g_SdlKeyboardFocus.store(reinterpret_cast<SdlGetKeyboardFocusFn>(GetProcAddress(sdl, "SDL_GetKeyboardFocus")));
                g_SdlTextInputActive.store(reinterpret_cast<SdlTextInputActiveFn>(GetProcAddress(sdl, "SDL_TextInputActive")));
            }

            g_SdlResolved.store(true, std::memory_order_release);
        }

        const SdlGetKeyboardFocusFn focus = g_SdlKeyboardFocus.load();
        const SdlTextInputActiveFn active = g_SdlTextInputActive.load();

        if (!focus || !active)
            return false;

        void* window = focus();
        return window && active(window);
    }

    template <typename T>
    T& At(void* base, std::uint32_t offset)
    {
        return *reinterpret_cast<T*>(static_cast<std::uint8_t*>(base) + offset);
    }

    // CS2 registers every input button at startup like this (one block per button):
    //   xor r8d, r8d / lea rdx, "jump" / lea rcx, [button + name] / call ctor
    //   mov rax, [head] / lea rcx, dtor / mov [button + next], rax / lea rax, [button] / mov [head], rax
    // Any one block gives the list head and where name / next sit, so walking the list by name survives updates.
    std::uint32_t* FindButtonState(const char* wanted)
    {
        std::uint8_t* match = M::scan("client.dll", Patterns::ButtonRegistration);
        if (!match)
            return nullptr;

        // every rip-relative instruction used here is 7 bytes with the displacement in its last 4
        const auto target = [](std::uint8_t* instruction) { return instruction + 7 + *reinterpret_cast<std::int32_t*>(instruction + 3); };
        std::uint8_t* nameField = target(match + 10);
        std::uint8_t* head = target(match + 22);
        std::uint8_t* nextField = target(match + 36);
        std::uint8_t* button = target(match + 43);

        const std::ptrdiff_t nameOffset = nameField - button;
        const std::ptrdiff_t nextOffset = nextField - button;
        if (nameOffset < 0 || nameOffset > 0x100 || nextOffset <= BUTTON_STATE || nextOffset > 0x400)
            return nullptr;

        std::uint8_t* node = nullptr;
        if (!SafeCopy(&node, head, sizeof(node)))
            return nullptr;

        for (int i = 0; node && i < 512; ++i)
        {
            const char* name = nullptr;
            char text[32]{};

            if (SafeCopy(&name, node + nameOffset, sizeof(name)) && name &&
                SafeCopy(text, name, sizeof(text) - 1) && std::strcmp(text, wanted) == 0)
                return reinterpret_cast<std::uint32_t*>(node + BUTTON_STATE);

            if (!SafeCopy(&node, node + nextOffset, sizeof(node)))
                break;
        }

        return nullptr;
    }

    std::uint32_t* JumpState()
    {
        if (!g_JumpState && GetTickCount64() >= g_NextJumpLookup)
        {
            g_JumpState = FindButtonState("jump");

            // the registration signature broke: buttons.jump from a fresh offsets\buttons.json
            if (!g_JumpState)
                g_JumpState = reinterpret_cast<std::uint32_t*>(Offsets::FreshButton("jump"));

            g_NextJumpLookup = GetTickCount64() + 5000;
            g_JumpLookup.store(static_cast<int>(g_JumpState ? Movement::ButtonLookup::Found : Movement::ButtonLookup::NotFound));
        }
        return g_JumpState;
    }

    Player ReadPlayer()
    {
        Player p;

        if (!I::EngineClient || !I::EngineClient->connected() || !I::EngineClient->in_game())
            return p;

        C_CSPlayerPawn* pawn = I::GetLocalPawn();
        if (!pawn)
            return p;

        const std::uint32_t flagsOffset = SchemaFinder::GetCached<hash_32_fnv1a_const("C_BaseEntity->m_fFlags")>();
        const std::uint32_t velocityOffset = SchemaFinder::GetCached<hash_32_fnv1a_const("C_BaseEntity->m_vecAbsVelocity")>();
        const std::uint32_t cameraOffset = SchemaFinder::GetCached<hash_32_fnv1a_const("C_BasePlayerPawn->m_pCameraServices")>();
        const std::uint32_t scopedOffset = SchemaFinder::GetCached<hash_32_fnv1a_const("C_CSPlayerPawn->m_bIsScoped")>();
        const std::uint32_t fovOffset = SchemaFinder::GetCached<hash_32_fnv1a_const("CCSPlayerBase_CameraServices->m_iFOV")>();

        if (!flagsOffset || !velocityOffset)
            return p;

        p.pawn = reinterpret_cast<std::uint8_t*>(pawn);
        p.alive = pawn->m_iHealth() > 0;
        p.flags = At<std::uint32_t>(pawn, flagsOffset);
        std::memcpy(p.velocity, &At<float>(pawn, velocityOffset), sizeof(p.velocity));

        if (cameraOffset && fovOffset)
        {
            p.camera = At<std::uint8_t*>(pawn, cameraOffset);
            if (p.camera)
                p.fov = At<std::uint32_t>(p.camera, fovOffset);
        }

        if (scopedOffset)
            p.scoped = At<bool>(pawn, scopedOffset);

        return p;
    }

    // 1 ms thread: the pawn can go away between frames (map change), so every read here is guarded.
    Player ReadFromThread()
    {
        Player p;
        std::uint8_t* pawn = g_Pawn.load(std::memory_order_acquire);
        if (!pawn)
            return p;

        const std::uint32_t healthOffset = SchemaFinder::GetCached<hash_32_fnv1a_const("C_BaseEntity->m_iHealth")>();
        const std::uint32_t flagsOffset = SchemaFinder::GetCached<hash_32_fnv1a_const("C_BaseEntity->m_fFlags")>();
        const std::uint32_t velocityOffset = SchemaFinder::GetCached<hash_32_fnv1a_const("C_BaseEntity->m_vecAbsVelocity")>();

        std::int32_t health = 0;
        if (!healthOffset || !flagsOffset || !velocityOffset ||
            !SafeCopy(&health, pawn + healthOffset, sizeof(health)) ||
            !SafeCopy(&p.flags, pawn + flagsOffset, sizeof(p.flags)) ||
            !SafeCopy(p.velocity, pawn + velocityOffset, sizeof(p.velocity)))
            return Player{};

        p.pawn = pawn;
        p.alive = health > 0;
        return p;
    }

    // Your active weapon if it's a sniper rifle and its crosshair should show (alive, not scoped, option on), else null.
    // Its VData sits right after m_nSubclassID (C_CSWeaponBase "should draw crosshair" reads it at weapon + 0x388).
    const void* UnscopedSniper(const Player& p)
    {
        if (!Config::menu.sniperCrosshair || !p.pawn || !p.alive || p.scoped || !I::GameEntity || !I::GameEntity->Instance)
            return nullptr;

        const std::uint32_t subclassOffset = SchemaFinder::GetCached<hash_32_fnv1a_const("C_BaseEntity->m_nSubclassID")>();
        const std::uint32_t typeOffset = SchemaFinder::GetCached<hash_32_fnv1a_const("CCSWeaponBaseVData->m_WeaponType")>();

        if (!subclassOffset || !typeOffset)
            return nullptr;

        CCSPlayer_WeaponServices* services = reinterpret_cast<C_CSPlayerPawn*>(p.pawn)->m_pWeaponServices();

        if (!services)
            return nullptr;

        const CBaseHandle handle = services->m_hActiveWeapon();
        C_CSWeaponBase* weapon = handle.valid() ? I::GameEntity->Instance->Get<C_CSWeaponBase>(handle.index()) : nullptr;

        if (!weapon)
            return nullptr;

        std::uint8_t* vdata = nullptr;
        std::int32_t type = -1;

        if (!SafeCopy(&vdata, reinterpret_cast<std::uint8_t*>(weapon) + subclassOffset + 8, sizeof(vdata)) || !vdata ||
            !SafeCopy(&type, vdata + typeOffset, sizeof(type)))
            return nullptr;

        return type == WEAPONTYPE_SNIPER_RIFLE ? weapon : nullptr;
    }

    // ── bunnyhop ──

    void ReleaseJump()
    {
        if (!g_Holding)
            return;

        if (g_JumpState)
            *g_JumpState = JUMP_RELEASE;
        g_Holding = false;
    }

    void RunBhop(const Player& p)
    {
        const bool wanted = Config::menu.bhop && p.alive && !chadware.renderer.menu.showMenu && GameFocused() && !TypingInGame() &&
            (GetAsyncKeyState(VK_SPACE) & 0x8000) != 0;

        std::uint32_t* state = JumpState();

        if (!wanted || !state)
        {
            ReleaseJump();
            return;
        }

        // On the ground: keep re-pressing +jump so the landing tick jumps. In the air: -jump, so the next landing is
        // a fresh press.
        *state = (p.flags & FL_ONGROUND) ? JUMP_PRESS : JUMP_RELEASE;
        g_Holding = true;
    }

    // ── fov ──

    void RestoreFov()
    {
        if (!g_FovApplied)
            return;

        g_FovApplied = false;

        const std::uint32_t fovOffset = SchemaFinder::GetCached<hash_32_fnv1a_const("CCSPlayerBase_CameraServices->m_iFOV")>();
        const std::uint32_t startOffset = SchemaFinder::GetCached<hash_32_fnv1a_const("CCSPlayerBase_CameraServices->m_iFOVStart")>();
        if (!fovOffset || !g_FovCamera)
            return;

        // Only undo our own value; if the game already changed it (scope, respawn) leave it be.
        std::uint32_t now = 0;
        if (SafeCopy(&now, g_FovCamera + fovOffset, sizeof(now)) && now == g_FovWritten)
        {
            SafeCopy(g_FovCamera + fovOffset, &g_FovOriginal, sizeof(g_FovOriginal));
            if (startOffset)
                SafeCopy(g_FovCamera + startOffset, &g_FovOriginal, sizeof(g_FovOriginal));
        }
    }

    // The camera's FOV is m_iFOV, but while a transition runs (weapon switch, unscoping) the game draws a blend
    // from m_iFOVStart to m_iFOV over m_flFOVRate seconds — and weapons with a zoom (AUG, SG 553) reset the FOV
    // every tick. Writing only m_iFOV once per frame lost to both: the switch animation and those rifles showed the
    // default FOV. So the start of the blend is pinned to our FOV too (the blend then goes ours -> ours), and this
    // runs at FRAME_RENDER_START as well, after the tick that reset it and right before the frame is drawn.
    void RunFov(const Player& p)
    {
        g_FovLocalCamera.store(p.alive ? p.camera : nullptr, std::memory_order_release);
        g_FovLocalScoped.store(p.scoped, std::memory_order_release);

        // The GetFOV hook does it without writing anything into the game (and covers the weapon-switch blend).
        if (!Config::menu.fovChanger || g_FovHookActive.load(std::memory_order_acquire))
        {
            RestoreFov();
            return;
        }

        const std::uint32_t fovOffset = SchemaFinder::GetCached<hash_32_fnv1a_const("CCSPlayerBase_CameraServices->m_iFOV")>();
        const std::uint32_t startOffset = SchemaFinder::GetCached<hash_32_fnv1a_const("CCSPlayerBase_CameraServices->m_iFOVStart")>();
        const std::uint32_t desired = static_cast<std::uint32_t>(DesiredFov());

        // Scoped: the game drives m_iFOV for the zoom levels, leave it alone.
        if (!fovOffset || !p.alive || !p.camera || p.scoped)
            return;

        const bool startWrong = startOffset && At<std::uint32_t>(p.camera, startOffset) != desired;

        if (p.fov == desired && !startWrong)
            return;

        if (!g_FovApplied || p.camera != g_FovCamera)
        {
            g_FovOriginal = p.fov == desired ? 0 : p.fov;
            g_FovCamera = p.camera;
        }

        At<std::uint32_t>(p.camera, fovOffset) = desired;
        if (startOffset)
            At<std::uint32_t>(p.camera, startOffset) = desired;

        g_FovWritten = desired;
        g_FovApplied = true;
    }

    // ── speed meter ──

    void RegisterHop(Movement::Stats& s, float takeoff, double groundMs)
    {
        const bool perfect = g_Hop.landSpeed < SLOW_SPEED ? groundMs <= PERFECT_MS : takeoff >= g_Hop.landSpeed * PERFECT_KEEP;

        s.haveHop = true;
        s.lastTakeoff = takeoff;
        s.lastGain = g_Hop.haveTakeoff ? takeoff - g_Hop.prevTakeoff : 0.0f;
        s.lastGroundMs = static_cast<float>(groundMs);
        s.lastPerfect = perfect;
        s.lastHopAt = GetTickCount64();
        s.hops++;

        if (perfect)
        {
            s.perfect++;
            s.bestStreak = std::max(s.bestStreak, ++s.streak);
        }
        else
        {
            s.streak = 0;
        }
    }

    void UpdateStats(const Player& p)
    {
        std::lock_guard<std::mutex> lock(g_StatsMutex);
        Movement::Stats& s = g_Stats;

        if (g_ResetStats.exchange(false))
        {
            s = Movement::Stats{};
            g_Hop.haveTakeoff = false;
        }

        s.alive = p.alive;

        if (!p.alive)
        {
            g_Hop.tracking = false;
            s.speed = 0.0f;
            return;
        }

        const float speed = std::hypot(p.velocity[0], p.velocity[1]);
        const bool ground = (p.flags & FL_ONGROUND) != 0;
        const double now = NowMs();

        s.speed = speed;
        s.topSpeed = std::max(s.topSpeed, speed);

        if (!g_Hop.tracking)   // (re)spawned: no fake landing / takeoff
        {
            g_Hop = HopTracker{};
            g_Hop.tracking = true;
            g_Hop.wasGround = ground;
            return;
        }

        if (ground && !g_Hop.wasGround)             // landed
        {
            g_Hop.landed = true;
            g_Hop.landMs = now;
            g_Hop.landSpeed = g_Hop.lastAirSpeed;
        }
        else if (!ground && g_Hop.wasGround)        // took off
        {
            if (g_Hop.landed)
            {
                const double groundMs = now - g_Hop.landMs;
                if (groundMs <= CHAIN_BREAK_MS)
                    RegisterHop(s, speed, groundMs);
                else
                    s.streak = 0;
            }

            g_Hop.landed = false;
            g_Hop.prevTakeoff = speed;
            g_Hop.haveTakeoff = true;
        }

        if (!ground)
            g_Hop.lastAirSpeed = speed;
        g_Hop.wasGround = ground;
    }

    // what the game thread knows (camera FOV / scope), for the menu
    void PublishView(const Player& p)
    {
        std::lock_guard<std::mutex> lock(g_StatsMutex);
        g_Stats.gameFov = static_cast<int>(p.fov);
        g_Stats.scoped = p.scoped;
    }

    // ── 1 ms thread ──

    DWORD WINAPI MovementThread(LPVOID)
    {
        timeBeginPeriod(1);

        while (!g_ShutdownRequested.load(std::memory_order_acquire))
        {
            const Player player = ReadFromThread();
            RunBhop(player);
            UpdateStats(player);
            Sleep(1);
        }

        ReleaseJump();
        timeEndPeriod(1);
        return 0;
    }

    void StartThreadOnce()
    {
        if (!g_ThreadStarted.exchange(true))
            g_Thread = CreateThread(nullptr, 0, MovementThread, nullptr, 0, nullptr);
    }

    // ── render thread ──

    bool KeyPressed(int vk)
    {
        static bool wasDown[256]{};
        const bool down = (GetAsyncKeyState(vk) & 0x8000) != 0;
        const bool pressed = down && !wasDown[vk];
        wasDown[vk] = down;
        return pressed;
    }

    void Toggle(bool& value, const char* on, const char* off)
    {
        value = !value;
        MenuState::MarkDirty();
        Notify::Push(tr(value ? on : off));
    }

    ImU32 Lerp(ImU32 a, ImU32 b, float t)
    {
        const auto channel = [&](int shift)
        {
            const float x = static_cast<float>((a >> shift) & 0xFF), y = static_cast<float>((b >> shift) & 0xFF);
            return static_cast<ImU32>(x + (y - x) * t) << shift;
        };
        return channel(IM_COL32_R_SHIFT) | channel(IM_COL32_G_SHIFT) | channel(IM_COL32_B_SHIFT) | channel(IM_COL32_A_SHIFT);
    }

    void DrawOutlinedText(ImDrawList* draw, ImFont* font, float size, const ImVec2& center, ImU32 color, const char* text)
    {
        const ImVec2 textSize = font->CalcTextSizeA(size, FLT_MAX, 0.0f, text);
        const ImVec2 pos(std::floor(center.x - textSize.x * 0.5f), std::floor(center.y));
        const float shadow = std::max(1.0f, size / 18.0f);
        draw->AddText(font, size, ImVec2(pos.x + shadow, pos.y + shadow), IM_COL32(0, 0, 0, (color >> IM_COL32_A_SHIFT) * 3 / 4), text);
        draw->AddText(font, size, pos, color, text);
    }

    // Speed under the crosshair; flashes green / red for the speed you gained / lost on a hop.
    void DrawSpeed(const Movement::Stats& s)
    {
        if (!Config::menu.speedHud || !s.alive)
            return;

        const ImVec2 display = ImGui::GetIO().DisplaySize;
        const float scale = std::clamp(display.y / 1080.0f, 0.5f, 3.0f);
        ImFont* bold = g_FontBold ? g_FontBold : ImGui::GetFont();
        ImFont* regular = g_FontRegular ? g_FontRegular : ImGui::GetFont();
        ImDrawList* draw = ImGui::GetBackgroundDrawList();

        const float since = s.haveHop ? static_cast<float>(GetTickCount64() - s.lastHopAt) / 1000.0f : 99.0f;
        const float flash = std::clamp(1.0f - since / 0.8f, 0.0f, 1.0f);
        const ImU32 white = IM_COL32(245, 245, 250, 235);
        const ImU32 hopColor = s.lastGain >= 0.0f ? IM_COL32(90, 230, 120, 255) : IM_COL32(255, 95, 95, 255);

        char speed[16];
        snprintf(speed, sizeof(speed), "%.0f", s.speed);
        const ImVec2 center(display.x * 0.5f, display.y * 0.74f);
        const float speedSize = 34.0f * scale;
        DrawOutlinedText(draw, bold, speedSize, center, Lerp(white, hopColor, flash), speed);

        // last hop: "+12  PERFECT", fades out
        const float fade = std::clamp(1.0f - (since - 1.2f) / 0.6f, 0.0f, 1.0f);
        if (s.haveHop && fade > 0.0f)
        {
            char line[48];
            snprintf(line, sizeof(line), "%+.0f  %s", s.lastGain, s.lastPerfect ? tr("PERFECT") : tr("late"));
            const ImU32 color = s.lastPerfect ? IM_COL32(90, 230, 120, 255) : IM_COL32(255, 160, 70, 255);
            const ImU32 faded = (color & ~IM_COL32_A_MASK) | (static_cast<ImU32>(fade * 230.0f) << IM_COL32_A_SHIFT);
            DrawOutlinedText(draw, regular, 17.0f * scale, ImVec2(center.x, center.y + speedSize * 1.05f), faded, line);
        }
    }
}

void Movement::RunGameThread()
{
    if (g_ShutdownRequested.load(std::memory_order_acquire))
    {
        g_Pawn.store(nullptr, std::memory_order_release);
        g_SniperCrosshairWeapon.store(nullptr, std::memory_order_release);
        RestoreFov();
        g_ShutdownDone.store(true, std::memory_order_release);
        return;
    }

    StartThreadOnce();

    const Player player = ReadPlayer();
    g_Pawn.store(player.pawn, std::memory_order_release);   // for the 1 ms bunnyhop / speed thread
    RunFov(player);
    PublishView(player);
    g_SniperCrosshairWeapon.store(g_SniperHookActive.load(std::memory_order_acquire) ? UnscopedSniper(player) : nullptr,
        std::memory_order_release);
}

void Movement::RunFovBeforeRender()
{
    if (g_ShutdownRequested.load(std::memory_order_acquire) || !Config::menu.fovChanger)
        return;

    RunFov(ReadPlayer());
}

float Movement::OverrideFov(void* cameraServices, float gameFov)
{
    if (!Config::menu.fovChanger || g_ShutdownRequested.load(std::memory_order_acquire))
        return gameFov;

    // Only your own camera, and not while scoped: the zoom levels are the game's.
    if (!cameraServices || cameraServices != g_FovLocalCamera.load(std::memory_order_acquire) ||
        g_FovLocalScoped.load(std::memory_order_acquire))
        return gameFov;

    return static_cast<float>(DesiredFov());
}

void Movement::SetFovHookActive(bool active)
{
    g_FovHookActive.store(active, std::memory_order_release);
}

bool Movement::ShowSniperCrosshair(const void* weapon)
{
    return weapon && weapon == g_SniperCrosshairWeapon.load(std::memory_order_acquire);
}

void Movement::SetSniperCrosshairHookActive(bool active)
{
    g_SniperHookActive.store(active, std::memory_order_release);
}

bool Movement::SniperCrosshairAvailable()
{
    return g_SniperHookActive.load(std::memory_order_acquire);
}

bool Movement::RenderThreadTick()
{
    // the hotkeys (Misc / Settings, F6 / F9 / none by default) while you're in the game - the menu is part of it, so
    // this also works with the menu open; not while a "Key" button waits for one, and not while you type (CS2's chat
    // or console, a text box in this menu: the key is a letter there). The keys are still read, so a key held while
    // typing doesn't fire the moment you stop.
    bool reapply = false;
    const bool typing = TypingInGame() || ImGui::GetIO().WantTextInput;

    if (GameFocused() && MenuState::capturingKey.load() == MenuState::KeyNone)
    {
        const auto pressed = [typing](int vk) { return vk > 0 && vk < 256 && KeyPressed(vk) && !typing; };

        if (pressed(Config::menu.bhopKey))
            Toggle(Config::menu.bhop, "Bunnyhop ON", "Bunnyhop OFF");
        if (pressed(Config::menu.fovKey))
            Toggle(Config::menu.fovChanger, "FOV changer ON", "FOV changer OFF");
        reapply = pressed(Config::menu.reapplyKey);
    }

    DrawSpeed(GetStats());
    return reapply;
}

void Movement::Shutdown()
{
    // The 1 ms thread sees this, lets go of +jump and exits. The FOV is given back by the game thread, which runs
    // FrameStageNotify every frame, so that normally happens within a frame or two.
    g_ShutdownRequested.store(true, std::memory_order_release);

    for (int i = 0; i < 50 && !g_ShutdownDone.load(std::memory_order_acquire); ++i)
        Sleep(10);

    if (g_Thread)
    {
        WaitForSingleObject(g_Thread, 3000);
        CloseHandle(g_Thread);
        g_Thread = nullptr;
    }
}

Movement::Stats Movement::GetStats()
{
    std::lock_guard<std::mutex> lock(g_StatsMutex);
    return g_Stats;
}

void Movement::ResetStats()
{
    g_ResetStats.store(true);
}

Movement::ButtonLookup Movement::JumpButton()
{
    return static_cast<ButtonLookup>(g_JumpLookup.load());
}
