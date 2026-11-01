#include "music_kit_changer.h"

#include <atomic>
#include <cstdint>

#include "../../config/config.h"
#include "../../interfaces/interfaces.h"
#include "../../utils/schema/schema.h"

#include "../../../cs2/entity/C_CSPlayerPawn/C_CSPlayerPawn.h"

// ─────────────────────────────────────────────────────────────────────────────────────────────────────────
// Music kit changer (ported from Nerv). Three fields, all resolved through the live schema so they survive
// CS2 patches (build 14183 offsets for reference: 0x968 / 0x962 / 0x820 → 0x58):
//   CCSPlayerController::m_iMusicKitID                  — the kit your MVP anthem plays in a match
//   CCSPlayerController::m_bMvpNoMusic                  — mutes the anthem; cleared while a custom kit is set
//   CCSPlayerController_InventoryServices::m_unMusicID  — the kit used in the main menu / scoreboard
// Client-side only — nobody else hears it. Your real values are captured per controller (per match) and put
// back when you choose "Default" or turn the skin changer off. Re-applied if the game ever overwrites them.
// ─────────────────────────────────────────────────────────────────────────────────────────────────────────

namespace
{
    template <typename T>
    T* Field(void* base, std::uint32_t offset)
    {
        return (base && offset) ? reinterpret_cast<T*>(static_cast<std::uint8_t*>(base) + offset) : nullptr;
    }

    std::uint32_t KitOffset()     { return SchemaFinder::GetCached<hash_32_fnv1a_const("CCSPlayerController->m_iMusicKitID")>(); }
    std::uint32_t MuteOffset()    { return SchemaFinder::GetCached<hash_32_fnv1a_const("CCSPlayerController->m_bMvpNoMusic")>(); }
    std::uint32_t InvOffset()     { return SchemaFinder::GetCached<hash_32_fnv1a_const("CCSPlayerController->m_pInventoryServices")>(); }
    std::uint32_t MusicIdOffset() { return SchemaFinder::GetCached<hash_32_fnv1a_const("CCSPlayerController_InventoryServices->m_unMusicID")>(); }

    std::atomic_bool g_forceUpdate{ true };

    void* g_lastController = nullptr;
    bool g_haveOriginal = false;          // captured the controller's own values before overwriting them
    int g_originalKit = 0;
    bool g_originalMute = false;
    std::uint16_t g_originalMusicId = 0;
    bool g_applied = false;               // a custom kit is currently written
}

void MusicKitChanger::Run(C_CSPlayerPawn* localPawn)
{
    if (!localPawn || !I::GameEntity || !I::GameEntity->Instance)
        return;

    void* controller = I::GameEntity->Instance->Get<C_BaseEntity>(localPawn->m_hController());

    if (!controller)
        return;

    int* kit = Field<int>(controller, KitOffset());

    if (!kit)
        return;

    bool* mute = Field<bool>(controller, MuteOffset());

    void* inventory = nullptr;
    if (void** inventoryPtr = Field<void*>(controller, InvOffset()))
        inventory = *inventoryPtr;

    std::uint16_t* musicId = Field<std::uint16_t>(inventory, MusicIdOffset());

    // A different controller means a new match / reconnect: whatever we captured belonged to the old one.
    if (controller != g_lastController)
    {
        g_lastController = controller;
        g_haveOriginal = false;
        g_applied = false;
        g_forceUpdate.store(true);
    }

    const int wanted = Config::Game().enabled ? Config::Game().musicKit : 0;

    if (wanted > 0)
    {
        if (!g_haveOriginal)
        {
            g_originalKit = *kit;
            g_originalMute = mute ? *mute : false;
            g_originalMusicId = musicId ? *musicId : 0;
            g_haveOriginal = true;
        }

        const bool differs = *kit != wanted
            || (mute && *mute)
            || (musicId && *musicId != static_cast<std::uint16_t>(wanted));

        if (differs || g_forceUpdate.load())
        {
            *kit = wanted;

            if (mute)
                *mute = false;

            if (musicId)
                *musicId = static_cast<std::uint16_t>(wanted);
        }

        g_applied = true;
    }
    else if (g_applied)
    {
        // back to your own kit
        if (g_haveOriginal)
        {
            *kit = g_originalKit;

            if (mute)
                *mute = g_originalMute;

            if (musicId)
                *musicId = g_originalMusicId;
        }

        g_applied = false;
    }

    g_forceUpdate.store(false);
}

void MusicKitChanger::ForceUpdate()
{
    g_forceUpdate.store(true);
}

void MusicKitChanger::Reset()
{
    g_lastController = nullptr;
    g_haveOriginal = false;
    g_applied = false;
    g_forceUpdate.store(true);
}
