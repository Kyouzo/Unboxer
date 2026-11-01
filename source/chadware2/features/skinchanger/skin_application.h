#pragma once

#include <string>

class C_BaseEntity;
class C_CSWeaponBase;
class C_CSPlayerPawn;
class C_EconItemView;

namespace Config
{
    struct item_skin_t;
}

namespace SkinApplication
{
    enum class ItemKind
    {
        Weapon,
        Knife
    };

    bool NeedsUpdate(C_CSWeaponBase* weapon, C_EconItemView* item, const Config::item_skin_t& config, bool forceUpdate, ItemKind kind,
        bool ignoreStatTrakCount = false);
    bool IsDefaultState(C_CSWeaponBase* weapon, C_EconItemView* item, ItemKind kind);

    void Apply(C_CSWeaponBase* weapon, C_EconItemView* item, const Config::item_skin_t& config, C_CSPlayerPawn* localPawn, ItemKind kind);
    void ApplyDefault(C_CSWeaponBase* weapon, C_EconItemView* item, C_CSPlayerPawn* localPawn, ItemKind kind);

    bool RebuildHudKeychainAddon(C_BaseEntity* weapon, C_CSPlayerPawn* localPawn);

    // Charms hanging on a gun's world model (C_KeychainModule entities, "keychain_module", parented to the gun or owned
    // by it). Game thread. RemoveWorldKeychains takes every one of them off right away (the game's own removal, the
    // one AddKeyChainEntity uses) and says how many there were. WorldKeychains counts them; firstDefinition = the
    // first one's charm id (0 = unknown).
    int RemoveWorldKeychains(C_BaseEntity* weapon);
    int WorldKeychains(C_BaseEntity* weapon, int& firstDefinition);
    // For a charm report: every charm entity on the gun, the first-person model's charm copy. Game thread.
    std::string DescribeKeychains(C_BaseEntity* weapon, C_CSPlayerPawn* localPawn);

    // The gun's first-person model (cs2_hudmodel_weapon) gets its AG2 animation graph rebuilt: after a knife change it
    // keeps the old knife's graph (inspect / reload restart and stutter). Game thread. "" = done, else why not.
    const char* RebuildHudAnimGraph(C_BaseEntity* weapon, C_CSPlayerPawn* localPawn);

    // The gun's first-person model (cs2_hudmodel_weapon under the pawn's first-person arms), null if it has none (a
    // holstered gun). Game thread.
    C_BaseEntity* HudWeaponOf(C_BaseEntity* weapon, C_CSPlayerPawn* localPawn);

    void ClearHudWeaponIconFor(C_BaseEntity* weapon);
    void RegenerateSkins();
}
