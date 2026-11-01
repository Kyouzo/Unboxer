#pragma once

class C_CSPlayerPawn;

namespace MusicKitChanger
{
    // Apply Config::skins.musicKit to the local controller (or restore your own kit when it's 0 / disabled).
    void Run(C_CSPlayerPawn* localPawn);
    void ForceUpdate();
    void Reset();
}
