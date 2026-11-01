#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "custom_color.h"

// Unboxer: cases, keys and what you open from them — in CS2's own inventory, opened with CS2's own Unlock button and
// spinning reel. Everything is local: Valve never hears of these items.
//
// The Unlock button calls InventoryAPI.UseTool(key, case). For one of our cases that call is kept from the Game
// Coordinator (it would answer "something went wrong"); a moment later the drop is put in the inventory, the case and
// key are taken out, and the UI gets the same "crate_unlock" notification the real server path sends — so the reel
// lands on your drop and the normal reveal plays.
//
// Threads: the menu (render thread) only queues requests and reads snapshots. All inventory work happens on the game's
// main thread (GameTick from panorama's CUIEngine::RunFrame, OnUseTool from the UseTool hook) — the thread where the
// Game Coordinator's answers are handled too. It has to be that one: panorama deletes events sent from any other
// thread without a word (FrameStageNotify isn't the main thread — that's why test 2 still showed the error).
namespace Unboxer
{
    enum class Kind : int
    {
        Case = 0,
        Key = 1,
        Drop = 2,
    };

    inline constexpr int STICKER_SLOTS = 6;

    // Items made from one base definition + a value (Add items tab).
    inline constexpr std::uint16_t DEF_CHARM = 1355;         // a charm: paintKit = which one ("keychain slot 0 id")
    inline constexpr std::uint16_t DEF_STICKER_SLAB = 4000;  // the Sticker Slab tool: seals a sticker into a slab charm
    inline constexpr int KEYCHAIN_STICKER_SLAB = 37;         // the Sticker Slab charm (its sticker: Item::charmSticker)
    inline constexpr std::uint16_t DEF_PET = 4681;           // a chicken: petId / petSeed / petLevel
    inline constexpr std::uint16_t DEF_CHICKEN_EGG = 4948;
    inline constexpr std::uint16_t DEF_C4 = 49;              // takes a charm; equips in CS2's "c4" slot (T)
    inline constexpr std::uint16_t DEF_CHICKEN_FEED = 4949;   // "sticker slot 0-5" (the popup picks one of the gun model's positions)
    inline constexpr std::uint16_t DEF_CASKET = 1201;        // a Storage Unit: named first, then holds up to 1000 items
    inline constexpr std::uint16_t DEF_NAME_TAG_TOOL = 1200; // a Name Tag
    inline constexpr std::uint16_t DEF_STATTRAK_SWAP = 1324; // the StatTrak Swap Tool: swaps two StatTrak items' counts
    inline constexpr std::uint16_t DEF_CHARM_DETACH_PACK = 4950;   // a Charm Detachment Pack: 3 Charm Detachments
    inline constexpr int CASKET_CAPACITY = 1000;
    inline constexpr int CHARM_DETACHMENTS_PER_PACK = 3;

    // A sticker on one of our guns (applied with CS2's own sticker popup).
    struct Sticker
    {
        int kit = 0;                    // 0 = empty slot
        float wear = 0.0f;              // 0 new ... 1 scraped off
        float rotation = 0.0f;
        float offsetX = 0.0f;           // CS2's free placement, as the popup's 3D preview gave it
        float offsetY = 0.0f;
        int schema = -1;                // the gun's sticker position the offsets are relative to (the popup's slot); -1 = its own slot
    };

    struct Item
    {
        std::uint64_t id = 0;
        Kind kind = Kind::Case;
        std::uint16_t definition = 0;
        int paintKit = 0;
        int seed = 0;
        float wear = 0.0f;
        bool statTrak = false;
        int rarity = 0;
        int quality = 4;
        std::uint16_t fromCase = 0;     // drops: the case it was opened from
        std::uint8_t equipped = 0;      // EQUIP_T | EQUIP_CT: on your weapons in a match (skin changer)
        std::string slotT;              // CS2 loadout slot it's in on each side ("rifle1", "melee"...); "" = none
        std::string slotCT;
        std::array<Sticker, STICKER_SLOTS> stickers{};   // guns
        int kills = -1;                 // StatTrak: the kills shown; -1 = your real kills count up in a match
        std::string customName;         // name tag, "" = none
        int charm = 0;                  // guns: the charm hanging on it (keychain id), 0 = none
        int charmSeed = 0;              // its pattern (charm items: their own pattern)
        float charmX = 0.0f;            // where it hangs (CS2's charm popup; 0 = the gun's usual spot)
        float charmY = 0.0f;
        float charmZ = 0.0f;
        int charmSticker = 0;           // a Sticker Slab's sealed sticker kit (slab charm items; guns: the slab on it), 0 = none
        int highlight = 0;              // a Major highlight charm's clip (Cases::Highlight number; charm items: their own clip,
                                        // guns: the charm on it), 0 = none / the charm's own (Cologne 2026 on: Cases::HighlightOf)
        int tint = 0;                   // graffiti: its colour ("spray tint id", Cases::TintName 1-19), 0 = none
        int petId = 0;                  // DEF_PET: 2 chick, 3 Catalana, 4 Silkie, 5 Polish
        int petSeed = 0;                // its colour
        int petLevel = 0;               // 1 chick, 2 pullet, 3 hen
        std::uint32_t petHatch = 0;     // DEF_PET: when it hatched (unix time, "deployment date"), set when it's added
        std::uint32_t expires = 0;      // an unsealed terminal (quality 14): when it runs out (unix time)
        int offerNumber = 0;            // an unsealed terminal: its current offer (1, 2...; the final one negative), 0 = none yet
        int sprays = 0;                 // an unsealed graffiti: charges left (CS2's "sprays remaining"), 0 = a full one
        std::uint64_t casket = 0;       // the Lootline Storage Unit it's in (CS2 hides it from the inventory), 0 = none
        std::uint32_t modified = 0;     // a Storage Unit: when it was named / last changed (unix time), 0 = new (no name yet)
        int casketItems = 0;            // a Storage Unit: how many items are in it (counted again on load)
        CustomColor::Paint paint;       // guns / knives / gloves: the custom colours (Inventory -> Custom colour), off = the skin's own
    };

    inline constexpr int SPRAY_CHARGES = 50;   // an unsealed graffiti's charges, like CS2's

    enum : std::uint8_t
    {
        EQUIP_T = 1,
        EQUIP_CT = 2,
        EQUIP_BOTH = EQUIP_T | EQUIP_CT,
    };

    // What the next case you open gives you.
    struct DropSettings
    {
        bool random = true;             // true: a roll from the opened case with the real odds
        std::uint16_t weapon = 0;       // picked item (weapon, knife or gloves)
        int paintKit = 0;
        int tier = 5;                   // Cases::Tier of the picked item (rarity colour / number)
        float wear = 0.01f;
        int seed = 0;
        bool randomFloat = false;       // the picked skin's float: random in [floatFrom, floatTo] each time it comes (a wear's
        float floatFrom = 0.0f;         // range kept inside the finish's - the menu works it out) instead of `wear`
        float floatTo = 0.07f;
        int randomWear = 0;             // the menu's wear button for it (FN ... BS)
        bool randomSeed = false;        // its pattern: random each time instead of `seed`
        bool statTrak = false;
        int dropOn = 1;                 // cases: it drops on this opening (1 = the next one); the ones before are rolled (real odds)
        int terminalOn = 1;             // terminals: it comes in this terminal (1 = the next one you unseal) ...
        int offerOn = 1;                // ... as this offer (1-5); the other offers are rolled
        bool keepPicked = true;         // false: once it came (a case gave it / a terminal offered it), back to Random;
                                        // true: again every dropOn cases / terminalOn terminals
        bool showInCase = false;        // list the picked skin in the case's contents + reel in CS2 (looks legit)
        std::uint32_t revision = 0;     // Lootline's own changes (back to Random after "only once"): an older copy isn't written back
    };

    // For the loot list hooks (main thread): the picked skin to add to this case's contents in CS2, when "show in
    // the case" is on, the case is one of ours and the skin isn't a ★ its "Rare Special Item" tile already covers.
    struct ShownDrop
    {
        std::uint64_t fauxId = 0;       // the game's loot list id for a definition + finish
        std::uint16_t definition = 0;
        int paintKit = 0;
        int tier = 0;                   // Cases::Tier
        std::uint16_t caseDefinition = 0;
    };

    struct Status
    {
        bool resolved = false;          // every function / offset found
        bool inventoryReady = false;    // connected to the Game Coordinator, inventory loaded
        bool panoramaReady = false;     // can talk to the case popup
        bool casesReady = false;        // case list read from the game
        std::string error;              // what's missing
        int opened = 0;                 // this session
        std::string lastDrop;           // "AK-47 | Redline (Field-Tested)" — for the menu
    };

    bool Initialize();                  // render / init thread, once
    // The start-up thread, after the hooks: the signature check Settings runs; anything missing (a CS2 update) = a CS2
    // popup in the main menu naming what may not work. Waits for the check (up to a minute).
    void CheckAfterUpdate();

    // Lootline's prices in CS2's own screens (Settings -> Prices, "Prices in CS2's screens"): the menu's thread hands
    // over each Lootline item's price text, the total and the two titles (translated there); the game's main thread puts
    // them into panorama - under the item's name in CS2's inspect popup, left of the inventory's sort button.
    struct GamePrices
    {
        std::vector<std::pair<std::uint64_t, std::string>> items;   // id, "$12.34" ("~$12.34" = estimated)
        std::string total;              // all Lootline items, "" = none priced
        std::string itemTitle;          // "Lootline price: "
        std::string totalTitle;         // "Lootline items: "
    };
    void SetGamePrices(GamePrices prices);
    void SetGamePricesEnabled(bool on);
    void LogLine(const char* text);     // one line in unboxer_log.txt (any thread)
    void Shutdown();
    // The AcknowledgeNewItembyItemID hook is in: items added from the menu arrive NEW (else already seen - CS2's own
    // acknowledge would ask Valve about them).
    void SetNewItemsEnabled(bool enabled);
    // Unloading (Lootline's own thread, hooks still running): the main thread acknowledges every NEW Lootline item, so
    // CS2 never acknowledges one itself after the hook is gone. Waits up to timeoutMs.
    void AcknowledgeBeforeUnload(unsigned timeoutMs);
    void TickRenderThread();            // Menu::render: retries the case list, builds the ★ pool bit by bit

    void GameTick();                    // CUIEngine::RunFrame (main thread); FrameStageNotify only as a fallback
    bool OnUseTool(const char* toolId, const char* itemId);   // UseTool hook (main thread): true = ours, handled
    // WearItemSticker hook (main thread): scraping (wear 0-100) or taking off (> 100) a sticker of one of our guns.
    // True = ours, handled here.
    bool OnWearSticker(const char* itemId, int slot, int wear);
    // RemoveKeychain hook (main thread): the charm coming off one of our guns. True = ours, handled here.
    bool OnRemoveKeychain(const char* itemId, int slot);
    // SetNameToolString hook (main thread): the name typed for a name tag (the next name tag on our item uses it).
    void OnNameToolString(const char* text);
    // DeleteItem hook (main thread): CS2's "Delete" on a Lootline item. True = ours, removed here.
    bool OnDeleteItem(const char* itemId);
    // AcknowledgeNewItembyItemID hook (main thread): CS2's new item popup acknowledging an item. True = a Lootline id,
    // done here (never sent to Valve); wasNew = what the game's answer would be.
    bool OnAcknowledgeItem(const char* itemId, bool& wasNew);
    // ClearCustomName hook (main thread): the name tag's "Remove" on a Lootline item. True = ours, done here.
    bool OnClearCustomName(const char* itemId);
    // CS2's Uplink Terminals on Lootline items (main thread). UseToolWithIntArg(terminal, terminal, n): the next offer,
    // or the end after the final one. PerformItemCasketTransaction(0, terminal, terminal): a reopened terminal's offer.
    // StoreItemPurchase("def(id),..."): "accept" - anything with a Lootline id is never sent to Steam. True = ours.
    bool OnUseToolWithIntArg(const char* toolId, const char* itemId, int value);
    bool OnCasketTransaction(int operation, const char* casketId, const char* itemId);
    bool OnStoreItemPurchase(const char* items);

    // Settings -> Prices -> "Buy keys with Lootline" (render thread, every frame): CS2's own "buy a key" (a case without a
    // key: the popup's Buy, any quantity) gives Lootline keys - free, never sent to Steam - and CS2's popup goes on to
    // Unlock like after a real purchase. Off: the game's purchase. The keys bought count as spent at Valve's store price.
    void SetBuyKeys(bool on);
    struct KeySpending
    {
        double dollars = 0.0;   // US dollars (Prices::Format shows them in the picked currency)
        int keys = 0;
    };
    [[nodiscard]] KeySpending GetKeySpending();
    void ResetKeySpending();
    // CraftIngredients hook (main thread): the trade-up contract submitted. True = all Lootline items, done here.
    bool OnCraftIngredients(void* api);

    // The Armory with Lootline stars (Settings -> Armory): CS2's own Armory screen shows Lootline's star balance and every
    // redeem is done here - a case as it is, one roll (real odds) from a collection / sticker capsule / charm pack - and
    // never sent to Valve. SetArmoryEnabled: render thread, every frame. Stars: any thread (saved with the items).
    void SetArmoryEnabled(bool on);
    [[nodiscard]] int ArmoryStars();
    void SetArmoryStars(int stars);
    // MissionsAPI.ActionRedeemOperationGoods hook (main thread). True = done here (Lootline's Armory is on).
    bool OnRedeemArmory(int season, int index, int points);

    // Loadout shuffle with Lootline items (main thread). CS2's "Add to shuffle" / "Remove from shuffle" are its favorites:
    // OnFavorite (Add/RemoveItemFromFavorites hooks) keeps a Lootline item's itself - true = ours; IsFavorite (ItemIsInFavorites
    // hook): -1 = not ours; ShuffleExtra (GetShuffleItems hook): Lootline's items for that slot as '"id","id"' to add to the
    // game's list ("" = none). A match's first spawn (every spawn with SetShuffleEveryRound on), a slot with shuffle on
    // rolls one of all its items (Lootline's and the game's).
    bool OnFavorite(const char* team, const char* itemId, bool add);
    [[nodiscard]] int IsFavorite(const char* team, const char* itemId);
    [[nodiscard]] std::string ShuffleExtra(const char* team, const char* slot);
    // Settings -> "Shuffle every round" (render thread, every frame): off = once per match, like CS2.
    void SetShuffleEveryRound(bool on);
    // A map loads (AgentRules level init hook, main thread): the next spawn starts a new match.
    void OnLevelChange();

    // InventoryAPI.SetStatTrakSwapToolItems hook (main thread): the two items CS2's StatTrak Swap popup is about to swap (a
    // Lootline StatTrak Swap Tool swaps two Lootline StatTrak items of the same weapon locally).
    void OnStatTrakSwapItems(const char* first, const char* second);
    // A StatTrak item's kills: the number set on it, or (kills -1) your real kills so far - the skin changer's count for
    // that weapon + finish, credited in a match. CS2's inventory gets the same number (written when it changes). Any thread.
    [[nodiscard]] int ShownKills(const Item& item);
    // Lootline's Charm Detachments (from Lootline Charm Detachment Packs; one is used per charm taken off a Lootline gun), added
    // to the count CS2 shows (KeychainRemoveToolCharges). Any thread.
    [[nodiscard]] int CharmDetachments();
    bool ShownDropFor(const char* caseItemId, ShownDrop& out);   // loot list hooks (main thread)

    // The game's loot list ids ("faux" item ids): 0xF000000000000000 | finish << 16 | definition.
    [[nodiscard]] std::uint64_t FauxId(std::uint16_t definition, int paintKit);
    [[nodiscard]] bool FromFauxId(std::uint64_t id, std::uint16_t& definition, int& paintKit);

    // The panorama UI engine (whose RunFrame the hooks swap in the vtable), or null before the game made it.
    [[nodiscard]] void* PanoramaEngine();
    [[nodiscard]] bool OnMainThread();  // tier0 ThreadInMainThread (true if tier0 doesn't have it)

    // ── menu (render thread) ──
    inline constexpr int MAX_ADD_COUNT = 1000;   // per "Add" (CS2's own inventory holds 1000 real items)
    void AddCase(std::uint16_t caseDefinition, int count, bool withKeys);
    void AddKey(std::uint16_t keyDefinition, int count);
    // Add items tab: `count` copies of `item` (a drop of any kind: skin, agent, sticker, charm, pet...) put in your
    // inventory over the next main frames. randomFloat: each copy gets its own float in [from, to] instead of item.wear
    // (Add items' "Random float": a wear's range, already kept inside the finish's). randomSeed: each copy gets its own
    // pattern (0 - 1000) instead of item.seed (Add items' "Random seed").
    struct RandomFloat
    {
        bool on = false;
        float from = 0.0f;
        float to = 0.0f;
    };
    void AddItem(const Item& item, int count, RandomFloat randomFloat = {}, bool randomSeed = false);
    void Remove(std::uint64_t id);
    void RemoveAll();

    [[nodiscard]] std::vector<Item> Items();
    [[nodiscard]] Status GetStatus();
    [[nodiscard]] DropSettings GetDropSettings();
    void SetDropSettings(const DropSettings& settings);   // another item / number / mode starts the count again

    // Cases tab -> Random -> "Custom odds": a container's own chance per grade (Cases::Tier order, in %; only the grades the
    // container has count, scaled to 100 % together). Saved with the items. Any thread.
    using TierOdds = std::array<double, 7>;   // Cases::TierCount
    [[nodiscard]] bool GetCustomOdds(std::uint16_t container, TierOdds& out);   // false = it rolls with the real odds
    void SetCustomOdds(std::uint16_t container, const TierOdds& odds);
    void ClearCustomOdds(std::uint16_t container);
    [[nodiscard]] int PickedOpenings();   // Choose: cases opened since the item was picked / last dropped
    [[nodiscard]] int PickedTerminals();  // Choose: terminals unsealed since the item was picked / last offered
    // Settings -> "Drop message in chat" (every frame): each case you open in a match prints CS2's own "<you> has
    // opened a container and found: <item>" line in your chat when the reel stops. Only you see it.
    void SetChatDrops(bool enabled);
    // Settings -> "Reapply now": every equipped item put on again in the match you're in (gloves included).
    void ReapplyInMatch();

    // ── equipping: the skin changer paints equipped drops on your gun / knife / gloves in a match ──
    // Skins (guns, knives, gloves), agents, music kits, pets and unsealed graffiti can be equipped. One per side for each gun, one knife, one pair of
    // gloves; equipping takes the previous one off that side. sides = EQUIP_* (0 = take it off). Any thread.
    [[nodiscard]] bool CanEquip(const Item& item);
    void Equip(std::uint64_t id, std::uint8_t sides);   // also put in / taken out of CS2's loadout (next main frame)
    // Inventory -> Custom colour (render thread): a gun's / knife's / gloves' colours. CS2's screens and the preview get
    // them right away; an equipped item is put on again in the match a moment after the last change.
    [[nodiscard]] bool CanPaint(const Item& item);
    void SetPaint(std::uint64_t id, const CustomColor::Paint& paint);

    // Inventory tab -> Loadouts: what's equipped (the Lootline items and their sides) saved under a name, put back on with one
    // click - the rest of the equipped Lootline items come off. Saved with the items. Any thread.
    struct LoadoutInfo
    {
        std::string name;
        int items = 0;      // still in your inventory
        int missing = 0;    // removed / traded up since it was saved
    };
    [[nodiscard]] std::vector<LoadoutInfo> Loadouts();
    bool SaveLoadout(const std::string& name);      // false = nothing equipped; the same name is saved over
    int ApplyLoadout(const std::string& name);      // how many items went on
    void DeleteLoadout(const std::string& name);
    // LoadoutAPI.EquipItemInSlot hook (main thread), after CS2 equipped something: mirrors it for Lootline items, and
    // takes a Lootline item off a side when something else goes into its slot there.
    void OnLoadoutEquip(const char* team, const char* itemId, const char* slot);
    // EquipItemInSlot hook, when CS2 refused (a competitive match locks the loadout): a Lootline item is equipped
    // anyway - it only has to look right in the match. True = it was one of ours, in a slot of its kind.
    bool ForceLoadoutEquip(const char* team, const char* itemId, const char* slot);
    // LoadoutAPI.GetItemID hook (main thread): the Lootline item Lootline has in that team's slot ("t", "ct", "noteam"),
    // as text, when CS2's own answer (nativeId) is another item - the loadout screen shows what the match shows. Null =
    // CS2's answer stands. The text lives until the next call.
    [[nodiscard]] const char* LoadoutShownItem(const char* team, const char* slot, const char* nativeId);
    // InventoryAPI.IsEquipped hook (main thread): a Lootline item Lootline has in a slot of that team.
    [[nodiscard]] bool LoadoutShowsEquipped(const char* itemId, const char* team);
    // The spray wheel's slot (0-3, CS2's "spray0"...): the Lootline graffiti in it - its kit and colour. False = none of
    // Lootline's there (a real one, or nothing). Any thread (the shown-slots lock only).
    [[nodiscard]] bool EquippedGraffiti(int slot, int& kit, int& tint);
    // LocalSpray, main thread: the Lootline graffiti in that spray slot was just sprayed - CS2's spray sound, and one of
    // its charges is used (at 0 it's gone, like CS2's).
    void OnGraffitiSprayed(int slot);
    // A Lootline item id (not a real item, not the store's): opening it has no regional restriction (X-Ray / loot box
    // laws are about Valve's cases - Lootline's never reach Valve). Any thread, no lock.
    [[nodiscard]] bool IsLootlineItemId(const char* itemId);
    // The same for an id as a number (the trade-up recipe hooks read them out of the game's item list).
    [[nodiscard]] bool IsLootlineId(std::uint64_t id);
    // Render thread, every frame: rebuilds the skin changer's loadout (Config::skins) when the equipment changed.
    void SyncLoadout();
    // Settings -> "Reapply skins when you spawn" (render thread, every frame): on / off, and when - seconds after each
    // spawn, one reapply each (up to 8; the menu's list).
    void SetReapplyOnSpawn(bool on, const std::vector<float>& secondsAfterSpawn);
    // Settings -> "Lootline pin on the scoreboard" (render thread, every frame).
    void SetScoreboardPin(bool on);
    // GetFlairItemId / GetFlairItemImage hooks (main thread): your equipped Lootline pin / coin / medal for the
    // scoreboard - its item id and picture (image_inventory; the scoreboard adds "_small.png"). False = not you, no
    // Lootline pin equipped, or switched off: the game's own answer.
    bool ScoreboardFlair(const char* xuid, std::uint64_t& id, std::string& image);
    // Game thread, FSN NET_UPDATE_END: a new spawn of the local player -> every skin put on again (and the gloves) at
    // the times picked in Settings (the game is still handing out the weapons at first), and a "spawn check" line in
    // the log a few seconds later (each weapon's skin and whether its paint is done). Not in a demo.
    void WatchSpawn(void* localPawn);
    // StoreAPI.GetStoreItemEmbeddedAttributePrice hook (main thread): a Lootline terminal offer's price as text in the
    // currency picked in Settings -> Prices ("$0.61", "52,16 руб."...). Null = not one of Lootline's offers.
    [[nodiscard]] const char* OfferPriceText(std::uint64_t itemId);
    // The equipped Lootline pet - CS2's live pet for its scripts (main menu chicken, loadout, pet book), 0 = none
    // (CS2's own then). Any thread.
    [[nodiscard]] std::uint64_t LivePetId();

    // ── upgrader: trade one of your drops for a better skin, won or lost when the wheel stops ──
    struct UpgradeStats
    {
        int tries = 0;
        int wins = 0;
        int losses = 0;
        double bestMultiplier = 0.0;    // biggest win
        std::string bestName;
        int streak = 0;                 // > 0 wins in a row, < 0 losses in a row
    };

    // At `delayMs` from now (when the wheel stops) on the game thread: the input items go (1-6, their prices added up);
    // on a win the target comes. False if one isn't one of your drops or is already being upgraded.
    bool Upgrade(const std::vector<std::uint64_t>& inputIds, const Item& target, bool win, double multiplier, unsigned delayMs);
    [[nodiscard]] bool UpgradePending();
    [[nodiscard]] UpgradeStats GetUpgradeStats();
    void ResetUpgradeStats();

    // "AK-47 | Redline (Field-Tested)" / "★ Karambit | Fade" / "Kilowatt Case" — render thread.
    [[nodiscard]] std::string DisplayName(const Item& item);
    [[nodiscard]] const char* WearName(float wear);
}
