#include "item_schema.h"

#include <algorithm>
#include <atomic>
#include <charconv>
#include <cctype>
#include <cstddef>
#include <cstring>
#include <map>
#include <mutex>
#include <unordered_map>
#include <string_view>
#include <utility>
#include <vector>
#include <excpt.h>
#include <Windows.h>

#include "../../interfaces/interfaces.h"
#include "../../../patterns.h"
#include "../../utils/memory/patternscan/patternscan.h"


namespace
{
    constexpr int MAX_REASONABLE_ITEM_DEFINITIONS = 10000;
    constexpr int MAX_REASONABLE_PAINT_KITS = 10000;
    constexpr std::uint64_t MAX_ITEMS_GAME_FILE_SIZE = 64ULL * 1024ULL * 1024ULL;

    bool TryCopyGameString(const char* source, char* destination, std::size_t capacity)
    {
        if (!destination || capacity == 0)
            return false;

        destination[0] = '\0';

        if (!source)
            return true;

        __try
        {
            for (std::size_t i = 0; i + 1 < capacity; ++i)
            {
                const char c = source[i];
                destination[i] = c;

                if (c == '\0')
                    return true;
            }

            destination[capacity - 1] = '\0';
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            destination[0] = '\0';
            return false;
        }
    }

    bool TryReadGameString(const char* source, std::string& output)
    {
        char buffer[2048]{};

        if (!TryCopyGameString(source, buffer, sizeof(buffer)))
        {
            output.clear();
            return false;
        }

        output.assign(buffer);
        return true;
    }

    struct ItemDefinitionSnapshot
    {
        std::uint16_t definitionIndex = 0;
        const char* itemBaseName = nullptr;
        const char* itemTypeName = nullptr;
        const char* modelName = nullptr;
        const char* simpleWeaponName = nullptr;
        const char* weaponName = nullptr;
        const char* itemName = nullptr;
        bool isKnife = false;
        bool isGlove = false;
        bool isAgent = false;
    };

    // The schema's paint kit map moves between CS2 builds (+0x2F0 up to 14186, +0x2A0 on 14190). The offset comes from
    // the game's own lookup: GetPaintKit (Patterns::GloveCreatePaintKit) ends "mov edx, esi / mov rbx, [rsp+x] /
    // add rsp, x / pop rsi / jmp <schema lookup>", and that lookup's "cmp r8d, [rbx + disp32]" reads the map's count =
    // where the map starts. Not found: CEconItemSchema::m_PaintKits (the last known one).
    std::atomic<std::uint32_t> g_paintKitMapOffset{ 0 };

    std::uint32_t ReadPaintKitMapOffset()
    {
        const auto* getPaintKit = M::FindPattern("client.dll", Patterns::GloveCreatePaintKit);

        if (!getPaintKit)
            return 0;

        for (int i = 0; i < 0xC0; ++i)
        {
            const std::uint8_t* p = getPaintKit + i;

            if (p[0] != 0x8B || p[1] != 0xD6 || p[2] != 0x48 || p[3] != 0x8B || p[4] != 0x5C || p[5] != 0x24 ||
                p[7] != 0x48 || p[8] != 0x83 || p[9] != 0xC4 || p[11] != 0x5E || p[12] != 0xE9)
                continue;

            const std::uint8_t* lookup = p + 17 + *reinterpret_cast<const std::int32_t*>(p + 13);

            for (int j = 0; j < 0x40; ++j)
            {
                if (lookup[j] == 0x44 && lookup[j + 1] == 0x3B && lookup[j + 2] == 0x83)   // cmp r8d, [rbx + disp32]
                {
                    const std::uint32_t offset = *reinterpret_cast<const std::uint32_t*>(lookup + j + 3);
                    return offset >= 0x100 && offset < 0x1000 ? offset : 0;
                }
            }

            return 0;
        }

        return 0;
    }

    std::uint32_t PaintKitMapOffset()
    {
        if (const std::uint32_t known = g_paintKitMapOffset.load(std::memory_order_acquire))
            return known;

        std::uint32_t offset = 0;

        __try
        {
            offset = ReadPaintKitMapOffset();
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            offset = 0;
        }

        if (!offset)
            offset = static_cast<std::uint32_t>(offsetof(CEconItemSchema, m_PaintKits));

        g_paintKitMapOffset.store(offset, std::memory_order_release);
        return offset;
    }

    C_EconUtlMap<int, CPaintKit*>& PaintKitMap(CEconItemSchema* schema)
    {
        return *reinterpret_cast<C_EconUtlMap<int, CPaintKit*>*>(reinterpret_cast<std::uint8_t*>(schema) + PaintKitMapOffset());
    }

    bool TryReadItemDefinition(CEconItemDefinition* definition, ItemDefinitionSnapshot& snapshot)
    {
        if (!definition)
            return false;

        __try
        {
            snapshot.definitionIndex = definition->m_DefinitionIndex;
            snapshot.itemBaseName = definition->m_ItemBaseName;
            snapshot.itemTypeName = definition->m_ItemTypeName;
            snapshot.modelName = definition->GetModelName();
            snapshot.simpleWeaponName = definition->GetSimpleWeaponName();
            snapshot.weaponName = definition->GetWeaponName();
            snapshot.itemName = definition->GetItemName();
            snapshot.isKnife = definition->IsKnife(false);
            snapshot.isGlove = definition->IsGlove(false);
            snapshot.isAgent = definition->IsAgent();
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    struct PaintKitSnapshot
    {
        int id = 0;
        int rarity = 0;
        const char* name = nullptr;
        const char* descriptionTag = nullptr;
        bool usesOldModel = false;
    };

    bool TryReadPaintKit(CPaintKit* paintKit, PaintKitSnapshot& snapshot)
    {
        if (!paintKit)
            return false;

        __try
        {
            snapshot.id = paintKit->m_ID;
            snapshot.rarity = paintKit->m_Rarity;
            snapshot.name = paintKit->m_Name;
            snapshot.descriptionTag = paintKit->m_DescriptionTag;
            snapshot.usesOldModel = paintKit->UsesOldModel();
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    bool TryWritePaintKitName(CPaintKit* paintKit, const char* name)
    {
        if (!paintKit || !name)
            return false;

        __try
        {
            paintKit->m_Name = name;
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    std::mutex g_permanentNamesMutex;
    std::unordered_map<std::string, const char*> g_permanentNames;   // the copies themselves are never freed

    bool IsReasonableMap(int count, const void* elements, int maxCount)
    {
        return count > 0 && count <= maxCount && elements;
    }

    bool IsNormalWeaponType(std::string_view typeName)
    {
        return
            typeName.find("Pistol") != std::string_view::npos ||
            typeName.find("Rifle") != std::string_view::npos ||
            typeName.find("SMG") != std::string_view::npos ||
            typeName.find("Sniper") != std::string_view::npos ||
            typeName.find("Shotgun") != std::string_view::npos ||
            typeName.find("Machine") != std::string_view::npos ||
            typeName.find("SubMachinegun") != std::string_view::npos;
    }

    std::string LocalizeToken(const std::string& token)
    {
        if (!I::Localize || token.empty())
            return {};

        const char* lookup = token.c_str();

        if (*lookup == '#')
            ++lookup;

        const char* localized = I::Localize->FindSafe(lookup);
        std::string localizedText;

        if (!TryReadGameString(localized, localizedText) || localizedText.empty())
            return {};

        if (localizedText == lookup || localizedText == token)
            return {};

        return localizedText;
    }

    std::string HumanizeName(std::string value)
    {
        for (char& c : value)
        {
            if (c == '_' || c == '-')
                c = ' ';
        }

        bool upperNext = true;

        for (char& c : value)
        {
            if (c == ' ')
            {
                upperNext = true;
                continue;
            }

            if (upperNext)
            {
                c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
                upperNext = false;
            }
        }

        return value.empty() ? std::string("Unknown") : value;
    }

    std::string LowercaseAscii(std::string value)
    {
        std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c)
        {
            return static_cast<char>(std::tolower(c));
        });

        return value;
    }

    bool GameModelExists(const std::string& modelName)
    {
        if (!I::FileSystem || modelName.empty())
            return false;

        std::string compiledPath = modelName;
        compiledPath += "_c";
        return I::FileSystem->Exists(compiledPath.c_str(), "GAME");
    }

    std::string GetAgentName(const std::string& itemName)
    {
        constexpr const char* CUSTOMPLAYER_PREFIX = "customplayer_";

        if (itemName.rfind(CUSTOMPLAYER_PREFIX, 0) != 0)
            return {};

        return itemName.substr(std::strlen(CUSTOMPLAYER_PREFIX));
    }

    std::string GetAgentFamilyName(const std::string& itemName, const std::string& fallbackModelName)
    {
        static constexpr const char* families[] =
        {
            "tm_jungle_raider", "tm_phoenix_heavy", "tm_professional", "tm_separatist",
            "tm_anarchist", "tm_jumpsuit", "tm_phoenix", "tm_balkan", "tm_pirate", "tm_leet",
            "ctm_gendarmerie", "ctm_heavy", "ctm_diver", "ctm_gign", "ctm_gsg9",
            "ctm_swat", "ctm_sas", "ctm_st6", "ctm_fbi", "ctm_idf"
        };

        const std::string agentName = GetAgentName(itemName);

        for (const char* family : families)
        {
            const std::size_t length = std::strlen(family);

            if (agentName.compare(0, length, family) == 0)
                return family;
        }

        const std::size_t lastSlash = fallbackModelName.find_last_of("/\\");

        if (lastSlash != std::string::npos && lastSlash > 0)
        {
            const std::size_t parentSlash = fallbackModelName.find_last_of("/\\", lastSlash - 1);
            const std::size_t begin = parentSlash == std::string::npos ? 0 : parentSlash + 1;

            if (lastSlash > begin)
            {
                const std::string family = fallbackModelName.substr(begin, lastSlash - begin);

                if (family.rfind("tm_", 0) == 0 || family.rfind("ctm_", 0) == 0)
                    return family;
            }
        }

        return {};
    }

    std::string GetModelStem(const std::string& modelName)
    {
        if (modelName.empty())
            return {};

        const std::size_t slash = modelName.find_last_of("/\\");
        const std::size_t begin = slash == std::string::npos ? 0 : slash + 1;
        std::string stem = modelName.substr(begin);

        constexpr const char* extension = ".vmdl";
        constexpr std::size_t extensionLength = 5;

        if (stem.size() >= extensionLength && stem.compare(stem.size() - extensionLength, extensionLength, extension) == 0)
            stem.resize(stem.size() - extensionLength);

        return LowercaseAscii(std::move(stem));
    }

    bool IsDefaultAgentEntry(const std::string& displayName, const std::string& itemName)
    {
        if (itemName == "customplayer_t_map_based" || itemName == "customplayer_ct_map_based")
            return true;

        const std::string name = LowercaseAscii(displayName);
        return name == "default t agent" || name == "default ct agent" || name == "default agent";
    }

    struct AgentModelResolution
    {
        std::string modelName;
        bool usable = false;
    };

    AgentModelResolution ResolveAgentModel(const std::string& itemName, const std::string& fallbackModelName, const std::string& displayName)
    {
        AgentModelResolution result;
        const std::string agentName = GetAgentName(itemName);

        if (agentName.empty())
        {
            if (GameModelExists(fallbackModelName))
            {
                result.modelName = fallbackModelName;
                result.usable = true;
            }

            return result;
        }

        // If the schema already points at this exact agent model, trust it first.
        if (GameModelExists(fallbackModelName) && GetModelStem(fallbackModelName) == LowercaseAscii(agentName))
        {
            result.modelName = fallbackModelName;
            result.usable = true;
            return result;
        }

        const std::string family = GetAgentFamilyName(itemName, fallbackModelName);

        if (!family.empty())
        {
            // Only models under agents/models are valid player-agent world models.
            // Legacy assets under characters/models can exist and even report as the
            // active model after SetModel(), while rendering the engine ERROR model.
            std::string agentModel = "agents/models/";
            agentModel.reserve(agentModel.size() + family.size() + agentName.size() + 7);
            agentModel += family;
            agentModel += "/";
            agentModel += agentName;
            agentModel += ".vmdl";

            if (GameModelExists(agentModel))
            {
                result.modelName = std::move(agentModel);
                result.usable = true;
                return result;
            }
        }

        // Paid/named agents already worked using their schema model. Keep that behaviour.
        // For legacy "Default T/CT Agent" definitions, never fall back to a generic faction
        // model such as tm_phoenix.vmdl, because that is what made dozens of distinct cards
        // all apply the same appearance.
        if (!IsDefaultAgentEntry(displayName, itemName) && GameModelExists(fallbackModelName))
        {
            result.modelName = fallbackModelName;
            result.usable = true;
        }

        return result;
    }

    bool ReadGameTextFile(const char* path, std::string& output)
    {
        output.clear();

        if (!I::FileSystem || !path || !*path)
            return false;

        void* handle = I::FileSystem->Open(path);

        if (!handle)
            return false;

        const std::uint64_t size = I::FileSystem->Size(handle);

        if (size == 0 || size > MAX_ITEMS_GAME_FILE_SIZE)
        {
            I::FileSystem->Close(handle);
            return false;
        }

        output.resize(static_cast<std::size_t>(size));
        const int bytesRead = I::FileSystem->Read(output.data(), static_cast<int>(size), handle);
        I::FileSystem->Close(handle);

        if (bytesRead != static_cast<int>(size))
        {
            output.clear();
            return false;
        }

        return true;
    }

    class KeyValuesTokenizer
    {
    public:
        explicit KeyValuesTokenizer(std::string_view input) : m_input(input) {}

        bool Next(std::string& token)
        {
            token.clear();
            SkipWhitespaceAndComments();

            if (m_position >= m_input.size())
                return false;

            const char c = m_input[m_position];

            if (c == '{' || c == '}')
            {
                token.assign(1, c);
                ++m_position;
                return true;
            }

            if (c == '"')
            {
                ++m_position;

                while (m_position < m_input.size())
                {
                    const char current = m_input[m_position++];

                    if (current == '"')
                        return true;

                    if (current == '\\' && m_position < m_input.size())
                    {
                        const char escaped = m_input[m_position++];

                        switch (escaped)
                        {
                        case 'n': token.push_back('\n'); break;
                        case 'r': token.push_back('\r'); break;
                        case 't': token.push_back('\t'); break;
                        default: token.push_back(escaped); break;
                        }

                        continue;
                    }

                    token.push_back(current);
                }

                return !token.empty();
            }

            const std::size_t start = m_position;

            while (m_position < m_input.size())
            {
                const char current = m_input[m_position];

                if (std::isspace(static_cast<unsigned char>(current)) || current == '{' || current == '}')
                    break;

                ++m_position;
            }

            if (m_position == start)
                return false;

            token.assign(m_input.substr(start, m_position - start));
            return true;
        }

    private:
        void SkipWhitespaceAndComments()
        {
            while (m_position < m_input.size())
            {
                while (m_position < m_input.size() && std::isspace(static_cast<unsigned char>(m_input[m_position])))
                    ++m_position;

                if (m_position + 1 >= m_input.size() || m_input[m_position] != '/')
                    return;

                if (m_input[m_position + 1] == '/')
                {
                    m_position += 2;

                    while (m_position < m_input.size() && m_input[m_position] != '\n')
                        ++m_position;

                    continue;
                }

                if (m_input[m_position + 1] == '*')
                {
                    m_position += 2;

                    while (m_position + 1 < m_input.size() &&
                        !(m_input[m_position] == '*' && m_input[m_position + 1] == '/'))
                    {
                        ++m_position;
                    }

                    if (m_position + 1 < m_input.size())
                        m_position += 2;

                    continue;
                }

                return;
            }
        }

        std::string_view m_input;
        std::size_t m_position = 0;
    };

    int ParseInt(const std::string& value)
    {
        int result = 0;
        const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), result);
        return error == std::errc{} && end == value.data() + value.size() ? result : 0;
    }
}

bool ItemSchema::Initialize()
{
    if (m_initialized)
        return true;

    Reset();

    m_source2Client = I::Source2Client;

    if (!m_source2Client || !I::FileSystem)
        return false;

    m_itemSystem = m_source2Client->GetEconItemSystem();

    if (!m_itemSystem)
    {
        Reset();
        return false;
    }

    m_schema = m_itemSystem->GetEconItemSchema();

    if (!m_schema)
    {
        Reset();
        return false;
    }

    auto& itemDefinitions = m_schema->GetSortedItemDefinitionMap();
    auto& paintKits = PaintKitMap(m_schema);

    if (!IsReasonableMap(itemDefinitions.Count(), itemDefinitions.m_Elements, MAX_REASONABLE_ITEM_DEFINITIONS) ||
        !IsReasonableMap(paintKits.Count(), paintKits.m_Elements, MAX_REASONABLE_PAINT_KITS))
    {
        Reset();
        return false;
    }

    if (!BuildItemList() || !BuildPaintKitList())
    {
        Reset();
        return false;
    }

    BuildItemsGameLists();

    // A finish whose name could be read neither from the game nor from items_game.txt can't be shown.
    std::erase_if(m_paintKits, [](const PaintKitInfo& paintKit) { return paintKit.name.empty(); });
    m_gamePaintKits.clear();

    std::sort(m_items.begin(), m_items.end(), [](const ItemDefinitionInfo& a, const ItemDefinitionInfo& b)
    {
        return a.definitionIndex < b.definitionIndex;
    });

    std::sort(m_paintKits.begin(), m_paintKits.end(), [](const PaintKitInfo& a, const PaintKitInfo& b)
    {
        return a.id < b.id;
    });

    std::sort(m_stickerKits.begin(), m_stickerKits.end(), [](const StickerKitInfo& a, const StickerKitInfo& b)
    {
        return a.id < b.id;
    });

    m_initialized = true;
    return true;
}

void ItemSchema::Reset()
{
    m_initialized = false;

    m_source2Client = nullptr;
    m_itemSystem = nullptr;
    m_schema = nullptr;

    m_items.clear();
    m_paintKits.clear();
    m_stickerKits.clear();
    m_musicKits.clear();
    m_keychains.clear();
    m_skinRarity.clear();
    m_itemPaintKits.clear();
    m_gamePaintKits.clear();
}

const MusicKitInfo* ItemSchema::GetMusicKit(int musicKitId) const
{
    for (const MusicKitInfo& kit : m_musicKits)
    {
        if (kit.id == musicKitId)
            return &kit;
    }

    return nullptr;
}

int ItemSchema::GetSkinRarity(const std::string& weaponName, const PaintKitInfo& paintKit) const
{
    if (const auto it = m_skinRarity.find(paintKit.name + "|" + weaponName); it != m_skinRarity.end())
        return it->second;

    return std::clamp(paintKit.rarity, 0, 7);
}

const KeychainInfo* ItemSchema::GetKeychain(int keychainId) const
{
    for (const KeychainInfo& keychain : m_keychains)
    {
        if (keychain.id == keychainId)
            return &keychain;
    }

    return nullptr;
}

bool ItemSchema::BuildItemList()
{
    if (!m_schema)
        return false;

    auto& map = m_schema->GetSortedItemDefinitionMap();

    if (!IsReasonableMap(map.Count(), map.m_Elements, MAX_REASONABLE_ITEM_DEFINITIONS))
        return false;

    m_items.reserve(map.Count());

    for (int i = 0; i < map.Count(); ++i)
    {
        const auto& node = map.Element(i);
        CEconItemDefinition* definition = node.m_Value;
        ItemDefinitionSnapshot snapshot{};

        if (!TryReadItemDefinition(definition, snapshot) || snapshot.definitionIndex == 0)
            continue;

        ItemDefinitionInfo info;
        info.definitionIndex = snapshot.definitionIndex;

        if (!TryReadGameString(snapshot.itemBaseName, info.baseName) ||
            !TryReadGameString(snapshot.itemTypeName, info.typeName) ||
            !TryReadGameString(snapshot.modelName, info.schemaModelName) ||
            !TryReadGameString(snapshot.simpleWeaponName, info.simpleWeaponName) ||
            !TryReadGameString(snapshot.weaponName, info.weaponName) ||
            !TryReadGameString(snapshot.itemName, info.itemName))
        {
            continue;
        }

        info.modelName = info.schemaModelName;
        info.displayName = LocalizeToken(info.baseName);

        info.isKnife = snapshot.isKnife;
        info.isGlove = snapshot.isGlove;
        info.isAgent = snapshot.isAgent;

        if (info.isAgent)
        {
            const AgentModelResolution resolution = ResolveAgentModel(info.itemName, info.schemaModelName, info.displayName);
            info.modelName = resolution.modelName;
            info.hasUsableAgentModel = resolution.usable;
            info.isTerroristAgent = info.itemName.rfind("customplayer_tm_", 0) == 0 || info.itemName == "customplayer_t_map_based" || info.modelName.find("/tm_") != std::string::npos;
            info.isCounterTerroristAgent = info.itemName.rfind("customplayer_ctm_", 0) == 0 || info.itemName == "customplayer_ct_map_based" || info.modelName.find("/ctm_") != std::string::npos;
        }

        info.isWeapon =
            !info.isKnife &&
            !info.isGlove &&
            !info.isAgent &&
            snapshot.definitionIndex <= 70 &&
            (IsNormalWeaponType(info.typeName) || snapshot.definitionIndex == 31);   // 31 = Zeus x27 ("Equipment", has skins)

        m_items.emplace_back(std::move(info));
    }

    return !m_items.empty();
}

bool ItemSchema::BuildPaintKitList()
{
    if (!m_schema)
        return false;

    auto& map = PaintKitMap(m_schema);

    if (!IsReasonableMap(map.Count(), map.m_Elements, MAX_REASONABLE_PAINT_KITS))
        return false;

    m_paintKits.reserve(map.Count());

    for (int i = 0; i < map.Count(); ++i)
    {
        const auto& node = map.Element(i);
        CPaintKit* paintKit = node.m_Value;
        PaintKitSnapshot snapshot{};

        if (!TryReadPaintKit(paintKit, snapshot) || snapshot.id < 0)
            continue;

        PaintKitInfo info;
        info.id = snapshot.id;
        info.rarity = snapshot.rarity;

        if (!TryReadGameString(snapshot.descriptionTag, info.descriptionTag))
            continue;

        // An unreadable name is repaired from items_game.txt (BuildItemsGameLists), not dropped.
        if (!TryReadGameString(snapshot.name, info.name))
            info.name.clear();

        m_gamePaintKits[info.id] = paintKit;

        info.displayName = LocalizeToken(info.descriptionTag);
        info.usesOldModel = snapshot.usesOldModel;

        m_paintKits.emplace_back(std::move(info));
    }

    return !m_paintKits.empty();
}

namespace
{
    // Reads one entry body { "key" "value" ... } (the opening "{" is already consumed, the closing "}" is consumed
    // here) and calls onValue(key, value) for every top-level key/value pair. Nested blocks are skipped.
    template <typename Tokenizer, typename OnValue>
    void ParseEntryBody(Tokenizer& tokenizer, OnValue onValue)
    {
        std::string token;
        int depth = 1;

        while (depth > 0 && tokenizer.Next(token))
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

            const std::string key = token;

            if (!tokenizer.Next(token))
                break;

            if (token == "{")
            {
                ++depth;
                continue;
            }

            onValue(key, token);
        }
    }

    // Walks one section { "<id>" { ... } "<id>" { ... } } and calls onEntry(id) for every entry, positioned just
    // after the entry's "{". onEntry must consume the entry body (ParseEntryBody). Consumes the section's "}".
    template <typename Tokenizer, typename OnEntry>
    void ParseSection(Tokenizer& tokenizer, OnEntry onEntry)
    {
        std::string token;

        while (tokenizer.Next(token))
        {
            if (token == "}")
                break;

            const int id = ParseInt(token);

            if (!tokenizer.Next(token))
                break;

            if (token != "{")
                continue;   // plain key/value inside the section

            onEntry(id);
        }
    }

    int ParseRarity(const std::string& name)
    {
        static const std::pair<const char*, int> rarities[] =
        {
            { "default", RARITY_DEFAULT }, { "common", RARITY_COMMON }, { "uncommon", RARITY_UNCOMMON },
            { "rare", RARITY_RARE }, { "mythical", RARITY_MYTHICAL }, { "legendary", RARITY_LEGENDARY },
            { "ancient", RARITY_ANCIENT }, { "immortal", RARITY_IMMORTAL }, { "unusual", RARITY_UNUSUAL }
        };

        for (const auto& [key, value] : rarities)
        {
            if (name == key)
                return value;
        }

        return RARITY_DEFAULT;
    }

    int RarityFromLootListName(const std::string& name)
    {
        static const std::pair<const char*, int> suffixes[] =
        {
            { "_common", RARITY_COMMON }, { "_uncommon", RARITY_UNCOMMON }, { "_rare", RARITY_RARE },
            { "_mythical", RARITY_MYTHICAL }, { "_legendary", RARITY_LEGENDARY }, { "_ancient", RARITY_ANCIENT },
            { "_immortal", RARITY_IMMORTAL }
        };

        for (const auto& [suffix, value] : suffixes)
        {
            const std::size_t length = std::strlen(suffix);

            if (name.size() > length && name.compare(name.size() - length, length, suffix) == 0)
                return value;
        }

        return 0;
    }

    StickerKitInfo FinishStickerKit(StickerKitInfo info)
    {
        info.displayName = LocalizeToken(info.itemName);

        if (info.displayName.empty())
            info.displayName = HumanizeName(info.name);

        constexpr const char* DISPLAY_PREFIX = "Sticker | ";

        if (info.displayName.rfind(DISPLAY_PREFIX, 0) == 0)
            info.displayName.erase(0, std::strlen(DISPLAY_PREFIX));

        info.sortName = LowercaseAscii(info.displayName);

        info.searchText.reserve(
            info.displayName.size() + info.name.size() + info.itemName.size() +
            info.material.size() + 32);
        info.searchText = info.displayName;
        info.searchText += '\n';
        info.searchText += info.name;
        info.searchText += '\n';
        info.searchText += info.itemName;
        info.searchText += '\n';
        info.searchText += info.material;
        info.searchText += '\n';
        info.searchText += std::to_string(info.id);
        info.searchText = LowercaseAscii(std::move(info.searchText));

        return info;
    }

    // Localized name with the "Music Kit | " / "Charm | " prefix stripped.
    std::string DefinitionDisplayName(const EconDefinitionInfo& info, const char* displayPrefix)
    {
        std::string displayName = LocalizeToken(info.locName);

        if (displayName.empty())
            displayName = HumanizeName(info.name);

        if (displayName.rfind(displayPrefix, 0) == 0)
            displayName.erase(0, std::strlen(displayPrefix));

        return displayName;
    }

    void SortByDisplayName(std::vector<EconDefinitionInfo>& list)
    {
        std::sort(list.begin(), list.end(), [](const EconDefinitionInfo& a, const EconDefinitionInfo& b)
        {
            return a.sortName != b.sortName ? a.sortName < b.sortName : a.id < b.id;
        });
    }
}

// Stickers, music kits and charms come from scripts/items/items_game.txt. Valve APPENDS a new top-level block for
// every update ("sticker_kits" appears 55 times, "music_definitions" 32, "keychain_definitions" 5 — build 14183),
// so every block has to be read, not just the first one. Later blocks win when an id repeats.
void ItemSchema::BuildItemsGameLists()
{
    std::string itemsGame;

    if (!ReadGameTextFile("scripts/items/items_game.txt", itemsGame))
        return;

    std::map<int, StickerKitInfo> stickers;
    std::map<int, EconDefinitionInfo> musicKits;
    std::map<int, EconDefinitionInfo> keychains;
    std::unordered_map<int, std::string> paintKitNames;

    KeyValuesTokenizer tokenizer(itemsGame);
    std::string token;
    int depth = 0;

    const auto parseDefinitions = [&](std::map<int, EconDefinitionInfo>& out)
    {
        ParseSection(tokenizer, [&](int id)
        {
            EconDefinitionInfo info{};
            info.id = id;

            ParseEntryBody(tokenizer, [&](const std::string& key, const std::string& value)
            {
                if (key == "name")
                    info.name = value;
                else if (key == "loc_name")
                    info.locName = value;
                else if (key == "image_inventory")
                    info.image = value;
                else if (key == "base")
                    info.base = value;
                else if (key == "item_rarity")
                    info.rarity = ParseRarity(value);
            });

            if (info.id > 0 && !info.name.empty())
                out.insert_or_assign(info.id, std::move(info));
        });
    };

    while (tokenizer.Next(token))
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

        // Sections live directly under the root "items_game" block (depth 1). Anything deeper is skipped.
        if (depth != 1)
            continue;

        const std::string section = token;

        if (!tokenizer.Next(token))
            break;

        if (token != "{")
            continue;   // top-level key/value pair

        if (section == "sticker_kits")
        {
            ParseSection(tokenizer, [&](int id)
            {
                StickerKitInfo info{};
                info.id = id;

                ParseEntryBody(tokenizer, [&](const std::string& key, const std::string& value)
                {
                    if (key == "name")
                        info.name = value;
                    else if (key == "item_name")
                        info.itemName = value;
                    else if (key == "sticker_material")
                        info.material = value;
                    else if (key == "item_rarity")
                        info.rarity = ParseRarity(value);
                });

                if (info.id > 0 && !info.material.empty())
                    stickers.insert_or_assign(info.id, std::move(info));
            });
        }
        else if (section == "music_definitions")
        {
            parseDefinitions(musicKits);
        }
        else if (section == "paint_kits")
        {
            ParseSection(tokenizer, [&](int id)
            {
                std::string name;

                ParseEntryBody(tokenizer, [&](const std::string& key, const std::string& value)
                {
                    if (key == "name")
                        name = value;
                });

                if (!name.empty())
                    paintKitNames.insert_or_assign(id, std::move(name));
            });
        }
        else if (section == "keychain_definitions")
        {
            parseDefinitions(keychains);
        }
        else if (section == "client_loot_lists")
        {
            // "<list name>" { "[paint kit]weapon_x" "1" ... }. Lists named ..._rare / ..._ancient etc. hold the skins
            // of that rarity (e.g. "crate_community_1_ancient" → M4A4 | Asimov is Covert even though the Asimov paint
            // kit itself is "legendary"). This is where the in-game rarity colour comes from.
            while (tokenizer.Next(token))
            {
                if (token == "}")
                    break;

                const std::string listName = token;

                if (!tokenizer.Next(token))
                    break;

                if (token != "{")
                    continue;

                const int rarity = RarityFromLootListName(listName);

                ParseEntryBody(tokenizer, [&](const std::string& key, const std::string&)
                {
                    if (rarity <= 0 || key.size() < 4 || key[0] != '[')
                        return;

                    const std::size_t close = key.find(']');

                    if (close == std::string::npos || close + 1 >= key.size())
                        return;

                    m_skinRarity[key.substr(1, close - 1) + "|" + key.substr(close + 1)] = rarity;
                });
            }
        }
        else
        {
            ++depth;   // some other section: step inside and let the depth counter skip it
        }
    }

    // Paint kit names are read from the game's own paint kit objects. The glove changer used to point a glove paint
    // kit's name at one of OUR strings; once Chadware was unloaded that was freed memory, so after loading Chadware
    // again (without restarting CS2) the finishes you had applied were gone from the lists and Moto Gloves had no
    // picture. items_game.txt has the real names: use them, and give the game its name back as a copy that is never
    // freed.
    for (PaintKitInfo& paintKit : m_paintKits)
    {
        const auto it = paintKitNames.find(paintKit.id);

        if (it == paintKitNames.end() || it->second == paintKit.name)
            continue;

        paintKit.name = it->second;

        if (const auto game = m_gamePaintKits.find(paintKit.id); game != m_gamePaintKits.end())
            TryWritePaintKitName(game->second, Skins::PermanentName(paintKit.name));
    }

    m_stickerKits.reserve(stickers.size());

    for (auto& [id, info] : stickers)
        m_stickerKits.emplace_back(FinishStickerKit(std::move(info)));   // std::map → already sorted by id

    m_musicKits.reserve(musicKits.size());

    for (auto& [id, info] : musicKits)
    {
        if (info.rarity == RARITY_DEFAULT)
            info.rarity = RARITY_RARE;   // music kits are all "High Grade" (blue)

        info.displayName = DefinitionDisplayName(info, "Music Kit | ");
        info.sortName = LowercaseAscii(info.displayName);
        m_musicKits.emplace_back(std::move(info));
    }

    // Highlight-reel (souvenir) charms have no art of their own: they point at a "base" charm and show a video
    // clip on it. Use the base charm's art and name it "<base name> | <clip name>".
    std::unordered_map<std::string, const EconDefinitionInfo*> keychainsByName;

    for (const auto& [id, info] : keychains)
        keychainsByName.emplace(info.name, &info);

    for (auto& [id, info] : keychains)   // names + base art first — nothing may be moved out yet
    {
        info.displayName = DefinitionDisplayName(info, "Charm | ");

        if (!info.base.empty())
        {
            if (const auto it = keychainsByName.find(info.base); it != keychainsByName.end())
            {
                if (info.image.empty())
                    info.image = it->second->image;

                if (info.rarity == RARITY_DEFAULT)
                    info.rarity = it->second->rarity;

                info.displayName = DefinitionDisplayName(*it->second, "Charm | ") + " | " + info.displayName;
            }
        }

        info.sortName = LowercaseAscii(info.displayName);
    }

    m_keychains.reserve(keychains.size());

    for (auto& [id, info] : keychains)
        m_keychains.emplace_back(std::move(info));

    SortByDisplayName(m_musicKits);
    SortByDisplayName(m_keychains);
}

bool ItemSchema::IsPaintKitForItem(const char* itemName, const char* paintKitName) const
{
    if (!I::FileSystem || !itemName || !*itemName || !paintKitName || !*paintKitName)
        return false;

    std::string path = "panorama/images/econ/default_generated/";
    path.reserve(path.size() + std::strlen(itemName) + std::strlen(paintKitName) + 20);

    path += itemName;
    path += "_";
    path += paintKitName;
    path += "_light_png.vtex_c";

    return I::FileSystem->Exists(path.c_str(), "GAME");
}

bool ItemSchema::BuildCompatiblePaintKitList(std::uint16_t definitionIndex)
{
    if (!I::FileSystem)
        return false;

    if (m_itemPaintKits.contains(definitionIndex))
        return true;

    const ItemDefinitionInfo* itemDefinition = GetItem(definitionIndex);

    if (!itemDefinition)
        return false;

    if ((!itemDefinition->isWeapon && !itemDefinition->isKnife && !itemDefinition->isGlove) ||
        itemDefinition->itemName.empty())
    {
        return false;
    }

    std::vector<PaintKitInfo> compatible;
    compatible.reserve(128);

    PaintKitInfo defaultKit;
    defaultKit.id = 0;
    defaultKit.rarity = 0;
    defaultKit.name = "default";
    defaultKit.descriptionTag = "Default";
    defaultKit.displayName = "Default";

    compatible.emplace_back(std::move(defaultKit));

    for (const auto& paintKit : m_paintKits)
    {
        if (paintKit.id <= 0 || paintKit.name.empty())
            continue;

        if (!IsPaintKitForItem(itemDefinition->itemName.c_str(), paintKit.name.c_str()))
            continue;

        compatible.emplace_back(paintKit);
    }

    m_itemPaintKits.emplace(definitionIndex, std::move(compatible));
    return true;
}

const ItemDefinitionInfo* ItemSchema::GetItem(std::uint16_t definitionIndex) const
{
    const auto it = std::lower_bound(
        m_items.begin(),
        m_items.end(),
        definitionIndex,
        [](const ItemDefinitionInfo& item, std::uint16_t index)
    {
        return item.definitionIndex < index;
    }
    );

    if (it == m_items.end() || it->definitionIndex != definitionIndex)
        return nullptr;

    return &*it;
}

const PaintKitInfo* ItemSchema::GetPaintKit(int paintKitId) const
{
    const auto it = std::lower_bound(
        m_paintKits.begin(),
        m_paintKits.end(),
        paintKitId,
        [](const PaintKitInfo& paintKit, int id)
    {
        return paintKit.id < id;
    }
    );

    if (it == m_paintKits.end() || it->id != paintKitId)
        return nullptr;

    return &*it;
}

const StickerKitInfo* ItemSchema::GetStickerKit(int stickerKitId) const
{
    const auto it = std::lower_bound(
        m_stickerKits.begin(),
        m_stickerKits.end(),
        stickerKitId,
        [](const StickerKitInfo& stickerKit, int id)
    {
        return stickerKit.id < id;
    });

    if (it == m_stickerKits.end() || it->id != stickerKitId)
        return nullptr;

    return &*it;
}

const std::vector<PaintKitInfo>* ItemSchema::GetPaintKitsForItem(std::uint16_t definitionIndex)
{
    auto it = m_itemPaintKits.find(definitionIndex);

    if (it != m_itemPaintKits.end())
        return &it->second;

    if (!BuildCompatiblePaintKitList(definitionIndex))
        return nullptr;

    it = m_itemPaintKits.find(definitionIndex);

    return it != m_itemPaintKits.end() ? &it->second : nullptr;
}

const char* Skins::PermanentName(const std::string& text)
{
    const std::lock_guard lock(g_permanentNamesMutex);

    if (const auto it = g_permanentNames.find(text); it != g_permanentNames.end())
        return it->second;

    // The process heap, never freed: it outlives Chadware's own memory.
    char* copy = static_cast<char*>(HeapAlloc(GetProcessHeap(), 0, text.size() + 1));

    if (!copy)
        return nullptr;

    std::memcpy(copy, text.c_str(), text.size() + 1);
    g_permanentNames.emplace(text, copy);
    return copy;
}

bool ItemSchema::ReadGameText(const char* path, std::string& output)
{
    return ReadGameTextFile(path, output);
}

std::string ItemSchema::Localize(const std::string& token)
{
    return LocalizeToken(token);
}
