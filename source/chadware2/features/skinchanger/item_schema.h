#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "../../interfaces/CEconItemSystem/CEconItemSystem.h"

struct ItemDefinitionInfo
{
    std::uint16_t definitionIndex = 0;

    std::string baseName;
    std::string typeName;

    std::string modelName;
    std::string schemaModelName;
    std::string simpleWeaponName;
    std::string weaponName;
    std::string itemName;
    std::string displayName;

    bool isWeapon = false;
    bool isKnife = false;
    bool isGlove = false;
    bool isAgent = false;
    bool hasUsableAgentModel = false;
    bool isTerroristAgent = false;
    bool isCounterTerroristAgent = false;
};

struct PaintKitInfo
{
    int id = 0;
    int rarity = 0;

    std::string name;
    std::string descriptionTag;
    std::string displayName;

    bool usesOldModel = false;
};

// Item rarity as items_game.txt numbers it: 1 consumer ... 6 covert, 7 contraband, 99 = ★ (knives / gloves).
enum ItemRarity : int
{
    RARITY_DEFAULT = 0,
    RARITY_COMMON = 1,
    RARITY_UNCOMMON = 2,
    RARITY_RARE = 3,
    RARITY_MYTHICAL = 4,
    RARITY_LEGENDARY = 5,
    RARITY_ANCIENT = 6,
    RARITY_IMMORTAL = 7,
    RARITY_UNUSUAL = 99
};

struct StickerKitInfo
{
    int id = 0;
    int rarity = 0;

    std::string name;
    std::string itemName;
    std::string displayName;
    std::string material;
    std::string sortName;
    std::string searchText;
};

// An entry from an items_game.txt definition section that has { name, loc_name, image_inventory }:
//   "music_definitions"    → music kits (id = CCSPlayerController::m_iMusicKitID)
//   "keychain_definitions" → charms     (id = the "keychain slot 0 id" attribute)
struct EconDefinitionInfo
{
    int id = 0;
    int rarity = 0;

    std::string name;          // e.g. "valve_csgo_01" / "kc_missinglink_ava"
    std::string locName;       // e.g. "#musickit_valve_csgo_01"
    std::string displayName;   // localized, "Music Kit | " / "Charm | " prefix stripped
    std::string image;         // image_inventory, e.g. "econ/music_kits/valve_01"
    std::string sortName;      // lowercase display name
    std::string base;          // charms only: highlight-reel charms reuse this charm's model/art
};

using MusicKitInfo = EconDefinitionInfo;
using KeychainInfo = EconDefinitionInfo;

class ItemSchema
{
public:
    bool Initialize();
    void Reset();

    [[nodiscard]] bool IsInitialized() const
    {
        return m_initialized;
    }

    [[nodiscard]] const std::vector<ItemDefinitionInfo>& GetItems() const
    {
        return m_items;
    }

    [[nodiscard]] const std::vector<PaintKitInfo>& GetPaintKits() const
    {
        return m_paintKits;
    }

    [[nodiscard]] const std::vector<StickerKitInfo>& GetStickerKits() const
    {
        return m_stickerKits;
    }

    [[nodiscard]] const std::vector<MusicKitInfo>& GetMusicKits() const
    {
        return m_musicKits;
    }

    [[nodiscard]] const MusicKitInfo* GetMusicKit(int musicKitId) const;

    [[nodiscard]] const std::vector<KeychainInfo>& GetKeychains() const
    {
        return m_keychains;
    }

    [[nodiscard]] const KeychainInfo* GetKeychain(int keychainId) const;

    // The rarity the game shows for this skin on this weapon ("weapon_awp" + Dragon Lore → Covert). Comes from the
    // collection / case loot lists; falls back to the paint kit's own rarity for the few skins in none of them.
    [[nodiscard]] int GetSkinRarity(const std::string& weaponName, const PaintKitInfo& paintKit) const;

    [[nodiscard]] const ItemDefinitionInfo* GetItem(std::uint16_t definitionIndex) const;
    [[nodiscard]] const PaintKitInfo* GetPaintKit(int paintKitId) const;
    [[nodiscard]] const StickerKitInfo* GetStickerKit(int stickerKitId) const;
    [[nodiscard]] const std::vector<PaintKitInfo>* GetPaintKitsForItem(std::uint16_t definitionIndex);

    // A game file as text (e.g. scripts/items/items_game.txt) / a "#token" in your game language (empty if unknown).
    static bool ReadGameText(const char* path, std::string& output);
    [[nodiscard]] static std::string Localize(const std::string& token);

private:
    bool BuildItemList();
    bool BuildPaintKitList();
    void BuildItemsGameLists();   // stickers + music kits + charms (one pass over items_game.txt)
    bool BuildCompatiblePaintKitList(std::uint16_t definitionIndex);

    [[nodiscard]] bool IsPaintKitForItem(const char* itemName, const char* paintKitName) const;

private:
    bool m_initialized = false;

    CSource2Client* m_source2Client = nullptr;
    CEconItemSystem* m_itemSystem = nullptr;
    CEconItemSchema* m_schema = nullptr;

    std::vector<ItemDefinitionInfo> m_items;
    std::vector<PaintKitInfo> m_paintKits;
    std::vector<StickerKitInfo> m_stickerKits;
    std::vector<MusicKitInfo> m_musicKits;
    std::vector<KeychainInfo> m_keychains;
    std::unordered_map<std::string, int> m_skinRarity;   // "paintkit|weapon_x" → rarity

    std::unordered_map<std::uint16_t, std::vector<PaintKitInfo>> m_itemPaintKits;

    std::unordered_map<int, CPaintKit*> m_gamePaintKits;   // id → the game's paint kit object (for name repairs)
};

namespace Skins
{
    inline ItemSchema g_ItemSchema;

    // A copy of text that is never freed, not even when Chadware is unloaded: for any string pointer handed to the
    // game. (A pointer to one of our std::strings dangles after unload and the game reads garbage from then on.)
    [[nodiscard]] const char* PermanentName(const std::string& text);
}
