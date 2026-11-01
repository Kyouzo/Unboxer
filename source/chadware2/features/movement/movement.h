#pragma once

#include <cstdint>

// Misc tab: bunnyhop, camera FOV changer, the speed meter and your crosshair on snipers.
// RunGameThread() does every game read / write from FrameStageNotify; the menu and the on-screen speed only read a
// snapshot (GetStats) and flip Config::menu.
namespace Movement
{
    struct Stats
    {
        bool  alive = false;
        float speed = 0.0f;             // horizontal u/s right now
        float topSpeed = 0.0f;
        bool  haveHop = false;          // the "last*" fields are valid
        float lastTakeoff = 0.0f;       // horizontal speed leaving the ground
        float lastGain = 0.0f;          // vs the takeoff before it
        float lastGroundMs = 0.0f;      // time on the ground before that hop
        bool  lastPerfect = false;
        unsigned long long lastHopAt = 0;   // GetTickCount64 of the last hop (on-screen flash)
        int   hops = 0;
        int   perfect = 0;
        int   streak = 0;               // perfect hops in a row
        int   bestStreak = 0;
        int   gameFov = 0;              // camera m_iFOV right now (0 = the game's default)
        bool  scoped = false;
    };

    enum class ButtonLookup : int
    {
        Pending,
        Found,
        NotFound,                       // a CS2 update changed how input buttons are registered
    };

    void RunGameThread();               // FrameStageNotify, every frame
    void RunFovBeforeRender();          // FrameStageNotify FRAME_RENDER_START: re-pin the FOV the last tick reset
    // Menu::render, every frame: the bunnyhop / FOV hotkeys + the on-screen speed. True when the "Reapply skins" key
    // was pressed (the menu does that one: Movement doesn't know the skin changer).
    bool RenderThreadTick();
    void Shutdown();                    // unload: release +jump and give the game its FOV back

    // H::hkGetFov: the FOV the game computed for a camera → the one to draw with (yours for your own camera while
    // the FOV changer is on and you're not scoped). SetFovHookActive: whether that hook is in (else m_iFOV is written).
    [[nodiscard]] float OverrideFov(void* cameraServices, float gameFov);
    void SetFovHookActive(bool active);

    // H::hkShouldDrawCrosshair: whether to draw the crosshair for `weapon` although the game says no - true for your
    // own active sniper while you're alive and not scoped and the option is on (the game thread publishes it each
    // frame). SniperCrosshairAvailable: the hook is in (its signature was found).
    [[nodiscard]] bool ShowSniperCrosshair(const void* weapon);
    void SetSniperCrosshairHookActive(bool active);
    [[nodiscard]] bool SniperCrosshairAvailable();

    Stats GetStats();
    void ResetStats();
    ButtonLookup JumpButton();
}
