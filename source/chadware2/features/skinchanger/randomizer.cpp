#include "randomizer.h"

#include <array>
#include <atomic>
#include <chrono>
#include <random>
#include <vector>

#include "glove_changer.h"
#include "item_schema.h"
#include "music_kit_changer.h"
#include "skinchanger.h"

#include "../../config/config.h"
#include "../../menu/localization.h"
#include "../../menu/notify.h"
#include "../../globals/globals.h"

#include "../../../cs2/entity/C_CSPlayerPawn/C_CSPlayerPawn.h"

// ─────────────────────────────────────────────────────────────────────────────────────────────────────────
// Randomizer — same rules as Nerv's:
//   guns     every gun gets a random compatible skin, wear 0 - 0.6, random seed
//   stickers slots wiped, then ~1 in 3 guns gets 1-3 random stickers
//   charms   ~1 in 3 guns gets a random charm (off by default)
//   knife    random knife model + random skin
//   gloves   random glove model + random skin
//   agent    random agent for the team you're on (off by default)
//   music    random music kit (off by default)
// StatTrak and name tags are left as you set them. Turns the skin changer on so the result is visible.
// ─────────────────────────────────────────────────────────────────────────────────────────────────────────

namespace
{
    std::atomic_bool g_requested{ false };

    std::mt19937& Rng()
    {
        static std::mt19937 rng(static_cast<std::uint32_t>(
            std::random_device{}() ^ std::chrono::steady_clock::now().time_since_epoch().count()));
        return rng;
    }

    int RandomInt(int minValue, int maxValue)
    {
        return std::uniform_int_distribution<int>(minValue, maxValue)(Rng());
    }

    float RandomFloat(float minValue, float maxValue)
    {
        return std::uniform_real_distribution<float>(minValue, maxValue)(Rng());
    }

    bool Chance(int percent)
    {
        return RandomInt(0, 99) < percent;
    }

    template <typename T>
    const T* Pick(const std::vector<T>& values)
    {
        return values.empty() ? nullptr : &values[static_cast<std::size_t>(RandomInt(0, static_cast<int>(values.size()) - 1))];
    }

    // A random real skin for the item (the list includes a vanilla id-0 entry, which is skipped).
    const PaintKitInfo* RandomPaintKit(std::uint16_t definitionIndex)
    {
        const std::vector<PaintKitInfo>* kits = Skins::g_ItemSchema.GetPaintKitsForItem(definitionIndex);

        if (!kits)
            return nullptr;

        std::vector<const PaintKitInfo*> real;
        real.reserve(kits->size());

        for (const PaintKitInfo& kit : *kits)
        {
            if (kit.id > 0)
                real.push_back(&kit);
        }

        return real.empty() ? nullptr : *Pick(real);
    }

    void RollSkin(Config::item_skin_t& skin, const PaintKitInfo& paintKit)
    {
        skin.enabled = true;
        skin.paintKit = paintKit.id;
        skin.wear = RandomFloat(0.0001f, 0.6f);
        skin.seed = RandomInt(0, 1000);
    }

    void RollStickers(Config::item_skin_t& skin)
    {
        for (auto& sticker : skin.stickers)
            sticker = Config::skin_sticker_t{};

        const auto& kits = Skins::g_ItemSchema.GetStickerKits();

        if (kits.empty() || !Chance(33))
            return;

        const int count = RandomInt(1, 3);

        for (int slot = 0; slot < count && slot < static_cast<int>(skin.stickers.size()); ++slot)
        {
            auto& sticker = skin.stickers[slot];
            sticker.enabled = true;
            sticker.kit = Pick(kits)->id;
            sticker.wear = RandomFloat(0.0f, 0.5f);
            sticker.scale = 1.0f;
        }
    }

    void RollCharm(Config::item_skin_t& skin)
    {
        skin.keychain = Config::skin_keychain_t{};

        const auto& charms = Skins::g_ItemSchema.GetKeychains();

        if (charms.empty() || !Chance(33))
            return;

        skin.keychain.enabled = true;
        skin.keychain.id = Pick(charms)->id;
        skin.keychain.seed = RandomInt(0, 100000);
    }

    // Knife models the knife changer can apply (it needs a known subclass id for each).
    bool IsSupportedKnife(std::uint16_t definitionIndex)
    {
        static constexpr std::array<std::uint16_t, 20> supported = {
            500, 503, 505, 506, 507, 508, 509, 512, 514, 515, 516, 517, 518, 519, 520, 521, 522, 523, 525, 526
        };

        for (const std::uint16_t knife : supported)
        {
            if (knife == definitionIndex)
                return true;
        }

        return false;
    }

    void RandomizeGuns(Config::skin_changer_t& loadout)
    {
        const Config::menu_t& options = Config::menu;

        for (const ItemDefinitionInfo& item : Skins::g_ItemSchema.GetItems())
        {
            if (!item.isWeapon || item.isKnife || item.isGlove)
                continue;

            const PaintKitInfo* paintKit = RandomPaintKit(item.definitionIndex);

            if (!paintKit)
                continue; // grenades, C4, etc.

            Config::item_skin_t& skin = loadout.weapons[item.definitionIndex];
            RollSkin(skin, *paintKit);

            if (options.randomStickers)
                RollStickers(skin);

            if (options.randomCharms)
                RollCharm(skin);
        }
    }

    void RandomizeKnife(Config::skin_changer_t& loadout)
    {
        std::vector<const ItemDefinitionInfo*> knives;

        for (const ItemDefinitionInfo& item : Skins::g_ItemSchema.GetItems())
        {
            if (item.isKnife && IsSupportedKnife(item.definitionIndex))
                knives.push_back(&item);
        }

        if (knives.empty())
            return;

        const ItemDefinitionInfo* knife = *Pick(knives);
        const PaintKitInfo* paintKit = RandomPaintKit(knife->definitionIndex);

        loadout.knifeDefinition = knife->definitionIndex;

        if (paintKit)
            RollSkin(loadout.knives[knife->definitionIndex], *paintKit);
    }

    void RandomizeGloves(Config::skin_changer_t& loadout)
    {
        std::vector<std::pair<const ItemDefinitionInfo*, const PaintKitInfo*>> gloves;

        for (const ItemDefinitionInfo& item : Skins::g_ItemSchema.GetItems())
        {
            if (!item.isGlove || item.definitionIndex == 5028 || item.definitionIndex == 5029)
                continue; // 5028 / 5029 = default T / CT gloves, they have no finishes

            if (const PaintKitInfo* paintKit = RandomPaintKit(item.definitionIndex))
                gloves.emplace_back(&item, paintKit);
        }

        if (gloves.empty())
            return;

        const auto& [glove, paintKit] = *Pick(gloves);

        loadout.gloveDefinition = glove->definitionIndex;
        RollSkin(loadout.gloves[glove->definitionIndex], *paintKit);
    }

    void RandomizeAgent(int side)
    {
        const Team team = side == Config::TEAM_CT ? Team::CT : Team::T;

        std::vector<const ItemDefinitionInfo*> agents;

        for (const ItemDefinitionInfo& item : Skins::g_ItemSchema.GetItems())
        {
            if (!item.isAgent || !item.hasUsableAgentModel)
                continue;

            if (team == Team::T && !item.isTerroristAgent)
                continue;

            if (team == Team::CT && !item.isCounterTerroristAgent)
                continue;

            agents.push_back(&item);
        }

        if (!agents.empty())
            Config::AgentFor(side) = (*Pick(agents))->definitionIndex;
    }

    void RandomizeMusicKit()
    {
        if (const MusicKitInfo* kit = Pick(Skins::g_ItemSchema.GetMusicKits()))
            Config::skins.musicKit = kit->id;
    }
}

void Randomizer::RandomizeNow(int team)
{
    if (!Skins::g_ItemSchema.IsInitialized())
        return;

    const Config::menu_t& options = Config::menu;
    Config::skin_changer_t& loadout = Config::Loadout(team);

    if (options.randomGuns)
        RandomizeGuns(loadout);

    if (options.randomKnife)
        RandomizeKnife(loadout);

    if (options.randomGloves)
        RandomizeGloves(loadout);

    if (options.randomAgent)
        RandomizeAgent(team);

    if (options.randomMusicKit)
        RandomizeMusicKit();

    Config::skins.enabled = true;

    const char* side = team == Config::TEAM_CT ? " (CT)" : " (T)";
    Notify::Push(std::string(tr("Randomized your loadout")) + side);

    SkinChanger::ForceUpdate();
    SkinChanger::ForceAgentUpdate();
    GloveChanger::ForceUpdate();
    MusicKitChanger::ForceUpdate();
}

void Randomizer::RequestRandomize()
{
    g_requested.store(true, std::memory_order_release);
}

bool Randomizer::ConsumeRequest()
{
    return g_requested.exchange(false, std::memory_order_acq_rel);
}
