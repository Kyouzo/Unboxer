#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

// Items in your CS2 inventory, made by us. They go into the game's own item cache (the "shared object cache" the
// Game Coordinator normally fills), so the inventory, the case popup and the reveal show them like real items.
// They only exist on this PC — Valve's servers never hear of them.
//
// Everything here was checked against the 2026-09-29 client.dll (see the notes in game_inventory.cpp). Only call it
// on the game's main thread (panorama RunFrame / the UseTool hook): the game reads and fills that cache there too.
namespace GameInv
{
    inline constexpr int STICKER_SLOTS = 6;   // "sticker slot 0-5": a gun model has up to 6 sticker positions

    // One sticker on a gun. kit 0 = an empty slot.
    struct StickerSlot
    {
        int kit = 0;                // sticker kit ("sticker slot N id")
        float wear = 0.0f;          // 0 new ... 1 scraped off
        float rotation = 0.0f;
        float offsetX = 0.0f;       // CS2's free placement, as the sticker popup's 3D preview gives it
        float offsetY = 0.0f;
        int schema = -1;            // the gun's sticker position the offsets are relative to; -1 = the slot itself
    };

    struct NewItem
    {
        std::uint64_t id = 0;
        std::uint16_t definition = 0;
        int quality = 4;            // 4 unique, 9 strange (StatTrak), 3 unusual (★)
        int rarity = 1;             // items_game rarity number (1 consumer ... 6 covert)
        int origin = 0;             // 8 = found in crate
        int paintKit = 0;           // 0 = none
        int seed = 0;
        float wear = 0.0f;
        bool statTrak = false;
        int statTrakKills = 0;
        int statTrakType = 0;       // kill eater score type: 0 kills, 1 MVPs (music kits)
        int stickerKit = 0;         // stickers / patches / graffiti: "sticker slot 0 id"
        int musicKit = 0;           // music kits: "music id"
        std::array<StickerSlot, STICKER_SLOTS> stickers{};   // guns: the stickers on them
        std::string customName;     // name tag ("custom name attr"), "" = none
        int keychain = 0;           // "keychain slot 0 id": a charm item's charm, or the charm hanging on a gun
        int keychainSeed = 0;       // its pattern
        float keychainX = 0.0f;     // where it hangs (0 = the gun's usual spot)
        float keychainY = 0.0f;
        float keychainZ = 0.0f;
        int keychainSticker = 0;    // a Sticker Slab's sealed sticker ("keychain slot 0 sticker"), 0 = none
        int keychainHighlight = 0;  // a Major highlight charm's clip ("keychain slot 0 highlight"): inspecting plays it
        int sprayTint = 0;          // graffiti: its colour ("spray tint id" 1-19), 0 = none
        int sprayCharges = 0;       // an unsealed graffiti: "sprays remaining" (CS2 shows "Charges Remaining"), 0 = none
        int petId = 0;              // pets: "pet id" (pet_definitions: 2 chick, 3 Catalana, 4 Silkie, 5 Polish); 0 = not a pet
        int petSeed = 0;            // "pet seed": its colour
        int petLevel = 0;           // "upgrade level": 0 egg, 1 chick, 2 pullet, 3 hen
        std::uint32_t petHatch = 0; // "deployment date": when it hatched (CS2's pet book shows it), 0 = now
        std::uint32_t expirationDate = 0;   // "expiration date" (unix time): an unsealed terminal; 0 = none
        int offerNumber = 0;        // "quest points remaining": an unsealed terminal's current offer (1, 2...; the final one
                                    // negative), 0 = none made yet
        std::uint64_t casketId = 0; // "casket item id low / high": the container it's inside (a terminal's offer), 0 = none
        int purchasePrice = 0;      // "purchase price", cents: what a terminal's offer costs (shown only), 0 = none
        std::uint32_t modificationDate = 0;   // a named storage unit: "modification date" (unix time); 0 = a new one
        int itemsCount = 0;         // ... and its "items count"; without a date: a Charm Detachment Pack's (0 = none)
        int pickup = 0;             // > 0: arrives NEW (unacknowledged) - the inventory tab's count + CS2's new item popup;
                                    // how you got it (CS2's list: 1 dropped ... 4 purchased, 5 found in crate)
    };

    inline constexpr int PICKUP_PURCHASED = 4;
    // An Armory reward: InventoryAPI.GetItemPickupMethod gives "xpshopredeem" (its list is indexed by pickup - 1), and
    // CS2's store then shows its reward popup over the Armory.
    inline constexpr int PICKUP_XPSHOP_REDEEM = 23;

    // Resolves every function / offset. Any thread, once (read-only scans). False = something wasn't found; the
    // reason is in LastError().
    bool Initialize();
    [[nodiscard]] const char* LastError();

    // Game thread. Your inventory exists (you're connected to the Game Coordinator).
    [[nodiscard]] bool InventoryReady();

    // Game thread. True if the item was added.
    bool CreateItem(const NewItem& item);
    // Game thread. CS2's acknowledge of a NEW item, done locally for one of ours (Valve never hears of it): it gets a
    // place in the inventory, the "new" count drops. wasNew: it was new. False = not found.
    bool Acknowledge(std::uint64_t id, bool& wasNew);
    // Game thread. Items in your inventory still NEW (unacknowledged), real ones too.
    [[nodiscard]] int NewItemCount();
    [[nodiscard]] std::vector<std::uint64_t> NewItemIds();
    // Game thread. Takes the item out of the inventory (only ours are ever passed here).
    bool RemoveItem(std::uint64_t id);
    [[nodiscard]] bool HasItem(std::uint64_t id);

    // Game thread. Puts `sticker` into `slot` of one of our items (kit 0 = takes it off) and tells the inventory, so
    // the UI redraws it like after a Game Coordinator answer.
    bool SetSticker(std::uint64_t id, int slot, const StickerSlot& sticker);
    // Game thread. Where the sticker popup's 3D preview has the sticker right now: what CS2 itself sends with "apply"
    // (slot, offset, rotation, wear). False when that part of the game wasn't found.
    bool StickerPlacement(int& slot, StickerSlot& placement);
    // Game thread. Hangs a charm on one of our guns (keychain 0 = takes it off), and tells the inventory.
    bool SetKeychain(std::uint64_t id, int keychain, int seed, float x, float y, float z, int sticker = 0, int highlight = 0);
    // Game thread. Where CS2's charm popup has the charm right now (what it would send with "apply").
    bool KeychainPlacement(float& x, float& y, float& z);
    // Game thread. The slot CS2's patch popup picked on an agent, -1 = none (or not found).
    [[nodiscard]] int PatchSlot();
    // Game thread. A name tag on one of our items. petLevel >= 2: a grown pet (CS2 keeps a name per life stage).
    bool SetName(std::uint64_t id, const std::string& name, int petLevel);
    // Game thread. An unsealed terminal's current offer number ("quest points remaining"; the final offer negative).
    bool SetOfferNumber(std::uint64_t id, int offerNumber);
    // Game thread. An unsealed graffiti's charges left ("sprays remaining").
    bool SetSprayCharges(std::uint64_t id, int charges);
    // Game thread. A StatTrak item's kills ("kill eater") - what CS2's inventory, inspect and loadout show.
    bool SetKills(std::uint64_t id, int kills);
    // Game thread. A named storage unit's "items count" + "modification date".
    bool SetCasketInfo(std::uint64_t id, int itemsCount, std::uint32_t modificationDate);
    // Game thread. The sticker kit of a sticker item in your inventory (real or ours), 0 if there is none.
    [[nodiscard]] int StickerKitOf(std::uint64_t id);
    // Game thread. The sticker kit CS2's own view of an item shows in a slot (0 = none): the check after applying one.
    [[nodiscard]] int StickerKitAt(std::uint64_t id, int slot);

    // What CS2's own view of an item shows - any item: a real one, one of ours, or a stock one (the default weapons'
    // "faux" ids). For making a copy of it. Game thread. False if the game doesn't know the id.
    struct ItemSnapshot
    {
        std::uint16_t definition = 0;
        int quality = 0;            // 0 stock, 4 unique, 9 StatTrak, 12 Souvenir, 3 ★
        int paintKit = 0;
        int seed = 0;
        float wear = 0.0f;
        std::string customName;
        std::array<StickerSlot, STICKER_SLOTS> stickers{};
    };
    bool ReadItem(std::uint64_t id, ItemSnapshot& out);
    // The sticker functions were found (for the log).
    [[nodiscard]] bool StickersResolved();
    // Where the sticker / charm / patch popups' placement came from (for the log): "signatures", or read from CS2's
    // tools when an update broke those signatures.
    [[nodiscard]] const char* PlacementSource();
    // Game thread. Definition index of any item in your inventory (real or ours), 0 if not found.
    [[nodiscard]] std::uint16_t ItemDefinition(std::uint64_t id);

    // Game thread. Next free inventory position / your account id.
    [[nodiscard]] std::uint32_t NextPosition();
    [[nodiscard]] std::uint32_t AccountId();

    // For the UseTool hook's resolution in hooks.cpp: the UseTool function (found here while scanning).
    [[nodiscard]] void* UseToolFunction();

    // A panorama InventoryAPI function by its JS name ("GetLootListItemsCount"), from the table client.dll registers
    // them with: lea rdx, [function] / lea rcx, ["name"] / call. Any thread (read-only scan). Null if not found.
    [[nodiscard]] void* InventoryApiFunction(const char* name);
    // A panorama StoreAPI function by its JS name ("StoreItemPurchase"): registered as lea rax, [function] /
    // mov [rbp-10h], rax ... lea rcx, ["name"] / call. Any thread. Null if not found.
    [[nodiscard]] void* StoreApiFunction(const char* name);
    // client.dll's code section, and `name` as its own zero-terminated string in its read-only data (null if not there).
    // For finding things by the names the game registers them with. Any thread.
    bool ClientCode(const std::uint8_t*& text, std::size_t& textSize);
    [[nodiscard]] const std::uint8_t* ClientString(const char* name);

    // CS2's loadout (main thread, what the loadout screen calls). team: "t", "ct" or "noteam"; id 0 = the default
    // item back in that slot. DefaultSlot: where an item goes ("rifle1", "melee", "clothing_hands", "musickit"...).
    bool LoadoutEquip(const char* team, std::uint64_t id, const char* slot);
    [[nodiscard]] std::string DefaultSlot(std::uint64_t id);
    [[nodiscard]] void* LoadoutEquipFunction();   // LoadoutAPI.EquipItemInSlot, for the hook in hooks.cpp

    // The trade-up contract (main thread). InventoryAPI keeps its items in its own object: CraftIngredients(api)
    // reads them out (false = the list wasn't found / unreadable), ClearCraftIngredients(api) empties it the way the
    // panel's "clear" does (the panel updates). CraftIngredientsFunction: for the hook in hooks.cpp.
    bool CraftIngredients(void* api, std::vector<std::uint64_t>& out);
    bool ClearCraftIngredients(void* api);
    [[nodiscard]] void* CraftIngredientsFunction();
    // The item ids in a list a crafting recipe checks (CUtlVector<CEconItem*>: the count, then the items at +8). Any
    // thread, nothing allocated. The count (up to `max` ids written), or -1 if unreadable or longer than `max`.
    [[nodiscard]] int CraftListIds(const void* list, std::uint64_t* ids, int max);

    // "6=Uint32 7=Uint32 8=Float 80=Uint32" — the attribute types found in the game (for the log).
    [[nodiscard]] std::string AttributeTypes();
}
