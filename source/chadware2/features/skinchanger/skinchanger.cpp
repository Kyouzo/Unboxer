#include "skinchanger.h"
#include "../../../patterns.h"

#include "item_schema.h"
#include "skin_application.h"
#include "econ_item_attribute_manager.h"
#include "glove_changer.h"
#include "knife_model_changer.h"
#include "music_kit_changer.h"
#include "randomizer.h"
#include "stattrak_tracker.h"
#include "weapon_cosmetics.h"

#include <algorithm>
#include <atomic>
#include <mutex>
#include <bit>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <chadware2/utils/math/vector/vector.h>
#include "../../config/config.h"
#include "../../globals/globals.h"
#include "../../interfaces/interfaces.h"
#include "../../utils/memory/Interface/Interface.h"
#include "../../utils/memory/patternscan/patternscan.h"
#include <cs2/entity/C_CSPlayerPawn/C_CSPlayerPawn.h>


namespace
{
    constexpr int FORCE_UPDATE_FRAMES = 6;
using AgentSetModelFn = void(__fastcall*)(void*, const char*);
    using AgentPrecacheFn = void* (__fastcall*)(void*, void*, const char*);
    using BufferStringInsertFn = const char* (__fastcall*)(void*, int, const char*, int, bool);
    using BufferStringPurgeFn = void(__fastcall*)(void*, int);

    struct CBufferString
    {
        int m_nLength = 0;
        int m_nAllocatedSize = static_cast<int>(0xC0000008u);

        union
        {
            char* m_pString;
            char m_szString[8];
        };

        CBufferString()
        {
            m_pString = nullptr;
        }
    };

    std::uint16_t g_activeAgentDefinition = 0;
    std::uint16_t g_failedAgentDefinition = 0;
    std::uintptr_t g_agentPawnAddress = 0;
    Team g_agentTeam = Team::UNASSIGNED;
    float g_agentSpawnTime = -1.0f;
    std::string g_originalAgentModel;
    bool g_agentCustomApplied = false;
    std::atomic_bool g_forceAgentUpdate{ true };

    std::uint16_t g_lastAgentTargetDefinition = 0;
    int g_agentApplyAttempts = 0;
    // RefreshPlayerModel: another model for one frame, then yours again
    std::atomic_int g_modelSwapRequest{ 0 };
    int g_modelSwapStep = 0;                 // 1 = the other model is on for this frame
    std::string g_modelSwapBack;

    bool IsErrorPlayerModelName(const std::string& modelName)
    {
        return modelName == "models/dev/error.vmdl" || modelName.ends_with("/error.vmdl");
    }

    AgentSetModelFn GetAgentSetModel()
    {
        const auto fn = M::ResolveCached([] { return reinterpret_cast<AgentSetModelFn>(M::scan("client.dll", Patterns::AgentSetModel)); });
        return fn;
    }

    AgentPrecacheFn GetAgentPrecache()
    {
        const auto fn = M::ResolveCached([] { return reinterpret_cast<AgentPrecacheFn>(M::scan("resourcesystem.dll", Patterns::AgentPrecache)); });
        return fn;
    }

    void* GetAgentResourceSystem()
    {
        void* resourceSystem = M::ResolveCached([] { return I::Get<void>("resourcesystem.dll", "ResourceSystem013"); });
        return resourceSystem;
    }

    BufferStringInsertFn GetBufferStringInsert()
    {
        const auto fn = M::ResolveCached([]
        {
            const HMODULE tier0 = GetModuleHandleA("tier0.dll");
            return tier0 ? reinterpret_cast<BufferStringInsertFn>(GetProcAddress(tier0, "?Insert@CBufferString@@QEAAPEBDHPEBDH_N@Z")) : nullptr;
        });
        return fn;
    }

    BufferStringPurgeFn GetBufferStringPurge()
    {
        const auto fn = M::ResolveCached([]
        {
            const HMODULE tier0 = GetModuleHandleA("tier0.dll");
            return tier0 ? reinterpret_cast<BufferStringPurgeFn>(GetProcAddress(tier0, "?Purge@CBufferString@@QEAAXH@Z")) : nullptr;
        });
        return fn;
    }

    bool PrecacheAgentModel(const std::string& modelName)
    {
        if (modelName.empty())
            return false;

        void* resourceSystem = GetAgentResourceSystem();
        const auto precache = GetAgentPrecache();
        const auto insert = GetBufferStringInsert();

        if (!resourceSystem || !precache || !insert)
            return false;

        CBufferString names;
        insert(&names, 0, modelName.c_str(), -1, false);
        precache(resourceSystem, &names, "");

        if (const auto purge = GetBufferStringPurge())
            purge(&names, 0);

        return true;
    }

    const char* GetCurrentPlayerModelName(C_BaseEntity* pawn)
    {
        if (!pawn)
            return nullptr;

        CGameSceneNode* sceneNode = pawn->m_pGameSceneNode();

        if (!sceneNode)
            return nullptr;

        CSkeletonInstance* skeleton = sceneNode->GetSkeletonInstance();

        if (!skeleton)
            return nullptr;

        C_ModelState* modelState = skeleton->m_model_state();

        if (!modelState)
            return nullptr;

        C_Model* model = modelState->m_model();

        if (!model || !model->perm_model_data.name || !*model->perm_model_data.name)
            return nullptr;

        return model->perm_model_data.name;
    }

    bool SetAgentModel(C_CSPlayerPawn* pawn, const char* modelName)
    {
        if (!pawn || !modelName || !*modelName)
            return false;

        const auto setModel = GetAgentSetModel();

        if (!setModel)
            return false;

        CCollisionProperty* collision = pawn->m_pCollision();
        Vector_t mins{};
        Vector_t maxs{};
        const bool preserveBounds = collision != nullptr;

        if (preserveBounds)
        {
            mins = collision->m_vecMins();
            maxs = collision->m_vecMaxs();
        }

        PrecacheAgentModel(modelName);
        setModel(pawn, modelName);

        collision = pawn->m_pCollision();

        if (preserveBounds && collision)
        {
            collision->m_vecMins() = mins;
            collision->m_vecMaxs() = maxs;
        }

        return true;
    }

    // Another model of this side for RefreshPlayerModel: the one you had before our agent, or any other agent.
    std::string OtherAgentModel(Team team, const std::string& current)
    {
        if (!g_originalAgentModel.empty() && g_originalAgentModel != current && !IsErrorPlayerModelName(g_originalAgentModel))
            return g_originalAgentModel;

        for (const ItemDefinitionInfo& item : Skins::g_ItemSchema.GetItems())
        {
            if (!item.isAgent || !item.hasUsableAgentModel || item.modelName.empty() || item.modelName == current)
                continue;

            if ((team == Team::T && item.isTerroristAgent) || (team == Team::CT && item.isCounterTerroristAgent))
                return item.modelName;
        }

        return {};
    }

    void ResetAgentRuntime()
    {
        g_modelSwapRequest.store(0, std::memory_order_release);
        g_modelSwapStep = 0;
        g_modelSwapBack.clear();
        g_agentPawnAddress = 0;
        g_agentTeam = Team::UNASSIGNED;
        g_agentSpawnTime = -1.0f;
        g_originalAgentModel.clear();
        g_agentCustomApplied = false;
        g_activeAgentDefinition = 0;
        g_failedAgentDefinition = 0;
        g_lastAgentTargetDefinition = 0;
        g_agentApplyAttempts = 0;
        g_forceAgentUpdate.store(true, std::memory_order_release);
    }

    std::atomic_int g_updateFrames{ FORCE_UPDATE_FRAMES };
    std::vector<CBaseHandle> g_lastWeaponHandles;
    std::unordered_map<std::uintptr_t, std::uint64_t> g_lastAppliedGunConfigHash;
    // the same hash without the StatTrak count: a kill alone doesn't re-apply a gun with a charm while you hold it
    std::unordered_map<std::uintptr_t, std::uint64_t> g_lastAppliedGunLookHash;

    struct AppliedKeychainState
    {
        bool effective = false;
        int id = 0;
        int seed = 0;
        float offsetX = 0.0f;
        float offsetY = 0.0f;
        float offsetZ = 0.0f;
        int sticker = 0;   // a Sticker Slab's sealed sticker
    };

    std::unordered_map<std::uintptr_t, AppliedKeychainState> g_lastAppliedKeychainState;

    struct PendingKeychainOperation
    {
        CBaseHandle weaponHandle{};
        AppliedKeychainState desired{};
        bool clearWorld = false;      // no charm wanted, but the real item's charm hangs on the gun: it comes off
    };

    std::atomic<std::uint16_t> g_liveKnifeDefinition{ 0 };   // SkinChanger::LiveKnifeDefinition

    std::unordered_map<std::uintptr_t, PendingKeychainOperation> g_pendingKeychainOperations;

    // UI edits and FrameStageNotify run on different threads; use a short lease.
    std::atomic_bool g_interactiveEditActive{ false };
    std::atomic<std::uint64_t> g_interactiveEditHeartbeatMs{ 0 };

    constexpr std::uint64_t INTERACTIVE_EDIT_LEASE_MS = 500;

    // Keep the previous preview frame while live custom-material work settles.
    constexpr std::uint64_t PREVIEW_POST_UPDATE_SETTLE_MS = 500;
    std::atomic<std::uint64_t> g_previewBlockedUntilMs{ 0 };

    std::uint64_t NowMilliseconds()
    {
        using namespace std::chrono;
        return static_cast<std::uint64_t>(
            duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
    }

    void ExtendPreviewUpdateBlock()
    {
        g_previewBlockedUntilMs.store(
            NowMilliseconds() + PREVIEW_POST_UPDATE_SETTLE_MS,
            std::memory_order_release);
    }

    // Written by the game thread (UpdateLiveInfo); read by the preview on other threads, which must not touch the
    // pawn themselves (it can be freed under them, e.g. on disconnect).
    std::atomic_bool g_localAlive{ false };
    std::mutex g_ownedWeaponsMutex;
    std::vector<std::uint16_t> g_ownedWeapons;

    bool HasLivePreviewUpdateTarget()
    {
        return g_localAlive.load(std::memory_order_acquire);
    }

    bool g_restoreKnifeAfterDefault = false;
    std::uint16_t g_restoreKnifeDefinition = 0;

    // After a knife model change: its first-person model's animation graph is rebuilt ONCE (UC knife-inspect recipe),
    // after the forced update frames, ~0.25 s later and only while that knife is in your hand (rebuilding while an
    // inspect plays would restart it). A few tries in case the first-person model isn't there yet.
    std::uintptr_t g_knifeGraphWeapon = 0;
    std::uint64_t g_knifeGraphDueMs = 0;
    int g_knifeGraphTries = 0;
    std::uint64_t g_knifeGraphIdleSinceMs = 0;   // the knife in your hand has been idle since then (0 = not idle)
    constexpr std::uint64_t KNIFE_GRAPH_DELAY_MS = 250;
    // The rebuild resets the first-person animation graph: done mid-draw it cut the draw - a knife's rare draw (the Nomad
    // "cutting himself") snapped back to idle half-way. It waits until the knife has been idle this long (the rare draw's
    // picture can run a little past the idle state); an attack / inspect / putting it away starts the wait again.
    constexpr std::uint16_t WPN_ANIMSTATE_IDLE = 50;   // WeaponGameplayAnimState (DEPLOY 11, INSPECT 1000)
    constexpr std::uint64_t KNIFE_GRAPH_IDLE_MS = 2000;
    constexpr int KNIFE_GRAPH_MAX_TRIES = 8;

    std::unordered_set<std::uintptr_t> g_demoKnives;   // a demo: the knives whose model we changed (ProcessKnife)

    void ReportKnife(const char* line);   // a line in Lootline's log (next to the charm reports, below)

    // A demo playing (GOTV / your matches / your own recording): IEngineClient vfunc 42 = IsPlayingDemo (panorama's
    // IsPlayingDemo native jumps to it, client 2026-10-14). The game plays every model's animation from the recording
    // there: a knife's first-person animation rebuilt, a knife model swapped under an animating knife or the player
    // model swapped for a frame crashed CS2's animation job ("AG2 - EvaluateAnimList", 2026-10-17). Game thread.
    bool InDemo()
    {
        return I::EngineClient && M::CallVFunc<bool, 42U>(I::EngineClient);
    }

    // The game put the real item's id back on a weapon of ours (the server sent the item again: a full update) - then the
    // game reads that item's own look and ours is gone. Put back once, at most once a second per weapon (a game that
    // keeps doing it can't make us re-apply every frame, and a weapon gets it at most 5 times); a few lines in Lootline's log.
    struct IdBack
    {
        std::uint64_t lastMs = 0;
        int times = 0;
    };

    std::unordered_map<std::uintptr_t, IdBack> g_idBack;   // per weapon (forgotten with the weapon)
    int g_idBackReports = 0;
    constexpr std::uint64_t ID_BACK_INTERVAL_MS = 1000;
    constexpr int ID_BACK_MAX_PER_WEAPON = 5;
    constexpr int ID_BACK_MAX_REPORTS = 30;

    bool RealItemIdBack(C_CSWeaponBase* weapon, C_EconItemView* item, std::uint16_t definition, std::uint64_t nowMs)
    {
        if (!weapon || !item || item->m_iItemIDHigh() == 0xFFFFFFFFu)
            return false;

        IdBack& state = g_idBack[reinterpret_cast<std::uintptr_t>(weapon)];

        if (state.times >= ID_BACK_MAX_PER_WEAPON || (state.lastMs != 0 && nowMs - state.lastMs < ID_BACK_INTERVAL_MS))
            return false;

        state.lastMs = nowMs;
        ++state.times;

        if (g_idBackReports < ID_BACK_MAX_REPORTS)
        {
            ++g_idBackReports;
            char line[160]{};
            snprintf(line, sizeof(line), "skin: the game put the real item back on weapon %u (item id high %08X) - your skin went on again",
                static_cast<unsigned>(definition), item->m_iItemIDHigh());
            ReportKnife(line);
        }

        return true;
    }

    void ResetKnifeDefaultRefresh()
    {
        g_restoreKnifeAfterDefault = false;
        g_restoreKnifeDefinition = 0;
    }

    std::uint16_t GetTemporaryKnifeDefinition(std::uint16_t currentDefinition)
    {
        return currentDefinition == 500 ? 507 : 500;
    }

    C_CSWeaponBase* GetWeaponFromHandle(const CBaseHandle& handle)
    {
        if (!handle.valid() || !I::GameEntity || !I::GameEntity->Instance)
            return nullptr;

        return I::GameEntity->Instance->Get<C_CSWeaponBase>(handle.index());
    }

    // The gun in your hands. Charms only go on (or come off) that one: a charm made on a holstered gun isn't hidden with
    // it - it hangs frozen where that gun is, in first person right next to the gun you hold.
    bool IsActiveWeapon(C_CSWeaponBase* weapon)
    {
        C_CSPlayerPawn* pawn = g_ctx ? g_ctx->local_pawn : nullptr;
        CCSPlayer_WeaponServices* services = pawn ? pawn->m_pWeaponServices() : nullptr;
        return weapon && services && GetWeaponFromHandle(services->m_hActiveWeapon()) == weapon;
    }

    // Your Steam account id (the low half of your controller's m_steamID), 0 if it can't be read.
    std::uint32_t LocalAccountId(C_CSPlayerPawn* pawn)
    {
        if (!pawn || !I::GameEntity || !I::GameEntity->Instance)
            return 0;

        const std::uint32_t offset = SchemaFinder::GetCached<hash_32_fnv1a_const("CBasePlayerController->m_steamID")>();
        auto* controller = reinterpret_cast<std::uint8_t*>(I::GameEntity->Instance->Get<C_BaseEntity>(pawn->m_hController()));
        return controller && offset ? static_cast<std::uint32_t>(*reinterpret_cast<const std::uint64_t*>(controller + offset) & 0xFFFFFFFFull) : 0;
    }

    // Whose gun it is: the first player who had it (CS2 keeps that on the weapon - it's the owner CS2 names when you
    // look at a picked-up gun). Only your own guns get your skins; one picked up from someone else keeps its real look
    // (their skin, or none). Unknown (0, or your id can't be read) counts as yours, as before.
    bool IsOwnWeapon(C_CSWeaponBase* weapon, C_CSPlayerPawn* localPawn)
    {
        const std::uint32_t ownerOffset = SchemaFinder::GetCached<hash_32_fnv1a_const("C_EconEntity->m_OriginalOwnerXuidLow")>();

        if (!weapon || !ownerOffset)
            return true;

        const std::uint32_t owner = *reinterpret_cast<const std::uint32_t*>(reinterpret_cast<const std::uint8_t*>(weapon) + ownerOffset);

        if (owner == 0)
            return true;

        const std::uint32_t mine = LocalAccountId(localPawn);
        return mine == 0 || owner == mine;
    }

    C_EconItemView* GetWeaponItem(C_CSWeaponBase* weapon)
    {
        if (!weapon)
            return nullptr;

        C_AttributeContainer* attributeManager = weapon->m_AttributeManager();
        return attributeManager ? attributeManager->m_Item() : nullptr;
    }

    // A knife model change happened: its first-person animation graph gets rebuilt once it's settled and in your hand.
    void QueueKnifeGraphRebuild(C_CSWeaponBase* weapon, std::uint16_t definition)
    {
        g_knifeGraphWeapon = reinterpret_cast<std::uintptr_t>(weapon);
        g_knifeGraphDueMs = NowMilliseconds() + KNIFE_GRAPH_DELAY_MS;
        g_knifeGraphTries = 0;
        g_knifeGraphIdleSinceMs = 0;

        char line[128]{};
        snprintf(line, sizeof(line), "knife: model changed to %u (%s)", static_cast<unsigned>(definition),
            IsActiveWeapon(weapon) ? "in your hand" : "holstered - its animation waits until you pull it out");
        ReportKnife(line);
    }

    void ProcessKnifeGraphRebuild(C_CSWeaponBase* weapon, C_CSPlayerPawn* localPawn)
    {
        if (g_knifeGraphWeapon != reinterpret_cast<std::uintptr_t>(weapon))
            return;

        const std::uint64_t nowMs = NowMilliseconds();

        // in your hand and idle (not drawing, attacking or inspecting) for KNIFE_GRAPH_IDLE_MS
        if (!IsActiveWeapon(weapon) || weapon->State() != WPN_ANIMSTATE_IDLE)
        {
            g_knifeGraphIdleSinceMs = 0;
            return;
        }

        if (g_knifeGraphIdleSinceMs == 0)
            g_knifeGraphIdleSinceMs = nowMs;

        if (nowMs < g_knifeGraphDueMs || nowMs - g_knifeGraphIdleSinceMs < KNIFE_GRAPH_IDLE_MS ||
            g_updateFrames.load(std::memory_order_acquire) > 0)
            return;

        // a demo: never (the rebuild is the inspect fix - nothing to inspect there - and it crashed the replay)
        if (InDemo())
        {
            g_knifeGraphWeapon = 0;
            ReportKnife("knife: first-person animation left as it is (a replay plays it from the recording)");
            return;
        }

        const char* problem = SkinApplication::RebuildHudAnimGraph(weapon, localPawn);
        ++g_knifeGraphTries;

        if (problem[0] != '\0' && g_knifeGraphTries < KNIFE_GRAPH_MAX_TRIES)
        {
            g_knifeGraphDueMs = NowMilliseconds() + KNIFE_GRAPH_DELAY_MS;   // the first-person model may not be there yet
            return;
        }

        g_knifeGraphWeapon = 0;

        char line[160]{};
        snprintf(line, sizeof(line), "knife: first-person animation %s%s", problem[0] == '\0' ? "rebuilt (after the draw, once idle)" : "NOT rebuilt: ", problem);
        ReportKnife(line);
    }

    void ProcessKnife(C_CSWeaponBase* weapon, C_EconItemView* item, C_CSPlayerPawn* localPawn, bool forceUpdate, bool& didUpdate)
    {
        if (!weapon || !item || !localPawn)
            return;

        ProcessKnifeGraphRebuild(weapon, localPawn);

        if (g_restoreKnifeAfterDefault)
        {
            const std::uint16_t restoreDefinition = g_restoreKnifeDefinition;

            ResetKnifeDefaultRefresh();

            if (restoreDefinition >= 500 && KnifeModelChanger::Apply(weapon, item, restoreDefinition))
            {
                QueueKnifeGraphRebuild(weapon, restoreDefinition);
                SkinApplication::ClearHudWeaponIconFor(weapon);
                g_updateFrames.store(FORCE_UPDATE_FRAMES, std::memory_order_release);
                didUpdate = true;
            }

            return;
        }

        const Config::skin_changer_t& loadout = Config::GameLive();

        if (loadout.knifeDefinition < 500)
            return;

        const std::uint16_t targetDefinition = loadout.knifeDefinition;
        const std::uintptr_t knifeKey = reinterpret_cast<std::uintptr_t>(weapon);
        const bool demo = InDemo();

        // A demo: a knife already changed into another one keeps it until the next round's knife (another one picked in
        // the menu mid-replay) - its model isn't swapped while the recording animates it. A knife the recording put back
        // to the default one gets yours again.
        if (demo && g_demoKnives.contains(knifeKey))
        {
            const std::uint16_t current = item->m_iItemDefinitionIndex();

            if (current >= 500 && current != targetDefinition)
                return;
        }

        if (KnifeModelChanger::Apply(weapon, item, targetDefinition))
        {
            if (demo)
                g_demoKnives.insert(knifeKey);

            QueueKnifeGraphRebuild(weapon, targetDefinition);
            SkinApplication::ClearHudWeaponIconFor(weapon);
            g_updateFrames.store(FORCE_UPDATE_FRAMES, std::memory_order_release);
            didUpdate = true;
            return;
        }

        const auto configIt = loadout.knives.find(targetDefinition);
        Config::item_skin_t effectiveConfig;

        // Real StatTrak: the applied number comes from the kill tracker, not from the config.
        if (configIt != loadout.knives.end())
        {
            effectiveConfig = configIt->second;
            effectiveConfig.statTrakKills = StatTrakTracker::Displayed(targetDefinition, effectiveConfig);
        }

        const Config::item_skin_t* config = configIt != loadout.knives.end() ? &effectiveConfig : nullptr;

        if (!config || !config->enabled || config->paintKit <= 0)
        {
            const bool hadCustomPaint = weapon->m_nFallbackPaintKit() > 0;

            if (hadCustomPaint)
            {
                SkinApplication::ApplyDefault(weapon, item, localPawn, SkinApplication::ItemKind::Knife);

                const std::uint16_t temporaryDefinition = GetTemporaryKnifeDefinition(targetDefinition);

                // (not in a demo: no knife model swapped back and forth under the recording's animation)
                if (!demo && KnifeModelChanger::Apply(weapon, item, temporaryDefinition))
                {
                    g_restoreKnifeAfterDefault = true;
                    g_restoreKnifeDefinition = targetDefinition;
                    g_updateFrames.store(FORCE_UPDATE_FRAMES, std::memory_order_release);
                }

                SkinApplication::ClearHudWeaponIconFor(weapon);
                didUpdate = true;
                return;
            }

            if (forceUpdate || !SkinApplication::IsDefaultState(weapon, item, SkinApplication::ItemKind::Knife))
            {
                SkinApplication::ApplyDefault(weapon, item, localPawn, SkinApplication::ItemKind::Knife);
                SkinApplication::ClearHudWeaponIconFor(weapon);
                didUpdate = true;
            }

            return;
        }

        if (!SkinApplication::NeedsUpdate(weapon, item, *config, forceUpdate, SkinApplication::ItemKind::Knife) &&
            !RealItemIdBack(weapon, item, targetDefinition, NowMilliseconds()))
            return;

        SkinApplication::Apply(weapon, item, *config, localPawn, SkinApplication::ItemKind::Knife);
        SkinApplication::ClearHudWeaponIconFor(weapon);

        didUpdate = true;
    }

    // Charm upkeep (game thread). Your own gun's REAL charm comes from your inventory item (the game looks it up by
    // the item id — it is in neither attribute list of the weapon), and CS2 hangs it on by itself when the gun spawns
    // or comes out again. A gun with a picked charm is checked once a second: anything other than exactly the picked
    // charm hanging there → everything comes off and the picked one goes back on (at most 20 times per gun, so a
    // charm the game refuses can't loop forever).
    struct KeychainUpkeep
    {
        std::uint64_t nextCheckMs = 0;
        std::uint64_t reportAtMs = 0;   // the "3 s later" report (0 = none due)
        int repairs = 0;
        bool reported = false;          // the "first seen" report
    };

    std::unordered_map<std::uintptr_t, KeychainUpkeep> g_keychainUpkeep;
    constexpr std::uint64_t KEYCHAIN_CHECK_MS = 1000;
    constexpr std::uint64_t KEYCHAIN_REPORT_DELAY_MS = 3000;
    constexpr int KEYCHAIN_MAX_REPAIRS = 20;

    std::atomic<void (*)(const char*)> g_charmLog{ nullptr };   // Lootline's log; null in Chadware (no reports)
    std::atomic<bool (*)(std::uint64_t)> g_ownItemCheck{ nullptr };   // Lootline's IsLootlineId; null in Chadware
    std::uint16_t g_ownItemReported[16]{};   // definitions already reported in the log (once each)

    void ReportKnife(const char* line)
    {
        if (const auto log = g_charmLog.load(std::memory_order_acquire))
            log(line);
    }

    // The gun carries one of your own inventory items (SetOwnItemCheck): a server in your own game gave it. Its full item
    // id stays the item's even while the skin changer's id high (-1) is on.
    bool CarriesOwnItem(C_EconItemView* item, std::uint16_t definitionIndex)
    {
        const auto isOwnItem = g_ownItemCheck.load(std::memory_order_acquire);
        const std::uint32_t idOffset = SchemaFinder::GetCached<hash_32_fnv1a_const("C_EconItemView->m_iItemID")>();

        if (!isOwnItem || !item || !idOffset)
            return false;

        const std::uint64_t id = *reinterpret_cast<const std::uint64_t*>(reinterpret_cast<const std::uint8_t*>(item) + idOffset);

        if (!isOwnItem(id))
            return false;

        for (std::uint16_t& reported : g_ownItemReported)
        {
            if (reported == definitionIndex)
                break;

            if (reported == 0)
            {
                reported = definitionIndex;
                char line[200]{};
                snprintf(line, sizeof(line), "skin: weapon %u is your own Lootline item %llu (a server in your own game: you host / play "
                    "offline) - the game shows it as it is, the skin changer leaves it", static_cast<unsigned>(definitionIndex),
                    static_cast<unsigned long long>(id));
                ReportKnife(line);
                break;
            }
        }

        return true;
    }

    void ReportCharm(const char* when, C_CSWeaponBase* weapon, C_EconItemView* item, std::uint16_t definitionIndex, int wanted)
    {
        const auto log = g_charmLog.load(std::memory_order_acquire);

        if (!log || !weapon || !item)
            return;

        const std::uint32_t lowOffset = SchemaFinder::GetCached<hash_32_fnv1a_const("C_EconItemView->m_iItemIDLow")>();
        const std::uint32_t low = lowOffset ? *reinterpret_cast<std::uint32_t*>(reinterpret_cast<std::uint8_t*>(item) + lowOffset) : 0;
        char head[256]{};
        snprintf(head, sizeof(head), "charm, gun %u, %s: picked charm %d, item id %08X%08X, keychain attribute %d (networked list %d); ",
            static_cast<unsigned>(definitionIndex), when, wanted, item->m_iItemIDHigh(), low,
            EconItemAttributeManager::GetKeychainId(item, true), EconItemAttributeManager::GetKeychainId(item, false));

        const std::string line = head + SkinApplication::DescribeKeychains(weapon, g_ctx ? g_ctx->local_pawn : nullptr);
        log(line.c_str());
    }

    bool HasCustomGunConfiguration(const Config::item_skin_t& config)
    {
        if (config.paintKit > 0)
            return true;

        if (config.statTrakEnabled)
            return true;

        if (!config.customName.empty())
            return true;

        return WeaponCosmetics::HasAny(config);
    }

    void HashByte(std::uint64_t& hash, std::uint8_t value)
    {
        constexpr std::uint64_t FNV_PRIME = 1099511628211ULL;
        hash ^= value;
        hash *= FNV_PRIME;
    }

    template <typename T>
    void HashPod(std::uint64_t& hash, const T& value)
    {
        const auto* bytes = reinterpret_cast<const std::uint8_t*>(&value);

        for (std::size_t i = 0; i < sizeof(T); ++i)
            HashByte(hash, bytes[i]);
    }

    void HashFloat(std::uint64_t& hash, float value)
    {
        const std::uint32_t bits = std::bit_cast<std::uint32_t>(value);
        HashPod(hash, bits);
    }

    AppliedKeychainState GetDesiredKeychainState(const Config::item_skin_t& config)
    {
        AppliedKeychainState state{};
        state.effective = config.keychain.enabled && config.keychain.id > 0;

        if (!state.effective)
            return state;

        state.id = config.keychain.id;
        state.seed = (std::clamp)(config.keychain.seed, 0, 100000);
        state.sticker = config.keychain.sticker;
        state.offsetX = config.keychain.offsetX;
        state.offsetY = config.keychain.offsetY;
        state.offsetZ = config.keychain.offsetZ;
        return state;
    }

    bool KeychainStatesEqual(const AppliedKeychainState& lhs, const AppliedKeychainState& rhs)
    {
        if (lhs.effective != rhs.effective)
            return false;

        if (!lhs.effective)
            return true;

        return lhs.id == rhs.id &&
            lhs.seed == rhs.seed &&
            lhs.sticker == rhs.sticker &&
            lhs.offsetX == rhs.offsetX &&
            lhs.offsetY == rhs.offsetY &&
            lhs.offsetZ == rhs.offsetZ;
    }

    // A charm change already waiting (for the gun to come out) is the same as this one.
    bool IsPendingKeychainState(std::uintptr_t weaponKey, const AppliedKeychainState& desired)
    {
        const auto it = g_pendingKeychainOperations.find(weaponKey);
        return it != g_pendingKeychainOperations.end() && KeychainStatesEqual(it->second.desired, desired);
    }

    bool IsAppliedKeychainStateCurrent(std::uintptr_t weaponKey, const AppliedKeychainState& desired)
    {
        const auto it = g_lastAppliedKeychainState.find(weaponKey);

        if (it == g_lastAppliedKeychainState.end())
            return !desired.effective;

        return KeychainStatesEqual(it->second, desired);
    }

    void QueueKeychainState(const CBaseHandle& weaponHandle, C_CSWeaponBase* weapon, std::uintptr_t weaponKey, const Config::item_skin_t& config)
    {
        if (!weapon || !weaponHandle.valid())
            return;

        PendingKeychainOperation pending{};
        pending.weaponHandle = weaponHandle;
        pending.desired = GetDesiredKeychainState(config);

        // Rapid committed edits collapse to the newest desired state. Native
        // C_KeychainModule creation/destruction is deliberately not performed from
        // the pre-original FRAME_NET_UPDATE_END skin pass.
        g_pendingKeychainOperations[weaponKey] = pending;
    }


    std::uint64_t ComputeGunConfigHash(const Config::item_skin_t& config, bool withKills = true)
    {
        constexpr std::uint64_t FNV_OFFSET_BASIS = 14695981039346656037ULL;

        std::uint64_t hash = FNV_OFFSET_BASIS;

        HashPod(hash, config.paintKit);
        HashPod(hash, config.seed);
        HashFloat(hash, config.wear);
        HashPod(hash, config.statTrakEnabled);
        HashPod(hash, config.souvenir);

        if (withKills)
            HashPod(hash, config.statTrakKills);

        for (const char c : config.customName)
            HashByte(hash, static_cast<std::uint8_t>(c));

        HashByte(hash, 0xFF);

        for (const auto& sticker : config.stickers)
        {
            const bool effective = sticker.enabled && sticker.kit > 0;
            HashPod(hash, effective);

            if (!effective)
                continue;

            HashPod(hash, sticker.kit);
            HashFloat(hash, sticker.wear);
            HashFloat(hash, sticker.scale);
            HashFloat(hash, sticker.rotation);
            HashFloat(hash, sticker.offsetX);
            HashFloat(hash, sticker.offsetY);
        }

        const bool keychainEffective = config.keychain.enabled && config.keychain.id > 0;
        HashPod(hash, keychainEffective);

        if (keychainEffective)
        {
            HashPod(hash, config.keychain.id);
            HashPod(hash, config.keychain.seed);
            HashPod(hash, config.keychain.sticker);
            HashFloat(hash, config.keychain.offsetX);
            HashFloat(hash, config.keychain.offsetY);
            HashFloat(hash, config.keychain.offsetZ);
        }

        return hash;
    }

    void PruneAppliedKeychainState(const std::vector<CBaseHandle>& currentWeaponHandles)
    {
        for (auto it = g_lastAppliedKeychainState.begin(); it != g_lastAppliedKeychainState.end();)
        {
            bool stillPresent = false;

            for (const CBaseHandle& handle : currentWeaponHandles)
            {
                C_CSWeaponBase* weapon = GetWeaponFromHandle(handle);

                if (reinterpret_cast<std::uintptr_t>(weapon) == it->first)
                {
                    stillPresent = true;
                    break;
                }
            }

            if (stillPresent)
                ++it;
            else
                it = g_lastAppliedKeychainState.erase(it);
        }
        for (auto it = g_pendingKeychainOperations.begin(); it != g_pendingKeychainOperations.end();)
        {
            bool stillPresent = false;

            for (const CBaseHandle& handle : currentWeaponHandles)
            {
                C_CSWeaponBase* weapon = GetWeaponFromHandle(handle);

                if (reinterpret_cast<std::uintptr_t>(weapon) == it->first)
                {
                    stillPresent = true;
                    break;
                }
            }

            if (stillPresent)
                ++it;
            else
                it = g_pendingKeychainOperations.erase(it);
        }

    }

    void ProcessGun(const CBaseHandle& weaponHandle, C_CSWeaponBase* weapon, C_EconItemView* item, C_CSPlayerPawn* localPawn, std::uint16_t definitionIndex, bool forceUpdate, bool& didUpdate)
    {
        if (!weapon || !item || !localPawn)
            return;

        const std::uintptr_t weaponKey = reinterpret_cast<std::uintptr_t>(weapon);
        const Config::skin_changer_t& loadout = Config::GameLive();
        const auto configIt = loadout.weapons.find(definitionIndex);
        const std::uint64_t nowMs = NowMilliseconds();
        KeychainUpkeep& upkeep = g_keychainUpkeep[weaponKey];

        // charm reports (Lootline's log only): before anything is changed on this gun, and 3 s after ours went on
        if (g_charmLog.load(std::memory_order_acquire))
        {
            const bool picked = configIt != loadout.weapons.end() && configIt->second.enabled &&
                configIt->second.keychain.enabled && configIt->second.keychain.id > 0;
            const int wanted = picked ? configIt->second.keychain.id : 0;

            if (!upkeep.reported)
            {
                upkeep.reported = true;
                int hanging = 0;

                if (picked || SkinApplication::WorldKeychains(weapon, hanging) > 0)
                    ReportCharm("first seen", weapon, item, definitionIndex, wanted);
            }

            if (upkeep.reportAtMs != 0 && nowMs >= upkeep.reportAtMs)
            {
                upkeep.reportAtMs = 0;
                ReportCharm("3 s after ours went on", weapon, item, definitionIndex, wanted);
            }
        }

        // your own Lootline item, given by a server in your own game: it already looks like it should (see SetOwnItemCheck)
        if (CarriesOwnItem(item, definitionIndex))
        {
            g_lastAppliedGunConfigHash.erase(weaponKey);
            return;
        }

        if (configIt == loadout.weapons.end())
        {
            const auto keychainIt = g_lastAppliedKeychainState.find(weaponKey);
            const bool hadKeychain =
                keychainIt != g_lastAppliedKeychainState.end() && keychainIt->second.effective;

            if (hadKeychain)
            {
                PendingKeychainOperation pending{};
                pending.weaponHandle = weaponHandle;
                pending.desired = AppliedKeychainState{};
                g_pendingKeychainOperations[weaponKey] = pending;

                SkinApplication::ApplyDefault(weapon, item, localPawn, SkinApplication::ItemKind::Weapon);
                SkinApplication::ClearHudWeaponIconFor(weapon);
                didUpdate = true;
            }

            g_lastAppliedGunConfigHash.erase(weaponKey);
            return;
        }

        // Real StatTrak: the applied number comes from the kill tracker, not from the config.
        Config::item_skin_t effectiveConfig = configIt->second;
        effectiveConfig.statTrakKills = StatTrakTracker::Displayed(definitionIndex, effectiveConfig);

        const Config::item_skin_t& config = effectiveConfig;
        const AppliedKeychainState desiredKeychain = GetDesiredKeychainState(config);
        const bool keychainStateChanged =
            !IsAppliedKeychainStateCurrent(weaponKey, desiredKeychain) && !IsPendingKeychainState(weaponKey, desiredKeychain);

        if (!config.enabled || !HasCustomGunConfiguration(config))
        {
            const auto keychainIt = g_lastAppliedKeychainState.find(weaponKey);
            const bool hadKeychain =
                keychainIt != g_lastAppliedKeychainState.end() && keychainIt->second.effective;

            if (hadKeychain)
            {
                PendingKeychainOperation pending{};
                pending.weaponHandle = weaponHandle;
                pending.desired = AppliedKeychainState{};
                g_pendingKeychainOperations[weaponKey] = pending;
            }

            if (forceUpdate || hadKeychain || !SkinApplication::IsDefaultState(weapon, item, SkinApplication::ItemKind::Weapon))
            {
                SkinApplication::ApplyDefault(weapon, item, localPawn, SkinApplication::ItemKind::Weapon);
                SkinApplication::ClearHudWeaponIconFor(weapon);
                didUpdate = true;
            }

            g_lastAppliedGunConfigHash.erase(weaponKey);
            return;
        }

        // The picked charm is on: make sure it's the only one (CS2 puts your real one back when the gun comes out).
        if (desiredKeychain.effective && !keychainStateChanged && nowMs >= upkeep.nextCheckMs &&
            upkeep.repairs < KEYCHAIN_MAX_REPAIRS && !g_pendingKeychainOperations.contains(weaponKey) && IsActiveWeapon(weapon))
        {
            upkeep.nextCheckMs = nowMs + KEYCHAIN_CHECK_MS;
            int hanging = 0;
            const int count = SkinApplication::WorldKeychains(weapon, hanging);

            if (count != 1 || (hanging != 0 && hanging != desiredKeychain.id))
            {
                ++upkeep.repairs;
                QueueKeychainState(weaponHandle, weapon, weaponKey, config);

                if (const auto log = g_charmLog.load(std::memory_order_acquire))
                {
                    char line[160]{};
                    snprintf(line, sizeof(line), "charm, gun %u: %d hanging (first is charm %d), putting charm %d back (%d/%d)",
                        static_cast<unsigned>(definitionIndex), count, hanging, desiredKeychain.id, upkeep.repairs, KEYCHAIN_MAX_REPAIRS);
                    log(line);
                }
            }
        }

        const std::uint64_t configHash = ComputeGunConfigHash(config);
        const std::uint64_t lookHash = ComputeGunConfigHash(config, false);
        const auto hashIt = g_lastAppliedGunConfigHash.find(weaponKey);
        const bool cosmeticStateChanged =
            hashIt == g_lastAppliedGunConfigHash.end() ||
            hashIt->second != configHash;

        // the real item's attributes (stickers, charm...) arrived from the server (again: a real StatTrak kill re-sends
        // them): ours go on again, without them
        const bool realItemShows =
            EconItemAttributeManager::ReplaceRealItem() && EconItemAttributeManager::HasNetworkedAttributes(item);

        if (!cosmeticStateChanged &&
            !keychainStateChanged &&
            !realItemShows &&
            !SkinApplication::NeedsUpdate(weapon, item, config, forceUpdate, SkinApplication::ItemKind::Weapon) &&
            !RealItemIdBack(weapon, item, definitionIndex, nowMs))
        {
            return;
        }

        // Only the StatTrak count changed (a kill) on the gun in your hands, and a charm hangs on it: re-applying now
        // would take the charm off and hang it back on in front of you. The counter catches up once it's put away.
        if (cosmeticStateChanged && !keychainStateChanged && !forceUpdate && desiredKeychain.effective && IsActiveWeapon(weapon))
        {
            const auto lookIt = g_lastAppliedGunLookHash.find(weaponKey);

            if (lookIt != g_lastAppliedGunLookHash.end() && lookIt->second == lookHash &&
                !SkinApplication::NeedsUpdate(weapon, item, config, false, SkinApplication::ItemKind::Weapon, true))
                return;
        }

        // Apply keychains after the normal attribute/material refresh has finished.
        if (desiredKeychain.effective || keychainStateChanged)
            QueueKeychainState(weaponHandle, weapon, weaponKey, config);

        // no charm of ours, but the real item has one hanging: it comes off (once the gun is in your hands)
        if (realItemShows && !desiredKeychain.effective && EconItemAttributeManager::GetKeychainId(item, false) > 0)
        {
            PendingKeychainOperation& pending = g_pendingKeychainOperations[weaponKey];
            pending.weaponHandle = weaponHandle;
            pending.clearWorld = true;
        }

        SkinApplication::Apply(weapon, item, config, localPawn, SkinApplication::ItemKind::Weapon);
        SkinApplication::ClearHudWeaponIconFor(weapon);

        g_lastAppliedGunConfigHash[weaponKey] = configHash;
        g_lastAppliedGunLookHash[weaponKey] = lookHash;
        didUpdate = true;
    }

    // ── your guns out of your hands ──
    // When a gun leaves your hands (dropped, also when you die) CS2 sends it again with its real item: a dropped Lootline
    // Glock turned plain (its real item is the stock one), a dropped AK showed your real Steam skin. Like a real skin, yours
    // stays on your gun wherever it is - dropped, or picked up by someone else. "Yours" = the gun's first owner
    // (m_OriginalOwnerXuidLow) is you, or it left your hands and its first owner isn't someone else (CS2 leaves it 0 on the
    // guns it hands out at spawn). Every 0.5 s over the networked entities; not in demos.
    constexpr std::uint64_t WORLD_SCAN_MS = 500;
    constexpr int MAX_NETWORKED_INDEX = 0x3FFF;
    constexpr std::size_t MAX_LEFT_HANDS = 32;
    std::uint64_t g_worldScanNextMs = 0;
    std::vector<std::uint32_t> g_leftYourHands;   // full entity handles (serial too: a reused slot isn't the same gun)
    int g_worldReports = 0;
    constexpr int MAX_WORLD_REPORTS = 40;

    void RememberLeftHands(const CBaseHandle& handle)
    {
        if (!handle.valid() || std::find(g_leftYourHands.begin(), g_leftYourHands.end(), handle.value()) != g_leftYourHands.end())
            return;

        if (g_leftYourHands.size() >= MAX_LEFT_HANDS)
            g_leftYourHands.erase(g_leftYourHands.begin());

        g_leftYourHands.push_back(handle.value());
    }

    // A gun entity (designer name "weapon_...") and its full entity handle. SEH: straight from the game.
    bool WorldGun(const void* entity, std::uint32_t& handle)
    {
        __try
        {
            const auto* identity = *reinterpret_cast<const std::uint8_t* const*>(static_cast<const std::uint8_t*>(entity) + 0x10);

            if (!identity)
                return false;

            const char* name = *reinterpret_cast<const char* const*>(identity + 0x20);

            if (!name || std::strncmp(name, "weapon_", 7) != 0)
                return false;

            handle = *reinterpret_cast<const std::uint32_t*>(identity + 0x10);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    void ProcessWorldGuns(C_CSPlayerPawn* localPawn, const std::vector<CBaseHandle>& held, bool& didUpdate)
    {
        const std::uint64_t nowMs = NowMilliseconds();

        if (nowMs < g_worldScanNextMs || InDemo())
            return;

        g_worldScanNextMs = nowMs + WORLD_SCAN_MS;

        const std::uint32_t mine = LocalAccountId(localPawn);
        const std::uint32_t ownerOffset = SchemaFinder::GetCached<hash_32_fnv1a_const("C_EconEntity->m_OriginalOwnerXuidLow")>();

        if (!mine || !ownerOffset)
            return;

        std::vector<std::uint32_t> stillThere;

        for (int index = 1; index <= MAX_NETWORKED_INDEX; ++index)
        {
            auto* weapon = I::GameEntity->Instance->Get<C_CSWeaponBase>(index);
            std::uint32_t raw = 0;

            if (!weapon || !WorldGun(weapon, raw))
                continue;

            const CBaseHandle handle(static_cast<int>(raw & ENT_ENTRY_MASK), static_cast<int>(raw >> 15));

            if (std::find(held.begin(), held.end(), handle) != held.end())
                continue;   // in your hands: SkinChanger::Run's own loop

            const bool leftYourHands = std::find(g_leftYourHands.begin(), g_leftYourHands.end(), raw) != g_leftYourHands.end();
            const std::uint32_t owner = *reinterpret_cast<const std::uint32_t*>(reinterpret_cast<const std::uint8_t*>(weapon) + ownerOffset);

            if (leftYourHands)
                stillThere.push_back(raw);

            if (leftYourHands ? (owner != 0 && owner != mine) : (owner == 0 || owner != mine))
                continue;   // someone else's (or a gun nobody can tell): never painted

            C_EconItemView* item = GetWeaponItem(weapon);

            if (!item)
                continue;

            const std::uint16_t definitionIndex = item->m_iItemDefinitionIndex();

            if (KnifeModelChanger::IsKnifeDefinition(definitionIndex))
                continue;   // knives: in your hands only

            bool updated = false;
            ProcessGun(handle, weapon, item, localPawn, definitionIndex, false, updated);

            if (updated)
            {
                didUpdate = true;

                if (g_worldReports < MAX_WORLD_REPORTS)
                {
                    ++g_worldReports;
                    char line[160]{};
                    snprintf(line, sizeof(line), "skin: your gun %u out of your hands (%s) got your skin back",
                        static_cast<unsigned>(definitionIndex), leftYourHands ? "it left your hands" : "first owner: you");
                    ReportKnife(line);
                }
            }
        }

        g_leftYourHands = std::move(stillThere);   // gone ones (round restart, cleaned up) are forgotten
    }
}

void SkinChanger::Run()
{
    C_CSPlayerPawn* localPawn = g_ctx ? g_ctx->local_pawn : nullptr;

    // Config values are intentionally allowed to change while the user drags a menu
    // control, but the actual held weapon is not regenerated until the edit commits.
    if (IsInteractiveEditActive())
        return;

    // Which side you're on decides which loadout is applied. Switching sides re-applies everything from the
    // other side's loadout.
    if (localPawn)
    {
        const Team team = localPawn->getTeam();

        if (team == Team::T || team == Team::CT)
        {
            const int side = team == Team::CT ? Config::TEAM_CT : Config::TEAM_T;

            if (Config::liveTeam.exchange(side, std::memory_order_relaxed) != side)
            {
                g_updateFrames.store(FORCE_UPDATE_FRAMES, std::memory_order_release);
                g_lastAppliedGunConfigHash.clear();
                g_lastAppliedGunLookHash.clear();
                GloveChanger::ForceUpdate();
                ResetKnifeDefaultRefresh();
            }
        }
    }

    // New kills → credited to the StatTrak counter of the gun / knife in your hands. The counter is part of the
    // gun config hash (and the knife's NeedsUpdate check), so only that one weapon re-applies.
    StatTrakTracker::Poll(localPawn);

    if (!Config::Game().enabled)
    {
        if (localPawn && localPawn->m_iHealth() > 0)
            GloveChanger::Run(localPawn);

        MusicKitChanger::Run(localPawn);   // puts your own music kit back if we'd changed it

        g_updateFrames.store(FORCE_UPDATE_FRAMES, std::memory_order_release);
        g_lastWeaponHandles.clear();
        g_lastAppliedGunConfigHash.clear();
        g_lastAppliedGunLookHash.clear();
        g_lastAppliedKeychainState.clear();
        g_pendingKeychainOperations.clear();
        g_keychainUpkeep.clear();
        g_idBack.clear();
        g_demoKnives.clear();
        g_knifeGraphWeapon = 0;
        ResetKnifeDefaultRefresh();
        return;
    }

    // the knife in your hand on this side, for whoever needs it off this thread (the kill feed)
    g_liveKnifeDefinition.store(Config::GameLive().knifeDefinition, std::memory_order_relaxed);

    // Music kit lives on the controller, not the pawn — it applies whether you're alive or dead
    // (MVP anthems play at round end, often while you're spectating).
    MusicKitChanger::Run(localPawn);

    // your guns out of your hands (dropped, picked up by someone): also while you're dead / spectating
    if (localPawn && I::GameEntity && I::GameEntity->Instance)
    {
        const bool alive = localPawn->m_iHealth() > 0;

        if (!alive)
            for (const CBaseHandle& handle : g_lastWeaponHandles)
                RememberLeftHands(handle);   // you died: everything you held left your hands

        bool worldUpdate = false;
        ProcessWorldGuns(localPawn, alive ? g_lastWeaponHandles : std::vector<CBaseHandle>{}, worldUpdate);

        if (worldUpdate)
            SkinApplication::RegenerateSkins();
    }

    if (!localPawn || !I::GameEntity || !I::GameEntity->Instance || localPawn->m_iHealth() <= 0)
        return;

    // "Every Round" randomizer: flag each respawn. The roll itself runs on the render thread (Menu::render),
    // where every other Config::skin_changer write happens. The first spawn seen after injecting is skipped.
    {
        static float lastSpawnTime = -1.0f;
        const float spawnTime = localPawn->m_flLastSpawnTimeIndex();

        if (spawnTime != lastSpawnTime)
        {
            if (lastSpawnTime >= 0.0f && Config::menu.randomEveryRound)
                Randomizer::RequestRandomize();

            lastSpawnTime = spawnTime;
        }
    }

    GloveChanger::Run(localPawn);

    CCSPlayer_WeaponServices* weaponServices = localPawn->m_pWeaponServices();

    if (!weaponServices)
        return;

    auto& weaponHandles = weaponServices->m_hMyWeapons();

    std::vector<CBaseHandle> currentWeaponHandles;
    currentWeaponHandles.reserve(weaponHandles.count());

    for (int i = 0; i < weaponHandles.count(); ++i)
    {
        const CBaseHandle handle = weaponHandles.element(i);

        if (handle.valid())
            currentWeaponHandles.emplace_back(handle);
    }

    if (currentWeaponHandles != g_lastWeaponHandles)
    {
        // guns that just left your hands (dropped / thrown): they keep your skin (ProcessWorldGuns)
        for (const CBaseHandle& handle : g_lastWeaponHandles)
            if (std::find(currentWeaponHandles.begin(), currentWeaponHandles.end(), handle) == currentWeaponHandles.end())
                RememberLeftHands(handle);

        // Forget charm upkeep for weapons that are gone (their address may be reused by a new weapon).
        for (auto it = g_keychainUpkeep.begin(); it != g_keychainUpkeep.end();)
        {
            bool present = false;

            for (const CBaseHandle& handle : currentWeaponHandles)
            {
                if (reinterpret_cast<std::uintptr_t>(GetWeaponFromHandle(handle)) == it->first)
                {
                    present = true;
                    break;
                }
            }

            it = present ? std::next(it) : g_keychainUpkeep.erase(it);
        }

        // the same for the "real item back" counts and a demo's changed knives
        const auto gone = [&](std::uintptr_t key)
        {
            return std::none_of(currentWeaponHandles.begin(), currentWeaponHandles.end(),
                [&](const CBaseHandle& handle) { return reinterpret_cast<std::uintptr_t>(GetWeaponFromHandle(handle)) == key; });
        };

        std::erase_if(g_idBack, [&](const auto& entry) { return gone(entry.first); });
        std::erase_if(g_demoKnives, [&](std::uintptr_t key) { return gone(key); });

        // Only the guns that are gone are forgotten: re-applying every gun because a grenade left your hands (or you
        // bought / dropped / picked up something) took every charm off and hung it back on - the charm "pop". A new
        // gun applies by itself (NeedsUpdate: its fallback paint kit isn't ours yet).
        for (auto it = g_lastAppliedGunConfigHash.begin(); it != g_lastAppliedGunConfigHash.end();)
        {
            bool present = false;

            for (const CBaseHandle& handle : currentWeaponHandles)
            {
                if (reinterpret_cast<std::uintptr_t>(GetWeaponFromHandle(handle)) == it->first)
                {
                    present = true;
                    break;
                }
            }

            // a gun that left your hands keeps its applied state too (ProcessWorldGuns): no re-apply flash on it
            // every time you buy something or throw a grenade
            for (std::size_t i = 0; !present && i < g_leftYourHands.size(); ++i)
            {
                const std::uint32_t raw = g_leftYourHands[i];
                const CBaseHandle handle(static_cast<int>(raw & ENT_ENTRY_MASK), static_cast<int>(raw >> 15));
                present = reinterpret_cast<std::uintptr_t>(GetWeaponFromHandle(handle)) == it->first;
            }

            if (present)
            {
                ++it;
            }
            else
            {
                g_lastAppliedGunLookHash.erase(it->first);
                it = g_lastAppliedGunConfigHash.erase(it);
            }
        }

        g_lastWeaponHandles = currentWeaponHandles;
        PruneAppliedKeychainState(currentWeaponHandles);
        ResetKnifeDefaultRefresh();
    }

    const bool forceUpdate = g_updateFrames.load(std::memory_order_acquire) > 0;

    // Each forced application frame extends the preview quiet period. This means the
    // standalone preview starts only after the last live-weapon refresh frame rather
    // than racing the same custom-material pipeline.
    if (forceUpdate)
        ExtendPreviewUpdateBlock();

    bool didUpdate = false;

    for (const CBaseHandle& handle : currentWeaponHandles)
    {
        C_CSWeaponBase* weapon = GetWeaponFromHandle(handle);

        if (!weapon)
            continue;

        C_EconItemView* item = GetWeaponItem(weapon);

        if (!item)
            continue;

        // someone else's gun you picked up: it keeps its own skin (yours only go on your own guns)
        if (!IsOwnWeapon(weapon, localPawn))
            continue;

        const std::uint16_t definitionIndex = item->m_iItemDefinitionIndex();

        if (KnifeModelChanger::IsKnifeDefinition(definitionIndex))
        {
            ProcessKnife(weapon, item, localPawn, forceUpdate, didUpdate);
            continue;
        }

        ProcessGun(handle, weapon, item, localPawn, definitionIndex, forceUpdate, didUpdate);
    }

    if (didUpdate)
    {
        SkinApplication::RegenerateSkins();
        ExtendPreviewUpdateBlock();
    }

    if (forceUpdate)
        g_updateFrames.fetch_sub(1, std::memory_order_acq_rel);
}

void SkinChanger::RunAgentChanger()
{
    // a demo: no agent model change, no model swap for a frame (RefreshPlayerModel) - the recording animates the players
    if (!I::EngineClient || !I::EngineClient->connected() || !I::EngineClient->in_game() || InDemo())
    {
        ResetAgentRuntime();
        return;
    }

    C_CSPlayerPawn* localPawn = I::GetLocalPawn();

    if (!localPawn || localPawn->m_iHealth() <= 0)
    {
        ResetAgentRuntime();
        return;
    }

    const std::uintptr_t pawnAddress = reinterpret_cast<std::uintptr_t>(localPawn);
    const Team team = localPawn->getTeam();
    const float spawnTime = localPawn->m_flLastSpawnTimeIndex();

    if (pawnAddress != g_agentPawnAddress || team != g_agentTeam || spawnTime != g_agentSpawnTime)
    {
        g_agentPawnAddress = pawnAddress;
        g_agentTeam = team;
        g_agentSpawnTime = spawnTime;
        g_originalAgentModel.clear();
        g_agentCustomApplied = false;
        g_activeAgentDefinition = 0;
        g_failedAgentDefinition = 0;
        g_lastAgentTargetDefinition = 0;
        g_agentApplyAttempts = 0;
        g_forceAgentUpdate.store(true, std::memory_order_release);
        g_modelSwapStep = 0;
        g_modelSwapBack.clear();
        return;
    }

    // RefreshPlayerModel: another model for one frame, then this one again. The game rebuilds your first-person
    // gloves on a model change (it raises m_bNeedToReApplyGloves) and the glove changer puts yours back on.
    if (g_modelSwapStep == 1)
    {
        g_modelSwapStep = 0;

        if (!SetAgentModel(localPawn, g_modelSwapBack.c_str()))
            GloveChanger::LogEvent(localPawn, "model swap: couldn't put the model back");

        g_modelSwapBack.clear();
        return;
    }

    if (g_modelSwapRequest.exchange(0, std::memory_order_acq_rel) != 0)
    {
        const char* nowPtr = GetCurrentPlayerModelName(localPawn);
        const std::string current = nowPtr ? nowPtr : "";
        const std::string other = current.empty() ? std::string() : OtherAgentModel(team, current);

        if (!other.empty() && SetAgentModel(localPawn, other.c_str()))
        {
            g_modelSwapBack = current;
            g_modelSwapStep = 1;
            return;
        }

        GloveChanger::LogEvent(localPawn, "model swap: no other model to swap to");
    }

    std::uint16_t targetDefinition = 0;

    // Agents are always per side: the T pick on T, the CT pick on CT.
    if (Config::Game().enabled && (team == Team::T || team == Team::CT))
        targetDefinition = Config::GameAgentFor(team == Team::CT ? Config::TEAM_CT : Config::TEAM_T);

    const ItemDefinitionInfo* targetItem = targetDefinition != 0 ? Skins::g_ItemSchema.GetItem(targetDefinition) : nullptr;

    if (targetItem && (!targetItem->isAgent || !targetItem->hasUsableAgentModel || targetItem->modelName.empty()))
        targetItem = nullptr;

    // Never put a T agent on a CT (or the other way round) — e.g. an old single-agent config.
    if (targetItem &&
        ((team == Team::T && !targetItem->isTerroristAgent) || (team == Team::CT && !targetItem->isCounterTerroristAgent)))
    {
        targetItem = nullptr;
        targetDefinition = 0;
    }

    if (targetDefinition != g_lastAgentTargetDefinition)
    {
        g_lastAgentTargetDefinition = targetDefinition;
        g_agentApplyAttempts = 0;
    }

    if (g_failedAgentDefinition != 0 && targetDefinition != g_failedAgentDefinition)
        g_failedAgentDefinition = 0;

    if (targetItem && g_failedAgentDefinition == targetDefinition)
    {
        g_forceAgentUpdate.store(false, std::memory_order_release);
        return;
    }

    const char* currentModelPtr = GetCurrentPlayerModelName(localPawn);
    const std::string currentModel = currentModelPtr ? currentModelPtr : "";

    if (!targetItem)
    {
        if (g_agentCustomApplied && !g_originalAgentModel.empty() && (currentModel.empty() || g_originalAgentModel != currentModel))
            SetAgentModel(localPawn, g_originalAgentModel.c_str());

        g_agentCustomApplied = false;
        g_activeAgentDefinition = 0;
        g_agentApplyAttempts = 0;
        g_forceAgentUpdate.store(false, std::memory_order_release);
        return;
    }

    if (!g_agentCustomApplied && g_originalAgentModel.empty() && !currentModel.empty() && targetItem->modelName != currentModel)
        g_originalAgentModel = currentModel;

    const bool forceUpdate = g_forceAgentUpdate.exchange(false, std::memory_order_acq_rel);
    const bool needsApply = forceUpdate || currentModel.empty() || targetItem->modelName != currentModel;

    if (!needsApply)
    {
        g_agentCustomApplied = true;
        g_activeAgentDefinition = targetDefinition;
        g_agentApplyAttempts = 0;
        return;
    }

    const std::string beforeModel = currentModel;
    const bool setResult = SetAgentModel(localPawn, targetItem->modelName.c_str());
    const char* afterModelPtr = GetCurrentPlayerModelName(localPawn);
    const std::string afterModel = afterModelPtr ? afterModelPtr : "";

    ++g_agentApplyAttempts;

    if (IsErrorPlayerModelName(afterModel))
    {
        std::string rollbackModel;

        if (!beforeModel.empty() && !IsErrorPlayerModelName(beforeModel))
            rollbackModel = beforeModel;
        else if (!g_originalAgentModel.empty() && !IsErrorPlayerModelName(g_originalAgentModel))
            rollbackModel = g_originalAgentModel;

        if (!rollbackModel.empty())
            SetAgentModel(localPawn, rollbackModel.c_str());

        g_failedAgentDefinition = targetDefinition;
        g_agentCustomApplied = g_activeAgentDefinition != 0;
        g_forceAgentUpdate.store(false, std::memory_order_release);
        return;
    }

    if (setResult && afterModel == targetItem->modelName)
    {
        g_agentCustomApplied = true;
        g_activeAgentDefinition = targetDefinition;
        g_agentApplyAttempts = 0;
        return;
    }

    g_agentCustomApplied = false;
    g_forceAgentUpdate.store(g_agentApplyAttempts < 5, std::memory_order_release);
}

void SkinChanger::ForceAgentUpdate()
{
    g_forceAgentUpdate.store(true, std::memory_order_release);
}

void SkinChanger::RefreshPlayerModel()
{
    g_modelSwapRequest.store(1, std::memory_order_release);
}

bool SkinChanger::IsAgentDefinitionRejected(std::uint16_t definitionIndex)
{
    (void)definitionIndex;
    return false;
}

void SkinChanger::PrecacheAgentModels()
{
    if (!Skins::g_ItemSchema.IsInitialized())
        return;

    for (const ItemDefinitionInfo& item : Skins::g_ItemSchema.GetItems())
    {
        if (item.isAgent && item.hasUsableAgentModel && !item.modelName.empty())
            PrecacheAgentModel(item.modelName);
    }
}

void SkinChanger::ProcessPendingKeychains()
{
    if (g_updateFrames.load(std::memory_order_acquire) > 0 || g_pendingKeychainOperations.empty())
        return;

    for (auto it = g_pendingKeychainOperations.begin(); it != g_pendingKeychainOperations.end();)
    {
        const std::uintptr_t weaponKey = it->first;
        const PendingKeychainOperation pending = it->second;
        C_CSWeaponBase* weapon = GetWeaponFromHandle(pending.weaponHandle);

        if (!weapon || reinterpret_cast<std::uintptr_t>(weapon) != weaponKey)
        {
            it = g_pendingKeychainOperations.erase(it);
            continue;
        }

        C_EconItemView* item = GetWeaponItem(weapon);

        if (!item)
        {
            it = g_pendingKeychainOperations.erase(it);
            continue;
        }

        // Holstered: it waits until you pull the gun out (see IsActiveWeapon).
        if (!IsActiveWeapon(weapon))
        {
            ++it;
            continue;
        }

        const auto currentIt = g_lastAppliedKeychainState.find(weaponKey);
        const bool hadKeychain =
            currentIt != g_lastAppliedKeychainState.end() && currentIt->second.effective;

        bool completed = true;
        int removed = 0;

        if (!pending.desired.effective)
        {
            if (hadKeychain)
            {
                // The item has no charm any more: AddKeyChainEntity takes ours off (and puts back whatever charm the
                // game itself finds on the item — your real one, if the gun has one).
                weapon->AddKeyChainEntity();
                SkinApplication::RebuildHudKeychainAddon(weapon, g_ctx ? g_ctx->local_pawn : nullptr);
            }
            else if (pending.clearWorld)
            {
                // the real item's charm, hanging since the gun was made: off (its attributes are gone, nothing hangs it back)
                removed = SkinApplication::RemoveWorldKeychains(weapon);
                SkinApplication::RebuildHudKeychainAddon(weapon, g_ctx ? g_ctx->local_pawn : nullptr);
            }
        }
        else
        {
            completed = EconItemAttributeManager::SetKeychainId(item, pending.desired.id);

            if (completed)
            {
                // EVERY charm comes off first — ours from before AND the real one CS2 hung on from your inventory item.
                // AddKeyChainEntity alone only swaps the one charm it finds itself, which left the real charm swinging
                // next to ours (the "double charm"). Then ours goes on, and the first-person model rebuilds its copy.
                removed = SkinApplication::RemoveWorldKeychains(weapon);
                weapon->AddKeyChainEntity();
                SkinApplication::RebuildHudKeychainAddon(weapon, g_ctx ? g_ctx->local_pawn : nullptr);
            }
        }

        if (completed)
        {
            g_lastAppliedKeychainState[weaponKey] = pending.desired;

            if (pending.desired.effective && g_charmLog.load(std::memory_order_acquire))
            {
                KeychainUpkeep& upkeep = g_keychainUpkeep[weaponKey];
                upkeep.reportAtMs = NowMilliseconds() + KEYCHAIN_REPORT_DELAY_MS;
                const std::string when = "ours just went on (" + std::to_string(removed) + " taken off first)";
                ReportCharm(when.c_str(), weapon, item, item->m_iItemDefinitionIndex(), pending.desired.id);
            }
        }

        it = g_pendingKeychainOperations.erase(it);
    }
}

std::uint16_t SkinChanger::LiveKnifeDefinition()
{
    return Config::Game().enabled ? g_liveKnifeDefinition.load(std::memory_order_relaxed) : 0;
}

void SkinChanger::SetInteractiveEditActive(bool active)
{
    if (active)
    {
        g_interactiveEditHeartbeatMs.store(NowMilliseconds(), std::memory_order_release);
        g_interactiveEditActive.store(true, std::memory_order_release);
        return;
    }

    g_interactiveEditActive.store(false, std::memory_order_release);
}

bool SkinChanger::IsInteractiveEditActive()
{
    if (!g_interactiveEditActive.load(std::memory_order_acquire))
        return false;

    const std::uint64_t heartbeat =
        g_interactiveEditHeartbeatMs.load(std::memory_order_acquire);

    const std::uint64_t now = NowMilliseconds();

    if (heartbeat == 0 || now - heartbeat > INTERACTIVE_EDIT_LEASE_MS)
    {
        g_interactiveEditActive.store(false, std::memory_order_release);
        return false;
    }

    return true;
}

bool SkinChanger::IsUpdatePending()
{
    // Main-menu previews do not have a live weapon update to wait for.
    if (!HasLivePreviewUpdateTarget())
        return false;

    if (g_updateFrames.load(std::memory_order_acquire) > 0)
        return true;

    return NowMilliseconds() <
        g_previewBlockedUntilMs.load(std::memory_order_acquire);
}

void SkinChanger::ForceUpdate()
{
    // Preserve the frame budget while an interactive edit is still active.
    g_updateFrames.store(FORCE_UPDATE_FRAMES, std::memory_order_release);
}

void SkinChanger::Reset()
{
    g_interactiveEditActive.store(false, std::memory_order_release);
    g_interactiveEditHeartbeatMs.store(0, std::memory_order_release);
    g_previewBlockedUntilMs.store(0, std::memory_order_release);

    g_updateFrames.store(FORCE_UPDATE_FRAMES, std::memory_order_release);
    g_lastWeaponHandles.clear();
    g_lastAppliedGunConfigHash.clear();
    g_lastAppliedGunLookHash.clear();
    g_lastAppliedKeychainState.clear();
    g_pendingKeychainOperations.clear();
    g_keychainUpkeep.clear();
    g_idBack.clear();
    g_demoKnives.clear();
    ResetKnifeDefaultRefresh();
    ResetAgentRuntime();
    g_failedAgentDefinition = 0;
    GloveChanger::Reset();
    MusicKitChanger::Reset();
}

std::vector<std::uint16_t> SkinChanger::GetOwnedWeaponDefinitionIndices()
{
    std::lock_guard lock(g_ownedWeaponsMutex);
    return g_ownedWeapons;
}

void SkinChanger::UpdateLiveInfo(void* pawn)
{
    C_CSPlayerPawn* localPawn = static_cast<C_CSPlayerPawn*>(pawn);
    const bool alive = localPawn && localPawn->m_iHealth() > 0;
    g_localAlive.store(alive, std::memory_order_release);

    std::vector<std::uint16_t> result;

    if (!alive || !I::GameEntity || !I::GameEntity->Instance || !Skins::g_ItemSchema.IsInitialized())
    {
        std::lock_guard lock(g_ownedWeaponsMutex);
        g_ownedWeapons.clear();
        return;
    }

    CCSPlayer_WeaponServices* weaponServices = localPawn->m_pWeaponServices();

    if (!weaponServices)
    {
        std::lock_guard lock(g_ownedWeaponsMutex);
        g_ownedWeapons.clear();
        return;
    }

    const auto appendWeapon = [&](const CBaseHandle& handle)
    {
        C_CSWeaponBase* weapon = GetWeaponFromHandle(handle);

        if (!weapon)
            return;

        C_EconItemView* item = GetWeaponItem(weapon);

        if (!item)
            return;

        const std::uint16_t definitionIndex = item->m_iItemDefinitionIndex();

        if (KnifeModelChanger::IsKnifeDefinition(definitionIndex))
            return;

        const ItemDefinitionInfo* itemInfo = Skins::g_ItemSchema.GetItem(definitionIndex);

        if (!itemInfo || !itemInfo->isWeapon)
            return;

        if (std::find(result.begin(), result.end(), definitionIndex) == result.end())
            result.emplace_back(definitionIndex);
    };

    const CBaseHandle activeWeapon = weaponServices->m_hActiveWeapon();

    if (activeWeapon.valid())
        appendWeapon(activeWeapon);

    auto& weapons = weaponServices->m_hMyWeapons();

    for (int i = 0; i < weapons.count(); ++i)
    {
        const CBaseHandle handle = weapons.element(i);

        if (handle.valid() && handle != activeWeapon)
            appendWeapon(handle);
    }

    std::lock_guard lock(g_ownedWeaponsMutex);
    g_ownedWeapons = std::move(result);
}

void SkinChanger::SetCharmLog(void (*log)(const char* line))
{
    g_charmLog.store(log, std::memory_order_release);
}

void SkinChanger::SetOwnItemCheck(bool (*isOwnItem)(std::uint64_t id))
{
    g_ownItemCheck.store(isOwnItem, std::memory_order_release);
}
