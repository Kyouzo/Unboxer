#pragma once

#include <cstdint>

class C_CSPlayerPawn;

namespace Config
{
    struct item_skin_t;
}

// Real StatTrak: every kill you get is credited to the gun / knife in your hands, per weapon + skin
// ("AK-47 | Redline" has its own count). Counts live in %USERPROFILE%\.standalone_skinchanger\stattrak.json —
// NOT in the skin configs, so loading a config never rolls your counters back.
namespace StatTrakTracker
{
    // Game thread, every frame (alive or dead): watches your match kill count and credits new kills.
    void Poll(C_CSPlayerPawn* localPawn);

    [[nodiscard]] int Get(std::uint16_t definitionIndex, int paintKit);
    void Set(std::uint16_t definitionIndex, int paintKit, int kills);

    // The number the game should show for this item: the real count, or the typed one when counting is off.
    [[nodiscard]] int Displayed(std::uint16_t definitionIndex, const Config::item_skin_t& skin);

    void Load();          // once, at start
    void SaveIfDirty();   // render thread; cheap when nothing changed
}
