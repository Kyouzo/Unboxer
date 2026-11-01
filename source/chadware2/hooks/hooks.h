#pragma once

#include <cstdint>

#include "includeHooks.h"
#include "vmt_hook.h"

class C_CSWeaponBase;
class WeaponGraphController;
struct RadioTextMessage;

namespace H
{
    void __fastcall hkUiRunFrame(void* engine);
    void __fastcall hkFrameStageNotify(void* instance, int stage);
    bool __fastcall hkMouseInputEnabled(void* instance);
    void* __fastcall hkIsRelativeMouseMode(void* instance, bool active);
    void __fastcall hkUseTool(void* api, const char* toolId, const char* itemId);
    bool __fastcall hkLoadoutEquip(void* api, const char* team, const char* itemId, const char* slot);
    bool __fastcall hkWearItemSticker(void* api, const char* itemId, int slot, int wear);
    bool __fastcall hkRemoveKeychain(void* api, const char* itemId, int slot);
    bool __fastcall hkSetNameToolString(void* api, const char* text, const char* itemId);
    void __fastcall hkDeleteItem(void* api, const char* itemId);
    bool __fastcall hkAcknowledgeNewItem(void* api, const char* itemId);
    const char* __fastcall hkGetFlairItemId(void* api, const char* xuid);
    const char* __fastcall hkGetFlairItemImage(void* api, const char* xuid);
    void __fastcall hkClearCustomName(void* api, const char* itemId);
    void __fastcall hkUseToolWithIntArg(void* api, const char* toolId, const char* itemId, int value);
    void __fastcall hkPerformItemCasketTransaction(void* api, int operation, const char* casketId, const char* itemId);
    void __fastcall hkStoreItemPurchase(void* api, const char* items);
    void __fastcall hkCraftIngredients(void* api);
    void __fastcall hkRedeemArmory(void* api, int season, int index, int points);
    void __fastcall hkSetStatTrakSwapItems(void* api, const char* first, const char* second);
    void __fastcall hkAddFavorite(void* api, const char* team, const char* itemId);
    void __fastcall hkRemoveFavorite(void* api, const char* team, const char* itemId);
    bool __fastcall hkIsFavorite(void* api, const char* team, const char* itemId);
    void __fastcall hkGetShuffleItems(void* api, void* out, const char* team, const char* slot);
    std::uint64_t __fastcall hkCacheTypeField(void* api, const char* type, int index, const char* field);
    bool __fastcall hkTradeUpRecipeMatch(void* recipe, const void* items, bool partial);
    bool __fastcall hkTradeUpCollectionRare(void* collection, int quality);
    float __fastcall hkGetFov(void* cameraServices);
    bool __fastcall hkShouldDrawCrosshair(void* weapon);
    std::uint64_t __fastcall hkLivePetItemId(void* unused);
    int __fastcall hkGetLootListItemsCount(void* api, const char* itemId);
    const char* __fastcall hkGetLootListItemIdByIndex(void* api, const char* itemId, int index);
    char __fastcall hkKnifeInspect(C_CSWeaponBase* weapon);
    const char* __fastcall hkLoadoutGetItemId(void* api, const char* team, const char* slot);
    const char* __fastcall hkDecodeableRestriction(void* api, const char* itemId);
    bool __fastcall hkIsEquipped(void* api, const char* itemId, const char* team);
    std::int64_t __fastcall hkKnifeReflect(WeaponGraphController* controller, C_CSWeaponBase* weapon);
    void* __fastcall hkAgentVoiceStart(void* system, void* out, std::uint32_t opaque, std::uint32_t guid,
        std::uint32_t eventHash, int source, std::uint16_t seed, const void* packedParams, std::uint32_t packedSize, const void* timing);
    void __fastcall hkAgentRadioText(void* hud, const RadioTextMessage* message);
    std::uintptr_t __fastcall hkAgentLevelInit(void* manager);
    std::uintptr_t __fastcall hkAgentLevelShutdown(void* manager);
    std::uint64_t __fastcall hkSprayAction(int slot);
    const char* __fastcall hkEmbeddedPrice(void* api, std::uint64_t itemId, int quantity, const char* format);
    bool __fastcall hkFireEventClientSide(void* manager, void* event);
    void* __fastcall hkLineupBuild(void* position);

    // vtable slots (Chadware hooks the same functions in their code — both work side by side)
    inline VmtSlotHook UiRunFrame{};          // panorama CUIEngine::RunFrame: main thread, the Unboxer tick
    inline VmtSlotHook FrameStageNotify{};    // only ticks Unboxer while UiRunFrame couldn't be installed
    inline VmtSlotHook MouseInputEnabled{};
    inline VmtSlotHook IsRelativeMouseMode{};

    // InventoryAPI.UseTool (only Unboxer hooks this one)
    inline CInlineHookObj<decltype(&hkUseTool)> UseTool{};
    // InventoryAPI.GetLootListItemsCount / GetLootListItemIdByIndex: a case's contents list (the popup's tiles, the
    // reel's pool, the rental bar's count, the inspect carousel all come from these two)
    inline CInlineHookObj<decltype(&hkGetLootListItemsCount)> GetLootListItemsCount{};
    // LoadoutAPI.EquipItemInSlot: equipping a Lootline item in CS2's loadout also puts it on your weapon in a match
    inline CInlineHookObj<decltype(&hkLoadoutEquip)> LoadoutEquip{};
    // InventoryAPI.WearItemSticker(itemId, slot, wear): scraping / taking off a sticker — done here for Lootline guns
    inline CInlineHookObj<decltype(&hkWearItemSticker)> WearItemSticker{};
    // InventoryAPI.RemoveKeychain(itemId, slot): taking a charm off — done here for Lootline guns
    inline CInlineHookObj<decltype(&hkRemoveKeychain)> RemoveKeychain{};
    // InventoryAPI.SetNameToolString(text, itemId): the name typed for a name tag (pets too) — remembered for Lootline items
    inline CInlineHookObj<decltype(&hkSetNameToolString)> SetNameToolString{};
    // InventoryAPI.DeleteItem(itemId): CS2's "Delete" — Lootline items are removed here (Valve doesn't know them)
    inline CInlineHookObj<decltype(&hkDeleteItem)> DeleteItem{};
    // InventoryAPI.AcknowledgeNewItembyItemID(itemId): CS2's new item popup - Lootline items are acknowledged here (Valve
    // doesn't know them: its answer never comes, the "new" count would stay)
    inline CInlineHookObj<decltype(&hkAcknowledgeNewItem)> AcknowledgeNewItem{};
    // InventoryAPI.GetFlairItemId / GetFlairItemImage(xuid): the scoreboard's pin next to a name - yours is your
    // equipped Lootline pin (Settings -> "Lootline pin on the scoreboard")
    inline CInlineHookObj<decltype(&hkGetFlairItemId)> GetFlairItemId{};
    inline CInlineHookObj<decltype(&hkGetFlairItemImage)> GetFlairItemImage{};
    // InventoryAPI.ClearCustomName(itemId): the name tag's "Remove" — done here for Lootline items
    inline CInlineHookObj<decltype(&hkClearCustomName)> ClearCustomName{};
    // InventoryAPI.UseToolWithIntArg(terminal, terminal, n): a Lootline terminal's next offer / its end - done here
    inline CInlineHookObj<decltype(&hkUseToolWithIntArg)> UseToolWithIntArg{};
    // InventoryAPI.PerformItemCasketTransaction(0, terminal, terminal): a reopened Lootline terminal's offer - done here
    inline CInlineHookObj<decltype(&hkPerformItemCasketTransaction)> PerformItemCasketTransaction{};
    // StoreAPI.StoreItemPurchase("def(id),..."): a terminal's "accept" - anything with a Lootline id never reaches Steam
    inline CInlineHookObj<decltype(&hkStoreItemPurchase)> StoreItemPurchase{};
    // InventoryAPI.CraftIngredients(): the trade-up contract's "submit" — done here when it's all Lootline items
    inline CInlineHookObj<decltype(&hkCraftIngredients)> CraftIngredients{};
    // MissionsAPI.ActionRedeemOperationGoods(season, reward, stars): the Armory's "redeem" - done locally while Lootline's
    // Armory is on (Lootline's stars; never sent to Valve)
    inline CInlineHookObj<decltype(&hkRedeemArmory)> RedeemArmory{};
    // InventoryAPI.SetStatTrakSwapToolItems(a, b): the StatTrak Swap popup's two items (a Lootline tool swaps Lootline items)
    inline CInlineHookObj<decltype(&hkSetStatTrakSwapItems)> SetStatTrakSwapItems{};
    // InventoryAPI.GetCacheTypeElementFieldByIndex: "KeychainRemoveToolCharges" "charges" counts Lootline's Charm Detachments too
    inline CInlineHookObj<decltype(&hkCacheTypeField)> CacheTypeField{};
    // CS2's loadout shuffle ("favorites"): Lootline items' kept by Lootline, added to the slot's list; rolled every spawn
    inline CInlineHookObj<decltype(&hkAddFavorite)> AddFavorite{};
    inline CInlineHookObj<decltype(&hkRemoveFavorite)> RemoveFavorite{};
    inline CInlineHookObj<decltype(&hkIsFavorite)> IsFavorite{};
    inline CInlineHookObj<decltype(&hkGetShuffleItems)> GetShuffleItems{};
    // A recipe's "do these items fit" + a collection's "has knives / gloves for this quality": in a contract of only
    // Lootline items, Lootline's Souvenir reds count as normal ones (CS2 itself keeps Souvenir reds out of contracts)
    inline CInlineHookObj<decltype(&hkTradeUpRecipeMatch)> TradeUpRecipeMatch{};
    inline CInlineHookObj<decltype(&hkTradeUpCollectionRare)> TradeUpCollectionRare{};
    // CCSPlayerBase_CameraServices::GetFOV: the Misc tab's FOV changer (Chadware's)
    inline CInlineHookObj<decltype(&hkGetFov)> GetFov{};
    // C_CSWeaponBase "should draw crosshair": the Misc tab's crosshair on snipers (your own, unscoped)
    inline CInlineHookObj<decltype(&hkShouldDrawCrosshair)> ShouldDrawCrosshair{};
    // client.dll's "the live pet" (the first pet in your inventory): an equipped Lootline pet for InventoryAPI.GetPetItemID
    inline CInlineHookObj<decltype(&hkLivePetItemId)> LivePetItemId{};
    inline CInlineHookObj<decltype(&hkGetLootListItemIdByIndex)> GetLootListItemIdByIndex{};
    // LoadoutAPI.GetItemID(team, slot) / InventoryAPI.IsEquipped(item, team): the loadout screen shows a Lootline item
    // Lootline forced into a slot in a locked match (CS2 itself still has the old one there)
    inline CInlineHookObj<decltype(&hkLoadoutGetItemId)> LoadoutGetItemId{};
    inline CInlineHookObj<decltype(&hkIsEquipped)> IsEquipped{};
    // InventoryAPI.GetDecodeableRestriction(item): no X-Ray / loot box restriction for Lootline cases
    inline CInlineHookObj<decltype(&hkDecodeableRestriction)> DecodeableRestriction{};
    // The knife inspect fix (from the new Chadware source): the weapon's inspect request and the viewmodel graph
    // reading the weapon's state - a changed knife's inspect replayed while it's still playing works again
    inline CInlineHookObj<decltype(&hkKnifeInspect)> KnifeInspect{};
    inline CInlineHookObj<decltype(&hkKnifeReflect)> KnifeReflect{};
    // Agent voice lines (from the new Chadware source): a sound event starting (your voice lines are swapped for the
    // Lootline agent's), the radio text (which radio command you used), the response rules' level init / shutdown
    inline CInlineHookObj<decltype(&hkAgentVoiceStart)> AgentVoiceStart{};
    inline CInlineHookObj<decltype(&hkAgentRadioText)> AgentRadioText{};
    inline CInlineHookObj<decltype(&hkAgentLevelInit)> AgentLevelInit{};
    inline CInlineHookObj<decltype(&hkAgentLevelShutdown)> AgentLevelShutdown{};
    // The spray wheel's "spray": a Lootline graffiti is sprayed on your screen only, never sent to the server
    inline CInlineHookObj<decltype(&hkSprayAction)> SprayAction{};
    // StoreAPI.GetStoreItemEmbeddedAttributePrice: a Lootline terminal offer's price in the currency picked in Settings
    inline CInlineHookObj<decltype(&hkEmbeddedPrice)> EmbeddedPrice{};
    // CGameEventManager::FireEventClientSide: your knife kills say your Lootline knife (the kill feed's icon)
    inline CInlineHookObj<decltype(&hkFireEventClientSide)> FireEventClientSide{};
    // The lineup character builder (team intro / team select / end of match): yours wears your Lootline items
    inline CInlineHookObj<decltype(&hkLineupBuild)> LineupBuild{};

    class Hooks
    {
    public:
        void init();
        void shutdown();   // puts every hook back (call before waiting for in-flight calls)
    };
}
