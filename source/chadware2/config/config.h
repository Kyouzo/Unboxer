#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace Config
{
    struct ui_t
    {
        std::string themePreset = "__DEFAULT__";
    };

    inline ui_t ui{};

    // A skin you picked recently (Guns page → "Recent"). Only the last six are kept.
    struct recent_skin_t
    {
        std::uint16_t definitionIndex = 0;
        int paintKit = 0;
    };

    // Menu / interface settings. Global (not per skin-config): auto-loaded on inject and auto-saved when changed,
    // to %USERPROFILE%\.standalone_skinchanger\menu.json (see ConfigManager::LoadMenuSettings / SaveMenuSettings).
    struct menu_t
    {
        int   language = 0;          // 0 = English, 1 = Russian
        int   scaleMode = 0;         // 0 = Auto (from the game resolution), 1 = Manual
        float uiScale = 1.0f;        // multiplier on top of Auto (or the whole scale in Manual)
        float fontScale = 1.0f;      // text size on top of the UI scale
        float width = 1240.0f;       // menu size before UI scale
        float height = 800.0f;
        bool  animations = true;     // fade in when the menu opens
        float animSpeed = 0.20f;     // fade duration in seconds
        int   menuKey = 0x24;        // VK_HOME (Chadware uses Insert)
        int   stretchMode = 0;       // 0 = Normal look, 1 = Crisp (match game stretch), 2 = Off (original)
        bool  autoLoadConfig = true; // load the last used config when you inject
        bool  notifications = true;  // small pop-ups at the side of the screen ("AK-47 | Redline applied")
        int   skinSort = 1;          // skin grids: 0 = A-Z, 1 = rarity (best first), 2 = popular (the most wanted first)

        // Randomizer (Skins tab): what "Randomize" rolls, and whether it re-rolls on every respawn.
        bool  randomGuns = true;
        bool  randomStickers = true;
        bool  randomCharms = false;
        bool  randomKnife = true;
        bool  randomGloves = true;
        bool  randomAgent = false;
        bool  randomMusicKit = false;
        bool  randomEveryRound = false;

        // Viewmodel (how your gun sits on screen). Written into the game's viewmodel settings after its own
        // 60-68 FOV clamp, so any FOV works.
        bool  viewmodelEnabled = false;
        float viewmodelFov = 68.0f;
        float viewmodelX = 2.5f;
        float viewmodelY = 0.0f;
        float viewmodelZ = -1.5f;

        // Misc tab: bunnyhop, camera FOV changer, speed meter (see features/movement).
        bool  bhop = false;          // toggled with bhopKey too
        bool  fovChanger = false;    // toggled with fovKey too
        int   bhopKey = 0x75;        // VK_F6 (0 = no key)
        int   fovKey = 0x78;         // VK_F9 (0 = no key)
        int   reapplyKey = 0;        // "Reapply skins" in a match (0 = no key)
        float fov = 110.0f;          // camera FOV, 60-150 (the game's default is 90)
        bool  speedHud = false;      // speed number under the crosshair (Misc tab)
        bool  sniperCrosshair = false;   // your own crosshair on snipers while not scoped (Misc tab)

        // Lootline: "<you> has opened a container and found: <item>" in your own chat for each case you open.
        bool  chatDrops = true;

        bool  showTips = false;      // the grey help lines under the controls (Settings -> Menu)
        bool  reapplyOnSpawn = true; // every spawn: all skins put on again, gloves too (Settings -> In a match)
        std::vector<float> reapplyTimes{ 0.6f };   // ... when: seconds after the spawn, one reapply each (at most 8)
        bool  shuffleEveryRound = false;   // CS2's loadout shuffle: a new pick every round, not once per match (Settings -> In a match)
        bool  localSpray = true;     // a sprayed Lootline graffiti shows on your screen (Settings -> In a match)
        bool  scoreboardPin = true;  // your equipped Lootline pin next to your name on the scoreboard (Settings -> In a match)
        bool  armoryLootline = true; // CS2's Armory with Lootline's stars: redeems are local (Settings -> Armory)
        int   accentColor = 0xECB43C;   // the menu's colour, 0xRRGGBB (Settings -> Menu; gold = Lootline's own)
        bool  accentRgb = false;     // ... cycling through the colours instead
        int   currency = 0;          // prices shown in (Prices::Currency*; 0 = US dollar)
        bool  cs2Prices = true;      // a Lootline item's price in CS2's inspect popup (under its name)
        bool  cs2PriceTotal = true;  // what all Lootline items are worth, at the top of CS2's inventory
        bool  buyKeysLootline = false;   // CS2's "buy a key" gives Lootline keys, never Steam (Settings -> Prices)

        // The item cards' size on each tab (CARD_TABS: Cases, Add items, Inventory, Upgrader): the pictures and the text
        // on the cards, 1 = the usual. The sliders above each tab's cards.
        std::array<float, 4> cardPicture{ 1.0f, 1.0f, 1.0f, 1.0f };
        std::array<float, 4> cardText{ 1.0f, 1.0f, 1.0f, 1.0f };

        std::vector<recent_skin_t> recentSkins;   // newest first, at most 6
    };

    inline menu_t menu{};

    struct skin_sticker_t
    {
        bool enabled = false;
        int kit = 0;
        float wear = 0.0f;
        float scale = 1.0f;
        float rotation = 0.0f;
        float offsetX = 0.0f;
        float offsetY = 0.0f;
        int schema = -1;             // the gun's sticker position (Lootline: CS2's sticker popup); -1 = the usual one. Not saved.
    };

    struct skin_keychain_t
    {
        bool enabled = false;
        int id = 0;
        int seed = 0;
        float offsetX = 0.0f;
        float offsetY = 0.0f;
        float offsetZ = 0.0f;
        int sticker = 0;   // a Sticker Slab's sealed sticker ("keychain slot 0 sticker"), 0 = none
    };

    struct item_skin_t
    {
        bool enabled = false;
        int paintKit = 0;
        int seed = 0;
        float wear = 0.0001f;
        bool statTrakEnabled = false;
        bool statTrakReal = true;    // count your real kills (kept in stattrak.json, not in the config)
        int statTrakKills = 0;       // the number shown when statTrakReal is off
        bool souvenir = false;       // a gun named "Souvenir ..." (a Lootline souvenir package drop); never with StatTrak
        std::string customName;
        std::array<skin_sticker_t, 6> stickers{};   // sticker slots 0-5
        skin_keychain_t keychain{};
    };

    // One side's loadout.
    struct skin_changer_t
    {
        std::unordered_map<std::uint16_t, item_skin_t> weapons;
        std::uint16_t knifeDefinition = 0;
        std::unordered_map<std::uint16_t, item_skin_t> knives;
        std::uint16_t gloveDefinition = 0;
        std::unordered_map<std::uint16_t, item_skin_t> gloves;
        std::uint16_t agentDefinition = 0;   // agents are ALWAYS per side (see AgentFor)
    };

    enum : int
    {
        TEAM_T = 0,
        TEAM_CT = 1
    };

    // Everything the skin changer applies (saved in the skin configs). T and CT each have their own loadout; the side
    // you're playing is the one applied. "Copy to CT / T" in the menu fills the other side in one click.
    struct skins_t
    {
        bool enabled = false;
        std::array<skin_changer_t, 2> loadouts{};   // [TEAM_T], [TEAM_CT]
        int musicKit = 0;                           // music_definitions id; 0 = keep your own kit
    };

    inline skins_t skins{};

    // The side the local player is on right now (written by the game thread every frame).
    inline std::atomic_int liveTeam{ TEAM_T };

    // The side the menu is editing. Follows your live side when the menu opens.
    inline int editTeam = TEAM_T;

    inline skin_changer_t& Loadout(int team)
    {
        return skins.loadouts[team == TEAM_CT ? TEAM_CT : TEAM_T];
    }

    // The side you're playing, in the menu's copy (render thread). The game thread uses GameLive() below.
    inline skin_changer_t& Live()
    {
        return Loadout(liveTeam.load(std::memory_order_relaxed));
    }

    // What the menu edits (render thread).
    inline skin_changer_t& Edit()
    {
        return Loadout(editTeam);
    }

    // T agents only fit T, CT agents only fit CT.
    inline std::uint16_t& AgentFor(int team)
    {
        return skins.loadouts[team == TEAM_CT ? TEAM_CT : TEAM_T].agentDefinition;
    }

    // ── The game thread's copy ──────────────────────────────────────────────────────────────────────────────────
    // `skins` belongs to the render thread: the menu inserts into its maps, loading a config / share code / the
    // randomizer rebuild them. std::unordered_map can't be read on one thread while another one inserts (a rehash
    // frees the buckets under the reader) — that was a crash waiting for someone to pick a skin mid-match.
    // So the game thread never touches `skins`: the render thread publishes a copy whenever it changed
    // (PublishSkins, end of every menu frame) and the game thread takes the newest one at the start of its frame
    // (SyncGameSkins) and only ever reads that (Game / GameLive / GameAgentFor).
    namespace detail
    {
        inline std::mutex publishMutex;
        inline skins_t published{};
        inline std::atomic<std::uint64_t> publishedVersion{ 0 };
        inline std::uint64_t publishedHash = 0;

        inline skins_t game{};                    // game thread only
        inline std::uint64_t gameVersion = 0;     // game thread only

        inline void Hash(std::uint64_t& hash, const void* data, std::size_t size)
        {
            const auto* bytes = static_cast<const std::uint8_t*>(data);
            for (std::size_t i = 0; i < size; ++i)
            {
                hash ^= bytes[i];
                hash *= 1099511628211ULL;
            }
        }

        inline void HashSkin(std::uint64_t& hash, std::uint16_t definitionIndex, const item_skin_t& skin)
        {
            Hash(hash, &definitionIndex, sizeof(definitionIndex));
            Hash(hash, &skin.enabled, sizeof(skin.enabled));
            Hash(hash, &skin.paintKit, sizeof(skin.paintKit));
            Hash(hash, &skin.seed, sizeof(skin.seed));
            Hash(hash, &skin.wear, sizeof(skin.wear));
            Hash(hash, &skin.statTrakEnabled, sizeof(skin.statTrakEnabled));
            Hash(hash, &skin.statTrakReal, sizeof(skin.statTrakReal));
            Hash(hash, &skin.statTrakKills, sizeof(skin.statTrakKills));
            Hash(hash, &skin.souvenir, sizeof(skin.souvenir));
            Hash(hash, skin.customName.data(), skin.customName.size());

            // field by field: the structs have padding after their bools
            for (const skin_sticker_t& sticker : skin.stickers)
            {
                Hash(hash, &sticker.enabled, sizeof(sticker.enabled));
                Hash(hash, &sticker.kit, sizeof(sticker.kit));
                Hash(hash, &sticker.wear, sizeof(sticker.wear));
                Hash(hash, &sticker.scale, sizeof(sticker.scale));
                Hash(hash, &sticker.rotation, sizeof(sticker.rotation));
                Hash(hash, &sticker.offsetX, sizeof(sticker.offsetX));
                Hash(hash, &sticker.offsetY, sizeof(sticker.offsetY));
            }

            const skin_keychain_t& keychain = skin.keychain;
            Hash(hash, &keychain.enabled, sizeof(keychain.enabled));
            Hash(hash, &keychain.id, sizeof(keychain.id));
            Hash(hash, &keychain.seed, sizeof(keychain.seed));
            Hash(hash, &keychain.sticker, sizeof(keychain.sticker));
            Hash(hash, &keychain.offsetX, sizeof(keychain.offsetX));
            Hash(hash, &keychain.offsetY, sizeof(keychain.offsetY));
            Hash(hash, &keychain.offsetZ, sizeof(keychain.offsetZ));
        }

        // Order-independent: an unordered_map can iterate differently after a rehash with the same content.
        inline std::uint64_t HashMap(const std::unordered_map<std::uint16_t, item_skin_t>& map)
        {
            std::uint64_t sum = map.size();
            for (const auto& [definitionIndex, skin] : map)
            {
                std::uint64_t hash = 14695981039346656037ULL;
                HashSkin(hash, definitionIndex, skin);
                sum += hash * 0x9E3779B97F4A7C15ULL;
            }
            return sum;
        }

        inline std::uint64_t HashSkins(const skins_t& value)
        {
            std::uint64_t hash = 14695981039346656037ULL;
            Hash(hash, &value.enabled, sizeof(value.enabled));
            Hash(hash, &value.musicKit, sizeof(value.musicKit));

            for (const skin_changer_t& loadout : value.loadouts)
            {
                Hash(hash, &loadout.knifeDefinition, sizeof(loadout.knifeDefinition));
                Hash(hash, &loadout.gloveDefinition, sizeof(loadout.gloveDefinition));
                Hash(hash, &loadout.agentDefinition, sizeof(loadout.agentDefinition));
                const std::uint64_t maps[3] = { HashMap(loadout.weapons), HashMap(loadout.knives), HashMap(loadout.gloves) };
                Hash(hash, maps, sizeof(maps));
            }

            return hash;
        }
    }

    // Render thread: hand the game thread a copy if anything changed. Cheap when nothing did (a hash).
    inline void PublishSkins(bool force = false)
    {
        const std::uint64_t hash = detail::HashSkins(skins);

        if (!force && hash == detail::publishedHash && detail::publishedVersion.load(std::memory_order_relaxed) != 0)
            return;

        {
            std::lock_guard lock(detail::publishMutex);
            detail::published = skins;
        }

        detail::publishedHash = hash;
        detail::publishedVersion.fetch_add(1, std::memory_order_release);
    }

    // Game thread, start of its frame: take the newest published copy.
    inline void SyncGameSkins()
    {
        const std::uint64_t version = detail::publishedVersion.load(std::memory_order_acquire);

        if (version == detail::gameVersion)
            return;

        std::lock_guard lock(detail::publishMutex);
        detail::game = detail::published;
        detail::gameVersion = version;
    }

    // Game thread only.
    inline const skins_t& Game() { return detail::game; }
    inline const skin_changer_t& GameLive() { return detail::game.loadouts[liveTeam.load(std::memory_order_relaxed) == TEAM_CT ? TEAM_CT : TEAM_T]; }
    inline std::uint16_t GameAgentFor(int team) { return detail::game.loadouts[team == TEAM_CT ? TEAM_CT : TEAM_T].agentDefinition; }
}
