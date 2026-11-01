#include "unboxer.h"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cmath>
#include <condition_variable>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <random>
#include <unordered_map>

#include "cases.h"
#include "game_chat.h"
#include "game_inventory.h"
#include "lineup.h"
#include "prices.h"
#include "../skinchanger/glove_changer.h"
#include "../skinchanger/item_schema.h"
#include "../skinchanger/skinchanger.h"
#include "../skinchanger/stattrak_tracker.h"
#include "../../../cs2/entity/C_CSPlayerPawn/C_CSPlayerPawn.h"
#include "../../config/config.h"
#include "../../config/configmanager.h"
#include "../../interfaces/interfaces.h"
#include "../../../patterns.h"
#include "../../utils/memory/patternscan/patternscan.h"
#include "../../utils/offsets/offsets.h"
#include "../../menu/localization.h"
#include "../../../../external/kiero/minhook/src/hde/hde64.h"

namespace
{
    // Our item ids: far above real ones (~4e10 in 2026) and below 2^53, so they survive the trip through JS numbers.
    constexpr std::uint64_t ID_BASE = 0x0000700000000000ull;

    // Unlock -> result. The reel is already spinning by then (the popup starts it right after UseTool); a real
    // Game Coordinator answer takes about this long.
    constexpr ULONGLONG OPEN_RESULT_DELAY_MS = 700;

    // Result -> its chat line, when the reel stops. decodable.js: the reel starts on Unlock (2 s case animation,
    // then ~5 s of scrolling; the item shows 6 s into it), and the result comes OPEN_RESULT_DELAY_MS after Unlock.
    constexpr ULONGLONG CHAT_LINE_DELAY_MS = 6500;

    constexpr int QUALITY_UNIQUE = 4;
    constexpr int QUALITY_STRANGE = 9;
    constexpr int QUALITY_UNUSUAL = 3;
    constexpr int QUALITY_SOUVENIR = 12;
    constexpr int QUALITY_VOLATILE = 14;              // an unsealed Uplink Terminal
    constexpr int ORIGIN_FOUND_IN_CRATE = 8;
    constexpr int MAX_CREATED_PER_TICK = 50;           // items made per frame (big additions take a few frames)
    constexpr int MAX_NEW_ITEMS = 60;                  // added items arrive NEW (CS2's new item popup) up to this many
                                                       // at once - a popup of hundreds would freeze the game. An addition
                                                       // is NEW as a whole or not at all (Request::asNew)
    constexpr int NOTIFICATION_UNLOCK_CRATE = 1007;   // client.dll maps GC request type 1007 to "crate_unlock"
    constexpr int NOTIFICATION_STICKER_APPLY = 1086;  // "sticker_apply"   (the same table, 2026-10-01)
    constexpr int NOTIFICATION_STICKER_REMOVE = 1053; // "sticker_remove"
    constexpr int NOTIFICATION_PATCH_APPLY = 1090;    // "patch_apply"
    constexpr int NOTIFICATION_PATCH_REMOVE = 1089;   // "patch_remove"
    constexpr int NOTIFICATION_KEYCHAIN_APPLY = 1091; // "keychain_apply"
    constexpr int NOTIFICATION_KEYCHAIN_REMOVE = 1092;// "keychain_remove"
    constexpr int NOTIFICATION_KEYCHAIN_SEAL = 1055;  // "keychain_seal": a sticker sealed in a Sticker Slab (2026-10-07)
    constexpr int NOTIFICATION_STICKER_EXTRACT = 1054;// "sticker_extract": a slab cracked, its sticker back
    constexpr int NOTIFICATION_NAME_ITEM = 1006;      // "nametag_add"
    constexpr int NOTIFICATION_NAME_REMOVE = 1030;    // "nametag_remove"
    constexpr int NOTIFICATION_GRAFFITI_UNSEAL = 9185;// "graffity_unseal" (Valve's spelling; k_EGCItemCustomizationNotification_GraffitiUnseal)
    constexpr int NOTIFICATION_CASKET_CONTENTS = 1012;// "casket_contents": a terminal's offer is in it (the laptop reads it out)

    // CS2's Uplink Terminals
    constexpr int TERMINAL_OFFERS = 5;                // the 5th offer is the final one
    constexpr std::uint32_t TERMINAL_LIFETIME_S = 3 * 86400;   // the laptop's battery is full at 3 days
    constexpr ULONGLONG TERMINAL_PURCHASE_DELAY_MS = 1000;

    // "Buy keys with Lootline": the keys come this long after Buy (a purchase's moment), counted at Valve's store price
    constexpr ULONGLONG KEY_PURCHASE_DELAY_MS = 900;
    constexpr double KEY_STORE_PRICE_USD = 2.49;
    constexpr int MAX_KEYS_PER_PURCHASE = 50;
    constexpr ULONGLONG TERMINAL_DISCARD_DELAY_MS = 2500;   // after the laptop closed itself
    constexpr std::uint16_t DEF_NAME_TAG = 1200;

    // A sticker on one of our guns -> the popup's answer: about a Game Coordinator round trip. (The popup arms its
    // 5 s "item not given" timer right after the call, so the answer must not come inside it.)
    constexpr ULONGLONG STICKER_RESULT_DELAY_MS = 400;

    // A trade-up contract of our items -> its result: about what the Game Coordinator takes for one.
    constexpr ULONGLONG TRADE_UP_RESULT_DELAY_MS = 800;

    struct Request
    {
        enum Type { AddCase, AddKey, Remove, RemoveAll, AddItem } type = AddCase;
        std::uint16_t definition = 0;
        int count = 0;
        bool withKeys = false;
        std::uint64_t id = 0;
        Unboxer::Item item;          // AddItem: the template
        Unboxer::RandomFloat randomFloat;   // AddItem: each copy's own float in this range
        bool randomSeed = false;            // AddItem: each copy's own pattern
        int asNew = -1;                     // adds: NEW all of it (1) or none of it (0); -1 = decided when it starts
    };

    struct PendingOpen
    {
        ULONGLONG at = 0;
        std::uint64_t caseId = 0;    // ours, 0 = a real case (opened with one of our keys)
        std::uint64_t keyId = 0;     // ours, 0 = none / a real key
        std::uint64_t opened = 0;    // the case the popup is for (ours or real) — its popup is "popup-inspect-<id>"
        Unboxer::Item drop;
    };

    // What our own scripts report back: they call InventoryAPI.UseTool("unboxer:<what>", "<text>"), which the UseTool
    // hook catches (main thread, g_mutex).
    constexpr const char* SCRIPT_REPORT_PREFIX = "unboxer:";
    std::vector<std::string> g_scriptReports;

    std::mutex g_mutex;              // everything below the line
    constexpr ULONGLONG SAVE_INTERVAL_MS = 1500;
    ULONGLONG g_lastSave = 0;        // GameTick's last items.json save
    std::map<std::uint16_t, Unboxer::TierOdds> g_customOdds;   // container -> its "Custom odds" (saved with the items)

    // Lootline Storage Units: CS2's casket popups' requests, answered a moment later like the Game Coordinator's
    struct CasketJob
    {
        ULONGLONG at = 0;
        int operation = 0;           // 1 put in, -1 take out, 0 look inside
        std::uint64_t casket = 0;
        std::uint64_t item = 0;
    };
    std::vector<CasketJob> g_casketJobs;
    void ReleaseCasketContents(std::uint64_t casket);   // (below, with the jobs) a unit about to go: its items back out

    // Lootline tools used with CS2's own popups: the StatTrak Swap Tool (its two items come first through
    // SetStatTrakSwapToolItems) and the Charm Detachment Pack (3 Charm Detachments, Lootline's own - saved)
    struct ToolJob
    {
        ULONGLONG at = 0;
        std::uint64_t tool = 0;
        std::uint64_t first = 0;     // the swap's two items
        std::uint64_t second = 0;
    };
    std::vector<ToolJob> g_toolJobs;
    std::uint64_t g_swapFirst = 0;
    std::uint64_t g_swapSecond = 0;
    int g_charmDetachments = 0;

    // "Buy keys with Lootline" (Settings -> Prices): on / off, the purchases waiting (g_mutex), what was spent (saved)
    std::atomic_bool g_buyKeys{ false };
    struct KeyPurchase
    {
        ULONGLONG at = 0;
        std::uint16_t key = 0;
        int count = 0;
    };
    std::vector<KeyPurchase> g_keyPurchases;
    double g_keySpent = 0.0;     // US dollars
    int g_keysBought = 0;

    // Loadout shuffle: Lootline items CS2's "Add to shuffle" (favorites) was used on, per side (saved); a spawn asks the main
    // thread to roll every slot with shuffle on
    struct Favorite
    {
        std::uint64_t id = 0;
        std::uint8_t sides = 0;      // EQUIP_T / EQUIP_CT; both = CS2's "noteam" (music kits)
    };
    std::vector<Favorite> g_favorites;
    std::atomic_bool g_shufflePending{ false };
    // CS2 itself picks once per match; Settings -> "Shuffle every round" picks at every spawn. A match starts with a map
    // load (OnLevelChange); until one is seen (the level hooks missing / injected mid-match) a new pawn counts as one.
    std::atomic_bool g_shuffleEveryRound{ false };
    std::atomic_bool g_newMatch{ true };
    std::atomic_bool g_levelSeen{ false };
    std::uintptr_t g_shufflePawn = 0;        // game thread: the pawn the last pick was for

    // Inventory tab -> Loadouts: what was equipped, saved under a name - item id + its sides (saved with the items).
    struct LoadoutProfile
    {
        std::string name;
        std::vector<std::pair<std::uint64_t, std::uint8_t>> items;
    };

    std::vector<LoadoutProfile> g_loadouts;

    // The Armory with Lootline stars: the balance (saved), the redeems waiting for their answer (~a Game Coordinator's)
    std::atomic_bool g_armoryOn{ false };
    int g_armoryStars = 40;
    struct PendingArmory
    {
        ULONGLONG at = 0;
        int index = -1;              // the reward; -1 = nothing to give (refused): only the screen is set back
    };
    std::vector<PendingArmory> g_armoryJobs;
    constexpr ULONGLONG ARMORY_RESULT_DELAY_MS = 900;
    constexpr int MAX_ARMORY_STARS = 100000;
    std::vector<Unboxer::Item> g_items;
    std::vector<Request> g_requests;
    std::vector<PendingOpen> g_pending;
    struct PendingUpgrade
    {
        ULONGLONG at = 0;
        std::vector<std::uint64_t> inputIds;
        Unboxer::Item target;
        bool win = false;
        double multiplier = 1.0;
    };
    std::vector<PendingUpgrade> g_upgrades;
    Unboxer::UpgradeStats g_upgradeStats;
    Unboxer::DropSettings g_drop;
    int g_pickedOpenings = 0;        // Choose: openings since the item was picked / last given (DropSettings::dropOn)
    Unboxer::Status g_status;
    std::vector<std::pair<std::uint16_t, int>> g_goldPool;   // ★ knife / gloves + finish, built by the render thread
    std::uint64_t g_nextId = ID_BASE;
    bool g_dirty = false;
    std::atomic_bool g_loadoutDirty{ true };   // equipment changed: SyncLoadout rebuilds the changer's loadout
    std::atomic<std::uint64_t> g_livePetId{ 0 };   // the equipped Lootline pet (SyncLoadout), see Unboxer::LivePetId
    struct ChatLine
    {
        ULONGLONG at = 0;
        std::string item;            // "StatTrak™ AK-47 | Redline": no wear, like the real message
        int rarity = 0;
        bool crafted = false;        // a trade-up ("has fulfilled a contract and received")
    };
    std::vector<ChatLine> g_chatLines;         // drop messages waiting for their reel to stop
    struct PendingSticker
    {
        ULONGLONG at = 0;
        bool apply = true;           // true: put `sticker` in `slot`; false: scrape / take off (`wear`)
        std::uint64_t gunId = 0;
        std::uint64_t toolId = 0;    // our sticker item, used up when it's applied (0 = a real one: it stays)
        int slot = 0;
        Unboxer::Sticker sticker;
        int wear = 0;                // WearItemSticker: 0-100 scrape to that much, > 100 take it off
        bool patch = false;          // a patch on one of our agents (the patch popup's answers)
        bool copy = false;           // on one of your own items: our sticker / patch goes on a Lootline copy of it
    };
    std::vector<PendingSticker> g_stickerJobs;
    struct PendingCharm
    {
        ULONGLONG at = 0;
        bool apply = true;           // true: hang `keychain` on the gun; false: take it off (the charm comes back)
        bool copy = false;           // the gun isn't a Lootline one (a real or stock weapon): a Lootline copy of it gets the charm
        std::uint64_t gunId = 0;
        std::uint64_t toolId = 0;    // our charm item, used up when it goes on
        int keychain = 0;
        int seed = 0;
        float x = 0.0f;              // where the popup put it
        float y = 0.0f;
        float z = 0.0f;
        int sticker = 0;             // a Sticker Slab's sealed sticker
        int highlight = 0;           // a highlight charm's clip
    };
    std::vector<PendingCharm> g_charmJobs;

    // CS2's "Seal in Sticker Slab" / "Crack Slab and Extract Sticker" with a Lootline sticker / slab
    struct PendingSlab
    {
        ULONGLONG at = 0;
        bool seal = true;            // true: sticker -> slab charm; false: slab charm -> sticker (the slab is gone)
        std::uint64_t stickerId = 0; // seal: the sticker used (ours: used up; a real one: left alone)
        std::uint64_t slabId = 0;    // seal: the slab tool (ours: used up; a real / the store's one: left alone); crack: the slab charm
        bool ourSticker = false;
        bool ourSlab = false;
        int kit = 0;                 // the sticker
    };
    std::vector<PendingSlab> g_slabJobs;
    struct PendingName
    {
        ULONGLONG at = 0;
        std::uint64_t itemId = 0;
        std::string name;
        bool copy = false;           // on one of your own items: the name goes on a Lootline copy of it
        std::uint64_t toolId = 0;    // copy: our name tag (used up)
    };
    std::vector<PendingName> g_nameJobs;
    std::string g_nameToolText;      // the last name typed for a name tag (SetNameToolString)
    struct PendingUnseal
    {
        ULONGLONG at = 0;
        std::uint64_t sealedId = 0;  // our sealed graffiti: becomes an unsealed one
    };
    std::vector<PendingUnseal> g_unsealJobs;
    struct TerminalOffer
    {
        std::uint64_t terminalId = 0;
        Unboxer::Item offer;         // in the game's inventory "inside" the terminal; in g_items only once it's bought
    };
    std::vector<TerminalOffer> g_terminalOffers;   // not saved: a reopened terminal after a restart makes a new one
    struct TerminalJob
    {
        enum class What { Unseal, Offer, Resume, End, Purchase, Discard };
        ULONGLONG at = 0;
        What what = What::Offer;
        std::uint64_t id = 0;        // the terminal (the sealed one for Unseal)
    };
    std::vector<TerminalJob> g_terminalJobs;
    int g_pickedTerminals = 0;       // Choose: terminals unsealed since the item was picked / last offered (DropSettings::terminalOn)
    std::vector<std::pair<std::uint64_t, int>> g_terminalPicks;   // unsealed terminal -> the offer your item comes as
    struct PendingTradeUp
    {
        ULONGLONG at = 0;
        void* api = nullptr;                   // the InventoryAPI object holding the contract
        std::vector<std::uint64_t> inputs;     // ours, all of them
        Unboxer::Item output;                  // rolled when it was submitted
    };
    std::vector<PendingTradeUp> g_tradeUps;
    std::vector<std::string> g_popupMessages;  // "OK" popups to show from the main menu (next GameTick)
    // ─────────────────────────────────────────────────────

    std::atomic_bool g_chatDrops{ true };      // Settings -> "Drop message in chat"

    // game thread
    bool g_restored = false;
    std::mt19937_64 g_rng{ static_cast<std::uint64_t>(GetTickCount64()) ^ 0x9E3779B97F4A7C15ull };

    // render thread
    // In a match the hands keep the gloves they were built with: after a glove change the player model is swapped
    // for a frame (SkinChanger::RefreshPlayerModel, Chadware's "Refresh Gloves"), the game rebuilds the hands and the
    // glove changer puts the new ones on. A moment later, so the new gloves are written first.
    constexpr ULONGLONG HANDS_REFRESH_DELAY_MS = 300;
    std::atomic<ULONGLONG> g_handsRefreshAt{ 0 };   // set from the game thread too (the spawn reapplies)
    struct GloveChoice
    {
        std::uint16_t definition = 0;
        int paintKit = 0;
        int seed = 0;
        float wear = 0.0f;
        CustomColor::Paint paint;   // new colours = the hands rebuilt too

        bool operator==(const GloveChoice&) const = default;
    };
    std::array<GloveChoice, 2> g_appliedGloves{};   // [Config::TEAM_T], [Config::TEAM_CT]: what the skin changer has

    // Inventory -> Custom colour: an equipped item's new colours are put on in the match this long after the last change
    // (a colour picker changes them every frame while it's dragged, and each reapply rebuilds every skin)
    constexpr ULONGLONG PAINT_REAPPLY_DELAY_MS = 400;
    std::atomic<ULONGLONG> g_paintReapplyAt{ 0 };
    std::vector<std::uint16_t> g_goldCandidates;
    std::size_t g_goldNext = 0;
    bool g_goldListed = false;

    // ── panorama: one line of JS in the main menu ──
    struct PanoramaUiPanel;
    struct PanoramaUiEngine;

    struct PanoramaPanel2D
    {
        const void* vmt;
        PanoramaUiPanel* uiPanel;
    };

    using RunScriptFn = void(__fastcall*)(PanoramaUiEngine*, PanoramaUiPanel*, const char*, const char*, std::uint64_t);

    PanoramaUiEngine** g_uiEngine = nullptr;
    PanoramaPanel2D** g_mainMenu = nullptr;
    RunScriptFn g_runScript = nullptr;

    bool RunMainMenuScript(const char* script)
    {
        if (!g_uiEngine || !g_mainMenu || !g_runScript || !script)
            return false;

        __try
        {
            PanoramaUiEngine* engine = *g_uiEngine;
            PanoramaPanel2D* root = *g_mainMenu;

            if (!engine || !root || !root->uiPanel)
                return false;

            static const char originFile = '\0';
            g_runScript(engine, root->uiPanel, script, &originFile, 1);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    // ── the result, written straight into the case popup ──
    // The popup (popup_capability_decodable) keeps the result in its panel's Data().itemFromContainer: the reel's
    // landing tile shows that item when the reel is filled (~2 s after Unlock), and the reveal (~8 s) opens it — or
    // shows "unable to retrieve your item" when it's empty. Data() belongs to the panel, not to a script: Valve's own
    // openers set elPanel.Data().oSettings and the popup reads $.GetContextPanel().Data().oSettings. So a script in
    // the main menu can hand the popup its result directly, whatever happens to the notification event. The popup
    // panel is "popup-inspect-<case id>" (else: found from its DecodableItemsScroll). Reports back through UseTool.
    // The "unable to retrieve your item" error on weapon cases: those popups have the Open / Rent bar
    // (allow_rent = InventoryAPI.CanOpenForRental; souvenir packages don't, and never showed the error). Its Open
    // button starts a 6 s timer (popup_inspect_rental-bar _OpenActions) that closes the popup with that error unless
    // an inventory update brings a new unacknowledged "found_in_crate" item — what Valve's server sends. Our drop
    // isn't made a new item (acknowledging new items goes through the Game Coordinator), so the timer is
    // cancelled the way the rental bar itself does it: $.CancelScheduled on the handle it keeps in Data(). Schedule
    // handles are engine-wide, so the main menu can cancel it (tested in game 2026-09-30; the popup's own context
    // is off limits: RunScriptInPanelContext refuses with "a domain that matches this panel").
    constexpr const char* DIRECT_RESULT_SCRIPT = R"JS((function(){
var id='%s',caseId='%s',r=[];
try{
var root=$.GetContextPanel();while(root.GetParent())root=root.GetParent();
var isPopup=function(p){return p&&p.IsValid()&&p.Data()&&p.Data().scrollListsPanelIds!==undefined;};
var cp=root.FindChildTraverse('popup-inspect-'+caseId);
if(isPopup(cp)){r.push('by-id');}
else{cp=root.FindChildTraverse('DecodableItemsScroll');while(cp&&!isPopup(cp))cp=cp.GetParent();if(cp)r.push('by-scroll');}
if(!cp){r.push('popup-not-found');}
else{
var d=cp.Data();r.push('before='+d.itemFromContainer);d.itemFromContainer=id;
var s=cp.FindChildInLayoutFile('DecodableItemsScroll');var hidden=!s||s.BHasClass('hidden');r.push('reel-hidden='+hidden);
if(!hidden){for(var k=0;k<d.scrollListsPanelIds.length;++k){var l=cp.FindChildInLayoutFile(d.scrollListsPanelIds[k]);var t=l?l.FindChildInLayoutFile('ItemFromContainer'):null;if(!t)continue;
if(InventoryAPI.GetItemQuality(id)===3&&d.unusualItemImagePath){t.FindChildInLayoutFile('ItemImage').SetImage('file://{images}/'+d.unusualItemImagePath);t.FindChildInLayoutFile('JsRarity').style.washColor='#ffd700';t.FindChildInLayoutFile('JItemTint').style.washColor='#ffd700';}
else{var c=InventoryAPI.GetItemRarityColor(id);t.FindChildInLayoutFile('ItemImage').itemid=id;t.FindChildInLayoutFile('JsRarity').style.washColor=c;t.FindChildInLayoutFile('JItemTint').style.washColor=c;}
r.push('tile-'+k);}}
r.push('now='+d.itemFromContainer);
if(typeof d.rentalBarScheduleActionTimoutHandle!=='number'){r.push('rental-timer=none');}
else{$.CancelScheduled(d.rentalBarScheduleActionTimoutHandle);d.rentalBarScheduleActionTimoutHandle=null;r.push('rental-timer=cancelled');}
}
}catch(e){r.push('error: '+e);}
InventoryAPI.UseTool('unboxer:direct',r.join(' '));
})();)JS";

    // A listener in the main menu for the duration of one native send: shows in the log whether (and with what
    // arguments) the notification reaches JS at all. Registered and removed around the send, so it never outlives it.
    constexpr const char* PROBE_ON_SCRIPT = R"JS((function(){
var d=$.GetContextPanel().Data();if(d.unboxerProbe!==undefined)return;
d.unboxerProbe=$.RegisterForUnhandledEvent('PanoramaComponent_Inventory_ItemCustomizationNotification',function(n,t,i){InventoryAPI.UseTool('unboxer:probe',String(n)+' | '+String(t)+' | '+String(i));});
})();)JS";

    constexpr const char* PROBE_OFF_SCRIPT = R"JS((function(){
var d=$.GetContextPanel().Data();if(d.unboxerProbe===undefined)return;
$.UnregisterForUnhandledEvent('PanoramaComponent_Inventory_ItemCustomizationNotification',d.unboxerProbe);d.unboxerProbe=undefined;
})();)JS";

    // ── the game's main thread ──
    // panorama.dll's DispatchEvent (CUIEngine vfunc 47) starts with tier0 ThreadInMainThread() and deletes the event
    // unread when it's false. FrameStageNotify runs on another thread: tests 1 and 2 sent the result from there and
    // the popup never got it (reel on a random tile, then "unable to retrieve your item").
    using ThreadInMainThreadFn = bool(__cdecl*)();
    ThreadInMainThreadFn g_threadInMainThread = nullptr;
    bool g_threadLogged = false;
    std::atomic_bool g_newItemsEnabled{ false };       // the acknowledge hook is in: added items may arrive NEW
    std::atomic_bool g_unloadAcknowledge{ false };     // unloading: the main thread acknowledges every NEW Lootline item
    std::atomic_bool g_unloadAcknowledged{ false };


    // ── the result for the case popup, sent the way the game sends it ──
    // What the Game Coordinator reply handler does for "crate_unlock" (client.dll+0xF81B51, 2026-09-29): a 0x30-byte
    // event from the game's allocator — vtable, event id (word +8), no target panel (+0xC = 0, +0x10 = -1),
    // int32 request type (+0x28), strings type (+0x20) and item id (+0x18) — handed to the UI engine's dispatch vfunc.
    // The client builds the same event itself for other notifications (+0xFDF1A5), so this is the normal way in.
    using AllocFn = void* (__fastcall*)(std::size_t);
    using SetStringFn = void(__fastcall*)(void* field, const char* text);
    using DispatchFn = bool(__fastcall*)(void* engine, void** eventPointer);

    void** g_notifyEngine = nullptr;
    AllocFn g_notifyAlloc = nullptr;
    const std::uint16_t* g_notifyEventId = nullptr;
    void* g_notifyVtable = nullptr;
    SetStringFn g_notifySetString = nullptr;
    std::uint32_t g_notifyDispatchOffset = 0;

    template <typename T>
    T RipTarget(std::uint8_t* instructionEnd, std::uint8_t* displacement)
    {
        return reinterpret_cast<T>(instructionEnd + *reinterpret_cast<const std::int32_t*>(displacement));
    }

    const char* g_notifyWhy = "";   // why the native notification wasn't found (the start-up log)

    // CrateUnlockNotify matches "mov r14, [UI engine]" where the reply handler makes the event; the rest is read from the
    // instructions after it with MinHook's length decoder, not at fixed offsets - CS2 updates move them (2026-10-14: a
    // short je became a near one and the fields are zeroed with immediates, everything shifted by 4 to 17 bytes). In
    // order: mov ecx, 30h (the event's size) / mov r15, [rax + disp32] (the engine's dispatch slot) / call (allocator) /
    // movzx ecx, word [rip + id] (event id) / lea rax, [rip + vtable] / call (the string setter, type name into +0x20).
    // Nothing is kept unless every piece is there and inside client.dll.
    bool ResolveNativeNotify()
    {
        std::uint8_t* p = M::FindPattern("client.dll", Patterns::CrateUnlockNotify);

        if (!p || p[0] != 0x4C || p[1] != 0x8B || p[2] != 0x35)
        {
            g_notifyWhy = p ? "the signature doesn't start with mov r14, [rip + engine]" : "signature CrateUnlockNotify not found";
            return false;
        }

        void** engine = RipTarget<void**>(p + 7, p + 3);
        std::uint32_t dispatchOffset = 0;
        AllocFn alloc = nullptr;
        const std::uint16_t* eventId = nullptr;
        void* vtable = nullptr;
        SetStringFn setString = nullptr;
        bool sizeSeen = false;
        std::uint8_t* at = p;

        for (int count = 0; count < 64 && at < p + 0x100 && !setString; ++count)
        {
            hde64s hs{};
            const unsigned int length = hde64_disasm(at, &hs);

            if (length == 0 || (hs.flags & F_ERROR))
            {
                g_notifyWhy = "an instruction after the signature couldn't be decoded";
                return false;
            }

            const bool ripRelative = (hs.flags & F_MODRM) && hs.modrm_mod == 0 && hs.modrm_rm == 5;
            std::uint8_t* next = at + length;

            if (!sizeSeen)
            {
                sizeSeen = hs.opcode == 0xB9 && hs.imm.imm32 == 0x30;   // mov ecx, 30h
            }
            else if (dispatchOffset == 0)
            {
                // mov reg64, [rax + disp32]
                if (hs.opcode == 0x8B && hs.rex_w && (hs.flags & F_MODRM) && hs.modrm_mod == 2 && hs.modrm_rm == 0 &&
                    !hs.rex_b && !(hs.flags & F_SIB))
                    dispatchOffset = hs.disp.disp32;
            }
            else if (!alloc)
            {
                if (hs.opcode == 0xE8)
                    alloc = reinterpret_cast<AllocFn>(next + static_cast<std::int32_t>(hs.imm.imm32));
            }
            else if (!eventId)
            {
                // movzx r32, word [rip + disp32]
                if (hs.opcode == 0x0F && hs.opcode2 == 0xB7 && ripRelative)
                    eventId = reinterpret_cast<const std::uint16_t*>(next + static_cast<std::int32_t>(hs.disp.disp32));
            }
            else if (!vtable)
            {
                // lea reg64, [rip + disp32]
                if (hs.opcode == 0x8D && hs.rex_w && ripRelative)
                    vtable = next + static_cast<std::int32_t>(hs.disp.disp32);
            }
            else if (hs.opcode == 0xE8)
            {
                setString = reinterpret_cast<SetStringFn>(next + static_cast<std::int32_t>(hs.imm.imm32));
            }

            at = next;
        }

        if (!setString)
        {
            g_notifyWhy = !sizeSeen ? "no mov ecx, 30h after the signature" : dispatchOffset == 0 ? "no dispatch slot" : !alloc ? "no allocator call" :
                          !eventId ? "no event id" : !vtable ? "no event vtable" : "no string setter call";
            return false;
        }

        if (dispatchOffset >= 0x1000 || (dispatchOffset % 8) != 0)
        {
            g_notifyWhy = "the dispatch slot makes no sense";
            return false;
        }

        // a wrong match would point anywhere: everything found must be inside client.dll
        const auto* base = reinterpret_cast<const std::uint8_t*>(GetModuleHandleA("client.dll"));

        if (!base)
            return false;

        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
        const std::uint8_t* end = base + nt->OptionalHeader.SizeOfImage;
        const auto inside = [base, end](const void* address) {
            const auto* x = static_cast<const std::uint8_t*>(address);
            return x >= base && x < end;
        };

        if (!inside(engine) || !inside(reinterpret_cast<const void*>(alloc)) || !inside(eventId) || !inside(vtable) ||
            !inside(reinterpret_cast<const void*>(setString)))
        {
            g_notifyWhy = "something found points outside client.dll";
            return false;
        }

        g_notifyEngine = engine;
        g_notifyDispatchOffset = dispatchOffset;
        g_notifyAlloc = alloc;
        g_notifyEventId = eventId;
        g_notifyVtable = vtable;
        g_notifySetString = setString;
        return true;
    }

    // True = handed to the engine on the main thread (it goes to the listeners right away). `handled` = what the
    // dispatch returned (for the log only: an event nobody "handles" still reaches RegisterForUnhandledEvent).
    // type / typeName: the Game Coordinator request it answers (1007 "crate_unlock", 1086 "sticker_apply"...).
    bool NotifyItemCustomization(int type, const char* typeName, const char* itemIdText, bool& handled)
    {
        handled = false;

        if (!g_notifyEngine || !g_notifyAlloc || !g_notifyEventId || !g_notifyVtable || !g_notifySetString)
            return false;

        if (!Unboxer::OnMainThread())
            return false;   // it would be deleted unread

        __try
        {
            void* engine = *g_notifyEngine;

            if (!engine)
                return false;

            const auto dispatch = reinterpret_cast<DispatchFn>((*reinterpret_cast<void***>(engine))[g_notifyDispatchOffset / 8]);
            auto* event = static_cast<std::uint8_t*>(g_notifyAlloc(0x30));

            if (!event)
                return false;

            std::memset(event, 0, 0x30);
            *reinterpret_cast<void**>(event) = g_notifyVtable;
            *reinterpret_cast<std::uint16_t*>(event + 0x08) = *g_notifyEventId;
            *reinterpret_cast<std::uint64_t*>(event + 0x0C) = 0xFFFFFFFF00000000ull;
            *reinterpret_cast<std::int32_t*>(event + 0x28) = type;
            g_notifySetString(event + 0x20, typeName);
            g_notifySetString(event + 0x18, itemIdText);

            void* eventPointer = event;
            handled = dispatch(engine, &eventPointer);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    // ── "purchase completed", sent the way the store sends it ──
    // CS2's terminal laptop waits for PanoramaComponent_Store_PurchaseCompleted(item id) after "accept". The store's
    // handler (client.dll+0x10970E0, 2026-10-03) builds it like the item customization event above: 0x20 bytes from the
    // same allocator, its own vtable, the event id (word +8), +0xC = 0xFFFFFFFF00000000, the item id string at +0x18,
    // handed to the same dispatch. Its id and vtable are found by the event's name: the registration (lea rdx, [name] ...
    // lea rcx, [id]) and the one place that reads that id and then puts the vtable in (movzx ecx, word [id] ... lea rax,
    // [vtable] ... call SetString).
    const std::uint16_t* g_purchaseEventId = nullptr;
    void* g_purchaseEventVtable = nullptr;

    const std::uint8_t* RipTarget7(const std::uint8_t* instruction)
    {
        std::int32_t displacement = 0;
        std::memcpy(&displacement, instruction + 3, sizeof(displacement));
        return instruction + 7 + displacement;
    }

    bool ResolvePurchaseCompleted()
    {
        const std::uint8_t* text = nullptr;
        std::size_t size = 0;
        const std::uint8_t* name = GameInv::ClientString("PanoramaComponent_Store_PurchaseCompleted");

        if (!name || !g_notifySetString || !GameInv::ClientCode(text, size))
            return false;

        const std::uint8_t* id = nullptr;

        for (std::size_t i = 0; i + 0x30 <= size && !id; ++i)
        {
            const std::uint8_t* p = text + i;

            if (p[0] != 0x48 || p[1] != 0x8D || p[2] != 0x15 || RipTarget7(p) != name)
                continue;

            for (std::size_t k = 7; k + 7 <= 0x30; ++k)
            {
                if (p[k] == 0x48 && p[k + 1] == 0x8D && p[k + 2] == 0x0D)
                {
                    id = RipTarget7(p + k);
                    break;
                }
            }
        }

        if (!id)
            return false;

        static constexpr std::uint8_t store[] = { 0x66, 0x89, 0x48, 0x08 };                              // mov [rax+8], cx
        static constexpr std::uint8_t target[] = { 0x00, 0x00, 0x00, 0x00, 0xFF, 0xFF, 0xFF, 0xFF };   // 0xFFFFFFFF00000000

        for (std::size_t i = 0; i + 0x60 <= size; ++i)
        {
            const std::uint8_t* p = text + i;

            if (p[0] != 0x0F || p[1] != 0xB7 || p[2] != 0x0D || RipTarget7(p) != id)
                continue;

            // the event's shape right after: the id into +8, 0xFFFFFFFF00000000 into +0xC
            if (std::search(p + 7, p + 0x17, std::begin(store), std::end(store)) == p + 0x17 ||
                std::search(p + 7, p + 0x30, std::begin(target), std::end(target)) == p + 0x30)
                continue;

            for (std::size_t k = 7; k + 7 <= 0x40; ++k)
            {
                if (p[k] != 0x48 || p[k + 1] != 0x8D || p[k + 2] != 0x05)
                    continue;

                const std::uint8_t* vtable = RipTarget7(p + k);

                // the string goes in with the same SetString as the item customization event's
                for (std::size_t c = k + 7; c + 5 <= k + 0x20; ++c)
                {
                    if (p[c] != 0xE8)
                        continue;

                    std::int32_t displacement = 0;
                    std::memcpy(&displacement, p + c + 1, sizeof(displacement));

                    if (p + c + 5 + displacement == reinterpret_cast<const std::uint8_t*>(g_notifySetString))
                    {
                        g_purchaseEventId = reinterpret_cast<const std::uint16_t*>(id);
                        g_purchaseEventVtable = const_cast<std::uint8_t*>(vtable);
                        return true;
                    }
                }

                break;
            }
        }

        return false;
    }

    // True = handed to the engine on the main thread.
    bool NotifyPurchaseCompleted(const char* itemIdText, bool& handled)
    {
        handled = false;

        if (!g_notifyEngine || !g_notifyAlloc || !g_notifySetString || !g_purchaseEventId || !g_purchaseEventVtable)
            return false;

        if (!Unboxer::OnMainThread())
            return false;   // it would be deleted unread

        __try
        {
            void* engine = *g_notifyEngine;

            if (!engine)
                return false;

            const auto dispatch = reinterpret_cast<DispatchFn>((*reinterpret_cast<void***>(engine))[g_notifyDispatchOffset / 8]);
            auto* event = static_cast<std::uint8_t*>(g_notifyAlloc(0x20));

            if (!event)
                return false;

            std::memset(event, 0, 0x20);
            *reinterpret_cast<void**>(event) = g_purchaseEventVtable;
            *reinterpret_cast<std::uint16_t*>(event + 0x08) = *g_purchaseEventId;
            *reinterpret_cast<std::uint64_t*>(event + 0x0C) = 0xFFFFFFFF00000000ull;
            g_notifySetString(event + 0x18, itemIdText);

            void* eventPointer = event;
            handled = dispatch(engine, &eventPointer);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    // ── files: %USERPROFILE%\.unboxer\ ──
    std::filesystem::path DataFolder()
    {
        return internal_config::ConfigManager::GetConfigDirectory().parent_path();
    }

    void Log(const char* format, ...)
    {
        char line[2048];
        va_list args;
        va_start(args, format);
        vsnprintf(line, sizeof(line), format, args);
        va_end(args);

        const std::filesystem::path path = DataFolder() / "lootline_log.txt";
        std::error_code ec;
        const bool tooBig = std::filesystem::exists(path, ec) && std::filesystem::file_size(path, ec) > 512 * 1024;

        FILE* file = nullptr;
        if (_wfopen_s(&file, path.c_str(), tooBig ? L"w" : L"a") != 0 || !file)
            return;

        SYSTEMTIME time{};
        GetLocalTime(&time);
        fprintf(file, "%02d:%02d:%02d.%03d  %s\n", time.wHour, time.wMinute, time.wSecond, time.wMilliseconds, line);
        fclose(file);
    }

    std::filesystem::path ItemsPath()
    {
        return DataFolder() / "items.json";
    }

    // What items.json holds, copied under g_mutex (cheap) and written out of it: on the save thread (QueueSaveLocked) or
    // right away (SaveLocked, unloading). Building and writing a big inventory's JSON on the game's main thread was a
    // hitch after every opening / every change (2026-10-19).
    struct SaveSnapshot
    {
        std::vector<Unboxer::Item> items;
        Unboxer::UpgradeStats upgrade;
        std::map<std::uint16_t, Unboxer::TierOdds> customOdds;
        std::vector<LoadoutProfile> loadouts;
        std::vector<Favorite> favorites;
        std::uint64_t nextId = 0;
        int armoryStars = 0;
        int charmDetachments = 0;
        double keySpent = 0.0;
        int keysBought = 0;
    };

    // call with g_mutex held
    SaveSnapshot SnapshotLocked()
    {
        SaveSnapshot snapshot;
        snapshot.items = g_items;
        snapshot.upgrade = g_upgradeStats;
        snapshot.customOdds = g_customOdds;
        snapshot.loadouts = g_loadouts;
        snapshot.favorites = g_favorites;
        snapshot.nextId = g_nextId;
        snapshot.armoryStars = g_armoryStars;
        snapshot.charmDetachments = g_charmDetachments;
        snapshot.keySpent = g_keySpent;
        snapshot.keysBought = g_keysBought;
        return snapshot;
    }

    // items.json from a snapshot (via a .tmp, renamed over the old one). Any thread, g_saveFileMutex held. True = written.
    bool WriteSnapshot(const SaveSnapshot& snapshot)
    {
        using json = nlohmann::json;
        json list = json::array();

        for (const Unboxer::Item& item : snapshot.items)
        {
            json entry = {
                { "id", item.id },
                { "kind", static_cast<int>(item.kind) },
                { "def", item.definition },
                { "paint", item.paintKit },
                { "seed", item.seed },
                { "wear", item.wear },
                { "stattrak", item.statTrak },
                { "rarity", item.rarity },
                { "quality", item.quality },
                { "case", item.fromCase },
                { "equip", item.equipped },
                { "slot_t", item.slotT },
                { "slot_ct", item.slotCT },
            };

            json stickers = json::array();
            for (int slot = 0; slot < Unboxer::STICKER_SLOTS; ++slot)
            {
                const Unboxer::Sticker& sticker = item.stickers[slot];
                if (sticker.kit > 0)
                    stickers.push_back({ { "slot", slot }, { "kit", sticker.kit }, { "wear", sticker.wear }, { "rotation", sticker.rotation },
                        { "x", sticker.offsetX }, { "y", sticker.offsetY }, { "schema", sticker.schema } });
            }

            if (!stickers.empty())
                entry["stickers"] = std::move(stickers);

            if (item.kills >= 0)
                entry["kills"] = item.kills;
            if (!item.customName.empty())
                entry["name"] = item.customName;
            if (item.charm > 0)
                entry["charm"] = item.charm;
            if (item.charmSeed > 0)
                entry["charm_seed"] = item.charmSeed;
            if (item.charm > 0 && (item.charmX != 0.0f || item.charmY != 0.0f || item.charmZ != 0.0f))
            {
                entry["charm_x"] = item.charmX;
                entry["charm_y"] = item.charmY;
                entry["charm_z"] = item.charmZ;
            }
            if (item.charmSticker > 0)
                entry["charm_sticker"] = item.charmSticker;
            if (item.highlight > 0)
                entry["highlight"] = item.highlight;
            if (item.tint > 0)
                entry["tint"] = item.tint;
            if (item.sprays > 0)
                entry["sprays"] = item.sprays;
            if (item.petId > 0)
            {
                entry["pet_id"] = item.petId;
                entry["pet_seed"] = item.petSeed;
                entry["pet_level"] = item.petLevel;
                entry["pet_hatch"] = item.petHatch;
            }
            if (item.expires != 0)
            {
                entry["expires"] = item.expires;
                entry["offer"] = item.offerNumber;
            }
            if (item.casket != 0)
                entry["casket"] = item.casket;
            if (item.modified != 0)
                entry["modified"] = item.modified;
            if (item.paint != CustomColor::Paint{})
                entry["colour"] = { { "on", item.paint.on }, { "mode", item.paint.mode }, { "primary", item.paint.primary },
                    { "secondary", item.paint.secondary } };

            list.push_back(std::move(entry));
        }

        const json upgrader = {
            { "tries", snapshot.upgrade.tries }, { "wins", snapshot.upgrade.wins }, { "losses", snapshot.upgrade.losses },
            { "best_multiplier", snapshot.upgrade.bestMultiplier }, { "best_name", snapshot.upgrade.bestName }, { "streak", snapshot.upgrade.streak },
        };
        json customOdds = json::array();
        for (const auto& [container, odds] : snapshot.customOdds)
            customOdds.push_back({ { "case", container }, { "odds", odds } });

        json loadouts = json::array();
        for (const LoadoutProfile& profile : snapshot.loadouts)
        {
            json entries = json::array();
            for (const auto& [id, sides] : profile.items)
                entries.push_back({ { "id", id }, { "sides", sides } });

            loadouts.push_back({ { "name", profile.name }, { "items", std::move(entries) } });
        }

        json root = { { "version", 1 }, { "next_id", snapshot.nextId }, { "items", list }, { "upgrader", upgrader }, { "custom_odds", customOdds },
            { "loadouts", loadouts }, { "armory_stars", snapshot.armoryStars }, { "charm_detachments", snapshot.charmDetachments },
            { "key_spent", snapshot.keySpent }, { "keys_bought", snapshot.keysBought } };

        json favorites = json::array();
        for (const Favorite& favorite : snapshot.favorites)
            favorites.push_back({ { "id", favorite.id }, { "sides", favorite.sides } });
        root["favorites"] = std::move(favorites);
        const std::filesystem::path path = ItemsPath();
        const std::filesystem::path temp = path.wstring() + L".tmp";

        {
            std::ofstream file(temp, std::ios::trunc);
            if (!file)
                return false;
            file << root.dump(2, ' ', false, json::error_handler_t::replace);
            if (!file)
                return false;
        }

        std::error_code ec;
        std::filesystem::rename(temp, path, ec);
        return !ec;
    }

    // ── the save thread: one snapshot waits at a time (a newer one replaces it); started with the first save, stopped
    //    (after writing what waits) by Shutdown - a Windows thread handle, not a std::thread (a CS2 closed without
    //    unloading must not run a joinable std::thread's destructor) ──
    std::mutex g_saveQueueMutex;
    std::condition_variable g_saveWake;
    std::unique_ptr<SaveSnapshot> g_saveQueued;   // the newest snapshot waiting, null = none
    bool g_saveStop = false;
    HANDLE g_saveThread = nullptr;
    std::mutex g_saveFileMutex;                   // items.json and its .tmp: one writer at a time
    std::atomic_bool g_saveFailed{ false };       // a write failed: GameTick marks the list unsaved again

    DWORD WINAPI SaveThreadMain(LPVOID)
    {
        for (;;)
        {
            std::unique_ptr<SaveSnapshot> snapshot;
            {
                std::unique_lock lock(g_saveQueueMutex);
                g_saveWake.wait(lock, [] { return g_saveStop || g_saveQueued != nullptr; });

                if (!g_saveQueued)
                    return 0;   // stopping, nothing left to write

                snapshot = std::move(g_saveQueued);
            }

            bool written = false;

            try
            {
                std::lock_guard file(g_saveFileMutex);
                written = WriteSnapshot(*snapshot);
            }
            catch (...)
            {
                written = false;
            }

            if (!written)
                g_saveFailed.store(true, std::memory_order_release);
        }
    }

    // call with g_mutex held: written right away (unloading)
    void SaveLocked()
    {
        const SaveSnapshot snapshot = SnapshotLocked();
        std::lock_guard file(g_saveFileMutex);

        if (WriteSnapshot(snapshot))
            g_dirty = false;
    }

    // call with g_mutex held (the game's main thread): only the copy here, the writing on the save thread
    void QueueSaveLocked()
    {
        auto snapshot = std::make_unique<SaveSnapshot>(SnapshotLocked());
        bool threadRunning = false;
        {
            std::lock_guard lock(g_saveQueueMutex);

            if (!g_saveStop)
            {
                if (!g_saveThread)
                    g_saveThread = CreateThread(nullptr, 0, &SaveThreadMain, nullptr, 0, nullptr);

                if (g_saveThread)
                {
                    g_saveQueued = std::move(snapshot);
                    threadRunning = true;
                }
            }
        }

        if (threadRunning)
        {
            g_dirty = false;
            g_saveWake.notify_one();
            return;
        }

        // no thread (it couldn't be made, or Lootline is unloading): written here, as it used to be
        std::lock_guard file(g_saveFileMutex);

        if (WriteSnapshot(*snapshot))
            g_dirty = false;
    }

    // Shutdown: what waits is written, then the thread ends (unloading waits for it - its code is about to go)
    void StopSaveThread()
    {
        HANDLE thread = nullptr;
        {
            std::lock_guard lock(g_saveQueueMutex);
            g_saveStop = true;
            thread = g_saveThread;
            g_saveThread = nullptr;
        }

        g_saveWake.notify_all();

        if (thread)
        {
            WaitForSingleObject(thread, INFINITE);
            CloseHandle(thread);
        }
    }

    // Lootline used to be "Unboxer" and kept its items in %USERPROFILE%\.unboxer. An old Unboxer build started after
    // the rename still saves there: when that file is newer than ours, it's the one being used — take it over once
    // (ours is kept as items.before-unboxer.json). The copy keeps the file time, so it doesn't happen again.
    void AdoptNewerUnboxerItems()
    {
        std::error_code ec;
        const std::filesystem::path ours = ItemsPath();
        const std::filesystem::path old = DataFolder().parent_path() / ".unboxer" / "items.json";

        if (!std::filesystem::is_regular_file(old, ec))
            return;

        const auto oldTime = std::filesystem::last_write_time(old, ec);
        if (ec)
            return;

        const bool hadOurs = std::filesystem::exists(ours, ec);

        if (hadOurs)
        {
            const auto ourTime = std::filesystem::last_write_time(ours, ec);
            if (ec || ourTime >= oldTime)
                return;

            std::filesystem::copy_file(ours, DataFolder() / "items.before-unboxer.json", std::filesystem::copy_options::overwrite_existing, ec);
            if (ec)
                return;   // ours stays unless it's kept safe first
        }

        std::filesystem::copy_file(old, ours, std::filesystem::copy_options::overwrite_existing, ec);

        if (ec)
            Log("items: couldn't take over the newer items.json of an old Unboxer build (%%USERPROFILE%%\\.unboxer)");
        else
            Log("items: took over the newer items.json of an old Unboxer build (%%USERPROFILE%%\\.unboxer)%s",
                hadOurs ? "; Lootline's previous one is items.before-unboxer.json" : "");
    }

    // items.json couldn't be read (all or some of it): a copy next to it, items.unreadable-<unix time>.json, before a save
    // writes over it. Returns the copy's name ("(not copied)" if that failed too).
    std::string KeepUnreadableItemsFile()
    {
        const std::string name = "items.unreadable-" + std::to_string(static_cast<long long>(std::time(nullptr))) + ".json";
        std::error_code ec;
        std::filesystem::copy_file(ItemsPath(), DataFolder() / name, std::filesystem::copy_options::overwrite_existing, ec);
        return ec ? std::string("(not copied)") : name;
    }

    void Load()
    {
        using json = nlohmann::json;
        AdoptNewerUnboxerItems();

        // An id is never used twice, also across loads: CS2 keeps an item's picture by its id for as long as the game runs,
        // so an item made with a deleted one's id (Lootline reloaded) showed the old picture for a moment while scrolling.
        // New ids start above the clock (2^12 per second: below 2^53, JS-safe) and above the counter items.json keeps.
        g_nextId = std::max(g_nextId, ID_BASE + (static_cast<std::uint64_t>(std::time(nullptr)) << 12));

        std::ifstream file(ItemsPath());

        if (!file)
            return;

        try
        {
            const json root = json::parse(file);

            int skipped = 0;   // entries that couldn't be read (a wrong type in one): only those are left out

            for (const json& entry : root.value("items", json::array()))
            {
                try
                {
                    Unboxer::Item item;
                    item.id = entry.value("id", 0ull);
                    item.kind = static_cast<Unboxer::Kind>(std::clamp(entry.value("kind", 0), 0, 2));
                    item.definition = entry.value("def", static_cast<std::uint16_t>(0));
                    item.paintKit = entry.value("paint", 0);
                    item.seed = entry.value("seed", 0);
                    item.wear = entry.value("wear", 0.0f);
                    item.statTrak = entry.value("stattrak", false);
                    item.rarity = entry.value("rarity", 0);
                    item.quality = entry.value("quality", QUALITY_UNIQUE);
                    item.fromCase = entry.value("case", static_cast<std::uint16_t>(0));
                    item.equipped = static_cast<std::uint8_t>(entry.value("equip", 0) & Unboxer::EQUIP_BOTH);
                    item.slotT = entry.value("slot_t", std::string());
                    item.slotCT = entry.value("slot_ct", std::string());

                    item.kills = entry.value("kills", -1);
                    item.customName = entry.value("name", std::string());
                    item.charm = entry.value("charm", 0);
                    item.charmSeed = entry.value("charm_seed", 0);
                    item.charmX = entry.value("charm_x", 0.0f);
                    item.charmY = entry.value("charm_y", 0.0f);
                    item.charmZ = entry.value("charm_z", 0.0f);
                    item.charmSticker = entry.value("charm_sticker", 0);
                    item.highlight = std::max(0, entry.value("highlight", 0));
                    item.tint = std::clamp(entry.value("tint", 0), 0, Cases::TINT_COUNT);
                    item.sprays = std::clamp(entry.value("sprays", 0), 0, Unboxer::SPRAY_CHARGES);

                    // a Sticker Slab charm with nothing sealed in it (added before Lootline knew slabs): CS2's Sticker
                    // Slab tool, which seals a sticker in ("Seal in Sticker Slab" on a sticker)
                    if (item.definition == Unboxer::DEF_CHARM && item.paintKit == Unboxer::KEYCHAIN_STICKER_SLAB && item.charmSticker <= 0)
                    {
                        item.definition = Unboxer::DEF_STICKER_SLAB;
                        item.paintKit = 0;
                        item.charmSeed = 0;
                        g_dirty = true;
                    }

                    item.petId = entry.value("pet_id", 0);
                    item.petSeed = entry.value("pet_seed", 0);
                    item.petLevel = entry.value("pet_level", 0);
                    item.petHatch = entry.value("pet_hatch", 0u);

                    if (item.definition == Unboxer::DEF_PET && item.petHatch == 0)
                    {
                        item.petHatch = static_cast<std::uint32_t>(std::time(nullptr));   // saved before hatch dates: from now on
                        g_dirty = true;
                    }
                    item.expires = entry.value("expires", 0u);
                    item.offerNumber = entry.value("offer", 0);
                    item.casket = entry.value("casket", 0ull);
                    item.modified = entry.value("modified", 0u);

                    if (const auto colour = entry.find("colour"); colour != entry.end() && colour->is_object())
                    {
                        item.paint.on = colour->value("on", false);
                        item.paint.mode = std::clamp(colour->value("mode", 0), 0, CustomColor::MODE_COUNT - 1);
                        item.paint.primary = colour->value("primary", item.paint.primary);
                        item.paint.secondary = colour->value("secondary", item.paint.secondary);
                    }

                    if (const auto stickers = entry.find("stickers"); stickers != entry.end() && stickers->is_array())
                    {
                        for (const json& sticker : *stickers)
                        {
                            if (!sticker.is_object())
                                continue;

                            const int slot = sticker.value("slot", -1);
                            if (slot < 0 || slot >= Unboxer::STICKER_SLOTS)
                                continue;

                            item.stickers[slot] = { sticker.value("kit", 0), sticker.value("wear", 0.0f), sticker.value("rotation", 0.0f),
                                sticker.value("x", 0.0f), sticker.value("y", 0.0f), sticker.value("schema", -1) };
                        }
                    }

                    if (item.id < ID_BASE || item.definition == 0)
                        continue;

                    g_items.push_back(item);
                    g_nextId = std::max(g_nextId, item.id + 1);
                }
                catch (...)
                {
                    ++skipped;
                }
            }

            if (skipped > 0)
            {
                const std::string kept = KeepUnreadableItemsFile();
                Log("items.json: %d item(s) couldn't be read and were left out - the file as it was is kept as %s", skipped, kept.c_str());
            }

            g_nextId = std::max(g_nextId, root.value("next_id", 0ull));
            g_armoryStars = std::clamp(root.value("armory_stars", g_armoryStars), 0, MAX_ARMORY_STARS);
            g_keySpent = std::max(0.0, root.value("key_spent", 0.0));
            g_keysBought = std::max(0, root.value("keys_bought", 0));
            g_charmDetachments = std::clamp(root.value("charm_detachments", 0), 0, 1000000);

            if (const auto favorites = root.find("favorites"); favorites != root.end() && favorites->is_array())
            {
                for (const json& entry : *favorites)
                {
                    if (!entry.is_object())
                        continue;

                    Favorite favorite;
                    favorite.id = entry.value("id", 0ull);
                    favorite.sides = static_cast<std::uint8_t>(entry.value("sides", 0) & Unboxer::EQUIP_BOTH);

                    if (favorite.id >= ID_BASE && favorite.sides != 0)
                        g_favorites.push_back(favorite);
                }
            }

            // Storage Units: what's in each (counted again); an item whose unit is gone is back in the inventory
            for (Unboxer::Item& unit : g_items)
                unit.casketItems = 0;

            for (Unboxer::Item& item : g_items)
            {
                if (item.casket == 0)
                    continue;

                const auto unit = std::find_if(g_items.begin(), g_items.end(), [&](const Unboxer::Item& candidate)
                {
                    return candidate.id == item.casket && candidate.definition == Unboxer::DEF_CASKET && candidate.modified != 0;
                });

                if (unit == g_items.end())
                {
                    item.casket = 0;
                    g_dirty = true;
                    continue;
                }

                ++unit->casketItems;
                item.equipped = 0;   // stored items aren't equipped
                item.slotT.clear();
                item.slotCT.clear();
            }

            if (const auto customOdds = root.find("custom_odds"); customOdds != root.end() && customOdds->is_array())
            {
                for (const json& entry : *customOdds)
                {
                    const auto values = entry.is_object() ? entry.find("odds") : entry.end();
                    const std::uint16_t container = entry.is_object() ? entry.value("case", static_cast<std::uint16_t>(0)) : 0;

                    if (container == 0 || values == entry.end() || !values->is_array() || values->size() != std::tuple_size_v<Unboxer::TierOdds>)
                        continue;

                    Unboxer::TierOdds odds{};
                    for (std::size_t tier = 0; tier < odds.size(); ++tier)
                        odds[tier] = (*values)[tier].is_number() ? std::clamp((*values)[tier].get<double>(), 0.0, 100.0) : 0.0;

                    g_customOdds[container] = odds;
                }
            }

            if (const auto loadouts = root.find("loadouts"); loadouts != root.end() && loadouts->is_array())
            {
                for (const json& entry : *loadouts)
                {
                    if (!entry.is_object())
                        continue;

                    LoadoutProfile profile;
                    profile.name = entry.value("name", std::string());

                    if (const auto entries = entry.find("items"); entries != entry.end() && entries->is_array())
                    {
                        for (const json& saved : *entries)
                        {
                            if (!saved.is_object())
                                continue;

                            const std::uint64_t id = saved.value("id", 0ull);
                            const auto sides = static_cast<std::uint8_t>(saved.value("sides", 0) & Unboxer::EQUIP_BOTH);

                            if (id >= ID_BASE && sides != 0)
                                profile.items.emplace_back(id, sides);
                        }
                    }

                    if (!profile.name.empty() && !profile.items.empty())
                        g_loadouts.push_back(std::move(profile));
                }
            }

            if (const auto upgrader = root.find("upgrader"); upgrader != root.end() && upgrader->is_object())
            {
                g_upgradeStats.tries = upgrader->value("tries", 0);
                g_upgradeStats.wins = upgrader->value("wins", 0);
                g_upgradeStats.losses = upgrader->value("losses", 0);
                g_upgradeStats.bestMultiplier = upgrader->value("best_multiplier", 0.0);
                g_upgradeStats.bestName = upgrader->value("best_name", std::string());
                g_upgradeStats.streak = upgrader->value("streak", 0);
            }
        }
        catch (...)
        {
            // the next save writes items.json again: a copy of the unreadable one first, or the inventory would be gone
            const std::string kept = KeepUnreadableItemsFile();
            Log("items.json couldn't be read - starting empty; the file as it was is kept as %s", kept.c_str());
            g_items.clear();
        }
    }

    // A charm's clip: the one on the item, else the charm's own (a Cologne 2026 highlight charm), 0 = none.
    int CharmHighlight(int keychain, int highlight)
    {
        return highlight > 0 ? highlight : Cases::HighlightOf(keychain);
    }

    GameInv::NewItem ToNewItem(const Unboxer::Item& item)
    {
        GameInv::NewItem out;
        out.id = item.id;
        out.definition = item.definition;
        out.quality = item.quality;
        out.rarity = item.rarity;
        out.origin = item.kind == Unboxer::Kind::Drop ? ORIGIN_FOUND_IN_CRATE : 0;
        out.paintKit = item.paintKit;
        out.seed = item.seed;
        out.wear = item.wear;
        out.statTrak = item.statTrak;
        out.statTrakKills = 0;

        for (int slot = 0; slot < Unboxer::STICKER_SLOTS; ++slot)
        {
            const Unboxer::Sticker& sticker = item.stickers[slot];
            out.stickers[slot] = { sticker.kit, sticker.wear, sticker.rotation, sticker.offsetX, sticker.offsetY, sticker.schema };
        }

        out.statTrakKills = Unboxer::ShownKills(item);   // your real kills so far too (they were 0 in CS2's inventory)
        out.customName = item.customName;

        if (item.kind == Unboxer::Kind::Case && item.quality == QUALITY_VOLATILE)
        {
            out.expirationDate = item.expires;   // an unsealed terminal
            out.offerNumber = item.offerNumber;
        }

        out.casketId = item.casket;   // in a Lootline Storage Unit (CS2 lists it inside, not in the inventory)

        if (item.definition == Unboxer::DEF_CASKET && item.modified != 0)
        {
            out.modificationDate = item.modified;   // a named Storage Unit
            out.itemsCount = item.casketItems;
        }

        if (item.definition == Unboxer::DEF_CHARM_DETACH_PACK)
            out.itemsCount = Unboxer::CHARM_DETACHMENTS_PER_PACK;   // CS2's "use" button says how many it gives

        // charms and chickens keep their value in fields of their own: never as a paint kit
        if (item.definition == Unboxer::DEF_CHARM)
        {
            out.keychain = item.paintKit;
            out.keychainSeed = item.charmSeed;
            out.keychainSticker = item.charmSticker;
            out.keychainHighlight = CharmHighlight(item.paintKit, item.highlight);
            out.paintKit = 0;
            return out;
        }

        if (item.definition == Unboxer::DEF_PET)
        {
            out.petId = item.petId;
            out.petSeed = item.petSeed;
            out.petLevel = item.petLevel;
            out.petHatch = item.petHatch;
            out.paintKit = 0;
            return out;
        }

        if (item.charm > 0)
        {
            out.keychain = item.charm;
            out.keychainSeed = item.charmSeed;
            out.keychainX = item.charmX;
            out.keychainY = item.charmY;
            out.keychainZ = item.charmZ;
            out.keychainSticker = item.charmSticker;
            out.keychainHighlight = CharmHighlight(item.charm, item.highlight);
        }

        // stickers / patches / graffiti / music kits keep their kit id in paintKit's place
        switch (Cases::KindOf(item.definition))
        {
        case Cases::DropKind::Sticker:
        case Cases::DropKind::Patch:
        case Cases::DropKind::Graffiti:
            out.stickerKit = item.paintKit;
            out.sprayTint = item.tint;
            out.paintKit = 0;

            if (item.definition == Cases::DEF_SPRAY_PAINT)   // unsealed: its charges ("Charges Remaining" in CS2)
                out.sprayCharges = item.sprays > 0 ? item.sprays : Unboxer::SPRAY_CHARGES;
            break;
        case Cases::DropKind::MusicKit:
            out.musicKit = item.paintKit;
            out.paintKit = 0;
            out.statTrakType = 1;   // StatTrak music kits count MVPs
            break;
        case Cases::DropKind::Pin:
            out.paintKit = 0;
            break;
        default:
            break;
        }

        return out;
    }

    // ── rolling a drop (game thread, g_mutex held) ──
    double Random01()
    {
        return std::uniform_real_distribution<double>(0.0, 1.0)(g_rng);
    }

    // Wear the way cases hand it out: FN 3 %, MW 24 %, FT 33 %, WW 24 %, BS 16 %.
    float RandomWear()
    {
        struct Range { double chance; float min; float max; };
        static constexpr Range ranges[] = {
            { 0.03, 0.00f, 0.07f }, { 0.24, 0.07f, 0.15f }, { 0.33, 0.15f, 0.38f }, { 0.24, 0.38f, 0.45f }, { 0.16, 0.45f, 1.00f },
        };

        double pick = Random01();

        for (const Range& range : ranges)
        {
            if (pick < range.chance)
                return range.min + static_cast<float>(Random01()) * (range.max - range.min);

            pick -= range.chance;
        }

        return 0.25f;
    }

    bool IsGloves(std::uint16_t definition)
    {
        const ItemDefinitionInfo* info = Skins::g_ItemSchema.GetItem(definition);
        return info && info->isGlove;
    }

    bool IsKnife(std::uint16_t definition)
    {
        const ItemDefinitionInfo* info = Skins::g_ItemSchema.GetItem(definition);
        return info && info->isKnife;
    }

    bool IsAgent(std::uint16_t definition)
    {
        const ItemDefinitionInfo* info = Skins::g_ItemSchema.GetItem(definition);
        return info && info->isAgent;
    }

    bool IsGun(std::uint16_t definition)
    {
        const ItemDefinitionInfo* info = Skins::g_ItemSchema.GetItem(definition);
        return info && info->isWeapon;
    }

    // What a charm can hang on: a gun, or the C4.
    bool CanHoldCharm(std::uint16_t definition)
    {
        return definition == Unboxer::DEF_C4 || IsGun(definition);
    }

    // What stickers go on: a gun, or the C4.
    bool CanHoldSticker(std::uint16_t definition)
    {
        return definition == Unboxer::DEF_C4 || IsGun(definition);
    }

    // Puts a Lootline copy of one of your own weapons where the original is in CS2's loadout (each side where that
    // slot holds it). CS2's own LoadoutAPI; EquipItemInSlot goes through the loadout hook (OnLoadoutEquip). Reports back
    // through UseTool('unboxer:copy', ...).
    constexpr const char* EQUIP_LIKE_ORIGINAL_SCRIPT = R"JS((function(){
var original='%s',copy='%s',def=%d,r=[];
try{['t','ct'].forEach(function(team){var slot=LoadoutAPI.GetSlotEquippedWithDefIndex(team,def);
if(slot&&LoadoutAPI.GetItemID(team,slot)===original){r.push(team+' '+slot+' '+LoadoutAPI.EquipItemInSlot(team,copy,slot));}});}
catch(e){r.push('error: '+e);}
InventoryAPI.UseTool('unboxer:copy',r.length?r.join(', '):'the original is not equipped');
})();)JS";

    // The side an agent belongs to (EQUIP_T / EQUIP_CT), 0 if it isn't one.
    std::uint8_t AgentSide(std::uint16_t definition)
    {
        const ItemDefinitionInfo* info = Skins::g_ItemSchema.GetItem(definition);

        if (!info || !info->isAgent)
            return 0;

        return info->isCounterTerroristAgent ? Unboxer::EQUIP_CT : info->isTerroristAgent ? Unboxer::EQUIP_T : 0;
    }

    // Cases: Choose counts openings (the picked item on number dropOn). Never: a plain roll (a terminal's other offers).
    // Now: the picked item (the terminal offer it's meant for).
    enum class PickMode { Cases, Never, Now };

    // Cases tab "Custom odds" (g_mutex held): a grade by the container's own chances - only the grades it has (★ only when
    // it can drop one), together scaled to 100 %. False = none set / all 0: the real odds.
    bool RollCustomTierLocked(const Cases::Case& opened, Cases::Tier& out)
    {
        const auto custom = g_customOdds.find(opened.definition);

        if (custom == g_customOdds.end())
            return false;

        bool present[Cases::TierCount]{};
        for (const Cases::Drop& drop : opened.drops)
            present[drop.tier] = true;
        present[Cases::TierGold] = opened.hasGold;

        const Unboxer::TierOdds& odds = custom->second;
        double total = 0.0;
        int last = -1;

        for (int tier = 0; tier < Cases::TierCount; ++tier)
        {
            if (present[tier] && odds[tier] > 0.0)
            {
                total += odds[tier];
                last = tier;
            }
        }

        if (total <= 0.0 || last < 0)
            return false;

        double pick = Random01() * total;

        for (int tier = 0; tier < Cases::TierCount; ++tier)
        {
            if (!present[tier] || odds[tier] <= 0.0)
                continue;

            if (pick < odds[tier])
            {
                out = static_cast<Cases::Tier>(tier);
                return true;
            }

            pick -= odds[tier];
        }

        out = static_cast<Cases::Tier>(last);   // rounding at the very end
        return true;
    }

    Unboxer::Item RollDropLocked(const Cases::Case* opened, PickMode mode = PickMode::Cases)
    {
        Unboxer::Item drop;
        drop.kind = Unboxer::Kind::Drop;
        drop.fromCase = opened ? opened->definition : 0;
        Cases::Tier tier = Cases::TierMilSpec;

        // Choose: the picked item on opening number dropOn, random rolls before it (a container whose contents aren't
        // known can't roll anything: it gives the picked item right away)
        const bool canRoll = opened && !opened->drops.empty();
        bool givePicked = false;

        if (!g_drop.random && g_drop.weapon != 0)
        {
            if (mode == PickMode::Now)
            {
                givePicked = true;
            }
            else if (mode == PickMode::Cases)
            {
                ++g_pickedOpenings;
                givePicked = g_pickedOpenings >= std::max(g_drop.dropOn, 1) || !canRoll;
            }
        }

        if (givePicked)
        {
            if (mode == PickMode::Cases)
                g_pickedOpenings = 0;

            drop.definition = g_drop.weapon;
            drop.paintKit = g_drop.paintKit;
            drop.wear = std::clamp(g_drop.wear, 0.0f, 1.0f);
            drop.seed = std::clamp(g_drop.seed, 0, 1000);

            if (g_drop.randomFloat)
            {
                const float from = std::clamp(g_drop.floatFrom, 0.0f, 1.0f);
                const float to = std::clamp(g_drop.floatTo, from, 1.0f);
                drop.wear = std::clamp(from + static_cast<float>(Random01()) * (to - from), from, to);
            }

            if (g_drop.randomSeed)
                drop.seed = static_cast<int>(Random01() * 1001.0) % 1001;
            drop.statTrak = g_drop.statTrak;
            tier = static_cast<Cases::Tier>(std::clamp(g_drop.tier, 0, static_cast<int>(Cases::TierGold)));

            if (!g_drop.keepPicked)
            {
                g_drop.random = true;   // only once: back to Random
                ++g_drop.revision;
                Log("choose: your item came (only once) - back to Random");
            }
        }
        else if (canRoll)
        {
            if (!RollCustomTierLocked(*opened, tier))
                tier = Cases::RollTier(*opened, Random01());

            // ★: the case's own knives / gloves (data\case_rares.txt), else the types it names (Kilowatt: Kukri), else any
            std::vector<std::pair<std::uint16_t, int>> golds;
            if (tier == Cases::TierGold)
            {
                if (!opened->golds.empty())
                {
                    for (const Cases::Drop& gold : opened->golds)
                        golds.emplace_back(gold.weapon, gold.paintKit);
                }
                else
                {
                    for (const auto& entry : g_goldPool)
                        if (opened->goldTypes.empty() || Cases::GoldTypeFrom(*opened, entry.first))
                            golds.push_back(entry);
                }
            }

            if (!golds.empty())
            {
                const auto [definition, paintKit] = golds[static_cast<std::size_t>(Random01() * golds.size()) % golds.size()];
                drop.definition = definition;
                drop.paintKit = paintKit;
            }
            else
            {
                if (tier == Cases::TierGold)
                    tier = Cases::TierCovert;   // the ★ list isn't built yet: the next best thing

                std::vector<const Cases::Drop*> candidates;
                for (const Cases::Drop& candidate : opened->drops)
                    if (candidate.tier == tier)
                        candidates.push_back(&candidate);

                if (candidates.empty())
                    for (const Cases::Drop& candidate : opened->drops)
                        candidates.push_back(&candidate);

                const Cases::Drop* chosen = candidates[static_cast<std::size_t>(Random01() * candidates.size()) % candidates.size()];
                drop.definition = chosen->weapon;
                drop.paintKit = chosen->paintKit;
                tier = chosen->tier;
            }

            // a skin's wear roll put into its finish's float range, like CS2's (an Asiimov is never Factory New, a
            // Doppler never more than 0.08); stickers / graffiti / music kits keep theirs (their paintKit is a kit id)
            const Cases::WearRange range = Cases::KindOf(drop.definition) == Cases::DropKind::Skin ? Cases::WearRangeOf(drop.paintKit) : Cases::WearRange{};
            drop.wear = std::clamp(range.min + RandomWear() * (range.max - range.min), range.min, range.max);
            drop.seed = static_cast<int>(Random01() * 1001.0) % 1001;

            const Cases::DropKind kind = Cases::KindOf(drop.definition);
            const bool souvenirPackage = opened->category == Cases::Category::Souvenir;   // Souvenirs, never StatTrak
            drop.statTrak = kind == Cases::DropKind::Skin ? !souvenirPackage && Random01() < 0.10 :
                (kind == Cases::DropKind::MusicKit && opened->statTrakOnly);
        }
        else
        {
            return drop;   // definition 0: nothing to give
        }

        const Cases::DropKind kind = Cases::KindOf(drop.definition);
        const bool gold = kind == Cases::DropKind::Skin && (tier == Cases::TierGold || IsKnife(drop.definition) || IsGloves(drop.definition));

        if (IsGloves(drop.definition) || (kind != Cases::DropKind::Skin && kind != Cases::DropKind::MusicKit))
            drop.statTrak = false;   // gloves, stickers, patches, graffiti and pins have no StatTrak

        drop.rarity = Cases::Rarity(gold ? Cases::TierGold : tier);
        drop.quality = gold ? QUALITY_UNUSUAL : (drop.statTrak ? QUALITY_STRANGE : QUALITY_UNIQUE);

        // a souvenir package's skin is a Souvenir like a real one: never StatTrak (a picked StatTrak one stays as picked),
        // with the match's gold stickers - the event's logo, the two teams, the MVP's autograph
        if (opened && opened->category == Cases::Category::Souvenir && kind == Cases::DropKind::Skin && !gold && !drop.statTrak)
        {
            drop.quality = QUALITY_SOUVENIR;
            int kits[4]{};

            if (Cases::RollSouvenirStickers(opened->tournamentEvent, Random01(), Random01(), Random01(), kits))
                for (int slot = 0; slot < 4; ++slot)
                    if (kits[slot] > 0)
                        drop.stickers[slot] = { kits[slot], 0.0f, 0.0f, 0.0f, 0.0f, -1 };
        }

        // a graffiti that comes in colours comes in a random one (like a Graffiti Box)
        if (kind == Cases::DropKind::Graffiti)
            if (const Cases::Kit* kit = Cases::FindKit(kind, drop.paintKit); kit && kit->tintable)
                drop.tint = 1 + static_cast<int>(Random01() * Cases::TINT_COUNT) % Cases::TINT_COUNT;

        return drop;
    }

    Unboxer::Item* FindLocked(std::uint64_t id)
    {
        for (Unboxer::Item& item : g_items)
        {
            if (item.id == id)
                return &item;
        }

        return nullptr;
    }

    void EraseLocked(std::uint64_t id)
    {
        g_items.erase(std::remove_if(g_items.begin(), g_items.end(), [id](const Unboxer::Item& item) { return item.id == id; }), g_items.end());
        g_loadoutDirty = true;
    }

    // ── game thread work ──

    void RestoreSaved()
    {
        std::vector<Unboxer::Item> items;
        {
            std::lock_guard lock(g_mutex);
            items = g_items;
        }

        int added = 0, present = 0, failed = 0;

        for (const Unboxer::Item& item : items)
        {
            if (GameInv::HasItem(item.id))
                ++present;
            else if (GameInv::CreateItem(ToNewItem(item)))
                ++added;
            else
                ++failed;
        }

        Log("inventory ready: %d saved items put back, %d already there, %d failed; attribute types: %s", added, present, failed,
            GameInv::AttributeTypes().c_str());
    }

    // Creates `count` items of one kind. Returns how many were made.
    // Added from the Lootline menu: arrives NEW (the inventory tab's count, CS2's new item popup) while there's room.
    // allowed = false: the addition it's part of is too big to be NEW (Request::asNew).
    GameInv::NewItem AsAdded(GameInv::NewItem item, bool allowed = true)
    {
        if (allowed && g_newItemsEnabled.load(std::memory_order_acquire) && !g_unloadAcknowledge.load(std::memory_order_acquire) &&
            GameInv::NewItemCount() < MAX_NEW_ITEMS)
            item.pickup = GameInv::PICKUP_PURCHASED;

        return item;
    }

    int CreateMany(Unboxer::Kind kind, std::uint16_t definition, int count, std::uint64_t* firstId = nullptr, bool asNew = true)
    {
        int made = 0;

        for (int i = 0; i < count; ++i)
        {
            Unboxer::Item item;
            item.kind = kind;
            item.definition = definition;
            item.rarity = 0;               // Base Grade, like real cases / keys
            item.quality = QUALITY_UNIQUE;

            {
                std::lock_guard lock(g_mutex);
                item.id = g_nextId++;
            }

            if (!GameInv::CreateItem(AsAdded(ToNewItem(item), asNew)))
            {
                Log("couldn't create item def %u (id %llu)", definition, static_cast<unsigned long long>(item.id));
                break;
            }

            std::lock_guard lock(g_mutex);
            g_items.push_back(item);
            g_dirty = true;

            if (made == 0 && firstId)
                *firstId = item.id;

            ++made;
        }

        return made;
    }

    void ProcessRequests()
    {
        std::vector<Request> requests;
        {
            std::lock_guard lock(g_mutex);
            requests.swap(g_requests);
        }

        // Big additions are spread over frames (every new item is announced to the inventory UI right away).
        int budget = MAX_CREATED_PER_TICK;
        std::size_t next = 0;

        for (; next < requests.size(); ++next)
        {
            Request& request = requests[next];
            const bool adds = request.type == Request::AddCase || request.type == Request::AddKey || request.type == Request::AddItem;

            if (adds && budget <= 0)
                break;   // the rest waits for the next frame

            // NEW (CS2's new item popup) for the whole addition or none of it: part of it NEW made the popup's "N / total"
            // look like a wrong count (25 cases + 25 keys with 2 NEW items already there showed "1 / 28")
            if (adds && request.asNew < 0)
            {
                const Cases::Case* info = request.type == Request::AddCase ? Cases::Find(request.definition) : nullptr;
                const int total = request.count * (request.type == Request::AddCase && request.withKeys && info && info->key != 0 ? 2 : 1);
                request.asNew = GameInv::NewItemCount() + total <= MAX_NEW_ITEMS ? 1 : 0;
            }

            switch (request.type)
            {
            case Request::AddCase:
            {
                const Cases::Case* info = Cases::Find(request.definition);
                const bool keys = request.withKeys && info && info->key != 0;
                const int chunk = std::clamp(keys ? budget / 2 : budget, 1, request.count);

                const int made = CreateMany(Unboxer::Kind::Case, request.definition, chunk, nullptr, request.asNew == 1);
                const int madeKeys = keys ? CreateMany(Unboxer::Kind::Key, info->key, made, nullptr, request.asNew == 1) : 0;
                budget -= made + madeKeys;

                Log("added %d x case %u (+%d keys)%s", made, request.definition, madeKeys, request.asNew == 1 ? "" : " - not NEW (too many at once)");

                request.count -= chunk;
                if (made == chunk && request.count > 0)
                    --next;   // same request again (next frame if the budget is used up)
                break;
            }
            case Request::AddKey:
            {
                const int chunk = std::clamp(budget, 1, request.count);
                const int made = CreateMany(Unboxer::Kind::Key, request.definition, chunk, nullptr, request.asNew == 1);
                budget -= made;

                Log("added %d x key %u%s", made, request.definition, request.asNew == 1 ? "" : " - not NEW (too many at once)");

                request.count -= chunk;
                if (made == chunk && request.count > 0)
                    --next;
                break;
            }
            case Request::AddItem:
            {
                const int chunk = std::clamp(budget, 1, request.count);
                int made = 0;

                for (; made < chunk; ++made)
                {
                    Unboxer::Item item = request.item;
                    item.kind = Unboxer::Kind::Drop;
                    item.equipped = 0;
                    item.slotT.clear();
                    item.slotCT.clear();

                    if (item.definition == Unboxer::DEF_PET && item.petHatch == 0)
                        item.petHatch = static_cast<std::uint32_t>(std::time(nullptr));   // hatched today

                    // a Storage Unit named in Add items: named today, so CS2 offers "put in" / "look inside" right away
                    // (without a "modification date" it asks for a name first)
                    if (item.definition == Unboxer::DEF_CASKET && !item.customName.empty() && item.modified == 0)
                        item.modified = static_cast<std::uint32_t>(std::time(nullptr));

                    {
                        std::lock_guard lock(g_mutex);
                        item.id = g_nextId++;

                        if (request.randomFloat.on)
                        {
                            const Unboxer::RandomFloat& range = request.randomFloat;
                            item.wear = std::clamp(range.from + static_cast<float>(Random01()) * (range.to - range.from), range.from, range.to);
                        }

                        if (request.randomSeed)
                            item.seed = static_cast<int>(Random01() * 1001.0) % 1001;   // CS2's patterns: 0 - 1000
                    }

                    if (!GameInv::CreateItem(AsAdded(ToNewItem(item), request.asNew == 1)))
                    {
                        Log("couldn't add item def %u (paint %d)", item.definition, item.paintKit);
                        break;
                    }

                    std::lock_guard lock(g_mutex);
                    g_items.push_back(std::move(item));
                    g_dirty = true;
                }

                budget -= made;
                Log("added %d x %s%s", made, Unboxer::DisplayName(request.item).c_str(), request.asNew == 1 ? "" : " - not NEW (too many at once)");

                request.count -= chunk;
                if (made == chunk && request.count > 0)
                    --next;
                break;
            }
            case Request::Remove:
            {
                // a Storage Unit: what's in it comes back out first; an item in one: the unit holds one less
                std::uint64_t holder = 0;
                bool unit = false;
                {
                    std::lock_guard lock(g_mutex);
                    if (const Unboxer::Item* item = FindLocked(request.id))
                    {
                        holder = item->casket;
                        unit = item->definition == Unboxer::DEF_CASKET;
                    }
                }

                if (unit)
                    ReleaseCasketContents(request.id);

                GameInv::RemoveItem(request.id);
                int count = 0;
                std::uint32_t date = 0;
                {
                    std::lock_guard lock(g_mutex);
                    EraseLocked(request.id);
                    g_dirty = true;

                    if (Unboxer::Item* holderItem = holder ? FindLocked(holder) : nullptr)
                    {
                        holderItem->casketItems = std::max(0, holderItem->casketItems - 1);
                        count = holderItem->casketItems;
                        date = holderItem->modified;
                    }
                }

                if (holder && date)
                    GameInv::SetCasketInfo(holder, count, date);
                break;
            }
            case Request::RemoveAll:
            {
                std::vector<Unboxer::Item> items;
                {
                    std::lock_guard lock(g_mutex);
                    items.swap(g_items);
                    g_dirty = true;
                }

                std::vector<TerminalOffer> offers;
                {
                    std::lock_guard lock(g_mutex);
                    offers.swap(g_terminalOffers);
                }

                for (const Unboxer::Item& item : items)
                    GameInv::RemoveItem(item.id);

                for (const TerminalOffer& entry : offers)
                    GameInv::RemoveItem(entry.offer.id);   // inside terminals

                Log("removed all (%zu items)", items.size());
                break;
            }
            }
        }

        if (next < requests.size())
        {
            std::lock_guard lock(g_mutex);
            g_requests.insert(g_requests.begin(), requests.begin() + static_cast<std::ptrdiff_t>(next), requests.end());
        }
    }

    // The real message names the item without its wear: "StatTrak™ AK-47 | Redline".
    std::string ChatName(const Unboxer::Item& item, const std::string& name)
    {
        if (item.paintKit <= 0 || Cases::KindOf(item.definition) != Cases::DropKind::Skin)
            return name;

        const std::size_t wear = name.rfind(" (");
        return wear != std::string::npos ? name.substr(0, wear) : name;
    }

    void FinishOpenings()
    {
        const ULONGLONG now = GetTickCount64();
        std::vector<PendingOpen> due;
        {
            std::lock_guard lock(g_mutex);

            for (auto it = g_pending.begin(); it != g_pending.end();)
            {
                if (now >= it->at)
                {
                    due.push_back(*it);
                    it = g_pending.erase(it);
                }
                else
                {
                    ++it;
                }
            }
        }

        for (PendingOpen& open : due)
        {
            {
                std::lock_guard lock(g_mutex);
                open.drop.id = g_nextId++;
            }

            if (!GameInv::CreateItem(ToNewItem(open.drop)))
            {
                Log("opening FAILED: couldn't create the drop (def %u, paint %d)", open.drop.definition, open.drop.paintKit);
                continue;
            }

            // Take the key and the case (ours only) — the Game Coordinator does the same when you open a real case.
            if (open.keyId)
                GameInv::RemoveItem(open.keyId);
            if (open.caseId)
                GameInv::RemoveItem(open.caseId);

            const std::string name = Unboxer::DisplayName(open.drop);

            {
                std::lock_guard lock(g_mutex);
                if (open.keyId)
                    EraseLocked(open.keyId);
                if (open.caseId)
                    EraseLocked(open.caseId);
                g_items.push_back(open.drop);
                g_dirty = true;
                ++g_status.opened;
                g_status.lastDrop = name;
            }

            char idText[32];
            snprintf(idText, sizeof(idText), "%llu", static_cast<unsigned long long>(open.drop.id));

            char openedText[32];
            snprintf(openedText, sizeof(openedText), "%llu", static_cast<unsigned long long>(open.opened));

            const bool mainThread = Unboxer::OnMainThread();
            {
                std::lock_guard lock(g_mutex);
                g_scriptReports.clear();
            }

            // 1) the notification, the way the Game Coordinator's answer arrives (a probe listener watches it)
            // (the probe listener around the send - PROBE_ON / OFF_SCRIPT - only proved the event arrives; it was two more
            // scripts compiled per opening, so it's off: turn it on again only to debug the reveal)
            bool handled = false;
            const bool dispatched = NotifyItemCustomization(NOTIFICATION_UNLOCK_CRATE, "crate_unlock", idText, handled);

            // 2) the result written into the popup itself (doesn't depend on the event)
            bool direct = false;
            if (mainThread)
            {
                char script[4096];
                const int written = snprintf(script, sizeof(script), DIRECT_RESULT_SCRIPT, idText, openedText);
                direct = written > 0 && static_cast<std::size_t>(written) < sizeof(script) && RunMainMenuScript(script);
            }

            std::string reports;
            {
                std::lock_guard lock(g_mutex);
                for (const std::string& report : g_scriptReports)
                    reports += "\n             " + report;
                g_scriptReports.clear();
            }

            const Cases::Case* opened = Cases::Find(open.drop.fromCase);

            Log("opened %s (case %llu, key %llu): %s  def %u paint %d  id %llu  wear %.4f seed %d  -> event %s (handled %d), "
                "direct %s, main thread %s%s",
                opened ? opened->displayName.c_str() : "a case", static_cast<unsigned long long>(open.opened),
                static_cast<unsigned long long>(open.keyId), name.c_str(), open.drop.definition, open.drop.paintKit,
                static_cast<unsigned long long>(open.drop.id), open.drop.wear, open.drop.seed, dispatched ? "sent" : "NOT sent",
                handled ? 1 : 0, direct ? "ran" : "NOT run", mainThread ? "yes" : "NO",
                reports.empty() ? "\n             (no script reported back)" : reports.c_str());

            if (g_chatDrops.load(std::memory_order_relaxed))
            {
                std::lock_guard lock(g_mutex);
                g_chatLines.push_back({ GetTickCount64() + CHAT_LINE_DELAY_MS, ChatName(open.drop, name), open.drop.rarity });
            }
        }
    }

    // GameTick: the drop messages whose reel has stopped, into your chat. Only in a match: the main menu has no game
    // chat (the line is dropped then).
    void PrintChatLines()
    {
        std::vector<ChatLine> due;
        {
            std::lock_guard lock(g_mutex);
            const ULONGLONG now = GetTickCount64();

            for (auto it = g_chatLines.begin(); it != g_chatLines.end();)
            {
                if (now >= it->at)
                {
                    due.push_back(std::move(*it));
                    it = g_chatLines.erase(it);
                }
                else
                {
                    ++it;
                }
            }
        }

        if (due.empty())
            return;

        const bool inMatch = I::EngineClient && I::EngineClient->in_game() && Unboxer::OnMainThread();
        const std::string player = inMatch ? GameChat::LocalPlayerName() : std::string();

        for (const ChatLine& line : due)
        {
            if (player.empty())
            {
                Log("chat: \"%s\" not printed (%s)", line.item.c_str(), inMatch ? "no local player" : "not in a match");
                continue;
            }

            const bool printed = GameChat::Print(line.crafted ? GameChat::CraftedLine(player, line.item, line.rarity) :
                GameChat::FoundInCrateLine(player, line.item, line.rarity));
            Log("chat: \"%s\" %s", line.item.c_str(), printed ? "printed" : "NOT printed (chat function not found)");
        }
    }
}

namespace
{
    // The first slot without a sticker on a gun, -1 if all are taken.
    int FirstFreeStickerSlot(const Unboxer::Item& gun)
    {
        for (int i = 0; i < Unboxer::STICKER_SLOTS; ++i)
            if (gun.stickers[i].kit <= 0)
                return i;

        return -1;
    }

    // ── Lootline tools on your own items ──
    // Valve's server refuses a Lootline sticker / patch / name tag / charm on a real or stock item. That item gets a
    // Lootline copy instead - as CS2 shows it (finish, wear, pattern, StatTrak / Souvenir, name, stickers) plus what the
    // tool puts on - put in the loadout wherever the original is. The original isn't touched.

    // The copy, as CS2 shows the original (guns, the C4, knives, agents). False if the game doesn't know it / anything else.
    bool CopyOfOriginal(std::uint64_t originalId, Unboxer::Item& copy)
    {
        GameInv::ItemSnapshot original;

        if (!GameInv::ReadItem(originalId, original))
            return false;

        const std::uint16_t definition = original.definition;

        if (!CanHoldSticker(definition) && !IsKnife(definition) && !IsAgent(definition))
            return false;

        copy = Unboxer::Item{};
        copy.kind = Unboxer::Kind::Drop;
        copy.definition = definition;
        copy.paintKit = original.paintKit;
        copy.seed = original.seed;
        copy.wear = original.wear;
        copy.quality = original.quality;               // stock / unique / StatTrak / Souvenir / ★, as it was
        copy.statTrak = original.quality == QUALITY_STRANGE;
        copy.kills = -1;                               // StatTrak: your kills count up in a match
        copy.customName = original.customName;

        if (const PaintKitInfo* paint = original.paintKit > 0 ? Skins::g_ItemSchema.GetPaintKit(original.paintKit) : nullptr)
            copy.rarity = std::clamp(paint->rarity, 0, 7);

        for (int slot = 0; slot < Unboxer::STICKER_SLOTS && slot < GameInv::STICKER_SLOTS; ++slot)
        {
            const GameInv::StickerSlot& sticker = original.stickers[slot];
            copy.stickers[slot] = { sticker.kit, sticker.wear, sticker.rotation, sticker.offsetX, sticker.offsetY, sticker.schema };
        }

        return true;
    }

    // Game thread: the copy into your inventory (our tool used up), into the loadout where the original is, CS2's popup
    // told (its own answer type, with the copy's id: it shows the "new item" for it).
    void FinishCopy(std::uint64_t originalId, Unboxer::Item copy, std::uint64_t toolId, int notification, const char* notificationName,
        const char* what)
    {
        bool usedUp = false;
        {
            std::lock_guard lock(g_mutex);
            copy.id = g_nextId++;
            usedUp = toolId != 0 && FindLocked(toolId) != nullptr;   // ours only (a faux name tag isn't an item)
        }

        if (!GameInv::CreateItem(ToNewItem(copy)))
        {
            Log("%s: the copy of %llu couldn't be made", what, static_cast<unsigned long long>(originalId));
            return;
        }

        if (usedUp)
            GameInv::RemoveItem(toolId);

        {
            std::lock_guard lock(g_mutex);

            if (usedUp)
                EraseLocked(toolId);

            g_items.push_back(copy);
            g_dirty = true;
            g_scriptReports.clear();
        }

        // into the loadout where the original is
        char originalText[32];
        char copyText[32];
        snprintf(originalText, sizeof(originalText), "%llu", static_cast<unsigned long long>(originalId));
        snprintf(copyText, sizeof(copyText), "%llu", static_cast<unsigned long long>(copy.id));

        char script[1024];
        const int written = snprintf(script, sizeof(script), EQUIP_LIKE_ORIGINAL_SCRIPT, originalText, copyText, static_cast<int>(copy.definition));
        const bool ran = written > 0 && static_cast<std::size_t>(written) < sizeof(script) && Unboxer::OnMainThread() && RunMainMenuScript(script);

        std::string loadout;
        {
            std::lock_guard lock(g_mutex);
            for (const std::string& report : g_scriptReports)
                loadout += report;
            g_scriptReports.clear();
        }

        bool handled = false;
        const bool notified = NotifyItemCustomization(notification, notificationName, copyText, handled);

        Log("%s on your %s (%llu) - a Lootline copy (%llu) has it; loadout: %s; popup %s", what, Unboxer::DisplayName(copy).c_str(),
            static_cast<unsigned long long>(originalId), static_cast<unsigned long long>(copy.id),
            ran ? (loadout.empty() ? "no answer" : loadout.c_str()) : "script NOT run", notified ? "told" : "NOT told");
    }

    // UseTool(sticker, gun) / UseTool(patch, agent) from CS2's popups, once it's placed (main thread, g_mutex NOT held:
    // reading a real sticker goes through the game). Ours: any sticker / patch on one of our guns, our C4 or our agents,
    // and our sticker / patch on one of your own items (it goes on a Lootline copy). True = applied a moment later by
    // FinishStickerJobs, never sent to Valve. False = a real sticker on a real item (or not a sticker at all): the game's.
    bool QueueStickerOnGun(std::uint64_t toolId, std::uint64_t gunId)
    {
        int ourKit = -1;      // -1: the tool isn't ours
        bool agent = false;   // a patch on an agent (else a sticker on a gun / the C4)
        bool copy = false;    // the target isn't ours: our sticker / patch goes on a Lootline copy of it

        {
            std::lock_guard lock(g_mutex);
            const Unboxer::Item* gun = FindLocked(gunId);
            const Unboxer::Item* tool = FindLocked(toolId);

            if (gun)
            {
                if (gun->kind != Unboxer::Kind::Drop)
                    return false;

                agent = IsAgent(gun->definition);

                if (!agent && !CanHoldSticker(gun->definition))
                    return false;   // not something a sticker goes on (charms / name tags are handled elsewhere)

                const Cases::DropKind wanted = agent ? Cases::DropKind::Patch : Cases::DropKind::Sticker;

                if (tool)
                    ourKit = tool->kind == Unboxer::Kind::Drop && Cases::KindOf(tool->definition) == wanted ? tool->paintKit : 0;
            }
            else
            {
                // one of your own items: only with our sticker / patch (a real one on a real item is Valve's)
                if (!tool || tool->kind != Unboxer::Kind::Drop)
                    return false;

                const Cases::DropKind kind = Cases::KindOf(tool->definition);

                if (kind != Cases::DropKind::Sticker && kind != Cases::DropKind::Patch)
                    return false;

                agent = kind == Cases::DropKind::Patch;
                ourKit = tool->paintKit;
                copy = true;
            }
        }

        const char* const what = agent ? "patch" : "sticker";

        if (copy)
        {
            GameInv::ItemSnapshot target;

            if (!GameInv::ReadItem(gunId, target) || (agent ? !IsAgent(target.definition) : !CanHoldSticker(target.definition)))
            {
                Log("%s: UseTool(%llu, %llu) - your item isn't one a %s goes on, left to the game", what, static_cast<unsigned long long>(toolId),
                    static_cast<unsigned long long>(gunId), what);
                return false;
            }
        }

        // the sticker / patch: ours (kit known) or a real one, read the way the game reads it
        const Cases::DropKind wanted = agent ? Cases::DropKind::Patch : Cases::DropKind::Sticker;
        const bool ourTool = ourKit >= 0;
        const int kit = ourTool ? ourKit :
            (Cases::KindOf(GameInv::ItemDefinition(toolId)) == wanted ? GameInv::StickerKitOf(toolId) : 0);

        // a kit the game knows (the item schema is complete and read-only once the case list is ready)
        const bool knownKit = kit > 0 && Cases::Ready() &&
            (Skins::g_ItemSchema.GetStickerKit(kit) != nullptr || Cases::FindKit(wanted, kit) != nullptr);

        if (!knownKit)
        {
            Log("%s: UseTool(%llu, %llu) on a %s isn't a %s (kit %d) - left to the game", what, static_cast<unsigned long long>(toolId),
                static_cast<unsigned long long>(gunId), agent ? "agent" : "gun", what, kit);
            return false;
        }

        // where the popup has it (what CS2 would send): a sticker's slot + 3D placement, a patch's slot
        int slot = -1;
        GameInv::StickerSlot placement;
        bool placed = false;

        if (agent)
        {
            slot = GameInv::PatchSlot();
            placed = slot >= 0;

            if (placed)
                Log("patch: the popup picked slot %d", slot);
        }
        else
        {
            placed = GameInv::StickerPlacement(slot, placement);

            if (placed)
                Log("sticker: the popup placed it in slot %d at x %.4f y %.4f, rotation %.1f, wear %.3f", slot, placement.offsetX, placement.offsetY,
                    placement.rotation, placement.wear);
        }

        const bool validSlot = placed && slot >= 0 && slot < Unboxer::STICKER_SLOTS;

        if (!validSlot)
        {
            Log("%s: no placement from the game (%s, slot %d) - first free slot", what, placed ? "read" : "not found", slot);
            placement = {};
        }

        std::lock_guard lock(g_mutex);

        for (const PendingSticker& pending : g_stickerJobs)
        {
            if (pending.gunId == gunId)
                return true;   // already applying (double click)
        }

        // A sticker's popup slot is the POSITION its placement is relative to (stickers put near the same spot share it):
        // Valve keeps each sticker in a free slot of its own with that position as its "schema". A patch's slot is its
        // own. -1 = the first free slot (a copy's is picked when it's made).
        int position = -1;

        if (!agent && validSlot)
        {
            position = slot;
            slot = -1;
        }
        else if (!validSlot)
        {
            slot = -1;
        }

        if (!copy && slot < 0)
        {
            const Unboxer::Item* gun = FindLocked(gunId);

            if (!gun)
                return false;

            slot = FirstFreeStickerSlot(*gun);

            if (slot < 0)
            {
                Log("%s: %llu has no free slot (all %d taken)", what, static_cast<unsigned long long>(gunId), Unboxer::STICKER_SLOTS);
                return false;
            }
        }

        PendingSticker job;
        job.at = GetTickCount64() + STICKER_RESULT_DELAY_MS;
        job.apply = true;
        job.gunId = gunId;
        job.toolId = ourTool ? toolId : 0;
        job.slot = slot;
        job.sticker = { kit, placement.wear > 0.0f ? std::min(placement.wear, 1.0f) : 0.0f, placement.rotation, placement.offsetX, placement.offsetY, position };
        job.patch = agent;
        job.copy = copy;
        g_stickerJobs.push_back(job);

        Log("%s: kit %d (%s) on %s%llu, slot %d (position %d), x %.4f y %.4f rotation %.1f wear %.3f - applying locally", what, kit,
            ourTool ? "ours" : "a real one", copy ? "your own item " : "", static_cast<unsigned long long>(gunId), slot, position,
            job.sticker.offsetX, job.sticker.offsetY, job.sticker.rotation, job.sticker.wear);
        return true;
    }

    // Our sticker / patch on one of your own items: it goes on that item's Lootline copy.
    void StickerOnCopy(const PendingSticker& job)
    {
        Unboxer::Item copy;

        if (!CopyOfOriginal(job.gunId, copy))
        {
            Log("%s: %llu couldn't be read any more - no copy made", job.patch ? "patch" : "sticker", static_cast<unsigned long long>(job.gunId));
            return;
        }

        const int slot = job.slot >= 0 && job.slot < Unboxer::STICKER_SLOTS ? job.slot : FirstFreeStickerSlot(copy);

        if (slot < 0)
        {
            Log("%s: your %s has no free slot", job.patch ? "patch" : "sticker", Unboxer::DisplayName(copy).c_str());
            return;
        }

        copy.stickers[slot] = job.sticker;

        char what[48];
        snprintf(what, sizeof(what), "%s %d", job.patch ? "patch" : "sticker", job.sticker.kit);
        FinishCopy(job.gunId, copy, job.toolId, job.patch ? NOTIFICATION_PATCH_APPLY : NOTIFICATION_STICKER_APPLY,
            job.patch ? "patch_apply" : "sticker_apply", what);
    }

    // Game thread: the sticker answers that are due — the gun's attributes, the sticker used up, the popup told.
    void FinishStickerJobs()
    {
        const ULONGLONG now = GetTickCount64();
        std::vector<PendingSticker> due;
        {
            std::lock_guard lock(g_mutex);

            for (auto it = g_stickerJobs.begin(); it != g_stickerJobs.end();)
            {
                if (now >= it->at)
                {
                    due.push_back(*it);
                    it = g_stickerJobs.erase(it);
                }
                else
                {
                    ++it;
                }
            }
        }

        for (const PendingSticker& job : due)
        {
            if (job.copy)
            {
                StickerOnCopy(job);
                continue;
            }

            GameInv::StickerSlot result;
            bool removed = false;
            int slot = job.slot;

            {
                std::lock_guard lock(g_mutex);
                Unboxer::Item* gun = FindLocked(job.gunId);

                if (!gun)
                    continue;   // removed meanwhile

                // a sticker never goes on top of another one (its free slot was taken meanwhile: the next free one)
                if (job.apply && !job.patch && gun->stickers[slot].kit > 0)
                {
                    slot = FirstFreeStickerSlot(*gun);

                    if (slot < 0)
                    {
                        Log("sticker: %llu has no free slot any more - kit %d not applied", static_cast<unsigned long long>(job.gunId), job.sticker.kit);
                        continue;
                    }
                }

                Unboxer::Sticker& sticker = gun->stickers[slot];

                if (job.apply)
                {
                    sticker = job.sticker;
                }
                else if (job.wear > 100)
                {
                    sticker = {};
                    removed = true;
                }
                else
                {
                    sticker.wear = std::max(sticker.wear, std::clamp(static_cast<float>(job.wear) / 100.0f, 0.0f, 1.0f));
                }

                result = { sticker.kit, sticker.wear, sticker.rotation, sticker.offsetX, sticker.offsetY, sticker.schema };
                g_dirty = true;
                g_loadoutDirty = true;   // the skin changer puts it on the gun in a match too
            }

            const bool written = GameInv::SetSticker(job.gunId, slot, result);

            if (job.apply && job.toolId)
            {
                GameInv::RemoveItem(job.toolId);   // a sticker is used up when it's applied

                std::lock_guard lock(g_mutex);
                EraseLocked(job.toolId);
                g_dirty = true;
            }

            char idText[32];
            snprintf(idText, sizeof(idText), "%llu", static_cast<unsigned long long>(job.gunId));
            bool handled = false;
            bool notified = false;

            if (job.apply)
                notified = job.patch ? NotifyItemCustomization(NOTIFICATION_PATCH_APPLY, "patch_apply", idText, handled) :
                    NotifyItemCustomization(NOTIFICATION_STICKER_APPLY, "sticker_apply", idText, handled);
            else if (removed)
                notified = job.patch ? NotifyItemCustomization(NOTIFICATION_PATCH_REMOVE, "patch_remove", idText, handled) :
                    NotifyItemCustomization(NOTIFICATION_STICKER_REMOVE, "sticker_remove", idText, handled);

            // what CS2's own item view of the gun says is in that slot now (the sticker check: 0 = the game doesn't see it)
            const int seen = GameInv::StickerKitAt(job.gunId, slot);

            Log("sticker: %s slot %d of %llu (kit %d, wear %.3f): item %s, popup %s, the game's view of the slot: kit %d", job.apply ? "applied" : removed ? "took off" : "scraped", slot,
                static_cast<unsigned long long>(job.gunId), result.kit, result.wear, written ? "updated" : "NOT updated",
                job.apply || removed ? (notified ? "told" : "NOT told") : "updates by itself", seen);
        }
    }

    // UseTool(our charm, our gun) from CS2's charm popup: hung on locally. A real charm / a real gun: the game's.
    bool QueueCharmOnGun(std::uint64_t toolId, std::uint64_t gunId)
    {
        PendingCharm job;
        bool ourGun = false;

        {
            std::lock_guard lock(g_mutex);
            const Unboxer::Item* tool = FindLocked(toolId);

            if (!tool || tool->kind != Unboxer::Kind::Drop || tool->definition != Unboxer::DEF_CHARM)
                return false;

            const Unboxer::Item* gun = FindLocked(gunId);
            ourGun = gun != nullptr;

            if (gun && (gun->kind != Unboxer::Kind::Drop || !CanHoldCharm(gun->definition)))
            {
                Log("charm: UseTool(%llu, %llu) - that Lootline item can't take a charm, left to the game", static_cast<unsigned long long>(toolId),
                    static_cast<unsigned long long>(gunId));
                return false;
            }

            for (const PendingCharm& pending : g_charmJobs)
            {
                if (pending.gunId == gunId)
                    return true;   // already on it (double click)
            }

            job.keychain = tool->paintKit;
            job.seed = tool->charmSeed;
            job.sticker = tool->charmSticker;
            job.highlight = tool->highlight;
        }

        if (job.keychain <= 0)
            return false;

        // one of your own weapons (a real one, or a stock one): Valve refuses a Lootline charm, so it goes on a copy
        if (!ourGun)
        {
            GameInv::ItemSnapshot target;

            if (!GameInv::ReadItem(gunId, target) || !CanHoldCharm(target.definition))
            {
                Log("charm: UseTool(%llu, %llu) - not a weapon that takes a charm, left to the game", static_cast<unsigned long long>(toolId),
                    static_cast<unsigned long long>(gunId));
                return false;
            }

            job.copy = true;
        }

        const bool placed = GameInv::KeychainPlacement(job.x, job.y, job.z);
        job.at = GetTickCount64() + STICKER_RESULT_DELAY_MS;
        job.gunId = gunId;
        job.toolId = toolId;

        std::lock_guard lock(g_mutex);
        g_charmJobs.push_back(job);
        Log("charm: %d (pattern %d) on %s%llu at x %.3f y %.3f z %.3f (%s) - applying locally", job.keychain, job.seed,
            job.copy ? "your own weapon " : "", static_cast<unsigned long long>(gunId), job.x, job.y, job.z,
            placed ? "the popup's spot" : "placement not found: the usual spot");
        return true;
    }

    // Our charm on one of your own weapons: it goes on that weapon's Lootline copy.
    void MakeCharmedCopy(const PendingCharm& job)
    {
        Unboxer::Item copy;

        if (!CopyOfOriginal(job.gunId, copy) || !CanHoldCharm(copy.definition))
        {
            Log("charm: %llu couldn't be read any more - no copy made", static_cast<unsigned long long>(job.gunId));
            return;
        }

        copy.charm = job.keychain;
        copy.charmSeed = job.seed;
        copy.charmSticker = job.sticker;
        copy.highlight = job.highlight;
        copy.charmX = job.x;
        copy.charmY = job.y;
        copy.charmZ = job.z;

        char what[32];
        snprintf(what, sizeof(what), "charm %d", job.keychain);
        FinishCopy(job.gunId, copy, job.toolId, NOTIFICATION_KEYCHAIN_APPLY, "keychain_apply", what);
    }

    // Game thread: the charm answers that are due - the gun's attributes, the charm used up / given back, the popup told.
    void FinishCharmJobs()
    {
        const ULONGLONG now = GetTickCount64();
        std::vector<PendingCharm> due;
        {
            std::lock_guard lock(g_mutex);

            for (auto it = g_charmJobs.begin(); it != g_charmJobs.end();)
            {
                if (now >= it->at)
                {
                    due.push_back(*it);
                    it = g_charmJobs.erase(it);
                }
                else
                {
                    ++it;
                }
            }
        }

        for (const PendingCharm& job : due)
        {
            if (job.copy)
            {
                MakeCharmedCopy(job);
                continue;
            }

            Unboxer::Item returned;   // taking a charm off gives the charm item back, like CS2 does
            PendingCharm write = job;

            {
                std::lock_guard lock(g_mutex);
                Unboxer::Item* gun = FindLocked(job.gunId);

                if (!gun)
                    continue;   // removed meanwhile

                if (job.apply)
                {
                    gun->charm = job.keychain;
                    gun->charmSeed = job.seed;
                    gun->charmSticker = job.sticker;
                    gun->highlight = job.highlight;
                    gun->charmX = job.x;
                    gun->charmY = job.y;
                    gun->charmZ = job.z;
                }
                else
                {
                    if (gun->charm <= 0)
                        continue;

                    const KeychainInfo* info = Skins::g_ItemSchema.GetKeychain(gun->charm);
                    returned.id = g_nextId++;
                    returned.kind = Unboxer::Kind::Drop;
                    returned.definition = Unboxer::DEF_CHARM;
                    returned.paintKit = gun->charm;
                    returned.charmSeed = gun->charmSeed;
                    returned.charmSticker = gun->charmSticker;   // a slab comes back with its sticker
                    returned.highlight = gun->highlight;         // a highlight with its clip
                    returned.rarity = info ? std::max(info->rarity, 1) : 1;
                    returned.quality = QUALITY_UNIQUE;

                    gun->charm = 0;
                    gun->charmSeed = 0;
                    gun->charmSticker = 0;
                    gun->highlight = 0;
                    gun->charmX = gun->charmY = gun->charmZ = 0.0f;
                    write.keychain = 0;
                    write.sticker = 0;
                }

                g_dirty = true;
                g_loadoutDirty = true;   // the skin changer hangs it on the gun in a match too
            }

            const bool written = GameInv::SetKeychain(job.gunId, write.keychain, write.seed, write.x, write.y, write.z, write.sticker,
                write.keychain > 0 ? CharmHighlight(write.keychain, write.highlight) : 0);

            if (job.apply && job.toolId)
            {
                GameInv::RemoveItem(job.toolId);   // the charm item is on the gun now

                std::lock_guard lock(g_mutex);
                EraseLocked(job.toolId);
                g_dirty = true;
            }

            bool gaveBack = false;

            if (!job.apply && returned.id && GameInv::CreateItem(ToNewItem(returned)))
            {
                gaveBack = true;
                std::lock_guard lock(g_mutex);
                g_items.push_back(returned);
                g_dirty = true;
            }

            char idText[32];
            snprintf(idText, sizeof(idText), "%llu", static_cast<unsigned long long>(job.gunId));
            bool handled = false;
            const bool notified = job.apply ? NotifyItemCustomization(NOTIFICATION_KEYCHAIN_APPLY, "keychain_apply", idText, handled) :
                NotifyItemCustomization(NOTIFICATION_KEYCHAIN_REMOVE, "keychain_remove", idText, handled);

            Log("charm: %s %llu (charm %d): item %s, popup %s%s", job.apply ? "hung on" : "took off", static_cast<unsigned long long>(job.gunId),
                job.apply ? job.keychain : returned.paintKit, written ? "updated" : "NOT updated", notified ? "told" : "NOT told",
                job.apply ? "" : gaveBack ? ", charm back in the inventory" : ", charm NOT given back");
        }
    }

    // UseTool(sticker, Sticker Slab) - CS2's "Seal in Sticker Slab". Done here when the sticker or the slab tool is a
    // Lootline item (a Lootline sticker with the store's slab too: free); a real sticker / slab in it is left alone
    // (Valve's), ours are used up. A real sticker with a real or the store's slab: the game's.
    bool QueueSeal(std::uint64_t stickerId, std::uint64_t slabId)
    {
        PendingSlab job;
        job.seal = true;
        job.stickerId = stickerId;
        job.slabId = slabId;

        std::uint16_t fauxDefinition = 0;
        int fauxPaintKit = 0;
        const bool fauxSlab = Unboxer::FromFauxId(slabId, fauxDefinition, fauxPaintKit) && fauxDefinition == Unboxer::DEF_STICKER_SLAB;

        {
            std::lock_guard lock(g_mutex);
            const Unboxer::Item* sticker = FindLocked(stickerId);
            const Unboxer::Item* slab = FindLocked(slabId);

            if (sticker && (sticker->kind != Unboxer::Kind::Drop || sticker->definition != Cases::DEF_STICKER))
                return false;   // ours, but not a sticker

            if (slab && (slab->kind != Unboxer::Kind::Drop || slab->definition != Unboxer::DEF_STICKER_SLAB))
                return false;   // ours, but not a slab tool

            job.ourSticker = sticker != nullptr;
            job.ourSlab = slab != nullptr;

            if (job.ourSticker)
                job.kit = sticker->paintKit;

            for (const PendingSlab& pending : g_slabJobs)
            {
                if (pending.seal && pending.stickerId == stickerId)
                    return true;   // already on it (double click)
            }
        }

        if (!job.ourSticker && !job.ourSlab)
            return false;   // nothing of ours: the game's

        // the other side: a real slab tool (or the store's) / a real sticker - read, never changed
        if (!job.ourSlab && !fauxSlab && GameInv::ItemDefinition(slabId) != Unboxer::DEF_STICKER_SLAB)
            return false;

        if (!job.ourSticker)
        {
            GameInv::ItemSnapshot sticker;

            if (!GameInv::ReadItem(stickerId, sticker) || sticker.definition != Cases::DEF_STICKER || sticker.stickers[0].kit <= 0)
            {
                Log("slab: UseTool(%llu, %llu) - not a sticker, left to the game", static_cast<unsigned long long>(stickerId),
                    static_cast<unsigned long long>(slabId));
                return false;
            }

            job.kit = sticker.stickers[0].kit;
        }

        if (job.kit <= 0)
            return false;

        job.at = GetTickCount64() + STICKER_RESULT_DELAY_MS;

        std::lock_guard lock(g_mutex);
        g_slabJobs.push_back(job);
        Log("slab: sealing sticker %d (%s sticker %llu, %s slab %llu) - locally", job.kit, job.ourSticker ? "Lootline" : "your own",
            static_cast<unsigned long long>(stickerId), job.ourSlab ? "Lootline" : fauxSlab ? "the store's" : "your own",
            static_cast<unsigned long long>(slabId));
        return true;
    }

    // Game thread: the slab answers that are due - the new slab charm / sticker, ours used up, the popup told.
    void FinishSlabJobs()
    {
        const ULONGLONG now = GetTickCount64();
        std::vector<PendingSlab> due;
        {
            std::lock_guard lock(g_mutex);

            for (auto it = g_slabJobs.begin(); it != g_slabJobs.end();)
            {
                if (now >= it->at)
                {
                    due.push_back(*it);
                    it = g_slabJobs.erase(it);
                }
                else
                {
                    ++it;
                }
            }
        }

        for (const PendingSlab& job : due)
        {
            Unboxer::Item made;
            made.kind = Unboxer::Kind::Drop;
            made.quality = QUALITY_UNIQUE;

            if (job.seal)
            {
                const KeychainInfo* info = Skins::g_ItemSchema.GetKeychain(Unboxer::KEYCHAIN_STICKER_SLAB);
                made.definition = Unboxer::DEF_CHARM;
                made.paintKit = Unboxer::KEYCHAIN_STICKER_SLAB;
                made.charmSticker = job.kit;
                made.rarity = info ? std::max(info->rarity, 1) : 1;
            }
            else
            {
                const Cases::Kit* kit = Cases::FindKit(Cases::DropKind::Sticker, job.kit);
                made.definition = Cases::DEF_STICKER;
                made.paintKit = job.kit;
                made.rarity = Cases::Rarity(static_cast<Cases::Tier>(kit ? kit->tier : static_cast<int>(Cases::TierMilSpec)));
            }

            {
                std::lock_guard lock(g_mutex);

                // used up meanwhile (deleted, sealed twice...): nothing to make
                if ((job.seal && job.ourSticker && !FindLocked(job.stickerId)) || (job.seal && job.ourSlab && !FindLocked(job.slabId)) ||
                    (!job.seal && !FindLocked(job.slabId)))
                    continue;

                made.id = g_nextId++;
            }

            if (!GameInv::CreateItem(ToNewItem(made)))
            {
                Log("slab: the %s couldn't be made - nothing used up", job.seal ? "slab charm" : "sticker");
                continue;
            }

            // used up: our sticker and slab tool (sealing) / our slab charm (cracking). Real ones are never touched.
            std::vector<std::uint64_t> used;
            if (job.seal && job.ourSticker)
                used.push_back(job.stickerId);
            if (job.ourSlab)
                used.push_back(job.slabId);

            for (const std::uint64_t id : used)
                GameInv::RemoveItem(id);

            {
                std::lock_guard lock(g_mutex);

                for (const std::uint64_t id : used)
                    EraseLocked(id);

                g_items.push_back(made);
                g_dirty = true;
            }

            char idText[32];
            snprintf(idText, sizeof(idText), "%llu", static_cast<unsigned long long>(made.id));
            bool handled = false;
            const bool notified = job.seal ? NotifyItemCustomization(NOTIFICATION_KEYCHAIN_SEAL, "keychain_seal", idText, handled) :
                NotifyItemCustomization(NOTIFICATION_STICKER_EXTRACT, "sticker_extract", idText, handled);

            Log("slab: %s sticker %d -> %llu, popup %s", job.seal ? "sealed" : "cracked out", job.kit,
                static_cast<unsigned long long>(made.id), notified ? "told" : "NOT told");
        }
    }

    void RequeueLoadoutLocked(const Unboxer::Item& item);

    // UseTool(name tag, our item): a name tag (a "faux" one: what CS2 sends when you name your pet) on one of our
    // items, with the name typed into SetNameToolString. Anything else: the game's.
    bool QueueNameTag(std::uint64_t toolId, std::uint64_t itemId)
    {
        std::uint16_t toolDefinition = 0;
        int unusedPaintKit = 0;

        if (!Unboxer::FromFauxId(toolId, toolDefinition, unusedPaintKit))
            toolDefinition = GameInv::ItemDefinition(toolId);

        if (toolDefinition != DEF_NAME_TAG)
            return false;

        bool copy = false;
        {
            std::lock_guard lock(g_mutex);
            const Unboxer::Item* item = FindLocked(itemId);
            const Unboxer::Item* tool = FindLocked(toolId);

            if (item && item->kind != Unboxer::Kind::Drop)
                return false;

            if (!item)
            {
                // one of your own items: only with our name tag (a real one on a real item is Valve's)
                if (!tool || tool->kind != Unboxer::Kind::Drop)
                    return false;

                copy = true;
            }
        }

        if (copy)
        {
            GameInv::ItemSnapshot target;

            if (!GameInv::ReadItem(itemId, target) || (!CanHoldSticker(target.definition) && !IsKnife(target.definition) && !IsAgent(target.definition)))
            {
                Log("name tag: UseTool(%llu, %llu) - your item can't be copied, left to the game", static_cast<unsigned long long>(toolId),
                    static_cast<unsigned long long>(itemId));
                return false;
            }
        }

        std::lock_guard lock(g_mutex);

        if (g_nameToolText.empty())
        {
            Log("name tag: on %llu, but no name was typed - left to the game", static_cast<unsigned long long>(itemId));
            return false;
        }

        for (const PendingName& pending : g_nameJobs)
        {
            if (pending.itemId == itemId)
                return true;
        }

        PendingName job;
        job.at = GetTickCount64() + STICKER_RESULT_DELAY_MS;
        job.itemId = itemId;
        job.name = g_nameToolText;
        job.copy = copy;
        job.toolId = copy ? toolId : 0;
        g_nameJobs.push_back(job);
        Log("name tag: '%s' on %s%llu - locally", g_nameToolText.c_str(), copy ? "your own item " : "", static_cast<unsigned long long>(itemId));
        return true;
    }

    // Our name tag on one of your own items: the name goes on that item's Lootline copy.
    void NameOnCopy(const PendingName& job)
    {
        Unboxer::Item copy;

        if (!CopyOfOriginal(job.itemId, copy))
        {
            Log("name tag: %llu couldn't be read any more - no copy made", static_cast<unsigned long long>(job.itemId));
            return;
        }

        copy.customName = job.name;
        FinishCopy(job.itemId, copy, job.toolId, NOTIFICATION_NAME_ITEM, "nametag_add", "name tag");
    }

    // Game thread: the names that are due.
    void FinishNameJobs()
    {
        const ULONGLONG now = GetTickCount64();
        std::vector<PendingName> due;
        {
            std::lock_guard lock(g_mutex);

            for (auto it = g_nameJobs.begin(); it != g_nameJobs.end();)
            {
                if (now >= it->at)
                {
                    due.push_back(*it);
                    it = g_nameJobs.erase(it);
                }
                else
                {
                    ++it;
                }
            }
        }

        for (const PendingName& job : due)
        {
            if (job.copy)
            {
                NameOnCopy(job);
                continue;
            }

            if (job.name.empty())
            {
                // the name tag's "Remove": CS2 counts a name attribute that's there at all, even an empty one, in
                // places - so the item is made again without it (same id), and goes back into the loadout if it was in
                Unboxer::Item unnamed;
                {
                    std::lock_guard lock(g_mutex);
                    Unboxer::Item* item = FindLocked(job.itemId);

                    if (!item)
                        continue;

                    item->customName.clear();
                    unnamed = *item;
                    g_dirty = true;
                    g_loadoutDirty = true;   // a gun's name shows in a match too
                }

                const bool removed = GameInv::RemoveItem(job.itemId);
                const bool remade = removed && GameInv::CreateItem(ToNewItem(unnamed));

                if (remade && unnamed.equipped)
                {
                    std::lock_guard lock(g_mutex);
                    RequeueLoadoutLocked(unnamed);
                }

                char idText[32];
                snprintf(idText, sizeof(idText), "%llu", static_cast<unsigned long long>(job.itemId));
                bool handled = false;
                const bool notified = remade && NotifyItemCustomization(NOTIFICATION_NAME_REMOVE, "nametag_remove", idText, handled);

                Log("name tag: removed from %llu: item %s, popup %s", static_cast<unsigned long long>(job.itemId),
                    remade ? "made again without it" : removed ? "NOT made again (back on the next start)" : "NOT found",
                    notified ? "told" : "NOT told");
                continue;
            }

            int petLevel = -1;
            bool newUnit = false;
            Unboxer::Item named;

            {
                std::lock_guard lock(g_mutex);
                Unboxer::Item* item = FindLocked(job.itemId);

                if (!item)
                    continue;

                item->customName = job.name;
                petLevel = item->definition == Unboxer::DEF_PET ? item->petLevel : -1;

                // a new Storage Unit: naming it is what makes it usable (CS2 asks for a name until it has a "modification date")
                if (item->definition == Unboxer::DEF_CASKET && item->modified == 0)
                {
                    item->modified = static_cast<std::uint32_t>(std::time(nullptr));
                    newUnit = true;
                }

                named = *item;
                g_dirty = true;
                g_loadoutDirty = true;   // a gun's name shows in a match too
            }

            // a new unit is made again (same id) with its date + count, so CS2 offers "put in" / "look inside" from now on
            const bool written = newUnit ? (GameInv::RemoveItem(job.itemId) && GameInv::CreateItem(ToNewItem(named))) :
                GameInv::SetName(job.itemId, job.name, petLevel);

            char idText[32];
            snprintf(idText, sizeof(idText), "%llu", static_cast<unsigned long long>(job.itemId));
            bool handled = false;
            const bool notified = NotifyItemCustomization(NOTIFICATION_NAME_ITEM, "nametag_add", idText, handled);

            Log("name tag: '%s' on %llu: item %s, popup %s", job.name.c_str(), static_cast<unsigned long long>(job.itemId),
                written ? "updated" : "NOT updated", notified ? "told" : "NOT told");
        }
    }

    // UseTool(our sealed graffiti, "") - CS2's "Unseal" (popup_capability_decodable). Valve would answer with a new,
    // unsealed graffiti item ("graffity_unseal" + its id); without one the popup's countdown ends in "unable to
    // retrieve your item". Anything else: not this.
    bool QueueUnseal(std::uint64_t toolId, std::uint64_t itemId)
    {
        if (itemId != 0 && itemId != toolId)
            return false;

        std::lock_guard lock(g_mutex);
        const Unboxer::Item* sealed = FindLocked(toolId);

        if (!sealed || sealed->kind != Unboxer::Kind::Drop || sealed->definition != Cases::DEF_GRAFFITI)
            return false;

        for (const PendingUnseal& pending : g_unsealJobs)
        {
            if (pending.sealedId == toolId)
                return true;   // already on it (double click)
        }

        g_unsealJobs.push_back({ GetTickCount64() + OPEN_RESULT_DELAY_MS, toolId });
        Log("graffiti: unsealing %llu (kit %d) - locally", static_cast<unsigned long long>(toolId), sealed->paintKit);
        return true;
    }

    // Game thread: the unsealings that are due - the unsealed graffiti made, the sealed one used up, the popup told.
    void FinishUnsealJobs()
    {
        const ULONGLONG now = GetTickCount64();
        std::vector<PendingUnseal> due;
        {
            std::lock_guard lock(g_mutex);

            for (auto it = g_unsealJobs.begin(); it != g_unsealJobs.end();)
            {
                if (now >= it->at)
                {
                    due.push_back(*it);
                    it = g_unsealJobs.erase(it);
                }
                else
                {
                    ++it;
                }
            }
        }

        for (const PendingUnseal& job : due)
        {
            Unboxer::Item graffiti;
            {
                std::lock_guard lock(g_mutex);
                const Unboxer::Item* sealed = FindLocked(job.sealedId);

                if (!sealed)
                    continue;   // removed meanwhile

                graffiti = *sealed;   // the same graffiti (kit, rarity), now the kind that equips
                graffiti.id = g_nextId++;
                graffiti.definition = Cases::DEF_SPRAY_PAINT;
                graffiti.equipped = 0;
                graffiti.slotT.clear();
                graffiti.slotCT.clear();
            }

            if (!GameInv::CreateItem(ToNewItem(graffiti)))
            {
                Log("graffiti: unsealing %llu FAILED: couldn't make the unsealed graffiti", static_cast<unsigned long long>(job.sealedId));
                continue;
            }

            GameInv::RemoveItem(job.sealedId);   // used up, like Valve does it

            {
                std::lock_guard lock(g_mutex);
                EraseLocked(job.sealedId);
                g_items.push_back(graffiti);
                g_dirty = true;
            }

            char idText[32];
            snprintf(idText, sizeof(idText), "%llu", static_cast<unsigned long long>(graffiti.id));
            bool handled = false;
            const bool notified = NotifyItemCustomization(NOTIFICATION_GRAFFITI_UNSEAL, "graffity_unseal", idText, handled);

            Log("graffiti: unsealed %llu -> %s (%llu), popup %s", static_cast<unsigned long long>(job.sealedId),
                Unboxer::DisplayName(graffiti).c_str(), static_cast<unsigned long long>(graffiti.id), notified ? "told" : "NOT told");
        }
    }

    // ── CS2's Uplink Terminals (popup_offers_laptop) on Lootline items ──
    // Valve's server answers each step of the laptop; for Lootline terminals these jobs do (game thread, never holding
    // g_mutex while the game's inventory is changed: its listeners can come back into the hooks).

    bool IsUnsealedTerminal(const Unboxer::Item* item)
    {
        return item && item->kind == Unboxer::Kind::Case && item->quality == QUALITY_VOLATILE;
    }

    bool TerminalJobPendingLocked(std::uint64_t id)
    {
        return std::any_of(g_terminalJobs.begin(), g_terminalJobs.end(), [id](const TerminalJob& job) { return job.id == id; });
    }

    // A terminal goes away before your item's offer came (you took an earlier one): it comes in the next one instead.
    void DropTerminalPickLocked(std::uint64_t terminalId)
    {
        const auto it = std::find_if(g_terminalPicks.begin(), g_terminalPicks.end(), [terminalId](const auto& pick) { return pick.first == terminalId; });

        if (it == g_terminalPicks.end())
            return;

        g_terminalPicks.erase(it);

        if (!g_drop.random && g_drop.weapon != 0)
            g_pickedTerminals = std::max(g_drop.terminalOn, 1) - 1;   // the next terminal you unseal
    }

    // Each Lootline offer's price (cents) by its id, for CS2's "Accept Offer" button (Unboxer::OfferPriceText). Its own
    // lock: the store native asks while panorama runs scripts.
    std::mutex g_offerPriceMutex;
    std::unordered_map<std::uint64_t, int> g_offerPrices;   // g_offerPriceMutex
    char g_offerPriceText[4][64]{};                         // main thread: the texts handed to panorama
    int g_offerPriceNext = 0;

    // The offer's price in cents (only shown): the built-in price list, else none.
    int OfferPriceCents(const Unboxer::Item& offer)
    {
        if (!Prices::Ready())
            return 0;

        const bool souvenir = offer.quality == 12;
        bool phaseGuessed = false;
        double price = Prices::FindPhased(Cases::MarketName(offer.definition, offer.paintKit, offer.wear, offer.statTrak, souvenir),
            offer.paintKit, phaseGuessed);

        if (price <= 0.0 && offer.statTrak)
            price = Prices::FindPhased(Cases::MarketName(offer.definition, offer.paintKit, offer.wear, false, souvenir), offer.paintKit,
                phaseGuessed);

        return price > 0.0 ? static_cast<int>(std::lround(std::min(price, 1000000.0) * 100.0)) : 0;
    }

    void TellTerminal(std::uint64_t terminalId, const char* what)
    {
        char idText[32];
        snprintf(idText, sizeof(idText), "%llu", static_cast<unsigned long long>(terminalId));
        bool handled = false;
        const bool notified = NotifyItemCustomization(NOTIFICATION_CASKET_CONTENTS, "casket_contents", idText, handled);
        Log("terminal: %llu %s, laptop %s", static_cast<unsigned long long>(terminalId), what, notified ? "told" : "NOT told");
    }

    void UnsealTerminal(std::uint64_t sealedId)
    {
        Unboxer::Item opened;
        {
            std::lock_guard lock(g_mutex);
            const Unboxer::Item* sealed = FindLocked(sealedId);

            if (!sealed)
                return;   // removed meanwhile

            opened = *sealed;
            opened.id = g_nextId++;
            opened.quality = QUALITY_VOLATILE;
            opened.expires = static_cast<std::uint32_t>(std::time(nullptr)) + TERMINAL_LIFETIME_S;
            opened.offerNumber = 0;
            opened.equipped = 0;

            // Choose: is this the terminal your item comes in? (terminals are counted, like cases count openings)
            if (!g_drop.random && g_drop.weapon != 0 && ++g_pickedTerminals >= std::max(g_drop.terminalOn, 1))
            {
                g_pickedTerminals = 0;
                g_terminalPicks.emplace_back(opened.id, std::clamp(g_drop.offerOn, 1, TERMINAL_OFFERS));
                Log("choose: your item comes in this terminal, as offer %d", std::clamp(g_drop.offerOn, 1, TERMINAL_OFFERS));
            }
        }

        if (!GameInv::CreateItem(ToNewItem(opened)))
        {
            Log("terminal: unsealing %llu FAILED: couldn't make the unsealed terminal", static_cast<unsigned long long>(sealedId));
            return;
        }

        GameInv::RemoveItem(sealedId);

        {
            std::lock_guard lock(g_mutex);
            EraseLocked(sealedId);
            g_items.push_back(opened);
            g_dirty = true;
        }

        // what Valve's server answers: the laptop opens with the new item
        char idText[32];
        snprintf(idText, sizeof(idText), "%llu", static_cast<unsigned long long>(opened.id));
        bool handled = false;
        const bool notified = NotifyItemCustomization(NOTIFICATION_UNLOCK_CRATE, "crate_unlock", idText, handled);
        Log("terminal: unsealed %llu -> %llu, popup %s", static_cast<unsigned long long>(sealedId), static_cast<unsigned long long>(opened.id),
            notified ? "told" : "NOT told");
    }

    // The next offer (resume: the current one again - a reopened laptop asks for it).
    void MakeTerminalOffer(std::uint64_t terminalId, bool resume)
    {
        Unboxer::Item offer;
        std::uint64_t previous = 0;
        int number = 0;

        {
            std::lock_guard lock(g_mutex);
            Unboxer::Item* terminal = FindLocked(terminalId);

            if (!IsUnsealedTerminal(terminal))
                return;

            const auto existing = std::find_if(g_terminalOffers.begin(), g_terminalOffers.end(),
                [terminalId](const TerminalOffer& entry) { return entry.terminalId == terminalId; });

            if (existing != g_terminalOffers.end())
            {
                if (resume && terminal->offerNumber != 0)
                {
                    offer = existing->offer;   // still there (checked below, outside the lock)
                    number = terminal->offerNumber;
                }
                else
                {
                    previous = existing->offer.id;
                }
            }

            if (offer.id == 0)
            {
                const int current = terminal->offerNumber;
                number = resume && current != 0 ? current : current < 0 ? current : current + 1;

                if (number >= TERMINAL_OFFERS)
                    number = -TERMINAL_OFFERS;   // the final one

                // your item if this is its offer, else a plain roll (offers never count as case openings)
                const int offerIndex = number < 0 ? -number : number;
                const auto pick = std::find_if(g_terminalPicks.begin(), g_terminalPicks.end(),
                    [terminalId, offerIndex](const auto& entry) { return entry.first == terminalId && entry.second == offerIndex; });
                const bool picked = pick != g_terminalPicks.end();

                if (picked)
                    g_terminalPicks.erase(pick);

                offer = RollDropLocked(Cases::Find(terminal->definition), picked ? PickMode::Now : PickMode::Never);

                if (offer.definition == 0)
                {
                    Log("terminal: %llu - nothing to offer (its contents aren't known)", static_cast<unsigned long long>(terminalId));
                    return;
                }

                offer.id = g_nextId++;
                offer.kind = Unboxer::Kind::Drop;
                offer.fromCase = terminal->definition;
            }
        }

        // the reopened laptop's offer is still in the game's inventory: just tell it again
        if (resume && previous == 0 && GameInv::HasItem(offer.id))
        {
            TellTerminal(terminalId, "offer again");
            return;
        }

        if (previous)
            GameInv::RemoveItem(previous);   // turned down

        GameInv::NewItem request = ToNewItem(offer);
        request.casketId = terminalId;       // inside the terminal, like Valve's offers
        request.purchasePrice = OfferPriceCents(offer);

        if (resume && offer.id != 0 && !GameInv::HasItem(offer.id))
        {
            // the old offer is gone (Lootline restarted): a new one with the same number
            std::lock_guard lock(g_mutex);
            offer.id = g_nextId++;
            request.id = offer.id;
        }

        if (!GameInv::CreateItem(request))
        {
            Log("terminal: %llu - the offer couldn't be made", static_cast<unsigned long long>(terminalId));
            return;
        }

        {
            std::lock_guard lock(g_offerPriceMutex);
            g_offerPrices[request.id] = request.purchasePrice;   // "Accept Offer" shows it in your currency
        }

        const bool numbered = GameInv::SetOfferNumber(terminalId, number);

        {
            std::lock_guard lock(g_mutex);

            if (Unboxer::Item* terminal = FindLocked(terminalId))
                terminal->offerNumber = number;

            std::erase_if(g_terminalOffers, [terminalId](const TerminalOffer& entry) { return entry.terminalId == terminalId; });
            g_terminalOffers.push_back({ terminalId, offer });
            g_dirty = true;
        }

        char what[256];
        snprintf(what, sizeof(what), "offer %d%s: %s (%llu, $%.2f)%s", number < 0 ? -number : number, number < 0 ? " (final)" : "",
            Unboxer::DisplayName(offer).c_str(), static_cast<unsigned long long>(offer.id), request.purchasePrice / 100.0,
            numbered ? "" : " - its number NOT set");
        TellTerminal(terminalId, what);
    }

    // Turned down the final offer: the terminal is discarded (the laptop "explodes").
    void EndTerminal(std::uint64_t terminalId)
    {
        std::uint64_t offerId = 0;
        {
            std::lock_guard lock(g_mutex);

            for (const TerminalOffer& entry : g_terminalOffers)
                if (entry.terminalId == terminalId)
                    offerId = entry.offer.id;

            std::erase_if(g_terminalOffers, [terminalId](const TerminalOffer& entry) { return entry.terminalId == terminalId; });
            DropTerminalPickLocked(terminalId);
        }

        if (offerId)
            GameInv::RemoveItem(offerId);

        GameInv::RemoveItem(terminalId);

        {
            std::lock_guard lock(g_mutex);
            EraseLocked(terminalId);
            g_dirty = true;
        }

        TellTerminal(terminalId, "ended (no deal), discarded");
    }

    // "Accept": the offer comes out of the terminal into your inventory, for free. The laptop shows it (the store's
    // purchase completed event); the terminal goes a moment later, once the laptop closed itself.
    void BuyTerminalOffer(std::uint64_t terminalId)
    {
        Unboxer::Item bought;
        {
            std::lock_guard lock(g_mutex);

            for (const TerminalOffer& entry : g_terminalOffers)
                if (entry.terminalId == terminalId)
                    bought = entry.offer;

            std::erase_if(g_terminalOffers, [terminalId](const TerminalOffer& entry) { return entry.terminalId == terminalId; });
            DropTerminalPickLocked(terminalId);
        }

        if (bought.id == 0)
        {
            Log("terminal: %llu - accepted, but there's no offer", static_cast<unsigned long long>(terminalId));
            return;
        }

        GameInv::RemoveItem(bought.id);   // out of the terminal ...
        const bool created = GameInv::CreateItem(ToNewItem(bought));   // ... into the inventory, same id

        if (created)
        {
            std::lock_guard lock(g_mutex);
            g_items.push_back(bought);
            g_dirty = true;
            g_terminalJobs.push_back({ GetTickCount64() + TERMINAL_DISCARD_DELAY_MS, TerminalJob::What::Discard, terminalId });
        }

        char idText[32];
        snprintf(idText, sizeof(idText), "%llu", static_cast<unsigned long long>(bought.id));
        bool handled = false;
        const bool notified = created && NotifyPurchaseCompleted(idText, handled);

        Log("terminal: %llu - bought %s (%llu): %s, laptop %s", static_cast<unsigned long long>(terminalId), Unboxer::DisplayName(bought).c_str(),
            static_cast<unsigned long long>(bought.id), created ? "in your inventory" : "NOT made", notified ? "told" : "NOT told");
    }

    void FinishTerminalJobs()
    {
        const ULONGLONG now = GetTickCount64();
        std::vector<TerminalJob> due;
        {
            std::lock_guard lock(g_mutex);

            for (auto it = g_terminalJobs.begin(); it != g_terminalJobs.end();)
            {
                if (now >= it->at)
                {
                    due.push_back(*it);
                    it = g_terminalJobs.erase(it);
                }
                else
                {
                    ++it;
                }
            }
        }

        for (const TerminalJob& job : due)
        {
            switch (job.what)
            {
            case TerminalJob::What::Unseal: UnsealTerminal(job.id); break;
            case TerminalJob::What::Offer: MakeTerminalOffer(job.id, false); break;
            case TerminalJob::What::Resume: MakeTerminalOffer(job.id, true); break;
            case TerminalJob::What::End: EndTerminal(job.id); break;
            case TerminalJob::What::Purchase: BuyTerminalOffer(job.id); break;
            case TerminalJob::What::Discard:
                GameInv::RemoveItem(job.id);
                {
                    std::lock_guard lock(g_mutex);
                    EraseLocked(job.id);
                    g_dirty = true;
                }
                Log("terminal: %llu discarded (its item was bought)", static_cast<unsigned long long>(job.id));
                break;
            }
        }
    }
}

namespace
{
    // A JS string literal for one of our scripts: 'text', with quotes / backslashes / line breaks escaped.
    std::string JsString(const std::string& text)
    {
        std::string out = "'";

        for (const char c : text)
        {
            switch (c)
            {
            case '\\': out += "\\\\"; break;
            case '\'': out += "\\'"; break;
            case '\n': out += "\\n"; break;
            case '\r': break;
            default: out += c; break;
            }
        }

        return out + "'";
    }

    // CS2's own "OK" popup, from the main menu (main thread, not inside one of the game's JS calls).
    void ShowPopupMessage(const std::string& text)
    {
        const std::string script = "UiToolkitAPI.ShowGenericPopupOk('Lootline', " + JsString(text) + ", '', function(){});";
        RunMainMenuScript(script.c_str());
    }

    // The container a skin of ours stands for in a contract (its collection): the one it came from, else any that has it,
    // else its collection when no container hands it out (the Dust Collection's Glock-18 | Brass...).
    const Cases::Case* CollectionOf(const Unboxer::Item& item)
    {
        if (const Cases::Case* from = item.fromCase ? Cases::Find(item.fromCase) : nullptr;
            from && Cases::TierIn(*from, item.definition, item.paintKit) >= 0)
            return from;

        for (const Cases::Case& candidate : Cases::All())
        {
            if (Cases::TierIn(candidate, item.definition, item.paintKit) >= 0)
                return &candidate;
        }

        return Cases::SetCollectionWith(item.definition, item.paintKit);
    }

    int SkinTier(const Unboxer::Item& item)
    {
        return std::clamp(item.rarity - 1, 0, static_cast<int>(Cases::TierCovert));
    }

    // A trade-up of our guns (main thread, g_mutex held), the way CS2 works it out: the quality above theirs, from the
    // collection of one of them picked at random (each input gives its collection an equal share), StatTrak if they
    // all are. Covert -> a knife / gloves of that case. The float: each input's float as a share of its finish's range
    // (0 = its lowest, 1 = its highest), averaged, then that share of the result's range - inputs at 0.01 of a 0 - 1
    // range give a Blue Steel 0.01, a Doppler (0 - 0.08) 0.0008, a Crimson Web (0.06 - 0.80) 0.0674. Returns "" with
    // `out` rolled, or why not.
    std::string RollTradeUpLocked(const std::vector<std::uint64_t>& ids, Unboxer::Item& out)
    {
        if (!Cases::Ready())
            return "the case list isn't read yet - try again in a moment";

        std::vector<const Unboxer::Item*> inputs;

        for (const std::uint64_t id : ids)
        {
            const Unboxer::Item* item = FindLocked(id);

            if (!item || item->kind != Unboxer::Kind::Drop || !IsGun(item->definition))
                return "only guns go in a contract";

            inputs.push_back(item);
        }

        if (inputs.empty())
            return "the contract is empty";

        const int tier = SkinTier(*inputs.front());
        bool statTrak = true;
        double wearShareSum = 0.0;

        for (const Unboxer::Item* input : inputs)
        {
            if (SkinTier(*input) != tier)
                return "the items aren't all of the same quality";

            statTrak = statTrak && input->statTrak;

            const Cases::WearRange range = Cases::WearRangeOf(input->paintKit);
            const double span = static_cast<double>(range.max) - static_cast<double>(range.min);
            wearShareSum += span > 0.0 ? std::clamp((static_cast<double>(input->wear) - range.min) / span, 0.0, 1.0) : 0.0;
        }

        const bool toGold = tier == Cases::TierCovert;
        const int target = toGold ? static_cast<int>(Cases::TierGold) : tier + 1;

        std::vector<const Unboxer::Item*> order = inputs;
        std::shuffle(order.begin(), order.end(), g_rng);

        for (const Unboxer::Item* input : order)
        {
            const Cases::Case* collection = CollectionOf(*input);

            if (!collection)
                continue;

            std::vector<std::pair<std::uint16_t, int>> outcomes;

            // the grade above the skin's grade in its collection (an item added with its finish's own rarity can be a
            // grade off: the USP-S | Business Class finish is Industrial, the gun in its collection Mil-Spec)
            const int own = toGold ? -1 : Cases::TierIn(*collection, input->definition, input->paintKit);
            const int want = toGold ? target : (own >= 0 && own < static_cast<int>(Cases::TierCovert) ? own + 1 : target);

            if (toGold)
            {
                if (!collection->golds.empty())
                {
                    for (const Cases::Drop& gold : collection->golds)   // its own knives / gloves
                        outcomes.emplace_back(gold.weapon, gold.paintKit);
                }
                else if (collection->hasGold)
                {
                    for (const auto& entry : g_goldPool)
                        if (collection->goldTypes.empty() || Cases::GoldTypeFrom(*collection, entry.first))
                            outcomes.push_back(entry);
                }
            }
            else
            {
                for (const Cases::Drop& drop : collection->drops)
                    if (drop.tier == want && Cases::KindOf(drop.weapon) == Cases::DropKind::Skin)
                        outcomes.emplace_back(drop.weapon, drop.paintKit);
            }

            if (outcomes.empty())
                continue;   // its collection has nothing above: the next input decides

            const auto [definition, paintKit] = outcomes[static_cast<std::size_t>(Random01() * outcomes.size()) % outcomes.size()];

            out = Unboxer::Item{};
            out.kind = Unboxer::Kind::Drop;
            out.definition = definition;
            out.paintKit = paintKit;
            out.fromCase = collection->definition;
            const Cases::WearRange range = Cases::WearRangeOf(paintKit);
            const double share = wearShareSum / static_cast<double>(inputs.size());
            out.wear = std::clamp(static_cast<float>(range.min + share * (static_cast<double>(range.max) - range.min)), range.min, range.max);
            out.seed = static_cast<int>(Random01() * 1001.0) % 1001;
            out.statTrak = statTrak && !IsGloves(definition);
            out.rarity = Cases::Rarity(static_cast<Cases::Tier>(want));
            out.quality = toGold ? QUALITY_UNUSUAL : (out.statTrak ? QUALITY_STRANGE : QUALITY_UNIQUE);
            return {};
        }

        return toGold ? "none of these items' cases has knives or gloves to trade up to" :
            "these items' collections have nothing of the next quality";
    }

    // Game thread: the popups asked for, and the contracts whose answer is due — the result in, the inputs out, the
    // panel emptied, CS2's "New Item" popup for it.
    void FinishTradeUps()
    {
        const ULONGLONG now = GetTickCount64();
        std::vector<PendingTradeUp> due;
        std::vector<std::string> messages;
        {
            std::lock_guard lock(g_mutex);
            messages.swap(g_popupMessages);

            for (auto it = g_tradeUps.begin(); it != g_tradeUps.end();)
            {
                if (now >= it->at)
                {
                    due.push_back(std::move(*it));
                    it = g_tradeUps.erase(it);
                }
                else
                {
                    ++it;
                }
            }
        }

        for (const std::string& message : messages)
            ShowPopupMessage(message);

        for (PendingTradeUp& job : due)
        {
            {
                std::lock_guard lock(g_mutex);
                bool complete = true;

                for (const std::uint64_t id : job.inputs)
                    complete = complete && FindLocked(id) != nullptr;

                if (!complete)
                {
                    Log("trade-up: an item of the contract was taken out meanwhile - nothing done");
                    continue;
                }

                job.output.id = g_nextId++;
            }

            GameInv::ClearCraftIngredients(job.api);   // the panel empties, like after a real contract

            if (!GameInv::CreateItem(ToNewItem(job.output)))
            {
                Log("trade-up FAILED: couldn't create the result (def %u, paint %d) - the items stay", job.output.definition, job.output.paintKit);
                continue;
            }

            for (const std::uint64_t id : job.inputs)
                GameInv::RemoveItem(id);

            const std::string name = Unboxer::DisplayName(job.output);

            {
                std::lock_guard lock(g_mutex);

                for (const std::uint64_t id : job.inputs)
                    EraseLocked(id);

                g_items.push_back(job.output);
                g_dirty = true;

                if (g_chatDrops.load(std::memory_order_relaxed))
                {
                    ChatLine line;
                    line.at = now;
                    line.item = ChatName(job.output, name);
                    line.rarity = job.output.rarity;
                    line.crafted = true;
                    g_chatLines.push_back(std::move(line));
                }
            }

            char script[160];
            snprintf(script, sizeof(script), "$.DispatchEvent('ShowAcknowledgePopup','acknowledge','%llu');", static_cast<unsigned long long>(job.output.id));
            const bool shown = RunMainMenuScript(script);

            Log("trade-up: %zu items -> %s  def %u paint %d wear %.4f  id %llu, popup %s", job.inputs.size(), name.c_str(), job.output.definition,
                job.output.paintKit, job.output.wear, static_cast<unsigned long long>(job.output.id), shown ? "shown" : "NOT shown");
        }
    }
}

namespace
{
    // ── the Armory with Lootline stars ──
    // CS2's Armory screen (xpshop.js) keeps the star balance in its balance panel's Data().balance; a redeem button is
    // enabled from it when the button is set up (opening a reward's grid). Data() belongs to the panel, so the main menu
    // can write it (like the case popup's result). Every second while Lootline's Armory is on: Lootline's balance in, the
    // balance shown, the "get a pass" banner hidden.
    // Every "UpdateXpShop" (after a redeem, store navigation, inventory updates) the screen puts Valve's balance back and
    // sets the open grid's buttons up again with it - all disabled. So when the balance isn't Lootline's any more, the
    // open grid's nav button is activated again (what the screen's own refresh does): the buttons are set up with
    // Lootline's stars. Not while a redeem is waiting (that would put its confirm bar away).
    // The buttons' hover text said "get a pass and earn stars" without an active pass: it's "not enough stars" now, only
    // when that's true (p = each reward's stars by shop index, 0 = a bidding one, left as it is).
    // m: 0 every second; 1 after a redeem that gave nothing (the screen's refresh now: it stops its particles and its 5 s
    // "item not given" timer); 2 after a reward came NEW - like CS2's store does on the Game Coordinator's answer: 1.5 s
    // later the reward popup ("xpshopredeem" items first) and the refresh.
    // Only while CS2's Store tab is open (the main menu's tabs are in its layout file: a lookup, not a search; a tab not
    // open has "mainmenu-content--hidden"), and the Armory page is looked up in the Store the way mainmenu.js does it
    // (FindChildInLayoutFile), then kept in the main menu's Data(). It used to search the WHOLE UI every second - in a
    // match too: a hitch every second (2026-10-19).
    constexpr const char* ARMORY_SCRIPT = R"JS((function(){
var n=%d,p=[%s],m=%d;
function tab(d,mm,id){var k='ll_'+id,q=d[k];if(q&&q.IsValid())return q;q=mm.FindChildInLayoutFile(id);
if(!q){var now=Date.now();if(now-(d[k+'_at']||0)<10000)return null;d[k+'_at']=now;var r=mm;while(r.GetParent())r=r.GetParent();q=r.FindChildTraverse(id);}
d[k]=q||null;return q||null;}
function fix(){try{var mm=$.GetContextPanel(),d=mm.Data();
var store=tab(d,mm,'JsMainMenuStore');if(!store||!store.IsValid()||!store.visible||store.BHasClass('mainmenu-content--hidden'))return;
var x=d.llXpShop;if(!x||!x.IsValid()){x=store.FindChildInLayoutFile('id-store-page-xpshop')||store.FindChildTraverse('id-store-page-xpshop');d.llXpShop=x||null;}if(!x||!x.IsValid())return;
var b=x.FindChildTraverse('id-xpshop-balance');if(!b||!b.IsValid())return;
var changed=b.Data().balance!==n;
if(changed||b.BHasClass('hide')){b.Data().balance=n;b.SetDialogVariableInt('redeemable-points',n);b.SetHasClass('hide',false);}
var u=x.FindChildTraverse('id-xpshop-upsell');if(u&&u.IsValid()&&!u.BHasClass('hide'))u.SetHasClass('hide',true);
var waiting=false;for(var q=b;q;q=q.GetParent())if(q.BHasClass('waiting-for-redeem')){waiting=true;break;}
if(changed&&!waiting){var nav=x.FindChildTraverse('id-xpshop-top-nav');
if(nav&&nav.IsValid()){var on=nav.Children().filter(function(c){return c.checked===true;})[0];
if(on&&on.IsValid()&&on.id!=='id-nav-show-main-tiles-btn')$.DispatchEvent('Activated',on,'mouse');}}
p.forEach(function(cost,i){if(cost<=0)return;var r=x.FindChildTraverse('id-xpshop-item-redeem-btn-'+i);if(!r||!r.IsValid())return;
r.SetPanelEvent('onmouseover',function(){var need=cost-n;if(need<=0)return;r.SetDialogVariableInt('stars_needed',need);
UiToolkitAPI.ShowTextTooltip(r.id,$.Localize('#xpshop_redeem_not_enough_stars:f',r));});});
}catch(e){}}
if(m===2){$.Schedule(1.5,function(){try{$.DispatchEvent('ShowAcknowledgePopup','','');$.DispatchEvent('UpdateXpShop');}catch(e){}fix();});}
else{if(m===1){try{$.DispatchEvent('UpdateXpShop');}catch(e){}}fix();}
})();)JS";

    enum class ArmoryRun { Tick = 0, Refresh = 1, RewardPopup = 2 };

    ULONGLONG g_armoryBalanceAt = 0;   // main thread: when the balance is written in next

    void RunArmoryScript(ArmoryRun run)
    {
        int stars = 0;
        {
            std::lock_guard lock(g_mutex);
            stars = g_armoryStars;
        }

        std::string points;

        for (const Cases::ArmoryReward& reward : Cases::ArmoryRewards())
        {
            if (!points.empty())
                points += ',';

            points += std::to_string(reward.bid ? 0 : reward.points);
        }

        std::vector<char> script(std::strlen(ARMORY_SCRIPT) + points.size() + 64);
        const int written = snprintf(script.data(), script.size(), ARMORY_SCRIPT, stars, points.c_str(), static_cast<int>(run));

        if (written > 0 && static_cast<std::size_t>(written) < script.size())
            RunMainMenuScript(script.data());
    }

    // One redeem (g_mutex held): a case as it is; else one roll from the reward's loot list with the real odds (a grade
    // like a case's, then any item of it) - a skin with a random float in its finish's range and a pattern, a sticker, a
    // charm with a pattern. Definition 0 = nothing.
    Unboxer::Item RollArmoryRewardLocked(const Cases::ArmoryReward& reward)
    {
        Unboxer::Item out;

        if (reward.crate != 0)
        {
            out.kind = Unboxer::Kind::Case;
            out.definition = reward.crate;
            out.rarity = 0;
            out.quality = QUALITY_UNIQUE;
            return out;
        }

        if (reward.pool.drops.empty())
            return out;

        const Cases::Tier tier = Cases::RollTier(reward.pool, Random01());
        std::vector<const Cases::Drop*> candidates;

        for (const Cases::Drop& drop : reward.pool.drops)
            if (drop.tier == tier)
                candidates.push_back(&drop);

        if (candidates.empty())
            for (const Cases::Drop& drop : reward.pool.drops)
                candidates.push_back(&drop);

        const Cases::Drop& chosen = *candidates[static_cast<std::size_t>(Random01() * candidates.size()) % candidates.size()];

        out.kind = Unboxer::Kind::Drop;
        out.definition = chosen.weapon;
        out.paintKit = chosen.paintKit;
        out.rarity = Cases::Rarity(chosen.tier);
        out.quality = QUALITY_UNIQUE;

        if (chosen.weapon == Unboxer::DEF_CHARM)
        {
            out.charmSeed = static_cast<int>(Random01() * 100001.0) % 100001;   // its pattern
        }
        else if (Cases::KindOf(chosen.weapon) == Cases::DropKind::Skin)
        {
            const Cases::WearRange range = Cases::WearRangeOf(chosen.paintKit);
            out.wear = std::clamp(range.min + RandomWear() * (range.max - range.min), range.min, range.max);
            out.seed = static_cast<int>(Random01() * 1001.0) % 1001;
        }

        return out;
    }

    // Game thread: the redeems whose answer is due - the item in (NEW: CS2's own "new item" popup), the screen refreshed.
    void FinishArmoryJobs()
    {
        const ULONGLONG now = GetTickCount64();
        std::vector<PendingArmory> due;
        {
            std::lock_guard lock(g_mutex);

            for (auto it = g_armoryJobs.begin(); it != g_armoryJobs.end();)
            {
                if (now >= it->at)
                {
                    due.push_back(*it);
                    it = g_armoryJobs.erase(it);
                }
                else
                {
                    ++it;
                }
            }
        }

        if (due.empty())
            return;

        const std::vector<Cases::ArmoryReward>& rewards = Cases::ArmoryRewards();
        bool rewardPopup = false;   // a reward came NEW: CS2's reward popup, like after a Game Coordinator's answer

        for (const PendingArmory& job : due)
        {
            if (job.index < 0 || job.index >= static_cast<int>(rewards.size()))
                continue;   // refused: only the screen is set back (below)

            const Cases::ArmoryReward& reward = rewards[static_cast<std::size_t>(job.index)];
            Unboxer::Item item;
            {
                std::lock_guard lock(g_mutex);
                item = RollArmoryRewardLocked(reward);
                item.id = g_nextId++;
            }

            GameInv::NewItem added;

            if (item.definition != 0)
            {
                added = AsAdded(ToNewItem(item));

                if (added.pickup > 0)
                    added.pickup = GameInv::PICKUP_XPSHOP_REDEEM;   // "xpshopredeem": the popup's reward title, shown first
            }

            if (item.definition == 0 || !GameInv::CreateItem(added))
            {
                std::lock_guard lock(g_mutex);
                g_armoryStars = std::min(g_armoryStars + reward.points, MAX_ARMORY_STARS);   // nothing came: the stars back
                g_dirty = true;
                Log("armory: %s - the item couldn't be made (def %u), %d stars back", reward.itemName.c_str(), item.definition, reward.points);
                continue;
            }

            {
                std::lock_guard lock(g_mutex);
                g_items.push_back(item);
                g_dirty = true;
            }

            rewardPopup = rewardPopup || added.pickup > 0;
            Log("armory: redeemed %s for %d stars -> %s (def %u paint %d, id %llu%s)", reward.itemName.c_str(), reward.points,
                Unboxer::DisplayName(item).c_str(), item.definition, item.paintKit, static_cast<unsigned long long>(item.id),
                added.pickup > 0 ? ", new: reward popup" : "");
        }

        RunArmoryScript(rewardPopup ? ArmoryRun::RewardPopup : ArmoryRun::Refresh);
        g_armoryBalanceAt = GetTickCount64() + 1000;
    }

    // Game thread, every frame: Lootline's balance into CS2's Armory screen (once a second) while it's on.
    void TickArmory()
    {
        FinishArmoryJobs();

        if (!g_armoryOn.load(std::memory_order_relaxed))
            return;

        const ULONGLONG now = GetTickCount64();

        if (now < g_armoryBalanceAt)
            return;

        g_armoryBalanceAt = now + 1000;
        RunArmoryScript(ArmoryRun::Tick);
    }
}

namespace
{
    // ── Lootline's prices in CS2's own screens ──
    // The menu's thread hands the table over (SetGamePrices); the main thread writes it into the root panel's Data()
    // when it changes (llPrices: id -> price text, llTotal, the titles) and every half second puts the labels up:
    // under the item's name in CS2's inspect popup (PopUpInspectHeader -> InspectName; the popup's item is its context
    // panel's Data().oSettings.item_id, popup_inspect_shared.js) and left of the inventory's sort button
    // (InvSortDropdown's row). Only Lootline items are in the table.
    std::atomic_bool g_gamePricesOn{ true };
    Unboxer::GamePrices g_gamePrices;            // g_mutex
    unsigned g_gamePricesRevision = 0;           // g_mutex
    unsigned g_gamePricesSent = 0;               // main thread: the revision panorama has
    bool g_gamePricesShown = false;              // main thread: labels may be up
    ULONGLONG g_gamePricesAt = 0;                // main thread: the next label pass

    // Every half second, but cheap (2026-10-19): the inspect popup only while CS2 has a popup open (its PopupManager has
    // "HaveActivePopups"; only the open popups are searched), the total only while the Inventory tab is open (the main
    // menu's tabs and PopupManager are in its layout file: lookups, not searches). The panels found are kept in the
    // main menu's Data(). It used to search the WHOLE UI twice every half second.
    constexpr const char* GAME_PRICES_LABELS = R"JS((function(){try{var mm=$.GetContextPanel(),md=mm.Data();var root=mm;while(root.GetParent())root=root.GetParent();
var d=root.Data();if(!d.llPrices)return;
function tab(id){var k='ll_'+id,q=md[k];if(q&&q.IsValid())return q;q=mm.FindChildInLayoutFile(id);
if(!q){var now=Date.now();if(now-(md[k+'_at']||0)<10000)return null;md[k+'_at']=now;q=root.FindChildTraverse(id);}
md[k]=q||null;return q||null;}
function make(parent,id){var l=$.CreatePanel('Label',parent,id);l.style.color='#ECB43C';l.style.fontSize='18px';l.style.fontWeight='bold';return l;}
var pm=tab('PopupManager');
if(pm&&pm.IsValid()&&pm.BHasClass('HaveActivePopups')){var h=md.llHeader;
if(!h||!h.IsValid()){h=null;var kids=pm.Children();for(var i=0;i<kids.length&&!h;i++)h=kids[i].FindChildTraverse('PopUpInspectHeader');md.llHeader=h;}
if(h&&h.IsValid()){var id='';for(var p=h;p;p=p.GetParent()){var o=p.Data().oSettings;if(o&&o.item_id){id=String(o.item_id);break;}}
var name=h.FindChildTraverse('InspectName');var text=id?d.llPrices[id]:undefined;var l=h.FindChildTraverse('id-lootline-price');
if(text&&name&&name.IsValid()&&name.GetParent()){if(!l||!l.IsValid()){l=make(name.GetParent(),'id-lootline-price');name.GetParent().MoveChildAfter(l,name);}
l.text=d.llItemTitle+text;l.visible=true;}else if(l&&l.IsValid())l.visible=false;}}
var inv=tab('JsInventory');
if(inv&&inv.IsValid()&&inv.visible&&!inv.BHasClass('mainmenu-content--hidden')){var sort=md.llSort;
if(!sort||!sort.IsValid()){sort=inv.FindChildInLayoutFile('InvSortDropdown')||inv.FindChildTraverse('InvSortDropdown');md.llSort=sort||null;}
if(sort&&sort.IsValid()&&sort.GetParent()){var bar=sort.GetParent();var t=bar.FindChildTraverse('id-lootline-total');
if(d.llTotal){if(!t||!t.IsValid()){t=make(bar,'id-lootline-total');t.style.marginRight='12px';t.style.verticalAlign='center';var first=bar.GetChild(0);if(first&&first!==t)bar.MoveChildBefore(t,first);}
t.text=d.llTotalTitle+d.llTotal;t.visible=true;}else if(t&&t.IsValid())t.visible=false;}}
}catch(e){}})();)JS";

    constexpr const char* GAME_PRICES_OFF = R"JS((function(){try{var root=$.GetContextPanel();while(root.GetParent())root=root.GetParent();
var d=root.Data();d.llPrices=undefined;d.llTotal='';
['id-lootline-price','id-lootline-total'].forEach(function(id){var p=root.FindChildTraverse(id);if(p&&p.IsValid())p.DeleteAsync(0);});
}catch(e){}})();)JS";

    // Main thread: the labels down (switched off, unloading).
    void RemoveGamePriceLabels()
    {
        if (g_gamePricesShown && RunMainMenuScript(GAME_PRICES_OFF))
        {
            g_gamePricesShown = false;
            g_gamePricesSent = 0;
        }
    }

    // Main thread, every frame: the table when it changed, the labels every half second.
    void TickGamePrices()
    {
        const ULONGLONG now = GetTickCount64();

        if (now < g_gamePricesAt)
            return;

        g_gamePricesAt = now + 500;

        if (!g_gamePricesOn.load(std::memory_order_relaxed))
        {
            RemoveGamePriceLabels();
            return;
        }

        std::string table;
        unsigned revision = 0;
        {
            std::lock_guard lock(g_mutex);
            revision = g_gamePricesRevision;

            if (revision != g_gamePricesSent)
            {
                table = "(function(){try{var root=$.GetContextPanel();while(root.GetParent())root=root.GetParent();var d=root.Data();d.llPrices={";

                for (std::size_t i = 0; i < g_gamePrices.items.size(); ++i)
                {
                    if (i)
                        table += ',';

                    table += '"';
                    table += std::to_string(g_gamePrices.items[i].first);
                    table += "\":";
                    table += JsString(g_gamePrices.items[i].second);
                }

                table += "};d.llTotal=" + JsString(g_gamePrices.total) + ";d.llItemTitle=" + JsString(g_gamePrices.itemTitle) +
                    ";d.llTotalTitle=" + JsString(g_gamePrices.totalTitle) + ";}catch(e){}})();";
            }
        }

        if (!table.empty())
        {
            if (!RunMainMenuScript(table.c_str()))
                return;   // no main menu yet: the next pass

            g_gamePricesSent = revision;
        }

        if (g_gamePricesSent != 0 && RunMainMenuScript(GAME_PRICES_LABELS))
            g_gamePricesShown = true;
    }
}

void Unboxer::SetGamePrices(GamePrices prices)
{
    std::lock_guard lock(g_mutex);
    g_gamePrices = std::move(prices);

    if (++g_gamePricesRevision == 0)
        g_gamePricesRevision = 1;   // 0 = "nothing sent"
}

void Unboxer::SetGamePricesEnabled(bool on)
{
    g_gamePricesOn.store(on, std::memory_order_relaxed);
}

void Unboxer::SetArmoryEnabled(bool on)
{
    g_armoryOn.store(on, std::memory_order_relaxed);
}

int Unboxer::ArmoryStars()
{
    std::lock_guard lock(g_mutex);
    return g_armoryStars;
}

void Unboxer::SetArmoryStars(int stars)
{
    stars = std::clamp(stars, 0, MAX_ARMORY_STARS);
    std::lock_guard lock(g_mutex);

    if (stars != g_armoryStars)
    {
        g_armoryStars = stars;
        g_dirty = true;
    }
}

bool Unboxer::OnRedeemArmory(int season, int index, int points)
{
    if (!g_armoryOn.load(std::memory_order_relaxed))
        return false;   // Valve's Armory, as before

    const bool russian = Config::menu.language == 1;
    const std::vector<Cases::ArmoryReward>& rewards = Cases::ArmoryRewards();
    std::string why;
    std::string name;
    int cost = 0;

    {
        std::lock_guard lock(g_mutex);

        if (!Cases::Ready() || season != Cases::ArmorySeason() || index < 0 || index >= static_cast<int>(rewards.size()))
        {
            why = russian ? "этой награды нет в файлах игры" : "this reward isn't in the game files";
        }
        else
        {
            const Cases::ArmoryReward& reward = rewards[static_cast<std::size_t>(index)];
            name = reward.itemName;
            cost = reward.points;

            if (reward.bid)
                why = russian ? "ставки на награды Lootline не делает" : "bidding rewards aren't done by Lootline";
            else if (reward.crate == 0 && reward.pool.drops.empty())
                why = russian ? "Lootline не знает, что в этой награде" : "Lootline doesn't know what this reward holds";
            else if (g_armoryStars < reward.points)
                why = russian ? "не хватает звёзд (Настройки -> Арсенал)" : "not enough stars (Settings -> Armory)";
        }

        if (why.empty())
        {
            g_armoryStars -= cost;
            g_dirty = true;
            g_armoryJobs.push_back({ GetTickCount64() + ARMORY_RESULT_DELAY_MS, index });
        }
        else
        {
            g_popupMessages.push_back(std::string(russian ? "Арсенал Lootline: " : "Lootline's Armory: ") + why + ".");
            g_armoryJobs.push_back({ GetTickCount64() + 200, -1 });   // the screen stops waiting
        }
    }

    if (why.empty())
        Log("armory: redeem %s (season %d, reward %d) for %d stars (the screen said %d)", name.c_str(), season, index, cost, points);
    else
        Log("armory: redeem refused (season %d, reward %d, %s): %s", season, index, name.c_str(), why.c_str());

    return true;   // never sent to Valve while Lootline's Armory is on
}

bool Unboxer::OnCraftIngredients(void* api)
{
    std::vector<std::uint64_t> ids;

    if (!GameInv::CraftIngredients(api, ids) || ids.empty())
        return false;   // unreadable / empty: the game's, as before

    const bool russian = Config::menu.language == 1;
    std::size_t ours = 0;
    std::string why;
    Item output;

    {
        std::lock_guard lock(g_mutex);

        for (const std::uint64_t id : ids)
            if (FindLocked(id))
                ++ours;

        if (ours == 0)
            return false;   // real items: Valve's contract

        if (ours != ids.size())
        {
            g_popupMessages.push_back(russian ? "Контракт обмена не может смешивать предметы Lootline с настоящими. Используйте только одни или только другие." :
                "A trade-up contract can't mix Lootline items with real ones. Use only one or the other.");
            Log("trade-up: %zu Lootline + %zu real items - can't be mixed, nothing done", ours, ids.size() - ours);
            return true;
        }

        for (const PendingTradeUp& pending : g_tradeUps)
        {
            if (pending.inputs == ids)
                return true;   // already submitted (double click)
        }

        why = RollTradeUpLocked(ids, output);

        if (why.empty())
        {
            PendingTradeUp job;
            job.at = GetTickCount64() + TRADE_UP_RESULT_DELAY_MS;
            job.api = api;
            job.inputs = ids;
            job.output = output;
            g_tradeUps.push_back(std::move(job));
        }
        else
        {
            g_popupMessages.push_back(std::string(russian ? "Lootline не может выполнить этот контракт: " : "Lootline can't do this contract: ") + why + ".");
        }
    }

    if (!why.empty())
    {
        Log("trade-up: %zu Lootline items - not done (%s)", ids.size(), why.c_str());
        return true;
    }

    Log("trade-up: %zu Lootline items submitted - done locally", ids.size());
    return true;
}

bool Unboxer::OnWearSticker(const char* itemId, int slot, int wear)
{
    const std::uint64_t id = itemId ? _strtoui64(itemId, nullptr, 10) : 0;

    if (id < ID_BASE || slot < 0 || slot >= STICKER_SLOTS)
        return false;   // not ours

    std::lock_guard lock(g_mutex);
    const Item* gun = FindLocked(id);

    if (!gun || gun->kind != Kind::Drop || gun->stickers[slot].kit <= 0)
        return false;

    for (const PendingSticker& pending : g_stickerJobs)
    {
        if (pending.gunId == id && pending.slot == slot)
            return true;   // already on it
    }

    PendingSticker job;
    job.at = GetTickCount64() + STICKER_RESULT_DELAY_MS;
    job.apply = false;
    job.gunId = id;
    job.slot = slot;
    job.wear = wear;
    job.patch = IsAgent(gun->definition);

    if (job.patch)
        job.wear = 101;   // a patch comes off whole (the patch popup sends 0)

    g_stickerJobs.push_back(job);

    Log("%s: %s slot %d of %llu (%d) - locally", job.patch ? "patch" : "sticker", job.wear > 100 ? "taking off" : "scraping", slot,
        static_cast<unsigned long long>(id), wear);
    return true;
}

bool Unboxer::OnRemoveKeychain(const char* itemId, int)
{
    const std::uint64_t id = itemId ? _strtoui64(itemId, nullptr, 10) : 0;

    if (id < ID_BASE)
        return false;   // not ours

    std::lock_guard lock(g_mutex);
    const Item* gun = FindLocked(id);

    // "Crack Slab and Extract Sticker" on one of our slab charms: RemoveKeychain(slab, 0)
    if (gun && gun->kind == Kind::Drop && gun->definition == DEF_CHARM && gun->charmSticker > 0)
    {
        for (const PendingSlab& pending : g_slabJobs)
        {
            if (pending.slabId == id)
                return true;   // already on it
        }

        PendingSlab job;
        job.at = GetTickCount64() + STICKER_RESULT_DELAY_MS;
        job.seal = false;
        job.slabId = id;
        job.ourSlab = true;
        job.kit = gun->charmSticker;
        g_slabJobs.push_back(job);
        Log("slab: cracking %llu (sticker %d) - locally", static_cast<unsigned long long>(id), job.kit);
        return true;
    }

    if (!gun || gun->kind != Kind::Drop || gun->charm <= 0)
        return false;

    for (const PendingCharm& pending : g_charmJobs)
    {
        if (pending.gunId == id)
            return true;   // already on it
    }

    PendingCharm job;
    job.at = GetTickCount64() + STICKER_RESULT_DELAY_MS;
    job.apply = false;
    job.gunId = id;
    g_charmJobs.push_back(job);

    // one of Lootline's Charm Detachments is used (CS2 lets you detach only with one; real ones aren't touched)
    if (g_charmDetachments > 0)
    {
        --g_charmDetachments;
        g_dirty = true;
    }

    Log("charm: taking %d off %llu - locally (%d Lootline Charm Detachments left)", gun->charm, static_cast<unsigned long long>(id), g_charmDetachments);
    return true;
}

void Unboxer::OnNameToolString(const char* text)
{
    std::lock_guard lock(g_mutex);
    g_nameToolText = text ? std::string(text).substr(0, 160) : std::string();
}

bool Unboxer::OnDeleteItem(const char* itemId)
{
    std::uint16_t fauxDefinition = 0;
    int fauxPaintKit = 0;
    const std::uint64_t id = itemId ? _strtoui64(itemId, nullptr, 10) : 0;

    if (id < ID_BASE || FromFauxId(id, fauxDefinition, fauxPaintKit))
        return false;   // a real item (Valve deletes it) / a display-only one

    // every id from ID_BASE up is Lootline's (Valve's are far below): one that isn't in the list any more is removed too
    std::string name = "an item Lootline no longer lists";
    {
        std::lock_guard lock(g_mutex);
        if (const Item* item = FindLocked(id))
            name = DisplayName(*item);
    }

    Remove(id);
    Log("delete: %s (%llu) - CS2's Delete, removed locally", name.c_str(), static_cast<unsigned long long>(id));
    return true;
}

bool Unboxer::OnAcknowledgeItem(const char* itemId, bool& wasNew)
{
    wasNew = false;

    if (!IsLootlineItemId(itemId))
        return false;   // a real item: CS2 tells Valve

    // a Lootline id never goes to Valve, found or not
    const std::uint64_t id = _strtoui64(itemId, nullptr, 10);
    const bool found = GameInv::Acknowledge(id, wasNew);

    if (wasNew)
        Log("new item acknowledged locally: %llu", static_cast<unsigned long long>(id));
    else if (!found)
        Log("new item acknowledge: %llu isn't in the inventory (nothing sent)", static_cast<unsigned long long>(id));

    return true;
}

bool Unboxer::OnUseToolWithIntArg(const char* toolId, const char* itemId, int value)
{
    const std::uint64_t id = itemId ? _strtoui64(itemId, nullptr, 10) : 0;

    if (id < ID_BASE)
        return false;   // not ours

    std::lock_guard lock(g_mutex);
    const Item* terminal = FindLocked(id);

    if (!IsUnsealedTerminal(terminal))
        return false;

    if (TerminalJobPendingLocked(id))
        return true;   // already on it

    // the laptop asks with the number it has; after the final offer (negative) this is its "end" (it only offers that then)
    const bool end = terminal->offerNumber < 0;
    g_terminalJobs.push_back({ GetTickCount64() + OPEN_RESULT_DELAY_MS, end ? TerminalJob::What::End : TerminalJob::What::Offer, id });
    Log("terminal: %llu asks for %s (UseToolWithIntArg %s, %d) - locally", static_cast<unsigned long long>(id),
        end ? "the end" : "an offer", toolId ? toolId : "", value);
    return true;
}

bool Unboxer::OnCasketTransaction(int operation, const char* casketId, const char* itemId)
{
    const std::uint64_t casket = casketId ? _strtoui64(casketId, nullptr, 10) : 0;
    const std::uint64_t item = itemId ? _strtoui64(itemId, nullptr, 10) : 0;

    if (casket < ID_BASE && item < ID_BASE)
        return false;   // real items: Valve's

    std::lock_guard lock(g_mutex);

    // a reopened terminal asking for its offer (PerformItemCasketTransaction(0, terminal, terminal))
    if (operation == 0 && casket == item && IsUnsealedTerminal(FindLocked(casket)))
    {
        if (!TerminalJobPendingLocked(casket))
            g_terminalJobs.push_back({ GetTickCount64() + STICKER_RESULT_DELAY_MS, TerminalJob::What::Resume, casket });

        Log("terminal: %llu reopened - its offer again, locally", static_cast<unsigned long long>(casket));
        return true;
    }

    const bool russian = Config::menu.language == 1;

    // a Lootline Storage Unit: 1 put in, -1 take out, 0 look inside - done locally, CS2's popup gets its answer
    if (const Item* unit = FindLocked(casket); unit && unit->kind == Kind::Drop && unit->definition == DEF_CASKET)
    {
        const Item* subject = operation != 0 ? FindLocked(item) : nullptr;
        std::string why;

        if (unit->modified == 0)
            why = russian ? "сначала дайте хранилищу имя" : "name the Storage Unit first";
        else if (operation == 1 && !subject)
            why = russian ? "настоящий предмет не кладётся в хранилище Lootline" : "a real item can't go in a Lootline Storage Unit";
        else if (operation == 1 && subject->definition == DEF_CASKET)
            why = russian ? "хранилище не кладётся в другое хранилище" : "a Storage Unit can't go in another one";
        else if (operation == 1 && subject->casket != 0)
            why = russian ? "он уже в хранилище" : "it's in a Storage Unit already";
        else if (operation == -1 && (!subject || subject->casket != casket))
            why = russian ? "его нет в этом хранилище" : "it isn't in this Storage Unit";
        else if (operation != 1 && operation != -1 && operation != 0)
            why = "unknown operation";

        if (why.empty())
        {
            g_casketJobs.push_back({ GetTickCount64() + STICKER_RESULT_DELAY_MS, operation, casket, item });
            Log("storage: %s %llu (unit %llu) - locally", operation == 1 ? "put in" : operation == -1 ? "take out" : "look inside",
                static_cast<unsigned long long>(item), static_cast<unsigned long long>(casket));
        }
        else
        {
            g_popupMessages.push_back(std::string(russian ? "Хранилище Lootline: " : "Lootline's Storage Unit: ") + why + ".");
            Log("storage: refused (%d, unit %llu, item %llu): %s", operation, static_cast<unsigned long long>(casket),
                static_cast<unsigned long long>(item), why.c_str());
        }

        return true;
    }

    // a Lootline item and one of your real Storage Units: Valve doesn't know the item
    if (item >= ID_BASE)
        g_popupMessages.push_back(russian ? "Предметы Lootline кладутся только в хранилища Lootline (Добавить предметы -> Инструменты)." :
            "Lootline items go in Lootline Storage Units only (Add items -> Tools).");

    Log("storage: PerformItemCasketTransaction(%d, %s, %s) with a Lootline item - not sent", operation, casketId ? casketId : "", itemId ? itemId : "");
    return true;
}

namespace
{
    // CS2's case popup buys keys as "<key>,<key>,..." - the key's faux id (0xF000000000000000 | definition) once per key.
    // Only a purchase of nothing but one kind of case key is Lootline's: anything else goes on as it was.
    bool QueueKeyPurchase(const char* items)
    {
        std::uint16_t key = 0;
        int count = 0;

        for (const char* p = items; *p;)
        {
            while (*p == ' ' || *p == ',')
                ++p;

            if (!*p)
                break;

            if (!std::isdigit(static_cast<unsigned char>(*p)))
                return false;   // "def(id)" (a terminal's accept) or anything else

            char* end = nullptr;
            const std::uint64_t id = _strtoui64(p, &end, 10);
            std::uint16_t definition = 0;
            int paintKit = 0;

            if (!end || end == p || (*end != ',' && *end != '\0' && *end != ' ') || !Unboxer::FromFauxId(id, definition, paintKit) ||
                paintKit != 0 || !Cases::FindKey(definition) || (key != 0 && definition != key))
                return false;

            key = definition;
            ++count;
            p = end;
        }

        if (key == 0 || count <= 0 || count > MAX_KEYS_PER_PURCHASE)
            return false;

        std::lock_guard lock(g_mutex);
        g_keyPurchases.push_back({ GetTickCount64() + KEY_PURCHASE_DELAY_MS, key, count });
        Log("store: %d x key %u bought with Lootline (never sent to Steam)", count, key);
        return true;
    }

    // Main thread (GameTick): the keys made (new items, "purchased"), the spending counted, CS2's popup told the way the
    // store tells it (PanoramaComponent_Store_PurchaseCompleted with a new key's id: the case popup takes it and shows Unlock).
    void FinishKeyPurchases()
    {
        const ULONGLONG now = GetTickCount64();
        std::vector<KeyPurchase> due;
        {
            std::lock_guard lock(g_mutex);

            for (auto it = g_keyPurchases.begin(); it != g_keyPurchases.end();)
            {
                if (now >= it->at)
                {
                    due.push_back(*it);
                    it = g_keyPurchases.erase(it);
                }
                else
                {
                    ++it;
                }
            }
        }

        for (const KeyPurchase& purchase : due)
        {
            std::uint64_t first = 0;
            const int made = CreateMany(Unboxer::Kind::Key, purchase.key, purchase.count, &first);

            if (made > 0)
            {
                std::lock_guard lock(g_mutex);
                g_keySpent += KEY_STORE_PRICE_USD * made;
                g_keysBought += made;
                g_dirty = true;
            }

            char idText[32];
            snprintf(idText, sizeof(idText), "%llu", static_cast<unsigned long long>(first));
            bool handled = false;
            const bool notified = made > 0 && NotifyPurchaseCompleted(idText, handled);

            Log("store: %d of %d x key %u made (first %llu), popup %s", made, purchase.count, purchase.key,
                static_cast<unsigned long long>(first), notified ? "told" : "NOT told");
        }
    }
}

void Unboxer::SetBuyKeys(bool on)
{
    g_buyKeys.store(on, std::memory_order_relaxed);
}

Unboxer::KeySpending Unboxer::GetKeySpending()
{
    std::lock_guard lock(g_mutex);
    return { g_keySpent, g_keysBought };
}

void Unboxer::ResetKeySpending()
{
    std::lock_guard lock(g_mutex);
    g_keySpent = 0.0;
    g_keysBought = 0;
    g_dirty = true;
}

bool Unboxer::OnStoreItemPurchase(const char* items)
{
    if (!items)
        return false;

    // "Buy keys with Lootline": a purchase of case keys only - Lootline keys, never Steam
    if (g_buyKeys.load(std::memory_order_relaxed) && QueueKeyPurchase(items))
        return true;

    // "def(id),def(id)...": a Lootline id anywhere and it never goes to Steam
    std::vector<std::uint64_t> ours;

    for (const char* open = std::strchr(items, '('); open; open = std::strchr(open + 1, '('))
    {
        const std::uint64_t id = _strtoui64(open + 1, nullptr, 10);
        std::uint16_t fauxDefinition = 0;
        int fauxPaintKit = 0;

        if (id >= ID_BASE && !FromFauxId(id, fauxDefinition, fauxPaintKit))
            ours.push_back(id);
    }

    if (ours.empty())
        return false;   // a real purchase: the game's

    std::lock_guard lock(g_mutex);

    if (ours.size() == 1 && IsUnsealedTerminal(FindLocked(ours.front())))
    {
        const std::uint64_t id = ours.front();
        const bool hasOffer = std::any_of(g_terminalOffers.begin(), g_terminalOffers.end(), [id](const TerminalOffer& entry) { return entry.terminalId == id; });

        if (hasOffer)
        {
            if (!TerminalJobPendingLocked(id))
                g_terminalJobs.push_back({ GetTickCount64() + TERMINAL_PURCHASE_DELAY_MS, TerminalJob::What::Purchase, id });

            Log("terminal: %llu - its offer accepted: yours, locally (never sent to Steam, free)", static_cast<unsigned long long>(id));
            return true;
        }
    }

    Log("store: StoreItemPurchase(%s) has a Lootline item - not sent to Steam", items);
    return true;
}

bool Unboxer::OnClearCustomName(const char* itemId)
{
    const std::uint64_t id = itemId ? _strtoui64(itemId, nullptr, 10) : 0;

    if (id < ID_BASE)
        return false;   // not ours

    std::lock_guard lock(g_mutex);
    const Item* item = FindLocked(id);

    if (!item || item->kind != Kind::Drop)
        return false;

    if (item->customName.empty())
        return true;   // nothing to take off (and nothing for Valve to do with it)

    for (const PendingName& pending : g_nameJobs)
    {
        if (pending.itemId == id)
            return true;
    }

    g_nameJobs.push_back({ GetTickCount64() + STICKER_RESULT_DELAY_MS, id, std::string() });   // "" = take it off
    Log("name tag: removing '%s' from %llu - locally", item->customName.c_str(), static_cast<unsigned long long>(id));
    return true;
}

void Unboxer::LogLine(const char* text)
{
    Log("%s", text ? text : "");
}

namespace
{
    // The upgrader's results, when the wheel stops (game thread).
    void FinishUpgrades()
    {
        const ULONGLONG now = GetTickCount64();
        std::vector<PendingUpgrade> due;
        {
            std::lock_guard lock(g_mutex);

            for (auto it = g_upgrades.begin(); it != g_upgrades.end();)
            {
                if (now >= it->at)
                {
                    due.push_back(*it);
                    it = g_upgrades.erase(it);
                }
                else
                {
                    ++it;
                }
            }
        }

        for (PendingUpgrade& upgrade : due)
        {
            std::string inputName;
            std::vector<std::uint64_t> present;
            {
                std::lock_guard lock(g_mutex);

                for (const std::uint64_t id : upgrade.inputIds)
                {
                    if (const Unboxer::Item* input = FindLocked(id))
                    {
                        inputName += (inputName.empty() ? "" : " + ") + Unboxer::DisplayName(*input);
                        present.push_back(id);
                    }
                }
            }

            if (present.size() != upgrade.inputIds.size())
            {
                Log("upgrade skipped: %d of its %d items are gone", static_cast<int>(upgrade.inputIds.size() - present.size()),
                    static_cast<int>(upgrade.inputIds.size()));
                continue;
            }

            for (const std::uint64_t id : present)
                GameInv::RemoveItem(id);

            bool created = false;
            if (upgrade.win)
            {
                {
                    std::lock_guard lock(g_mutex);
                    upgrade.target.id = g_nextId++;
                }

                created = GameInv::CreateItem(ToNewItem(upgrade.target));
            }

            const std::string targetName = Unboxer::DisplayName(upgrade.target);

            std::lock_guard lock(g_mutex);
            for (const std::uint64_t id : present)
                EraseLocked(id);

            if (created)
                g_items.push_back(upgrade.target);

            ++g_upgradeStats.tries;
            if (upgrade.win)
            {
                ++g_upgradeStats.wins;
                g_upgradeStats.streak = std::max(0, g_upgradeStats.streak) + 1;

                if (upgrade.multiplier > g_upgradeStats.bestMultiplier)
                {
                    g_upgradeStats.bestMultiplier = upgrade.multiplier;
                    g_upgradeStats.bestName = targetName;
                }
            }
            else
            {
                ++g_upgradeStats.losses;
                g_upgradeStats.streak = std::min(0, g_upgradeStats.streak) - 1;
            }

            g_dirty = true;
            Log("upgrade x%.2f: %s -> %s  %s%s", upgrade.multiplier, inputName.c_str(), targetName.c_str(), upgrade.win ? "WON" : "lost",
                upgrade.win && !created ? " (but the new item couldn't be made)" : "");
        }
    }
}

namespace
{
    // What an equipped item takes up on a side: a gun its own definition, the knife, the gloves, the music kit, the
    // pin. 0 = can't be equipped (stickers, patches, sealed graffiti).
    int EquipSlot(std::uint16_t definition)
    {
        switch (Cases::KindOf(definition))
        {
        case Cases::DropKind::Skin:
            if (definition == Unboxer::DEF_PET)
                return -5;  // CS2's "pet" slot (one for both sides): the chicken next to you in the main menu
            if (definition == Unboxer::DEF_CHARM || definition == Unboxer::DEF_CHICKEN_EGG || definition == Unboxer::DEF_CHICKEN_FEED)
                return 0;   // not equippable
            if (IsAgent(definition))
                return -4;
            if (definition == Unboxer::DEF_C4)
                return -7;  // CS2's "c4" slot (T only)
            if (!IsGun(definition) && !IsKnife(definition) && !IsGloves(definition))
                return 0;   // anything else (a case-less item)
            return IsKnife(definition) ? -1 : IsGloves(definition) ? -2 : definition;
        case Cases::DropKind::MusicKit: return -3;
        case Cases::DropKind::Pin: return -8;   // pins, coins, medals: CS2's "flair0" slot (the scoreboard shows it)
        case Cases::DropKind::Graffiti:
            return definition == Cases::DEF_SPRAY_PAINT ? -6 : 0;   // unsealed graffiti: CS2's "spray0" slot (sealed: unseal it first)
        default: return 0;
        }
    }

    // A music kit (-3) / a pet (-5) / graffiti (-6) / a pin (-8): one for both sides, CS2's "noteam" loadout.
    bool NoTeam(int slot)
    {
        return slot == -3 || slot == -5 || slot == -6 || slot == -8;
    }

    // The CS2 slot a no-team item goes in: the one it was in, else "pet" for a pet, "spray0" for graffiti, "flair0" for a
    // pin ("" = CS2's default slot for it).
    std::string NoTeamSlot(const Unboxer::Item& item)
    {
        if (!item.slotT.empty())
            return item.slotT;

        const int slot = EquipSlot(item.definition);
        return slot == -5 ? std::string("pet") : slot == -6 ? std::string("spray0") : slot == -8 ? std::string("flair0") : std::string();
    }
}

bool Unboxer::CanEquip(const Item& item)
{
    return item.kind == Kind::Drop && item.casket == 0 && EquipSlot(item.definition) != 0;   // not while in a Storage Unit
}

namespace
{
    // every coloured Lootline item -> the custom colour hooks (g_mutex held)
    void PublishPaintsLocked()
    {
        std::vector<CustomColor::Entry> entries;

        for (const Unboxer::Item& item : g_items)
            if (item.kind == Unboxer::Kind::Drop && item.paint.on)
                entries.push_back({ item.id, item.definition, item.equipped, item.paint });

        CustomColor::Publish(std::move(entries));
    }
}

bool Unboxer::CanPaint(const Item& item)
{
    // a finish is what gets recoloured (a vanilla knife has no paint material)
    return item.kind == Kind::Drop && item.paintKit > 0 && (IsGun(item.definition) || IsKnife(item.definition) || IsGloves(item.definition));
}

void Unboxer::SetPaint(std::uint64_t id, const CustomColor::Paint& paint)
{
    std::lock_guard lock(g_mutex);
    Item* item = FindLocked(id);

    if (!item || !CanPaint(*item) || item->paint == paint)
        return;

    item->paint = paint;
    g_dirty = true;
    PublishPaintsLocked();   // CS2's screens and the preview: right away

    if (item->equipped)
        g_paintReapplyAt.store(GetTickCount64() + PAINT_REAPPLY_DELAY_MS, std::memory_order_release);
}

namespace
{
    // What CS2's loadout should show, done on the main thread (GameTick): put an item in (slot "" = its default
    // slot) or the default item back into a slot.
    struct LoadoutRequest
    {
        std::uint64_t id = 0;            // 0 = the default item
        std::uint8_t side = 0;           // EQUIP_T, EQUIP_CT, or both = "noteam" (music kits)
        std::string slot;
    };

    std::vector<LoadoutRequest> g_loadoutRequests;   // g_mutex

    // Which Lootline item is in which CS2 slot, per side - for the loadout screen (LoadoutShownItem). Rebuilt from
    // g_items (PublishShownSlotsLocked, g_mutex held, then this lock); read by the hooks with only this lock, so a
    // script run while g_mutex is held can still ask.
    struct ShownSlot
    {
        std::uint8_t side = 0;           // EQUIP_T, EQUIP_CT, or EQUIP_BOTH = "noteam"
        std::string slot;
        std::uint64_t id = 0;
        std::uint16_t definition = 0;
        int paintKit = 0;                // graffiti: its kit and colour (the local spray)
        int tint = 0;
    };

    std::mutex g_shownMutex;
    std::vector<ShownSlot> g_shownSlots;   // g_shownMutex
    char g_shownText[8][24]{};             // main thread: the texts handed to panorama (it copies them right away)
    int g_shownNext = 0;

    void PublishShownSlotsLocked();

    std::string& SlotFor(Unboxer::Item& item, std::uint8_t side)
    {
        return side == Unboxer::EQUIP_CT ? item.slotCT : item.slotT;
    }

    // Sets `item`'s sides (g_mutex held). Another Lootline item of the same kind loses those sides (and its CS2 slot
    // gets the default item back). queueOwn: also put `item` in / take it out of CS2's loadout.
    void SetEquippedLocked(Unboxer::Item& item, std::uint8_t sides, bool queueOwn)
    {
        const int slot = EquipSlot(item.definition);
        const bool bothAtOnce = NoTeam(slot);   // a music kit / a pet: one for both sides ("noteam")

        for (Unboxer::Item& other : g_items)
        {
            if (other.id == item.id || !(other.equipped & sides) || EquipSlot(other.definition) != slot)
                continue;

            for (const std::uint8_t side : { Unboxer::EQUIP_T, Unboxer::EQUIP_CT })
            {
                if (!(sides & side) || !(other.equipped & side))
                    continue;

                other.equipped = static_cast<std::uint8_t>(other.equipped & ~side);
                std::string& otherSlot = SlotFor(other, side);

                if (!otherSlot.empty() && !bothAtOnce)
                    g_loadoutRequests.push_back({ 0, side, otherSlot });
                otherSlot.clear();
            }
        }

        if (queueOwn)
        {
            if (bothAtOnce)
            {
                if (sides && !item.equipped)
                    g_loadoutRequests.push_back({ item.id, Unboxer::EQUIP_BOTH, NoTeamSlot(item) });
                else if (!sides && item.equipped && !NoTeamSlot(item).empty())
                    g_loadoutRequests.push_back({ 0, Unboxer::EQUIP_BOTH, NoTeamSlot(item) });
            }
            else
            {
                for (const std::uint8_t side : { Unboxer::EQUIP_T, Unboxer::EQUIP_CT })
                {
                    const bool had = item.equipped & side;
                    const bool wants = sides & side;
                    std::string& itemSlot = SlotFor(item, side);

                    if (wants && !had)
                        g_loadoutRequests.push_back({ item.id, side, std::string() });
                    else if (!wants && had && !itemSlot.empty())
                        g_loadoutRequests.push_back({ 0, side, itemSlot });

                    if (!wants)
                        itemSlot.clear();
                }
            }
        }

        item.equipped = sides;

        if (bothAtOnce && !sides)
            item.slotT.clear();

        g_dirty = true;
        g_loadoutDirty = true;
    }

    // GameTick (main thread): what the menu asked of CS2's loadout. Goes through the EquipItemInSlot hook, which
    // records the slot an item ended up in (OnLoadoutEquip).
    void ProcessLoadoutRequests()
    {
        std::vector<LoadoutRequest> requests;
        {
            std::lock_guard lock(g_mutex);
            requests.swap(g_loadoutRequests);
        }

        for (const LoadoutRequest& request : requests)
        {
            const char* team = request.side == Unboxer::EQUIP_T ? "t" : request.side == Unboxer::EQUIP_CT ? "ct" : "noteam";
            const std::string slot = request.slot.empty() ? GameInv::DefaultSlot(request.id) : request.slot;

            if (slot.empty())
                continue;

            const bool ok = GameInv::LoadoutEquip(team, request.id, slot.c_str());
            Log("loadout: %s %s %llu in %s: %s", request.id ? "equip" : "default", team, static_cast<unsigned long long>(request.id),
                slot.c_str(), ok ? "ok" : "refused by CS2");
        }
    }

    // An equipped item back into CS2's loadout on its sides (g_mutex held): after it was made again.
    void RequeueLoadoutLocked(const Unboxer::Item& item)
    {
        if (!item.equipped)
            return;

        if (NoTeam(EquipSlot(item.definition)))
        {
            g_loadoutRequests.push_back({ item.id, Unboxer::EQUIP_BOTH, NoTeamSlot(item) });
            return;
        }

        for (const std::uint8_t side : { Unboxer::EQUIP_T, Unboxer::EQUIP_CT })
            if (item.equipped & side)
                g_loadoutRequests.push_back({ item.id, side, side == Unboxer::EQUIP_CT ? item.slotCT : item.slotT });
    }

    // After the saved items are back: CS2 starts with your real loadout, so equipped Lootline items go back in.
    void QueueEquippedIntoLoadout()
    {
        std::lock_guard lock(g_mutex);

        for (const Unboxer::Item& item : g_items)
            RequeueLoadoutLocked(item);
    }
}

namespace
{
    // Equip's work (g_mutex held; `item` can be equipped): the sides it can really go on, then on / off.
    void EquipItemLocked(Unboxer::Item& item, std::uint8_t sides)
    {
        sides &= Unboxer::EQUIP_BOTH;

        if (NoTeam(EquipSlot(item.definition)) && sides != 0)
            sides = Unboxer::EQUIP_BOTH;   // a music kit / a pet is yours on both sides

        if (EquipSlot(item.definition) == -4 && sides != 0)
            sides = AgentSide(item.definition);   // an agent plays on its own side only

        if (EquipSlot(item.definition) == -7)
            sides &= Unboxer::EQUIP_T;   // the C4 is the T side's

        if (EquipSlot(item.definition) == -4 && sides == 0 && item.equipped == 0)
            return;

        SetEquippedLocked(item, sides, true);
    }

    constexpr int NOTIFICATION_CASKET_TOO_FULL = 1011;   // "casket_too_full"
    constexpr int NOTIFICATION_CASKET_ADDED = 1013;      // "casket_added" (with the unit's id)
    constexpr int NOTIFICATION_CASKET_REMOVED = 1014;    // "casket_removed"

    // Game thread: the Storage Unit requests whose answer is due - the item made again in / out of its unit (same id; CS2
    // lists an item with "casket item id" inside that unit, not in the inventory), the unit's count + date, the popup told.
    void FinishCasketJobs()
    {
        const ULONGLONG now = GetTickCount64();
        std::vector<CasketJob> due;
        {
            std::lock_guard lock(g_mutex);

            for (auto it = g_casketJobs.begin(); it != g_casketJobs.end();)
            {
                if (now >= it->at)
                {
                    due.push_back(*it);
                    it = g_casketJobs.erase(it);
                }
                else
                {
                    ++it;
                }
            }
        }

        for (const CasketJob& job : due)
        {
            int notification = NOTIFICATION_CASKET_CONTENTS;
            const char* type = "casket_contents";
            Unboxer::Item moved;
            bool remake = false;
            int count = 0;
            std::uint32_t date = 0;

            {
                std::lock_guard lock(g_mutex);
                Unboxer::Item* unit = FindLocked(job.casket);
                Unboxer::Item* item = job.operation != 0 ? FindLocked(job.item) : nullptr;

                if (!unit || unit->definition != Unboxer::DEF_CASKET)
                    continue;

                if (job.operation == 1)
                {
                    if (!item || item->casket != 0)
                        continue;

                    if (unit->casketItems >= Unboxer::CASKET_CAPACITY)
                    {
                        notification = NOTIFICATION_CASKET_TOO_FULL;
                        type = "casket_too_full";
                    }
                    else
                    {
                        if (item->equipped != 0)
                            EquipItemLocked(*item, 0);   // a stored item comes off (CS2's own default goes back in)

                        item->casket = unit->id;
                        ++unit->casketItems;
                        unit->modified = static_cast<std::uint32_t>(std::time(nullptr));
                        moved = *item;
                        remake = true;
                        notification = NOTIFICATION_CASKET_ADDED;
                        type = "casket_added";
                        g_dirty = true;
                    }
                }
                else if (job.operation == -1)
                {
                    if (!item || item->casket != unit->id)
                        continue;

                    item->casket = 0;
                    unit->casketItems = std::max(0, unit->casketItems - 1);
                    unit->modified = static_cast<std::uint32_t>(std::time(nullptr));
                    moved = *item;
                    remake = true;
                    notification = NOTIFICATION_CASKET_REMOVED;
                    type = "casket_removed";
                    g_dirty = true;
                }

                count = unit->casketItems;
                date = unit->modified;
            }

            const bool remade = !remake || (GameInv::RemoveItem(moved.id) && GameInv::CreateItem(ToNewItem(moved)));

            if (remake)
                GameInv::SetCasketInfo(job.casket, count, date);

            char idText[32];
            snprintf(idText, sizeof(idText), "%llu", static_cast<unsigned long long>(job.casket));
            bool handled = false;
            const bool notified = NotifyItemCustomization(notification, type, idText, handled);

            Log("storage: %s - unit %llu now holds %d, item %llu %s, popup %s", type, static_cast<unsigned long long>(job.casket), count,
                static_cast<unsigned long long>(job.item), remade ? "ok" : "NOT made again (back on the next start)", notified ? "told" : "NOT told");
        }
    }

    // Game thread: a Storage Unit about to go - what's in it back in the inventory (its items aren't lost with it).
    void ReleaseCasketContents(std::uint64_t casket)
    {
        std::vector<Unboxer::Item> released;
        {
            std::lock_guard lock(g_mutex);

            for (Unboxer::Item& item : g_items)
            {
                if (item.casket == casket)
                {
                    item.casket = 0;
                    released.push_back(item);
                    g_dirty = true;
                }
            }
        }

        for (const Unboxer::Item& item : released)
            if (GameInv::RemoveItem(item.id))
                GameInv::CreateItem(ToNewItem(item));

        if (!released.empty())
            Log("storage: unit %llu removed - its %zu items are back in your inventory", static_cast<unsigned long long>(casket), released.size());
    }

    constexpr int NOTIFICATION_STATTRAK_SWAP = 1088;     // "stattrack_swap" (Valve's spelling)

    // UseTool(our tool, "") (main thread): a Lootline StatTrak Swap Tool / Charm Detachment Pack - done a moment later. Not one
    // of those: false. A swap needs two of our StatTrak items of the same weapon (CS2's popup picks them).
    bool QueueToolUse(std::uint64_t toolId, std::uint64_t subject)
    {
        if (subject != 0 && subject != toolId)
            return false;

        const bool russian = Config::menu.language == 1;
        std::lock_guard lock(g_mutex);
        const Unboxer::Item* tool = FindLocked(toolId);

        if (!tool || tool->kind != Unboxer::Kind::Drop || (tool->definition != Unboxer::DEF_STATTRAK_SWAP && tool->definition != Unboxer::DEF_CHARM_DETACH_PACK))
            return false;

        for (const ToolJob& pending : g_toolJobs)
            if (pending.tool == toolId)
                return true;   // already on it (double click)

        ToolJob job;
        job.at = GetTickCount64() + STICKER_RESULT_DELAY_MS;
        job.tool = toolId;

        if (tool->definition == Unboxer::DEF_STATTRAK_SWAP)
        {
            const Unboxer::Item* first = FindLocked(g_swapFirst);
            const Unboxer::Item* second = FindLocked(g_swapSecond);
            std::string why;

            if (!first || !second)
                why = russian ? "оба предмета должны быть из Lootline" : "both items have to be Lootline's";
            else if (!first->statTrak || !second->statTrak || first->id == second->id)
                why = russian ? "нужны два разных предмета StatTrak" : "it takes two different StatTrak items";
            else if (first->definition != second->definition)
                why = russian ? "предметы должны быть одного типа" : "the items have to be the same weapon";

            if (!why.empty())
            {
                g_popupMessages.push_back(std::string(russian ? "Обмен StatTrak: " : "StatTrak Swap: ") + why + ".");
                Log("stattrak swap: refused (%llu <-> %llu): %s", static_cast<unsigned long long>(g_swapFirst), static_cast<unsigned long long>(g_swapSecond), why.c_str());
                return true;   // never Valve's: the tool is ours
            }

            job.first = g_swapFirst;
            job.second = g_swapSecond;
        }

        g_toolJobs.push_back(job);
        return true;
    }

    // Game thread: the tools whose answer is due. The swap: the two counts change places (a "count my real kills" item's
    // count is the one the StatTrak tracker has, and stays counting), both items made again, the tool used up.
    // The pack: +3 Charm Detachments, the pack used up.
    void FinishToolJobs()
    {
        const ULONGLONG now = GetTickCount64();
        std::vector<ToolJob> due;
        {
            std::lock_guard lock(g_mutex);

            for (auto it = g_toolJobs.begin(); it != g_toolJobs.end();)
            {
                if (now >= it->at)
                {
                    due.push_back(*it);
                    it = g_toolJobs.erase(it);
                }
                else
                {
                    ++it;
                }
            }
        }

        for (const ToolJob& job : due)
        {
            Unboxer::Item first, second;
            bool swap = false, pack = false;

            {
                std::lock_guard lock(g_mutex);
                const Unboxer::Item* tool = FindLocked(job.tool);

                if (!tool)
                    continue;

                if (tool->definition == Unboxer::DEF_CHARM_DETACH_PACK)
                {
                    g_charmDetachments += Unboxer::CHARM_DETACHMENTS_PER_PACK;
                    pack = true;
                }
                else
                {
                    Unboxer::Item* a = FindLocked(job.first);
                    Unboxer::Item* b = FindLocked(job.second);

                    if (!a || !b)
                        continue;

                    const int countA = a->kills >= 0 ? a->kills : StatTrakTracker::Get(a->definition, a->paintKit);
                    const int countB = b->kills >= 0 ? b->kills : StatTrakTracker::Get(b->definition, b->paintKit);

                    if (a->kills >= 0)
                        a->kills = countB;
                    else
                        StatTrakTracker::Set(a->definition, a->paintKit, countB);

                    if (b->kills >= 0)
                        b->kills = countA;
                    else
                        StatTrakTracker::Set(b->definition, b->paintKit, countA);

                    first = *a;
                    second = *b;
                    swap = true;
                    g_loadoutDirty = true;   // the counts show in a match
                }

                EraseLocked(job.tool);   // used up
                g_dirty = true;
            }

            GameInv::RemoveItem(job.tool);

            char idText[32];
            bool handled = false;

            if (pack)
            {
                snprintf(idText, sizeof(idText), "%llu", static_cast<unsigned long long>(job.tool));
                const bool notified = NotifyItemCustomization(0, "keychain_tool_charges", idText, handled);
                Log("charm detachments: a pack used - %d now, popup %s", Unboxer::CharmDetachments(), notified ? "told" : "NOT told");
                continue;
            }

            if (swap)
            {
                const bool remade = GameInv::RemoveItem(first.id) && GameInv::CreateItem(ToNewItem(first)) &&
                    GameInv::RemoveItem(second.id) && GameInv::CreateItem(ToNewItem(second));
                snprintf(idText, sizeof(idText), "%llu", static_cast<unsigned long long>(first.id));
                const bool notified = NotifyItemCustomization(NOTIFICATION_STATTRAK_SWAP, "stattrack_swap", idText, handled);
                Log("stattrak swap: %llu <-> %llu, items %s, popup %s", static_cast<unsigned long long>(first.id), static_cast<unsigned long long>(second.id),
                    remade ? "made again" : "NOT made again (right on the next start)", notified ? "told" : "NOT told");
            }
        }
    }
}

namespace
{
    // ── loadout shuffle (CS2's own: a slot with shuffle on holds your "favorites"; the server rolls one each round) ──
    // LoadoutAPI natives (their `this` isn't read): IsShuffleEnabled(team, slot), GetShuffleItems(out CUtlString, team, slot)
    // ('["id","id"]'), GetSlotEquippedWithDefIndex(team, definition) - where a gun sits. tier0 frees the CUtlString.
    using ShuffleEnabledFn = bool(__fastcall*)(void* api, const char* team, const char* slot);
    using ShuffleItemsFn = void(__fastcall*)(void* api, void* out, const char* team, const char* slot);
    using SlotWithDefinitionFn = const char*(__fastcall*)(void* api, const char* team, int definition);
    using UtlStringPurgeFn = void(__fastcall*)(void* self);

    ShuffleEnabledFn g_isShuffleEnabled = nullptr;
    ShuffleItemsFn g_getShuffleItems = nullptr;
    SlotWithDefinitionFn g_slotWithDefinition = nullptr;
    UtlStringPurgeFn g_utlStringPurge = nullptr;
    bool g_shuffleResolved = false;

    void ResolveShuffle()
    {
        if (g_shuffleResolved)
            return;

        g_shuffleResolved = true;
        g_isShuffleEnabled = reinterpret_cast<ShuffleEnabledFn>(GameInv::InventoryApiFunction("IsShuffleEnabled"));
        g_getShuffleItems = reinterpret_cast<ShuffleItemsFn>(GameInv::InventoryApiFunction("GetShuffleItems"));
        g_slotWithDefinition = reinterpret_cast<SlotWithDefinitionFn>(GameInv::InventoryApiFunction("GetSlotEquippedWithDefIndex"));

        if (const HMODULE tier0 = GetModuleHandleA("tier0.dll"))
            g_utlStringPurge = reinterpret_cast<UtlStringPurgeFn>(GetProcAddress(tier0, "?Purge@CUtlString@@QEAAXXZ"));
    }

    const char* CallSlotWithDefinition(const char* team, int definition)
    {
        __try
        {
            return g_slotWithDefinition(nullptr, team, definition);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return nullptr;
        }
    }

    bool CallShuffleEnabled(const char* team, const char* slot)
    {
        __try
        {
            return g_isShuffleEnabled(nullptr, team, slot);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    // The slot's shuffle as the game lists it (Lootline's added by the hook): its ids' text, "" if unreadable.
    bool CallShuffleItems(const char* team, const char* slot, char* text, std::size_t size)
    {
        __try
        {
            void* utlString = nullptr;   // CUtlString: one heap pointer
            g_getShuffleItems(nullptr, &utlString, team, slot);
            const char* value = static_cast<const char*>(utlString);
            strncpy_s(text, size, value ? value : "", _TRUNCATE);

            if (utlString && g_utlStringPurge)
                g_utlStringPurge(&utlString);

            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            text[0] = '\0';
            return false;
        }
    }

    const char* TeamName(std::uint8_t side)
    {
        return side == Unboxer::EQUIP_T ? "t" : side == Unboxer::EQUIP_CT ? "ct" : "noteam";
    }

    std::uint8_t SideOfTeamName(const char* team)
    {
        if (!team)
            return 0;

        return std::strcmp(team, "t") == 0 ? Unboxer::EQUIP_T : std::strcmp(team, "ct") == 0 ? Unboxer::EQUIP_CT :
            std::strcmp(team, "noteam") == 0 ? Unboxer::EQUIP_BOTH : 0;
    }

    // The CS2 loadout slot one of our items goes in on a team ("" = none). Main thread: a gun's comes from the game.
    std::string ShuffleSlotOf(std::uint16_t definition, const char* team)
    {
        switch (EquipSlot(definition))
        {
        case 0: case -5: case -6: case -8: return {};
        case -1: return "melee";
        case -2: return "clothing_hands";
        case -3: return "musickit";
        case -4: return "customplayer";
        case -7: return "c4";
        default: break;
        }

        ResolveShuffle();
        const char* slot = g_slotWithDefinition ? CallSlotWithDefinition(team, definition) : nullptr;
        return slot ? std::string(slot) : std::string();
    }

    // Main thread, after a spawn: every slot with shuffle on that has Lootline favorites rolls one of all its items (Lootline's
    // and the game's own, each the same chance) - one of ours goes on that side, else ours come off it (the real one shows).
    void RollShuffles()
    {
        ResolveShuffle();

        if (!g_isShuffleEnabled || !g_getShuffleItems)
            return;

        struct Candidate
        {
            std::uint64_t id;
            std::uint16_t definition;
            std::uint8_t sides;
        };

        std::vector<Candidate> favorites;
        {
            std::lock_guard lock(g_mutex);

            for (const Favorite& favorite : g_favorites)
                if (const Unboxer::Item* item = FindLocked(favorite.id); item && Unboxer::CanEquip(*item))
                    favorites.push_back({ favorite.id, item->definition, favorite.sides });
        }

        for (const std::uint8_t side : { Unboxer::EQUIP_T, Unboxer::EQUIP_CT, Unboxer::EQUIP_BOTH })
        {
            const char* team = TeamName(side);
            std::vector<std::pair<std::string, std::uint64_t>> bySlot;   // slot, one of ours

            for (const Candidate& candidate : favorites)
            {
                const bool noTeam = NoTeam(EquipSlot(candidate.definition));

                if ((side == Unboxer::EQUIP_BOTH) != noTeam || (side != Unboxer::EQUIP_BOTH && !(candidate.sides & side)))
                    continue;

                if (const std::string slot = ShuffleSlotOf(candidate.definition, team); !slot.empty())
                    bySlot.emplace_back(slot, candidate.id);
            }

            std::vector<std::string> slots;
            for (const auto& [slot, id] : bySlot)
                if (std::find(slots.begin(), slots.end(), slot) == slots.end())
                    slots.push_back(slot);

            for (const std::string& slot : slots)
            {
                if (!CallShuffleEnabled(team, slot.c_str()))
                    continue;

                std::vector<std::uint64_t> ours;
                for (const auto& [entrySlot, id] : bySlot)
                    if (entrySlot == slot)
                        ours.push_back(id);

                // the game's own items in it: the list's ids that aren't ours
                char text[4096];
                int real = 0;
                if (CallShuffleItems(team, slot.c_str(), text, sizeof(text)))
                    for (const char* p = std::strchr(text, '"'); p; p = std::strchr(p + 1, '"'))
                        if (std::isdigit(static_cast<unsigned char>(p[1])))
                            if (_strtoui64(p + 1, nullptr, 10) < ID_BASE)
                                ++real;

                // Only a pick that changes something is equipped: an equip re-applies every skin in a match, and the same
                // item again (or "ours off" when none of ours is on) did that at every spawn for nothing (2026-10-19).
                std::size_t pick = 0;
                bool changed = false;
                {
                    std::lock_guard lock(g_mutex);
                    pick = static_cast<std::size_t>(Random01() * static_cast<double>(ours.size() + static_cast<std::size_t>(real)));
                    pick = std::min(pick, ours.size() + static_cast<std::size_t>(real) - 1);
                    const std::uint8_t sideMask = side == Unboxer::EQUIP_BOTH ? Unboxer::EQUIP_BOTH : side;

                    if (pick < ours.size())
                    {
                        if (Unboxer::Item* item = FindLocked(ours[pick]); item && Unboxer::CanEquip(*item) && (item->equipped & sideMask) != sideMask)
                        {
                            EquipItemLocked(*item, static_cast<std::uint8_t>(item->equipped | sideMask));
                            changed = true;
                        }
                    }
                    else
                    {
                        // a real one this round: ours in this slot come off that side (only the ones on it)
                        for (Unboxer::Item& item : g_items)
                            if ((item.equipped & sideMask) != 0 && std::find(ours.begin(), ours.end(), item.id) != ours.end())
                            {
                                EquipItemLocked(item, side == Unboxer::EQUIP_BOTH ? 0 : static_cast<std::uint8_t>(item.equipped & ~side));
                                changed = true;
                            }
                    }
                }

                Log("shuffle: %s %s - %zu of Lootline's + %d real, this %s %s%s", team, slot.c_str(), ours.size(), real,
                    g_shuffleEveryRound.load(std::memory_order_relaxed) ? "round" : "match",
                    pick < ours.size() ? "a Lootline item" : "a real one", changed ? "" : " (already on: nothing changed)");
            }
        }
    }
}

bool Unboxer::OnFavorite(const char* team, const char* itemId, bool add)
{
    const std::uint64_t id = itemId ? _strtoui64(itemId, nullptr, 10) : 0;
    const std::uint8_t side = SideOfTeamName(team);

    if (!IsLootlineId(id))
        return false;   // a real item: Valve's

    std::lock_guard lock(g_mutex);
    const Item* item = FindLocked(id);

    if (!item || !side)
        return true;   // ours but unknown / an unknown team: nothing for Valve either

    auto favorite = std::find_if(g_favorites.begin(), g_favorites.end(), [id](const Favorite& entry) { return entry.id == id; });

    if (add)
    {
        if (favorite == g_favorites.end())
            g_favorites.push_back({ id, side });
        else
            favorite->sides = static_cast<std::uint8_t>(favorite->sides | side);
    }
    else if (favorite != g_favorites.end())
    {
        favorite->sides = static_cast<std::uint8_t>(favorite->sides & ~side);

        if (favorite->sides == 0)
            g_favorites.erase(favorite);
    }

    g_dirty = true;
    Log("shuffle: %s %llu %s (%s)", DisplayName(*item).c_str(), static_cast<unsigned long long>(id), add ? "added" : "taken out", team ? team : "");
    return true;
}

int Unboxer::IsFavorite(const char* team, const char* itemId)
{
    const std::uint64_t id = itemId ? _strtoui64(itemId, nullptr, 10) : 0;

    if (!IsLootlineId(id))
        return -1;

    const std::uint8_t side = SideOfTeamName(team);
    std::lock_guard lock(g_mutex);

    for (const Favorite& favorite : g_favorites)
        if (favorite.id == id)
            return side && (favorite.sides & side) == side ? 1 : 0;

    return 0;
}

std::string Unboxer::ShuffleExtra(const char* team, const char* slot)
{
    const std::uint8_t side = SideOfTeamName(team);

    if (!side || !slot || !*slot)
        return {};

    std::vector<std::pair<std::uint64_t, std::uint16_t>> mine;
    {
        std::lock_guard lock(g_mutex);

        for (const Favorite& favorite : g_favorites)
            if ((favorite.sides & side) == side)
                if (const Item* item = FindLocked(favorite.id); item && CanEquip(*item))
                    mine.emplace_back(favorite.id, item->definition);
    }

    std::string out;

    for (const auto& [id, definition] : mine)
    {
        if (ShuffleSlotOf(definition, team) != slot)
            continue;

        out += out.empty() ? "\"" : ",\"";
        out += std::to_string(id);
        out += '"';
    }

    return out;
}

void Unboxer::OnStatTrakSwapItems(const char* first, const char* second)
{
    std::lock_guard lock(g_mutex);
    g_swapFirst = first ? _strtoui64(first, nullptr, 10) : 0;
    g_swapSecond = second ? _strtoui64(second, nullptr, 10) : 0;
}

int Unboxer::CharmDetachments()
{
    std::lock_guard lock(g_mutex);
    return g_charmDetachments;
}

void Unboxer::Equip(std::uint64_t id, std::uint8_t sides)
{
    std::lock_guard lock(g_mutex);

    if (Item* item = FindLocked(id); item && CanEquip(*item))
        EquipItemLocked(*item, sides);
}

std::vector<Unboxer::LoadoutInfo> Unboxer::Loadouts()
{
    std::lock_guard lock(g_mutex);
    std::vector<LoadoutInfo> out;

    for (const LoadoutProfile& profile : g_loadouts)
    {
        LoadoutInfo info;
        info.name = profile.name;

        for (const auto& [id, sides] : profile.items)
            ++(FindLocked(id) ? info.items : info.missing);

        out.push_back(std::move(info));
    }

    return out;
}

bool Unboxer::SaveLoadout(const std::string& name)
{
    if (name.empty())
        return false;

    std::lock_guard lock(g_mutex);
    LoadoutProfile profile;
    profile.name = name;

    for (const Item& item : g_items)
        if (item.kind == Kind::Drop && item.equipped != 0)
            profile.items.emplace_back(item.id, item.equipped);

    if (profile.items.empty())
        return false;

    const auto same = std::find_if(g_loadouts.begin(), g_loadouts.end(), [&](const LoadoutProfile& saved) { return saved.name == name; });

    if (same != g_loadouts.end())
        *same = std::move(profile);   // the same name: saved over
    else
        g_loadouts.push_back(std::move(profile));

    g_dirty = true;
    Log("loadouts: \"%s\" saved", name.c_str());
    return true;
}

int Unboxer::ApplyLoadout(const std::string& name)
{
    std::lock_guard lock(g_mutex);
    const auto profile = std::find_if(g_loadouts.begin(), g_loadouts.end(), [&](const LoadoutProfile& saved) { return saved.name == name; });

    if (profile == g_loadouts.end())
        return 0;

    const auto inProfile = [&](std::uint64_t id)
    {
        return std::any_of(profile->items.begin(), profile->items.end(), [id](const auto& entry) { return entry.first == id; });
    };

    // what isn't in it comes off first (then the slots it held get the profile's items, or CS2's own back)
    for (Item& item : g_items)
        if (item.kind == Kind::Drop && item.equipped != 0 && !inProfile(item.id) && CanEquip(item))
            EquipItemLocked(item, 0);

    int equipped = 0;

    for (const auto& [id, sides] : profile->items)
    {
        if (Item* item = FindLocked(id); item && CanEquip(*item))
        {
            EquipItemLocked(*item, sides);
            ++equipped;
        }
    }

    Log("loadouts: \"%s\" put on - %d items (%d gone since)", name.c_str(), equipped, static_cast<int>(profile->items.size()) - equipped);
    return equipped;
}

void Unboxer::DeleteLoadout(const std::string& name)
{
    std::lock_guard lock(g_mutex);

    if (std::erase_if(g_loadouts, [&](const LoadoutProfile& saved) { return saved.name == name; }) > 0)
        g_dirty = true;
}

void Unboxer::OnLoadoutEquip(const char* team, const char* itemId, const char* slot)
{
    if (!team || !itemId || !slot || !*slot)
        return;

    const std::uint8_t sides = std::strcmp(team, "t") == 0 ? EQUIP_T : std::strcmp(team, "ct") == 0 ? EQUIP_CT : EQUIP_BOTH;
    const std::uint64_t id = _strtoui64(itemId, nullptr, 10);

    std::lock_guard lock(g_mutex);
    bool changed = false;

    // whatever Lootline item sat in this slot on these sides came out
    for (Item& item : g_items)
    {
        if (item.id == id)
            continue;

        for (const std::uint8_t side : { EQUIP_T, EQUIP_CT })
        {
            std::string& itemSlot = SlotFor(item, side);

            if ((sides & side) && !itemSlot.empty() && itemSlot == slot)
            {
                itemSlot.clear();
                item.equipped = static_cast<std::uint8_t>(item.equipped & ~side);
                changed = true;
            }
        }
    }

    // and a Lootline item went in: the skin changer puts it on your weapon on those sides
    if (Item* item = FindLocked(id); item && CanEquip(*item))
    {
        Log("loadout: %s equipped in %s (%s)", DisplayName(*item).c_str(), slot, team);

        for (const std::uint8_t side : { EQUIP_T, EQUIP_CT })
            if (sides & side)
                SlotFor(*item, side) = slot;

        SetEquippedLocked(*item, static_cast<std::uint8_t>(item->equipped | sides), false);
        changed = true;
    }

    if (changed)
    {
        g_dirty = true;
        g_loadoutDirty = true;
        PublishShownSlotsLocked();   // the loadout screen asks right after this
    }
}

bool Unboxer::ForceLoadoutEquip(const char* team, const char* itemId, const char* slot)
{
    if (!team || !itemId || !slot || !*slot)
        return false;

    const std::uint64_t id = _strtoui64(itemId, nullptr, 10);
    std::string name;

    {
        std::lock_guard lock(g_mutex);
        const Item* item = FindLocked(id);

        if (!item || !CanEquip(*item))
            return false;

        // only the slot kind the item belongs in (the loadout screen asks CanEquipItemInSlot first, but be sure)
        const std::string_view slotName(slot);
        const int kind = EquipSlot(item->definition);
        const bool fits =
            kind == -1 ? slotName == "melee" :
            kind == -2 ? slotName == "clothing_hands" :
            kind == -3 ? slotName == "musickit" :
            kind == -4 ? slotName == "customplayer" :
            kind == -5 ? slotName == "pet" :
            kind == -6 ? slotName == "spray0" :
            kind == -7 ? slotName == "c4" :
            kind == -8 ? slotName == "flair0" :
            slotName.starts_with("secondary") || slotName.starts_with("smg") || slotName.starts_with("rifle") || slotName.starts_with("heavy");

        if (!fits)
            return false;

        name = DisplayName(*item);
    }

    OnLoadoutEquip(team, itemId, slot);
    Log("loadout: %s forced into %s (%s) - CS2 locks the loadout in this match; the skin changer shows it, CS2's loadout screen "
        "catches up after the match", name.c_str(), slot, team);
    return true;
}

namespace
{
    void PublishShownSlotsLocked()
    {
        std::vector<ShownSlot> shown;

        for (const Unboxer::Item& item : g_items)
        {
            if (item.kind != Unboxer::Kind::Drop || !item.equipped)
                continue;

            if (NoTeam(EquipSlot(item.definition)))
            {
                if (const std::string slot = NoTeamSlot(item); !slot.empty())
                    shown.push_back({ Unboxer::EQUIP_BOTH, slot, item.id, item.definition, item.paintKit, item.tint });
                continue;
            }

            if ((item.equipped & Unboxer::EQUIP_T) && !item.slotT.empty())
                shown.push_back({ Unboxer::EQUIP_T, item.slotT, item.id, item.definition });
            if ((item.equipped & Unboxer::EQUIP_CT) && !item.slotCT.empty())
                shown.push_back({ Unboxer::EQUIP_CT, item.slotCT, item.id, item.definition });
        }

        std::lock_guard lock(g_shownMutex);
        g_shownSlots = std::move(shown);
    }

    std::uint8_t SideOfTeam(const char* team)
    {
        if (!team)
            return 0;
        if (std::strcmp(team, "t") == 0)
            return Unboxer::EQUIP_T;
        if (std::strcmp(team, "ct") == 0)
            return Unboxer::EQUIP_CT;
        if (std::strcmp(team, "noteam") == 0)
            return Unboxer::EQUIP_BOTH;
        return 0;
    }
}

const char* Unboxer::LoadoutShownItem(const char* team, const char* slot, const char* nativeId)
{
    const std::uint8_t side = SideOfTeam(team);

    if (!side || !slot || !*slot)
        return nullptr;

    ShownSlot ours;
    {
        std::lock_guard lock(g_shownMutex);

        for (const ShownSlot& entry : g_shownSlots)
            if (entry.side == side && entry.slot == slot)
                ours = entry;
    }

    const std::uint64_t native = nativeId ? _strtoui64(nativeId, nullptr, 10) : 0;

    if (!ours.id || ours.id == native)
        return nullptr;

    // a gun slot holds one weapon: only that weapon's Lootline skin shows (an M4A1-S forced over an M4A4 isn't what the
    // match gives you); knives, gloves, agents, music kits, pets, graffiti, the C4: whatever is in the slot
    const std::string_view name(slot);
    const bool gunSlot = name.starts_with("secondary") || name.starts_with("smg") || name.starts_with("rifle") || name.starts_with("heavy");

    if (gunSlot && native)
        if (const std::uint16_t definition = GameInv::ItemDefinition(native); definition && definition != ours.definition)
            return nullptr;

    char* text = g_shownText[g_shownNext];
    g_shownNext = (g_shownNext + 1) % 8;
    snprintf(text, sizeof(g_shownText[0]), "%llu", static_cast<unsigned long long>(ours.id));
    return text;
}

const char* Unboxer::OfferPriceText(std::uint64_t id)
{
    if (id < ID_BASE)
        return nullptr;

    int cents = 0;
    {
        std::lock_guard lock(g_offerPriceMutex);
        const auto it = g_offerPrices.find(id);

        if (it == g_offerPrices.end())
            return nullptr;

        cents = it->second;
    }

    if (cents <= 0)
        return nullptr;

    char* text = g_offerPriceText[g_offerPriceNext];
    g_offerPriceNext = (g_offerPriceNext + 1) % 4;
    snprintf(text, sizeof(g_offerPriceText[0]), "%s", Prices::Format(cents / 100.0).c_str());
    return text;
}

bool Unboxer::EquippedGraffiti(int slot, int& kit, int& tint)
{
    char name[16];
    snprintf(name, sizeof(name), "spray%d", slot);

    std::lock_guard lock(g_shownMutex);

    for (const ShownSlot& entry : g_shownSlots)
    {
        if (entry.side == EQUIP_BOTH && entry.slot == name && Cases::KindOf(entry.definition) == Cases::DropKind::Graffiti)
        {
            kit = entry.paintKit;
            tint = entry.tint;
            return true;
        }
    }

    return false;
}

void Unboxer::OnGraffitiSprayed(int slot)
{
    // CS2's own spray sounds (server.dll has both: the can rattling, then the hiss) - for a real spray the server plays them,
    // this one never reaches the server
    if (OnMainThread())
        RunMainMenuScript("UiToolkitAPI.PlaySoundEvent('SprayCan.Shake');"
            "$.Schedule(0.45,function(){UiToolkitAPI.PlaySoundEvent('SprayCan.Paint');});");

    char name[16];
    snprintf(name, sizeof(name), "spray%d", slot);
    std::uint64_t id = 0;
    {
        std::lock_guard lock(g_shownMutex);

        for (const ShownSlot& entry : g_shownSlots)
            if (entry.side == EQUIP_BOTH && entry.slot == name && entry.definition == Cases::DEF_SPRAY_PAINT)
                id = entry.id;
    }

    if (!id)
        return;

    int left = 0;
    std::string label;
    {
        std::lock_guard lock(g_mutex);
        Item* item = FindLocked(id);

        if (!item || item->definition != Cases::DEF_SPRAY_PAINT)
            return;

        item->sprays = (item->sprays > 0 ? item->sprays : SPRAY_CHARGES) - 1;
        left = item->sprays;
        label = DisplayName(*item);
        g_dirty = true;

        // the last charge: out of the loadout (CS2's slot gets its default back), then out of the inventory
        if (left <= 0)
        {
            SetEquippedLocked(*item, 0, true);
            PublishShownSlotsLocked();
            g_requests.push_back({ Request::Remove, 0, 0, false, id });
        }
    }

    if (left > 0)
    {
        GameInv::SetSprayCharges(id, left);
        Log("graffiti: %s - %d of %d charges left", label.c_str(), left, SPRAY_CHARGES);
    }
    else
    {
        Log("graffiti: %s - the last charge, it's used up (removed, like CS2's)", label.c_str());
    }
}

bool Unboxer::IsLootlineItemId(const char* itemId)
{
    return IsLootlineId(itemId ? _strtoui64(itemId, nullptr, 10) : 0);
}

bool Unboxer::IsLootlineId(std::uint64_t id)
{
    std::uint16_t fauxDefinition = 0;
    int fauxPaintKit = 0;
    return id >= ID_BASE && !FromFauxId(id, fauxDefinition, fauxPaintKit);
}

bool Unboxer::LoadoutShowsEquipped(const char* itemId, const char* team)
{
    const std::uint8_t side = SideOfTeam(team);
    const std::uint64_t id = itemId ? _strtoui64(itemId, nullptr, 10) : 0;

    if (!side || id < ID_BASE)
        return false;

    std::lock_guard lock(g_shownMutex);

    for (const ShownSlot& entry : g_shownSlots)
        if (entry.id == id && entry.side == side)
            return true;

    return false;
}

std::uint64_t Unboxer::LivePetId()
{
    return g_livePetId.load(std::memory_order_acquire);
}

void Unboxer::SyncLoadout()
{
    if (ULONGLONG at = g_handsRefreshAt.load(std::memory_order_acquire); at != 0 && GetTickCount64() >= at &&
        g_handsRefreshAt.compare_exchange_strong(at, 0, std::memory_order_acq_rel))
        SkinChanger::RefreshPlayerModel();   // only acts in a match while you're alive (never in a demo)

    // an equipped item's colours changed (Inventory -> Custom colour): everything is put on again below
    if (ULONGLONG at = g_paintReapplyAt.load(std::memory_order_acquire); at != 0 && GetTickCount64() >= at &&
        g_paintReapplyAt.compare_exchange_strong(at, 0, std::memory_order_acq_rel))
        g_loadoutDirty = true;

    if (!g_loadoutDirty.exchange(false))
        return;

    std::vector<Item> equipped;
    {
        std::lock_guard lock(g_mutex);
        for (const Item& item : g_items)
            if (item.kind == Kind::Drop && item.equipped)
                equipped.push_back(item);

        PublishShownSlotsLocked();   // the loadout screen's table too
        PublishPaintsLocked();       // the custom colours: which item is on which side
    }

    // the equipped pet is the one CS2's scripts show (the main menu chicken, the loadout, the pet book)
    std::uint64_t livePet = 0;
    for (const Item& item : equipped)
    {
        if (item.definition == DEF_PET)
        {
            livePet = item.id;
            break;
        }
    }

    if (g_livePetId.exchange(livePet) != livePet)
    {
        if (livePet)
            Log("pet: %llu is your live pet (the main menu chicken)", static_cast<unsigned long long>(livePet));
        else
            Log("pet: no Lootline pet equipped - CS2 shows its own");
    }

    // The same loadout Chadware's skin tab fills in, built from what's equipped.
    Config::skins_t skins;
    std::array<CustomColor::Paint, 2> glovePaint{};   // each side's gloves' custom colours (GloveChoice)

    for (const Item& item : equipped)
    {
        const Cases::DropKind kind = Cases::KindOf(item.definition);

        if (kind == Cases::DropKind::MusicKit)
        {
            skins.musicKit = item.paintKit;
            continue;
        }

        if (kind != Cases::DropKind::Skin || item.definition == DEF_PET)
            continue;   // a pet: CS2's own pet slot, not the skin changer

        if (IsAgent(item.definition))
        {
            for (const int team : { Config::TEAM_T, Config::TEAM_CT })
                if (item.equipped & (team == Config::TEAM_T ? EQUIP_T : EQUIP_CT))
                    skins.loadouts[team].agentDefinition = item.definition;
            continue;
        }

        Config::item_skin_t skin;
        skin.enabled = true;
        skin.paintKit = item.paintKit;
        skin.seed = item.seed;
        skin.wear = std::max(item.wear, 0.0001f);
        skin.statTrakEnabled = item.statTrak;
        skin.statTrakReal = item.kills < 0;   // -1: counts your real kills (stattrak.json); else the number you set
        skin.statTrakKills = std::max(item.kills, 0);
        skin.souvenir = item.quality == QUALITY_SOUVENIR && !item.statTrak;   // "Souvenir AWP | ..." in a match too
        skin.customName = item.customName;

        if (item.charm > 0 && CanHoldCharm(item.definition))
        {
            skin.keychain.enabled = true;
            skin.keychain.id = item.charm;
            skin.keychain.seed = item.charmSeed;
            skin.keychain.sticker = item.charmSticker;
            skin.keychain.offsetX = item.charmX;
            skin.keychain.offsetY = item.charmY;
            skin.keychain.offsetZ = item.charmZ;
        }

        for (std::size_t slot = 0; slot < skin.stickers.size() && slot < item.stickers.size(); ++slot)
        {
            const Sticker& sticker = item.stickers[slot];
            Config::skin_sticker_t& out = skin.stickers[slot];
            out.enabled = sticker.kit > 0;
            out.kit = sticker.kit;
            out.wear = sticker.wear;
            out.scale = 1.0f;
            out.rotation = sticker.rotation;
            out.offsetX = sticker.offsetX;
            out.offsetY = sticker.offsetY;
            out.schema = sticker.schema;
        }

        for (const int team : { Config::TEAM_T, Config::TEAM_CT })
        {
            if (!(item.equipped & (team == Config::TEAM_T ? EQUIP_T : EQUIP_CT)))
                continue;

            Config::skin_changer_t& loadout = skins.loadouts[team];

            if (IsKnife(item.definition))
            {
                loadout.knifeDefinition = item.definition;
                loadout.knives[item.definition] = skin;
            }
            else if (IsGloves(item.definition))
            {
                loadout.gloveDefinition = item.definition;
                loadout.gloves[item.definition] = skin;
                glovePaint[team] = item.paint;
            }
            else
            {
                loadout.weapons[item.definition] = skin;
            }
        }
    }

    skins.enabled = !equipped.empty();

    // gloves changed on the side you're playing: the hands are rebuilt too (no rejoin needed)
    std::array<GloveChoice, 2> gloves{};
    for (const int team : { Config::TEAM_T, Config::TEAM_CT })
    {
        const Config::skin_changer_t& loadout = skins.loadouts[team];
        GloveChoice& choice = gloves[team];
        choice.definition = skins.enabled ? loadout.gloveDefinition : 0;

        if (const auto it = loadout.gloves.find(choice.definition); choice.definition != 0 && it != loadout.gloves.end())
        {
            choice.paintKit = it->second.paintKit;
            choice.seed = it->second.seed;
            choice.wear = it->second.wear;
            choice.paint = glovePaint[team];
        }
    }

    const int liveTeam = Config::liveTeam.load(std::memory_order_relaxed) == Config::TEAM_CT ? Config::TEAM_CT : Config::TEAM_T;
    if (!(gloves[liveTeam] == g_appliedGloves[liveTeam]))
        g_handsRefreshAt.store(GetTickCount64() + HANDS_REFRESH_DELAY_MS, std::memory_order_release);
    g_appliedGloves = gloves;

    Config::skins = std::move(skins);
    Config::PublishSkins(true);
    SkinChanger::ForceUpdate();
    GloveChanger::ForceUpdate();
}

namespace
{
    // Settings -> "Reapply skins when you spawn": on / off and when - ms after the spawn, up to MAX_SPAWN_REAPPLIES times
    // (the menu hands them over every frame; one at 0.6 s until it does)
    constexpr int MAX_SPAWN_REAPPLIES = 8;
    std::atomic_bool g_reapplyOnSpawn{ true };
    std::atomic<std::uint32_t> g_spawnReapplyMs[MAX_SPAWN_REAPPLIES] = { 600 };
    std::atomic_int g_spawnReapplyCount{ 1 };

    // game thread
    std::uintptr_t g_spawnPawn = 0;
    float g_spawnTime = -1.0f;
    ULONGLONG g_spawnAt = 0;                 // when this spawn was seen
    std::vector<ULONGLONG> g_spawnDue;       // this spawn's reapplies still to come (soonest first)
    ULONGLONG g_spawnCheckAt = 0;            // the log's spawn check: each weapon's skin and paint, once they've settled
    int g_spawnChecks = 0;                   // logged so far (a session logs at most MAX_SPAWN_CHECKS)
    constexpr ULONGLONG SPAWN_CHECK_MIN_MS = 4000;      // at least this long after the spawn...
    constexpr ULONGLONG SPAWN_CHECK_AFTER_MS = 2000;    // ... and this long after the last reapply
    constexpr int MAX_SPAWN_CHECKS = 60;

    // Every equipped item put on again (gloves too: the player model is swapped for a frame 0.3 s later - not in a demo).
    void ReapplyNow()
    {
        g_loadoutDirty = true;   // rebuilt and handed over again
        SkinChanger::ForceUpdate();
        GloveChanger::ForceUpdate();
        g_handsRefreshAt.store(GetTickCount64() + HANDS_REFRESH_DELAY_MS, std::memory_order_release);
    }
}

namespace
{
    std::atomic_bool g_scoreboardPin{ true };   // Settings -> "Lootline pin on the scoreboard"
}

void Unboxer::SetScoreboardPin(bool on)
{
    g_scoreboardPin.store(on, std::memory_order_relaxed);
}

bool Unboxer::ScoreboardFlair(const char* xuid, std::uint64_t& id, std::string& image)
{
    if (!g_scoreboardPin.load(std::memory_order_relaxed) || !xuid || !*xuid)
        return false;

    // you only (your SteamID's low half is your account id)
    const std::uint32_t account = GameInv::AccountId();

    if (!account || static_cast<std::uint32_t>(_strtoui64(xuid, nullptr, 10) & 0xFFFFFFFFull) != account)
        return false;

    std::lock_guard lock(g_mutex);

    for (const Item& item : g_items)
    {
        if (item.kind != Kind::Drop || !item.equipped || EquipSlot(item.definition) != -8)
            continue;

        id = item.id;
        image = Cases::ItemImage(item.definition);
        return !image.empty();
    }

    return false;
}

void Unboxer::SetReapplyOnSpawn(bool on, const std::vector<float>& secondsAfterSpawn)
{
    g_reapplyOnSpawn.store(on, std::memory_order_relaxed);

    const int count = static_cast<int>(std::min<std::size_t>(secondsAfterSpawn.size(), MAX_SPAWN_REAPPLIES));

    for (int i = 0; i < count; ++i)
        g_spawnReapplyMs[i].store(static_cast<std::uint32_t>(std::lround(std::clamp(secondsAfterSpawn[i], 0.0f, 60.0f) * 1000.0f)),
            std::memory_order_relaxed);

    g_spawnReapplyCount.store(count, std::memory_order_release);
}

void Unboxer::SetShuffleEveryRound(bool on)
{
    g_shuffleEveryRound.store(on, std::memory_order_relaxed);
}

void Unboxer::OnLevelChange()
{
    g_levelSeen.store(true, std::memory_order_release);
    g_newMatch.store(true, std::memory_order_release);
}

void Unboxer::WatchSpawn(void* localPawn)
{
    auto* pawn = static_cast<C_CSPlayerPawn*>(localPawn);

    if (!pawn || pawn->m_iHealth() <= 0)
        return;

    const float spawnTime = pawn->m_flLastSpawnTimeIndex();
    const auto address = reinterpret_cast<std::uintptr_t>(pawn);
    const ULONGLONG now = GetTickCount64();

    if (address != g_spawnPawn || spawnTime != g_spawnTime)
    {
        g_spawnPawn = address;
        g_spawnTime = spawnTime;
        g_spawnAt = now;
        g_spawnDue.clear();

        // the main thread rolls the loadout shuffles: every spawn, or (CS2's way) a match's first one
        const bool newMatch = g_newMatch.exchange(false, std::memory_order_acq_rel) ||
            (!g_levelSeen.load(std::memory_order_acquire) && address != g_shufflePawn);
        g_shufflePawn = address;

        if (newMatch || g_shuffleEveryRound.load(std::memory_order_relaxed))
            g_shufflePending.store(true, std::memory_order_release);

        if (g_reapplyOnSpawn.load(std::memory_order_relaxed))
        {
            const int count = std::clamp(g_spawnReapplyCount.load(std::memory_order_acquire), 0, MAX_SPAWN_REAPPLIES);

            for (int i = 0; i < count; ++i)
                g_spawnDue.push_back(now + g_spawnReapplyMs[i].load(std::memory_order_relaxed));

            std::sort(g_spawnDue.begin(), g_spawnDue.end());
        }

        g_spawnCheckAt = std::max(now + SPAWN_CHECK_MIN_MS, (g_spawnDue.empty() ? now : g_spawnDue.back()) + SPAWN_CHECK_AFTER_MS);
    }

    // a reapply due (times that fall together: one)
    if (!g_spawnDue.empty() && now >= g_spawnDue.front())
    {
        while (!g_spawnDue.empty() && now >= g_spawnDue.front())
            g_spawnDue.erase(g_spawnDue.begin());

        ReapplyNow();
        Log("spawn: every skin put on again %.1f s after spawning (Settings -> Reapply skins when you spawn)",
            static_cast<double>(now - g_spawnAt) / 1000.0);
    }

    // the spawn check: how each weapon's skin is doing once everything has settled (the answer when one "vanished")
    if (g_spawnCheckAt != 0 && now >= g_spawnCheckAt)
    {
        g_spawnCheckAt = 0;

        if (g_spawnChecks < MAX_SPAWN_CHECKS)
        {
            ++g_spawnChecks;
            const Team team = pawn->getTeam();
            const std::string weapons = Lineup::DescribeYourWeapons(pawn);
            Log("spawn check (%s, %.1f s after spawning): %s", team == Team::CT ? "CT" : team == Team::T ? "T" : "no side",
                static_cast<double>(now - g_spawnAt) / 1000.0, weapons.c_str());
        }
    }
}

void Unboxer::ReapplyInMatch()
{
    ReapplyNow();
    Log("in match: reapply asked from the menu");
}

bool Unboxer::Upgrade(const std::vector<std::uint64_t>& inputIds, const Item& target, bool win, double multiplier, unsigned delayMs)
{
    if (inputIds.empty() || inputIds.size() > 6 || target.definition == 0)
        return false;

    std::lock_guard lock(g_mutex);

    for (std::size_t i = 0; i < inputIds.size(); ++i)
    {
        const Item* input = FindLocked(inputIds[i]);

        if (!input || input->kind != Kind::Drop)
            return false;

        for (std::size_t j = 0; j < i; ++j)
            if (inputIds[j] == inputIds[i])
                return false;

        for (const PendingUpgrade& pending : g_upgrades)
            for (const std::uint64_t busy : pending.inputIds)
                if (busy == inputIds[i])
                    return false;
    }

    PendingUpgrade upgrade;
    upgrade.at = GetTickCount64() + delayMs;
    upgrade.inputIds = inputIds;
    upgrade.target = target;
    upgrade.target.kind = Kind::Drop;
    upgrade.target.fromCase = 0;
    upgrade.win = win;
    upgrade.multiplier = multiplier;
    g_upgrades.push_back(upgrade);
    return true;
}

bool Unboxer::UpgradePending()
{
    std::lock_guard lock(g_mutex);
    return !g_upgrades.empty();
}

Unboxer::UpgradeStats Unboxer::GetUpgradeStats()
{
    std::lock_guard lock(g_mutex);
    return g_upgradeStats;
}

void Unboxer::ResetUpgradeStats()
{
    std::lock_guard lock(g_mutex);
    g_upgradeStats = {};
    g_dirty = true;
}

namespace
{
    bool g_nativeNotifyFound = false;   // the start-up thread: Initialize, then CheckAfterUpdate

    // A UI string in the language picked in Settings (tr() follows the menu's copy, set while it draws).
    const char* Translated(const char* english, bool russian)
    {
        if (!russian || !english)
            return english;

        const auto& table = Localization::Russian();
        const auto it = table.find(english);
        return it != table.end() ? it->second : english;
    }
}

void Unboxer::CheckAfterUpdate()
{
    Offsets::StartSignatureCheck();

    for (int waited = 0; Offsets::SignatureCheckRunning() && waited < 60000; waited += 100)
        Sleep(100);

    if (Offsets::SignatureCheckRunning())
        return;   // still going: Settings -> Game update shows it when it's done

    const bool russian = Config::menu.language == 1;
    const std::vector<Offsets::SignatureResult> results = Offsets::SignatureResults();
    std::vector<const char*> features;   // English, each once

    for (const Offsets::SignatureResult& result : results)
    {
        if (result.found || result.rescued || !result.name)
            continue;

        Log("after a CS2 update? signature %s (%s) wasn't found", result.name, result.module ? result.module : "?");

        for (const Patterns::Entry& entry : Patterns::All)
            if (std::strcmp(entry.name, result.name) == 0 && entry.usedFor &&
                std::none_of(features.begin(), features.end(), [&](const char* f) { return std::strcmp(f, entry.usedFor) == 0; }))
                features.push_back(entry.usedFor);
    }

    // CS2's item popups (ResolveNativeNotify: the signature, or what follows it, wasn't found)
    if (!g_nativeNotifyFound)
        features.push_back("CS2's item popups (storage units, Uplink Terminals, stickers, charms)");

    if (features.empty())
    {
        Log("signatures: all %zu found", results.size());
        return;
    }

    std::string list;

    for (const char* feature : features)
    {
        if (!list.empty())
            list += ", ";

        list += Translated(feature, russian);
    }

    std::string text = Translated("CS2 seems to have updated: some parts of the game weren't found, so these may not work: ", russian);
    text += list;
    text += Translated(". Settings -> Game update -> Check signatures shows which (the details are in lootline_log.txt).", russian);
    Log("update check: %zu feature(s) may not work - a popup tells the user", features.size());

    std::lock_guard lock(g_mutex);
    g_popupMessages.push_back(std::move(text));
}

bool Unboxer::Initialize()
{
    const bool resolved = GameInv::Initialize();

    if (const std::uint8_t* pattern = M::FindPattern("client.dll", Patterns::UiEnginePointer))
        g_uiEngine = reinterpret_cast<PanoramaUiEngine**>(M::GetAbsoluteAddress(const_cast<std::uint8_t*>(pattern), 7));

    if (const std::uint8_t* pattern = M::FindPattern("client.dll", Patterns::MainMenuPanelPointer))
        g_mainMenu = reinterpret_cast<PanoramaPanel2D**>(M::GetAbsoluteAddress(const_cast<std::uint8_t*>(pattern), 7));

    g_runScript = reinterpret_cast<RunScriptFn>(M::FindPattern("panorama.dll", Patterns::RunPanoramaScript));
    const bool nativeNotify = ResolveNativeNotify();
    const bool purchaseEvent = nativeNotify && ResolvePurchaseCompleted();
    g_nativeNotifyFound = nativeNotify;

    if (const HMODULE tier0 = GetModuleHandleA("tier0.dll"))
        g_threadInMainThread = reinterpret_cast<ThreadInMainThreadFn>(GetProcAddress(tier0, "ThreadInMainThread"));

    const bool cases = Cases::Build();

    std::lock_guard lock(g_mutex);
    Load();

    g_status.resolved = resolved;
    g_status.panoramaReady = nativeNotify || (g_uiEngine && g_mainMenu && g_runScript);
    g_status.casesReady = cases;
    g_status.error = GameInv::LastError();

    if (!g_status.panoramaReady)
        g_status.error += "Case popup notification not found. ";

    Log("--- Lootline started: functions %s, popup notify %s, cases %s (%zu), %zu saved items%s%s",
        resolved ? "ok" : "MISSING", nativeNotify ? "native" : (g_status.panoramaReady ? "js only" : "MISSING"), cases ? "ok" : "not yet",
        Cases::All().size(), g_items.size(), g_status.error.empty() ? "" : " - ", g_status.error.c_str());
    if (!nativeNotify)
        Log("popup notify: %s (cases fall back to the popup's script; storage units, stickers, charms and tools need it)", g_notifyWhy);

    Log("sticker / charm / patch placement: %s", GameInv::PlacementSource());

    Log("terminals: purchase event %s", purchaseEvent ? "found (accepting an offer works)" : "NOT found (an accepted offer is yours, the laptop stays open)");
    return resolved;
}

void Unboxer::SetNewItemsEnabled(bool enabled)
{
    g_newItemsEnabled.store(enabled, std::memory_order_release);
}

void Unboxer::AcknowledgeBeforeUnload(unsigned timeoutMs)
{
    g_unloadAcknowledge.store(true, std::memory_order_release);   // from now on nothing new arrives NEW

    for (unsigned waited = 0; waited < timeoutMs && !g_unloadAcknowledged.load(std::memory_order_acquire); waited += 5)
        Sleep(5);
}

void Unboxer::Shutdown()
{
    StopSaveThread();   // a save waiting on it is written first

    std::lock_guard lock(g_mutex);

    if (g_saveFailed.exchange(false, std::memory_order_acq_rel))
        g_dirty = true;

    if (g_dirty)
        SaveLocked();
}

void Unboxer::TickRenderThread()
{
    // the item schema / case list can't be read before the game has loaded them (inject at the very start)
    if (!Cases::Ready())
    {
        static ULONGLONG nextTry = 0;
        const ULONGLONG now = GetTickCount64();

        if (now >= nextTry)
        {
            nextTry = now + 2000;

            if (!Skins::g_ItemSchema.IsInitialized())
                Skins::g_ItemSchema.Initialize();

            if (Cases::Build())
            {
                std::lock_guard lock(g_mutex);
                g_status.casesReady = true;
                Log("case list ready: %zu cases", Cases::All().size());
            }
        }

        return;
    }

    // ★ pool for random rolls: every knife / gloves with its finishes, one item per frame
    if (!g_goldListed)
    {
        for (const ItemDefinitionInfo& item : Skins::g_ItemSchema.GetItems())
        {
            if ((item.isKnife && item.definitionIndex >= 500) ||
                (item.isGlove && item.definitionIndex != 5028 && item.definitionIndex != 5029))
                g_goldCandidates.push_back(item.definitionIndex);
        }

        g_goldListed = true;
        return;
    }

    if (g_goldNext < g_goldCandidates.size())
    {
        const std::uint16_t definition = g_goldCandidates[g_goldNext++];
        std::vector<std::pair<std::uint16_t, int>> entries;

        if (const std::vector<PaintKitInfo>* paintKits = Skins::g_ItemSchema.GetPaintKitsForItem(definition))
        {
            for (const PaintKitInfo& paintKit : *paintKits)
                entries.emplace_back(definition, paintKit.id);   // id 0 = vanilla (knives)
        }

        std::lock_guard lock(g_mutex);
        g_goldPool.insert(g_goldPool.end(), entries.begin(), entries.end());
    }
}

namespace
{
    // Main thread: every NEW Lootline item acknowledged (locally).
    void AcknowledgeAllNew()
    {
        int done = 0;

        for (const std::uint64_t id : GameInv::NewItemIds())
        {
            char text[24];
            snprintf(text, sizeof(text), "%llu", static_cast<unsigned long long>(id));
            bool wasNew = false;

            if (Unboxer::IsLootlineItemId(text) && GameInv::Acknowledge(id, wasNew) && wasNew)
                ++done;
        }

        if (done)
            Log("unloading: %d new Lootline items acknowledged locally", done);
    }
}

int Unboxer::ShownKills(const Item& item)
{
    if (item.kills >= 0)
        return item.kills;

    // stattrak.json is per weapon + finish (music kits count MVPs, which nothing counts: 0)
    return item.statTrak && item.paintKit > 0 && Cases::KindOf(item.definition) == Cases::DropKind::Skin ?
        StatTrakTracker::Get(item.definition, item.paintKit) : 0;
}

namespace
{
    // StatTrak counting your real kills: the skin changer credits a kill to stattrak.json, CS2's inventory / inspect /
    // loadout read the item's own "kill eater" attribute - so it's written whenever the count changes (main thread, at most
    // once a second; the GC updates a real StatTrak in a match too).
    constexpr ULONGLONG STATTRAK_SYNC_MS = 1000;
    constexpr int MAX_STATTRAK_UPDATES = 16;   // per pass, the rest the next second
    ULONGLONG g_statTrakSyncAt = 0;
    std::unordered_map<std::uint64_t, int> g_statTrakShown;   // item id -> the kills its attribute holds (main thread)

    void SyncStatTrakKills()
    {
        const ULONGLONG now = GetTickCount64();

        if (now < g_statTrakSyncAt)
            return;

        g_statTrakSyncAt = now + STATTRAK_SYNC_MS;

        struct Count
        {
            std::uint64_t id = 0;
            int kills = 0;
        };

        std::vector<Count> counts;
        {
            std::lock_guard lock(g_mutex);

            for (const Unboxer::Item& item : g_items)
                if (item.statTrak && item.kills < 0 && item.paintKit > 0 && Cases::KindOf(item.definition) == Cases::DropKind::Skin)
                    counts.push_back({ item.id, Unboxer::ShownKills(item) });
        }

        std::unordered_map<std::uint64_t, int> shown;
        shown.reserve(counts.size());
        int updates = 0;

        for (const Count& count : counts)
        {
            // an item not seen yet was made with its count already (ToNewItem)
            const auto it = g_statTrakShown.find(count.id);
            int written = it != g_statTrakShown.end() ? it->second : count.kills;

            if (written != count.kills && updates < MAX_STATTRAK_UPDATES)
            {
                ++updates;

                if (GameInv::SetKills(count.id, count.kills))
                {
                    Log("stattrak: item %llu shows %d kills now (was %d)", static_cast<unsigned long long>(count.id), count.kills, written);
                    written = count.kills;
                }
            }

            shown[count.id] = written;
        }

        g_statTrakShown = std::move(shown);
    }
}

void Unboxer::GameTick()
{
    bool resolved = false;
    {
        std::lock_guard lock(g_mutex);
        resolved = g_status.resolved;
    }

    // unloading: no NEW Lootline item may stay behind (CS2 would acknowledge it itself once the hook is gone)
    if (g_unloadAcknowledge.load(std::memory_order_acquire) && !g_unloadAcknowledged.load(std::memory_order_acquire))
    {
        if (resolved && GameInv::InventoryReady())
            AcknowledgeAllNew();

        RemoveGamePriceLabels();   // CS2's screens keep no Lootline price after unloading
        g_unloadAcknowledged.store(true, std::memory_order_release);
    }

    if (!resolved)
        return;

    if (!g_threadLogged)
    {
        g_threadLogged = true;
        Log("game tick on the main thread: %s%s", OnMainThread() ? "yes" : "NO",
            g_threadInMainThread ? "" : " (tier0 ThreadInMainThread not found)");
    }

    const bool ready = GameInv::InventoryReady();
    {
        std::lock_guard lock(g_mutex);
        g_status.inventoryReady = ready;
    }

    if (!ready)
    {
        g_restored = false;   // disconnected from the Game Coordinator: put everything back once it's there again
        return;
    }

    if (!g_restored)
    {
        g_restored = true;
        RestoreSaved();
        QueueEquippedIntoLoadout();
    }

    ProcessLoadoutRequests();

    ProcessRequests();
    FinishOpenings();
    FinishUpgrades();
    FinishStickerJobs();
    FinishCharmJobs();
    FinishSlabJobs();
    FinishNameJobs();
    FinishUnsealJobs();
    FinishTerminalJobs();
    FinishKeyPurchases();   // "Buy keys with Lootline"
    SyncStatTrakKills();
    FinishTradeUps();
    TickArmory();
    FinishCasketJobs();
    FinishToolJobs();
    TickGamePrices();

    if (g_shufflePending.exchange(false, std::memory_order_acq_rel))
        RollShuffles();
    PrintChatLines();

    // items.json at most every SAVE_INTERVAL_MS (the whole list is written each time: every opening / every frame of a big
    // "Add" saving it was a hitch on the game's main thread with a big inventory); unloading saves what's left (Shutdown)
    // (written on the save thread: only a copy of the list is made here)
    const ULONGLONG now = GetTickCount64();
    std::lock_guard lock(g_mutex);
    if (g_saveFailed.exchange(false, std::memory_order_acq_rel))
        g_dirty = true;   // the last write failed: again
    if (g_dirty && now - g_lastSave >= SAVE_INTERVAL_MS)
    {
        QueueSaveLocked();
        g_lastSave = now;
    }
}

void* Unboxer::PanoramaEngine()
{
    if (!g_notifyEngine)
        return nullptr;

    __try
    {
        return *g_notifyEngine;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return nullptr;
    }
}

bool Unboxer::OnMainThread()
{
    return !g_threadInMainThread || g_threadInMainThread();
}

bool Unboxer::OnUseTool(const char* toolId, const char* itemId)
{
    // one of our scripts reporting back (see DIRECT_RESULT_SCRIPT): never an item
    if (toolId && std::strncmp(toolId, SCRIPT_REPORT_PREFIX, std::strlen(SCRIPT_REPORT_PREFIX)) == 0)
    {
        std::lock_guard lock(g_mutex);
        if (g_scriptReports.size() < 16)
            g_scriptReports.push_back(std::string(toolId + std::strlen(SCRIPT_REPORT_PREFIX)) + ": " + (itemId ? itemId : ""));
        return true;
    }

    const std::uint64_t tool = toolId ? _strtoui64(toolId, nullptr, 10) : 0;
    const std::uint64_t subject = itemId ? _strtoui64(itemId, nullptr, 10) : 0;

    // a name tag on one of our items (naming your pet sends a "faux" name tag)
    if (tool && subject && tool != subject && QueueNameTag(tool, subject))
        return true;

    // our charm going on one of our guns (CS2's charm popup)
    if (tool && subject && tool != subject && QueueCharmOnGun(tool, subject))
        return true;

    // unsealing one of our sealed graffiti (UseTool(graffiti, ""))
    if (tool && QueueUnseal(tool, subject))
        return true;

    // our StatTrak Swap Tool / Charm Detachment Pack (UseTool(tool, ""))
    if (tool && QueueToolUse(tool, subject))
        return true;

    // a sticker sealed in a Sticker Slab (CS2's "Seal in Sticker Slab": UseTool(sticker, slab))
    if (tool && subject && tool != subject && QueueSeal(tool, subject))
        return true;

    // a sticker going on one of our guns / a patch on one of our agents (CS2's popups, once they're placed)
    if (tool && tool != subject && QueueStickerOnGun(tool, subject))
        return true;

    std::lock_guard lock(g_mutex);

    const Item* caseItem = FindLocked(subject);
    const Item* keyItem = FindLocked(tool);

    if (caseItem && caseItem->kind != Kind::Case)
        caseItem = nullptr;
    if (keyItem && keyItem->kind != Kind::Key)
        keyItem = nullptr;

    if (!caseItem && !keyItem)
    {
        // a Lootline item that none of the above takes: say so (Valve will refuse it)
        std::uint16_t fauxDefinition = 0;
        int fauxPaintKit = 0;
        const bool oursInvolved = (tool >= ID_BASE && !FromFauxId(tool, fauxDefinition, fauxPaintKit) && FindLocked(tool)) ||
            (subject >= ID_BASE && !FromFauxId(subject, fauxDefinition, fauxPaintKit) && FindLocked(subject));

        if (oursInvolved)
            Log("UseTool(%s, %s): a Lootline item, but not one Lootline can do this with - sent to the game", toolId ? toolId : "",
                itemId ? itemId : "");

        return false;   // not ours: the game handles it
    }

    // one of our Uplink Terminals: unsealing it opens CS2's offer laptop (no reel, nothing rolled yet)
    if (caseItem)
    {
        if (const Cases::Case* info = Cases::Find(caseItem->definition); info && info->terminal)
        {
            if (caseItem->quality == QUALITY_VOLATILE)
            {
                Log("UseTool(%s, %s): an unsealed terminal - nothing to do", toolId ? toolId : "", itemId ? itemId : "");
                return true;
            }

            if (!TerminalJobPendingLocked(caseItem->id))
            {
                g_terminalJobs.push_back({ GetTickCount64() + OPEN_RESULT_DELAY_MS, TerminalJob::What::Unseal, caseItem->id });
                Log("UseTool(%s, %s): unsealing a terminal locally (%s)", toolId ? toolId : "", itemId ? itemId : "", info->displayName.c_str());
            }

            return true;
        }
    }

    // UseTool(case, case) = open without a key (souvenir packages); UseTool(case, "") too
    if (tool == subject)
        keyItem = nullptr;

    const std::uint64_t caseId = caseItem ? caseItem->id : 0;

    for (const PendingOpen& open : g_pending)
    {
        if (caseId && open.caseId == caseId)
            return true;   // already opening (double click)
    }

    const Cases::Case* info = caseItem ? Cases::Find(caseItem->definition) : Cases::Find(GameInv::ItemDefinition(subject));

    PendingOpen open;
    open.at = GetTickCount64() + OPEN_RESULT_DELAY_MS;
    open.caseId = caseId;
    open.keyId = keyItem ? keyItem->id : 0;
    open.opened = subject;
    open.drop = RollDropLocked(info);

    if (open.drop.definition == 0)
    {
        Log("UseTool(%s, %s): ours, but nothing to give (no drop picked and the case's contents are unknown)", toolId ? toolId : "", itemId ? itemId : "");
        return true;
    }

    g_pending.push_back(open);
    Log("UseTool(%s, %s): opening locally (def %u paint %d tier-rarity %d)", toolId ? toolId : "", itemId ? itemId : "",
        open.drop.definition, open.drop.paintKit, open.drop.rarity);
    return true;
}

std::uint64_t Unboxer::FauxId(std::uint16_t definition, int paintKit)
{
    return 0xF000000000000000ull | (static_cast<std::uint64_t>(static_cast<std::uint16_t>(paintKit)) << 16) | definition;
}

bool Unboxer::FromFauxId(std::uint64_t id, std::uint16_t& definition, int& paintKit)
{
    if ((id >> 60) != 0xF)
        return false;

    definition = static_cast<std::uint16_t>(id & 0xFFFF);
    paintKit = static_cast<int>((id >> 16) & 0xFFFF);   // bits 32+ are flags (StatTrak lists)
    return definition != 0;
}

bool Unboxer::ShownDropFor(const char* caseItemId, ShownDrop& out)
{
    const std::uint64_t id = caseItemId ? _strtoui64(caseItemId, nullptr, 10) : 0;

    if (id < ID_BASE)
        return false;   // not ours (real cases keep their real contents)

    std::lock_guard lock(g_mutex);

    if (g_drop.random || !g_drop.showInCase || g_drop.weapon == 0)
        return false;

    const Item* item = FindLocked(id);

    if (!item || item->kind != Kind::Case)
        return false;

    const Cases::Case* info = Cases::Find(item->definition);
    const bool gold = IsKnife(g_drop.weapon) || IsGloves(g_drop.weapon);

    if (gold && info && info->hasGold)
        return false;   // shows as the case's "★ Rare Special Item" tile, like every real ★

    if (info && Cases::DropsFrom(*info, g_drop.weapon, g_drop.paintKit))
        return false;   // it's in the case anyway

    out.fauxId = FauxId(g_drop.weapon, g_drop.paintKit);
    out.definition = g_drop.weapon;
    out.paintKit = g_drop.paintKit;
    out.tier = gold ? Cases::TierGold : std::clamp(g_drop.tier, 0, static_cast<int>(Cases::TierCovert));
    out.caseDefinition = item->definition;
    return true;
}

void Unboxer::AddCase(std::uint16_t caseDefinition, int count, bool withKeys)
{
    std::lock_guard lock(g_mutex);
    g_requests.push_back({ Request::AddCase, caseDefinition, std::clamp(count, 1, Unboxer::MAX_ADD_COUNT), withKeys, 0 });
}

void Unboxer::AddKey(std::uint16_t keyDefinition, int count)
{
    std::lock_guard lock(g_mutex);
    g_requests.push_back({ Request::AddKey, keyDefinition, std::clamp(count, 1, Unboxer::MAX_ADD_COUNT), false, 0 });
}

void Unboxer::AddItem(const Item& item, int count, RandomFloat randomFloat, bool randomSeed)
{
    if (item.definition == 0)
        return;

    Request request;
    request.type = Request::AddItem;
    request.definition = item.definition;
    request.count = std::clamp(count, 1, Unboxer::MAX_ADD_COUNT);
    request.item = item;

    if (randomFloat.on)
    {
        randomFloat.from = std::clamp(randomFloat.from, 0.0f, 1.0f);
        randomFloat.to = std::clamp(randomFloat.to, randomFloat.from, 1.0f);
        request.randomFloat = randomFloat;
    }

    request.randomSeed = randomSeed;

    std::lock_guard lock(g_mutex);
    g_requests.push_back(std::move(request));
}

void Unboxer::Remove(std::uint64_t id)
{
    std::lock_guard lock(g_mutex);
    g_requests.push_back({ Request::Remove, 0, 0, false, id });
}

void Unboxer::RemoveAll()
{
    std::lock_guard lock(g_mutex);
    g_requests.push_back({ Request::RemoveAll, 0, 0, false, 0 });
}

std::vector<Unboxer::Item> Unboxer::Items()
{
    std::lock_guard lock(g_mutex);
    return g_items;
}

Unboxer::Status Unboxer::GetStatus()
{
    std::lock_guard lock(g_mutex);
    return g_status;
}

Unboxer::DropSettings Unboxer::GetDropSettings()
{
    std::lock_guard lock(g_mutex);
    return g_drop;
}

void Unboxer::SetDropSettings(const DropSettings& settings)
{
    std::lock_guard lock(g_mutex);

    if (settings.revision != g_drop.revision)
        return;   // made before Lootline went back to Random by itself ("only once"): that one stands

    if (settings.random != g_drop.random || settings.weapon != g_drop.weapon || settings.paintKit != g_drop.paintKit ||
        settings.dropOn != g_drop.dropOn)
        g_pickedOpenings = 0;   // another item / number: count from the next opening

    if (settings.random != g_drop.random || settings.weapon != g_drop.weapon || settings.paintKit != g_drop.paintKit ||
        settings.terminalOn != g_drop.terminalOn)
        g_pickedTerminals = 0;   // ... and from the next terminal

    g_drop = settings;
}

bool Unboxer::GetCustomOdds(std::uint16_t container, TierOdds& out)
{
    std::lock_guard lock(g_mutex);
    const auto it = g_customOdds.find(container);

    if (it == g_customOdds.end())
        return false;

    out = it->second;
    return true;
}

void Unboxer::SetCustomOdds(std::uint16_t container, const TierOdds& odds)
{
    if (container == 0)
        return;

    TierOdds clamped{};
    for (std::size_t tier = 0; tier < odds.size(); ++tier)
        clamped[tier] = std::clamp(odds[tier], 0.0, 100.0);

    std::lock_guard lock(g_mutex);
    auto [it, added] = g_customOdds.try_emplace(container, clamped);

    if (!added && it->second == clamped)
        return;

    it->second = clamped;
    g_dirty = true;
}

void Unboxer::ClearCustomOdds(std::uint16_t container)
{
    std::lock_guard lock(g_mutex);

    if (g_customOdds.erase(container))
        g_dirty = true;
}

int Unboxer::PickedOpenings()
{
    std::lock_guard lock(g_mutex);
    return g_pickedOpenings;
}

int Unboxer::PickedTerminals()
{
    std::lock_guard lock(g_mutex);
    return g_pickedTerminals;
}

void Unboxer::SetChatDrops(bool enabled)
{
    if (g_chatDrops.exchange(enabled) && !enabled)
    {
        std::lock_guard lock(g_mutex);
        g_chatLines.clear();   // switched off: the lines still waiting for their reel aren't printed either
    }
}

const char* Unboxer::WearName(float wear)
{
    if (wear < 0.07f) return "Factory New";
    if (wear < 0.15f) return "Minimal Wear";
    if (wear < 0.38f) return "Field-Tested";
    if (wear < 0.45f) return "Well-Worn";
    return "Battle-Scarred";
}

std::string Unboxer::DisplayName(const Item& item)
{
    if (item.kind == Kind::Case)
    {
        if (const Cases::Case* info = Cases::Find(item.definition))
        {
            // an unsealed terminal: "Genesis Terminal", not "Sealed Genesis Terminal"
            if (item.quality == QUALITY_VOLATILE && info->displayName.starts_with("Sealed "))
                return info->displayName.substr(7);

            return info->displayName;
        }
    }
    else if (item.kind == Kind::Key)
    {
        if (const Cases::Key* key = Cases::FindKey(item.definition))
            return key->displayName;
    }

    if (item.kind == Kind::Drop && item.definition == DEF_CHARM)
    {
        const KeychainInfo* charm = Skins::g_ItemSchema.GetKeychain(item.paintKit);
        std::string text = std::string("Charm | ") + (charm ? charm->displayName : std::to_string(item.paintKit));

        // an event's highlight charm: which clip ("Charm | Austin 2025 Highlight | Ace Unleashed")
        if (const Cases::Highlight* clip = item.highlight > 0 ? Cases::FindHighlight(item.highlight) : nullptr; clip && !clip->own)
            text = std::string("Charm | ") + clip->displayName;

        // a Sticker Slab: which sticker is sealed in it
        if (item.charmSticker > 0)
        {
            const Cases::Kit* sticker = Cases::FindKit(Cases::DropKind::Sticker, item.charmSticker);
            text += " | " + (sticker ? sticker->displayName : std::to_string(item.charmSticker));
        }

        return text;
    }

    if (item.kind == Kind::Drop && item.definition == DEF_STICKER_SLAB)
        return "Sticker Slab";

    // tools: a Storage Unit with its name ("Storage Unit | Knives"), a Name Tag, the StatTrak Swap Tool, Charm Detachments
    if (item.kind == Kind::Drop && item.definition == DEF_CASKET)
        return item.customName.empty() ? std::string("Storage Unit") : "Storage Unit | " + item.customName;
    if (item.kind == Kind::Drop && item.definition == DEF_NAME_TAG_TOOL)
        return "Name Tag";
    if (item.kind == Kind::Drop && item.definition == DEF_STATTRAK_SWAP)
        return "StatTrak Swap Tool";
    if (item.kind == Kind::Drop && item.definition == DEF_CHARM_DETACH_PACK)
        return "Charm Detachment Pack";

    if (item.kind == Kind::Drop && item.definition == DEF_PET)
    {
        static constexpr const char* breeds[] = { "Catalana", "Silkie", "Polish" };
        const char* stage = item.petLevel <= 1 ? "Chick" : item.petLevel == 2 ? "Pullet" : "Hen";
        return item.petLevel <= 1 || item.petId < 3 || item.petId > 5 ? std::string(stage) : std::string(breeds[item.petId - 3]) + " " + stage;
    }

    const ItemDefinitionInfo* weapon = Skins::g_ItemSchema.GetItem(item.definition);
    std::string name = weapon ? (weapon->displayName.empty() ? weapon->itemName : weapon->displayName) : "Item " + std::to_string(item.definition);

    if (item.kind != Kind::Drop)
        return name;

    // "Sticker | Thirteen", "Patch | Payback", "Sealed Graffiti | ...", "StatTrak™ Music Kit | ...", "Guardian Pin"
    if (const Cases::DropKind kind = Cases::KindOf(item.definition); kind != Cases::DropKind::Skin)
    {
        const Cases::Kit* kit = Cases::FindKit(kind, kind == Cases::DropKind::Pin ? item.definition : item.paintKit);

        if (kind == Cases::DropKind::Pin)
            return kit ? kit->displayName : name;

        if (!weapon)
            name = kind == Cases::DropKind::Sticker ? "Sticker" : kind == Cases::DropKind::Patch ? "Patch" :
                kind == Cases::DropKind::Graffiti ? (item.definition == Cases::DEF_SPRAY_PAINT ? "Graffiti" : "Sealed Graffiti") : "Music Kit";

        std::string text = item.statTrak ? "StatTrak\xE2\x84\xA2 " : "";
        text += name + " | " + (kit ? kit->displayName : std::to_string(item.paintKit));

        if (kind == Cases::DropKind::Graffiti && item.tint > 0)
            text += std::string(" (") + Cases::TintName(item.tint) + ")";   // "Sealed Graffiti | 1G (Battle Green)"

        return text;
    }

    const bool gold = weapon && (weapon->isKnife || weapon->isGlove);
    std::string text;

    if (gold)
        text += "\xE2\x98\x85 ";   // ★
    if (item.statTrak)
        text += "StatTrak\xE2\x84\xA2 ";   // ™
    else if (item.quality == 12)
        text += "Souvenir ";

    text += name;

    if (item.paintKit > 0)
    {
        if (const PaintKitInfo* paintKit = Skins::g_ItemSchema.GetPaintKit(item.paintKit))
            text += " | " + (paintKit->displayName.empty() ? paintKit->name : paintKit->displayName);

        text += " (";
        text += WearName(item.wear);
        text += ")";
    }

    return text;
}
