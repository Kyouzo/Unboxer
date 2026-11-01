#include "agent_voice_rules.h"

#include <Windows.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <iterator>
#include <memory>
#include <set>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace AgentVoiceRules
{
    namespace
    {
        constexpr unsigned MaxEntries = 1u << 20;

        // Private response storage has no validating accessor. Guard only native copies
        // and strings so an unavailable layout falls back to native voice playback.
        bool Copy(void* to, const void* from, std::size_t size) noexcept
        {
            __try
            {
                if (!from)
                    return false;

                std::memcpy(to, from, size);
                return true;
            }
            __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH)
            {
                return false;
            }
        }

        template <class T>
        T Read(const T* address)
        {
            T value{};
            if (!Copy(&value, address, sizeof(value)))
                throw std::runtime_error("Unreadable native rule data");

            return value;
        }

        bool CopyString(const char* address, char* text, std::size_t size) noexcept
        {
            __try
            {
                if (!address)
                {
                    *text = 0;
                    return true;
                }

                for (std::size_t i = 0; i < size; ++i)
                {
                    if (!(text[i] = address[i]))
                        return true;
                }
            }
            __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH)
            {
            }

            return false;
        }

        std::string Text(const char* address)
        {
            char text[4096];
            if (!CopyString(address, text, sizeof(text)))
                throw std::runtime_error("Invalid native string");

            return text;
        }

        std::string Lower(std::string value)
        {
            for (char& c : value)
            {
                if (c >= 'A' && c <= 'Z')
                    c += 'a' - 'A';
            }

            return value;
        }

        template <class T>
        void Unique(std::vector<T>& values)
        {
            std::sort(values.begin(), values.end());
            values.erase(std::unique(values.begin(), values.end()), values.end());
        }

        struct TreeCursor
        {
            const ResponseRules::Tree* wide = nullptr;
            const ResponseRules::NarrowTree* narrow = nullptr;
            uint8_t* data = nullptr;
            unsigned slots = 0;
            unsigned count = 0;
            unsigned position = 0;
            unsigned seen = 0;
            unsigned stride = 0;

            template <class Header>
            void Load(const Header& header)
            {
                slots = header.slots;
                count = header.count;
                data = header.data;

                const auto capacity = header.Capacity();
                if (slots > capacity || capacity > MaxEntries || count > slots || (slots && !data))
                    throw std::runtime_error("Invalid native dictionary");

                position = seen = 0;
            }

            void Open(const ResponseRules::Tree* tree, unsigned bytes)
            {
                wide = tree;
                narrow = nullptr;
                stride = bytes;
                Load(Read(tree));
            }

            void Open(const ResponseRules::NarrowTree* tree, unsigned bytes)
            {
                narrow = tree;
                wide = nullptr;
                stride = bytes;
                Load(Read(tree));
            }

            bool Done() const
            {
                return position == slots;
            }

            const uint8_t* Next()
            {
                const auto id = position++;
                const auto* node = data + std::size_t(id) * stride;
                const unsigned link = narrow ? Read(reinterpret_cast<const std::uint16_t*>(node)) : Read(reinterpret_cast<const unsigned*>(node));

                if (link == id)
                    return nullptr;

                ++seen;
                return node;
            }

            template <class Header>
            void CheckStorage(const Header& header) const
            {
                if (header.count != count || header.slots != slots || header.data != data)
                    throw std::runtime_error("Native dictionary changed during copy");
            }

            void ValidateStorage() const
            {
                if (narrow)
                    CheckStorage(Read(narrow));
                else
                    CheckStorage(Read(wide));
            }

            void Validate() const
            {
                ValidateStorage();

                if (seen != count)
                    throw std::runtime_error("Native dictionary changed during copy");
            }
        };

        template <class Node>
        std::vector<const Node*> Tree(const ResponseRules::Tree* tree)
        {
            TreeCursor cursor;
            cursor.Open(tree, sizeof(Node));

            std::vector<const Node*> result;
            while (!cursor.Done())
            {
                if (const auto* node = cursor.Next())
                    result.push_back(reinterpret_cast<const Node*>(node));
            }

            cursor.Validate();
            return result;
        }

        std::vector<C_WeaponVData*> WeaponDefinitions(const WeaponDefinitionRegistry* registry)
        {
            // Negative hashes mark unused or deleted slots in this open-addressed table.
            const auto map = Read(&registry->definitions);
            const auto capacity = map.Capacity();

            if (map.buckets > capacity || capacity > MaxEntries || map.count > map.buckets || (map.buckets && !map.data))
                throw std::runtime_error("Invalid weapon-definition hash table");

            std::vector<C_WeaponVData*> result;
            result.reserve(map.count);

            for (unsigned i = 0; i < map.buckets; ++i)
            {
                const auto slot = Read(map.data + i);
                if (slot.hash >= 0)
                    result.push_back(slot.data);
            }

            if (result.size() != map.count)
                throw std::runtime_error("Weapon-definition hash table changed during copy");

            return result;
        }

        template <unsigned N>
        Choices SmallVector(const ResponseRules::InlineVector<N>* pointer)
        {
            const auto vector = Read(pointer);
            const auto capacity = vector.Capacity();

            if (vector.count > capacity || capacity > MaxEntries)
                throw std::runtime_error("Invalid rule vector");

            Choices result(vector.count);
            if (vector.count && !Copy(result.data(), vector.Data(), vector.count * sizeof(unsigned)))
                throw std::runtime_error("Unreadable rule vector");

            return result;
        }

        bool ExactIdentity(const std::string& text)
        {
            return !text.empty() && std::all_of(text.begin(), text.end(), [](unsigned char c)
            {
                return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '.';
            });
        }

        std::string Term(const Criterion& c, const std::string& bank)
        {
            std::string key = Lower(c.key);
            const std::string expr = Lower(c.expression);

            // Shipped worldSaid* contexts end in the response-bank namespace. Infer
            // that namespace from this voice's native responses, not an agent list.
            if (!bank.empty() && key.starts_with("world") && key.size() > bank.size() && key.ends_with(bank))
                key.replace(key.size() - bank.size(), bank.size(), "@voice");

            auto field = [](const std::string& v)
            {
                return std::to_string(v.size()) + ":" + v;
            };

            return field(key) + field(expr) + ":" + std::to_string(c.weight) + ":" + std::to_string(c.matcher) + ":" + std::to_string(c.limits);
        }

        Choices Intersect(const Choices& a, const Choices& b)
        {
            Choices result;
            std::set_intersection(a.begin(), a.end(), b.begin(), b.end(), std::back_inserter(result));
            return result;
        }

        // These are server radio protocol associations, not agent/event mappings.
        // The client wheel table has no basic-radio entries.
        void ProtocolCommands(std::vector<Command>& out)
        {
            constexpr const char* pairs[][2] =
            {
                {"#Cstrike_TitlesTXT_Roger_that", "radio.roger"},
                {"#Cstrike_TitlesTXT_Negative", "radio.negative"},
                {"#Cstrike_TitlesTXT_Cheer", "radio.cheer"},
                {"#Cstrike_TitlesTXT_Hold_this_position", "radio.holdposition"},
                {"#Cstrike_TitlesTXT_Follow_me", "radio.followme"},
                {"#Cstrike_TitlesTXT_Thanks", "radio.thanks"},
                {"#Cstrike_TitlesTXT_Cover_me", "radio.coverme"},
                {"#Cstrike_TitlesTXT_You_take_the_point", "radio.youtakethepoint"},
                {"#Cstrike_TitlesTXT_Regroup_team", "radio.regroup"},
                {"#Cstrike_TitlesTXT_Taking_fire", "radio.takingfire"},
                {"#Cstrike_TitlesTXT_Go_go_go", "radio.gogogo"},
                {"#Cstrike_TitlesTXT_Team_fall_back", "radio.teamfallback"},
                {"#Cstrike_TitlesTXT_Stick_together_team", "radio.sticktogether"},
                {"#Cstrike_TitlesTXT_Compliment", "radio.compliment"},
                {"#Cstrike_TitlesTXT_Report_in_team", "radio.reportinteam"},
                {"#Cstrike_TitlesTXT_Affirmative", "radio.affirmitive"},
                {"#Cstrike_TitlesTXT_Enemy_spotted", "radio.enemyspotted"},
                {"#Cstrike_TitlesTXT_Need_backup", "radio.needbackup"},
                {"#Cstrike_TitlesTXT_Sector_clear", "radio.sectorclear"},
                {"#Cstrike_TitlesTXT_In_position", "radio.inposition"},
                {"#Cstrike_TitlesTXT_Reporting_in", "radio.reportingin"},
                {"#Cstrike_TitlesTXT_Get_out_of_there", "radio.getoutofthere"},
                // The server accepts this second meaning for the client wheel token.
                {"#Chatwheel_gethostage", "cw.requestgethostages"}
            };

            for (const auto& p : pairs)
                AddCommand(out, {p[0], false, {p[1]}});
        }
    }

    void AddCommand(std::vector<Command>& commands, Command command)
    {
        if (command.token.empty())
            return;

        for (auto& conceptName : command.concepts)
            conceptName = Lower(std::move(conceptName));

        Unique(command.concepts);

        for (auto& existing : commands)
        {
            if (existing.token == command.token && existing.automatic == command.automatic)
            {
                existing.concepts.insert(existing.concepts.end(), command.concepts.begin(), command.concepts.end());
                Unique(existing.concepts);
                return;
            }
        }

        commands.push_back(std::move(command));
    }

    struct Reader::State
    {
        enum class Phase
        {
            Pending,
            Criteria,
            Groups,
            Rules,
            Complete
        };

        const ResponseRules::Root* root;
        const ResponseRules::PendingEntry* pendingData = nullptr;
        CUtlSymbolTable* symbols;
        unsigned buckets;
        unsigned pendingCount = 0;
        unsigned pendingPosition = 0;
        unsigned bucket = 0;
        Phase phase = Phase::Pending;
        Snapshot snapshot;
        std::unordered_set<const ResponseRules::Rule*> incompleteRules;
        std::unordered_map<unsigned, Criterion> criteria;
        std::unordered_map<unsigned, unsigned> groupIds;
        TreeCursor cursor;

        State(const ResponseRules::Root* pointer, CUtlSymbolTable* table, unsigned count) : root(pointer), symbols(table), buckets(count)
        {
            if (!root || !symbols || !I::SymbolString || !buckets || buckets > 65536)
                throw std::runtime_error("Invalid response-rule provider");

            const auto groups = Read(&root->groups.count);
            const auto criterionCount = Read(&root->criteria.count);

            if (!groups || !criterionCount)
                throw NotReady("Response dictionaries not loaded");

            if (groups > MaxEntries || criterionCount > MaxEntries)
                throw std::runtime_error("Invalid response dictionary counts");

            // Avoid container growth moving thousands of copied entries in a later slice.
            criteria.reserve(criterionCount);
            groupIds.reserve(groups);
            snapshot.groups.reserve(groups);
            snapshot.rules.reserve(groups);

            const auto pending = Read(root->Pending(buckets));
            pendingCount = pending.count;
            const auto capacity = pending.capacity;
            pendingData = pending.data;

            if (pendingCount > capacity || capacity > MaxEntries || (pendingCount && !pendingData))
                throw std::runtime_error("Invalid unresolved requirement list");

            incompleteRules.reserve(pendingCount);
        }

        void One()
        {
            // Each call visits at most one container slot (including free slots).
            if (phase == Phase::Pending)
            {
                if (pendingPosition < pendingCount)
                    incompleteRules.insert(Read(&pendingData[pendingPosition++].rule));
                else
                {
                    cursor.Open(&root->criteria, sizeof(ResponseRules::CriterionNode));
                    phase = Phase::Criteria;
                }

                return;
            }

            if (cursor.Done())
            {
                cursor.Validate();

                if (phase == Phase::Criteria)
                {
                    cursor.Open(&root->groups, sizeof(ResponseRules::GroupNode));
                    phase = Phase::Groups;
                }
                else if (phase == Phase::Groups)
                {
                    cursor.Open(&root->buckets[0].tree, sizeof(ResponseRules::RuleNode));
                    phase = Phase::Rules;
                }
                else if (++bucket < buckets)
                    cursor.Open(&root->buckets[bucket].tree, sizeof(ResponseRules::RuleNode));
                else
                {
                    if (snapshot.rules.empty())
                        throw std::runtime_error("Loaded response dictionaries contain no rules");

                    phase = Phase::Complete;
                }

                return;
            }

            const auto id = cursor.position;
            const auto node = cursor.Next();

            if (!node)
                return;

            if (phase == Phase::Criteria)
            {
                const auto native = Read(reinterpret_cast<const ResponseRules::CriterionNode*>(node));
                Criterion c;
                c.key = Text(symbols->String(native.symbol));
                c.expression = Text(native.expression);
                c.weight = native.weight;
                c.matcher = native.matcher & 0x7f;
                c.limits = native.Limits();

                if (c.key.empty())
                    throw std::runtime_error("Unresolved criterion symbol");

                criteria.emplace(id, std::move(c));
            }
            else if (phase == Phase::Groups)
            {
                Group group;
                const auto native = Read(reinterpret_cast<const ResponseRules::GroupNode*>(node));
                group.name = Text(native.name);

                const auto data = Read(native.group);
                const auto count = data.count;
                const auto capacity = data.Capacity();
                const auto entries = data.entries;

                if (count > capacity || (count && !entries))
                    throw std::runtime_error("Invalid response group");

                for (unsigned i = 0; i < count; ++i)
                {
                    const auto response = Read(entries + i);
                    group.responses.push_back({static_cast<std::uint8_t>(response.type & 0x3f), Text(response.text)});
                }

                groupIds.emplace(id, static_cast<unsigned>(snapshot.groups.size()));
                snapshot.groups.push_back(std::move(group));
            }
            else
            {
                const auto data = Read(reinterpret_cast<const ResponseRules::RuleNode*>(node)).rule;
                Rule rule;
                rule.complete = !incompleteRules.contains(data);

                for (auto key : SmallVector(&data->criteria))
                {
                    const auto it = criteria.find(key);
                    if (it == criteria.end())
                        rule.complete = false;
                    else
                        rule.criteria.push_back(it->second);
                }

                for (auto key : SmallVector(&data->groups))
                {
                    const auto it = groupIds.find(key);
                    if (it == groupIds.end())
                        rule.complete = false;
                    else
                        rule.groups.push_back(it->second);
                }

                snapshot.rules.push_back(std::move(rule));
                if (snapshot.rules.size() > MaxEntries)
                    throw std::runtime_error("Too many response rules");
            }
        }
    };

    Reader::Reader(const ResponseRules::Root* root, CUtlSymbolTable* symbols, unsigned buckets) : state(std::make_unique<State>(root, symbols, buckets))
    {
    }

    Reader::~Reader() = default;

    bool Reader::Step(unsigned entries)
    {
        // A new frame must validate storage before resuming its saved cursor.
        if (state->phase > State::Phase::Pending && state->phase < State::Phase::Complete)
            state->cursor.ValidateStorage();

        while (entries-- && state->phase != State::Phase::Complete)
            state->One();

        return state->phase == State::Phase::Complete;
    }

    Snapshot Reader::Take()
    {
        if (state->phase != State::Phase::Complete)
            throw std::runtime_error("Native snapshot is incomplete");

        return std::move(state->snapshot);
    }

    Snapshot Read(const ResponseRules::Root* pointer, CUtlSymbolTable* symbols, unsigned buckets)
    {
        Reader reader(pointer, symbols, buckets);
        while (!reader.Step(1024))
        {
        }

        return reader.Take();
    }

    std::vector<Command> ReadCommands(const ResponseRules::WheelRow* begin, const ResponseRules::WheelRow* end, WeaponDefinitionRegistry* registry)
    {
        std::vector<Command> out;
        ProtocolCommands(out);

        const auto bytes = reinterpret_cast<std::uintptr_t>(end) - reinterpret_cast<std::uintptr_t>(begin);
        if (!begin || end <= begin || bytes % sizeof(*begin) || bytes / sizeof(*begin) > 4096)
            throw std::runtime_error("Native communication wheel unavailable");

        for (auto* row = begin; row < end; ++row)
        {
            const auto entry = Read(row);
            auto token = Text(entry.token);
            auto conceptName = Text(entry.conceptName);

            if (!token.starts_with("#Chatwheel_") || !Lower(conceptName).starts_with("cw."))
                throw std::runtime_error("Invalid native communication wheel entry");

            AddCommand(out, {std::move(token), false, {std::move(conceptName)}});
        }

        if (!registry)
            throw NotReady("Weapon definitions not loaded");

        unsigned radios = 0;
        for (auto* data : WeaponDefinitions(registry))
        {
            if (!data || !Read(data).IsWeapon())
                continue;

            const auto token = Text(Read(&data->m_szUseRadioSubtitle()));
            if (token.empty())
                continue;

            for (const auto* sound : Tree<WeaponShootSound>(&data->m_aShootSounds()->tree))
            {
                constexpr unsigned WeaponSoundRadioUse = 23;
                const auto entry = Read(sound);

                if (entry.id != WeaponSoundRadioUse)
                    continue;

                // Inline strings remain in engine storage and may extend beyond the union.
                const auto* text = entry.flags & 0x40000000 ? sound->text.local : entry.text.heap;
                const auto conceptName = Text(text);

                if (conceptName.empty())
                    continue;

                AddCommand(out, {token, true, {conceptName}});
                ++radios;
            }
        }

        if (!radios)
            throw NotReady("Weapon radio definitions not ready");

        return out;
    }

    Index Build(const Snapshot& snapshot, std::vector<Command> commands)
    {
        Index index;
        index.commands = std::move(commands);

        std::unordered_map<std::string, unsigned> names;
        std::unordered_map<std::string, unsigned> events;
        std::unordered_map<std::string, unsigned> keys;

        for (unsigned i = 0; i < snapshot.groups.size(); ++i)
        {
            if (!names.emplace(Lower(snapshot.groups[i].name), i).second)
                throw std::runtime_error("Duplicate response group");
        }

        struct Flat
        {
            std::set<std::string> events;
            bool complete = true;
        };

        std::vector<unsigned char> visited(snapshot.groups.size());
        std::function<Flat(unsigned, unsigned)> flatten = [&](unsigned id, unsigned depth) -> Flat
        {
            if (id >= snapshot.groups.size() || depth > 128 || visited[id] == 1)
                return {{}, false};

            visited[id] = 1;
            Flat result;
            for (const auto& response : snapshot.groups[id].responses)
            {
                const auto value = Lower(response.value);
                if (response.type == 1 && !value.empty())
                    result.events.insert(value);
                else if (response.type == 4)
                {
                    const auto it = names.find(value);
                    Flat child = it == names.end() ? Flat{{}, false} : flatten(it->second, depth + 1);

                    result.events.insert(child.events.begin(), child.events.end());
                    result.complete &= child.complete;
                }
                else
                    result.complete = false;
            }

            visited[id] = 0;
            return result;
        };

        struct Row
        {
            Flat flat;
            std::string voice;
            std::string conceptName;
            const Rule* rule;
            bool valid = true;
        };

        std::vector<Row> rows;
        std::unordered_map<std::string, std::set<std::string>> banks;
        std::unordered_map<std::string, std::set<std::string>> contextKeys;

        for (const auto& rule : snapshot.rules)
        {
            // Response only groups have an empty embedded rule. Native scoring
            // returns zero for it, below the selector's positive minimum score.
            // Their recordings acquire meaning through the rules that reference them.
            if (rule.complete && rule.criteria.empty())
                continue;

            Row row{{}, {}, {}, &rule, rule.complete};
            for (const auto& criterion : rule.criteria)
            {
                const auto key = Lower(criterion.key);
                const auto expr = Lower(criterion.expression);

                if (key == "model" || key == "concept")
                {
                    auto& value = key == "model" ? row.voice : row.conceptName;
                    if (!ExactIdentity(expr) || (!value.empty() && value != expr))
                        row.valid = false;

                    value = expr;
                }
            }

            for (auto group : rule.groups)
            {
                const auto child = flatten(group, 0);
                row.flat.complete &= child.complete;
                row.flat.events.insert(child.events.begin(), child.events.end());
            }

            if (!row.voice.empty())
            {
                for (const auto& criterion : rule.criteria)
                {
                    if (auto key = Lower(criterion.key); key.starts_with("world"))
                        contextKeys[row.voice].insert(std::move(key));
                }
            }

            for (const auto& event : row.flat.events)
            {
                if (events.emplace(event, static_cast<unsigned>(index.events.size())).second)
                    index.events.push_back(event);

                const auto dot = event.find('.');
                if (!row.voice.empty() && dot != std::string::npos)
                    banks[row.voice].insert(event.substr(0, dot));
            }

            rows.push_back(std::move(row));
        }

        // Some stock banks borrow recordings from other banks. A namespace is a
        // voice context only when it also occurs in that voice's criterion keys.
        // Require one unambiguous namespace rather than choosing the most common sound.
        for (auto& [voice, prefixes] : banks)
            std::erase_if(prefixes, [&](const std::string& prefix)
            {
                const auto& contexts = contextKeys[voice];
                return std::none_of(contexts.begin(), contexts.end(), [&](const std::string& key)
                {
                    return key.ends_with(prefix);
                });
            });

        index.sources.resize(index.events.size());

        for (unsigned n = 0; n < rows.size(); ++n)
        {
            const auto& row = rows[n];
            const bool valid = row.valid && row.flat.complete && !row.voice.empty() && !row.conceptName.empty();
            std::string key;

            if (valid)
            {
                std::vector<std::string> terms;
                const auto& prefixes = banks[row.voice];
                const std::string bank = prefixes.size() == 1 ? *prefixes.begin() : "";

                for (const auto& c : row.rule->criteria)
                {
                    if (Lower(c.key) != "model")
                        terms.push_back(Term(c, bank));
                }

                Unique(terms);

                for (const auto& term : terms)
                    key += std::to_string(term.size()) + ":" + term;
            }
            else
                key = "unsupported:" + std::to_string(n);

            auto [it, added] = keys.emplace(key, static_cast<unsigned>(index.meanings.size()));
            if (added)
                index.meanings.push_back({key, row.conceptName, valid});

            const auto meaning = it->second;
            for (const auto& event : row.flat.events)
            {
                const auto id = events.at(event);
                index.sources[id].push_back(meaning);

                if (valid)
                    index.voices[row.voice][meaning].push_back(id);
            }
        }

        // Associate commands with only their eligible source events once, instead
        // of testing every event against every command again for each voice.
        index.commandSources.resize(index.commands.size());
        std::unordered_map<std::string, Choices> commandsByConcept;

        for (unsigned cmd = 0; cmd < index.commands.size(); ++cmd)
        {
            for (const auto& conceptName : index.commands[cmd].concepts)
                commandsByConcept[conceptName].push_back(cmd);
        }

        for (unsigned source = 0; source < index.sources.size(); ++source)
        {
            Unique(index.sources[source]);

            Choices eligible;
            for (auto key : index.sources[source])
            {
                const auto found = commandsByConcept.find(index.meanings[key].conceptName);
                if (found != commandsByConcept.end())
                    eligible.insert(eligible.end(), found->second.begin(), found->second.end());
            }

            Unique(eligible);

            for (auto cmd : eligible)
                index.commandSources[cmd].push_back(source);
        }

        for (auto& [voice, meanings] : index.voices)
        {
            for (auto& [key, choices] : meanings)
                Unique(choices);
        }

        return index;
    }

    Selection Select(const Index& index, const std::string& voice)
    {
        Selection result;
        result.commands.resize(index.commands.size());

        const auto target = index.voices.find(Lower(voice));
        if (target == index.voices.end())
            return result;

        auto select = [&](const Choices& meanings, const std::vector<std::string>* concepts) -> Choices
        {
            Choices candidates;
            bool first = true;

            for (auto key : meanings)
            {
                const auto& meaning = index.meanings.at(key);
                if (concepts && std::find(concepts->begin(), concepts->end(), meaning.conceptName) == concepts->end())
                    continue;

                const auto it = target->second.find(key);
                if (!meaning.supported || it == target->second.end())
                    return {};

                candidates = first ? it->second : Intersect(candidates, it->second);
                first = false;

                if (candidates.empty())
                    return {};
            }

            return candidates;
        };

        for (unsigned source = 0; source < index.sources.size(); ++source)
        {
            if (auto choices = select(index.sources[source], nullptr); !choices.empty())
                result.general.emplace(source, std::move(choices));
        }

        for (unsigned cmd = 0; cmd < index.commands.size(); ++cmd)
        {
            for (auto source : index.commandSources.at(cmd))
            {
                if (auto choices = select(index.sources[source], &index.commands[cmd].concepts); !choices.empty())
                    result.commands[cmd].emplace(source, std::move(choices));
            }
        }

        return result;
    }
}
