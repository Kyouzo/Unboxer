#pragma once

// Loadout randomizer (ported from Nerv). What it rolls is chosen in the Skins tab (Config::menu.random*).
namespace Randomizer
{
    // Roll the loadout now. Call from the render/menu thread — that's where all Config::skin_changer writes happen.
    // team: which side's loadout to roll (Config::TEAM_T / TEAM_CT; with one shared loadout both are the same).
    void RandomizeNow(int team);

    // "Every Round": the game thread only flags a respawn; the roll happens on the render thread next frame.
    void RequestRandomize();
    bool ConsumeRequest();
}
