#include "lineup.h"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <string_view>

#include "game_inventory.h"
#include "unboxer.h"
#include "../../../patterns.h"
#include "../../config/config.h"
#include "../../globals/globals.h"
#include "../../interfaces/interfaces.h"
#include "../../utils/memory/patternscan/patternscan.h"
#include "../../utils/schema/schema.h"
#include "../skinchanger/econ_item_attribute_manager.h"
#include "../skinchanger/item_schema.h"
#include "../skinchanger/knife_model_changer.h"
#include "../skinchanger/skin_application.h"
#include "../skinchanger/stattrak_tracker.h"
#include "../skinchanger/weapon_cosmetics.h"
#include "../../../cs2/entity/C_EconEntity/C_EconEntity.h"
#include "../../../cs2/entity/CCompositeMaterialOwner/CCompositeMaterialOwner.h"
#include "../../../cs2/entity/C_CSPlayerPawn/C_CSPlayerPawn.h"
#include "../../../cs2/entity/C_CSWeaponBase/C_CSWeaponBase.h"

namespace
{
    // client.dll, 2026-10-08 build: the builder (@+0x799640, C_CSGO_TeamPreviewCharacterPosition's vfunc 146 jumps to it
    // while the position wants a rebuild - once per lineup) reads the position like this:
    //   agent:  m_agentItem initialized -> its definition's model (else the controller's agent / a stock one)
    //   gloves: m_glovesItem initialized -> copied into the character's gloves item (else they're cleared)
    //   weapon: [team + 4] == -1 and m_weaponItem initialized -> that item; else m_sWeaponName's stock item
    //   team:   the int right after m_petItem (2 T, 3 CT)
    // After the team int: [team + 0x10] the character (a C_CSPlayerPawn: its gloves item is m_EconGloves, copied there
    // with m_bNeedToReApplyGloves raised), [team + 0x18] the gun - the weapon's own class (a C_CSWeaponBase), spawned
    // with the item, then BuildModernSkin(item, gun, 0, 2048, 0, 0, null, 1) once. The gun is cached by the item's id.
    //
    // A skin on it the way the skin changer does it in a match (lineup v1-v3 did less and showed no paint):
    //   - the item: our attributes, id high -1 (no inventory lookup), YOUR account id (the glove changer's; without it
    //     the gloves didn't even change model), m_bRestoreCustomMaterialAfterPrecache;
    //   - the gun: fallback paint fields, mesh group (old-model paint kits), BuildModernSkin, then RegenerateSkins
    //     (@+0x82A420: every C_CSWeaponBase with a skin material rebuilds it through the weapon path);
    //   - the gloves: m_bNeedToReApplyGloves + the game's glove routine run by us (the lineup character never runs it);
    //   - each step ONCE (ApplyStep below) - repeating them kept the knife's and the gloves' requests from ever finishing.
    constexpr std::uint32_t TEAM_TO_CHARACTER = 0x10;
    constexpr std::uint32_t TEAM_TO_WEAPON = 0x18;

    // BuildModernSkin @+0xE40450 (the skin changer's): (item, model, 0, 2048, econ icon, 0, null, 1) - the gun's skin
    // material (paint + stickers). RegenerateSkins @+0x82A420: () - every gun's skin material rebuilt.
    using BuildSkinFn = void(__fastcall*)(C_EconItemView* item, void* model, std::int64_t unknown, int size, char econIcon, char unknown2,
        void* unknown3, char unknown4);
    using RegenerateFn = void(__fastcall*)();

    constexpr int GAME_TEAM_T = 2;
    constexpr int GAME_TEAM_CT = 3;

    constexpr std::uint16_t DEF_KNIFE_CT = 42;
    constexpr std::uint16_t DEF_KNIFE_T = 59;
    constexpr std::uint16_t FIRST_KNIFE = 500;   // the knives you can own (the skin changer's rule too)

    constexpr int QUALITY_KEEP = -1;
    constexpr int QUALITY_NORMAL = 0;
    constexpr int QUALITY_UNUSUAL = 3;   // ★
    constexpr int QUALITY_STRANGE = 9;   // StatTrak
    constexpr int QUALITY_SOUVENIR = 12;

    constexpr std::uint32_t NO_SOC_ID_HIGH = 0xFFFFFFFFu;   // the item isn't looked up in the inventory (the skin changer's way)

    // The applies, once each and in order (v1-v7 repeated them: every RegenerateSkins re-queues EVERY gun's skin material
    // on the map and every glove rebuild throws the gloves' pending one away - the knife's and the gloves' never finished):
    enum ApplyStep : int
    {
        STEP_PAINT = 0,             // the first tick: the gun's skin (PaintGun), the gloves' rebuild flag
        STEP_REGENERATE,            // the next tick: the game's rebuild of the guns' skin materials, once
        STEP_GLOVES,                // STEP_GLOVES_MS: the game's glove routine, once (the lineup character doesn't run it)
        STEP_CHECK,                 // STEP_CHECK_MS: a gun / knife still without a finished skin material and nothing of yours
                                    // to copy from gets its request made again (the game's rebuild); the log line
        STEP_RECHECK,               // STEP_RECHECK_MS: the same once more (a sticker gun's first request can stay stuck)
        STEP_STATUS,                // STEP_STATUS_MS: what's finished by now, in the log
        STEP_DONE
    };

    constexpr ULONGLONG STEP_GLOVES_MS = 300;
    constexpr ULONGLONG STEP_CHECK_MS = 1500;
    constexpr ULONGLONG STEP_RECHECK_MS = 3000;
    constexpr ULONGLONG COPY_EVERY_MS = 300;   // a copy at most this often (something may reset what was copied)
    constexpr ULONGLONG STEP_STATUS_MS = 6000;
    constexpr ULONGLONG HOLD_MESH_MS = 60000;                    // an old-model skin's mesh is kept that long at most

    // CUtlVector's head (CAttributeList::m_Attributes): the count, then the data.
    struct VectorHead
    {
        std::int32_t size;
        std::int32_t pad;
        std::uintptr_t data;
    };

    // ── the position's fields (schema) ──
    struct Layout
    {
        std::uint32_t xuid = 0;
        std::uint32_t ordinal = 0;      // the character's place on the screen (only for the log; 0 = unknown)
        std::uint32_t weaponName = 0;   // CUtlString: a char*
        std::uint32_t agent = 0;
        std::uint32_t gloves = 0;
        std::uint32_t weapon = 0;
        std::uint32_t team = 0;         // 0 = not where it's expected: your live side is used
    };

    bool ReadLayout(Layout& out)
    {
        out.xuid = SchemaFinder::GetCached<hash_32_fnv1a_const("C_CSGO_TeamPreviewCharacterPosition->m_xuid")>();
        out.ordinal = SchemaFinder::GetCached<hash_32_fnv1a_const("C_CSGO_TeamPreviewCharacterPosition->m_nOrdinal")>();
        out.weaponName = SchemaFinder::GetCached<hash_32_fnv1a_const("C_CSGO_TeamPreviewCharacterPosition->m_sWeaponName")>();
        out.agent = SchemaFinder::GetCached<hash_32_fnv1a_const("C_CSGO_TeamPreviewCharacterPosition->m_agentItem")>();
        out.gloves = SchemaFinder::GetCached<hash_32_fnv1a_const("C_CSGO_TeamPreviewCharacterPosition->m_glovesItem")>();
        out.weapon = SchemaFinder::GetCached<hash_32_fnv1a_const("C_CSGO_TeamPreviewCharacterPosition->m_weaponItem")>();
        const std::uint32_t pet = SchemaFinder::GetCached<hash_32_fnv1a_const("C_CSGO_TeamPreviewCharacterPosition->m_petItem")>();

        if (!out.xuid || !out.weaponName || !out.agent || !out.gloves || !out.weapon || !pet)
            return false;

        // the four items sit back to back (0x5B0 apart on the 2026-10-08 build); the team is right after the last one
        const std::uint32_t stride = out.gloves > out.agent ? out.gloves - out.agent : 0;
        out.team = stride && out.weapon == out.gloves + stride && pet == out.weapon + stride ? pet + stride : 0;
        return true;
    }

    // ── you ──
    std::uint32_t g_accountId = 0;   // the builder's thread

    // Your Steam account id (the low half of your SteamID64), 0 if it can't be read yet.
    std::uint32_t LocalAccountId()
    {
        if (g_accountId)
            return g_accountId;

        std::uint32_t id = GameInv::AccountId();

        // no inventory yet: your controller's m_steamID
        const std::uint32_t steamIdOffset = SchemaFinder::GetCached<hash_32_fnv1a_const("CBasePlayerController->m_steamID")>();
        C_CSPlayerPawn* pawn = g_ctx ? g_ctx->local_pawn : nullptr;

        if (!id && pawn && steamIdOffset && I::GameEntity && I::GameEntity->Instance)
        {
            if (auto* controller = reinterpret_cast<const std::uint8_t*>(I::GameEntity->Instance->Get<C_BaseEntity>(pawn->m_hController())))
                id = static_cast<std::uint32_t>(*reinterpret_cast<const std::uint64_t*>(controller + steamIdOffset) & 0xFFFFFFFFull);
        }

        g_accountId = id;
        return id;
    }

    // ── what Lootline has for the side ──
    struct Wanted
    {
        bool enabled = false;
        std::uint16_t agent = 0;
        std::uint16_t glove = 0;
        bool gloveSkin = false;
        Config::item_skin_t gloveConfig{};
        std::uint16_t knife = 0;
        bool knifeSkin = false;
        Config::item_skin_t knifeConfig{};
        bool gunSkin = false;
        Config::item_skin_t gunConfig{};
    };

    // The skin changer's test for "this gun has something of ours".
    bool HasGunSkin(const Config::item_skin_t& config)
    {
        return config.enabled && (config.paintKit > 0 || config.statTrakEnabled || !config.customName.empty() || WeaponCosmetics::HasAny(config));
    }

    // From what the menu published last (any thread: its lock, our memory only - nothing of the game's is read under it).
    bool TakeWanted(int side, std::uint16_t gun, Wanted& out)
    {
        if (Config::detail::publishedVersion.load(std::memory_order_acquire) == 0)
            return false;

        std::lock_guard lock(Config::detail::publishMutex);
        const Config::skins_t& skins = Config::detail::published;
        out.enabled = skins.enabled;

        if (!out.enabled)
            return true;

        const Config::skin_changer_t& loadout = skins.loadouts[side == Config::TEAM_CT ? Config::TEAM_CT : Config::TEAM_T];
        out.agent = loadout.agentDefinition;
        out.glove = loadout.gloveDefinition;
        out.knife = loadout.knifeDefinition >= FIRST_KNIFE ? loadout.knifeDefinition : 0;

        if (out.glove)
        {
            const auto it = loadout.gloves.find(out.glove);

            if (it != loadout.gloves.end() && it->second.enabled && it->second.paintKit > 0)
            {
                out.gloveSkin = true;
                out.gloveConfig = it->second;
            }
        }

        if (out.knife)
        {
            const auto it = loadout.knives.find(out.knife);

            if (it != loadout.knives.end() && it->second.enabled && it->second.paintKit > 0)
            {
                out.knifeSkin = true;
                out.knifeConfig = it->second;
            }
        }

        if (gun)
        {
            const auto it = loadout.weapons.find(gun);

            if (it != loadout.weapons.end() && HasGunSkin(it->second))
            {
                out.gunSkin = true;
                out.gunConfig = it->second;
            }
        }

        return true;
    }

    // ── the item schema ──
    const ItemDefinitionInfo* ItemInfo(std::uint16_t definition)
    {
        return definition && Skins::g_ItemSchema.IsInitialized() ? Skins::g_ItemSchema.GetItem(definition) : nullptr;
    }

    bool IsKnife(std::uint16_t definition, std::string_view weaponName)
    {
        if (definition == DEF_KNIFE_CT || definition == DEF_KNIFE_T)
            return true;

        if (const ItemDefinitionInfo* info = ItemInfo(definition))
            return info->isKnife;

        return weaponName.starts_with("weapon_knife") || weaponName.starts_with("weapon_bayonet");
    }

    // "weapon_ak47" -> 7 (a stock gun: the position names it, its item isn't initialized)
    std::uint16_t GunFromName(std::string_view weaponName)
    {
        if (weaponName.empty() || !Skins::g_ItemSchema.IsInitialized())
            return 0;

        for (const ItemDefinitionInfo& item : Skins::g_ItemSchema.GetItems())
            if (item.isWeapon && item.weaponName == weaponName)
                return item.definitionIndex;

        return 0;
    }

    bool AgentFits(std::uint16_t definition, int side)
    {
        const ItemDefinitionInfo* info = ItemInfo(definition);

        if (!info)
            return Skins::g_ItemSchema.IsInitialized() ? false : true;   // not in the schema / no schema yet: trust the config

        return info->isAgent && (side == Config::TEAM_CT ? info->isCounterTerroristAgent : info->isTerroristAgent);
    }

    bool IsGlove(std::uint16_t definition)
    {
        const ItemDefinitionInfo* info = ItemInfo(definition);
        return info ? info->isGlove : !Skins::g_ItemSchema.IsInitialized();
    }

    // ── dressing an item ──
    struct Look
    {
        std::uint16_t definition = 0;
        int quality = QUALITY_KEEP;
        int paintKit = 0;
        float wear = 0.0001f;
        int seed = 0;
        int statTrak = -1;
        bool extras = false;   // stickers + charm (guns)
        EconItemAttributeManager::StickerArray stickers{};
        EconItemAttributeManager::KeychainData keychain{};
        char customName[64]{};   // the name tag (guns / knives): the item's m_szCustomName, CS2's tag model on the gun
    };

    void Hash(std::uint64_t& hash, const void* data, std::size_t size)
    {
        const auto* bytes = static_cast<const std::uint8_t*>(data);

        for (std::size_t i = 0; i < size; ++i)
        {
            hash ^= bytes[i];
            hash *= 1099511628211ULL;
        }
    }

    // field by field (the structs have padding after their bools)
    std::uint64_t Signature(const Look& look)
    {
        std::uint64_t hash = 14695981039346656037ULL;
        Hash(hash, &look.definition, sizeof(look.definition));
        Hash(hash, &look.quality, sizeof(look.quality));
        Hash(hash, &look.paintKit, sizeof(look.paintKit));
        Hash(hash, &look.wear, sizeof(look.wear));
        Hash(hash, &look.seed, sizeof(look.seed));
        Hash(hash, &look.statTrak, sizeof(look.statTrak));
        Hash(hash, &look.extras, sizeof(look.extras));

        if (!look.extras)
            return hash;

        for (const EconItemAttributeManager::StickerData& sticker : look.stickers)
        {
            Hash(hash, &sticker.enabled, sizeof(sticker.enabled));
            Hash(hash, &sticker.kit, sizeof(sticker.kit));
            Hash(hash, &sticker.wear, sizeof(sticker.wear));
            Hash(hash, &sticker.scale, sizeof(sticker.scale));
            Hash(hash, &sticker.rotation, sizeof(sticker.rotation));
            Hash(hash, &sticker.offsetX, sizeof(sticker.offsetX));
            Hash(hash, &sticker.offsetY, sizeof(sticker.offsetY));
            Hash(hash, &sticker.schema, sizeof(sticker.schema));
        }

        const EconItemAttributeManager::KeychainData& keychain = look.keychain;
        Hash(hash, &keychain.enabled, sizeof(keychain.enabled));
        Hash(hash, &keychain.id, sizeof(keychain.id));
        Hash(hash, &keychain.seed, sizeof(keychain.seed));
        Hash(hash, &keychain.offsetX, sizeof(keychain.offsetX));
        Hash(hash, &keychain.offsetY, sizeof(keychain.offsetY));
        Hash(hash, &keychain.offsetZ, sizeof(keychain.offsetZ));
        Hash(hash, &keychain.sticker, sizeof(keychain.sticker));
        Hash(hash, look.customName, sizeof(look.customName));
        return hash;
    }

    // The game's skin material manager caches by the material's inputs (paint, wear, seed...): a lineup item exactly like
    // your in-match one waits on the in-match one's material - your holstered knife's / your third-person gloves' are
    // hardly ever drawn, so it stayed pending through the whole lineup (v7-v10 logs). A wear 0.0003 apart (invisible)
    // makes the lineup's its own material, made for a model that's on screen.
    constexpr float LINEUP_WEAR_NUDGE = 0.0003f;

    float LineupWear(float wear)
    {
        wear = std::clamp(wear, 0.0001f, 1.0f);
        return wear + LINEUP_WEAR_NUDGE <= 1.0f ? wear + LINEUP_WEAR_NUDGE : wear - LINEUP_WEAR_NUDGE;
    }

    // A skin's numbers as the skin changer applies them (SkinApplication::Apply), the wear nudged (LineupWear).
    void TakeSkin(Look& look, std::uint16_t definition, const Config::item_skin_t& skin, bool extras)
    {
        look.paintKit = skin.paintKit;
        look.wear = LineupWear(skin.wear);
        look.seed = std::clamp(skin.seed, 0, 1000);
        look.statTrak = skin.statTrakEnabled ? (std::max)(0, StatTrakTracker::Displayed(definition, skin)) : -1;
        look.extras = extras;
        strncpy_s(look.customName, sizeof(look.customName), skin.customName.c_str(), _TRUNCATE);   // guns and knives

        if (!extras)
            return;

        for (std::size_t i = 0; i < look.stickers.size() && i < skin.stickers.size(); ++i)
        {
            look.stickers[i].enabled = skin.stickers[i].enabled;
            look.stickers[i].kit = skin.stickers[i].kit;
            look.stickers[i].wear = skin.stickers[i].wear;
            look.stickers[i].scale = skin.stickers[i].scale;
            look.stickers[i].rotation = skin.stickers[i].rotation;
            look.stickers[i].offsetX = skin.stickers[i].offsetX;
            look.stickers[i].offsetY = skin.stickers[i].offsetY;
            look.stickers[i].schema = skin.stickers[i].schema;
        }

        look.keychain.enabled = skin.keychain.enabled && skin.keychain.id > 0;
        look.keychain.id = skin.keychain.id;
        look.keychain.seed = skin.keychain.seed;
        look.keychain.sticker = skin.keychain.sticker;
        look.keychain.offsetX = skin.keychain.offsetX;
        look.keychain.offsetY = skin.keychain.offsetY;
        look.keychain.offsetZ = skin.keychain.offsetZ;
    }

    // The builder runs again on some updates: an item already dressed (and not reset by the server since) is left alone.
    struct Dressed
    {
        std::uintptr_t item = 0;
        std::uint64_t signature = 0;
        std::uintptr_t attributes = 0;   // the attribute list made for it (the server replacing it = dress again)
        std::uint32_t idLow = 0;         // the item id's low half it got (LineupItemIdLow)
    };

    std::array<Dressed, 32> g_dressed{};   // the builder's thread
    std::size_t g_dressedNext = 0;

    const VectorHead* AttributesOf(C_EconItemView* item)
    {
        CAttributeList* list = item->m_AttributeList();
        return list ? reinterpret_cast<const VectorHead*>(&list->m_Attributes()) : nullptr;
    }

    std::uint32_t* ItemAccountId(C_EconItemView* item)
    {
        const std::uint32_t offset = SchemaFinder::GetCached<hash_32_fnv1a_const("C_EconItemView->m_iAccountID")>();
        return offset ? reinterpret_cast<std::uint32_t*>(reinterpret_cast<std::uint8_t*>(item) + offset) : nullptr;
    }

    std::uint32_t* ItemIdLow(C_EconItemView* item)
    {
        const std::uint32_t offset = SchemaFinder::GetCached<hash_32_fnv1a_const("C_EconItemView->m_iItemIDLow")>();
        return offset ? reinterpret_cast<std::uint32_t*>(reinterpret_cast<std::uint8_t*>(item) + offset) : nullptr;
    }

    std::uint64_t* ItemIdFull(C_EconItemView* item)
    {
        const std::uint32_t offset = SchemaFinder::GetCached<hash_32_fnv1a_const("C_EconItemView->m_iItemID")>();
        return offset ? reinterpret_cast<std::uint64_t*>(reinterpret_cast<std::uint8_t*>(item) + offset) : nullptr;
    }

    // Every dressed lineup item gets its OWN item id. The skin / glove changers keep the real item's low half (only the
    // high one is -1), so your lineup knife / gloves had the very same id as your in-match ones - and the game's skin
    // material for that id is the in-match one's: your holstered knife's / third-person gloves', hardly ever drawn, so it
    // stayed pending through the whole lineup (guns you don't carry had no twin and always worked). The working
    // server-side lineup plugins give each one a fresh id too.
    std::uint32_t g_lineupIdLow = 0x7E000000u;   // the builder's thread

    std::uint32_t NextLineupIdLow()
    {
        g_lineupIdLow = g_lineupIdLow >= 0x7EFFFFFFu ? 0x7E000000u : g_lineupIdLow + 1;
        return g_lineupIdLow;
    }

    bool AlreadyDressed(C_EconItemView* item, const Look& look, std::uint64_t signature)
    {
        for (const Dressed& dressed : g_dressed)
        {
            if (dressed.item != reinterpret_cast<std::uintptr_t>(item))
                continue;

            const VectorHead* head = AttributesOf(item);
            const std::uint32_t* account = ItemAccountId(item);
            return dressed.signature == signature && item->m_bInitialized() && item->m_iItemDefinitionIndex() == look.definition &&
                item->m_iItemIDHigh() == NO_SOC_ID_HIGH && ItemIdLow(item) && *ItemIdLow(item) == dressed.idLow &&
                head && head->data == dressed.attributes &&
                (!account || *account == LocalAccountId()) && !EconItemAttributeManager::HasNetworkedAttributes(item);
        }

        return false;
    }

    void Remember(C_EconItemView* item, std::uint64_t signature)
    {
        const VectorHead* head = AttributesOf(item);
        Dressed* slot = nullptr;

        for (Dressed& dressed : g_dressed)
            if (dressed.item == reinterpret_cast<std::uintptr_t>(item))
                slot = &dressed;

        if (!slot)
        {
            slot = &g_dressed[g_dressedNext];
            g_dressedNext = (g_dressedNext + 1) % g_dressed.size();
        }

        slot->item = reinterpret_cast<std::uintptr_t>(item);
        slot->signature = signature;
        slot->attributes = head ? head->data : 0;
        const std::uint32_t* low = ItemIdLow(item);
        slot->idLow = low ? *low : 0;
    }

    // The item becomes the look the way the skin / glove changers make a match item ours: the real item's networked
    // extras (stickers, charm, StatTrak, patches) gone, no inventory lookup (id high -1; the low half stays), your
    // account as its owner, our attributes. True = it changed now.
    bool Dress(C_EconItemView* item, const Look& look)
    {
        const std::uint64_t signature = Signature(look);

        if (AlreadyDressed(item, look, signature))
            return false;

        EconItemAttributeManager::ClearNetworkedAttributes(item);
        EconItemAttributeManager::Remove(item);

        item->m_iItemDefinitionIndex() = look.definition;
        item->m_iItemIDHigh() = NO_SOC_ID_HIGH;

        if (std::uint32_t* low = ItemIdLow(item))
        {
            *low = NextLineupIdLow();

            if (std::uint64_t* full = ItemIdFull(item))
                *full = (static_cast<std::uint64_t>(NO_SOC_ID_HIGH) << 32) | *low;
        }
        item->m_bDisallowSOC() = false;
        item->m_bRestoreCustomMaterialAfterPrecache() = true;   // its skin material is built again once its model loaded

        if (std::uint32_t* account = ItemAccountId(item))
        {
            if (const std::uint32_t id = LocalAccountId())
                *account = id;
        }

        if (look.quality != QUALITY_KEEP)
            item->m_iEntityQuality() = look.quality;
        else if (!item->m_bInitialized())
            item->m_iEntityQuality() = QUALITY_NORMAL;

        item->m_bInitialized() = true;

        EconItemAttributeManager::Create(item, look.paintKit, look.wear, look.seed, look.statTrak, 0,
            look.extras ? &look.stickers : nullptr, look.extras ? &look.keychain : nullptr);

        // the name tag (empty = none: a real item's own name doesn't stay on our look)
        if (char* name = item->m_szCustomName())
            strncpy_s(name, 161, look.customName, _TRUNCATE);

        Remember(item, signature);
        return true;
    }

    // ── the log: what changed, the first lines of a session ──
    std::atomic_int g_linesLogged{ 0 };   // the builder's and the frame-stage thread
    constexpr int MAX_LINES = 80;

    void Log(const char* text)
    {
        const int line = g_linesLogged.fetch_add(1, std::memory_order_relaxed) + 1;

        if (line <= MAX_LINES)
            Unboxer::LogLine(text);
        else if (line == MAX_LINES + 1)
            Unboxer::LogLine("lineup: (no more lineup lines this session)");
    }

    struct Before
    {
        bool initialized = false;
        std::uint16_t definition = 0;
    };

    Before Read(C_EconItemView* item)
    {
        return { item->m_bInitialized(), item->m_iItemDefinitionIndex() };
    }

    // What Patch decided for the position being built right now (the builder's thread), for AfterBuild.
    struct Decided
    {
        void* position = nullptr;
        bool gun = false;               // the gun is ours (a Lootline skin / knife)
        Look gunLook{};
        std::uint16_t gloves = 0;       // our gloves' definition, 0 = not ours
    };

    Decided g_decided;

    // Your position gets your Lootline items. Non-inline: the SEH wrapper below can't hold C++ objects.
    // The side pick screen's characters (the map's team_select_terrorist / _counterterrorist): GAME_TEAM_T / _CT, 0 = another
    // lineup. The server often doesn't say whose they are (no xuid; the game falls back to SAS / Phoenix) - see Patch for
    // when one counts as you.
    int SidePickTeam(void* position)
    {
        const char* name = static_cast<CEntityInstance*>(position)->get_entity_class_name();

        if (!name)
            return 0;

        if (std::strcmp(name, "C_CSGO_TeamSelectTerroristPosition") == 0)
            return GAME_TEAM_T;

        return std::strcmp(name, "C_CSGO_TeamSelectCounterTerroristPosition") == 0 ? GAME_TEAM_CT : 0;
    }

    // The side pick screen, a character with no xuid (the game doesn't say whose it is). ONE model shows your items (user,
    // 2026-10-22: four T players all wore your T agent and skins), the rest stay the game's: only on your side - and with
    // other players (not bots) there, only the first place (ordinal 0); alone there, any (the screen shows one character:
    // you). Player controllers are entities 1 - 64.
    struct SideCount
    {
        bool you = false;
        int others = 0;      // other players on that side (bots aren't counted)
    };

    SideCount CountSide(int gameTeam)
    {
        SideCount count;
        const std::uint32_t teamOffset = SchemaFinder::GetCached<hash_32_fnv1a_const("C_BaseEntity->m_iTeamNum")>();
        const std::uint32_t steamIdOffset = SchemaFinder::GetCached<hash_32_fnv1a_const("CBasePlayerController->m_steamID")>();
        const std::uint32_t mine = LocalAccountId();

        if (!teamOffset || !steamIdOffset || !mine || !I::GameEntity || !I::GameEntity->Instance)
            return count;

        bool& you = count.you;
        int& others = count.others;

        for (int index = 1; index <= 64; ++index)
        {
            const auto* controller = reinterpret_cast<const std::uint8_t*>(I::GameEntity->Instance->Get<C_BaseEntity>(index));

            if (!controller || controller[teamOffset] != gameTeam)
                continue;

            const std::uint64_t steamId = *reinterpret_cast<const std::uint64_t*>(controller + steamIdOffset);

            if (steamId == 0)
                continue;   // a bot

            if (static_cast<std::uint32_t>(steamId & 0xFFFFFFFFull) == mine)
                you = true;
            else
                ++others;
        }

        return count;
    }

    // "left as it is" lines for side pick characters: at most one per side every 10 s (the builder runs per pose)
    ULONGLONG g_sidePickSkipLogged[2]{};

    void LogSidePickSkip(int gameTeam, std::uint64_t xuid, int ordinal, const char* why)
    {
        ULONGLONG& last = g_sidePickSkipLogged[gameTeam == GAME_TEAM_CT ? 1 : 0];
        const ULONGLONG now = GetTickCount64();

        if (last != 0 && now - last < 10000)
            return;

        last = now;
        char line[200];
        snprintf(line, sizeof(line), "lineup: side pick screen - a %s character left as it is (xuid %llX, place %d): %s",
            gameTeam == GAME_TEAM_CT ? "CT" : "T", static_cast<unsigned long long>(xuid), ordinal, why);
        Log(line);
    }

    __declspec(noinline) void Patch(void* position, const Layout& layout)
    {
        auto* base = static_cast<std::uint8_t*>(position);
        const int sidePick = SidePickTeam(position);
        const std::uint64_t xuid = *reinterpret_cast<const std::uint64_t*>(base + layout.xuid);
        const int ordinal = layout.ordinal ? *reinterpret_cast<const int*>(base + layout.ordinal) : -1;

        if (!sidePick || xuid != 0)
        {
            // a lineup character, or a side pick one that says whose it is: yours only
            const std::uint32_t mine = xuid ? LocalAccountId() : 0;

            if (!mine || static_cast<std::uint32_t>(xuid & 0xFFFFFFFFull) != mine)
            {
                if (sidePick)
                    LogSidePickSkip(sidePick, xuid, ordinal, "another player's");

                return;   // someone else (or a bot)
            }
        }
        else
        {
            const SideCount count = CountSide(sidePick);

            if (!count.you)
            {
                LogSidePickSkip(sidePick, 0, ordinal, "not your side");
                return;
            }

            if (count.others > 0 && ordinal != 0)
            {
                LogSidePickSkip(sidePick, 0, ordinal, "other players on your side - only the first place shows your items");
                return;
            }
        }

        // the position's side (the side pick screen: its class); unknown: the side you play
        const int gameTeam = sidePick ? sidePick : layout.team ? *reinterpret_cast<const int*>(base + layout.team) : 0;
        const int side = gameTeam == GAME_TEAM_CT ? Config::TEAM_CT
            : gameTeam == GAME_TEAM_T ? Config::TEAM_T
            : Config::liveTeam.load(std::memory_order_relaxed) == Config::TEAM_CT ? Config::TEAM_CT : Config::TEAM_T;
        const int weaponMode = layout.team ? *reinterpret_cast<const int*>(base + layout.team + 4) : -1;   // -1: the item is shown

        auto* agent = reinterpret_cast<C_EconItemView*>(base + layout.agent);
        auto* gloves = reinterpret_cast<C_EconItemView*>(base + layout.gloves);
        auto* weapon = reinterpret_cast<C_EconItemView*>(base + layout.weapon);

        const char* weaponText = *reinterpret_cast<const char* const*>(base + layout.weaponName);
        const std::string_view weaponName(weaponText ? weaponText : "");

        // the weapon this character holds: its item's, or the stock one the position names
        const Before weaponBefore = Read(weapon);
        const std::uint16_t heldDefinition = weaponBefore.initialized ? weaponBefore.definition : 0;
        const bool knife = IsKnife(heldDefinition, weaponName);
        const std::uint16_t gun = knife ? 0 : heldDefinition ? heldDefinition : GunFromName(weaponName);

        Wanted wanted;

        if (!TakeWanted(side, gun, wanted) || !wanted.enabled)
            return;

        g_decided.position = position;   // yours: AfterBuild queues the character and its gun
        const Before agentBefore = Read(agent);
        const Before glovesBefore = Read(gloves);
        bool agentDressed = false;
        bool glovesDressed = false;
        bool weaponDressed = false;
        int weaponPaint = 0;

        if (wanted.agent && AgentFits(wanted.agent, side))
        {
            Look look;
            look.definition = wanted.agent;
            agentDressed = Dress(agent, look);
        }

        if (wanted.glove && IsGlove(wanted.glove))
        {
            Look look;
            look.definition = wanted.glove;
            look.quality = QUALITY_UNUSUAL;

            if (wanted.gloveSkin)
            {
                TakeSkin(look, wanted.glove, wanted.gloveConfig, false);
                look.statTrak = -1;   // gloves have no StatTrak
            }

            glovesDressed = Dress(gloves, look);
            g_decided.gloves = wanted.glove;
        }

        if (knife && wanted.knife)
        {
            Look look;
            look.definition = wanted.knife;
            look.quality = QUALITY_UNUSUAL;

            if (wanted.knifeSkin)
            {
                TakeSkin(look, wanted.knife, wanted.knifeConfig, false);

                if (wanted.knifeConfig.statTrakEnabled)
                    look.quality = QUALITY_STRANGE;
            }

            weaponPaint = look.paintKit;
            weaponDressed = Dress(weapon, look);
            g_decided.gun = true;
            g_decided.gunLook = look;
        }
        else if (gun && wanted.gunSkin)
        {
            Look look;
            look.definition = gun;
            look.quality = wanted.gunConfig.statTrakEnabled ? QUALITY_STRANGE : wanted.gunConfig.souvenir ? QUALITY_SOUVENIR : QUALITY_NORMAL;
            TakeSkin(look, gun, wanted.gunConfig, true);
            weaponPaint = look.paintKit;
            weaponDressed = Dress(weapon, look);
            g_decided.gun = true;
            g_decided.gunLook = look;
        }

        if (!agentDressed && !glovesDressed && !weaponDressed)
            return;

        char line[400];
        snprintf(line, sizeof(line),
            "lineup: your %s character%s - agent %u%s -> %s, gloves %u%s -> %s, weapon \"%.40s\" %u%s -> %s (paint %d, name tag \"%.32s\")%s",
            side == Config::TEAM_CT ? "CT" : "T", sidePick ? (xuid ? " (side pick screen, yours by xuid)" : " (side pick screen, your side)") : "",
            static_cast<unsigned>(agentBefore.definition), agentBefore.initialized ? "" : " (none)",
            agentDressed ? "Lootline" : "kept",
            static_cast<unsigned>(glovesBefore.definition), glovesBefore.initialized ? "" : " (none)",
            glovesDressed ? "Lootline" : "kept",
            weaponText ? weaponText : "", static_cast<unsigned>(weaponBefore.definition), weaponBefore.initialized ? "" : " (none)",
            weaponDressed ? "Lootline" : "kept", weaponPaint, g_decided.gunLook.customName,
            weaponMode == -1 ? "" : " (this screen shows no weapon item)");
        Log(line);
    }

    bool CallPatch(void* position, const Layout& layout)
    {
        __try
        {
            Patch(position, layout);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    // ── after the build: your character and its gun, skinned the skin changer's way ──
    BuildSkinFn BuildSkinFunction()
    {
        return M::ResolveCached([] { return reinterpret_cast<BuildSkinFn>(M::scan("client.dll", Patterns::BuildModernSkin)); });
    }

    RegenerateFn RegenerateFunction()
    {
        return M::ResolveCached([] { return reinterpret_cast<RegenerateFn>(M::scan("client.dll", Patterns::RegenerateSkins)); });
    }

    // 2 = the old model (a paint kit made for the CS:GO model), 1 = the CS2 one (the skin changer's GetMeshMask)
    std::uint64_t MeshMask(int paintKit)
    {
        const PaintKitInfo* info = paintKit > 0 && Skins::g_ItemSchema.IsInitialized() ? Skins::g_ItemSchema.GetPaintKit(paintKit) : nullptr;
        return info && info->usesOldModel ? 2ULL : 1ULL;
    }

    // CEntityIdentity (+0x10) -> its handle (+0x10): the entity's index, -1 if unreadable.
    int EntityIndex(void* entity)
    {
        const auto* identity = *reinterpret_cast<const std::uint8_t* const*>(static_cast<const std::uint8_t*>(entity) + 0x10);
        return identity ? static_cast<int>(*reinterpret_cast<const std::uint32_t*>(identity + 0x10) & 0x7FFFu) : -1;
    }

    // Still the entity at that index (the lineup didn't end, the slot wasn't reused for something else).
    bool EntityAlive(void* entity, int index)
    {
        return entity && index >= 0 && I::GameEntity && I::GameEntity->Instance &&
            I::GameEntity->Instance->Get<C_BaseEntity>(index) == entity;
    }

    // The gun's own item (its copy of the position's), null if it isn't our dressed one.
    C_EconItemView* OurGunItem(void* gun, const Look& look)
    {
        C_AttributeContainer* container = static_cast<C_EconEntity*>(gun)->m_AttributeManager();
        C_EconItemView* item = container ? container->m_Item() : nullptr;

        if (!item || item->m_iItemDefinitionIndex() != look.definition || item->m_iItemIDHigh() != NO_SOC_ID_HIGH)
            return nullptr;

        return item;
    }

    // SkinApplication::Apply + ApplyWeaponMaterial on a match gun: the fallback paint, the mesh, the skin material.
    bool PaintGun(void* gun, const Look& look)
    {
        C_EconItemView* item = OurGunItem(gun, look);
        const BuildSkinFn build = BuildSkinFunction();

        if (!item || !build)
            return false;

        auto* econ = static_cast<C_EconEntity*>(gun);
        econ->m_nFallbackPaintKit() = look.paintKit;
        econ->m_nFallbackSeed() = look.seed;
        econ->m_flFallbackWear() = look.wear;
        econ->m_nFallbackStatTrak() = look.statTrak;
        item->m_bRestoreCustomMaterialAfterPrecache() = true;

        if (CGameSceneNode* node = static_cast<C_BaseEntity*>(gun)->m_pGameSceneNode())
            node->SetMeshGroupMask(MeshMask(look.paintKit));

        build(item, gun, 0, 2048, 0, 0, nullptr, 1);

        // CS2's name tag on the gun, like in a match (SkinApplication's UpdateNametag): it reads the item's custom name -
        // the same name already on = nothing, none = no tag. Once the mesh is set; every apply is fine.
        using AddNametagFn = char(__fastcall*)(C_BaseEntity*, C_EconItemView*);
        const auto addNametag = M::ResolveCached([] { return reinterpret_cast<AddNametagFn>(M::scan("client.dll", Patterns::AddNametagEntity)); });

        if (addNametag)
            addNametag(static_cast<C_BaseEntity*>(gun), item);

        return true;
    }

    // The entity's mesh group mask (its skeleton's model state), 0 if unreadable.
    std::uint64_t ReadMeshMask(void* entity)
    {
        const std::uint32_t modelState = SchemaFinder::GetCached<hash_32_fnv1a_const("CSkeletonInstance->m_modelState")>();
        const std::uint32_t mask = SchemaFinder::GetCached<hash_32_fnv1a_const("CModelState->m_MeshGroupMask")>();
        CGameSceneNode* node = static_cast<C_BaseEntity*>(entity)->m_pGameSceneNode();

        if (!node || !modelState || !mask)
            return 0;

        return *reinterpret_cast<const std::uint64_t*>(reinterpret_cast<const std::uint8_t*>(node) + modelState + mask);
    }

    // For the log: the paint kit an item reads (item vfunc 3, "set item texture prefab") and whether the game's
    // paint kit table has it (@+0x118B800, what BuildModernSkin and the glove code start from).
    void DescribePaint(C_EconItemView* item, char* out, std::size_t size)
    {
        using PaintKitFn = void*(__fastcall*)(void* item);
        const auto paintKit = M::ResolveCached([] { return reinterpret_cast<PaintKitFn>(M::scan("client.dll", Patterns::GloveCreatePaintKit)); });
        const VectorHead* head = AttributesOf(item);
        const std::uint32_t* account = ItemAccountId(item);

        snprintf(out, size, "def %u, reads paint %d (kit %s), %d attributes, id high %08X, account %s",
            static_cast<unsigned>(item->m_iItemDefinitionIndex()), M::CallVFunc<int, 3U>(item),
            paintKit ? (paintKit(item) ? "found" : "NOT found") : "?", head ? head->size : -1, item->m_iItemIDHigh(),
            account && *account == g_accountId && g_accountId ? "yours" : "other");
    }

    // One lineup character waiting for its applies.
    struct Pending
    {
        std::uint32_t serial = 0;       // 0 = a free slot
        void* character = nullptr;
        int characterIndex = -1;
        std::uint16_t gloves = 0;       // our gloves' definition, 0 = not ours
        void* gun = nullptr;
        int gunIndex = -1;
        bool gunOurs = false;
        Look gunLook{};
        ULONGLONG createdAt = 0;
        int step = STEP_PAINT;          // ApplyStep
        bool gunPainted = false;        // STEP_PAINT painted the gun: STEP_REGENERATE runs the game's rebuild
        int gunApplies = 0;
        int gloveApplies = 0;
        bool applied = false;           // the applies are done (the log line written)
        std::uint64_t mesh = 0;         // the gun's mesh group to keep (2 = old model), 0 = nothing to keep
        int meshResets = 0;             // times something else changed it back
        int flagAtFirst = -1;           // the gloves' rebuild flag before our first apply (0 = the game used the builder's)
        int directRebuilds = 0;         // the glove routine run by us
        int flagAfterDirect = -1;       // the flag right after it (0 = it ran through)
        int copyResult = 0;             // CopyResult: the last try at copying your in-match gloves' paint
        int copies = 0;
        std::uint32_t copiedTo = 0xFFFFFFFFu;   // the lineup glove model (handle) it was copied onto last
        bool copyLogged = false;
        bool characterGone = false;     // the lineup character went away: nothing more to copy onto
        ULONGLONG glovesCopiedAt = 0;
        int gunCopyResult = 0;          // CopyResult: your in-match weapon's skin material onto the lineup gun / knife
        int gunCopies = 0;
        ULONGLONG gunCopiedAt = 0;
        bool gunCopyLogged = false;
        int sourceFinished = -1;        // your in-match one's skin materials, the last look (for the log)
        int sourceWaiting = -1;
        bool gunGone = false;
        int gunUnshared = 0;            // Waited: the lineup gun / knife's skin waited for an identical one (made on its own)
        int glovesUnshared = 0;
        bool gunUnshareLogged = false;
        bool glovesUnshareLogged = false;
    };

    // Copying your in-match gloves' finished paint onto the lineup gloves (the lineup's own glove paint never shows).
    enum CopyResult : int
    {
        COPY_NOT_TRIED = 0,
        COPY_DONE,
        COPY_NO_FUNCTION,           // a signature is broken
        COPY_NO_LOCAL,              // you have no player right now (dead, spectating)
        COPY_DIFFERENT,             // your in-match gloves aren't these (another side's loadout)
        COPY_NO_LOCAL_MODEL,        // your player has no glove model
        COPY_NO_LINEUP_MODEL,       // the lineup character has no glove model (yet)
        COPY_NO_OWNER,              // a glove model without a skin material owner
        COPY_LOCAL_NOT_READY,       // your in-match gloves' paint isn't finished (yet)
    };

    const char* CopyResultText(int result)
    {
        switch (result)
        {
        case COPY_DONE: return "copied";
        case COPY_NO_FUNCTION: return "signature missing";
        case COPY_NO_LOCAL: return "no in-match player";
        case COPY_DIFFERENT: return "your in-match gloves are other gloves";
        case COPY_NO_LOCAL_MODEL: return "your in-match player has no glove model";
        case COPY_NO_LINEUP_MODEL: return "the lineup character has no glove model";
        case COPY_NO_OWNER: return "a glove model without a skin material owner";
        case COPY_LOCAL_NOT_READY: return "your in-match one's paint isn't finished";
        default: return "not tried";
        }
    }

    // The copy is tried every frame while the lineup character is there (at most this long): your in-match gloves are
    // rebuilt at every spawn and their paint is pending for a while - the copy happens once it's finished.
    constexpr ULONGLONG COPY_GLOVES_MS = 60000;

    // The glove routine and the pawn's glove helper it takes, from the pawn update's call of it. The frame-stage thread.
    using GloveRoutineFn = void(__fastcall*)(void* helper);

    struct GloveUpdate
    {
        bool resolved = false;
        GloveRoutineFn routine = nullptr;
        std::uint32_t helper = 0;       // pawn + this = the glove helper (+0x15F8 on the 2026-10-08 build)
    };

    GloveUpdate g_gloveUpdate;

    const GloveUpdate& GloveUpdateFunction()
    {
        if (g_gloveUpdate.resolved)
            return g_gloveUpdate;

        g_gloveUpdate.resolved = true;   // one scan: a broken signature stays broken (Settings -> Check signatures)

        if (const std::uint8_t* site = M::FindPattern("client.dll", Patterns::LineupGloveUpdate))
        {
            std::int32_t helper = 0;
            std::int32_t relative = 0;
            std::memcpy(&helper, site + 3, sizeof(helper));
            std::memcpy(&relative, site + 8, sizeof(relative));

            if (helper > 0 && helper < 0x4000)
            {
                g_gloveUpdate.helper = static_cast<std::uint32_t>(helper);
                g_gloveUpdate.routine = reinterpret_cast<GloveRoutineFn>(site + 12 + relative);
            }
        }

        if (!g_gloveUpdate.routine)
            Unboxer::LogLine("lineup: the glove routine wasn't found (LineupGloveUpdate) - lineup gloves only get the rebuild flag");

        return g_gloveUpdate;
    }

    // A skin material owner's finished materials onto another owner (the first-person gun's way). The frame-stage thread.
    using CompositeCopyFn = void(__fastcall*)(void* source, void* destination, char releaseFirst);

    struct CompositeCopy
    {
        bool resolved = false;
        CompositeCopyFn copy = nullptr;
        std::uint32_t count = 0;        // owner + this = its finished materials' count (+0x4A0); + 0x18 = the pending ones
    };

    CompositeCopy g_compositeCopy;

    const CompositeCopy& CompositeCopyFunction()
    {
        if (g_compositeCopy.resolved)
            return g_compositeCopy;

        g_compositeCopy.resolved = true;

        if (std::uint8_t* site = M::FindPattern("client.dll", Patterns::LineupCompositeCopy))
        {
            std::int32_t count = 0;
            std::memcpy(&count, site + 0xD, sizeof(count));

            if (count > 0 && count < 0x2000)
            {
                g_compositeCopy.count = static_cast<std::uint32_t>(count);
                g_compositeCopy.copy = reinterpret_cast<CompositeCopyFn>(site);
            }
        }

        if (!g_compositeCopy.copy)
            Unboxer::LogLine("lineup: the skin material copy wasn't found (LineupCompositeCopy) - lineup gloves can't take your in-match paint");

        return g_compositeCopy;
    }

    // The entity's full handle (index + serial: a reused slot has another one), 0xFFFFFFFF if unreadable.
    std::uint32_t EntityHandle(void* entity)
    {
        const auto* identity = *reinterpret_cast<const std::uint8_t* const*>(static_cast<const std::uint8_t*>(entity) + 0x10);
        return identity ? *reinterpret_cast<const std::uint32_t*>(identity + 0x10) : 0xFFFFFFFFu;
    }

    // A pawn's glove model (its glove helper keeps the handle), null if it has none.
    void* GloveModel(void* pawn, std::uint32_t* handleOut = nullptr)
    {
        const GloveUpdate& update = GloveUpdateFunction();

        if (!pawn || !update.helper || !I::GameEntity || !I::GameEntity->Instance)
            return nullptr;

        const std::uint32_t handle = *reinterpret_cast<const std::uint32_t*>(static_cast<const std::uint8_t*>(pawn) + update.helper);

        if (handleOut)
            *handleOut = handle;

        if (handle == 0xFFFFFFFFu)
            return nullptr;

        void* model = I::GameEntity->Instance->Get<C_BaseEntity>(static_cast<int>(handle & 0x7FFFu));
        return model && EntityHandle(model) == handle ? model : nullptr;
    }

    // An entity's skin material counts (finished, pending), -1 if unreadable.
    void MaterialCounts(void* entity, int& finished, int& pending)
    {
        finished = pending = -1;
        const CompositeCopy& copy = CompositeCopyFunction();
        auto* owner = entity && copy.count ? reinterpret_cast<std::uint8_t*>(GetCompositeMaterialOwner(entity)) : nullptr;

        if (!owner)
            return;

        finished = *reinterpret_cast<const int*>(owner + copy.count);
        pending = *reinterpret_cast<const int*>(owner + copy.count + 0x18);
    }

    // The skin material manager's requests (Patterns::LineupCompositeManager has the layout). An owner's pending list holds
    // request ids; a request still at step 0 with +0x108 set waits for the identical request with that id.
    constexpr std::size_t REQUEST_SLOT = 0x1B0;
    constexpr std::size_t REQUEST_STEP = 0x8;
    constexpr std::size_t REQUEST_SHARE = 0x88;
    constexpr std::size_t REQUEST_OWNER = 0x100;
    constexpr std::size_t REQUEST_WAITS_FOR = 0x108;
    constexpr std::size_t REQUEST_ADVANCED = 0x170;

    enum Waited : int
    {
        WAITED_NONE = 0,
        WAITED_YOURS,               // your in-match one (same paint)
        WAITED_OTHER,
    };

    const char* WaitedText(int waited)
    {
        return waited == WAITED_YOURS ? "your in-match one" : waited == WAITED_OTHER ? "another identical one" : "nothing";
    }

    std::uint8_t* const* CompositeManagerGlobal()
    {
        return M::ResolveCached([] {
            auto* getter = reinterpret_cast<std::uint8_t*>(M::scan("client.dll", Patterns::LineupCompositeManager));
            return getter ? reinterpret_cast<std::uint8_t* const*>(getter + 0xB + *reinterpret_cast<const std::int32_t*>(getter + 7)) : nullptr;
        });
    }

    // The manager's request with this id, null if it's gone.
    std::uint8_t* FindRequest(std::uint64_t id)
    {
        std::uint8_t* const* global = CompositeManagerGlobal();
        std::uint8_t* manager = global ? *global : nullptr;

        if (!manager || !id || !(*reinterpret_cast<const std::uint32_t*>(manager + 0x2C) & 0x7FFFFFFFu))
            return nullptr;

        std::uint8_t* slots = *reinterpret_cast<std::uint8_t* const*>(manager + 0x30);
        const std::uint32_t count = *reinterpret_cast<const std::uint32_t*>(manager + 0x3C);

        if (!slots || count > 0x10000)
            return nullptr;

        for (std::uint32_t i = 0; i < count; ++i)
        {
            std::uint8_t* slot = slots + i * REQUEST_SLOT;

            if (*reinterpret_cast<const std::int32_t*>(slot) >= 0 && *reinterpret_cast<const std::uint64_t*>(slot + 0x10) == id)
                return slot + 0x10;
        }

        return nullptr;
    }

    // An entity's skin material owner's pending request ids (count 0 if none / unreadable).
    const std::uint64_t* PendingIds(void* entity, int& count)
    {
        count = 0;
        const CompositeCopy& copy = CompositeCopyFunction();
        auto* owner = entity && copy.count ? reinterpret_cast<std::uint8_t*>(GetCompositeMaterialOwner(entity)) : nullptr;

        if (!owner)
            return nullptr;

        const auto* ids = *reinterpret_cast<const std::uint64_t* const*>(owner + copy.count + 0x20);
        count = ids ? std::clamp(*reinterpret_cast<const int*>(owner + copy.count + 0x18), 0, 8) : 0;
        return ids;
    }

    // What a request waits for (WAITED_*); yourOwner = your in-match one's owner (may be null).
    int WaitsFor(const std::uint8_t* request, const void* yourOwner)
    {
        const std::uint64_t id = *reinterpret_cast<const std::uint64_t*>(request + REQUEST_WAITS_FOR);

        if (!id)
            return WAITED_NONE;

        const std::uint8_t* other = FindRequest(id);
        return other && yourOwner && *reinterpret_cast<void* const*>(other + REQUEST_OWNER) == yourOwner ? WAITED_YOURS : WAITED_OTHER;
    }

    // The log: ", step N, advanced T times[, waits for ...]" for the entity's first pending request, empty if none.
    void PendingStep(void* entity, const void* yourOwner, char* out, std::size_t size)
    {
        out[0] = 0;
        int count = 0;
        const std::uint64_t* ids = PendingIds(entity, count);
        const std::uint8_t* request = count ? FindRequest(ids[0]) : nullptr;

        if (!request)
            return;

        const int waited = WaitsFor(request, yourOwner);
        snprintf(out, size, ", step %d, advanced %d times%s%s", static_cast<int>(request[REQUEST_STEP]),
            *reinterpret_cast<const int*>(request + REQUEST_ADVANCED), waited ? ", waits for " : "", waited ? WaitedText(waited) : "");
    }

    // Your lineup item's requests that wait for an identical one are made on their own: no sharing (+0x88: step 0 doesn't
    // look for one again), nothing to wait for (+0x108). Returns what the first of them waited for (WAITED_*).
    int Unshare(void* entity, const void* yourOwner)
    {
        int count = 0;
        const std::uint64_t* ids = PendingIds(entity, count);
        int result = WAITED_NONE;

        for (int i = 0; i < count; ++i)
        {
            std::uint8_t* request = FindRequest(ids[i]);

            if (!request || request[REQUEST_STEP] != 0 || !*reinterpret_cast<const std::uint64_t*>(request + REQUEST_WAITS_FOR))
                continue;

            if (result == WAITED_NONE)
                result = WaitsFor(request, yourOwner);

            request[REQUEST_SHARE] = 0;
            *reinterpret_cast<std::uint64_t*>(request + REQUEST_WAITS_FOR) = 0;
        }

        return result;
    }

    // Your in-match gloves' finished paint onto the lineup character's glove model, once per glove model.
    __declspec(noinline) void CopyGloves(Pending& pending)
    {
        if (!EntityAlive(pending.character, pending.characterIndex))
        {
            pending.characterGone = true;
            return;
        }

        const CompositeCopy& copy = CompositeCopyFunction();
        std::uint32_t lineupHandle = 0xFFFFFFFFu;
        void* lineupModel = GloveModel(pending.character, &lineupHandle);

        if (!copy.copy || !g_gloveUpdate.helper)
        {
            pending.copyResult = COPY_NO_FUNCTION;
            return;
        }

        if (!lineupModel)
        {
            pending.copyResult = COPY_NO_LINEUP_MODEL;
            return;
        }

        int lineupFinished = -1;
        int lineupWaiting = -1;
        MaterialCounts(lineupModel, lineupFinished, lineupWaiting);

        if (lineupFinished > 0)
            return;   // it has its paint (copied, or its own finished after all)

        const ULONGLONG now = GetTickCount64();

        if (pending.glovesCopiedAt && now - pending.glovesCopiedAt < COPY_EVERY_MS)
            return;

        C_CSPlayerPawn* local = g_ctx ? g_ctx->local_pawn : nullptr;

        if (!local)
        {
            pending.copyResult = COPY_NO_LOCAL;
            return;
        }

        C_EconItemView* mine = local->m_EconGloves();
        C_EconItemView* lineup = static_cast<C_CSPlayerPawn*>(pending.character)->m_EconGloves();

        if (!mine || !lineup || mine->m_iItemDefinitionIndex() != lineup->m_iItemDefinitionIndex() ||
            M::CallVFunc<int, 3U>(mine) != M::CallVFunc<int, 3U>(lineup))
        {
            pending.copyResult = COPY_DIFFERENT;
            return;
        }

        void* localModel = GloveModel(local);

        if (!localModel)
        {
            pending.copyResult = COPY_NO_LOCAL_MODEL;
            return;
        }

        void* from = GetCompositeMaterialOwner(localModel);
        void* to = GetCompositeMaterialOwner(lineupModel);

        if (!from || !to)
        {
            pending.copyResult = COPY_NO_OWNER;
            return;
        }

        int finished = -1;
        int waiting = -1;
        MaterialCounts(localModel, finished, waiting);

        if (finished <= 0 || waiting > 0)
        {
            pending.copyResult = COPY_LOCAL_NOT_READY;
            return;
        }

        copy.copy(from, to, 1);
        pending.copiedTo = lineupHandle;
        pending.copyResult = COPY_DONE;
        pending.glovesCopiedAt = now;
        ++pending.copies;
    }

    // Your in-match weapon with the same item as the lineup gun / knife (definition + paint), null if you don't carry one.
    void* YourSameWeapon(const Look& look)
    {
        C_CSPlayerPawn* local = g_ctx ? g_ctx->local_pawn : nullptr;
        CCSPlayer_WeaponServices* services = local ? local->m_pWeaponServices() : nullptr;

        if (!services || !I::GameEntity || !I::GameEntity->Instance)
            return nullptr;

        auto& handles = services->m_hMyWeapons();

        for (int i = 0; i < handles.count(); ++i)
        {
            const CBaseHandle handle = handles.element(i);
            auto* weapon = handle.valid() ? I::GameEntity->Instance->Get<C_CSWeaponBase>(handle) : nullptr;
            C_AttributeContainer* container = weapon ? weapon->m_AttributeManager() : nullptr;
            C_EconItemView* item = container ? container->m_Item() : nullptr;

            if (item && item->m_iItemDefinitionIndex() == look.definition && M::CallVFunc<int, 3U>(item) == look.paintKit)
                return weapon;
        }

        return nullptr;
    }

    // Your in-match weapon's finished skin material onto the lineup gun / knife whenever that has none finished (its own
    // request can stay pending; something may also reset it later).
    __declspec(noinline) void CopyGun(Pending& pending)
    {
        if (!pending.gunOurs || !EntityAlive(pending.gun, pending.gunIndex))
        {
            pending.gunGone = true;
            return;
        }

        int lineupFinished = -1;
        int lineupWaiting = -1;
        MaterialCounts(pending.gun, lineupFinished, lineupWaiting);

        if (lineupFinished > 0)
            return;   // it has its skin

        const CompositeCopy& copy = CompositeCopyFunction();

        if (!copy.copy)
        {
            pending.gunCopyResult = COPY_NO_FUNCTION;
            return;
        }

        void* yours = YourSameWeapon(pending.gunLook);

        if (!yours)
        {
            pending.gunCopyResult = COPY_DIFFERENT;
            return;
        }

        int finished = -1;
        int waiting = -1;
        MaterialCounts(yours, finished, waiting);
        pending.sourceFinished = finished;
        pending.sourceWaiting = waiting;

        if (finished <= 0 || waiting > 0)
        {
            pending.gunCopyResult = COPY_LOCAL_NOT_READY;
            return;
        }

        const ULONGLONG now = GetTickCount64();

        if (pending.gunCopiedAt && now - pending.gunCopiedAt < COPY_EVERY_MS)
            return;

        void* from = GetCompositeMaterialOwner(yours);
        void* to = GetCompositeMaterialOwner(pending.gun);

        if (!from || !to)
        {
            pending.gunCopyResult = COPY_NO_OWNER;
            return;
        }

        copy.copy(from, to, 1);
        pending.gunCopyResult = COPY_DONE;
        pending.gunCopiedAt = now;
        ++pending.gunCopies;
    }

    bool CallCopyGun(Pending& pending)
    {
        __try
        {
            CopyGun(pending);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            pending.gunGone = true;   // whatever crashed isn't tried again
            return false;
        }
    }

    // The lineup gun / knife still has no finished skin material (and nothing of yours to copy from).
    __declspec(noinline) bool GunUnfinished(const Pending& pending)
    {
        if (!pending.gunOurs || !EntityAlive(pending.gun, pending.gunIndex) || pending.gunCopyResult == COPY_DONE)
            return false;

        int finished = -1;
        int waiting = -1;
        MaterialCounts(pending.gun, finished, waiting);
        return finished == 0;
    }

    bool CallGunUnfinished(const Pending& pending)
    {
        __try
        {
            return GunUnfinished(pending);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    // The skin material manager starts the most recently "touched" owner's material first (the scene system touches an
    // owner when its entity is drawn in a normal view - not the lineup characters, so their gloves / knife never got their
    // turn at round start). Your lineup gun / knife and glove model are touched every frame (Patterns::LineupCompositeTouch).
    // A request with the same paint as one already being made waits for that one (state 0 sets request+0x108 and stops
    // there) - the lineup gloves / knife wait for your IN-MATCH gloves / knife, which aren't drawn during the lineup either,
    // so those are touched too.
    using CompositeTouchFn = void(__fastcall*)(void* owner);

    CompositeTouchFn CompositeTouchFunction()
    {
        return M::ResolveCached([] { return reinterpret_cast<CompositeTouchFn>(M::scan("client.dll", Patterns::LineupCompositeTouch)); });
    }

    __declspec(noinline) void TouchLineup(const Pending& pending)
    {
        const CompositeTouchFn touch = CompositeTouchFunction();

        if (!touch)
            return;

        if (pending.gunOurs && EntityAlive(pending.gun, pending.gunIndex))
        {
            if (void* owner = GetCompositeMaterialOwner(pending.gun))
                touch(owner);
        }

        if (pending.gloves && EntityAlive(pending.character, pending.characterIndex))
        {
            if (void* model = GloveModel(pending.character))
            {
                if (void* owner = GetCompositeMaterialOwner(model))
                    touch(owner);
            }
        }

        // the in-match ones the lineup's wait for
        if (pending.gunOurs && !pending.gunGone)
        {
            if (void* yours = YourSameWeapon(pending.gunLook))
            {
                if (void* owner = GetCompositeMaterialOwner(yours))
                    touch(owner);
            }
        }

        if (pending.gloves && !pending.characterGone)
        {
            if (void* model = GloveModel(g_ctx ? g_ctx->local_pawn : nullptr))
            {
                if (void* owner = GetCompositeMaterialOwner(model))
                    touch(owner);
            }
        }
    }

    // Your lineup gun / knife and gloves: made on their own when they wait for your in-match ones (from 0.3 s after the build:
    // an identical one already made is taken from the cache before that).
    __declspec(noinline) void UnshareLineup(Pending& pending)
    {
        if (pending.gunOurs && !pending.gunGone && EntityAlive(pending.gun, pending.gunIndex))
        {
            void* yours = YourSameWeapon(pending.gunLook);

            if (const int waited = Unshare(pending.gun, yours ? GetCompositeMaterialOwner(yours) : nullptr); waited && !pending.gunUnshared)
                pending.gunUnshared = waited;
        }

        if (pending.gloves && !pending.characterGone && EntityAlive(pending.character, pending.characterIndex))
        {
            void* local = GloveModel(g_ctx ? g_ctx->local_pawn : nullptr);

            if (const int waited = Unshare(GloveModel(pending.character), local ? GetCompositeMaterialOwner(local) : nullptr);
                waited && !pending.glovesUnshared)
                pending.glovesUnshared = waited;
        }
    }

    bool CallUnshareLineup(Pending& pending)
    {
        __try
        {
            UnshareLineup(pending);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    bool CallTouchLineup(const Pending& pending)
    {
        __try
        {
            TouchLineup(pending);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    // The lineup character's glove model's skin material counts, -1 if it's gone / unreadable.
    __declspec(noinline) void LineupGloveCounts(const Pending& pending, int& finished, int& waiting)
    {
        finished = waiting = -1;

        if (EntityAlive(pending.character, pending.characterIndex))
            MaterialCounts(GloveModel(pending.character), finished, waiting);
    }

    bool CallLineupGloveCounts(const Pending& pending, int& finished, int& waiting)
    {
        __try
        {
            LineupGloveCounts(pending, finished, waiting);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            finished = waiting = -1;
            return false;
        }
    }

    bool CallCopyGloves(Pending& pending)
    {
        __try
        {
            CopyGloves(pending);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            pending.gloves = 0;   // whatever crashed isn't tried again
            return false;
        }
    }

    // A skin material request that got lost: nothing finished and nothing pending on the gun / the lineup glove model.
    __declspec(noinline) void LostRequests(const Pending& pending, bool& gunLost, bool& glovesLost)
    {
        gunLost = glovesLost = false;
        int finished = -1;
        int waiting = -1;

        if (pending.gunOurs && EntityAlive(pending.gun, pending.gunIndex))
        {
            MaterialCounts(pending.gun, finished, waiting);
            gunLost = finished == 0 && waiting == 0;
        }

        if (pending.gloves && EntityAlive(pending.character, pending.characterIndex))
        {
            MaterialCounts(GloveModel(pending.character), finished, waiting);
            glovesLost = finished == 0 && waiting == 0;
        }
    }

    bool CallLostRequests(const Pending& pending, bool& gunLost, bool& glovesLost)
    {
        __try
        {
            LostRequests(pending, gunLost, glovesLost);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            gunLost = glovesLost = false;
            return false;
        }
    }

    std::mutex g_pendingMutex;
    std::array<Pending, 4> g_pending{};   // g_pendingMutex
    std::uint32_t g_pendingSerial = 0;    // g_pendingMutex


    // One apply on the gun (the frame-stage thread). alive = it still exists.
    __declspec(noinline) void ApplyGun(Pending& pending, bool& alive, bool& regenerate)
    {
        if (EntityAlive(pending.gun, pending.gunIndex))
        {
            alive = true;

            if (pending.gunOurs && PaintGun(pending.gun, pending.gunLook))
            {
                ++pending.gunApplies;
                regenerate = true;
            }
        }
    }

    bool CallApplyGun(Pending& pending, bool& alive, bool& regenerate)
    {
        __try
        {
            ApplyGun(pending, alive, regenerate);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    // One apply on the gloves - after the guns' rebuild (the glove routine spawns a glove model on the character).
    // alive = the character still exists. followUp: the gloves are also rebuilt right away by running the game's glove
    // routine (a lineup character doesn't run it by itself).
    __declspec(noinline) void ApplyGloves(Pending& pending, bool& alive, bool followUp)
    {
        if (EntityAlive(pending.character, pending.characterIndex))
        {
            alive = true;
            auto* pawn = static_cast<C_CSPlayerPawn*>(pending.character);
            C_EconItemView* gloves = pending.gloves ? pawn->m_EconGloves() : nullptr;

            if (gloves && gloves->m_iItemDefinitionIndex() == pending.gloves && gloves->m_iItemIDHigh() == NO_SOC_ID_HIGH)
            {
                if (pending.flagAtFirst < 0)
                    pending.flagAtFirst = pawn->m_bNeedToReApplyGloves() ? 1 : 0;

                gloves->m_bRestoreCustomMaterialAfterPrecache() = true;
                pawn->m_bNeedToReApplyGloves() = true;
                ++pending.gloveApplies;

                const GloveUpdate& update = GloveUpdateFunction();

                if (followUp && update.routine)
                {
                    update.routine(reinterpret_cast<std::uint8_t*>(pawn) + update.helper);
                    ++pending.directRebuilds;
                    pending.flagAfterDirect = pawn->m_bNeedToReApplyGloves() ? 1 : 0;
                }
            }
        }
    }

    bool CallApplyGloves(Pending& pending, bool& alive, bool followUp)
    {
        __try
        {
            ApplyGloves(pending, alive, followUp);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    // An old-model skin needs the gun's old mesh (mesh group 2); put back whenever something else changed it.
    __declspec(noinline) void HoldMesh(Pending& pending)
    {
        if (!EntityAlive(pending.gun, pending.gunIndex))
        {
            pending.mesh = 0;   // the gun is gone
            return;
        }

        if (ReadMeshMask(pending.gun) == pending.mesh)
            return;

        if (CGameSceneNode* node = static_cast<C_BaseEntity*>(pending.gun)->m_pGameSceneNode())
            node->SetMeshGroupMask(pending.mesh);

        ++pending.meshResets;
    }

    bool CallHoldMesh(Pending& pending)
    {
        __try
        {
            HoldMesh(pending);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            pending.mesh = 0;
            return false;
        }
    }

    // The log line once the applies are done: what the gun and the gloves read now.
    __declspec(noinline) void Describe(const Pending& pending, const char* when, char* out, std::size_t size)
    {
        char gun[560] = "gone";
        char gloves[900] = "gone";

        if (EntityAlive(pending.gun, pending.gunIndex))
        {
            C_AttributeContainer* container = static_cast<C_EconEntity*>(pending.gun)->m_AttributeManager();
            C_EconItemView* item = pending.gunOurs && container ? container->m_Item() : nullptr;

            if (item)
            {
                int finished = -1;
                int waiting = -1;
                MaterialCounts(pending.gun, finished, waiting);
                void* yours = YourSameWeapon(pending.gunLook);
                char step[96];
                PendingStep(pending.gun, yours ? GetCompositeMaterialOwner(yours) : nullptr, step, sizeof(step));

                DescribePaint(item, gun, sizeof(gun));
                const std::size_t used = std::strlen(gun);
                snprintf(gun + used, sizeof(gun) - used,
                    ", mesh %llu, skin materials %d (pending %d%s), copy from your weapon: %s (%d times; yours has %d, pending %d)",
                    static_cast<unsigned long long>(ReadMeshMask(pending.gun)), finished, waiting, step,
                    pending.gunCopyResult == COPY_DIFFERENT ? "you don't carry that one" : CopyResultText(pending.gunCopyResult), pending.gunCopies,
                    pending.sourceFinished, pending.sourceWaiting);
            }
            else
                snprintf(gun, sizeof(gun), "not ours");
        }

        if (EntityAlive(pending.character, pending.characterIndex))
        {
            C_EconItemView* item = pending.gloves ? static_cast<C_CSPlayerPawn*>(pending.character)->m_EconGloves() : nullptr;

            if (item)
            {
                auto* pawn = static_cast<C_CSPlayerPawn*>(pending.character);
                const std::uint32_t helper = g_gloveUpdate.helper;
                const std::uint32_t model = helper ? *reinterpret_cast<const std::uint32_t*>(reinterpret_cast<const std::uint8_t*>(pawn) + helper)
                    : 0xFFFFFFFFu;

                int lineupFinished = -1;
                int lineupWaiting = -1;
                int localFinished = -1;
                int localWaiting = -1;
                void* localModel = GloveModel(g_ctx ? g_ctx->local_pawn : nullptr);
                MaterialCounts(GloveModel(pawn), lineupFinished, lineupWaiting);
                MaterialCounts(localModel, localFinished, localWaiting);
                char lineupStep[96];
                char localStep[96];
                PendingStep(GloveModel(pawn), localModel ? GetCompositeMaterialOwner(localModel) : nullptr, lineupStep, sizeof(lineupStep));
                PendingStep(localModel, nullptr, localStep, sizeof(localStep));

                DescribePaint(item, gloves, sizeof(gloves));
                const std::size_t used = std::strlen(gloves);
                snprintf(gloves + used, sizeof(gloves) - used,
                    ", rebuild flag %d (before us %d), rebuilt by us %d times (flag after %d), glove model %08X with %d skin materials "
                    "(pending %d%s), your in-match glove model %d (pending %d%s), paint copy: %s (%d times)",
                    static_cast<int>(pawn->m_bNeedToReApplyGloves()), pending.flagAtFirst, pending.directRebuilds, pending.flagAfterDirect, model,
                    lineupFinished, lineupWaiting, lineupStep, localFinished, localWaiting, localStep, CopyResultText(pending.copyResult),
                    pending.copies);
            }
            else
                snprintf(gloves, sizeof(gloves), "not ours");
        }

        snprintf(out, size, "lineup: your lineup character %s - gun skin applied %d times, gloves %d times | gun: %s | gloves: %s",
            when, pending.gunApplies, pending.gloveApplies, gun, gloves);
    }

    bool CallDescribe(const Pending& pending, const char* when, char* out, std::size_t size)
    {
        __try
        {
            Describe(pending, when, out, size);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            snprintf(out, size, "lineup: your lineup character %s - gun skin applied %d times, gloves %d times (reading it crashed)",
                when, pending.gunApplies, pending.gloveApplies);
            return false;
        }
    }

    bool CallRegenerate()
    {
        const RegenerateFn regenerate = RegenerateFunction();

        if (!regenerate)
            return false;

        __try
        {
            regenerate();
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    // The first builds in the log: what the character's gloves and the gun are right after the game made them.
    int g_builtLogged = 0;   // the builder's thread

    __declspec(noinline) void LogBuilt(void* character, void* gun)
    {
        auto* pawn = static_cast<C_CSPlayerPawn*>(character);
        C_EconItemView* glovesItem = pawn ? pawn->m_EconGloves() : nullptr;
        C_AttributeContainer* container = gun ? static_cast<C_EconEntity*>(gun)->m_AttributeManager() : nullptr;
        C_EconItemView* gunItem = container ? container->m_Item() : nullptr;

        char line[200];
        snprintf(line, sizeof(line), "lineup: built - character %s (gloves %u%s, id high %08X), gun %s (def %u, id high %08X)",
            pawn ? "yes" : "none", glovesItem ? static_cast<unsigned>(glovesItem->m_iItemDefinitionIndex()) : 0u,
            glovesItem && glovesItem->m_bInitialized() ? "" : " not set", glovesItem ? glovesItem->m_iItemIDHigh() : 0u,
            gun ? "yes" : "none", gunItem ? static_cast<unsigned>(gunItem->m_iItemDefinitionIndex()) : 0u,
            gunItem ? gunItem->m_iItemIDHigh() : 0u);
        Log(line);
    }

    // After the builder (its thread): your character and its gun go in the queue FrameTick works through.
    __declspec(noinline) void Queue(void* position, const Layout& layout)
    {
        auto* base = static_cast<std::uint8_t*>(position);
        void* character = *reinterpret_cast<void* const*>(base + layout.team + TEAM_TO_CHARACTER);
        void* gun = *reinterpret_cast<void* const*>(base + layout.team + TEAM_TO_WEAPON);

        if (g_builtLogged < 4)
        {
            ++g_builtLogged;
            LogBuilt(character, gun);
        }

        if (!character && !gun)
            return;

        Pending pending;
        pending.character = character;
        pending.characterIndex = character ? EntityIndex(character) : -1;
        pending.gloves = g_decided.gloves;
        pending.gun = gun;
        pending.gunIndex = gun ? EntityIndex(gun) : -1;
        pending.gunOurs = g_decided.gun && gun;
        pending.gunLook = g_decided.gunLook;
        pending.createdAt = GetTickCount64();
        pending.mesh = pending.gunOurs && MeshMask(pending.gunLook.paintKit) == 2ULL ? 2ULL : 0ULL;

        std::lock_guard lock(g_pendingMutex);

        // the same character and gun queued a moment ago (the builder ran again): its applies carry on
        for (const Pending& queued : g_pending)
        {
            if (queued.serial && queued.character == character && queued.gun == gun && pending.createdAt - queued.createdAt < 3500)
                return;
        }

        Pending* slot = &g_pending[0];

        for (Pending& queued : g_pending)
        {
            if (!queued.serial)
            {
                slot = &queued;
                break;
            }

            if (queued.createdAt < slot->createdAt)
                slot = &queued;   // all busy: the oldest makes room
        }

        pending.serial = ++g_pendingSerial ? g_pendingSerial : ++g_pendingSerial;
        *slot = pending;
    }

    bool CallQueue(void* position, const Layout& layout)
    {
        __try
        {
            Queue(position, layout);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    bool g_layoutLogged = false;
    bool g_crashLogged = false;
    bool g_queueCrashLogged = false;
    bool g_applyCrashLogged = false;
    bool g_regenerateLogged = false;
}

void Lineup::BeforeBuild(void* position)
{
    g_decided = Decided{};

    if (!position)
        return;

    Layout layout;

    if (!ReadLayout(layout))
    {
        if (!g_layoutLogged)
        {
            g_layoutLogged = true;
            Unboxer::LogLine("lineup: the lineup character's fields aren't in the game's schema - lineups show your real items");
        }

        return;
    }

    if (!CallPatch(position, layout) && !g_crashLogged)
    {
        g_crashLogged = true;
        Unboxer::LogLine("lineup: reading a lineup character crashed - lineups show your real items");
    }
}

void Lineup::AfterBuild(void* position)
{
    if (!position || g_decided.position != position)
        return;

    g_decided.position = nullptr;
    Layout layout;

    if (!ReadLayout(layout) || !layout.team)
        return;   // the character / gun aren't where they're expected: the game's look stays

    if (!CallQueue(position, layout) && !g_queueCrashLogged)
    {
        g_queueCrashLogged = true;
        Unboxer::LogLine("lineup: reading your built lineup character crashed - its skins stay the game's");
    }
}

void Lineup::FrameTick()
{
    std::array<Pending, 4> work;

    {
        std::lock_guard lock(g_pendingMutex);
        bool any = false;

        for (const Pending& pending : g_pending)
            any = any || pending.serial != 0;

        if (!any)
            return;

        work = g_pending;
    }

    const ULONGLONG now = GetTickCount64();
    bool regenerate = false;
    std::array<std::uint32_t, 4> serials{};
    std::array<int, 4> gloveWork{};   // 0 none, 1 the rebuild flag, 2 the game's glove routine

    // 1. the guns (and an old-model skin's mesh), each step once
    for (std::size_t i = 0; i < work.size(); ++i)
    {
        Pending& pending = work[i];
        serials[i] = pending.serial;

        if (!pending.serial)
            continue;

        // an old-model skin keeps the gun's old mesh, every frame while the gun is there
        if (pending.mesh)
            CallHoldMesh(pending);

        const ULONGLONG age = now - pending.createdAt;
        bool gunAlive = false;
        bool crashed = false;

        switch (pending.step)
        {
        case STEP_PAINT:
        {
            bool painted = false;   // the game's rebuild comes next tick (STEP_REGENERATE), once the paint request is in
            crashed = !CallApplyGun(pending, gunAlive, painted);
            pending.gunPainted = !crashed && painted;
            gloveWork[i] = 1;
            pending.step = STEP_REGENERATE;
            break;
        }

        case STEP_REGENERATE:
            regenerate = regenerate || pending.gunPainted;
            pending.step = STEP_GLOVES;
            break;

        case STEP_GLOVES:
            if (age >= STEP_GLOVES_MS)
            {
                gloveWork[i] = 2;
                pending.step = STEP_CHECK;
            }
            break;

        case STEP_CHECK:
            if (age >= STEP_CHECK_MS)
            {
                bool gunLost = false;
                bool glovesLost = false;
                CallLostRequests(pending, gunLost, glovesLost);

                if (gunLost)
                    crashed = !CallApplyGun(pending, gunAlive, regenerate);
                else if (pending.gunCopyResult == COPY_DIFFERENT && CallGunUnfinished(pending))
                    regenerate = true;   // stuck and you don't carry one to copy from: made again (a re-request also
                                         // restarts your in-match weapons' skins - never while one is to be copied)

                if (glovesLost)
                    gloveWork[i] = 2;

                char line[1600];
                CallDescribe(pending, "1.5 s after the build", line, sizeof(line));
                Log(line);
                pending.applied = true;
                pending.step = STEP_RECHECK;
            }
            break;

        case STEP_RECHECK:
            if (age >= STEP_RECHECK_MS)
            {
                if (pending.gunCopyResult == COPY_DIFFERENT && CallGunUnfinished(pending))
                    regenerate = true;

                pending.step = STEP_STATUS;
            }
            break;

        case STEP_STATUS:
            if (age >= STEP_STATUS_MS)
            {
                char line[1600];
                CallDescribe(pending, "6 s after the build", line, sizeof(line));
                Log(line);
                pending.step = STEP_DONE;
            }
            break;

        default:
            break;
        }

        if (crashed)
        {
            pending.gunOurs = false;   // whatever crashed isn't tried again
            pending.mesh = 0;

            if (!g_applyCrashLogged)
            {
                g_applyCrashLogged = true;
                Unboxer::LogLine("lineup: putting the skin on your lineup gun crashed - it keeps the game's look");
            }
        }
    }

    // 2. the game's own rebuild of every gun's skin material - it re-queues every gun on the map, so at most once a step
    if (regenerate && !CallRegenerate() && !g_regenerateLogged)
    {
        g_regenerateLogged = true;
        Unboxer::LogLine("lineup: the game's skin rebuild crashed / wasn't found (RegenerateSkins)");
    }

    // 3. the gloves, after the guns' rebuild: the rebuild flag / the game's glove routine, then your in-match gloves'
    //    finished paint copied onto the lineup glove model (tried every frame while the character is there)
    for (std::size_t i = 0; i < work.size(); ++i)
    {
        Pending& pending = work[i];

        if (!pending.serial)
            continue;

        if (gloveWork[i])
        {
            bool characterAlive = false;

            if (!CallApplyGloves(pending, characterAlive, gloveWork[i] == 2))
            {
                pending.gloves = 0;

                if (!g_applyCrashLogged)
                {
                    g_applyCrashLogged = true;
                    Unboxer::LogLine("lineup: rebuilding your lineup gloves crashed - they keep the game's look");
                }
            }
        }

        // your lineup items count as just drawn: the material manager makes theirs first
        if (!CallTouchLineup(pending) && !g_applyCrashLogged)
        {
            g_applyCrashLogged = true;
            Unboxer::LogLine("lineup: touching your lineup items' skin materials crashed (LineupCompositeTouch)");
        }

        const bool copying = pending.gloves && !pending.characterGone && now < pending.createdAt + COPY_GLOVES_MS;
        const bool gunCopying = pending.gunOurs && !pending.gunGone && now < pending.createdAt + COPY_GLOVES_MS;

        if ((copying || gunCopying) && now >= pending.createdAt + STEP_GLOVES_MS && !CallUnshareLineup(pending) && !g_applyCrashLogged)
        {
            g_applyCrashLogged = true;
            Unboxer::LogLine("lineup: making your lineup skins on their own crashed (LineupCompositeManager)");
        }

        if (pending.gunUnshared && !pending.gunUnshareLogged)
        {
            pending.gunUnshareLogged = true;
            char line[192];
            snprintf(line, sizeof(line), "lineup: your lineup gun / knife's skin waited for %s - made on its own now",
                WaitedText(pending.gunUnshared));
            Log(line);
        }

        if (pending.glovesUnshared && !pending.glovesUnshareLogged)
        {
            pending.glovesUnshareLogged = true;
            char line[192];
            snprintf(line, sizeof(line), "lineup: your lineup gloves' skin waited for %s - made on its own now", WaitedText(pending.glovesUnshared));
            Log(line);
        }

        if (copying && !CallCopyGloves(pending) && !g_applyCrashLogged)
        {
            g_applyCrashLogged = true;
            Unboxer::LogLine("lineup: copying your gloves' paint onto your lineup gloves crashed - they keep the game's look");
        }

        // the gun / knife: your in-match one's finished skin material whenever the lineup one has none finished
        if (gunCopying && !CallCopyGun(pending) && !g_applyCrashLogged)
        {
            g_applyCrashLogged = true;
            Unboxer::LogLine("lineup: copying your weapon's skin onto your lineup gun crashed - it keeps the game's look");
        }

        if (pending.gunOurs && !pending.gunCopyLogged && (pending.gunCopyResult == COPY_DONE || !gunCopying))
        {
            pending.gunCopyLogged = true;

            char line[256];
            snprintf(line, sizeof(line), "lineup: your in-match weapon's skin onto your lineup gun / knife (def %u): %s (yours had %d skin "
                "materials, pending %d)", static_cast<unsigned>(pending.gunLook.definition),
                pending.gunCopyResult == COPY_DIFFERENT ? "you don't carry that one" : CopyResultText(pending.gunCopyResult),
                pending.sourceFinished, pending.sourceWaiting);
            Log(line);
        }

        // the copy's outcome in the log once: the first copy, or why it never happened
        if (pending.gloves && !pending.copyLogged && (pending.copyResult == COPY_DONE || !copying))
        {
            pending.copyLogged = true;
            int finished = -1;
            int waiting = -1;
            CallLineupGloveCounts(pending, finished, waiting);

            char line[256];
            snprintf(line, sizeof(line), "lineup: your in-match gloves' paint onto your lineup gloves: %s (lineup glove model now has %d skin "
                "materials, pending %d)", CopyResultText(pending.copyResult), finished, waiting);
            Log(line);
        }

        // done: the steps are over, no mesh to keep and no copy to try (any more)
        if (pending.step == STEP_DONE && (!pending.mesh || now > pending.createdAt + HOLD_MESH_MS) && !copying && !gunCopying)
        {
            if (pending.meshResets)
            {
                char line[128];
                snprintf(line, sizeof(line), "lineup: your gun's old-model mesh was put back %d times (something else changed it)",
                    pending.meshResets);
                Log(line);
            }

            pending.serial = 0;
        }
    }

    // the progress back (a slot Queue refilled meanwhile keeps its new character)
    std::lock_guard lock(g_pendingMutex);

    for (std::size_t i = 0; i < work.size(); ++i)
    {
        if (serials[i] && g_pending[i].serial == serials[i])
            g_pending[i] = work[i];
    }
}

namespace
{
    // One weapon of yours in the match, plain values only (read under SEH).
    struct WeaponLook
    {
        std::uint16_t definition = 0;
        int paint = 0;                 // the paint it has (its fallback paint kit: what the skin changer wrote)
        std::uint32_t idHigh = 0;      // 0xFFFFFFFF = the skin changer's; anything else = the real item is back
        std::uint64_t itemId = 0;      // the item's full id (one of your Lootline ids = a server in your own game gave it)
        int finished = -1;             // its skin materials (world model): done / waiting, -1 unreadable
        int pending = -1;
        bool firstPerson = false;      // it has a first-person model (it's in your hands)
        int hudFinished = -1;
        int hudPending = -1;
        char worldStep[96] = {};       // a waiting one's request: ", step N, advanced T times[, waits for ...]"
        char hudStep[96] = {};
    };

    bool ReadWeaponLook(C_CSWeaponBase* weapon, C_CSPlayerPawn* pawn, WeaponLook& out)
    {
        __try
        {
            C_AttributeContainer* container = weapon->m_AttributeManager();
            C_EconItemView* item = container ? container->m_Item() : nullptr;

            if (!item)
                return false;

            out.definition = item->m_iItemDefinitionIndex();
            out.paint = weapon->m_nFallbackPaintKit();
            out.idHigh = item->m_iItemIDHigh();
            if (const std::uint64_t* id = ItemIdFull(item))
                out.itemId = *id;
            MaterialCounts(weapon, out.finished, out.pending);
            if (out.pending > 0)
                PendingStep(weapon, nullptr, out.worldStep, sizeof(out.worldStep));

            if (C_BaseEntity* hud = SkinApplication::HudWeaponOf(weapon, pawn))
            {
                out.firstPerson = true;
                MaterialCounts(hud, out.hudFinished, out.hudPending);
                if (out.hudPending > 0)
                    PendingStep(hud, nullptr, out.hudStep, sizeof(out.hudStep));
            }

            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }
}

std::string Lineup::DescribeYourWeapons(void* pawnPointer)
{
    auto* pawn = static_cast<C_CSPlayerPawn*>(pawnPointer);
    CCSPlayer_WeaponServices* services = pawn ? pawn->m_pWeaponServices() : nullptr;

    if (!services || !I::GameEntity || !I::GameEntity->Instance)
        return "no weapons";

    const Config::skin_changer_t& live = Config::GameLive();
    auto& handles = services->m_hMyWeapons();
    std::string text;

    for (int i = 0; i < handles.count(); ++i)
    {
        const CBaseHandle handle = handles.element(i);
        auto* weapon = handle.valid() ? I::GameEntity->Instance->Get<C_CSWeaponBase>(handle) : nullptr;
        WeaponLook look;

        if (!weapon || !ReadWeaponLook(weapon, pawn, look))
            continue;

        // the skin it should have: the knife on this side (its own finish), or this gun's
        const bool knife = KnifeModelChanger::IsKnifeDefinition(look.definition);
        int wanted = 0;

        if (knife)
        {
            if (const auto it = live.knives.find(live.knifeDefinition); it != live.knives.end() && it->second.enabled)
                wanted = it->second.paintKit;
        }
        else if (const auto it = live.weapons.find(look.definition); it != live.weapons.end() && it->second.enabled)
        {
            wanted = it->second.paintKit;
        }

        char part[384];
        snprintf(part, sizeof(part), "%s%s %u: skin %d (wanted %d)%s, paint %d done / %d waiting%s, first person ",
            text.empty() ? "" : "; ", knife ? "knife" : "gun", static_cast<unsigned>(look.definition), look.paint, wanted,
            look.idHigh == 0xFFFFFFFFu ? "" : Unboxer::IsLootlineId(look.itemId) ? " YOUR LOOTLINE ITEM (the game shows it)" : " REAL ITEM BACK",
            look.finished, look.pending, look.worldStep);
        text += part;

        if (look.firstPerson)
        {
            snprintf(part, sizeof(part), "%d / %d%s", look.hudFinished, look.hudPending, look.hudStep);
            text += part;
        }
        else
        {
            text += "not out";
        }
    }

    return text.empty() ? "no weapons" : text;
}

namespace
{
    // ── the match: skin materials of your weapons that wait for an identical request forever ──
    // CS2 shares skin material requests with the same inputs (step 0 sets +0x108 to the one being made and waits). When
    // that one never finishes (its owner gone / never drawn), yours waits for good - a reapply makes the very same
    // request and waits again (the 2026-10-28 log: one knife or gun "0 done / 1 waiting" every spawn for a whole session,
    // the first-person one vanishing with it). The lineups had the same trouble (Unshare); here it's your weapons.
    constexpr int MAX_STUCK = 32;
    constexpr ULONGLONG UNSTICK_AFTER_MS = 1000;   // an identical one in progress is done long before this
    constexpr ULONGLONG UNSTICK_EVERY_MS = 250;
    constexpr int MAX_UNSTICK_LINES = 60;

    struct StuckRequest
    {
        std::uint64_t id = 0;
        ULONGLONG since = 0;
        bool seen = false;   // this pass
    };

    struct Unstuck
    {
        std::uint16_t definition = 0;
        bool firstPerson = false;
        ULONGLONG waitedMs = 0;
    };

    StuckRequest g_stuck[MAX_STUCK]{};
    ULONGLONG g_unstickNext = 0;
    int g_unstickLines = 0;
    bool g_unstickCrashLogged = false;

    // One model's (a weapon's world or first-person one) requests waiting at step 0: tracked, and made on their own
    // once they waited UNSTICK_AFTER_MS. The caller has the SEH.
    void UnstickModel(void* entity, std::uint16_t definition, bool firstPerson, ULONGLONG now, Unstuck* out, int& outCount)
    {
        int count = 0;
        const std::uint64_t* ids = PendingIds(entity, count);

        for (int i = 0; i < count; ++i)
        {
            std::uint8_t* request = FindRequest(ids[i]);

            if (!request || request[REQUEST_STEP] != 0 || !*reinterpret_cast<const std::uint64_t*>(request + REQUEST_WAITS_FOR))
                continue;

            StuckRequest* entry = nullptr;
            StuckRequest* empty = nullptr;

            for (StuckRequest& stuck : g_stuck)
            {
                if (stuck.id == ids[i])
                {
                    entry = &stuck;
                    break;
                }

                if (!stuck.id && !empty)
                    empty = &stuck;
            }

            if (!entry)
            {
                if (!empty)
                    continue;

                entry = empty;
                entry->id = ids[i];
                entry->since = now;
            }

            entry->seen = true;

            if (now - entry->since < UNSTICK_AFTER_MS)
                continue;

            request[REQUEST_SHARE] = 0;
            *reinterpret_cast<std::uint64_t*>(request + REQUEST_WAITS_FOR) = 0;

            if (outCount < 8)
                out[outCount++] = { definition, firstPerson, now - entry->since };

            entry->id = 0;
        }
    }

    __declspec(noinline) bool UnstickPass(C_CSPlayerPawn* pawn, ULONGLONG now, Unstuck* out, int& outCount)
    {
        __try
        {
            CCSPlayer_WeaponServices* services = pawn->m_pWeaponServices();

            if (!services)
                return true;

            auto& handles = services->m_hMyWeapons();

            for (int i = 0; i < handles.count(); ++i)
            {
                const CBaseHandle handle = handles.element(i);
                auto* weapon = handle.valid() ? I::GameEntity->Instance->Get<C_CSWeaponBase>(handle) : nullptr;
                C_AttributeContainer* container = weapon ? weapon->m_AttributeManager() : nullptr;
                C_EconItemView* item = container ? container->m_Item() : nullptr;

                if (!item || item->m_iItemIDHigh() != 0xFFFFFFFFu)
                    continue;   // only what the skin changer painted

                const std::uint16_t definition = item->m_iItemDefinitionIndex();
                UnstickModel(weapon, definition, false, now, out, outCount);

                if (C_BaseEntity* hud = SkinApplication::HudWeaponOf(weapon, pawn))
                    UnstickModel(hud, definition, true, now, out, outCount);
            }

            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }
}

void Lineup::UnstickYourWeapons(void* pawnPointer)
{
    const ULONGLONG now = GetTickCount64();

    if (now < g_unstickNext)
        return;

    g_unstickNext = now + UNSTICK_EVERY_MS;
    auto* pawn = static_cast<C_CSPlayerPawn*>(pawnPointer);

    if (!pawn || !I::GameEntity || !I::GameEntity->Instance)
        return;

    for (StuckRequest& stuck : g_stuck)
        stuck.seen = false;

    Unstuck freed[8]{};
    int count = 0;

    if (!UnstickPass(pawn, now, freed, count))
    {
        if (!g_unstickCrashLogged)
        {
            g_unstickCrashLogged = true;
            Unboxer::LogLine("in match: checking your weapons' skin materials crashed (LineupCompositeManager) - skipped this time");
        }

        return;
    }

    for (StuckRequest& stuck : g_stuck)
        if (!stuck.seen)
            stuck.id = 0;   // finished or gone

    for (int i = 0; i < count && g_unstickLines < MAX_UNSTICK_LINES; ++i)
    {
        if (++g_unstickLines == MAX_UNSTICK_LINES)
        {
            Unboxer::LogLine("in match: (no more 'made on its own' lines this session)");
            break;
        }

        char line[200];
        snprintf(line, sizeof(line), "in match: your %u's skin (%s) waited %.1f s for an identical one - made on its own now",
            static_cast<unsigned>(freed[i].definition), freed[i].firstPerson ? "first person" : "world model",
            static_cast<double>(freed[i].waitedMs) / 1000.0);
        Unboxer::LogLine(line);
    }
}

namespace
{
    // ── everywhere (menus, the preview, other models): a request waiting for one that stopped ──
    // A skin material request at step 0 that waits (+0x108) for an identical one is fine while that one moves on. If the
    // other is gone, or hasn't advanced (+0x170) for DEAD_WAIT_MS, it never will be released: made on its own.
    constexpr int MAX_DEAD_WAITS = 64;
    constexpr ULONGLONG DEAD_WAIT_MS = 2000;
    constexpr ULONGLONG DEAD_WAIT_EVERY_MS = 250;
    constexpr std::uint32_t MAX_SCANNED_REQUESTS = 4096;

    struct DeadWait
    {
        std::uint64_t id = 0;
        std::uint64_t target = 0;
        int targetAdvanced = -1;
        ULONGLONG since = 0;
        bool seen = false;
    };

    DeadWait g_deadWaits[MAX_DEAD_WAITS]{};
    ULONGLONG g_deadWaitNext = 0;
    int g_deadWaitLines = 0;
    bool g_deadWaitCrashLogged = false;

    __declspec(noinline) bool DeadWaitPass(ULONGLONG now, int& freed)
    {
        __try
        {
            std::uint8_t* const* global = CompositeManagerGlobal();
            std::uint8_t* manager = global ? *global : nullptr;

            if (!manager || !(*reinterpret_cast<const std::uint32_t*>(manager + 0x2C) & 0x7FFFFFFFu))
                return true;

            std::uint8_t* slots = *reinterpret_cast<std::uint8_t* const*>(manager + 0x30);
            const std::uint32_t count = *reinterpret_cast<const std::uint32_t*>(manager + 0x3C);

            if (!slots || count > MAX_SCANNED_REQUESTS)
                return true;

            for (std::uint32_t i = 0; i < count; ++i)
            {
                std::uint8_t* slot = slots + i * REQUEST_SLOT;

                if (*reinterpret_cast<const std::int32_t*>(slot) < 0)
                    continue;

                std::uint8_t* request = slot + 0x10;
                const std::uint64_t id = *reinterpret_cast<const std::uint64_t*>(request);
                const std::uint64_t target = *reinterpret_cast<const std::uint64_t*>(request + REQUEST_WAITS_FOR);

                if (!id || request[REQUEST_STEP] != 0 || !target)
                    continue;

                const std::uint8_t* other = FindRequest(target);
                const int advanced = other ? *reinterpret_cast<const int*>(other + REQUEST_ADVANCED) : -1;

                DeadWait* entry = nullptr;
                DeadWait* empty = nullptr;

                for (DeadWait& wait : g_deadWaits)
                {
                    if (wait.id == id)
                    {
                        entry = &wait;
                        break;
                    }

                    if (!wait.id && !empty)
                        empty = &wait;
                }

                if (!entry)
                {
                    if (!empty)
                        continue;

                    entry = empty;
                    *entry = { id, target, advanced, now, false };
                }

                entry->seen = true;

                // the one it waits for moved on (or it waits for another now): the clock starts again
                if (entry->target != target || entry->targetAdvanced != advanced)
                {
                    entry->target = target;
                    entry->targetAdvanced = advanced;
                    entry->since = now;
                    continue;
                }

                if (now - entry->since < DEAD_WAIT_MS)
                    continue;

                request[REQUEST_SHARE] = 0;
                *reinterpret_cast<std::uint64_t*>(request + REQUEST_WAITS_FOR) = 0;
                entry->id = 0;
                ++freed;
            }

            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }
}

void Lineup::UnstickDeadWaits()
{
    const ULONGLONG now = GetTickCount64();

    if (now < g_deadWaitNext)
        return;

    g_deadWaitNext = now + DEAD_WAIT_EVERY_MS;

    for (DeadWait& wait : g_deadWaits)
        wait.seen = false;

    int freed = 0;

    if (!DeadWaitPass(now, freed))
    {
        if (!g_deadWaitCrashLogged)
        {
            g_deadWaitCrashLogged = true;
            Unboxer::LogLine("skin materials: checking the requests crashed (LineupCompositeManager) - skipped this time");
        }

        return;
    }

    for (DeadWait& wait : g_deadWaits)
        if (!wait.seen)
            wait.id = 0;   // finished or gone

    if (freed > 0 && g_deadWaitLines < MAX_UNSTICK_LINES)
    {
        ++g_deadWaitLines;
        char line[160];
        snprintf(line, sizeof(line), "skin materials: %d request(s) waited %.1f s for an identical one that stopped - made on their own",
            freed, static_cast<double>(DEAD_WAIT_MS) / 1000.0);
        Unboxer::LogLine(line);
    }
}
