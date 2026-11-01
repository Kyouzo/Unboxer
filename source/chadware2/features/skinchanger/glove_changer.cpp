#include "glove_changer.h"
#include "../../../patterns.h"
#include "skinchanger.h"

#include "econ_item_attribute_manager.h"
#include "item_schema.h"

#include <algorithm>
#include <atomic>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <iterator>
#include <Windows.h>

#include "../../config/config.h"
#include "../../config/configmanager.h"
#include "../../interfaces/interfaces.h"
#include "../../utils/memory/patternscan/patternscan.h"
#include "../../utils/schema/schema.h"

#include "../../../cs2/entity/C_CSPlayerPawn/C_CSPlayerPawn.h"
#include "../../../cs2/entity/C_EconEntity/C_EconEntity.h"

// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
// Gloves. The mechanism is the one of the glove changer that is proven (on video) to keep its gloves through buying AND
// selling a defuse kit:
//   - every apply rewrites the whole glove item (definition, quality, paint attributes, ids, account) and raises
//     m_bNeedToReApplyGloves so the game rebuilds the glove model from it;
//   - applies run for 4 frames in a row, not once;
//   - the game raising m_bNeedToReApplyGloves by itself (it does when your loadout changes) counts as "the gloves were
//     reset" and triggers an apply — Chadware used to ignore it, so after selling a kit the game rebuilt the DEFAULT
//     gloves and nothing re-applied ours;
//   - a team switch takes the gloves off for 2 frames first.
// Selling a defuse kit leaves you with default first-person gloves although the glove item stays ours (log-proven: def /
// init / ids / flag unchanged) — the game redraws your hands from its own data. Re-applying doesn't help (the rebuild
// comes out default too, and flashed on every buy-menu close). What does (user-tested, Misc tab test round): your
// player model swapped for one frame (SkinChanger::RefreshPlayerModel). The game then raises m_bNeedToReApplyGloves
// itself and the apply burst below puts our gloves back. So a kit sold → that swap, 200 ms later. Buying one is fine.
// Every event is written to glove_log.txt next to the configs.
// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
namespace
{
    constexpr std::uint16_t DEFAULT_T_GLOVES = 5028;
    constexpr std::uint16_t DEFAULT_CT_GLOVES = 5029;

    constexpr std::uint8_t TEAM_T = 2;
    constexpr std::uint8_t TEAM_CT = 3;
    constexpr int QUALITY_UNUSUAL = 3;

    constexpr int APPLY_FRAMES = 4;       // the proven changer applies on 4 consecutive frames
    constexpr int CLEAR_FRAMES = 2;       // gloves off this long on a team switch
    constexpr std::uint64_t OWN_FLAG_FRAMES = 3;   // m_bNeedToReApplyGloves this soon after our own write is still ours

    using CreatePaintKitFn = CPaintKit * (__fastcall*)(void*);
    using SetBodyGroupFn = void(__fastcall*)(void*, const char*, unsigned int);

    std::atomic_bool g_forceUpdate{ true };

    C_CSPlayerPawn* g_lastPawn = nullptr;
    std::uint16_t g_lastDefinition = 0;
    int g_lastPaintKit = -1;
    int g_lastSeed = -1;
    float g_lastWear = -1.0f;
    float g_lastSpawnTime = -1.0f;
    std::uint8_t g_lastTeam = 0xFF;

    bool g_appliedCustomGloves = false;

    int g_updateFrames = 0;
    int g_clearFrames = 0;
    std::uint64_t g_frame = 0;            // Run() calls, for "was that flag ours?"
    std::uint64_t g_lastWriteFrame = 0;

    // defuse kit edge (logged)
    bool g_haveDefuserState = false;
    bool g_lastHasDefuser = false;

    // kit sold → player model swapped for a frame this much later (a spawn resets the kit tracking)
    constexpr ULONGLONG KIT_SOLD_REFRESH_DELAY_MS = 200;
    ULONGLONG g_kitRefreshAt = 0;
    float g_defuserSpawnTime = -1.0f;

    // Once a kit was sold, EVERY following round starts with default hands again (user report). So for that pawn
    // (= this map) the spawn follow-ups swap the player model instead of re-applying.
    const void* g_kitSoldPawn = nullptr;

    // Round start: the game sometimes puts its own gloves back a moment AFTER spawning. So after every spawn the
    // gloves are re-applied a few more times.
    constexpr ULONGLONG FOLLOW_UP_DELAYS_MS[] = { 400, 1200, 3000 };
    float g_followUpSpawnTime = -1.0f;
    ULONGLONG g_spawnSeenAt = 0;
    std::size_t g_followUpIndex = std::size(FOLLOW_UP_DELAYS_MS);

    CreatePaintKitFn GetCreatePaintKit()
    {
        const auto fn = M::ResolveCached([] { return reinterpret_cast<CreatePaintKitFn>(M::scan("client.dll", Patterns::GloveCreatePaintKit)); });
        return fn;
    }

    SetBodyGroupFn GetSetBodyGroup()
    {
        const auto fn = M::ResolveCached([] { return reinterpret_cast<SetBodyGroupFn>(M::scan_absolute("client.dll", Patterns::GloveSetBodyGroup, 0x1)); });
        return fn;
    }

    template <typename T>
    T* Field(void* base, std::uint32_t offset)
    {
        return base && offset ? reinterpret_cast<T*>(static_cast<std::uint8_t*>(base) + offset) : nullptr;
    }

    std::uint16_t GetDefaultGloveDefinition(std::uint8_t team)
    {
        if (team == TEAM_T)
            return DEFAULT_T_GLOVES;

        if (team == TEAM_CT)
            return DEFAULT_CT_GLOVES;

        return 0;
    }

    bool IsDefaultGloveDefinition(std::uint16_t definitionIndex)
    {
        return definitionIndex == DEFAULT_T_GLOVES || definitionIndex == DEFAULT_CT_GLOVES;
    }

    bool HasDefuser(C_CSPlayerPawn* pawn)
    {
        void** services = Field<void*>(pawn, SchemaFinder::GetCached<hash_32_fnv1a_const("C_BasePlayerPawn->m_pItemServices")>());
        if (!services || !*services)
            return false;

        const bool* defuser = Field<bool>(*services, SchemaFinder::GetCached<hash_32_fnv1a_const("CCSPlayer_ItemServices->m_bHasDefuser")>());
        return defuser && *defuser;
    }

    bool BuyMenuOpen(C_CSPlayerPawn* pawn)
    {
        const bool* open = Field<bool>(pawn, SchemaFinder::GetCached<hash_32_fnv1a_const("C_CSPlayerPawn->m_bIsBuyMenuOpen")>());
        return open && *open;
    }

    std::uint32_t LocalAccountId(C_CSPlayerPawn* pawn)
    {
        if (!I::GameEntity || !I::GameEntity->Instance)
            return 0;

        void* controller = I::GameEntity->Instance->Get<C_BaseEntity>(pawn->m_hController());
        const std::uint64_t* steamId = Field<std::uint64_t>(controller, SchemaFinder::GetCached<hash_32_fnv1a_const("CBasePlayerController->m_steamID")>());
        return steamId ? static_cast<std::uint32_t>(*steamId & 0xFFFFFFFFull) : 0;
    }

    // ── glove_log.txt: one line per event, so a failure online can be read back ──
    void Log(const char* format, ...)
    {
        char line[512];
        va_list args;
        va_start(args, format);
        vsnprintf(line, sizeof(line), format, args);
        va_end(args);

        const std::filesystem::path path = internal_config::ConfigManager::GetConfigDirectory().parent_path() / "glove_log.txt";

        std::error_code ec;
        const bool tooBig = std::filesystem::exists(path, ec) && std::filesystem::file_size(path, ec) > 256 * 1024;

        FILE* file = nullptr;
        if (_wfopen_s(&file, path.c_str(), tooBig ? L"w" : L"a") != 0 || !file)
            return;

        SYSTEMTIME time{};
        GetLocalTime(&time);
        fprintf(file, "%02d:%02d:%02d.%03d  %s\n", time.wHour, time.wMinute, time.wSecond, time.wMilliseconds, line);
        fclose(file);
    }

    void LogState(const char* what, C_CSPlayerPawn* pawn, C_EconItemView* item)
    {
        const std::uint32_t* low = Field<std::uint32_t>(item, SchemaFinder::GetCached<hash_32_fnv1a_const("C_EconItemView->m_iItemIDLow")>());
        Log("%-22s def=%u init=%d needReapply=%d idHigh=%08X idLow=%08X defuser=%d buyMenu=%d",
            what, item ? item->m_iItemDefinitionIndex() : 0, item ? item->m_bInitialized() : 0, pawn->m_bNeedToReApplyGloves(),
            item ? item->m_iItemIDHigh() : 0, low ? *low : 0, HasDefuser(pawn), BuyMenuOpen(pawn));
    }

    // Gloves off (team switch): no definition, not initialised, and the game told to rebuild.
    void ClearGloves(C_CSPlayerPawn* pawn, C_EconItemView* item)
    {
        EconItemAttributeManager::Remove(item);
        item->m_iItemDefinitionIndex() = 0;
        item->m_bInitialized() = false;
        pawn->m_bNeedToReApplyGloves() = true;
    }

    bool PaintKitNameIs(CPaintKit* paintKit, const char* name)
    {
        __try
        {
            const char* current = paintKit->m_Name;
            return current && std::strcmp(current, name) == 0;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    bool ApplyGloves(C_CSPlayerPawn* pawn, C_EconItemView* item, std::uint16_t definitionIndex, int paintKit, int seed, float wear)
    {
        if (!pawn || !item || definitionIndex == 0)
            return false;

        const SetBodyGroupFn setBodyGroup = GetSetBodyGroup();

        if (!setBodyGroup)
            return false;

        CreatePaintKitFn createPaintKit = nullptr;

        if (paintKit > 0)
        {
            createPaintKit = GetCreatePaintKit();

            if (!createPaintKit)
                return false;
        }

        item->m_iItemDefinitionIndex() = definitionIndex;
        item->m_iEntityQuality() = QUALITY_UNUSUAL;

        EconItemAttributeManager::Remove(item);

        if (paintKit > 0)
        {
            EconItemAttributeManager::Create(item, paintKit, wear, seed, -1, 0);

            CPaintKit* generatedPaintKit = nullptr;

            __try
            {
                generatedPaintKit = createPaintKit(item);
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                item->m_bInitialized() = true;
                return false;
            }

            // This can be the game's OWN paint kit (the one every item with this finish uses). Never point its name at
            // one of our strings: that string is freed when Chadware unloads, and the finish then vanished from the
            // lists on the next load. Only a missing / wrong name is replaced, by a copy that is never freed.
            if (generatedPaintKit)
            {
                if (const PaintKitInfo* paintKitInfo = Skins::g_ItemSchema.GetPaintKit(paintKit))
                {
                    if (!PaintKitNameIs(generatedPaintKit, paintKitInfo->name.c_str()))
                    {
                        if (const char* name = Skins::PermanentName(paintKitInfo->name))
                            generatedPaintKit->m_Name = name;
                    }
                }
            }
        }

        // Keep the real item id (low / full) — only the high part marks it as ours. The account id is yours.
        item->m_iItemIDHigh() = IsDefaultGloveDefinition(definitionIndex) ? 0u : 0xFFFFFFFFu;
        if (std::uint32_t* account = Field<std::uint32_t>(item, SchemaFinder::GetCached<hash_32_fnv1a_const("C_EconItemView->m_iAccountID")>()))
        {
            if (const std::uint32_t id = LocalAccountId(pawn))
                *account = id;
        }

        item->m_bInitialized() = true;
        item->m_bDisallowSOC() = false;
        item->m_bRestoreCustomMaterialAfterPrecache() = true;

        __try
        {
            setBodyGroup(pawn, "first_or_third_person", 1);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }

        pawn->m_bNeedToReApplyGloves() = true;
        return true;
    }

    void ClearCachedState()
    {
        g_lastPawn = nullptr;
        g_lastDefinition = 0;
        g_lastPaintKit = -1;
        g_lastSeed = -1;
        g_lastWear = -1.0f;
        g_lastSpawnTime = -1.0f;
        g_lastTeam = 0xFF;
    }

    void ConstructPaintKit(C_EconItemView* item)
    {
        const CreatePaintKitFn createPaintKit = GetCreatePaintKit();

        if (!createPaintKit)
            return;

        __try
        {
            createPaintKit(item);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
        }
    }
}

void GloveChanger::Run(C_CSPlayerPawn* localPawn)
{
    ++g_frame;

    if (!localPawn || localPawn->m_iHealth() <= 0)
        return;

    // Keep glove/model material regeneration consistent with weapons: continuous
    // menu drags commit once on release instead of rebuilding every frame.
    if (SkinChanger::IsInteractiveEditActive())
        return;

    C_EconItemView* item = localPawn->m_EconGloves();

    if (!item)
        return;

    const std::uint8_t team = localPawn->m_iTeamNum();
    const float spawnTime = localPawn->m_flLastSpawnTimeIndex();
    const ULONGLONG now = GetTickCount64();

    const Config::skin_changer_t& loadout = Config::GameLive();
    std::uint16_t definitionIndex = loadout.gloveDefinition;

    int paintKit = 0;
    int seed = 0;
    float wear = 0.0001f;

    bool customSelection = Config::Game().enabled && definitionIndex != 0;

    if (customSelection)
    {
        const ItemDefinitionInfo* itemInfo = Skins::g_ItemSchema.GetItem(definitionIndex);

        if (!itemInfo || !itemInfo->isGlove)
            customSelection = false;
    }

    if (customSelection)
    {
        const auto configIt = loadout.gloves.find(definitionIndex);

        if (configIt != loadout.gloves.end())
        {
            const Config::item_skin_t& config = configIt->second;

            if (config.enabled && config.paintKit > 0)
            {
                paintKit = config.paintKit;
                seed = (std::clamp)(config.seed, 0, 1000);
                wear = (std::clamp)(config.wear, 0.0001f, 1.0f);
            }
        }
    }
    else
    {
        if (!g_appliedCustomGloves)
            return;

        definitionIndex = GetDefaultGloveDefinition(team);

        if (definitionIndex == 0)
            return;
    }

    // The proven changer constructs the item's paint kit every frame, not only when it applies.
    if (customSelection && paintKit > 0 && item->m_iItemDefinitionIndex() == definitionIndex)
        ConstructPaintKit(item);

    // ── what happened since last frame ──
    const bool configChanged =
        definitionIndex != g_lastDefinition || paintKit != g_lastPaintKit || seed != g_lastSeed || wear != g_lastWear;
    const bool teamChanged = g_lastTeam != 0xFF && team != g_lastTeam;
    const bool spawnChanged = spawnTime != g_lastSpawnTime || localPawn != g_lastPawn;

    // The game raising m_bNeedToReApplyGloves by itself = it's about to rebuild the gloves from its own data.
    const bool flagRaisedByGame = localPawn->m_bNeedToReApplyGloves() && g_frame - g_lastWriteFrame > OWN_FLAG_FRAMES;
    const bool idLost = !IsDefaultGloveDefinition(definitionIndex) && item->m_iItemIDHigh() != 0xFFFFFFFFu;
    const bool engineReset = customSelection &&
        (item->m_iItemDefinitionIndex() != definitionIndex || !item->m_bInitialized() || idLost || flagRaisedByGame);

    // Defuse kit bought / sold. A new spawn starts over (dying with a kit isn't selling it).
    if (spawnTime != g_defuserSpawnTime)
    {
        g_defuserSpawnTime = spawnTime;
        g_haveDefuserState = false;
        g_kitRefreshAt = 0;
    }

    const bool hasDefuser = HasDefuser(localPawn);
    const bool kitChanged = g_haveDefuserState && hasDefuser != g_lastHasDefuser;
    g_lastHasDefuser = hasDefuser;
    g_haveDefuserState = true;

    // spawn follow-ups
    bool followUp = false;

    if (spawnTime != g_followUpSpawnTime)
    {
        g_followUpSpawnTime = spawnTime;
        g_spawnSeenAt = now;
        g_followUpIndex = 0;
    }

    if (customSelection && g_followUpIndex < std::size(FOLLOW_UP_DELAYS_MS) && now - g_spawnSeenAt >= FOLLOW_UP_DELAYS_MS[g_followUpIndex])
    {
        followUp = true;
        ++g_followUpIndex;
    }

    const bool forced = g_forceUpdate.exchange(false, std::memory_order_acq_rel);

    // ── react ──
    if (customSelection && kitChanged)
    {
        LogState(hasDefuser ? "kit bought" : "kit sold", localPawn, item);

        if (!hasDefuser)
        {
            g_kitRefreshAt = now + KIT_SOLD_REFRESH_DELAY_MS;
            g_kitSoldPawn = localPawn;
        }
    }

    if (followUp && g_kitSoldPawn == localPawn)
    {
        followUp = false;
        LogState("round start: model swap", localPawn, item);
        SkinChanger::RefreshPlayerModel();
    }

    if (g_kitRefreshAt != 0 && now >= g_kitRefreshAt)
    {
        g_kitRefreshAt = 0;

        if (customSelection)
        {
            LogState("kit sold: model swap", localPawn, item);
            SkinChanger::RefreshPlayerModel();
        }
    }

    if (customSelection && engineReset && g_updateFrames <= 0 && g_clearFrames <= 0)
        LogState(flagRaisedByGame ? "game raised reapply" : "gloves reset by game", localPawn, item);

    if (teamChanged)
        g_clearFrames = CLEAR_FRAMES;

    if (configChanged || teamChanged || spawnChanged || engineReset || followUp || forced)
        g_updateFrames = APPLY_FRAMES;

    if (g_clearFrames > 0)
    {
        ClearGloves(localPawn, item);
        g_lastWriteFrame = g_frame;
        --g_clearFrames;

        g_lastDefinition = 0;
        g_lastPaintKit = -1;
        g_lastTeam = team;
        return;
    }

    if (g_updateFrames <= 0)
        return;

    if (!ApplyGloves(localPawn, item, definitionIndex, paintKit, seed, wear))
    {
        g_updateFrames = 0;
        Log("apply FAILED (def=%u paint=%d)", definitionIndex, paintKit);
        return;
    }

    g_lastWriteFrame = g_frame;
    --g_updateFrames;

    g_lastPawn = localPawn;
    g_lastDefinition = definitionIndex;
    g_lastPaintKit = paintKit;
    g_lastSeed = seed;
    g_lastWear = wear;
    g_lastSpawnTime = spawnTime;
    g_lastTeam = team;

    g_appliedCustomGloves = customSelection;
}

void GloveChanger::ForceUpdate()
{
    g_forceUpdate.store(true, std::memory_order_release);
}

void GloveChanger::Reset()
{
    g_forceUpdate.store(true, std::memory_order_release);
    ClearCachedState();
    g_appliedCustomGloves = false;
    g_updateFrames = 0;
    g_clearFrames = 0;
    g_kitRefreshAt = 0;
    g_defuserSpawnTime = -1.0f;
    g_kitSoldPawn = nullptr;
    g_haveDefuserState = false;
}

void GloveChanger::LogEvent(C_CSPlayerPawn* pawn, const char* what)
{
    if (C_EconItemView* item = pawn ? pawn->m_EconGloves() : nullptr)
        LogState(what, pawn, item);
    else
        Log("%s", what);
}
