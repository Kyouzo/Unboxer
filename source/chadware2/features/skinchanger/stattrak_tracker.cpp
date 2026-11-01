#include "stattrak_tracker.h"

#include <algorithm>
#include <atomic>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <unordered_map>

#include "item_schema.h"

#include "../../config/config.h"
#include "../../config/configmanager.h"
#include "../../interfaces/interfaces.h"
#include "../../utils/schema/schema.h"

#include <cs2/entity/C_CSPlayerPawn/C_CSPlayerPawn.h>

namespace
{
    using json = nlohmann::json;

    std::mutex g_mutex;
    std::unordered_map<std::uint32_t, int> g_counts;   // (definition index << 16 | paint kit) → kills
    std::atomic_bool g_dirty{ false };

    // Match kills: CCSPlayerController → m_pActionTrackingServices → m_matchStats (CSMatchStats_t : CSPerRoundStats_t) → m_iKills.
    std::uint32_t TrackingOffset() { return SchemaFinder::GetCached<hash_32_fnv1a_const("CCSPlayerController->m_pActionTrackingServices")>(); }
    std::uint32_t MatchStatsOffset() { return SchemaFinder::GetCached<hash_32_fnv1a_const("CCSPlayerController_ActionTrackingServices->m_matchStats")>(); }
    std::uint32_t KillsOffset() { return SchemaFinder::GetCached<hash_32_fnv1a_const("CSPerRoundStats_t->m_iKills")>(); }

    std::uint32_t Key(std::uint16_t definitionIndex, int paintKit)
    {
        return (static_cast<std::uint32_t>(definitionIndex) << 16) | static_cast<std::uint32_t>(paintKit & 0xFFFF);
    }

    std::filesystem::path FilePath()
    {
        return internal_config::ConfigManager::GetConfigDirectory().parent_path() / "stattrak.json";
    }

    bool ReadKills(void* controller, int& kills)
    {
        const std::uint32_t trackingOffset = TrackingOffset();
        const std::uint32_t statsOffset = MatchStatsOffset();
        const std::uint32_t killsOffset = KillsOffset();

        if (!controller || !trackingOffset || !statsOffset || !killsOffset)
            return false;

        void* tracking = *reinterpret_cast<void**>(static_cast<std::uint8_t*>(controller) + trackingOffset);

        if (!tracking)
            return false;

        kills = *reinterpret_cast<int*>(static_cast<std::uint8_t*>(tracking) + statsOffset + killsOffset);
        return kills >= 0 && kills < 100000;
    }

    // The paint kit the skin changer currently gives this weapon on your live side (0 = none → not StatTrak'd).
    const Config::item_skin_t* LiveSkinFor(std::uint16_t definitionIndex)
    {
        const Config::skin_changer_t& loadout = Config::GameLive();
        const bool knife = definitionIndex >= 500 && definitionIndex < 600;
        const auto& map = knife ? loadout.knives : loadout.weapons;
        const auto it = map.find(definitionIndex);
        return it != map.end() ? &it->second : nullptr;
    }
}

void StatTrakTracker::Poll(C_CSPlayerPawn* localPawn)
{
    static void* lastController = nullptr;
    static int lastKills = -1;

    // a demo (your own or a match's) counts nothing: those kills were counted when they happened. IEngineClient vfunc 42
    // = IsPlayingDemo (panorama's IsPlayingDemo native jumps to it, client 2026-10-14).
    if (I::EngineClient && M::CallVFunc<bool, 42U>(I::EngineClient))
    {
        lastController = nullptr;
        lastKills = -1;
        return;
    }

    if (!localPawn || !I::GameEntity || !I::GameEntity->Instance)
        return;

    void* controller = I::GameEntity->Instance->Get<C_BaseEntity>(localPawn->m_hController());
    int kills = 0;

    if (!controller || !ReadKills(controller, kills))
        return;

    // New match / reconnect / warmup reset / team-kill penalty: just take the new number as the baseline.
    if (controller != lastController || kills < lastKills || lastKills < 0)
    {
        lastController = controller;
        lastKills = kills;
        return;
    }

    if (kills == lastKills)
        return;

    const int newKills = kills - lastKills;
    lastKills = kills;

    if (!Config::Game().enabled || localPawn->m_iHealth() <= 0)
        return;   // dead (a molotov kill after you died, etc.) — nothing in your hands to credit

    CCSPlayer_WeaponServices* weaponServices = localPawn->m_pWeaponServices();

    if (!weaponServices)
        return;

    const CBaseHandle activeHandle = weaponServices->m_hActiveWeapon();
    C_CSWeaponBase* weapon = activeHandle.valid() ? I::GameEntity->Instance->Get<C_CSWeaponBase>(activeHandle.index()) : nullptr;
    C_AttributeContainer* attributes = weapon ? weapon->m_AttributeManager() : nullptr;
    C_EconItemView* item = attributes ? attributes->m_Item() : nullptr;

    if (!item)
        return;

    const std::uint16_t definitionIndex = item->m_iItemDefinitionIndex();
    const ItemDefinitionInfo* info = Skins::g_ItemSchema.GetItem(definitionIndex);

    if (!info || (!info->isWeapon && !info->isKnife))
        return;   // grenade / C4 in hand

    const Config::item_skin_t* skin = LiveSkinFor(definitionIndex);

    if (!skin || !skin->enabled || !skin->statTrakEnabled || !skin->statTrakReal || skin->paintKit <= 0)
        return;

    {
        std::lock_guard lock(g_mutex);
        g_counts[Key(definitionIndex, skin->paintKit)] += newKills;
    }

    g_dirty.store(true, std::memory_order_release);
}

int StatTrakTracker::Get(std::uint16_t definitionIndex, int paintKit)
{
    std::lock_guard lock(g_mutex);
    const auto it = g_counts.find(Key(definitionIndex, paintKit));
    return it != g_counts.end() ? it->second : 0;
}

void StatTrakTracker::Set(std::uint16_t definitionIndex, int paintKit, int kills)
{
    {
        std::lock_guard lock(g_mutex);
        g_counts[Key(definitionIndex, paintKit)] = (std::max)(0, kills);
    }

    g_dirty.store(true, std::memory_order_release);
}

int StatTrakTracker::Displayed(std::uint16_t definitionIndex, const Config::item_skin_t& skin)
{
    return skin.statTrakReal ? Get(definitionIndex, skin.paintKit) : (std::max)(0, skin.statTrakKills);
}

void StatTrakTracker::Load()
{
    std::ifstream file(FilePath());

    if (!file)
        return;

    json root;

    try
    {
        file >> root;
    }
    catch (...)
    {
        root = nullptr;
    }

    if (!root.is_object())
    {
        // unreadable: the first kill would write over it with only the new counts - a copy of it first
        file.close();
        std::error_code ec;
        std::filesystem::copy_file(FilePath(), FilePath().parent_path() / ("stattrak.unreadable-" + std::to_string(static_cast<long long>(std::time(nullptr))) + ".json"),
            std::filesystem::copy_options::overwrite_existing, ec);
        return;
    }

    std::lock_guard lock(g_mutex);
    g_counts.clear();

    // { "7:44": 123, ... }  →  definition index : paint kit
    for (const auto& [key, value] : root.items())
    {
        const std::size_t colon = key.find(':');

        if (colon == std::string::npos || !value.is_number_integer())
            continue;

        const int definitionIndex = std::atoi(key.substr(0, colon).c_str());
        const int paintKit = std::atoi(key.substr(colon + 1).c_str());

        if (definitionIndex > 0 && definitionIndex < 0x10000 && paintKit > 0)
            g_counts[Key(static_cast<std::uint16_t>(definitionIndex), paintKit)] = (std::max)(0, value.get<int>());
    }
}

void StatTrakTracker::SaveIfDirty()
{
    if (!g_dirty.exchange(false, std::memory_order_acq_rel))
        return;

    json root = json::object();

    {
        std::lock_guard lock(g_mutex);

        for (const auto& [key, kills] : g_counts)
            root[std::to_string(key >> 16) + ":" + std::to_string(key & 0xFFFF)] = kills;
    }

    // via a .tmp renamed over the old one (like items.json): a crash mid-write can't leave a cut-off file. A failed write
    // isn't retried every frame - the next kill writes every count again.
    const std::filesystem::path path = FilePath();
    const std::filesystem::path temp = path.wstring() + L".tmp";

    {
        std::ofstream file(temp, std::ios::trunc);

        if (!file || !(file << root.dump(2)))
            return;
    }

    std::error_code ec;
    std::filesystem::rename(temp, path, ec);
}
