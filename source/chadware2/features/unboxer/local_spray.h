#pragma once

// Graffiti only you see. Spraying a Lootline graffiti (CS2's spray wheel) puts it on the wall you look at as a REAL decal:
// the game's own graffiti decal (the one other players' graffiti and the spray wheel's preview use), drawn in the world
// - behind your hands and gun, lit like the wall, the game's own size. Only on your screen: the game's spray never
// runs for it (that one sends the server a spray of an item Valve doesn't know), nothing with a Lootline graffiti
// ever leaves your PC. One every 45 s like in CS2 (the spray wheel shows the cooldown).
namespace LocalSpray
{
    // Hooks::init, once: the game's spray check and graffiti decal functions. False = a signature broke.
    bool Initialize();
    // Render thread, every frame (Settings -> In a match): on / off.
    void SetOptions(bool shown);
    // The spray wheel's "spray" (SprayAction hook, main thread), slot 0-3. True = a Lootline graffiti is in that slot:
    // done here (a decal on the wall, or nothing when it's off / cooling down), the game's spray must not run.
    bool OnSprayAction(int slot);
    // panorama's frame (main thread): keeps the decals on the walls; takes them down when asked.
    void MainThreadTick();
    // The level is shutting down (main thread, before the world goes): every decal comes down.
    void LevelShutdown();
    // Settings -> "Remove my graffiti" (render thread): the main thread takes them down.
    void Clear();
    [[nodiscard]] int Count();
    // Unload (Lootline's unload thread, while the hooks still run): the main thread takes every decal down; waits for it
    // up to `timeoutMs`.
    void Shutdown(unsigned timeoutMs);
}
