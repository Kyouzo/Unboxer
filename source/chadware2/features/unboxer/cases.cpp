#include "cases.h"
#include "prices_resource.h"

#include <Windows.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <charconv>
#include <cstring>
#include <functional>
#include <iterator>
#include <map>
#include <mutex>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

#include "../skinchanger/item_schema.h"

namespace
{
    // ── a small KeyValues reader: only the four sections we need become a tree, everything else is skipped ──

    struct Node
    {
        std::string_view key;
        std::string_view value;
        std::vector<Node> children;
        bool block = false;

        [[nodiscard]] const Node* Child(std::string_view name) const
        {
            for (const Node& child : children)
            {
                if (child.key == name)
                    return &child;
            }

            return nullptr;
        }

        [[nodiscard]] std::string_view Value(std::string_view name) const
        {
            const Node* child = Child(name);
            return child && !child->block ? child->value : std::string_view{};
        }
    };

    class Reader
    {
    public:
        explicit Reader(std::string_view text) : m_text(text) {}

        // Next token: a quoted string (without quotes), a bare word, "{" or "}". False at the end.
        bool Next(std::string_view& token)
        {
            Skip();

            if (m_pos >= m_text.size())
                return false;

            const char c = m_text[m_pos];

            if (c == '{' || c == '}')
            {
                token = m_text.substr(m_pos++, 1);
                return true;
            }

            if (c == '"')
            {
                const std::size_t start = ++m_pos;

                while (m_pos < m_text.size() && m_text[m_pos] != '"')
                    m_pos += m_text[m_pos] == '\\' ? 2 : 1;

                token = m_text.substr(start, std::min(m_pos, m_text.size()) - start);
                ++m_pos;
                return true;
            }

            const std::size_t start = m_pos;

            while (m_pos < m_text.size() && !std::isspace(static_cast<unsigned char>(m_text[m_pos])) &&
                m_text[m_pos] != '{' && m_text[m_pos] != '}' && m_text[m_pos] != '"')
                ++m_pos;

            token = m_text.substr(start, m_pos - start);
            return !token.empty();
        }

        // Reads the rest of a block (its "{" already read) into `parent`.
        void ReadBlock(Node& parent)
        {
            std::string_view token;

            while (Next(token))
            {
                if (token == "}")
                    return;

                Node node;
                node.key = token;

                std::string_view value;
                if (!Next(value))
                    return;

                if (value == "{")
                {
                    node.block = true;
                    ReadBlock(node);
                }
                else if (value == "}")
                {
                    return;   // malformed: a key without a value
                }
                else
                {
                    node.value = value;
                }

                parent.children.push_back(std::move(node));
            }
        }

        // Skips the rest of a block (its "{" already read).
        void SkipBlock()
        {
            int depth = 1;
            std::string_view token;

            while (depth > 0 && Next(token))
            {
                if (token == "{")
                    ++depth;
                else if (token == "}")
                    --depth;
            }
        }

    private:
        void Skip()
        {
            while (m_pos < m_text.size())
            {
                const char c = m_text[m_pos];

                if (std::isspace(static_cast<unsigned char>(c)))
                {
                    ++m_pos;
                    continue;
                }

                if (c == '/' && m_pos + 1 < m_text.size() && m_text[m_pos + 1] == '/')
                {
                    while (m_pos < m_text.size() && m_text[m_pos] != '\n')
                        ++m_pos;
                    continue;
                }

                return;
            }
        }

        std::string_view m_text;
        std::size_t m_pos = 0;
    };

    std::vector<Cases::Case> g_cases;
    std::vector<Cases::Case> g_setCollections;   // collections without a container (definition 0): trade-ups only
    const std::vector<Cases::Case> g_noCases;
    std::vector<Cases::ArmoryReward> g_armory;   // the Armory's rewards, the game's order
    int g_armorySeason = 0;
    const std::vector<Cases::ArmoryReward> g_noArmory;
    std::vector<Cases::Key> g_keys;
    std::unordered_map<std::uint64_t, Cases::Kit> g_kits;   // (kind << 32) | id — stickers, patches, graffiti, music kits, pins
    std::vector<Cases::Kit> g_allKits[6];                    // by DropKind: everything the game has (Add items)
    std::unordered_map<std::uint16_t, std::string> g_itemImages;
    std::unordered_map<std::uint16_t, int> g_itemTiers;
    std::unordered_map<int, Cases::WearRange> g_wearRanges;  // finish -> float range (every finish items_game has)

    // a Major's gold souvenir stickers (items_game sticker_kits, "_gold"): its organizer's logo, each team's, the
    // players' autographs by team - what a souvenir drop of that event carries
    struct SouvenirTeam
    {
        int team = 0;                // tournament_team_id
        int kit = 0;                 // the team's gold sticker
        std::vector<int> players;    // its players' gold autographs
    };

    struct SouvenirEvent
    {
        int organizer = 0;           // the event's logo (tournament_team_id 0)
        std::vector<SouvenirTeam> teams;
    };

    std::unordered_map<int, SouvenirEvent> g_souvenirEvents;
    const std::vector<Cases::Kit> g_noKits;   // the empty answers (file scope: no function-local statics)
    const std::string g_noImage;

    std::uint64_t KitKey(Cases::DropKind kind, int id)
    {
        return (static_cast<std::uint64_t>(kind) << 32) | static_cast<std::uint32_t>(id);
    }
    std::atomic_bool g_ready{ false };

    int ToInt(std::string_view text)
    {
        int value = 0;
        std::from_chars(text.data(), text.data() + text.size(), value);
        return value;
    }

    // "0.060000" -> 0.06; `fallback` if it isn't a number
    float ToFloat(std::string_view text, float fallback)
    {
        float value = 0.0f;
        const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
        return error == std::errc{} && end != text.data() ? value : fallback;
    }

    std::string Lower(std::string text)
    {
        for (char& c : text)
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return text;
    }

    bool EndsWith(std::string_view text, std::string_view suffix)
    {
        return text.size() >= suffix.size() && text.substr(text.size() - suffix.size()) == suffix;
    }

    // Tier of a loot list from its name suffix, or -1 if the name says nothing (then the parent's tier applies).
    int TierFromListName(std::string_view name)
    {
        if (EndsWith(name, "_common")) return Cases::TierConsumer;
        if (EndsWith(name, "_uncommon")) return Cases::TierIndustrial;
        if (EndsWith(name, "_rare")) return Cases::TierMilSpec;
        if (EndsWith(name, "_mythical")) return Cases::TierRestricted;
        if (EndsWith(name, "_legendary")) return Cases::TierClassified;
        if (EndsWith(name, "_ancient")) return Cases::TierCovert;
        if (EndsWith(name, "_unusual")) return Cases::TierGold;
        return -1;
    }

    int TierFromRarity(std::string_view rarity)
    {
        if (rarity == "common") return Cases::TierConsumer;
        if (rarity == "uncommon") return Cases::TierIndustrial;
        if (rarity == "rare") return Cases::TierMilSpec;
        if (rarity == "mythical") return Cases::TierRestricted;
        if (rarity == "legendary") return Cases::TierClassified;
        if (rarity == "ancient" || rarity == "contraband") return Cases::TierCovert;
        return -1;
    }

    std::string Localize(std::string_view token, std::string_view fallback)
    {
        std::string text = ItemSchema::Localize(std::string(token));
        return text.empty() ? std::string(fallback) : text;
    }

    // resource/csgo_english.txt as token -> text, both lowercase (tokens are case-insensitive). "//" lines are
    // skipped (the file has commented-out duplicates).
    using EnglishTable = std::unordered_map<std::string, std::string>;

    EnglishTable ReadEnglish()
    {
        EnglishTable table;
        std::string text;

        if (!ItemSchema::ReadGameText("resource/csgo_english.txt", text))
            return table;

        // one quoted string starting at `at` (its opening quote); \" and \\ escapes kept as the character
        const auto quoted = [&](std::size_t& at, std::string& out) -> bool
        {
            out.clear();
            for (++at; at < text.size() && text[at] != '"' && text[at] != '\n'; ++at)
            {
                if (text[at] == '\\' && at + 1 < text.size())
                    ++at;
                out += static_cast<char>(std::tolower(static_cast<unsigned char>(text[at])));
            }
            return at < text.size() && text[at++] == '"';
        };

        table.reserve(80000);
        std::size_t line = 0;
        std::string key, value;

        while (line < text.size())
        {
            std::size_t end = text.find('\n', line);
            if (end == std::string::npos)
                end = text.size();

            std::size_t at = text.find_first_not_of(" \t\r", line);

            if (at < end && text[at] == '"')
            {
                if (quoted(at, key))
                {
                    at = text.find('"', at);
                    if (at < end && quoted(at, value) && !key.empty())
                        table.try_emplace(key, value);
                }
            }

            line = end + 1;
        }

        return table;
    }

    // The English text of a localization token ("#SFUI_WPNHUD_AK47" -> "ak-47"), lowercase; empty if unknown.
    std::string EnglishLower(const EnglishTable& english, std::string_view token)
    {
        if (!token.empty() && token.front() == '#')
            token.remove_prefix(1);

        const auto it = english.find(Lower(std::string(token)));
        return it != english.end() ? it->second : std::string{};
    }

    // English names (lowercase) for market names: weapons / knives / gloves by definition, finishes by id; every other
    // item (agents, cases, keys...) by definition; charms by id, as the market writes them ("charm | ..." / a highlight's
    // "souvenir charm | <event> highlight | <clip>").
    std::unordered_map<std::uint16_t, std::string> g_englishWeapons;
    std::unordered_map<int, std::string> g_englishFinishes;
    std::unordered_map<std::uint16_t, std::string> g_englishItems;
    std::unordered_map<int, std::string> g_englishCharms;
    std::vector<Cases::Highlight> g_highlights;                 // by display name
    std::unordered_map<int, std::size_t> g_highlightByNumber;  // number -> g_highlights index
    std::unordered_map<int, int> g_highlightOfCharm;           // a clip's own charm -> its number
    std::unordered_map<int, std::vector<const Cases::Highlight*>> g_clipsByEvent;   // event charm -> its clips
    std::unordered_map<int, int> g_eventOfCharm;               // event charm / a clip's own charm -> the event charm
    const std::vector<Cases::Highlight> g_noHighlights;
    const std::vector<const Cases::Highlight*> g_noClips;
}

namespace
{
    HMODULE SelfModule()
    {
        HMODULE self = nullptr;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(&SelfModule), &self);
        return self;
    }

    // data\case_rares.txt (compiled in by prices.rc): "LOOTLINE-CASE-RARES 1 <unix time> <date>", then per case
    // "<case definition>\t<definition>:<finish> <definition>:<finish> ...\t# <name>" (finish 0 = a vanilla knife).
    std::unordered_map<std::uint16_t, std::vector<Cases::Drop>> ReadCaseRares()
    {
        std::unordered_map<std::uint16_t, std::vector<Cases::Drop>> out;
        const HMODULE self = SelfModule();
        const HRSRC info = self ? FindResourceW(self, MAKEINTRESOURCEW(IDR_CASE_RARES_TXT), MAKEINTRESOURCEW(10) /* RT_RCDATA */) : nullptr;
        const HGLOBAL data = info ? LoadResource(self, info) : nullptr;
        const char* bytes = data ? static_cast<const char*>(LockResource(data)) : nullptr;
        const DWORD size = info ? SizeofResource(self, info) : 0;

        if (!bytes || !size)
            return out;

        const std::string_view text(bytes, size);

        if (!text.starts_with("LOOTLINE-CASE-RARES 1 "))
            return out;

        const auto number = [](std::string_view part, int& value)
        {
            return std::from_chars(part.data(), part.data() + part.size(), value).ec == std::errc{};
        };

        std::size_t lineStart = text.find('\n');

        while (lineStart != std::string_view::npos && lineStart + 1 < text.size())
        {
            const std::size_t lineEnd = text.find('\n', lineStart + 1);
            std::string_view line = text.substr(lineStart + 1, lineEnd == std::string_view::npos ? std::string_view::npos : lineEnd - lineStart - 1);
            lineStart = lineEnd;

            const std::size_t tab = line.find('\t');
            int caseDefinition = 0;

            if (tab == std::string_view::npos || !number(line.substr(0, tab), caseDefinition) || caseDefinition <= 0 || caseDefinition > 0xFFFF)
                continue;

            std::string_view list = line.substr(tab + 1);
            list = list.substr(0, list.find('\t'));   // the "# name" comment
            std::vector<Cases::Drop>& golds = out[static_cast<std::uint16_t>(caseDefinition)];

            while (!list.empty())
            {
                const std::size_t space = list.find(' ');
                const std::string_view entry = list.substr(0, space);
                list = space == std::string_view::npos ? std::string_view() : list.substr(space + 1);

                const std::size_t colon = entry.find(':');
                int definition = 0;
                int paintKit = 0;

                if (colon != std::string_view::npos && number(entry.substr(0, colon), definition) && number(entry.substr(colon + 1), paintKit) &&
                    definition > 0 && definition <= 0xFFFF && paintKit >= 0)
                    golds.push_back({ static_cast<std::uint16_t>(definition), paintKit, Cases::TierGold });
            }
        }

        return out;
    }
}

bool Cases::Build()
{
    if (g_ready.load(std::memory_order_acquire))
        return true;

    // every weapon case's knives / gloves (the game files don't have them)
    const std::unordered_map<std::uint16_t, std::vector<Cases::Drop>> caseRares = ReadCaseRares();

    if (!Skins::g_ItemSchema.IsInitialized())
        return false;

    std::string text;
    if (!ItemSchema::ReadGameText("scripts/items/items_game.txt", text))
        return false;

    // ── read the four sections (each appears many times: Valve appends a block per update) ──
    Reader reader(text);
    std::vector<Node> itemBlocks, prefabBlocks, revolvingBlocks, lootBlocks, stickerBlocks, musicBlocks, setBlocks, rarityBlocks, reelBlocks,
        teamBlocks, paintKitBlocks, seasonBlocks;
    std::string_view token;
    int depth = 0;

    while (reader.Next(token))
    {
        if (token == "{")
        {
            ++depth;
            continue;
        }

        if (token == "}")
        {
            --depth;
            continue;
        }

        if (depth != 1)
            continue;

        const std::string_view section = token;
        std::string_view value;

        if (!reader.Next(value))
            break;

        if (value != "{")
            continue;

        std::vector<Node>* target =
            section == "items" ? &itemBlocks :
            section == "prefabs" ? &prefabBlocks :
            section == "revolving_loot_lists" ? &revolvingBlocks :
            section == "client_loot_lists" ? &lootBlocks :
            section == "sticker_kits" ? &stickerBlocks :          // stickers, patches, graffiti
            section == "music_definitions" ? &musicBlocks :
            section == "item_sets" ? &setBlocks :                // collections (a container whose list isn't shipped)
            section == "paint_kits_rarity" ? &rarityBlocks :
            section == "paint_kits" ? &paintKitBlocks :          // float ranges
            section == "highlight_reels" ? &reelBlocks :
            section == "seasonaloperations" ? &seasonBlocks :    // the Armory's rewards
            section == "pro_teams" ? &teamBlocks : nullptr;

        if (!target)
        {
            reader.SkipBlock();
            continue;
        }

        Node block;
        block.block = true;
        reader.ReadBlock(block);
        target->push_back(std::move(block));
    }

    std::unordered_map<std::string_view, const Node*> prefabs, items, lootLists;
    std::unordered_map<std::string_view, std::string_view> revolving;

    for (const Node& block : prefabBlocks)
        for (const Node& node : block.children)
            if (node.block) prefabs[node.key] = &node;

    for (const Node& block : itemBlocks)
        for (const Node& node : block.children)
            if (node.block) items[node.key] = &node;

    for (const Node& block : lootBlocks)
        for (const Node& node : block.children)
            if (node.block) lootLists[node.key] = &node;

    for (const Node& block : revolvingBlocks)
        for (const Node& node : block.children)
            if (!node.block) revolving[node.key] = node.value;

    std::unordered_map<std::string_view, const Node*> itemSets;
    std::unordered_map<std::string_view, std::string_view> finishRarity;

    for (const Node& block : setBlocks)
        for (const Node& node : block.children)
            if (node.block) itemSets[node.key] = &node;

    for (const Node& block : rarityBlocks)
        for (const Node& node : block.children)
            if (!node.block) finishRarity[node.key] = node.value;

    // finish float ranges: a finish without its own min / max has the default finish's ("0": 0.06 - 0.80)
    Cases::WearRange defaultRange{ 0.06f, 0.80f };
    for (const Node& block : paintKitBlocks)
        for (const Node& node : block.children)
            if (node.block && node.key == "0")
                defaultRange = { ToFloat(node.Value("wear_remap_min"), 0.06f), ToFloat(node.Value("wear_remap_max"), 0.80f) };

    std::unordered_map<int, Cases::WearRange> wearRanges;
    for (const Node& block : paintKitBlocks)
    {
        for (const Node& node : block.children)
        {
            if (!node.block)
                continue;

            Cases::WearRange range{ ToFloat(node.Value("wear_remap_min"), defaultRange.min), ToFloat(node.Value("wear_remap_max"), defaultRange.max) };
            range.min = std::clamp(range.min, 0.0f, 1.0f);
            range.max = std::clamp(range.max, range.min, 1.0f);
            wearRanges[ToInt(node.key)] = range;
        }
    }

    // loot list entries name stickers / music kits / pins by their "name"
    std::unordered_map<std::string_view, std::pair<int, const Node*>> stickerByName, musicByName, itemByName;

    for (const Node& block : stickerBlocks)
        for (const Node& node : block.children)
            if (node.block) stickerByName[node.Value("name")] = { ToInt(node.key), &node };

    // the Majors' gold souvenir stickers: the organizer's logo, the teams', the players' (by team)
    std::unordered_map<int, SouvenirEvent> souvenirEvents;
    {
        std::unordered_map<int, std::unordered_map<int, std::vector<int>>> playersByTeam;   // event -> team -> autographs

        for (const Node& block : stickerBlocks)
        {
            for (const Node& node : block.children)
            {
                const std::string_view name = node.Value("name");
                const int event = ToInt(node.Value("tournament_event_id"));

                // the champions' own autographs ("..._champion") aren't a match's MVP sticker
                if (!node.block || event <= 0 || !name.ends_with("_gold") || node.Value("tournament_team_id").empty() ||
                    node.Value("item_name").find("champion") != std::string_view::npos)
                    continue;

                const int kit = ToInt(node.key);
                const int team = ToInt(node.Value("tournament_team_id"));

                if (!node.Value("tournament_player_id").empty())
                    playersByTeam[event][team].push_back(kit);
                else if (team == 0)
                    souvenirEvents[event].organizer = kit;
                else
                    souvenirEvents[event].teams.push_back({ team, kit, {} });
            }
        }

        for (auto& [event, souvenir] : souvenirEvents)
        {
            for (SouvenirTeam& team : souvenir.teams)
            {
                const auto players = playersByTeam[event].find(team.team);
                if (players != playersByTeam[event].end())
                    team.players = players->second;
            }

            std::sort(souvenir.teams.begin(), souvenir.teams.end(), [](const SouvenirTeam& a, const SouvenirTeam& b) { return a.team < b.team; });
        }
    }

    for (const Node& block : musicBlocks)
        for (const Node& node : block.children)
            if (node.block) musicByName[node.Value("name")] = { ToInt(node.key), &node };

    for (const Node& block : itemBlocks)
        for (const Node& node : block.children)
            if (node.block && !node.Value("name").empty()) itemByName[node.Value("name")] = { ToInt(node.key), &node };

    // ── English names (market prices, the ★ footers), whatever the game's language ──
    const EnglishTable english = ReadEnglish();
    std::unordered_map<std::uint16_t, std::string> englishWeapons;
    std::unordered_map<int, std::string> englishFinishes;
    std::unordered_map<std::uint16_t, std::string> englishItems;
    std::unordered_map<int, std::string> englishCharms;

    for (const ItemDefinitionInfo& item : Skins::g_ItemSchema.GetItems())
    {
        std::string name = EnglishLower(english, item.baseName);

        if (name.empty())
            continue;

        if (item.isWeapon || item.isKnife || item.isGlove)
            englishWeapons.emplace(item.definitionIndex, std::move(name));
        else
            englishItems.emplace(item.definitionIndex, std::move(name));   // agents, cases, keys, packages...
    }

    {
        std::unordered_map<std::string, const KeychainInfo*> charmsByName;
        for (const KeychainInfo& charm : Skins::g_ItemSchema.GetKeychains())
            charmsByName.emplace(charm.name, &charm);

        for (const KeychainInfo& charm : Skins::g_ItemSchema.GetKeychains())
        {
            const std::string name = EnglishLower(english, charm.locName);

            if (name.empty())
                continue;

            // a highlight charm: its event's charm + the clip ("souvenir charm | cologne 2026 highlight | asap ace")
            if (const auto base = charm.base.empty() ? charmsByName.end() : charmsByName.find(charm.base); base != charmsByName.end())
            {
                if (const std::string event = EnglishLower(english, base->second->locName); !event.empty())
                {
                    englishCharms.emplace(charm.id, "souvenir charm | " + event + " | " + name);
                    continue;
                }
            }

            englishCharms.emplace(charm.id, "charm | " + name);
        }
    }

    // ── Major highlight clips: the clip's own charm ("kc_" + id) or its event's ("kc_" + "aus2025"); later blocks win ──
    std::map<int, Cases::Highlight> highlightsByNumber;
    {
        // team tags ("VIT") by pro team id, for a clip's match line
        std::unordered_map<std::string_view, std::string_view> teamTags;
        for (const Node& block : teamBlocks)
            for (const Node& node : block.children)
                if (node.block && !node.Value("tag").empty()) teamTags[node.key] = node.Value("tag");

        std::unordered_map<std::string, const KeychainInfo*> charmsByName;
        for (const KeychainInfo& charm : Skins::g_ItemSchema.GetKeychains())
            charmsByName.emplace(charm.name, &charm);

        for (const Node& block : reelBlocks)
            for (const Node& node : block.children)
            {
                const int number = ToInt(node.key);
                const std::string id(node.Value("id"));

                if (!node.block || number <= 0 || id.empty())
                    continue;

                const auto own = charmsByName.find("kc_" + id);
                const auto event = charmsByName.find("kc_" + id.substr(0, id.find('_')));
                const KeychainInfo* charm = own != charmsByName.end() ? own->second : event != charmsByName.end() ? event->second : nullptr;

                if (!charm)
                    continue;   // no charm shows it

                Cases::Highlight highlight;
                highlight.number = number;
                highlight.keychain = charm->id;
                highlight.own = own != charmsByName.end();
                highlight.id = id;
                highlight.event = charm->id;

                if (highlight.own)
                    if (const auto base = charmsByName.find(charm->base); base != charmsByName.end())
                        highlight.event = base->second->id;

                highlight.clipName = Localize("#HighlightReel_" + id, id);

                // "VIT vs MOUZ · Mirage"
                const auto team0 = teamTags.find(node.Value("tournament event team0 id"));
                const auto team1 = teamTags.find(node.Value("tournament event team1 id"));

                if (team0 != teamTags.end() && team1 != teamTags.end())
                    highlight.match = std::string(team0->second) + " vs " + std::string(team1->second);

                if (std::string map(node.Value("map")); !map.empty())
                {
                    if (const std::size_t underscore = map.find('_'); underscore != std::string::npos && underscore + 1 < map.size())
                        map.erase(0, underscore + 1);   // de_mirage -> mirage

                    map[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(map[0])));
                    highlight.match += (highlight.match.empty() ? "" : " \xC2\xB7 ") + map;
                }

                if (highlight.own)
                {
                    highlight.displayName = charm->displayName;   // the item schema's "<event> | <clip>"

                    if (const auto name = englishCharms.find(charm->id); name != englishCharms.end())
                        highlight.english = name->second;
                }
                else
                {
                    highlight.displayName = charm->displayName + " | " + highlight.clipName;
                    const std::string eventName = EnglishLower(english, charm->locName);
                    const std::string clipName = EnglishLower(english, "#HighlightReel_" + id);

                    if (!eventName.empty() && !clipName.empty())
                        highlight.english = "souvenir charm | " + eventName + " | " + clipName;
                }

                highlight.search = Lower(highlight.clipName + " " + id + " " + highlight.match);
                highlightsByNumber[number] = std::move(highlight);
            }
    }

    std::vector<Cases::Highlight> highlights;
    highlights.reserve(highlightsByNumber.size());

    for (auto& [number, highlight] : highlightsByNumber)
        highlights.push_back(std::move(highlight));

    std::sort(highlights.begin(), highlights.end(), [](const Cases::Highlight& a, const Cases::Highlight& b)
    {
        return a.search != b.search ? a.search < b.search : a.number < b.number;
    });

    std::unordered_map<int, std::size_t> highlightByNumber;
    std::unordered_map<int, int> highlightOfCharm;

    for (std::size_t i = 0; i < highlights.size(); ++i)
    {
        highlightByNumber.emplace(highlights[i].number, i);

        if (highlights[i].own)
            highlightOfCharm.emplace(highlights[i].keychain, highlights[i].number);
    }

    for (const PaintKitInfo& paintKit : Skins::g_ItemSchema.GetPaintKits())
    {
        if (paintKit.id > 0)
            if (std::string name = EnglishLower(english, paintKit.descriptionTag); !name.empty())
                englishFinishes.emplace(paintKit.id, std::move(name));
    }

    std::unordered_map<std::uint64_t, Cases::Kit> kits;

    const auto dropKind = [&](const Cases::Drop& drop)
    {
        switch (drop.weapon)
        {
        case Cases::DEF_STICKER: return Cases::DropKind::Sticker;
        case Cases::DEF_PATCH: return Cases::DropKind::Patch;
        case Cases::DEF_GRAFFITI: return Cases::DropKind::Graffiti;
        case Cases::DEF_MUSIC_KIT: return Cases::DropKind::MusicKit;
        default: return drop.paintKit == 0 && kits.contains(KitKey(Cases::DropKind::Pin, drop.weapon)) ? Cases::DropKind::Pin : Cases::DropKind::Skin;
        }
    };

    // ── weapon / paint kit lookups by name ──
    std::unordered_map<std::string, std::uint16_t> weaponByName;
    for (const ItemDefinitionInfo& item : Skins::g_ItemSchema.GetItems())
    {
        if (!item.itemName.empty() && (item.isWeapon || item.isKnife || item.isGlove))
            weaponByName.emplace(item.itemName, item.definitionIndex);
    }

    std::unordered_map<std::string, int> paintKitByName;
    for (const PaintKitInfo& paintKit : Skins::g_ItemSchema.GetPaintKits())
    {
        if (paintKit.id > 0 && !paintKit.name.empty())
            paintKitByName.emplace(paintKit.name, paintKit.id);
    }

    // Does `prefab` (a space-separated list) or anything it inherits have `part` in its name ("souvenir")?
    const auto prefabHas = [&](std::string_view prefab, std::string_view part)
    {
        for (int guard = 0; guard < 12 && !prefab.empty(); ++guard)
        {
            const std::string_view first = prefab.substr(0, prefab.find(' '));

            if (first.find(part) != std::string_view::npos)
                return true;

            const auto it = prefabs.find(first);
            if (it == prefabs.end())
                return false;

            prefab = it->second->Value("prefab");
        }

        return false;
    };

    // Expands a loot list into drops (nested lists inherit / override the tier).
    std::vector<Cases::Drop> drops;
    std::unordered_set<std::string_view> visited;
    bool statTrakList = false;

    // charms in a loot list ("[kc_db_eco]keychain"): only for the Armory's charm packs (containers keep skipping them)
    bool withCharms = false;
    std::unordered_map<std::string_view, int> charmByName;
    for (const KeychainInfo& charm : Skins::g_ItemSchema.GetKeychains())
        charmByName.emplace(charm.name, charm.id);

    std::function<void(std::string_view, int, int)> expand;
    expand = [&](std::string_view listName, int tier, int level)
    {
        if (level > 6 || !visited.insert(listName).second)
            return;

        const auto it = lootLists.find(listName);
        if (it == lootLists.end())
            return;

        const int ownTier = TierFromListName(listName);
        if (ownTier >= 0)
            tier = ownTier;

        for (const Node& entry : it->second->children)
        {
            const std::string_view key = entry.key;

            if (key == "will_produce_stattrak")
            {
                statTrakList = true;
                continue;
            }

            if (lootLists.contains(key))
            {
                expand(key, tier, level + 1);
                continue;
            }

            if (tier == Cases::TierGold)
                continue;   // ★ lists: not in the game files

            // a pin (or any item) by its name: "Commodity Pin - Guardian"
            if (key.empty() || key.front() != '[')
            {
                const auto named = itemByName.find(key);
                if (named == itemByName.end() || tier < 0)
                    continue;

                const Node* itemNode = named->second.second;
                const int definition = named->second.first;
                kits.try_emplace(KitKey(Cases::DropKind::Pin, definition), Cases::Kit{ definition, Cases::DropKind::Pin,
                    Localize(itemNode->Value("item_name"), key), std::string(itemNode->Value("image_inventory")),
                    EnglishLower(english, itemNode->Value("item_name")) });
                drops.push_back({ static_cast<std::uint16_t>(definition), 0, static_cast<Cases::Tier>(tier) });
                continue;
            }

            // "[kit]what": weapon_x (a skin), sticker, patch, spray (graffiti), musickit
            const std::size_t close = key.find(']');
            if (close == std::string_view::npos || close < 2)
                continue;

            const std::string_view kitName = key.substr(1, close - 1);
            const std::string_view what = key.substr(close + 1);

            if (what == "sticker" || what == "patch" || what == "spray")
            {
                const auto sticker = stickerByName.find(kitName);
                if (sticker == stickerByName.end())
                    continue;

                const Node* kitNode = sticker->second.second;
                const int kitTier = tier >= 0 ? tier : TierFromRarity(kitNode->Value("item_rarity"));
                if (kitTier < 0)
                    continue;

                const Cases::DropKind kind = what == "sticker" ? Cases::DropKind::Sticker : what == "patch" ? Cases::DropKind::Patch : Cases::DropKind::Graffiti;
                const std::uint16_t definition = kind == Cases::DropKind::Sticker ? Cases::DEF_STICKER : kind == Cases::DropKind::Patch ? Cases::DEF_PATCH : Cases::DEF_GRAFFITI;
                const std::string_view material = kind == Cases::DropKind::Patch ? kitNode->Value("patch_material") : kitNode->Value("sticker_material");
                const std::string image = material.empty() ? std::string() :
                    std::string(kind == Cases::DropKind::Patch ? "econ/patches/" : "econ/stickers/") + std::string(material);

                kits.try_emplace(KitKey(kind, sticker->second.first), Cases::Kit{ sticker->second.first, kind,
                    Localize(kitNode->Value("item_name"), kitName), image, EnglishLower(english, kitNode->Value("item_name")) });
                drops.push_back({ definition, sticker->second.first, static_cast<Cases::Tier>(kitTier) });
                continue;
            }

            if (what == "musickit")
            {
                const auto music = musicByName.find(kitName);
                if (music == musicByName.end())
                    continue;

                const Node* kitNode = music->second.second;
                int kitTier = tier >= 0 ? tier : TierFromRarity(kitNode->Value("item_rarity"));
                if (kitTier < 0)
                    kitTier = Cases::TierMilSpec;   // music kits are all High Grade (their lists and definitions don't say)

                kits.try_emplace(KitKey(Cases::DropKind::MusicKit, music->second.first), Cases::Kit{ music->second.first, Cases::DropKind::MusicKit,
                    Localize(kitNode->Value("loc_name"), kitName), std::string(kitNode->Value("image_inventory")),
                    EnglishLower(english, kitNode->Value("loc_name")) });
                drops.push_back({ Cases::DEF_MUSIC_KIT, music->second.first, static_cast<Cases::Tier>(kitTier) });
                continue;
            }

            if (what == "keychain")
            {
                const auto charm = withCharms && tier >= 0 ? charmByName.find(kitName) : charmByName.end();

                if (charm != charmByName.end())
                    drops.push_back({ Cases::DEF_KEYCHAIN, charm->second, static_cast<Cases::Tier>(tier) });

                continue;
            }

            if (tier < 0)
                continue;

            const auto weapon = weaponByName.find(std::string(what));
            const auto paintKit = paintKitByName.find(std::string(kitName));

            if (weapon == weaponByName.end() || paintKit == paintKitByName.end())
                continue;

            drops.push_back({ weapon->second, paintKit->second, static_cast<Cases::Tier>(tier) });
        }
    };

    // A value on the item or the nearest prefab that has it ("loot_list_rare_item_footer" sits on both).
    const auto inherited = [&](const Node* node, std::string_view key) -> std::string_view
    {
        if (const std::string_view own = node->Value(key); !own.empty())
            return own;

        std::string_view prefab = node->Value("prefab");

        for (int guard = 0; guard < 12 && !prefab.empty(); ++guard)
        {
            const auto it = prefabs.find(prefab.substr(0, prefab.find(' ')));
            if (it == prefabs.end())
                break;

            if (const std::string_view value = it->second->Value(key); !value.empty())
                return value;

            prefab = it->second->Value("prefab");
        }

        return {};
    };

    // An attribute's value on the item or the nearest prefab that has it (from 2024 on, a souvenir package's "tournament
    // event id" sits on its event's prefab: crate_bud2025_promo_de_dust2 -> bud2025_souvenir_crate_promo_prefab).
    const auto inheritedAttribute = [&](const Node* node, std::string_view name) -> std::string_view
    {
        const auto own = [name](const Node* holder) -> std::string_view
        {
            const Node* attributes = holder->Child("attributes");
            const Node* attribute = attributes ? attributes->Child(name) : nullptr;
            return attribute ? (attribute->block ? attribute->Value("value") : attribute->value) : std::string_view{};
        };

        if (const std::string_view value = own(node); !value.empty())
            return value;

        std::string_view prefab = node->Value("prefab");

        for (int guard = 0; guard < 12 && !prefab.empty(); ++guard)
        {
            const auto it = prefabs.find(prefab.substr(0, prefab.find(' ')));
            if (it == prefabs.end())
                break;

            if (const std::string_view value = own(it->second); !value.empty())
                return value;

            prefab = it->second->Value("prefab");
        }

        return {};
    };

    // ── ★ types named by the cases' footers, matched in English ──

    std::vector<std::pair<std::string, std::uint16_t>> knifeNames;   // english name (lowercase), definition
    std::vector<std::uint16_t> gloveTypes;

    for (const ItemDefinitionInfo& item : Skins::g_ItemSchema.GetItems())
    {
        if (item.isKnife && item.definitionIndex >= 500)
        {
            if (std::string name = EnglishLower(english, item.baseName); !name.empty())
                knifeNames.emplace_back(std::move(name), item.definitionIndex);
        }
        else if (item.isGlove && item.definitionIndex != 5028 && item.definitionIndex != 5029)
        {
            gloveTypes.push_back(item.definitionIndex);
        }
    }

    // ── the cases ──
    std::vector<Cases::Case> cases;
    std::unordered_map<std::uint16_t, Cases::Key> keys;

    for (const auto& [id, node] : items)
    {
        const Node* attributes = node->Child("attributes");
        const Node* series = attributes ? attributes->Child("set supply crate series") : nullptr;
        const std::string_view seriesValue = series ? (series->block ? series->Value("value") : series->value) : std::string_view{};

        if (seriesValue.empty())
            continue;

        const auto list = revolving.find(seriesValue);
        if (list == revolving.end())
            continue;

        drops.clear();
        visited.clear();
        statTrakList = false;
        expand(list->second, -1, 0);

        // its list isn't in the game files (the X-Ray P250 Package): its collection's skins, at their own rarity
        if (drops.empty() && !lootLists.contains(list->second))
        {
            const Node* tags = node->Child("tags");
            const Node* set = tags ? tags->Child("ItemSet") : nullptr;
            const auto collection = set ? itemSets.find(set->Value("tag_value")) : itemSets.end();
            const Node* setItems = collection != itemSets.end() ? collection->second->Child("items") : nullptr;

            for (std::size_t i = 0; setItems && i < setItems->children.size(); ++i)
            {
                const Node& entry = setItems->children[i];
                const std::string_view key = entry.key;
                const std::size_t close = key.find(']');

                if (key.empty() || key.front() != '[' || close == std::string_view::npos || close < 2)
                    continue;

                const std::string_view kitName = key.substr(1, close - 1);
                const auto weapon = weaponByName.find(std::string(key.substr(close + 1)));
                const auto paintKit = paintKitByName.find(std::string(kitName));
                const auto rarity = finishRarity.find(kitName);
                const int tier = rarity != finishRarity.end() ? TierFromRarity(rarity->second) : -1;

                if (weapon != weaponByName.end() && paintKit != paintKitByName.end() && tier >= 0)
                    drops.push_back({ weapon->second, paintKit->second, static_cast<Cases::Tier>(tier) });
            }
        }

        if (drops.empty())
            continue;

        std::stable_sort(drops.begin(), drops.end(), [](const Cases::Drop& a, const Cases::Drop& b) { return a.tier < b.tier; });

        Cases::Case item;
        item.definition = static_cast<std::uint16_t>(ToInt(id));
        item.name = std::string(node->Value("name"));
        item.displayName = Localize(node->Value("item_name"), item.name);
        item.image = std::string(node->Value("image_inventory"));
        item.sortName = Lower(item.displayName);
        item.drops = drops;

        // what it is: by what's mostly in it
        int counts[6]{};
        for (const Cases::Drop& drop : drops)
            ++counts[static_cast<int>(dropKind(drop))];

        const int mostly = static_cast<int>(std::max_element(std::begin(counts), std::end(counts)) - std::begin(counts));
        const bool souvenir = prefabHas(node->Value("prefab"), "souvenir");

        switch (static_cast<Cases::DropKind>(mostly))
        {
        case Cases::DropKind::Sticker: item.category = Cases::Category::Stickers; break;
        case Cases::DropKind::Patch: item.category = Cases::Category::Patches; break;
        case Cases::DropKind::Graffiti: item.category = Cases::Category::Graffiti; break;
        case Cases::DropKind::MusicKit: item.category = Cases::Category::MusicKits; break;
        case Cases::DropKind::Pin: item.category = Cases::Category::Pins; break;
        default: item.category = souvenir ? Cases::Category::Souvenir : Cases::Category::Weapon; break;
        }

        item.statTrakOnly = statTrakList;

        if (item.category == Cases::Category::Souvenir)
            item.tournamentEvent = ToInt(inheritedAttribute(node, "tournament event id"));

        const Node* volatileAttribute = attributes->Child("volatile container");
        const std::string_view volatileValue = volatileAttribute ? (volatileAttribute->block ? volatileAttribute->Value("value") : volatileAttribute->value) : std::string_view{};
        item.terminal = !volatileValue.empty() && volatileValue != "0";

        const Node* associated = item.category == Cases::Category::Weapon ? node->Child("associated_items") : nullptr;

        if (associated && !associated->children.empty())
        {
            item.key = static_cast<std::uint16_t>(ToInt(associated->children.front().key));

            if (const auto keyNode = items.find(associated->children.front().key); keyNode != items.end() && !keys.contains(item.key))
            {
                Cases::Key key;
                key.definition = item.key;
                key.displayName = Localize(keyNode->second->Value("item_name"), keyNode->second->Value("name"));
                key.image = std::string(keyNode->second->Value("image_inventory"));
                keys.emplace(item.key, std::move(key));
            }
        }

        // no key of its own: the one its prefab names (old cases: "associated_item" "1203", the CS:GO Case Key)
        if (item.key == 0 && item.category == Cases::Category::Weapon)
        {
            std::string_view prefab = node->Value("prefab");

            for (int guard = 0; guard < 12 && !prefab.empty() && item.key == 0; ++guard)
            {
                const auto it = prefabs.find(prefab.substr(0, prefab.find(' ')));
                if (it == prefabs.end())
                    break;

                if (const std::string_view keyId = it->second->Value("associated_item"); !keyId.empty() && prefab.starts_with("weapon_case") && !prefab.starts_with("weapon_case_souvenirpkg"))
                    item.key = static_cast<std::uint16_t>(ToInt(keyId));

                prefab = it->second->Value("prefab");
            }

            if (item.key != 0 && !keys.contains(item.key))
            {
                if (const auto keyNode = items.find(std::to_string(item.key)); keyNode != items.end())
                {
                    Cases::Key key;
                    key.definition = item.key;
                    key.displayName = Localize(keyNode->second->Value("item_name"), keyNode->second->Value("name"));
                    key.image = std::string(keyNode->second->Value("image_inventory"));
                    keys.emplace(item.key, std::move(key));
                }
            }
        }

        if (item.category == Cases::Category::Weapon && item.key == 0)
            item.category = Cases::Category::Other;   // opens without a key and isn't a souvenir (Armory collections)

        item.hasGold = item.category == Cases::Category::Weapon && item.key != 0;

        // a terminal with ★ items in the built-in list (the Dead Hand Terminal's gloves) offers those too
        if (item.terminal)
            if (const auto rares = caseRares.find(item.definition); rares != caseRares.end() && !rares->second.empty())
                item.hasGold = true;

        if (item.hasGold)
        {
            const std::string footer = EnglishLower(english, inherited(node, "loot_list_rare_item_footer"));

            std::vector<const std::pair<std::string, std::uint16_t>*> named;
            for (const auto& knife : knifeNames)
                if (footer.find(knife.first) != std::string::npos)
                    named.push_back(&knife);

            for (const auto* knife : named)
            {
                // "bayonet" is also inside "m9 bayonet": only the longer name counts then
                const bool partOfLonger = std::any_of(named.begin(), named.end(), [&](const auto* other)
                    { return other != knife && other->first.size() > knife->first.size() && other->first.find(knife->first) != std::string::npos; });

                if (!partOfLonger)
                    item.goldTypes.push_back(knife->second);
            }

            if (footer.find("gloves") != std::string::npos)
                item.goldTypes.insert(item.goldTypes.end(), gloveTypes.begin(), gloveTypes.end());
        }

        // the case's own ★ items from the built-in list: exact, so they replace what the footer said
        if (item.hasGold)
        {
            if (const auto rares = caseRares.find(item.definition); rares != caseRares.end() && !rares->second.empty())
            {
                item.golds = rares->second;
                item.goldTypes.clear();

                for (const Cases::Drop& gold : item.golds)
                    if (std::find(item.goldTypes.begin(), item.goldTypes.end(), gold.weapon) == item.goldTypes.end())
                        item.goldTypes.push_back(gold.weapon);
            }
        }

        if (item.definition != 0)
            cases.push_back(std::move(item));
    }

    // ── the collections no container hands out (map collections like the Dust Collection, operation / Armory ones):
    // items_game has a loot list of the same name, its tiers as sub-lists - what trade-ups of their skins draw from ──
    std::vector<Cases::Case> setCollections;

    for (const auto& [setName, setNode] : itemSets)
    {
        if (!lootLists.contains(setName))
            continue;

        drops.clear();
        visited.clear();
        statTrakList = false;
        expand(setName, -1, 0);

        std::erase_if(drops, [&](const Cases::Drop& drop) { return dropKind(drop) != Cases::DropKind::Skin; });

        if (drops.empty())
            continue;

        std::stable_sort(drops.begin(), drops.end(), [](const Cases::Drop& a, const Cases::Drop& b) { return a.tier < b.tier; });

        Cases::Case collection;
        collection.name = std::string(setName);
        collection.displayName = Localize(setNode->Value("name"), collection.name);
        collection.sortName = Lower(collection.displayName);
        collection.drops = drops;
        collection.category = Cases::Category::Other;
        setCollections.push_back(std::move(collection));
    }

    // ── the Armory (CS2's XP shop): the season whose "redeemable_goods" is "xpshop", its rewards in the game's order (what
    // MissionsAPI.ActionRedeemOperationGoods(season, index, stars) redeems) - a case as it is, or a loot list to roll from ──
    std::vector<Cases::ArmoryReward> armory;
    int armorySeason = 0;

    for (const Node& block : seasonBlocks)
    {
        for (const Node& season : block.children)
        {
            if (!season.block || season.Value("redeemable_goods") != "xpshop")
                continue;

            armory.clear();
            armorySeason = ToInt(season.key);

            for (const Node& good : season.children)
            {
                if (!good.block || good.key != "operational_point_redeemable")
                    continue;

                Cases::ArmoryReward reward;
                reward.itemName = std::string(good.Value("item_name"));
                reward.points = ToInt(good.Value("points"));
                reward.bid = !good.Value("bidding_cycle").empty();
                reward.displayName = Localize(good.Value("callout"), reward.itemName);

                if (reward.itemName.starts_with("lootlist:"))
                {
                    drops.clear();
                    visited.clear();
                    statTrakList = false;
                    withCharms = true;
                    expand(std::string_view(reward.itemName).substr(9), -1, 0);
                    withCharms = false;

                    std::stable_sort(drops.begin(), drops.end(), [](const Cases::Drop& a, const Cases::Drop& b) { return a.tier < b.tier; });
                    reward.pool.name = reward.itemName.substr(9);
                    reward.pool.displayName = reward.displayName;
                    reward.pool.drops = drops;
                    reward.pool.category = Cases::Category::Other;
                }
                else if (const auto named = itemByName.find(reward.itemName); named != itemByName.end())
                {
                    reward.crate = static_cast<std::uint16_t>(named->second.first);   // a case: given as it is (no key)
                }

                armory.push_back(std::move(reward));
            }
        }
    }

    // ── everything there is, for the Add items tab (also known to FindKit, so names / pictures work everywhere) ──
    std::vector<Cases::Kit> allKits[6];

    for (const Node& block : stickerBlocks)
    {
        for (const Node& node : block.children)
        {
            const int id = node.block ? ToInt(node.key) : 0;
            if (id <= 0)
                continue;

            const std::string_view itemName = node.Value("item_name");
            const std::string_view patchMaterial = node.Value("patch_material");
            const std::string_view stickerMaterial = node.Value("sticker_material");

            Cases::DropKind kind;
            std::string image;

            if (!patchMaterial.empty())
            {
                kind = Cases::DropKind::Patch;
                image = "econ/patches/" + std::string(patchMaterial);
            }
            else if (!stickerMaterial.empty())
            {
                kind = itemName.starts_with("#SprayKit") ? Cases::DropKind::Graffiti : Cases::DropKind::Sticker;
                image = "econ/stickers/" + std::string(stickerMaterial);
            }
            else
            {
                continue;
            }

            const int tier = TierFromRarity(node.Value("item_rarity"));
            Cases::Kit kit{ id, kind, Localize(itemName, node.Value("name")), image, EnglishLower(english, itemName) };
            kit.tier = tier >= 0 ? tier : Cases::TierMilSpec;
            kit.tintable = kind == Cases::DropKind::Graffiti && node.Value("name").starts_with("spray_std");

            kits.try_emplace(KitKey(kind, id), kit);
            allKits[static_cast<int>(kind)].push_back(std::move(kit));
        }
    }

    for (const Node& block : musicBlocks)
    {
        for (const Node& node : block.children)
        {
            const int id = node.block ? ToInt(node.key) : 0;
            if (id <= 0)
                continue;

            Cases::Kit kit{ id, Cases::DropKind::MusicKit, Localize(node.Value("loc_name"), node.Value("name")), std::string(node.Value("image_inventory")),
                EnglishLower(english, node.Value("loc_name")) };
            kits.try_emplace(KitKey(Cases::DropKind::MusicKit, id), kit);
            allKits[static_cast<int>(Cases::DropKind::MusicKit)].push_back(std::move(kit));
        }
    }

    // pictures of every item; collectibles (pins, coins, medals, trophies) as def-based "Pin" kits
    std::unordered_map<std::uint16_t, std::string> itemImages;
    std::unordered_map<std::uint16_t, int> itemTiers;

    for (const auto& [id, node] : items)
    {
        const int definition = ToInt(id);
        if (definition <= 0 || definition > 0xFFFF)
            continue;

        if (const std::string_view image = inherited(node, "image_inventory"); !image.empty())
            itemImages.emplace(static_cast<std::uint16_t>(definition), std::string(image));

        if (const int tier = TierFromRarity(inherited(node, "item_rarity")); tier >= 0)
            itemTiers.emplace(static_cast<std::uint16_t>(definition), tier);
    }

    for (const ItemDefinitionInfo& item : Skins::g_ItemSchema.GetItems())
    {
        if (item.typeName != "#CSGO_Type_Collectible")
            continue;

        // placeholders without a name in the game files ("item 6046" ...): not real items
        const std::string& shownName = item.displayName.empty() ? item.itemName : item.displayName;
        if (shownName.empty() || (shownName.rfind("item ", 0) == 0 &&
            shownName.find_first_not_of("0123456789", 5) == std::string::npos && shownName.size() > 5))
            continue;

        const auto image = itemImages.find(item.definitionIndex);
        const auto node = items.find(std::to_string(item.definitionIndex));

        Cases::Kit kit{ item.definitionIndex, Cases::DropKind::Pin, item.displayName.empty() ? item.itemName : item.displayName,
            image != itemImages.end() ? image->second : std::string(),
            node != items.end() ? EnglishLower(english, node->second->Value("item_name")) : std::string() };
        kit.tier = node != items.end() && TierFromRarity(inherited(node->second, "item_rarity")) >= 0 ?
            TierFromRarity(inherited(node->second, "item_rarity")) : Cases::TierMilSpec;

        kits.try_emplace(KitKey(Cases::DropKind::Pin, item.definitionIndex), kit);
        allKits[static_cast<int>(Cases::DropKind::Pin)].push_back(std::move(kit));
    }

    for (std::vector<Cases::Kit>& list : allKits)
    {
        std::sort(list.begin(), list.end(), [](const Cases::Kit& a, const Cases::Kit& b) { return a.displayName < b.displayName; });
        list.erase(std::unique(list.begin(), list.end(), [](const Cases::Kit& a, const Cases::Kit& b) { return a.id == b.id && a.kind == b.kind; }), list.end());
    }

    // newest first
    std::sort(cases.begin(), cases.end(), [](const Cases::Case& a, const Cases::Case& b) { return a.definition > b.definition; });

    g_cases = std::move(cases);
    g_setCollections = std::move(setCollections);
    g_armory = std::move(armory);
    g_armorySeason = armorySeason;
    g_kits = std::move(kits);
    for (int i = 0; i < 6; ++i)
        g_allKits[i] = std::move(allKits[i]);
    g_itemImages = std::move(itemImages);
    g_itemTiers = std::move(itemTiers);
    g_wearRanges = std::move(wearRanges);
    g_souvenirEvents = std::move(souvenirEvents);
    g_englishWeapons = std::move(englishWeapons);
    g_englishFinishes = std::move(englishFinishes);
    g_englishItems = std::move(englishItems);
    g_englishCharms = std::move(englishCharms);
    g_highlights = std::move(highlights);
    g_highlightByNumber = std::move(highlightByNumber);
    g_highlightOfCharm = std::move(highlightOfCharm);

    // per event, in Valve's order (by match); built on the final list (the pointers stay valid: it's never changed again)
    std::map<int, std::map<int, const Cases::Highlight*>> clipsByEvent;
    for (const Cases::Highlight& highlight : g_highlights)
        clipsByEvent[highlight.event][highlight.number] = &highlight;

    for (const auto& [event, clips] : clipsByEvent)
    {
        std::vector<const Cases::Highlight*>& list = g_clipsByEvent[event];

        for (const auto& [number, clip] : clips)
        {
            list.push_back(clip);
            g_eventOfCharm[clip->keychain] = event;
        }

        g_eventOfCharm[event] = event;
    }
    g_keys.clear();
    for (auto& [id, key] : keys)
        g_keys.push_back(std::move(key));

    g_ready.store(true, std::memory_order_release);
    return true;
}

bool Cases::Ready()
{
    return g_ready.load(std::memory_order_acquire);
}

const std::vector<Cases::Case>& Cases::All()
{
    return g_cases;
}

const Cases::Case* Cases::Find(std::uint16_t definition)
{
    for (const Case& item : g_cases)
    {
        if (item.definition == definition)
            return &item;
    }

    return nullptr;
}

const Cases::Key* Cases::FindKey(std::uint16_t definition)
{
    for (const Key& key : g_keys)
    {
        if (key.definition == definition)
            return &key;
    }

    return nullptr;
}

const std::vector<Cases::Case>& Cases::SetCollections()
{
    return Ready() ? g_setCollections : g_noCases;
}

const std::vector<Cases::ArmoryReward>& Cases::ArmoryRewards()
{
    return Ready() ? g_armory : g_noArmory;
}

int Cases::ArmorySeason()
{
    return Ready() ? g_armorySeason : 0;
}

bool Cases::RollSouvenirStickers(int event, double r1, double r2, double r3, int (&kits)[4])
{
    kits[0] = kits[1] = kits[2] = kits[3] = 0;

    if (!Ready() || event <= 0)
        return false;

    const auto found = g_souvenirEvents.find(event);
    if (found == g_souvenirEvents.end() || found->second.teams.size() < 2)
        return false;

    const std::vector<SouvenirTeam>& teams = found->second.teams;
    const auto pick = [](double r, std::size_t count) { return static_cast<std::size_t>(std::clamp(r, 0.0, 1.0) * count) % count; };

    // two different teams: the second one is drawn from the others
    const std::size_t first = pick(r1, teams.size());
    std::size_t second = pick(r2, teams.size() - 1);
    if (second >= first)
        ++second;

    // the MVP: a player of either team (the one with autographs, if only one has them)
    std::vector<int> players = teams[first].players;
    players.insert(players.end(), teams[second].players.begin(), teams[second].players.end());
    const int mvp = players.empty() ? 0 : players[pick(r3, players.size())];

    // in order, without gaps (Cologne 2015 - Cluj 2015 have no organizer sticker, the oldest Majors no autographs)
    int slot = 0;
    for (const int kit : { found->second.organizer, teams[first].kit, teams[second].kit, mvp })
        if (kit > 0)
            kits[slot++] = kit;

    return slot > 0;
}

const Cases::Case* Cases::SetCollectionWith(std::uint16_t weapon, int paintKit)
{
    if (!Ready())
        return nullptr;

    for (const Case& collection : g_setCollections)
    {
        for (const Drop& drop : collection.drops)
        {
            if (drop.weapon == weapon && drop.paintKit == paintKit)
                return &collection;
        }
    }

    return nullptr;
}

Cases::WearRange Cases::WearRangeOf(int paintKit)
{
    if (paintKit <= 0 || !Ready())
        return {};

    const auto it = g_wearRanges.find(paintKit);
    return it != g_wearRanges.end() ? it->second : WearRange{};
}

namespace
{
    // items_game "graffiti_tints" (id 1-19), stable since 2016
    struct Tint
    {
        const char* name;
        std::uint32_t rgb;
    };

    constexpr Tint TINTS[Cases::TINT_COUNT] = {
        { "Brick Red", 0x874444 }, { "Blood Red", 0xB14D4D }, { "Tiger Orange", 0xB87148 }, { "Dust Brown", 0x8F7D5D },
        { "Desert Amber", 0xAE833D }, { "Tracer Yellow", 0xD4C95B }, { "Battle Green", 0x789D53 }, { "Jungle Green", 0x417A4A },
        { "Frog Green", 0x488F80 }, { "Cash Green", 0xA6C4A5 }, { "Wire Blue", 0x6BA5B2 }, { "Monarch Blue", 0x4E7FA9 },
        { "SWAT Blue", 0x4C5B98 }, { "Violent Violet", 0xAF92DF }, { "Monster Purple", 0x6E4F9F }, { "Bazooka Pink", 0xBA68B2 },
        { "Princess Pink", 0x9D567A }, { "War Pig Pink", 0xE4CCD5 }, { "Shark White", 0xC1C1C1 },
    };
}

const char* Cases::TintName(int tint)
{
    return tint >= 1 && tint <= TINT_COUNT ? TINTS[tint - 1].name : "";
}

std::uint32_t Cases::TintRgb(int tint)
{
    return tint >= 1 && tint <= TINT_COUNT ? TINTS[tint - 1].rgb : 0xFFFFFFu;
}

Cases::DropKind Cases::KindOf(std::uint16_t definition)
{
    switch (definition)
    {
    case DEF_STICKER: return DropKind::Sticker;
    case DEF_PATCH: return DropKind::Patch;
    case DEF_GRAFFITI:
    case DEF_SPRAY_PAINT: return DropKind::Graffiti;
    case DEF_MUSIC_KIT: return DropKind::MusicKit;
    default: return FindKit(DropKind::Pin, definition) ? DropKind::Pin : DropKind::Skin;
    }
}

const std::vector<Cases::Highlight>& Cases::Highlights()
{
    return g_ready.load(std::memory_order_acquire) ? g_highlights : g_noHighlights;
}

const Cases::Highlight* Cases::FindHighlight(int number)
{
    if (!g_ready.load(std::memory_order_acquire) || number <= 0)
        return nullptr;

    const auto it = g_highlightByNumber.find(number);
    return it != g_highlightByNumber.end() ? &g_highlights[it->second] : nullptr;
}

const std::vector<const Cases::Highlight*>& Cases::HighlightsOf(int eventCharm)
{
    if (!g_ready.load(std::memory_order_acquire))
        return g_noClips;

    const auto it = g_clipsByEvent.find(eventCharm);
    return it != g_clipsByEvent.end() ? it->second : g_noClips;
}

int Cases::HighlightEvent(int keychain)
{
    if (!g_ready.load(std::memory_order_acquire))
        return 0;

    const auto it = g_eventOfCharm.find(keychain);
    return it != g_eventOfCharm.end() ? it->second : 0;
}

int Cases::HighlightOf(int keychain)
{
    if (!g_ready.load(std::memory_order_acquire))
        return 0;

    const auto it = g_highlightOfCharm.find(keychain);
    return it != g_highlightOfCharm.end() ? it->second : 0;
}

const std::vector<Cases::Kit>& Cases::AllKits(DropKind kind)
{
    const int index = static_cast<int>(kind);
    return g_ready.load(std::memory_order_acquire) && index >= 0 && index < 6 ? g_allKits[index] : g_noKits;
}

const std::string& Cases::ItemImage(std::uint16_t definition)
{
    if (!g_ready.load(std::memory_order_acquire))
        return g_noImage;

    const auto it = g_itemImages.find(definition);
    return it != g_itemImages.end() ? it->second : g_noImage;
}

namespace
{
    std::once_flag g_extraImagesOnce;
    std::unordered_map<std::string, Cases::EmbeddedImage> g_extraImages;   // written once (g_extraImagesOnce)

    // data\extra_images.bin (compiled in by prices.rc): "LLIMG1", then per picture [u16 length][image_inventory path]
    // [u32 size][PNG], little endian (tools\make_extra_images.py).
    void ReadExtraImages()
    {
        const HMODULE self = SelfModule();
        const HRSRC info = self ? FindResourceW(self, MAKEINTRESOURCEW(IDR_EXTRA_IMAGES_BIN), MAKEINTRESOURCEW(10) /* RT_RCDATA */) : nullptr;
        const HGLOBAL data = info ? LoadResource(self, info) : nullptr;
        const auto* bytes = data ? static_cast<const std::uint8_t*>(LockResource(data)) : nullptr;
        const std::size_t size = info ? SizeofResource(self, info) : 0;

        if (!bytes || size < 6 || std::memcmp(bytes, "LLIMG1", 6) != 0)
            return;

        std::size_t at = 6;

        while (at + 2 <= size)
        {
            std::uint16_t length = 0;
            std::memcpy(&length, bytes + at, sizeof(length));
            at += sizeof(length);

            if (length == 0 || length > size - at || size - at - length < 4)
                break;

            std::string path(reinterpret_cast<const char*>(bytes + at), length);
            at += length;

            std::uint32_t pngSize = 0;
            std::memcpy(&pngSize, bytes + at, sizeof(pngSize));
            at += sizeof(pngSize);

            if (pngSize == 0 || pngSize > size - at)
                break;

            g_extraImages.emplace(std::move(path), Cases::EmbeddedImage{ bytes + at, pngSize });
            at += pngSize;
        }
    }
}

Cases::EmbeddedImage Cases::ExtraImage(const std::string& image)
{
    if (image.empty())
        return {};

    std::call_once(g_extraImagesOnce, ReadExtraImages);
    const auto it = g_extraImages.find(image);
    return it != g_extraImages.end() ? it->second : EmbeddedImage{};
}

int Cases::ItemTier(std::uint16_t definition)
{
    if (!g_ready.load(std::memory_order_acquire))
        return -1;

    const auto it = g_itemTiers.find(definition);
    return it != g_itemTiers.end() ? it->second : -1;
}

const Cases::Kit* Cases::FindKit(DropKind kind, int id)
{
    if (!g_ready.load(std::memory_order_acquire))
        return nullptr;

    const auto it = g_kits.find(KitKey(kind, id));
    return it != g_kits.end() ? &it->second : nullptr;
}

std::string Cases::MarketName(std::uint16_t definition, int paintKit, float wear, bool statTrak, bool souvenir)
{
    if (!g_ready.load(std::memory_order_acquire))
        return {};

    // a charm: "charm | ..." / "souvenir charm | <event> highlight | <clip>"
    if (definition == DEF_KEYCHAIN)
    {
        const auto charm = g_englishCharms.find(paintKit);
        return charm != g_englishCharms.end() ? charm->second : std::string();
    }

    const DropKind kind = KindOf(definition);
    const char* const statTrakText = statTrak ? "stattrak\xE2\x84\xA2 " : "";   // ™ (UTF-8, as the market writes it)

    if (kind != DropKind::Skin)
    {
        const Kit* kit = FindKit(kind, kind == DropKind::Pin ? definition : paintKit);

        if (!kit || kit->english.empty())
            return {};

        switch (kind)
        {
        case DropKind::Sticker: return "sticker | " + kit->english;
        case DropKind::Patch: return "patch | " + kit->english;
        case DropKind::Graffiti: return definition == DEF_SPRAY_PAINT ? std::string() : "sealed graffiti | " + kit->english;   // unsealed: can't be sold
        case DropKind::MusicKit: return std::string(statTrakText) + "music kit | " + kit->english;
        default: return kit->english;   // pins: "guardian pin"
        }
    }

    const auto weapon = g_englishWeapons.find(definition);
    if (weapon == g_englishWeapons.end())
    {
        // anything else is sold by its own name: an agent ("'the doctor' romanov | sabre"), a case, a key, a package
        const auto other = g_englishItems.find(definition);
        return other != g_englishItems.end() && paintKit <= 0 ? other->second : std::string();
    }

    const ItemDefinitionInfo* item = Skins::g_ItemSchema.GetItem(definition);
    const bool gold = item && (item->isKnife || item->isGlove);
    std::string name = gold ? "\xE2\x98\x85 " : "";   // ★
    name += statTrakText;
    if (souvenir && !statTrak && !gold)
        name += "souvenir ";
    name += weapon->second;

    if (paintKit <= 0)
        return gold ? name : std::string{};   // a vanilla knife: "★ karambit"

    const auto finish = g_englishFinishes.find(paintKit);
    if (finish == g_englishFinishes.end())
        return {};

    const char* wearName = wear < 0.07f ? "factory new" : wear < 0.15f ? "minimal wear" : wear < 0.38f ? "field-tested" :
        wear < 0.45f ? "well-worn" : "battle-scarred";

    return name + " | " + finish->second + " (" + wearName + ")";
}

const char* Cases::CategoryName(Category category)
{
    switch (category)
    {
    case Category::Weapon: return "Cases";
    case Category::Souvenir: return "Souvenirs";
    case Category::Stickers: return "Stickers";
    case Category::Patches: return "Patches";
    case Category::Graffiti: return "Graffiti";
    case Category::MusicKits: return "Music Kits";
    case Category::Pins: return "Pins";
    default: return "Other";
    }
}

const char* Cases::CategoryNoun(Category category)
{
    switch (category)
    {
    case Category::Stickers: return "stickers";
    case Category::Patches: return "patches";
    case Category::Graffiti: return "graffiti";
    case Category::MusicKits: return "music kits";
    case Category::Pins: return "pins";
    default: return "skins";
    }
}

bool Cases::GoldTypeFrom(const Case& item, std::uint16_t weapon)
{
    return std::find(item.goldTypes.begin(), item.goldTypes.end(), weapon) != item.goldTypes.end();
}

bool Cases::GoldFrom(const Case& item, std::uint16_t weapon, int paintKit)
{
    return std::any_of(item.golds.begin(), item.golds.end(), [&](const Drop& gold) { return gold.weapon == weapon && gold.paintKit == paintKit; });
}

int Cases::TierIn(const Case& item, std::uint16_t weapon, int paintKit)
{
    for (const Drop& drop : item.drops)
    {
        if (drop.weapon == weapon && drop.paintKit == paintKit)
            return drop.tier;
    }

    // its exact ★ list when it's known, else any finish of a knife / gloves type it names
    if (!item.golds.empty())
        return GoldFrom(item, weapon, paintKit) ? TierGold : -1;

    return GoldTypeFrom(item, weapon) ? TierGold : -1;
}

bool Cases::DropsFrom(const Case& item, std::uint16_t weapon, int paintKit)
{
    return TierIn(item, weapon, paintKit) >= 0;
}

int Cases::Rarity(Tier tier)
{
    switch (tier)
    {
    case TierConsumer: return 1;
    case TierIndustrial: return 2;
    case TierMilSpec: return 3;
    case TierRestricted: return 4;
    case TierClassified: return 5;
    case TierCovert: return 6;
    case TierGold: return 6;
    default: return 3;
    }
}

const char* Cases::TierName(Tier tier)
{
    switch (tier)
    {
    case TierConsumer: return "Consumer Grade";
    case TierIndustrial: return "Industrial Grade";
    case TierMilSpec: return "Mil-Spec";
    case TierRestricted: return "Restricted";
    case TierClassified: return "Classified";
    case TierCovert: return "Covert";
    case TierGold: return "Rare Special Item";
    default: return "";
    }
}

namespace
{
    // Relative weight of every tier the case has (each tier 5x rarer, a ★ 0.26 / 79.92 of the lowest).
    double Weights(const Cases::Case& item, double weights[Cases::TierCount])
    {
        bool present[Cases::TierCount]{};
        int lowest = Cases::TierCount;

        for (const Cases::Drop& drop : item.drops)
        {
            present[drop.tier] = true;
            lowest = std::min(lowest, static_cast<int>(drop.tier));
        }

        for (int tier = 0; tier < Cases::TierCount; ++tier)
            weights[tier] = 0.0;

        if (lowest == Cases::TierCount)
            return 0.0;

        double total = 0.0;

        for (int tier = lowest; tier < Cases::TierGold; ++tier)
        {
            if (!present[tier])
                continue;

            double weight = 1.0;
            for (int step = lowest; step < tier; ++step)
                weight /= 5.0;

            weights[tier] = weight;
            total += weight;
        }

        if (item.hasGold)
        {
            weights[Cases::TierGold] = weights[lowest] * (0.26 / 79.92);
            total += weights[Cases::TierGold];
        }

        return total;
    }
}

void Cases::TierOdds(const Case& item, double percent[TierCount])
{
    double weights[TierCount];
    const double total = Weights(item, weights);

    for (int tier = 0; tier < TierCount; ++tier)
        percent[tier] = total > 0.0 ? weights[tier] / total * 100.0 : 0.0;
}

Cases::Tier Cases::RollTier(const Case& item, double random01)
{
    bool present[TierCount]{};
    int lowest = TierCount;

    for (const Drop& drop : item.drops)
    {
        present[drop.tier] = true;
        lowest = std::min(lowest, static_cast<int>(drop.tier));
    }

    if (lowest == TierCount)
        return TierMilSpec;

    double weights[TierCount]{};
    double total = 0.0;

    for (int tier = lowest; tier < TierGold; ++tier)
    {
        if (!present[tier])
            continue;

        double weight = 1.0;
        for (int step = lowest; step < tier; ++step)
            weight /= 5.0;

        weights[tier] = weight;
        total += weight;
    }

    // a ★ is 0.26 % of a weapon case = 0.26 / 79.92 of the Mil-Spec weight
    if (item.hasGold)
    {
        weights[TierGold] = weights[lowest] * (0.26 / 79.92);
        total += weights[TierGold];
    }

    double pick = std::clamp(random01, 0.0, 0.999999) * total;

    for (int tier = 0; tier < TierCount; ++tier)
    {
        if (weights[tier] <= 0.0)
            continue;

        if (pick < weights[tier])
            return static_cast<Tier>(tier);

        pick -= weights[tier];
    }

    return static_cast<Tier>(lowest);
}
