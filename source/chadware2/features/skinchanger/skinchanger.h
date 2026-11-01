#pragma once

#include <cstdint>
#include <vector>

namespace SkinChanger
{
    void Run();
    // The knife the changer puts in your hand on the side you're playing (0 = none / off). Any thread.
    [[nodiscard]] std::uint16_t LiveKnifeDefinition();
    void RunAgentChanger();
    void ForceUpdate();
    void ForceAgentUpdate();

    // Another player model for one frame, then yours again. On a model change the game rebuilds your first-person
    // gloves and the glove changer puts yours back on: this is what brings them back after selling a defuse kit
    // (GloveChanger does it by itself then; the Misc tab has a button too). Any thread; done in RunAgentChanger.
    void RefreshPlayerModel();
    bool IsAgentDefinitionRejected(std::uint16_t definitionIndex);
    void PrecacheAgentModels();
    void Reset();

    // Hold live application until the current continuous UI edit is committed.
    void SetInteractiveEditActive(bool active);
    [[nodiscard]] bool IsInteractiveEditActive();
    [[nodiscard]] bool IsUpdatePending();
    void ProcessPendingKeychains();

    // Game thread, every FRAME_NET_UPDATE_END (pawn may be null): records what the menu may ask about the local
    // player — alive or not, which guns it holds — so the render thread never walks game entities itself.
    void UpdateLiveInfo(void* localPawn);

    // Guns you're holding right now (active one first), as recorded by UpdateLiveInfo. Any thread.
    [[nodiscard]] std::vector<std::uint16_t> GetOwnedWeaponDefinitionIndices();

    // Optional: where charm reports go (one line each: what charms hang on a gun when it's first seen, after ours goes
    // on, 3 s later, and when one has to be put back). Set once at startup; null (the default) = no reports.
    void SetCharmLog(void (*log)(const char* line));

    // Optional (Lootline): is this item id one of your own inventory items? A server running in your own game (you host,
    // offline practice) hands your guns those very items - with their paint, stickers and charm - and keeps sending them
    // again. The skin changer leaves such a gun alone (fighting it cleared the paint the game was about to show and gave
    // up unpainted after 5 tries). Set once at startup; null (the default) = every gun is the skin changer's.
    void SetOwnItemCheck(bool (*isOwnItem)(std::uint64_t id));
}
