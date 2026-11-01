#pragma once

#include "../../interfaces/IResponseSystem/IResponseSystem.h"
#include "../../../cs2/entity/C_WeaponVData/C_WeaponVData.h"
#include <cstdint>
#include <string>
#include <stdexcept>
#include <memory>
#include <unordered_map>
#include <vector>

namespace AgentVoiceRules
{
    // Expected loading state that may be polled until the native data is ready.
    struct NotReady : std::runtime_error
    {
        using std::runtime_error::runtime_error;
    };

    struct Criterion
    {
        std::string key;
        std::string expression;
        std::uint16_t weight = 0x3c00; // Native half-float 1.0.
        std::uint8_t matcher = 0;
        std::uint64_t limits = 0;
    };

    struct Response
    {
        std::uint8_t type = 0;
        std::string value;
    };

    struct Group
    {
        std::string name;
        std::vector<Response> responses;
    };

    struct Rule
    {
        std::vector<Criterion> criteria;
        std::vector<unsigned> groups;
        bool complete = true;
    };

    struct Snapshot
    {
        std::vector<Group> groups;
        std::vector<Rule> rules;
    };

    struct Command
    {
        std::string token;
        bool automatic = false;
        std::vector<std::string> concepts;
    };

    struct Meaning
    {
        std::string key;
        std::string conceptName;
        bool supported = false;
    };

    using Choices = std::vector<unsigned>;
    using Replacements = std::unordered_map<unsigned, Choices>;

    struct Index
    {
        std::vector<std::string> events;
        std::vector<Meaning> meanings;
        std::vector<Choices> sources;
        std::unordered_map<std::string, std::unordered_map<unsigned, Choices>> voices;
        std::vector<Command> commands;
        std::vector<Choices> commandSources;
    };

    struct Selection
    {
        Replacements general;
        std::vector<Replacements> commands;
    };

    // All returned data owns its strings. Native reads happen only on the game thread.
    Snapshot Read(const ResponseRules::Root* root, CUtlSymbolTable* symbols, unsigned buckets);

    // Step bounds the number of native container entries copied.
    // Callers must keep the same root and generation alive for every step.
    class Reader
    {
        struct State;
        std::unique_ptr<State> state;

    public:
        Reader(const ResponseRules::Root* root, CUtlSymbolTable* symbols, unsigned buckets);
        ~Reader();
        bool Step(unsigned entries);
        Snapshot Take();
    };

    std::vector<Command> ReadCommands(const ResponseRules::WheelRow* begin, const ResponseRules::WheelRow* end, WeaponDefinitionRegistry* registry);
    void AddCommand(std::vector<Command>& commands, Command command);
    Index Build(const Snapshot& snapshot, std::vector<Command> commands);
    Selection Select(const Index& index, const std::string& voice);
}
