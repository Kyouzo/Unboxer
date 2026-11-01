#include "skin_application.h"
#include "../../../patterns.h"
#include "econ_item_attribute_manager.h"
#include "item_schema.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cstddef>
#include <string>
#include <unordered_map>
#include <excpt.h>

#include "../../config/config.h"
#include "../../interfaces/interfaces.h"
#include "../../utils/memory/patternscan/patternscan.h"

#include <cs2/entity/C_BaseEntity/C_BaseEntity.h>
#include <cs2/entity/CCompositeMaterialOwner/CCompositeMaterialOwner.h>
#include <cs2/entity/C_CS2HudModelWeapon/C_CS2HudModelWeapon.h>
#include <cs2/hud/HudWeaponSelection.h>
#include <cs2/entity/C_EntityInstance/C_EntityInstance.h>
#include <cs2/entity/C_EconEntity/C_EconEntity.h>
#include <cs2/entity/C_CSWeaponBase/C_CSWeaponBase.h>
#include <cs2/entity/C_CSPlayerPawn/C_CSPlayerPawn.h>

namespace
{
    constexpr int ITEM_QUALITY_NORMAL = 0;
    constexpr int ITEM_QUALITY_UNUSUAL = 3;
    constexpr int ITEM_QUALITY_STRANGE = 9;
    constexpr int ITEM_QUALITY_TOURNAMENT = 12;   // Souvenir

    using BuildModernWeaponSkinMaterialFn = void(__fastcall*)(C_EconItemView*, void*, std::int64_t, int, char, char, void*);
    using ReleaseCompositeMaterialsFn = void(__fastcall*)(CCompositeMaterialOwner*, bool);

    struct WeaponStickerState
    {
        std::uint16_t definitionIndex = 0;
        int effectiveCount = 0;
    };

    // CS2 does not clear the old sticker composite when the sticker list becomes
    // empty. Track the previous count so the native release is only used for N -> 0.
    std::unordered_map<std::uintptr_t, WeaponStickerState> g_WeaponStickerStates;

    int GetEffectiveStickerCount(const Config::item_skin_t& config)
    {
        int count = 0;

        for (const auto& sticker : config.stickers)
        {
            if (sticker.enabled && sticker.kit > 0)
                ++count;
        }

        return count;
    }

    bool UpdateStickerState(C_CSWeaponBase* weapon, C_EconItemView* item, int effectiveCount)
    {
        if (!weapon || !item)
            return false;

        const std::uintptr_t key = reinterpret_cast<std::uintptr_t>(weapon);
        const std::uint16_t definitionIndex = item->m_iItemDefinitionIndex();
        const auto it = g_WeaponStickerStates.find(key);

        bool finalStickerRemoved = false;

        if (it != g_WeaponStickerStates.end() && it->second.definitionIndex == definitionIndex)
            finalStickerRemoved = it->second.effectiveCount > 0 && effectiveCount == 0;

        g_WeaponStickerStates[key] = { definitionIndex, effectiveCount };
        return finalStickerRemoved;
    }

    ReleaseCompositeMaterialsFn GetReleaseCompositeMaterials()
    {
        const auto fn = M::ResolveCached([] { return reinterpret_cast<ReleaseCompositeMaterialsFn>(M::scan("client.dll", Patterns::ReleaseCompositeMaterials)); });
        return fn;
    }

    void ReleaseEntityCompositeMaterials(C_BaseEntity* entity, bool stampGeneration)
    {
        CCompositeMaterialOwner* owner = GetCompositeMaterialOwner(entity);
        if (!owner)
            return;

        const auto release = GetReleaseCompositeMaterials();

        if (!release)
            return;

        __try
        {
            release(owner, stampGeneration);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
        }
    }

    int GetBaseQuality(SkinApplication::ItemKind kind)
    {
        return kind == SkinApplication::ItemKind::Knife ? ITEM_QUALITY_UNUSUAL : ITEM_QUALITY_NORMAL;
    }

    int GetDesiredQuality(const Config::item_skin_t& config, SkinApplication::ItemKind kind)
    {
        if (config.statTrakEnabled)
            return ITEM_QUALITY_STRANGE;

        // the quality is what the gun's name starts with ("Souvenir AWP | Dragon Lore"); knives / gloves have no Souvenirs
        return config.souvenir && kind == SkinApplication::ItemKind::Weapon ? ITEM_QUALITY_TOURNAMENT : GetBaseQuality(kind);
    }

    int GetDesiredStatTrak(const Config::item_skin_t& config)
    {
        return config.statTrakEnabled ? (std::max)(0, config.statTrakKills) : -1;
    }

    float GetDesiredWear(const Config::item_skin_t& config)
    {
        return std::clamp(config.wear, 0.0001f, 1.0f);
    }

    int GetDesiredSeed(const Config::item_skin_t& config)
    {
        return std::clamp(config.seed, 0, 1000);
    }

    C_BaseEntity* FindHudWeapon(C_BaseEntity* weapon, C_CSPlayerPawn* localPawn)
    {
        if (!weapon || !localPawn || !I::GameEntity || !I::GameEntity->Instance)
            return nullptr;

        const CBaseHandle armsHandle = localPawn->m_hHudModelArms();

        if (!armsHandle.valid())
            return nullptr;

        C_BaseEntity* hudArms = I::GameEntity->Instance->Get<C_BaseEntity>(armsHandle.index());

        if (!hudArms)
            return nullptr;

        CGameSceneNode* armsNode = hudArms->m_pGameSceneNode();

        if (!armsNode)
            return nullptr;

        for (CGameSceneNode* node = armsNode->GetChild(); node; node = node->GetNextSibling())
        {
            CEntityInstance* nodeOwner = node->GetOwner();

            if (!nodeOwner)
                continue;

            C_BaseEntity* hudEntity = reinterpret_cast<C_BaseEntity*>(nodeOwner);
            const CBaseHandle ownerHandle = hudEntity->m_hOwnerEntity();

            if (!ownerHandle.valid())
                continue;

            C_BaseEntity* ownerEntity = I::GameEntity->Instance->Get<C_BaseEntity>(ownerHandle.index());

            if (ownerEntity == weapon)
                return hudEntity;
        }

        return nullptr;
    }

    std::uintptr_t FindHudElement(const char* name)
    {
        using Fn = std::uintptr_t(__fastcall*)(const char*);

        const auto fn = M::ResolveCached([] { return reinterpret_cast<Fn>(M::scan("client.dll", Patterns::FindHudElement)); });
        return fn ? fn(name) : 0;
    }

    void ClearHudWeaponIconAtIndex(std::uintptr_t hudWeapons, std::int32_t index, std::int64_t unknown)
    {
        const auto callAddress = M::ResolveCached([] { return M::scan("client.dll", Patterns::ClearHudWeapon); });

        if (!callAddress)
            return;

        using Fn = std::int64_t(__fastcall*)(std::uintptr_t, std::int32_t, std::int64_t);

        const auto fn = reinterpret_cast<Fn>(reinterpret_cast<std::uintptr_t>(callAddress) + 5 + *reinterpret_cast<std::int32_t*>(callAddress + 1));

        fn(hudWeapons, index, unknown);
    }

    BuildModernWeaponSkinMaterialFn GetBuildModernWeaponSkinMaterial()
    {
        const auto fn = M::ResolveCached([] { return reinterpret_cast<BuildModernWeaponSkinMaterialFn>(M::scan("client.dll", Patterns::BuildModernSkin)); });
        return fn;
    }

    void BuildModernWeaponSkinMaterial(C_EconItemView* item, void* model)
    {
        if (!item || !model)
            return;

        const auto fn = GetBuildModernWeaponSkinMaterial();

        if (fn)
            fn(item, model, 0, 2048, 0, 0, nullptr);
    }

    void SetCustomName(C_EconItemView* item, const std::string& name)
    {
        if (!item)
            return;

        char* customName = item->m_szCustomName();

        if (!customName)
            return;

        if (name.empty())
        {
            customName[0] = '\0';
            return;
        }

        strncpy_s(customName, 161, name.c_str(), _TRUNCATE);
    }

    std::uint64_t GetMeshMask(int paintKit)
    {
        const PaintKitInfo* paintKitInfo = Skins::g_ItemSchema.GetPaintKit(paintKit);
        return paintKitInfo && paintKitInfo->usesOldModel ? 2ULL : 1ULL;
    }

    void SetMeshGroup(C_BaseEntity* entity, std::uint64_t meshMask)
    {
        if (!entity)
            return;

        if (CGameSceneNode* sceneNode = entity->m_pGameSceneNode())
            sceneNode->SetMeshGroupMask(meshMask);
    }

    void ApplyWeaponMaterial(C_CSWeaponBase* weapon, C_EconItemView* item, C_CSPlayerPawn* localPawn, std::uint64_t meshMask, bool finalStickerRemoved, bool replacementCompositeExpected)
    {
        C_BaseEntity* hudWeapon = FindHudWeapon(weapon, localPawn);

        if (finalStickerRemoved)
        {
            // If a paint kit remains, clear the old composite ownership without running
            // the derived empty-material callback. The replacement paint-only composite
            // is built immediately below, avoiding a transient stock-material reapply.
            // The HUD model has its own owner and must be cleared separately on N -> 0.
            const bool stampGeneration = !replacementCompositeExpected;
            ReleaseEntityCompositeMaterials(weapon, stampGeneration);
            ReleaseEntityCompositeMaterials(hudWeapon, stampGeneration);
        }

        SetMeshGroup(weapon, meshMask);
        SetMeshGroup(hudWeapon, meshMask);

        BuildModernWeaponSkinMaterial(item, weapon);

        if (hudWeapon)
            BuildModernWeaponSkinMaterial(item, hudWeapon);
    }

    void ApplyKnifeMaterial(C_CSWeaponBase* weapon, C_EconItemView* item, C_CSPlayerPawn* localPawn, std::uint64_t meshMask)
    {
        SetMeshGroup(weapon, meshMask);
        BuildModernWeaponSkinMaterial(item, weapon);

        C_BaseEntity* hudWeapon = FindHudWeapon(weapon, localPawn);

        SetMeshGroup(hudWeapon, meshMask);

        if (hudWeapon)
            BuildModernWeaponSkinMaterial(item, hudWeapon);

    }

    // CS2's name tag on the gun (Patterns::AddNametagEntity: the nametag model on the gun's "nametag" attachment; the
    // first-person gun copies it by itself). It reads the item's custom name: the same name already on = nothing, another
    // = the old tag replaced, none = the tag taken off. So it's called after every apply / default, once the mesh is set.
    using AddNametagFn = char(__fastcall*)(C_BaseEntity*, C_EconItemView*);

    AddNametagFn GetAddNametag()
    {
        const auto fn = M::ResolveCached([] { return reinterpret_cast<AddNametagFn>(M::scan("client.dll", Patterns::AddNametagEntity)); });
        return fn;
    }

    void UpdateNametag(C_BaseEntity* weapon, C_EconItemView* item)
    {
        const AddNametagFn addNametag = GetAddNametag();

        if (!addNametag || !weapon || !item)
            return;

        __try
        {
            addNametag(weapon, item);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
        }
    }
}

namespace
{
    constexpr const char* KEYCHAIN_MODULE = "keychain_module";      // the charm entity AddKeyChainEntity makes
    constexpr const char* HUD_ADDON = "cs2_hudmodel_addon";        // a first-person model part (its charm copy too)
    constexpr int MAX_KEYCHAINS = 16;
    constexpr int MAX_ENTITY_INDEX = 0x7FFE;   // networked 0..0x3FFF, client-side (charms are) 0x4000 and up

    // entity -> m_pEntity (CEntityIdentity*, +0x10) -> m_designerName (+0x20, a CUtlSymbolLarge = the text itself)
    bool IsDesigner(const void* entity, const char* expected)
    {
        __try
        {
            const auto* identity = *reinterpret_cast<const std::uint8_t* const*>(static_cast<const std::uint8_t*>(entity) + 0x10);
            if (!identity)
                return false;

            const char* name = *reinterpret_cast<const char* const*>(identity + 0x20);
            return name && std::strcmp(name, expected) == 0;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    // CEntityIdentity::m_EHandle (+0x10) & 0x7FFF; -1 = unreadable
    int EntityIndex(const void* entity)
    {
        __try
        {
            const auto* identity = *reinterpret_cast<const std::uint8_t* const*>(static_cast<const std::uint8_t*>(entity) + 0x10);
            return identity ? static_cast<int>(*reinterpret_cast<const std::uint32_t*>(identity + 0x10) & ENT_ENTRY_MASK) : -1;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return -1;
        }
    }

    std::uint32_t ReadU32(const void* base, std::uint32_t offset)
    {
        if (!base || offset == 0)
            return 0;

        __try
        {
            return *reinterpret_cast<const std::uint32_t*>(static_cast<const std::uint8_t*>(base) + offset);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return 0;
        }
    }

    void* ReadPointer(const void* base, std::uint32_t offset)
    {
        if (!base || offset == 0)
            return nullptr;

        __try
        {
            return *reinterpret_cast<void* const*>(static_cast<const std::uint8_t*>(base) + offset);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return nullptr;
        }
    }

    std::uint32_t KeychainDefinitionOffset() { return SchemaFinder::GetCached<hash_32_fnv1a_const("C_KeychainModule->m_nKeychainDefID")>(); }
    std::uint32_t KeychainSeedOffset() { return SchemaFinder::GetCached<hash_32_fnv1a_const("C_KeychainModule->m_nKeychainSeed")>(); }
    std::uint32_t SceneParentOffset() { return SchemaFinder::GetCached<hash_32_fnv1a_const("CGameSceneNode->m_pParent")>(); }

    // `node` is the gun's scene node or hangs under it (a few parents up).
    bool UnderNode(CGameSceneNode* node, CGameSceneNode* root)
    {
        for (int depth = 0; node && depth < 5; ++depth)
        {
            if (node == root)
                return true;

            node = static_cast<CGameSceneNode*>(ReadPointer(node, SceneParentOffset()));
        }

        return false;
    }

    // A charm entity belongs to this gun: the gun owns it, or it hangs under the gun's scene node.
    bool OnGun(C_BaseEntity* entity, C_BaseEntity* weapon, CGameSceneNode* weaponNode, bool* owned = nullptr, bool* parented = nullptr)
    {
        const CBaseHandle owner = entity->m_hOwnerEntity();
        const bool byOwner = owner.valid() && I::GameEntity->Instance->Get<C_BaseEntity>(owner.index()) == weapon;
        const bool byParent = weaponNode && UnderNode(entity->m_pGameSceneNode(), weaponNode);

        if (owned)
            *owned = byOwner;
        if (parented)
            *parented = byParent;

        return byOwner || byParent;
    }

    // Every charm entity on this gun, wherever CS2 hung it. A whole entity-list pass (~32k cheap lookups): only when a
    // charm is put on / checked (at most once a second per gun).
    int CollectWorldKeychains(C_BaseEntity* weapon, C_BaseEntity* (&out)[MAX_KEYCHAINS])
    {
        if (!weapon || !I::GameEntity || !I::GameEntity->Instance)
            return 0;

        CGameSceneNode* weaponNode = weapon->m_pGameSceneNode();
        int count = 0;

        for (int index = 0; index <= MAX_ENTITY_INDEX && count < MAX_KEYCHAINS; ++index)
        {
            C_BaseEntity* entity = I::GameEntity->Instance->Get<C_BaseEntity>(index);

            if (entity && entity != weapon && IsDesigner(entity, KEYCHAIN_MODULE) && OnGun(entity, weapon, weaponNode))
                out[count++] = entity;
        }

        return count;
    }
}

int SkinApplication::RemoveWorldKeychains(C_BaseEntity* weapon)
{
    using RemoveNowFn = void(__fastcall*)(C_BaseEntity*);
    const auto removeNow = M::ResolveCached([] { return reinterpret_cast<RemoveNowFn>(M::scan("client.dll", Patterns::RemoveKeychainModule)); });

    if (!removeNow)
        return 0;

    C_BaseEntity* found[MAX_KEYCHAINS]{};
    const int count = CollectWorldKeychains(weapon, found);

    for (int i = 0; i < count; ++i)
        removeNow(found[i]);

    return count;
}

int SkinApplication::WorldKeychains(C_BaseEntity* weapon, int& firstDefinition)
{
    firstDefinition = 0;
    C_BaseEntity* found[MAX_KEYCHAINS]{};
    const int count = CollectWorldKeychains(weapon, found);

    if (count > 0)
        firstDefinition = static_cast<int>(ReadU32(found[0], KeychainDefinitionOffset()));

    return count;
}

std::string SkinApplication::DescribeKeychains(C_BaseEntity* weapon, C_CSPlayerPawn* localPawn)
{
    if (!weapon || !I::GameEntity || !I::GameEntity->Instance)
        return "no gun";

    std::string text = "charm entities on the gun:";
    char part[192]{};
    CGameSceneNode* weaponNode = weapon->m_pGameSceneNode();
    int total = 0;
    int onGun = 0;

    for (int index = 0; index <= MAX_ENTITY_INDEX; ++index)
    {
        C_BaseEntity* entity = I::GameEntity->Instance->Get<C_BaseEntity>(index);

        if (!entity || entity == weapon || !IsDesigner(entity, KEYCHAIN_MODULE))
            continue;

        ++total;
        bool owned = false;
        bool parented = false;

        if (!OnGun(entity, weapon, weaponNode, &owned, &parented))
            continue;   // another player's charm: only counted

        ++onGun;
        snprintf(part, sizeof(part), " [#%d charm %u seed %u, owner %s, hangs %s]", index,
            ReadU32(entity, KeychainDefinitionOffset()), ReadU32(entity, KeychainSeedOffset()),
            owned ? "gun" : "other", parented ? "on gun" : "elsewhere");
        text += part;
    }

    snprintf(part, sizeof(part), " = %d on this gun, %d in the game", onGun, total);
    text += part;

    // first person: the gun's HUD model, its "keychain" addon, and every addon hanging on it
    C_BaseEntity* hudWeapon = FindHudWeapon(weapon, localPawn);

    if (!hudWeapon)
        return text + "; first-person model: not found";

    using AddonDictFindFn = std::uint32_t(__fastcall*)(HudAddonDictionary*, const char*, std::uint32_t, bool*);
    const auto findAddon = M::ResolveCached([] { return reinterpret_cast<AddonDictFindFn>(M::scan("client.dll", Patterns::FindHudKeychainAddon)); });
    auto* hud = reinterpret_cast<HudWeaponAddonLayout*>(hudWeapon);
    int dictionaryAddon = -1;

    if (findAddon)
    {
        constexpr std::uint32_t KEYCHAIN_HASH = 0x0AA5272A;
        const std::uint32_t slot = findAddon(&hud->addons, "keychain", KEYCHAIN_HASH, nullptr);

        if (slot != INVALID_EHANDLE_INDEX && slot < hud->addons.Capacity() && hud->addons.entries && hud->addons.entries[slot].entityHandle.valid())
            dictionaryAddon = hud->addons.entries[slot].entityHandle.index();
    }

    snprintf(part, sizeof(part), "; first-person model #%d, its charm addon #%d, state %u, addons hanging on it:",
        EntityIndex(hudWeapon), dictionaryAddon, static_cast<unsigned>(hud->keychainState));
    text += part;

    if (CGameSceneNode* hudNode = hudWeapon->m_pGameSceneNode())
    {
        int shown = 0;

        for (CGameSceneNode* child = hudNode->GetChild(); child && shown < 24; child = child->GetNextSibling(), ++shown)
        {
            CEntityInstance* owner = child->GetOwner();

            if (!owner || !IsDesigner(owner, HUD_ADDON))
                continue;

            snprintf(part, sizeof(part), " #%d", EntityIndex(owner));
            text += part;
        }
    }

    return text;
}

namespace
{
    using AnimGraphRebuildFn = void(__fastcall*)(void* controller, std::uint8_t mode);

    bool CallAnimGraphRebuild(AnimGraphRebuildFn rebuild, void* controller)
    {
        __try
        {
            rebuild(controller, 2);   // 2: what the recipe passes (0xFF would mean "the controller's own mode")
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }
}

const char* SkinApplication::RebuildHudAnimGraph(C_BaseEntity* weapon, C_CSPlayerPawn* localPawn)
{
    const auto rebuild = M::ResolveCached([] { return reinterpret_cast<AnimGraphRebuildFn>(M::scan("client.dll", Patterns::AnimGraphRebuild)); });

    if (!rebuild)
        return "signature AnimGraphRebuild not found";

    C_BaseEntity* hudWeapon = FindHudWeapon(weapon, localPawn);

    if (!hudWeapon || !IsDesigner(hudWeapon, "cs2_hudmodel_weapon"))
        return "first-person model not found";

    // hud weapon -> m_CBodyComponent -> m_animationController (embedded) -> m_pGraphInstanceAG2
    const std::uint32_t bodyOffset = SchemaFinder::GetCached<hash_32_fnv1a_const("C_BaseEntity->m_CBodyComponent")>();
    const std::uint32_t controllerOffset = SchemaFinder::GetCached<hash_32_fnv1a_const("CBodyComponentBaseAnimGraph->m_animationController")>();
    const std::uint32_t graphOffset = SchemaFinder::GetCached<hash_32_fnv1a_const("CBaseAnimGraphController->m_pGraphInstanceAG2")>();

    if (!bodyOffset || !controllerOffset || !graphOffset)
        return "schema fields not found";

    void* body = ReadPointer(hudWeapon, bodyOffset);

    if (!body)
        return "no body component";

    void* controller = static_cast<std::uint8_t*>(body) + controllerOffset;

    if (!ReadPointer(controller, graphOffset))
        return "no animation graph yet";

    return CallAnimGraphRebuild(rebuild, controller) ? "" : "rebuild faulted";
}

C_BaseEntity* SkinApplication::HudWeaponOf(C_BaseEntity* weapon, C_CSPlayerPawn* localPawn)
{
    return FindHudWeapon(weapon, localPawn);
}

bool SkinApplication::RebuildHudKeychainAddon(C_BaseEntity* weapon, C_CSPlayerPawn* localPawn)
{
    C_BaseEntity* hudWeapon = FindHudWeapon(weapon, localPawn);

    if (!hudWeapon || !weapon || !I::GameEntity || !I::GameEntity->Instance)
        return false;

    constexpr std::uint32_t KEYCHAIN_HASH = 0x0AA5272A; // Native dictionary hash of "keychain".
    using AddonDictFindFn = std::uint32_t(__fastcall*)(HudAddonDictionary*, const char*, std::uint32_t, bool*);
    using UtilRemoveFn = void(__fastcall*)(C_BaseEntity*);

    const auto findAddon = M::ResolveCached([] { return reinterpret_cast<AddonDictFindFn>(M::scan("client.dll", Patterns::FindHudKeychainAddon)); });

    const auto utilRemove = M::ResolveCached([] { return reinterpret_cast<UtilRemoveFn>(M::scan("client.dll", Patterns::RemoveHudKeychainAddon)); });

    if (!findAddon || !utilRemove)
        return false;

    const CBaseHandle worldWeaponHandle = hudWeapon->m_hOwnerEntity();
    if (!worldWeaponHandle.valid())
        return false;

    C_BaseEntity* resolvedWorldWeapon = I::GameEntity->Instance->Get<C_BaseEntity>(worldWeaponHandle.index());
    if (resolvedWorldWeapon != weapon)
        return false;

    auto* hud = reinterpret_cast<HudWeaponAddonLayout*>(hudWeapon);
    HudAddonDictionary& dictionary = hud->addons;
    const std::uint32_t addonIndex = findAddon(&dictionary, "keychain", KEYCHAIN_HASH, nullptr);

    if (addonIndex != INVALID_EHANDLE_INDEX)
    {
        if (addonIndex >= dictionary.Capacity() || !dictionary.entries)
            return false;

        CBaseHandle& addonHandle = dictionary.entries[addonIndex].entityHandle;
        if (addonHandle.valid())
        {
            if (C_BaseEntity* addon = I::GameEntity->Instance->Get<C_BaseEntity>(addonHandle.index()))
                utilRemove(addon);

            addonHandle = CBaseHandle{};
        }
    }

    hud->keychainState = 0xFFu;
    return true;
}

bool SkinApplication::NeedsUpdate(C_CSWeaponBase* weapon, C_EconItemView* item, const Config::item_skin_t& config, bool forceUpdate, ItemKind kind,
    bool ignoreStatTrakCount)
{
    if (!weapon || !item)
        return false;

    if (forceUpdate)
        return true;

    if (weapon->m_nFallbackPaintKit() != config.paintKit)
        return true;

    if (weapon->m_nFallbackSeed() != GetDesiredSeed(config))
        return true;

    if (weapon->m_flFallbackWear() != GetDesiredWear(config))
        return true;

    if (!ignoreStatTrakCount && weapon->m_nFallbackStatTrak() != GetDesiredStatTrak(config))
        return true;

    if (ignoreStatTrakCount && (weapon->m_nFallbackStatTrak() >= 0) != (GetDesiredStatTrak(config) >= 0))
        return true;   // StatTrak on / off still counts

    if (item->m_iEntityQuality() != GetDesiredQuality(config, kind))
        return true;

    char* customName = item->m_szCustomName();

    if (!customName)
        return !config.customName.empty();

    return std::strncmp(customName, config.customName.c_str(), 160) != 0;
}

bool SkinApplication::IsDefaultState(C_CSWeaponBase* weapon, C_EconItemView* item, ItemKind kind)
{
    if (!weapon || !item)
        return true;

    if (weapon->m_nFallbackPaintKit() != 0)
        return false;

    if (weapon->m_nFallbackStatTrak() != -1)
        return false;

    if (item->m_iEntityQuality() != GetBaseQuality(kind))
        return false;

    char* customName = item->m_szCustomName();

    return !customName || customName[0] == '\0';
}

void SkinApplication::Apply(C_CSWeaponBase* weapon, C_EconItemView* item, const Config::item_skin_t& config, C_CSPlayerPawn* localPawn, ItemKind kind)
{
    if (!weapon || !item || !localPawn)
        return;

    // the real gun's own stickers / charm / StatTrak / name (its networked attributes) don't show through ours
    if (kind == ItemKind::Weapon && EconItemAttributeManager::ReplaceRealItem())
        EconItemAttributeManager::ClearNetworkedAttributes(item);

    const bool finalStickerRemoved =
        kind == ItemKind::Weapon && UpdateStickerState(weapon, item, GetEffectiveStickerCount(config));

    const float wear = GetDesiredWear(config);
    const int seed = GetDesiredSeed(config);
    const int statTrak = GetDesiredStatTrak(config);

    EconItemAttributeManager::Remove(item);

    item->m_iItemIDHigh() = 0xFFFFFFFFu;
    item->m_bInitialized() = true;
    item->m_bDisallowSOC() = false;
    item->m_bRestoreCustomMaterialAfterPrecache() = true;
    item->m_iEntityQuality() = GetDesiredQuality(config, kind);

    weapon->m_nFallbackPaintKit() = config.paintKit;
    weapon->m_nFallbackSeed() = seed;
    weapon->m_flFallbackWear() = wear;
    weapon->m_nFallbackStatTrak() = statTrak;

    EconItemAttributeManager::StickerArray stickers{};
    EconItemAttributeManager::KeychainData keychain{};

    if (kind == ItemKind::Weapon)
    {
        for (std::size_t i = 0; i < stickers.size() && i < config.stickers.size(); ++i)
        {
            stickers[i].enabled = config.stickers[i].enabled;
            stickers[i].kit = config.stickers[i].kit;
            stickers[i].wear = config.stickers[i].wear;
            stickers[i].scale = config.stickers[i].scale;
            stickers[i].rotation = config.stickers[i].rotation;
            stickers[i].offsetX = config.stickers[i].offsetX;
            stickers[i].offsetY = config.stickers[i].offsetY;
            stickers[i].schema = config.stickers[i].schema;
        }

        keychain.enabled = config.keychain.enabled && config.keychain.id > 0;
        keychain.id = config.keychain.id;
        keychain.seed = config.keychain.seed;
        keychain.sticker = config.keychain.sticker;
        keychain.offsetX = config.keychain.offsetX;
        keychain.offsetY = config.keychain.offsetY;
        keychain.offsetZ = config.keychain.offsetZ;
    }

    EconItemAttributeManager::Create(
        item,
        config.paintKit,
        wear,
        seed,
        statTrak,
        0,
        kind == ItemKind::Weapon ? &stickers : nullptr,
        kind == ItemKind::Weapon ? &keychain : nullptr);

    // Keep the live keychain hidden from the normal HUD/material refresh; the
    // deferred keychain pass restores the ID and rebuilds the native world/HUD charm.
    if (kind == ItemKind::Weapon && keychain.enabled)
        EconItemAttributeManager::SetKeychainId(item, 0);

    SetCustomName(item, config.customName);
    item->m_name_description_ptr() = 0;

    const std::uint64_t meshMask = GetMeshMask(config.paintKit);

    if (kind == ItemKind::Knife)
    {
        ApplyKnifeMaterial(weapon, item, localPawn, meshMask);
        UpdateNametag(weapon, item);
        return;
    }

    ApplyWeaponMaterial(weapon, item, localPawn, meshMask, finalStickerRemoved, config.paintKit > 0);
    UpdateNametag(weapon, item);
}

void SkinApplication::ApplyDefault(C_CSWeaponBase* weapon, C_EconItemView* item, C_CSPlayerPawn* localPawn, ItemKind kind)
{
    if (!weapon || !item || !localPawn)
        return;

    const bool finalStickerRemoved =
        kind == ItemKind::Weapon && UpdateStickerState(weapon, item, 0);

    EconItemAttributeManager::Remove(item);

    item->m_iEntityQuality() = GetBaseQuality(kind);
    item->m_bInitialized() = true;
    item->m_bDisallowSOC() = false;
    item->m_bRestoreCustomMaterialAfterPrecache() = true;

    weapon->m_nFallbackPaintKit() = 0;
    weapon->m_nFallbackSeed() = 0;
    weapon->m_flFallbackWear() = 0.0f;
    weapon->m_nFallbackStatTrak() = -1;

    SetCustomName(item, {});
    item->m_name_description_ptr() = 0;

    if (kind == ItemKind::Knife)
    {
        SetMeshGroup(weapon, 1ULL);

        C_BaseEntity* hudWeapon = FindHudWeapon(weapon, localPawn);

        SetMeshGroup(hudWeapon, 1ULL);
        UpdateNametag(weapon, item);
        return;
    }

    C_BaseEntity* hudWeapon = FindHudWeapon(weapon, localPawn);

    if (finalStickerRemoved)
    {
        // No custom paint composite will replace the sticker material in the default
        // state, so use the native stamped release to reapply the base material now.
        ReleaseEntityCompositeMaterials(weapon, true);
        ReleaseEntityCompositeMaterials(hudWeapon, true);
    }

    weapon->PostDataUpdate(1);

    SetMeshGroup(weapon, 1ULL);
    SetMeshGroup(hudWeapon, 1ULL);
    UpdateNametag(weapon, item);
}

void SkinApplication::ClearHudWeaponIconFor(C_BaseEntity* weapon)
{
    if (!weapon || !I::GameEntity || !I::GameEntity->Instance)
        return;

    __try
    {
        const std::uintptr_t hud = FindHudElement("HudWeaponSelection");

        if (!hud)
            return;

        auto* selection = HudWeaponSelectionLayout::FromHudElement(hud);
        const HudWeaponSelectionEntry* data = selection->entries;
        const std::int32_t count = selection->count;

        if (!data || count <= 0 || count > 64)
            return;

        for (std::int32_t i = count - 1; i >= 0; --i)
        {
            const std::int32_t handle = data[i].weaponHandle;

            if (handle < 0)
                continue;

            C_BaseEntity* hudWeapon = I::GameEntity->Instance->Get<C_BaseEntity>(handle & ENT_ENTRY_MASK);

            if (hudWeapon == weapon)
            {
                ClearHudWeaponIconAtIndex(reinterpret_cast<std::uintptr_t>(selection), i, 0);
                return;
            }
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
    }
}

void SkinApplication::RegenerateSkins()
{
    using Fn = void(__fastcall*)();

    const Fn fn = M::ResolveCached([] { return reinterpret_cast<Fn>(M::scan("client.dll", Patterns::RegenerateSkins)); });

    if (fn)
        fn();
}
