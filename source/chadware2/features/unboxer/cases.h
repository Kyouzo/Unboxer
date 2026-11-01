#pragma once

#include <cstdint>
#include <string>
#include <vector>

// Every container in the game (weapon cases, souvenir packages, sticker / patch / pin / music kit capsules, graffiti
// boxes), its key and what it drops — read from
// scripts/items/items_game.txt:
//   case item: "associated_items" { "<key def>" "1" },  attributes "set supply crate series" → revolving_loot_lists
//   → client_loot_lists "<case>_lootlist" { "<case>_rare" ... "<case>_ancient", "<set>_unusual" }
//   → "[paint kit]weapon_x" entries.
// The ★ (knife / glove) lists aren't in the game files at all — Valve keeps them on its servers. What the game does
// have: 18 cases say which ★ they hold in their "loot_list_rare_item_footer" text ("or an Exceedingly Rare Kukri
// Knife!", "...Gloves!"). That gives those cases their ★ types (read in English, so the game's language doesn't
// matter); the others only say "Special Item".
namespace Cases
{
    enum Tier : int
    {
        TierConsumer = 0,
        TierIndustrial,
        TierMilSpec,
        TierRestricted,
        TierClassified,
        TierCovert,
        TierGold,        // ★ knife / gloves
        TierCount
    };

    // What a container holds. Skins: weapon + finish. The rest are items of a fixed definition with a kit id in
    // `paintKit`'s place (the same way the game's own loot list ids encode them): stickers, patches, graffiti, music
    // kits; a pin is its own definition (kit 0).
    enum class DropKind : int { Skin = 0, Sticker, Patch, Graffiti, MusicKit, Pin };

    inline constexpr std::uint16_t DEF_STICKER = 1209;
    inline constexpr std::uint16_t DEF_PATCH = 4609;
    inline constexpr std::uint16_t DEF_GRAFFITI = 1348;     // sealed graffiti
    inline constexpr std::uint16_t DEF_SPRAY_PAINT = 1349;  // unsealed graffiti (CS2's "spray0" slot; not sold on the market)
    inline constexpr std::uint16_t DEF_MUSIC_KIT = 1314;
    inline constexpr std::uint16_t DEF_KEYCHAIN = 1355;     // a charm (paint kit = which one; Unboxer::DEF_CHARM)

    enum class Category : int { Weapon = 0, Souvenir, Stickers, Patches, Graffiti, MusicKits, Pins, Other, Count };

    struct Drop
    {
        std::uint16_t weapon = 0;    // definition index (the weapon, or 1209 sticker / 4609 patch / 1348 graffiti / 1314 music kit / a pin)
        int paintKit = 0;            // finish, or the sticker / patch / graffiti / music kit id
        Tier tier = TierMilSpec;
    };

    // A sticker / patch / graffiti / music kit / pin, for names and pictures.
    struct Kit
    {
        int id = 0;
        DropKind kind = DropKind::Sticker;
        std::string displayName;     // "Thirteen", "Payback", "Guardian Pin"...
        std::string image;           // items_game inventory image, e.g. econ/stickers/standard/thirteen
        std::string english;         // English name, lowercase ("lucky 13") — for market prices
        int tier = TierMilSpec;      // its rarity as a Tier (AllKits lists)
        bool tintable = false;       // graffiti: comes in the 19 graffiti colours (items_game "spray_std*" kits)
    };

    // The graffiti colours (items_game graffiti_tints, "spray tint id" 1-19).
    inline constexpr int TINT_COUNT = 19;
    // "Shark White" ("" outside 1-19)
    [[nodiscard]] const char* TintName(int tint);
    // 0xRRGGBB (white outside 1-19)
    [[nodiscard]] std::uint32_t TintRgb(int tint);

    struct Case
    {
        std::uint16_t definition = 0;
        std::uint16_t key = 0;       // 0 = opens without a key (souvenir packages)
        std::string name;            // items_game name, e.g. crate_community_34
        std::string displayName;     // e.g. "Kilowatt Case"
        std::string image;           // image_inventory, e.g. econ/weapon_cases/crate_community_34
        std::string sortName;        // lowercase display name (search)
        std::vector<Drop> drops;     // from the game's loot list, lowest tier first
        bool hasGold = false;        // weapon cases (the ones with a key) can drop a ★
        std::vector<std::uint16_t> goldTypes;   // its knife / gloves definitions (from `golds`, else its ★ footer); empty = not known
        std::vector<Drop> golds;     // its ★ items: every knife / gloves + finish (each Doppler phase) that drops from it,
                                     // from the built-in list (data\case_rares.txt); empty = not known
        Category category = Category::Weapon;
        bool statTrakOnly = false;   // "StatTrak™ ... Music Kit Box": everything in it is StatTrak
        bool terminal = false;       // a "volatile container" (Genesis / Dead Hand Terminal...): unsealing it opens CS2's offer
                                     // laptop, which offers its items one by one, instead of a reel
        int tournamentEvent = 0;     // a souvenir package's "tournament event id" (its own or its prefab's), 0 = none
    };

    struct Key
    {
        std::uint16_t definition = 0;
        std::string displayName;
        std::string image;
    };

    // Builds the list (needs the item schema). Render / init thread, once. Afterwards everything is read-only, so
    // any thread may read it.
    bool Build();
    [[nodiscard]] bool Ready();

    [[nodiscard]] const std::vector<Case>& All();
    [[nodiscard]] const Case* Find(std::uint16_t definition);
    [[nodiscard]] const Key* FindKey(std::uint16_t definition);

    // What a definition is (1209 sticker, 1314 music kit, a pin... anything else: a skin).
    [[nodiscard]] DropKind KindOf(std::uint16_t definition);
    // The sticker / patch / graffiti / music kit with this id, or the pin with this definition. Null if unknown.
    [[nodiscard]] const Kit* FindKit(DropKind kind, int id);
    // Every sticker / patch / graffiti / music kit / collectible (pins, coins, medals: kind Pin) the game has, by name
    // — the Add items tab. Read-only once Ready().
    [[nodiscard]] const std::vector<Kit>& AllKits(DropKind kind);
    // An item's inventory picture (image_inventory on it or its prefab), "" if it has none.
    [[nodiscard]] const std::string& ItemImage(std::uint16_t definition);

    // A container picture CS2 doesn't ship (data\extra_images.bin, compiled in: PNG bytes by image_inventory path),
    // {nullptr, 0} if there's none. The bytes live as long as the DLL. Any thread.
    struct EmbeddedImage
    {
        const std::uint8_t* data = nullptr;
        std::size_t size = 0;
    };
    [[nodiscard]] EmbeddedImage ExtraImage(const std::string& image);
    // An item's rarity as a Tier (item_rarity on it or its prefab: agents, collectibles...), -1 if it has none.
    [[nodiscard]] int ItemTier(std::uint16_t definition);
    // The Steam market name, lowercase and in English whatever the game's language ("stattrak™ ak-47 | redline
    // (field-tested)", "souvenir awp | dragon lore (factory new)", "★ karambit", "sticker | lucky 13"). Empty if a
    // part isn't known.
    [[nodiscard]] std::string MarketName(std::uint16_t definition, int paintKit, float wear, bool statTrak, bool souvenir = false);

    // A finish's float range (items_game "wear_remap_min" / "wear_remap_max"; a finish without them has the default
    // finish's 0.06 - 0.80): Doppler 0 - 0.08, Asiimov 0.18 - 1. 0 - 1 for no finish / before Build.
    struct WearRange
    {
        float min = 0.0f;
        float max = 1.0f;
    };
    [[nodiscard]] WearRange WearRangeOf(int paintKit);

    // The collection a skin belongs to when no container hands it out (the Dust Collection's Glock-18 | Brass, operation
    // and Armory collections): its skins by tier, like a case's (definition 0, not in All()). Null if none has it.
    [[nodiscard]] const Case* SetCollectionWith(std::uint16_t weapon, int paintKit);
    [[nodiscard]] const std::vector<Case>& SetCollections();   // all of them (empty before Build)

    // The Armory (CS2's XP shop): items_game "seasonaloperations" -> the season with "redeemable_goods" "xpshop", its
    // "operational_point_redeemable" rewards in the game's order - the index MissionsAPI.ActionRedeemOperationGoods gets.
    struct ArmoryReward
    {
        std::string itemName;        // "lootlist:set_spy_tech", "crate_community_35"
        std::string displayName;     // its callout ("Spy Tech Collection")
        int points = 0;              // stars
        bool bid = false;            // a bidding reward (not done locally)
        std::uint16_t crate = 0;     // a case: its definition, given as it is (no key)
        Case pool;                   // a loot list: what one redeem rolls from (skins / stickers / charms - DEF_KEYCHAIN + charm id)
    };
    [[nodiscard]] const std::vector<ArmoryReward>& ArmoryRewards();
    [[nodiscard]] int ArmorySeason();   // its season number (MissionsAPI's "track"), 0 = none

    // A souvenir's stickers, like a real souvenir drop: the match of `event` (a souvenir package's tournamentEvent) -
    // the event's logo (its organizer's sticker), two different teams, an MVP's autograph from one of them, in that
    // order from slot 0 (what an old Major doesn't have is left out); the gold ("_gold") versions from items_game
    // sticker_kits. r1 - r3: random numbers 0..1 (the teams, the MVP). False = none (the 2013 / 2014 Majors).
    bool RollSouvenirStickers(int event, double r1, double r2, double r3, int (&kits)[4]);

    // A Major highlight clip (items_game "highlight_reels"): a charm whose "keychain slot 0 highlight" is `number` plays
    // it when inspected (CS2's video popup). Its charm: the clip's own (Cologne 2026 on: "kc_" + id) or its event's
    // (Austin / Budapest 2025: "kc_aus2025", one charm for every clip).
    struct Highlight
    {
        int number = 0;             // the highlight_reels number = the attribute's value
        int keychain = 0;           // the charm (keychain id) it's on
        int event = 0;              // its event's charm (the one card in Add items): "kc_aus2025", "kc_cologne2026"
        bool own = false;           // the charm is the clip's own; false: its event's
        std::string id;             // "aus2025_flamezaceunleashedvsmouzonmirage" (the player is only in here)
        std::string clipName;       // "Ace Unleashed"
        std::string match;          // "VIT vs MOUZ \xC2\xB7 Mirage" (team tags, map), "" = unknown
        std::string displayName;    // "Austin 2025 Highlight | Ace Unleashed" (no "Charm | ")
        std::string english;        // market name: "souvenir charm | austin 2025 highlight | ace unleashed", "" = unknown
        std::string search;         // lowercase: the clip's name, id (search "flamez"), teams and map
    };
    // Every clip, by display name. Read-only once Ready().
    [[nodiscard]] const std::vector<Highlight>& Highlights();
    [[nodiscard]] const Highlight* FindHighlight(int number);
    // An event charm's clips in Valve's order (by match), empty if it has none.
    [[nodiscard]] const std::vector<const Highlight*>& HighlightsOf(int eventCharm);
    // The event charm a charm belongs to: itself (it has clips) or a clip's own charm's event; 0 = not a highlight charm.
    [[nodiscard]] int HighlightEvent(int keychain);
    // A clip's own charm (Cologne 2026 on): its clip number; 0 = not one (an event's charm needs the number on the item).
    [[nodiscard]] int HighlightOf(int keychain);
    [[nodiscard]] const char* CategoryName(Category category);   // "Cases", "Stickers"...
    [[nodiscard]] const char* CategoryNoun(Category category);   // what a card counts: "skins", "stickers"...

    // Does this weapon / finish really drop from the case? (★: its knife / gloves type is named by the case.)
    [[nodiscard]] bool DropsFrom(const Case& item, std::uint16_t weapon, int paintKit);
    [[nodiscard]] bool GoldTypeFrom(const Case& item, std::uint16_t weapon);
    // Is this exact knife / gloves finish one of the case's ★ items? (False when its list isn't known.)
    [[nodiscard]] bool GoldFrom(const Case& item, std::uint16_t weapon, int paintKit);
    // The case's tier for a weapon / finish, -1 if it isn't in the case.
    [[nodiscard]] int TierIn(const Case& item, std::uint16_t weapon, int paintKit);

    // items_game rarity number for a tier (1 consumer ... 6 ancient; ★ = 6).
    [[nodiscard]] int Rarity(Tier tier);
    [[nodiscard]] const char* TierName(Tier tier);

    // A tier with the real odds: each tier is 5x rarer than the one below (79.92 / 15.98 / 3.2 / 0.64 %), a ★ is
    // 0.26 % of a weapon case. Only tiers the case has are rolled.
    [[nodiscard]] Tier RollTier(const Case& item, double random01);

    // The same odds as percentages (0 for tiers the case doesn't have). For the menu.
    void TierOdds(const Case& item, double percent[TierCount]);
}
