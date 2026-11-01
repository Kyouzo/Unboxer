#include "hooks.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <intrin.h>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include "../../patterns.h"
#include "../chadware2.h"
#include "../config/config.h"
#include "../features/model_preview/model_preview.h"
#include "../features/movement/movement.h"
#include "../features/skinchanger/skinchanger.h"
#include "../features/skinchanger/agent_voice_changer.h"
#include "../features/skinchanger/knife_animation.h"
#include "../features/skinchanger/item_schema.h"
#include "../../cs2/entity/C_CSPlayerPawn/C_CSPlayerPawn.h"
#include "../globals/globals.h"
#include "../features/unboxer/cases.h"
#include "../features/unboxer/custom_color.h"
#include "../features/unboxer/game_inventory.h"
#include "../features/unboxer/lineup.h"
#include "../features/unboxer/local_spray.h"
#include "../features/unboxer/unboxer.h"
#include "../interfaces/interfaces.h"
#include "../utils/memory/patternscan/patternscan.h"

namespace
{
    constexpr int FRAME_RENDER_START = 5;
    constexpr int FRAME_RENDER_END = 6;
    constexpr int FRAME_NET_UPDATE_END = 7;

    constexpr int VFUNC_MOUSE_INPUT_ENABLED = 23;    // CSGOInput
    constexpr int VFUNC_RELATIVE_MOUSE_MODE = 76;    // InputSystem

    // FrameStageNotify's address, even when Chadware (or anything else) already inline-hooked it: then its first
    // 5 bytes are a jump, so the rest of the signature is used (unique on its own on the 2026-09-29 build).
    void* FindFrameStageNotify()
    {
        if (std::uint8_t* found = M::FindPattern("client.dll", Patterns::FrameStageNotify))
            return found;

        std::uint8_t* tail = M::FindPattern("client.dll", "? ? ? ? ? 48 89 6C 24 ? 57 48 83 EC 40 48 8B F9 33 ED");

        if (tail && (tail[0] == 0xE9 || (tail[0] == 0xFF && tail[1] == 0x25)))
            return tail;

        return nullptr;
    }

    void** VTableOf(void* object)
    {
        return object ? *reinterpret_cast<void***>(object) : nullptr;
    }

    // The thread inside a trade-up recipe check of only Lootline items (0 = none): there a Souvenir red counts as a
    // normal one when its collection is asked for knives / gloves (hkTradeUpCollectionRare).
    constexpr int QUALITY_UNIQUE = 4;
    constexpr int QUALITY_SOUVENIR = 12;
    constexpr int MAX_RECIPE_ITEMS = 16;   // a contract holds 10, the checks add the one being tried
    std::atomic<DWORD> g_lootlineContractThread{ 0 };

    // RunFrame hook install / unload: FrameStageNotify may retry the install while Page Down unloads, so both take
    // this lock and nothing is installed once the unload started.
    std::mutex g_uiHookMutex;
    bool g_uiHookClosed = false;
    ULONGLONG g_uiHookNextTry = 0;
    bool g_uiHookScanned = false;
    void* g_uiRunFrameFunction = nullptr;

    // Swaps the UI engine's RunFrame slot. The engine exists as soon as the game shows its menu; if it doesn't yet,
    // FrameStageNotify keeps retrying (every ~2 s) and ticks Unboxer meanwhile.
    bool TryInstallUiRunFrame()
    {
        std::lock_guard lock(g_uiHookMutex);

        if (H::UiRunFrame.Installed())
            return true;

        const ULONGLONG now = GetTickCount64();

        if (g_uiHookClosed || now < g_uiHookNextTry)
            return false;

        g_uiHookNextTry = now + 2000;

        if (!g_uiHookScanned)
        {
            g_uiHookScanned = true;   // one scan: a broken signature stays broken (Settings -> Check signatures)
            g_uiRunFrameFunction = M::FindPattern("panorama.dll", Patterns::UiEngineRunFrame);
        }

        void** vtable = VTableOf(Unboxer::PanoramaEngine());

        if (!vtable || !g_uiRunFrameFunction)
            return false;

        const int slot = VmtSlotHook::FindSlot(vtable, g_uiRunFrameFunction, 64);
        return slot >= 0 && H::UiRunFrame.Install(vtable, slot, reinterpret_cast<void*>(&H::hkUiRunFrame));
    }
}

namespace
{
    // ── a case's contents with the picked skin in it ("show it in the case") — main thread (panorama JS) ──
    std::string g_lootCase;
    std::vector<std::string> g_lootIds;

    // Rebuilt on every call, so it always matches the current pick (a list is ~20 entries). False = the game's own.
    bool BuildLootView(void* api, const char* caseId)
    {
        Unboxer::ShownDrop shown;

        if (!caseId || !Unboxer::ShownDropFor(caseId, shown))
            return false;

        const auto count = H::GetLootListItemsCount.GetOriginal();
        const auto idAt = H::GetLootListItemIdByIndex.GetOriginal();

        if (!count || !idAt)
            return false;

        const int total = count(api, caseId);

        if (total <= 0 || total > 512)
            return false;   // no contents list (graffiti, music kits...): nothing to add to

        const Cases::Case* info = Cases::Find(shown.caseDefinition);
        std::vector<std::string> ids;
        std::vector<int> tiers;
        ids.reserve(static_cast<std::size_t>(total) + 1);
        tiers.reserve(static_cast<std::size_t>(total));

        for (int i = 0; i < total; ++i)
        {
            const char* id = idAt(api, caseId, i);
            ids.emplace_back(id ? id : "0");

            int tier = -1;
            std::uint16_t definition = 0;
            int paintKit = 0;

            if (ids.back() == "0")
            {
                tier = Cases::TierCount;   // the "★ Rare Special Item" tile: stays last
            }
            else if (Unboxer::FromFauxId(_strtoui64(ids.back().c_str(), nullptr, 10), definition, paintKit))
            {
                if (definition == shown.definition && paintKit == shown.paintKit)
                    return false;   // already listed

                tier = info ? Cases::TierIn(*info, definition, paintKit) : -1;
            }

            tiers.push_back(tier);
        }

        // the lists go Mil-Spec ... Covert, ★ last: after the last one of its tier or lower
        std::size_t at = 0;
        for (std::size_t i = 0; i < tiers.size(); ++i)
            if (tiers[i] >= 0 && tiers[i] <= shown.tier)
                at = i + 1;

        char text[32];
        snprintf(text, sizeof(text), "%llu", static_cast<unsigned long long>(shown.fauxId));
        ids.insert(ids.begin() + static_cast<std::ptrdiff_t>(at), text);

        g_lootCase = caseId;
        g_lootIds = std::move(ids);
        return true;
    }
}

namespace
{
    bool IsGunSlot(const char* slot)
    {
        const std::string_view name(slot ? slot : "");
        return name.starts_with("secondary") || name.starts_with("smg") || name.starts_with("rifle") || name.starts_with("heavy");
    }

    // What CS2 itself has in that team's slot (its own answer, not the one Lootline shows), 0 = none / unknown.
    std::uint64_t NativeLoadoutItem(void* api, const char* team, const char* slot)
    {
        const auto original = H::LoadoutGetItemId.GetOriginal();
        const char* text = original ? original(api, team, slot) : nullptr;
        return text ? _strtoui64(text, nullptr, 10) : 0;
    }
}

// The loadout screen equipping something (main thread). CS2 equips it locally either way; Lootline items are then
// also equipped for the skin changer, and a Lootline item pushed out of its slot comes off.
bool __fastcall H::hkLoadoutEquip(void* api, const char* team, const char* itemId, const char* slot)
{
    const HookCallGuard guard;
    const auto original = LoadoutEquip.GetOriginal();

    if (!original)
        return false;

    const std::uint64_t id = itemId ? _strtoui64(itemId, nullptr, 10) : 0;
    const bool lootline = Unboxer::IsLootlineItemId(itemId);

    // A Lootline gun going into a slot where CS2 has ANOTHER gun (an MP7 where your Steam loadout has the MP5-SD): the
    // server sells what your Steam loadout has, so the stock version of this gun goes into the slot first - an ordinary
    // loadout change with a default item, like clicking the plain MP7 in CS2 - and then the buy menu has it. Refused (a
    // match locks the loadout): the swap waits for the match to end, the gun in the slot keeps its Lootline skin.
    std::uint16_t wanted = 0, current = 0;

    if (lootline && IsGunSlot(slot))
    {
        wanted = GameInv::ItemDefinition(id);
        const std::uint64_t native = NativeLoadoutItem(api, team, slot);
        current = native ? GameInv::ItemDefinition(native) : 0;

        if (wanted && current && wanted != current)
        {
            char stock[32];
            snprintf(stock, sizeof(stock), "%llu", static_cast<unsigned long long>(Unboxer::FauxId(wanted, 0)));
            const bool swapped = original(api, team, stock, slot);

            char line[200];
            snprintf(line, sizeof(line), swapped ? "loadout: the stock gun %u goes into %s (%s) - the buy menu sells it now, its Lootline skin goes on it" :
                "loadout: CS2 locks which gun %u's slot %s (%s) buys in this match - the swap waits for the match to end", static_cast<unsigned>(wanted), slot, team ? team : "?");
            Unboxer::LogLine(line);

            if (!swapped)
                return false;   // CS2's own "Loadout Locked" popup; nothing moves
        }
    }

    const bool equipped = original(api, team, itemId, slot);

    if (equipped)
    {
        Unboxer::OnLoadoutEquip(team, itemId, slot);
    }
    else if (lootline)   // a Lootline item id
    {
        // CS2 said no - a competitive match locks the loadout ("Loadout Locked"). A Lootline item only has to look right
        // in the match, so it's equipped anyway and the skin changer puts it on (CS2's own loadout screen keeps the old
        // one until the match is over). True: the screen doesn't show the "Loadout Locked" popup.
        if (Unboxer::ForceLoadoutEquip(team, itemId, slot))
            return true;

        char line[160];
        snprintf(line, sizeof(line), "loadout: CS2 refused %s in %s (%s) - use Equip in the Lootline menu", itemId, slot ? slot : "?", team ? team : "?");
        Unboxer::LogLine(line);
    }

    return equipped;
}

// The sticker popup scraping a sticker (wear 0-100) or taking it off (> 100). Valve doesn't know Lootline guns:
// theirs are done locally; everything else goes to the game. (client.dll+0xFFCC80 on 2026-10-01: rdx item id string,
// r8d slot, r9d wear as an int, returns bool.)
bool __fastcall H::hkWearItemSticker(void* api, const char* itemId, int slot, int wear)
{
    const HookCallGuard guard;

    if (Unboxer::OnWearSticker(itemId, slot, wear))
        return true;

    const auto original = WearItemSticker.GetOriginal();
    return original ? original(api, itemId, slot, wear) : false;
}

// The charm popup's "remove" (main thread, client.dll+0xFE5070 on 2026-10-01: rdx item id string, r8d slot). A
// Lootline gun's charm comes off locally; everything else goes to the game.
bool __fastcall H::hkRemoveKeychain(void* api, const char* itemId, int slot)
{
    const HookCallGuard guard;

    if (Unboxer::OnRemoveKeychain(itemId, slot))
        return true;

    const auto original = RemoveKeychain.GetOriginal();
    return original ? original(api, itemId, slot) : false;
}

// The name typed for a name tag (main thread, client.dll+0xFEF1A0: rdx the text, r8 an item id string; returns
// whether CS2 accepts the name). Only remembered here - CS2 still checks it.
bool __fastcall H::hkSetNameToolString(void* api, const char* text, const char* itemId)
{
    const HookCallGuard guard;
    const auto original = SetNameToolString.GetOriginal();
    const bool valid = original ? original(api, text, itemId) : false;

    Unboxer::OnNameToolString(valid ? text : nullptr);
    return valid;
}

// CS2's "Delete" (main thread, client.dll+0xF891D0 on 2026-10-02: rdx an item id string, nothing returned). Valve
// doesn't know Lootline items (nothing happened): they're removed here; everything else goes to the game.
void __fastcall H::hkDeleteItem(void* api, const char* itemId)
{
    const HookCallGuard guard;

    if (Unboxer::OnDeleteItem(itemId))
        return;

    const auto original = DeleteItem.GetOriginal();
    if (original)
        original(api, itemId);
}

// CS2's new item popup acknowledging an item (main thread, client.dll+0xF7A220 on 2026-10-08: rdx an item id string,
// al = it was new). The game's own acknowledge asks Valve to move the item (and retries while it waits): a Lootline item
// is acknowledged here instead and never reaches it. Everything else goes to the game.
bool __fastcall H::hkAcknowledgeNewItem(void* api, const char* itemId)
{
    const HookCallGuard guard;
    bool wasNew = false;

    if (Unboxer::OnAcknowledgeItem(itemId, wasNew))
        return wasNew;

    const auto original = AcknowledgeNewItem.GetOriginal();
    return original ? original(api, itemId) : false;
}

// The scoreboard's pin column (main thread, client.dll+0xF969E0 / +0xF96A40 on 2026-10-08: rdx the player's xuid as
// text, rax text). Yours: your equipped Lootline pin (its id - the scoreboard only compares it - and its picture).
namespace
{
    char g_flairIdText[24];      // main thread: what the last answer points at
    std::string g_flairImage;
}

const char* __fastcall H::hkGetFlairItemId(void* api, const char* xuid)
{
    const HookCallGuard guard;
    std::uint64_t id = 0;
    std::string image;

    if (Unboxer::ScoreboardFlair(xuid, id, image))
    {
        snprintf(g_flairIdText, sizeof(g_flairIdText), "%llu", static_cast<unsigned long long>(id));
        return g_flairIdText;
    }

    const auto original = GetFlairItemId.GetOriginal();
    return original ? original(api, xuid) : "";
}

const char* __fastcall H::hkGetFlairItemImage(void* api, const char* xuid)
{
    const HookCallGuard guard;
    std::uint64_t id = 0;
    std::string image;

    if (Unboxer::ScoreboardFlair(xuid, id, image))
    {
        g_flairImage = std::move(image);
        return g_flairImage.c_str();
    }

    const auto original = GetFlairItemImage.GetOriginal();
    return original ? original(api, xuid) : "";
}

// The name tag popup's "Remove" (main thread, client.dll+0xF842E0 on 2026-10-02: rdx an item id string, nothing
// returned). A Lootline item's name comes off here; everything else goes to the game.
void __fastcall H::hkClearCustomName(void* api, const char* itemId)
{
    const HookCallGuard guard;

    if (Unboxer::OnClearCustomName(itemId))
        return;

    const auto original = ClearCustomName.GetOriginal();
    if (original)
        original(api, itemId);
}

// A terminal's laptop asking for its next offer / ending (main thread, client.dll+0xFFBFD0 on 2026-10-03: rdx tool id,
// r8 item id, r9d the number). A Lootline terminal is answered locally; everything else goes to the game.
void __fastcall H::hkUseToolWithIntArg(void* api, const char* toolId, const char* itemId, int value)
{
    const HookCallGuard guard;

    if (Unboxer::OnUseToolWithIntArg(toolId, itemId, value))
        return;

    const auto original = UseToolWithIntArg.GetOriginal();
    if (original)
        original(api, toolId, itemId, value);
}

// Storage unit / terminal contents (main thread, client.dll+0xFDEB10: edx the operation, r8 the container id, r9 the
// item id). Lootline containers are answered locally; everything else goes to the game.
void __fastcall H::hkPerformItemCasketTransaction(void* api, int operation, const char* casketId, const char* itemId)
{
    const HookCallGuard guard;

    if (Unboxer::OnCasketTransaction(operation, casketId, itemId))
        return;

    const auto original = PerformItemCasketTransaction.GetOriginal();
    if (original)
        original(api, operation, casketId, itemId);
}

// A store purchase (main thread, client.dll+0x10B48E0: rdx "def(id),..."). The terminal's "accept" on a Lootline
// terminal is done locally; nothing with a Lootline id is ever sent to Steam.
void __fastcall H::hkStoreItemPurchase(void* api, const char* items)
{
    const HookCallGuard guard;

    if (Unboxer::OnStoreItemPurchase(items))
        return;

    const auto original = StoreItemPurchase.GetOriginal();
    if (original)
        original(api, items);
}

// The trade-up contract submitted (main thread). Valve doesn't know Lootline items (the contract just emptied and
// nothing came back): a contract of Lootline items is done locally; anything else goes to the game.
void __fastcall H::hkCraftIngredients(void* api)
{
    const HookCallGuard guard;

    if (Unboxer::OnCraftIngredients(api))
        return;

    if (const auto original = CraftIngredients.GetOriginal())
        original(api);
}

// The Armory's "redeem" (main thread): with Lootline's Armory on, done locally with Lootline's stars - never sent.
void __fastcall H::hkRedeemArmory(void* api, int season, int index, int points)
{
    const HookCallGuard guard;

    if (Unboxer::OnRedeemArmory(season, index, points))
        return;

    if (const auto original = RedeemArmory.GetOriginal())
        original(api, season, index, points);
}

// The StatTrak Swap popup's two items (main thread): remembered for a Lootline tool; the game keeps them too.
void __fastcall H::hkSetStatTrakSwapItems(void* api, const char* first, const char* second)
{
    const HookCallGuard guard;
    Unboxer::OnStatTrakSwapItems(first, second);

    if (const auto original = SetStatTrakSwapItems.GetOriginal())
        original(api, first, second);
}

// CS2's "Add to shuffle" / "Remove from shuffle" (favorites, main thread): a Lootline item's are Lootline's.
void __fastcall H::hkAddFavorite(void* api, const char* team, const char* itemId)
{
    const HookCallGuard guard;

    if (Unboxer::OnFavorite(team, itemId, true))
        return;

    if (const auto original = AddFavorite.GetOriginal())
        original(api, team, itemId);
}

void __fastcall H::hkRemoveFavorite(void* api, const char* team, const char* itemId)
{
    const HookCallGuard guard;

    if (Unboxer::OnFavorite(team, itemId, false))
        return;

    if (const auto original = RemoveFavorite.GetOriginal())
        original(api, team, itemId);
}

bool __fastcall H::hkIsFavorite(void* api, const char* team, const char* itemId)
{
    const HookCallGuard guard;

    if (const int ours = Unboxer::IsFavorite(team, itemId); ours >= 0)
        return ours == 1;

    const auto original = IsFavorite.GetOriginal();
    return original ? original(api, team, itemId) : false;
}

namespace
{
    using UtlStringSetFn = void(__fastcall*)(void* self, const char* text);
    UtlStringSetFn g_utlStringSet = nullptr;   // tier0's CUtlString::Set (the out string GetShuffleItems fills)
}

// A loadout slot's shuffle as JSON ('["id","id"]', main thread): Lootline's items for that slot added to the game's.
void __fastcall H::hkGetShuffleItems(void* api, void* out, const char* team, const char* slot)
{
    const HookCallGuard guard;
    const auto original = GetShuffleItems.GetOriginal();

    if (original)
        original(api, out, team, slot);

    if (!out || !g_utlStringSet)
        return;

    const std::string extra = Unboxer::ShuffleExtra(team, slot);

    if (extra.empty())
        return;

    // the game's list without its "]", then ours
    const char* current = *static_cast<const char* const*>(out);
    std::string list = current && *current ? current : "[]";
    const std::size_t close = list.rfind(']');

    if (close == std::string::npos)
        return;

    const bool hasItems = list.find('"') != std::string::npos && list.find('"') < close;
    list = list.substr(0, close) + (hasItems ? "," : "") + extra + "]";
    g_utlStringSet(out, list.c_str());
}

// A shared object cache field for the UI (main thread). Charm Detachments: Valve's + Lootline's, so CS2's charm popup lets
// you detach a charm with one of Lootline's.
std::uint64_t __fastcall H::hkCacheTypeField(void* api, const char* type, int index, const char* field)
{
    const HookCallGuard guard;
    const auto original = CacheTypeField.GetOriginal();
    const std::uint64_t value = original ? original(api, type, index, field) : 0;

    if (index == 0 && type && field && std::strcmp(type, "KeychainRemoveToolCharges") == 0 && std::strcmp(field, "charges") == 0)
        return (value & 0xFFFFFFFFull) + static_cast<std::uint64_t>(std::max(0, Unboxer::CharmDetachments()));

    return value;
}

// A trade-up recipe's "do these items fit" (main thread: the contract's list of eligible items, its "X / 5" count, the
// Ready button). CS2 keeps Souvenir reds out: their collection has no knives / gloves for Souvenir quality. In a check
// of only Lootline items they count as normal reds (hkTradeUpCollectionRare) - Lootline does that contract itself and
// gives a normal knife / gloves, like CS2 gives a normal item for Souvenir trade-ups. Real items: CS2's rules.
bool __fastcall H::hkTradeUpRecipeMatch(void* recipe, const void* items, bool partial)
{
    const HookCallGuard guard;
    const auto original = TradeUpRecipeMatch.GetOriginal();

    if (!original)
        return false;

    std::uint64_t ids[MAX_RECIPE_ITEMS];
    const int count = GameInv::CraftListIds(items, ids, MAX_RECIPE_ITEMS);
    bool ours = count > 0;

    for (int i = 0; ours && i < count; ++i)
        ours = Unboxer::IsLootlineId(ids[i]);

    if (!ours)
        return original(recipe, items, partial);

    DWORD nobody = 0;
    const bool marked = g_lootlineContractThread.compare_exchange_strong(nobody, GetCurrentThreadId());
    const bool fits = original(recipe, items, partial);

    if (marked)
        g_lootlineContractThread.store(0);

    return fits;
}

// A collection's "has knives / gloves for this quality" - asked for a red in a trade-up check (see above).
bool __fastcall H::hkTradeUpCollectionRare(void* collection, int quality)
{
    const HookCallGuard guard;
    const auto original = TradeUpCollectionRare.GetOriginal();

    if (!original)
        return false;

    if (quality == QUALITY_SOUVENIR && g_lootlineContractThread.load() == GetCurrentThreadId())
        quality = QUALITY_UNIQUE;

    return original(collection, quality);
}

// CCSPlayerBase_CameraServices::GetFOV: the FOV the view is drawn with (after the game's own blends / weapon
// adjustments). Replaced here for your own camera while the FOV changer is on and you're not scoped (Movement::OverrideFov).
float __fastcall H::hkGetFov(void* cameraServices)
{
    const HookCallGuard guard;
    const auto original = GetFov.GetOriginal();
    const float fov = original ? original(cameraServices) : 90.0f;
    return Movement::OverrideFov(cameraServices, fov);
}

namespace
{
    const std::uint8_t* g_petNative = nullptr;   // InventoryAPI.GetPetItemID (its call into the live pet lookup is at +9)
}

// client.dll's live pet (+0xFA9EC0, 2026-10-03): the first item in your inventory whose definition goes in the "pet"
// slot - so a real chicken always beats Lootline's. When InventoryAPI.GetPetItemID asks (the scripts: main menu chicken,
// loadout, pet book) an equipped Lootline pet is the answer; the game's own callers (its Game Coordinator hello among
// them) keep getting the real one.
std::uint64_t __fastcall H::hkLivePetItemId(void* unused)
{
    const HookCallGuard guard;
    const auto* from = static_cast<const std::uint8_t*>(_ReturnAddress());

    if (g_petNative && from > g_petNative && from < g_petNative + 0x20)
        if (const std::uint64_t ours = Unboxer::LivePetId())
            return ours;

    const auto original = LivePetItemId.GetOriginal();
    return original ? original(unused) : 0;
}

// C_CSWeaponBase "should draw crosshair" (the HUD asks it for the weapon in your hands). Snipers say no; with the Misc
// tab's crosshair on snipers, your own unscoped one says yes - CS2 then draws your crosshair with all your settings.
bool __fastcall H::hkShouldDrawCrosshair(void* weapon)
{
    const HookCallGuard guard;
    const auto original = ShouldDrawCrosshair.GetOriginal();
    const bool draw = original ? original(weapon) : false;
    return draw || Movement::ShowSniperCrosshair(weapon);
}

int __fastcall H::hkGetLootListItemsCount(void* api, const char* itemId)
{
    const HookCallGuard guard;

    if (BuildLootView(api, itemId))
        return static_cast<int>(g_lootIds.size());

    const auto original = GetLootListItemsCount.GetOriginal();
    return original ? original(api, itemId) : 0;
}

const char* __fastcall H::hkGetLootListItemIdByIndex(void* api, const char* itemId, int index)
{
    const HookCallGuard guard;

    if (BuildLootView(api, itemId))
        return index >= 0 && index < static_cast<int>(g_lootIds.size()) ? g_lootIds[static_cast<std::size_t>(index)].c_str() : "";

    const auto original = GetLootListItemIdByIndex.GetOriginal();
    return original ? original(api, itemId, index) : nullptr;
}

// panorama's frame, on the game's main thread — where the Game Coordinator's answers are handled and the only thread
// panorama accepts events from. Before the original, so the popup sees the result in this very frame.
void __fastcall H::hkUiRunFrame(void* engine)
{
    const HookCallGuard guard;   // unload waits until no game thread is inside a hook
    Unboxer::GameTick();
    LocalSpray::MainThreadTick();   // which of your Lootline graffiti a wall hides

    if (const auto original = UiRunFrame.Original<decltype(&hkUiRunFrame)>())
        original(engine);
}

namespace
{
    // IEngineClient (Source2EngineToClient) vfunc 42 = IsPlayingDemo: panorama's own IsPlayingDemo native (client
    // +0xE86820, 2026-10-14) is "jmp [engine vtable + 0x150]" on the same global the client calls in_game (39) /
    // connected (40) on.
    bool PlayingDemo()
    {
        return I::EngineClient && M::CallVFunc<bool, 42U>(I::EngineClient);
    }

    std::atomic<std::uint32_t> g_yourAccount{ 0 };   // your account id (the inventory's owner), once it's known

    std::uint32_t ControllerAccount(const void* controller)
    {
        const std::uint32_t offset = SchemaFinder::GetCached<hash_32_fnv1a_const("CBasePlayerController->m_steamID")>();
        return controller && offset ? static_cast<std::uint32_t>(*reinterpret_cast<const std::uint64_t*>(static_cast<const std::uint8_t*>(controller) + offset)) : 0;
    }

    // The pawn the skin changer dresses. Live (and in a demo recorded from your own view) the local pawn is yours. In a
    // demo of a match you played (GOTV, "Your matches") the local player is the camera and you're one of the players:
    // your pawn then, found by your account id (entity indices 1 - 64 are the player controllers) - your Lootline items
    // are on you in your own demos too.
    C_CSPlayerPawn* YourPawn(C_CSPlayerPawn* local)
    {
        if (!I::GameEntity || !I::GameEntity->Instance)
            return local;

        std::uint32_t mine = g_yourAccount.load(std::memory_order_relaxed);

        if (!mine)
        {
            mine = GameInv::AccountId();
            g_yourAccount.store(mine, std::memory_order_relaxed);
        }

        if (!mine || (local && ControllerAccount(I::GameEntity->Instance->Get<C_BaseEntity>(local->m_hController())) == mine))
            return local;

        const std::uint32_t pawnOffset = SchemaFinder::GetCached<hash_32_fnv1a_const("CCSPlayerController->m_hPlayerPawn")>();

        if (!pawnOffset)
            return local;

        for (int index = 1; index <= 64; ++index)
        {
            const auto* controller = reinterpret_cast<const std::uint8_t*>(I::GameEntity->Instance->Get<C_BaseEntity>(index));

            if (!controller || ControllerAccount(controller) != mine)
                continue;

            const CBaseHandle handle = *reinterpret_cast<const CBaseHandle*>(controller + pawnOffset);
            C_CSPlayerPawn* pawn = handle.valid() ? I::GameEntity->Instance->Get<C_CSPlayerPawn>(handle) : nullptr;
            return pawn ? pawn : local;
        }

        return local;
    }

    std::atomic_bool g_demoLogged{ false };   // "your player in the demo" logged once per demo

    C_CSPlayerPawn* PawnToDress(bool inGame, bool demo)
    {
        if (!inGame)
            return nullptr;

        C_CSPlayerPawn* local = I::GetLocalPawn();

        if (!demo)
        {
            g_demoLogged.store(false, std::memory_order_relaxed);
            return local;
        }

        C_CSPlayerPawn* yours = YourPawn(local);

        if (yours && yours != local && !g_demoLogged.exchange(true, std::memory_order_relaxed))
            Unboxer::LogLine("demo: your player found in it (by your account id) - your Lootline items go on you");

        return yours;
    }
}

void __fastcall H::hkFrameStageNotify(void* instance, int stage)
{
    const HookCallGuard guard;
    const auto original = FrameStageNotify.Original<decltype(&hkFrameStageNotify)>();

    if (!original)
        return;

    KnifeAnimation::FrameStage(stage);   // the knife inspect fix: which thread is the game's, which knife is yours

    // The skin changer: equipped Lootline items on your weapons in a match (Chadware's changer, same order: before
    // the game's own net-update work, charms after it). The loadout comes from what the menu published.
    Config::SyncGameSkins();

    if (stage == FRAME_NET_UPDATE_END)
    {
        const bool inGame = I::EngineClient && I::EngineClient->connected() && I::EngineClient->in_game();
        const bool demo = inGame && PlayingDemo();
        g_ctx->local_pawn = PawnToDress(inGame, demo);   // a demo: your own player in it

        SkinChanger::UpdateLiveInfo(g_ctx->local_pawn);
        CustomColor::UpdateYourItems(g_ctx->local_pawn);   // custom colours: only on your own items in a match

        if (g_ctx->local_pawn)
        {
            if (!demo)
                Unboxer::WatchSpawn(g_ctx->local_pawn);   // every spawn: all skins + gloves put on again (not a demo's)

            SkinChanger::Run();

            if (!demo)
                Lineup::UnstickYourWeapons(g_ctx->local_pawn);   // a skin stuck behind an identical request: made on its own
        }

        if (inGame)
            Lineup::FrameTick();   // your lineup character's gun skin + gloves, the skin changer's way
    }

    if (stage == FRAME_RENDER_START)
        Movement::RunFovBeforeRender();   // the FOV, after this frame's ticks and right before it's drawn

    if (stage == FRAME_RENDER_END)
    {
        Lineup::UnstickDeadWaits();       // a skin request stuck behind a stopped identical one: made on its own (menus too)
        AgentVoice::Update();             // agent voice lines: reads the game's voice rules (a little per frame)
        SkinChanger::RunAgentChanger();   // agents; it also brings gloves back after a model refresh
        AgentVoice::Drive();              // ... and the agent's voice once its model is on
        Movement::RunGameThread();        // bunnyhop, camera FOV, speed meter
    }

    original(instance, stage);

    if (stage == FRAME_NET_UPDATE_END)
    {
        SkinChanger::ProcessPendingKeychains();

        const bool inGame = I::EngineClient && I::EngineClient->connected() && I::EngineClient->in_game();
        g_ctx->local_pawn = PawnToDress(inGame, inGame && PlayingDemo());
    }

    if (stage == FRAME_RENDER_END)
        ModelPreview::ProcessMainThread();   // the 3D preview: its temp item + hidden panorama panel

    // Not the main thread: only a stand-in until the RunFrame hook is in (items still work from here, the popup's
    // result doesn't — panorama drops events from other threads).
    if (stage == FRAME_RENDER_END && !TryInstallUiRunFrame())
        Unboxer::GameTick();

    KnifeAnimation::FrameStage(stage);
}

// LoadoutAPI.GetItemID (client.dll+0xF9EDE0, 2026-10-07: rdx team, r8 slot, returns its static "%llu" text or null):
// the Lootline item Lootline has in that slot when CS2's answer is another one (a match locked the loadout).
const char* __fastcall H::hkLoadoutGetItemId(void* api, const char* team, const char* slot)
{
    const HookCallGuard guard;
    const auto original = LoadoutGetItemId.GetOriginal();
    const char* native = original ? original(api, team, slot) : nullptr;

    if (const char* ours = Unboxer::LoadoutShownItem(team, slot, native))
        return ours;

    return native;
}

// InventoryAPI.GetDecodeableRestriction (client.dll+0xF954C0, 2026-10-07: rdx item id text, returns "", "xray" (Germany,
// France) or "restricted" (Belgium, the Netherlands)). A Lootline case: "" - it opens like anywhere else.
const char* __fastcall H::hkDecodeableRestriction(void* api, const char* itemId)
{
    const HookCallGuard guard;

    if (Unboxer::IsLootlineItemId(itemId))
        return "";

    const auto original = DecodeableRestriction.GetOriginal();
    return original ? original(api, itemId) : "";
}

// InventoryAPI.IsEquipped (+0xFCB9C0: rdx item id text, r8 team): yes for a Lootline item Lootline has in a slot there.
bool __fastcall H::hkIsEquipped(void* api, const char* itemId, const char* team)
{
    const HookCallGuard guard;
    const auto original = IsEquipped.GetOriginal();
    const bool native = original ? original(api, itemId, team) : false;
    return native || Unboxer::LoadoutShowsEquipped(itemId, team);
}

// The knife inspect fix (KnifeAnimation, from the new Chadware source): before / after the game handles an inspect
// request and before / after the viewmodel graph reads the weapon's state.
char __fastcall H::hkKnifeInspect(C_CSWeaponBase* weapon)
{
    const HookCallGuard guard;
    const auto context = KnifeAnimation::BeforeInspect(weapon);
    const char result = KnifeInspect.GetOriginal()(weapon);
    KnifeAnimation::AfterInspect(weapon, context, result);
    return result;
}

std::int64_t __fastcall H::hkKnifeReflect(WeaponGraphController* controller, C_CSWeaponBase* weapon)
{
    const HookCallGuard guard;
    const auto context = KnifeAnimation::BeforeReflection(controller, weapon);
    const auto result = KnifeReflect.GetOriginal()(controller, weapon);
    KnifeAnimation::AfterReflection(controller, weapon, context);
    return result;
}

// The spray wheel's "spray" (main thread, client.dll+0xD03510 on 2026-10-08: ecx the slot 0-3; it sends the server a
// spray of the graffiti in that slot). A Lootline graffiti never goes out - it's sprayed on your screen only.
std::uint64_t __fastcall H::hkSprayAction(int slot)
{
    const HookCallGuard guard;

    if (LocalSpray::OnSprayAction(slot))
        return 0;

    const auto original = SprayAction.GetOriginal();
    return original ? original(slot) : 0;
}

// StoreAPI.GetStoreItemEmbeddedAttributePrice (main thread, client.dll+0x107D4B0 on 2026-10-08: rdx the item id AS A
// NUMBER - the script's text is converted before the call; reading it as text crashed the game - r8d quantity, r9 a
// format; returns its static text): the terminal laptop's "Accept Offer" price. A Lootline offer's comes in the currency
// picked in Settings -> Prices; anything else is the game's.
const char* __fastcall H::hkEmbeddedPrice(void* api, std::uint64_t itemId, int quantity, const char* format)
{
    const HookCallGuard guard;

    if (const char* ours = Unboxer::OfferPriceText(itemId))
        return ours;

    const auto original = EmbeddedPrice.GetOriginal();
    return original ? original(api, itemId, quantity, format) : "";
}

namespace
{
    // ── the game's events (FireEventClientSide): the kill feed's knife ──
    using EventGetNameFn = const char*(__fastcall*)(void* event);
    using EventGetStringFn = const char*(__fastcall*)(void* event, const void* token, const char* fallback);
    using EventSetStringFn = void*(__fastcall*)(void* event, const void* token, const char* value, int unused);
    using EventGetPlayerControllerFn = void*(__fastcall*)(void* event, const void* token);

    EventGetNameFn g_eventGetName = nullptr;
    EventGetStringFn g_eventGetString = nullptr;
    EventSetStringFn g_eventSetString = nullptr;
    EventGetPlayerControllerFn g_eventGetPlayerController = nullptr;

    // A game event key: MurmurHash2 of the lowercase name (seed 0x31415926), -1, the name.
    struct EventToken
    {
        std::uint32_t hash = 0;
        std::uint32_t minusOne = 0xFFFFFFFFu;
        const char* name = nullptr;
    };

    EventToken MakeToken(const char* name)
    {
        constexpr std::uint32_t m = 0x5BD1E995u;
        const int length = static_cast<int>(std::strlen(name));
        std::uint32_t h = 0x31415926u ^ static_cast<std::uint32_t>(length);
        int i = 0;

        const auto lower = [&](int at) { const unsigned char c = static_cast<unsigned char>(name[at]); return static_cast<std::uint32_t>(c >= 'A' && c <= 'Z' ? c + 32 : c); };

        for (; length - i >= 4; i += 4)
        {
            std::uint32_t k = lower(i) | (lower(i + 1) << 8) | (lower(i + 2) << 16) | (lower(i + 3) << 24);
            k *= m;
            k ^= k >> 24;
            k *= m;
            h *= m;
            h ^= k;
        }

        switch (length - i)
        {
        case 3: h ^= lower(i + 2) << 16; [[fallthrough]];
        case 2: h ^= lower(i + 1) << 8; [[fallthrough]];
        case 1: h ^= lower(i); h *= m;
        }

        h ^= h >> 13;
        h *= m;
        h ^= h >> 15;
        return { h, 0xFFFFFFFFu, name };
    }

    // Your controller (the pawn's m_hController), null if there's none.
    void* LocalController()
    {
        C_CSPlayerPawn* pawn = g_ctx ? g_ctx->local_pawn : nullptr;

        if (!pawn || !I::GameEntity || !I::GameEntity->Instance)
            return nullptr;

        return I::GameEntity->Instance->Get<C_BaseEntity>(pawn->m_hController());
    }

    // The kill feed's name for a knife: its class without "weapon_" ("knife_karambit", "bayonet"), "" if unknown.
    // Main thread: one buffer.
    char g_knifeEventName[64]{};

    const char* KnifeEventName(std::uint16_t definition)
    {
        // items_game's knife names without "weapon_" (the kill feed's icons: icons/equipment/knife_widowmaker.svg). The
        // item schema's weapon name is the shared class ("weapon_knife") - not usable.
        struct Knife
        {
            std::uint16_t definition;
            const char* name;
        };

        static constexpr Knife KNIVES[] = {
            { 500, "bayonet" }, { 503, "knife_css" }, { 505, "knife_flip" }, { 506, "knife_gut" }, { 507, "knife_karambit" },
            { 508, "knife_m9_bayonet" }, { 509, "knife_tactical" }, { 512, "knife_falchion" }, { 514, "knife_survival_bowie" },
            { 515, "knife_butterfly" }, { 516, "knife_push" }, { 517, "knife_cord" }, { 518, "knife_canis" }, { 519, "knife_ursus" },
            { 520, "knife_gypsy_jackknife" }, { 521, "knife_outdoor" }, { 522, "knife_stiletto" }, { 523, "knife_widowmaker" },
            { 525, "knife_skeleton" }, { 526, "knife_kukri" },
        };

        for (const Knife& knife : KNIVES)
            if (knife.definition == definition)
                return knife.name;

        // a newer knife: its model's file name ("weapons/models/knife/knife_x/weapon_knife_x.vmdl" -> "knife_x")
        const ItemDefinitionInfo* item = Skins::g_ItemSchema.GetItem(definition);

        if (!item || !item->isKnife)
            return nullptr;

        std::string_view model = item->schemaModelName.empty() ? std::string_view(item->modelName) : std::string_view(item->schemaModelName);

        if (const std::size_t slash = model.find_last_of('/'); slash != std::string_view::npos)
            model.remove_prefix(slash + 1);

        if (const std::size_t dot = model.find('.'); dot != std::string_view::npos)
            model = model.substr(0, dot);

        if (model.starts_with("weapon_"))
            model.remove_prefix(7);

        if (model.empty() || model == "knife" || model == "knife_t")
            return nullptr;

        snprintf(g_knifeEventName, sizeof(g_knifeEventName), "%.*s", static_cast<int>(model.size()), model.data());
        return g_knifeEventName;
    }

    // The weapon name the kill feed itself works with: without "weapon_" / "item_" (it strips them too, then turns
    // "knife_default_ct" / "knife_default_t" into "knife" / "knife_t").
    std::string_view KillFeedName(const char* weapon)
    {
        std::string_view name(weapon ? weapon : "");

        for (const std::string_view prefix : { std::string_view("weapon_"), std::string_view("item_") })
            if (name.starts_with(prefix))
                name.remove_prefix(prefix.size());

        return name;
    }

    // A knife, as the server names it: your default one ("knife_default_ct", "knife_t"...) or a real one you own
    // ("knife_karambit", "bayonet").
    bool IsKnifeName(std::string_view name)
    {
        return name.starts_with("knife") || name == "bayonet";
    }

    int g_knifeKillsLogged = 0;   // main thread: the first few knife kills go to the log (what the server sent)

    // player_death of a kill of yours with your knife: the server only knows your real knife (the default one, or one you
    // own) - it says your Lootline knife instead
    void FixKnifeKill(void* event)
    {
        const char* eventName = g_eventGetName(event);

        if (!eventName || std::strcmp(eventName, "player_death") != 0)
            return;

        const EventToken weaponKey = MakeToken("weapon");
        const char* weapon = g_eventGetString(event, &weaponKey, "");

        if (!weapon || !IsKnifeName(KillFeedName(weapon)))
            return;   // not a knife kill

        const EventToken attackerKey = MakeToken("attacker");
        void* local = LocalController();
        const bool yours = local && g_eventGetPlayerController(event, &attackerKey) == local;
        const std::uint16_t knife = SkinChanger::LiveKnifeDefinition();
        const bool lootlineKnife = knife != 0 && knife != 42 && knife != 59;   // 42 / 59: the default CT / T knife
        const char* name = yours && lootlineKnife ? KnifeEventName(knife) : nullptr;

        // the first few knife kills in the log: what the server said, and what it says now
        if (g_knifeKillsLogged < 8)
        {
            ++g_knifeKillsLogged;
            char line[220];
            snprintf(line, sizeof(line), "kill feed: a knife kill said \"%s\" (yours: %s, Lootline knife %u) -> %s", weapon, yours ? "yes" : "no",
                static_cast<unsigned>(knife), name ? name : "left as it is");
            Unboxer::LogLine(line);
        }

        if (name && KillFeedName(weapon) != name)
            g_eventSetString(event, &weaponKey, name, 0);
    }

    bool CallFixKnifeKill(void* event)
    {
        __try
        {
            FixKnifeKill(event);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    bool g_knifeKillCrashLogged = false;
}

// CGameEventManager::FireEventClientSide (client.dll+0x9E2BA0 on 2026-10-08: rcx the manager, rdx the event): every
// game event on its way to the HUD. Your knife kills say your Lootline knife, so the kill feed shows its icon.
bool __fastcall H::hkFireEventClientSide(void* manager, void* event)
{
    const HookCallGuard guard;

    if (event && g_eventGetName && !CallFixKnifeKill(event) && !g_knifeKillCrashLogged)
    {
        g_knifeKillCrashLogged = true;
        Unboxer::LogLine("kill feed: reading a kill event crashed (signature?) - the knife icon stays the game's");
    }

    const auto original = FireEventClientSide.GetOriginal();
    return original ? original(manager, event) : false;
}

// The lineup character builder (client.dll+0x799640 on 2026-10-08, rcx the position): the team intro / team select /
// end of match. Your position gets your Lootline agent, gloves and weapon skin before the game reads it.
void* __fastcall H::hkLineupBuild(void* position)
{
    const HookCallGuard guard;
    Lineup::BeforeBuild(position);

    const auto original = LineupBuild.GetOriginal();
    void* result = original ? original(position) : nullptr;

    Lineup::AfterBuild(position);   // your character and its gun queued for Lineup::FrameTick
    return result;
}

// Agent voice lines (AgentVoice, from the new Chadware source).
void* __fastcall H::hkAgentVoiceStart(void* system, void* out, std::uint32_t opaque, std::uint32_t guid,
    std::uint32_t eventHash, int source, std::uint16_t seed, const void* packedParams, std::uint32_t packedSize, const void* timing)
{
    const HookCallGuard guard;
    const auto replacement = AgentVoice::SoundEvent(eventHash, source, seed);
    return AgentVoiceStart.GetOriginal()(system, out, opaque, guid, replacement, source, seed, packedParams, packedSize, timing);
}

void __fastcall H::hkAgentRadioText(void* hud, const RadioTextMessage* message)
{
    const HookCallGuard guard;
    AgentVoice::ObserveRadio(message);
    AgentRadioText.GetOriginal()(hud, message);
}

std::uintptr_t __fastcall H::hkAgentLevelInit(void* manager)
{
    const HookCallGuard guard;
    AgentVoice::LevelChange change;
    Unboxer::OnLevelChange();   // a new match: the loadout shuffle picks again
    return AgentLevelInit.GetOriginal()(manager);
}

std::uintptr_t __fastcall H::hkAgentLevelShutdown(void* manager)
{
    const HookCallGuard guard;
    LocalSpray::LevelShutdown();   // Lootline graffiti decals come down while the world still exists
    AgentVoice::LevelChange change;
    return AgentLevelShutdown.GetOriginal()(manager);
}

bool __fastcall H::hkMouseInputEnabled(void* instance)
{
    const HookCallGuard guard;
    const auto original = MouseInputEnabled.Original<decltype(&hkMouseInputEnabled)>();
    return chadware.renderer.menu.showMenu ? false : original(instance);
}

void* __fastcall H::hkIsRelativeMouseMode(void* instance, bool active)
{
    const HookCallGuard guard;
    const auto original = IsRelativeMouseMode.Original<decltype(&hkIsRelativeMouseMode)>();

    if (chadware.renderer.menu.showMenu)
        return original(instance, false);

    // The main menu (not connected): the cursor stays. NOT "not in game yet": loading into a map, the game asks for
    // mouse look a moment before it counts as in game - turning that down left the cursor on screen and the turning
    // stuck at the screen's edges after spawning, until the menu was opened and closed (2026-10-18).
    if (I::EngineClient && !I::EngineClient->connected() && !I::EngineClient->in_game())
        return original(instance, false);

    return original(instance, active);
}

// The Unlock button (and every other "use this item on that item" in the inventory). Ours = opened here, never
// sent to Valve; anything else goes to the game as usual.
void __fastcall H::hkUseTool(void* api, const char* toolId, const char* itemId)
{
    const HookCallGuard guard;

    if (Unboxer::OnUseTool(toolId, itemId))
        return;

    if (const auto original = UseTool.GetOriginal())
        original(api, toolId, itemId);
}

void H::Hooks::init()
{
    // before any hook can call them
    AgentVoice::Initialize();
    KnifeAnimation::Initialize();

    TryInstallUiRunFrame();

    if (void** vtable = VTableOf(I::Source2Client))
    {
        const int slot = VmtSlotHook::FindSlot(vtable, FindFrameStageNotify(), 256);

        if (slot >= 0)
            FrameStageNotify.Install(vtable, slot, reinterpret_cast<void*>(&hkFrameStageNotify));
    }

    if (I::Input)   // null when its signature broke: skip instead of crashing
        MouseInputEnabled.Install(VTableOf(I::Input), VFUNC_MOUSE_INPUT_ENABLED, reinterpret_cast<void*>(&hkMouseInputEnabled));

    if (I::InputSystem)
        IsRelativeMouseMode.Install(VTableOf(I::InputSystem), VFUNC_RELATIVE_MOUSE_MODE, reinterpret_cast<void*>(&hkIsRelativeMouseMode));

    UseTool.Add(GameInv::UseToolFunction(), reinterpret_cast<void*>(&hkUseTool));

    // both or neither: half a list would be worse than none
    void* lootCount = GameInv::InventoryApiFunction("GetLootListItemsCount");
    void* lootIdAt = GameInv::InventoryApiFunction("GetLootListItemIdByIndex");
    bool contents = false;

    if (lootCount && lootIdAt && GetLootListItemsCount.Add(lootCount, reinterpret_cast<void*>(&hkGetLootListItemsCount)))
    {
        contents = GetLootListItemIdByIndex.Add(lootIdAt, reinterpret_cast<void*>(&hkGetLootListItemIdByIndex));

        if (!contents)
            GetLootListItemsCount.Remove();
    }

    Unboxer::LogLine(contents ? "case contents hook: ok (show-in-case works)" : "case contents hook: NOT installed (show-in-case off)");

    // what charms hang on your guns (first seen / after ours goes on / 3 s later / put back) - for the double-charm check
    SkinChanger::SetCharmLog(&Unboxer::LogLine);

    // a server in your own game (hosting / offline) hands your guns your Lootline items themselves: leave those to the game
    SkinChanger::SetOwnItemCheck(&Unboxer::IsLootlineId);

    const bool loadout = LoadoutEquip.Add(GameInv::LoadoutEquipFunction(), reinterpret_cast<void*>(&hkLoadoutEquip));
    Unboxer::LogLine(loadout ? "loadout hook: ok (equipping in CS2 works)" : "loadout hook: NOT installed (equip from the Lootline menu)");

    // stickers on Lootline guns: applying goes through the UseTool hook, scraping / taking off through this one
    void* wearSticker = GameInv::InventoryApiFunction("WearItemSticker");
    const bool scrape = wearSticker && WearItemSticker.Add(wearSticker, reinterpret_cast<void*>(&hkWearItemSticker));
    Unboxer::LogLine(!GameInv::StickersResolved() ? "stickers: placement / item lookup NOT found (stickers on Lootline guns go to the first free slot)" :
        scrape ? "stickers: ok (apply, scrape and take off on Lootline guns)" : "stickers: apply ok, scrape hook NOT installed");

    // charms / name tags on Lootline items: applying goes through the UseTool hook, these two do the rest
    void* removeKeychain = GameInv::InventoryApiFunction("RemoveKeychain");
    const bool charms = removeKeychain && RemoveKeychain.Add(removeKeychain, reinterpret_cast<void*>(&hkRemoveKeychain));
    void* nameTool = GameInv::InventoryApiFunction("SetNameToolString");
    const bool names = nameTool && SetNameToolString.Add(nameTool, reinterpret_cast<void*>(&hkSetNameToolString));
    Unboxer::LogLine(charms ? "charms: ok (on and off Lootline guns)" : "charms: hang on ok, take-off hook NOT installed");
    Unboxer::LogLine(names ? "name tags: ok (Lootline items, pets too)" : "name tags: hook NOT installed");

    // CS2's Delete / the name tag's Remove on Lootline items (Valve doesn't know them)
    void* deleteItem = GameInv::InventoryApiFunction("DeleteItem");
    const bool deletes = deleteItem && DeleteItem.Add(deleteItem, reinterpret_cast<void*>(&hkDeleteItem));
    void* clearName = GameInv::InventoryApiFunction("ClearCustomName");
    const bool unnames = clearName && ClearCustomName.Add(clearName, reinterpret_cast<void*>(&hkClearCustomName));
    Unboxer::LogLine(deletes ? "delete: ok (CS2's Delete removes Lootline items)" : "delete: hook NOT installed (remove Lootline items from the Lootline menu)");

    // added items arrive NEW (CS2's new item popup); acknowledging them is local - without the hook they'd stay new
    void* acknowledge = GameInv::InventoryApiFunction("AcknowledgeNewItembyItemID");
    const bool acknowledges = acknowledge && AcknowledgeNewItem.Add(acknowledge, reinterpret_cast<void*>(&hkAcknowledgeNewItem));
    Unboxer::SetNewItemsEnabled(acknowledges);
    Unboxer::LogLine(acknowledges ? "new items: ok (added items show CS2's new item popup, acknowledged locally)" :
        "new items: acknowledge hook NOT installed (added items arrive already seen)");

    // the scoreboard's pin next to your name: your equipped Lootline pin - both or neither
    bool flair = false;
    void* flairId = GameInv::InventoryApiFunction("GetFlairItemId");
    void* flairImage = GameInv::InventoryApiFunction("GetFlairItemImage");

    if (flairId && flairImage && GetFlairItemId.Add(flairId, reinterpret_cast<void*>(&hkGetFlairItemId)))
    {
        flair = GetFlairItemImage.Add(flairImage, reinterpret_cast<void*>(&hkGetFlairItemImage));

        if (!flair)
            GetFlairItemId.Remove();
    }
    Unboxer::LogLine(flair ? "scoreboard pin: ok (your equipped Lootline pin next to your name)" : "scoreboard pin: hook NOT installed");
    Unboxer::LogLine(unnames ? "name tag remove: ok (Lootline items)" : "name tag remove: hook NOT installed");

    // CS2's Uplink Terminals on Lootline items: offers, reopening, "accept" (which must never reach Steam)
    void* intArg = GameInv::InventoryApiFunction("UseToolWithIntArg");
    const bool offers = intArg && UseToolWithIntArg.Add(intArg, reinterpret_cast<void*>(&hkUseToolWithIntArg));
    void* casket = GameInv::InventoryApiFunction("PerformItemCasketTransaction");
    const bool reopen = casket && PerformItemCasketTransaction.Add(casket, reinterpret_cast<void*>(&hkPerformItemCasketTransaction));
    void* purchase = GameInv::StoreApiFunction("StoreItemPurchase");
    const bool accept = purchase && StoreItemPurchase.Add(purchase, reinterpret_cast<void*>(&hkStoreItemPurchase));
    char terminals[160];
    snprintf(terminals, sizeof(terminals), "terminals: offers %s, reopen %s, accept %s", offers ? "ok" : "NOT installed",
        reopen ? "ok" : "NOT installed", accept ? "ok" : "NOT installed");
    Unboxer::LogLine(terminals);

    void* craft = GameInv::CraftIngredientsFunction();
    const bool tradeUp = craft && CraftIngredients.Add(craft, reinterpret_cast<void*>(&hkCraftIngredients));
    Unboxer::LogLine(tradeUp ? "trade-up hook: ok (contracts of Lootline items work)" : "trade-up hook: NOT installed");

    // the Armory with Lootline's stars: its redeem
    void* redeem = GameInv::StoreApiFunction("ActionRedeemOperationGoods");
    const bool armory = redeem && RedeemArmory.Add(redeem, reinterpret_cast<void*>(&hkRedeemArmory));
    Unboxer::LogLine(armory ? "armory: ok (redeems with Lootline's stars stay local)" : "armory: redeem hook NOT installed - Lootline's Armory is off");

    // Lootline's StatTrak Swap Tool and Charm Detachments
    void* swapItems = GameInv::InventoryApiFunction("SetStatTrakSwapToolItems");
    const bool swaps = swapItems && SetStatTrakSwapItems.Add(swapItems, reinterpret_cast<void*>(&hkSetStatTrakSwapItems));
    void* cacheField = GameInv::InventoryApiFunction("GetCacheTypeElementFieldByIndex");
    const bool detachments = cacheField && CacheTypeField.Add(cacheField, reinterpret_cast<void*>(&hkCacheTypeField));
    char tools[160];
    snprintf(tools, sizeof(tools), "tools: StatTrak swap %s, charm detachments %s", swaps ? "ok" : "NOT installed", detachments ? "ok" : "NOT installed");
    Unboxer::LogLine(tools);

    // the loadout shuffle: all four or none
    if (const HMODULE tier0 = GetModuleHandleA("tier0.dll"))
        g_utlStringSet = reinterpret_cast<UtlStringSetFn>(GetProcAddress(tier0, "?Set@CUtlString@@QEAAXPEBD@Z"));

    void* addFavorite = GameInv::InventoryApiFunction("AddItemToFavorites");
    void* removeFavorite = GameInv::InventoryApiFunction("RemoveItemFromFavorites");
    void* isFavorite = GameInv::InventoryApiFunction("ItemIsInFavorites");
    void* shuffleItems = GameInv::InventoryApiFunction("GetShuffleItems");
    bool shuffle = g_utlStringSet && addFavorite && removeFavorite && isFavorite && shuffleItems &&
        AddFavorite.Add(addFavorite, reinterpret_cast<void*>(&hkAddFavorite));
    shuffle = shuffle && RemoveFavorite.Add(removeFavorite, reinterpret_cast<void*>(&hkRemoveFavorite));
    shuffle = shuffle && IsFavorite.Add(isFavorite, reinterpret_cast<void*>(&hkIsFavorite));
    shuffle = shuffle && GetShuffleItems.Add(shuffleItems, reinterpret_cast<void*>(&hkGetShuffleItems));

    if (!shuffle)
    {
        AddFavorite.Remove();
        RemoveFavorite.Remove();
        IsFavorite.Remove();
        GetShuffleItems.Remove();
    }

    Unboxer::LogLine(shuffle ? "shuffle: ok (Lootline items in CS2's loadout shuffle)" : "shuffle: NOT installed - Lootline items can't be shuffled");

    // Lootline Souvenir reds in contracts: both or neither
    bool souvenirReds = false;
    if (tradeUp && TradeUpCollectionRare.Add(M::scan("client.dll", Patterns::TradeUpCollectionRare), reinterpret_cast<void*>(&hkTradeUpCollectionRare)))
    {
        souvenirReds = TradeUpRecipeMatch.Add(M::scan("client.dll", Patterns::TradeUpRecipeMatch), reinterpret_cast<void*>(&hkTradeUpRecipeMatch));

        if (!souvenirReds)
            TradeUpCollectionRare.Remove();
    }
    Unboxer::LogLine(souvenirReds ? "trade-up: Souvenir reds ok (Lootline ones go in a contract of Lootline items)" :
        "trade-up: Souvenir reds NOT installed (signature) - CS2 keeps Souvenir reds out of contracts");

    // Misc tab. Without the hook (signature broken after an update) the FOV changer writes m_iFOV instead.
    Movement::SetFovHookActive(GetFov.Add(M::scan("client.dll", Patterns::GetFov), reinterpret_cast<void*>(&hkGetFov)));
    Movement::SetSniperCrosshairHookActive(ShouldDrawCrosshair.Add(M::scan("client.dll", Patterns::ShouldDrawCrosshair),
        reinterpret_cast<void*>(&hkShouldDrawCrosshair)));

    // the live pet: GetPetItemID = push rbx / sub rsp, 20h / mov rbx, rdx / call <the lookup> (checked, then hooked)
    bool pets = false;
    if (const auto* native = static_cast<const std::uint8_t*>(GameInv::InventoryApiFunction("GetPetItemID")))
    {
        static constexpr std::uint8_t prologue[] = { 0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0xDA, 0xE8 };

        if (std::memcmp(native, prologue, sizeof(prologue)) == 0)
        {
            std::int32_t displacement = 0;
            std::memcpy(&displacement, native + 10, sizeof(displacement));
            g_petNative = native;
            pets = LivePetItemId.Add(const_cast<std::uint8_t*>(native + 14 + displacement), reinterpret_cast<void*>(&hkLivePetItemId));
        }
    }
    Unboxer::LogLine(pets ? "pets: ok (an equipped Lootline pet is your live pet)" : "pets: live pet hook NOT installed (CS2 shows its own pet)");

    // the loadout screen in a locked match: what Lootline put in a slot
    void* getItemId = GameInv::InventoryApiFunction("GetItemID");
    const bool shownLoadout = getItemId && LoadoutGetItemId.Add(getItemId, reinterpret_cast<void*>(&hkLoadoutGetItemId));
    void* isEquipped = GameInv::InventoryApiFunction("IsEquipped");
    const bool shownEquipped = isEquipped && IsEquipped.Add(isEquipped, reinterpret_cast<void*>(&hkIsEquipped));
    Unboxer::LogLine(shownLoadout ? (shownEquipped ? "loadout screen: ok (shows Lootline items forced in a locked match)" :
        "loadout screen: ok, the inventory's equipped marks NOT") : "loadout screen: GetItemID hook NOT installed (a locked match shows the old item)");

    // X-Ray / loot box laws (Germany, France / Belgium, the Netherlands): Lootline cases open anyway
    void* restriction = GameInv::InventoryApiFunction("GetDecodeableRestriction");
    const bool xray = restriction && DecodeableRestriction.Add(restriction, reinterpret_cast<void*>(&hkDecodeableRestriction));
    Unboxer::LogLine(xray ? "region: ok (Lootline cases open in X-Ray / loot box law countries too)" : "region: hook NOT installed (X-Ray countries get CS2's scanner)");

    // the knife inspect fix: both or neither
    bool knifeFix = false;
    if (KnifeInspect.Add(M::scan("client.dll", Patterns::KnifeInspect), reinterpret_cast<void*>(&hkKnifeInspect)))
    {
        knifeFix = KnifeReflect.Add(M::scan("client.dll", Patterns::KnifeReflect), reinterpret_cast<void*>(&hkKnifeReflect));

        if (!knifeFix)
            KnifeInspect.Remove();
    }
    Unboxer::LogLine(knifeFix ? "knife inspect fix: ok" : "knife inspect fix: NOT installed (signature)");

    // agent voice lines: the level hooks first (they reset it), then the sound / radio ones
    const bool voiceLevels = AgentLevelInit.Add(M::scan("client.dll", Patterns::AgentRulesLevelInit), reinterpret_cast<void*>(&hkAgentLevelInit)) &&
        AgentLevelShutdown.Add(M::scan("client.dll", Patterns::AgentRulesLevelShutdown), reinterpret_cast<void*>(&hkAgentLevelShutdown));
    const bool voiceSound = voiceLevels &&
        AgentVoiceStart.Add(M::scan("client.dll", Patterns::AgentVoiceStart), reinterpret_cast<void*>(&hkAgentVoiceStart));
    const bool voiceRadio = voiceSound &&
        AgentRadioText.Add(M::scan("client.dll", Patterns::AgentRadioText), reinterpret_cast<void*>(&hkAgentRadioText));
    char voices[160];
    snprintf(voices, sizeof(voices), "agent voice lines: %s%s", voiceSound ? "ok" : "NOT installed",
        voiceSound ? (voiceRadio ? " (radio commands too)" : " (radio commands NOT)") : "");
    Unboxer::LogLine(voices);

    // graffiti only you see: a Lootline graffiti is sprayed on your screen, the game's spray (to the server) never runs
    const bool sprays = LocalSpray::Initialize() &&
        SprayAction.Add(M::scan("client.dll", Patterns::SprayAction), reinterpret_cast<void*>(&hkSprayAction));
    Unboxer::LogLine(sprays ? "graffiti: ok (a Lootline graffiti sprays on your screen only)" :
        "graffiti: NOT installed (signature) - don't spray a Lootline graffiti");

    // the kill feed: your knife kills show your Lootline knife
    g_eventGetName = reinterpret_cast<EventGetNameFn>(M::FindPattern("client.dll", Patterns::EventGetName));
    g_eventGetString = reinterpret_cast<EventGetStringFn>(M::FindPattern("client.dll", Patterns::EventGetString));
    g_eventSetString = reinterpret_cast<EventSetStringFn>(M::FindPattern("client.dll", Patterns::EventSetString));
    g_eventGetPlayerController = reinterpret_cast<EventGetPlayerControllerFn>(M::FindPattern("client.dll", Patterns::EventGetPlayerController));
    const bool killFeed = g_eventGetName && g_eventGetString && g_eventSetString && g_eventGetPlayerController &&
        FireEventClientSide.Add(M::scan("client.dll", Patterns::FireEventClientSide), reinterpret_cast<void*>(&hkFireEventClientSide));
    if (!killFeed)
        g_eventGetName = nullptr;
    Unboxer::LogLine(killFeed ? "kill feed: ok (your knife kills show your Lootline knife)" : "kill feed: NOT installed (signature) - knife kills show the default knife");

    // the start / end lineups: your character wears your Lootline agent, gloves and weapon skin
    const bool lineup = LineupBuild.Add(M::scan("client.dll", Patterns::LineupBuild), reinterpret_cast<void*>(&hkLineupBuild));
    Unboxer::LogLine(lineup ? "lineups: ok (your Lootline agent, gloves and skins in the start / end lineups)" :
        "lineups: NOT installed (signature) - lineups show your real items");

    // the terminal laptop's "Accept Offer" price: Lootline offers in your currency
    void* embeddedPrice = GameInv::StoreApiFunction("GetStoreItemEmbeddedAttributePrice");
    const bool offerPrices = embeddedPrice && EmbeddedPrice.Add(embeddedPrice, reinterpret_cast<void*>(&hkEmbeddedPrice));
    Unboxer::LogLine(offerPrices ? "terminal prices: ok (your currency)" : "terminal prices: hook NOT installed (CS2 shows its own)");

    // custom skin colours (Inventory -> an item -> Custom colour): logs its own line
    CustomColor::Install();
}

void H::Hooks::shutdown()
{
    {
        std::lock_guard lock(g_uiHookMutex);
        g_uiHookClosed = true;
        UiRunFrame.Restore();
    }

    FrameStageNotify.Restore();
    MouseInputEnabled.Restore();
    IsRelativeMouseMode.Restore();

    if (UseTool.IsHooked())
        UseTool.Restore();

    if (GetLootListItemsCount.IsHooked())
        GetLootListItemsCount.Restore();

    if (GetLootListItemIdByIndex.IsHooked())
        GetLootListItemIdByIndex.Restore();

    if (LoadoutEquip.IsHooked())
        LoadoutEquip.Restore();

    if (WearItemSticker.IsHooked())
        WearItemSticker.Restore();

    if (RemoveKeychain.IsHooked())
        RemoveKeychain.Restore();

    if (SetNameToolString.IsHooked())
        SetNameToolString.Restore();

    if (DeleteItem.IsHooked())
        DeleteItem.Restore();

    if (ClearCustomName.IsHooked())
        ClearCustomName.Restore();

    if (UseToolWithIntArg.IsHooked())
        UseToolWithIntArg.Restore();

    if (PerformItemCasketTransaction.IsHooked())
        PerformItemCasketTransaction.Restore();

    if (StoreItemPurchase.IsHooked())
        StoreItemPurchase.Restore();

    if (CraftIngredients.IsHooked())
        CraftIngredients.Restore();

    if (RedeemArmory.IsHooked())
        RedeemArmory.Restore();

    if (SetStatTrakSwapItems.IsHooked())
        SetStatTrakSwapItems.Restore();

    if (CacheTypeField.IsHooked())
        CacheTypeField.Restore();

    if (AddFavorite.IsHooked())
        AddFavorite.Restore();

    if (RemoveFavorite.IsHooked())
        RemoveFavorite.Restore();

    if (IsFavorite.IsHooked())
        IsFavorite.Restore();

    if (GetShuffleItems.IsHooked())
        GetShuffleItems.Restore();

    if (TradeUpRecipeMatch.IsHooked())
        TradeUpRecipeMatch.Restore();

    if (TradeUpCollectionRare.IsHooked())
        TradeUpCollectionRare.Restore();

    if (GetFov.IsHooked())
        GetFov.Restore();

    if (ShouldDrawCrosshair.IsHooked())
        ShouldDrawCrosshair.Restore();

    if (LivePetItemId.IsHooked())
        LivePetItemId.Restore();

    if (LoadoutGetItemId.IsHooked())
        LoadoutGetItemId.Restore();

    if (DecodeableRestriction.IsHooked())
        DecodeableRestriction.Restore();

    if (IsEquipped.IsHooked())
        IsEquipped.Restore();

    if (KnifeInspect.IsHooked())
        KnifeInspect.Restore();

    if (KnifeReflect.IsHooked())
        KnifeReflect.Restore();

    if (AgentVoiceStart.IsHooked())
        AgentVoiceStart.Restore();

    if (AgentRadioText.IsHooked())
        AgentRadioText.Restore();

    if (AgentLevelInit.IsHooked())
        AgentLevelInit.Restore();

    if (AgentLevelShutdown.IsHooked())
        AgentLevelShutdown.Restore();

    if (SprayAction.IsHooked())
        SprayAction.Restore();

    if (EmbeddedPrice.IsHooked())
        EmbeddedPrice.Restore();

    if (FireEventClientSide.IsHooked())
        FireEventClientSide.Restore();

    if (LineupBuild.IsHooked())
        LineupBuild.Restore();

    CustomColor::Uninstall();
    Movement::SetFovHookActive(false);
}
