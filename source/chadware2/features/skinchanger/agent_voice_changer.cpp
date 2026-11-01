// Agent voice lines - the new Chadware source's agent_voice_changer.cpp (2026-10-07). Lootline's changes: Applied takes
// the agent's definition and voice, and Drive() (bottom) feeds it from Lootline's agent loadout, so the skin changer
// files shared with Chadware stay as they are.
#include "agent_voice_changer.h"
#include "agent_voice_rules.h"
#include "item_schema.h"
#include "../../../cs2/entity/C_CSPlayerPawn/C_CSPlayerPawn.h"
#include "../../../cs2/entity/C_BaseGrenade/C_BaseGrenade.h"
#include "../../../cs2/datatypes/radio_message.h"
#include "../../config/config.h"
#include "../../interfaces/interfaces.h"
#include "../../utils/schema/schema.h"

#include <Windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <iterator>
#include <memory>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace
{
    using Hash = std::uint32_t;

    constexpr Hash InvalidHash = 0xFFFFFFFFu;

    constexpr int RadioArgumentCount = 4;
    constexpr int RadioModeArgument = 3;
    constexpr unsigned RadioTicketLifetimeMs = 250;
    constexpr unsigned RadioTicketFrameLifetime = 1;

    enum GenderEvent
    {
        Death,
        DeathTaser,
        JumpThrow,
        DamageFall
    };

    constexpr const char* GenderEvents[][2] =
    {
        {"Player.Death", "Player.DeathFem"},
        {"Player.DeathTaser", "Player.DeathTaser_F"},
        {"BaseGrenade.JumpThrowM", "BaseGrenade.JumpThrowF"},
        {"Player.DamageFall", "Player.DamageFall.Fem"}
    };

    constexpr unsigned GenderPairCount = std::size(GenderEvents);
    constexpr unsigned GenderEventCount = GenderPairCount * std::size(GenderEvents[0]);

    SRWLOCK g_lock = SRWLOCK_INIT;
    SRWLOCK g_contextLock = SRWLOCK_INIT;
    SRWLOCK g_nativeLock = SRWLOCK_INIT;

    std::atomic_bool g_stopping{false};
    std::atomic_bool g_initialized{false};

    ISoundEventSystem* g_sound = nullptr;
    CUtlSymbolTable* g_symbols = nullptr;

    struct RadioContext
    {
        unsigned command = 0;
        unsigned time = 0;
        unsigned epoch = 0;
        std::uint64_t generation = 0;
    };

    std::uint32_t g_radioEpoch = 0;
    RadioContext g_radioTicket;

    std::unique_ptr<AgentVoiceRules::Index> g_rules;
    std::atomic<std::uint64_t> g_generation{1};

    std::uint64_t g_loadedGeneration = 0;
    std::uint64_t g_appliedGeneration = 0;
    void* g_loadedRoot = nullptr;
    std::uint32_t g_lastBuildAttempt = 0;

    void* g_refreshRoot = nullptr;
    std::uint64_t g_refreshGeneration = 0;
    bool g_refreshFailed = false;

    std::vector<Hash> g_events;
    std::unordered_set<Hash> g_collisions;
    std::unordered_set<Hash> g_postDeathEvents;

    using Replacements = std::unordered_map<Hash, std::vector<unsigned>>;

    struct PreparedVoice
    {
        Replacements general;
        std::vector<Replacements> radio;
    };

    std::unordered_map<std::string, std::unique_ptr<PreparedVoice>> g_voiceCache;
    const PreparedVoice* g_activeVoice = nullptr;
    std::string g_requestedVoice;
    bool g_prepareFailed = false;
    bool g_warmComplete = false;

    using RadioCommand = AgentVoiceRules::Command;
    using EventHashes = std::unordered_map<std::string, Hash>;

    std::vector<RadioCommand> g_radioCommands;

    struct CaptureJob
    {
        void* root = nullptr;
        std::uint64_t generation = 0;

        std::unique_ptr<AgentVoiceRules::Reader> reader;
        AgentVoiceRules::Snapshot snapshot;
        std::vector<RadioCommand> commands;
        EventHashes hashes;

        std::size_t group = 0;
        std::size_t response = 0;
        bool copied = false;
    };

    // Capture is owned by the game thread. Queued and retired jobs cross g_lock.
    // The existing DLL management thread builds and destroys their owned data.
    std::unique_ptr<CaptureJob> g_capture;
    std::unique_ptr<CaptureJob> g_pendingCapture;
    std::vector<std::unique_ptr<CaptureJob>> g_retiredCaptures;
    bool g_captureSubmitted = false;

    std::unordered_set<Hash> g_speechHashes;
    Hash g_genderHashes[GenderPairCount][2]{};

    struct Actor
    {
        C_CSPlayerPawn* pawn = nullptr;
        void* identity = nullptr;
        CBaseHandle handle;
        float spawn = 0;
        std::uint8_t team = 0;

        bool operator==(const Actor&) const = default;
    };

    Actor g_actor;
    unsigned short g_definition = 0;

    struct ExclusiveLock
    {
        ExclusiveLock()
        {
            AcquireSRWLockExclusive(&g_lock);
        }

        ~ExclusiveLock()
        {
            ReleaseSRWLockExclusive(&g_lock);
        }
    };

    bool IsGrenadeProjectile(const char* name)
    {
        if (!name)
            return false;

        constexpr const char* classes[] =
        {
            "C_BaseCSGrenadeProjectile",
            "C_DecoyProjectile",
            "C_MolotovProjectile",
            "C_SmokeGrenadeProjectile",
            "C_HEGrenadeProjectile",
            "C_FlashbangProjectile"
        };

        for (const auto candidate : classes)
        {
            if (!std::strcmp(name, candidate))
                return true;
        }

        return false;
    }

    bool IsGrenadeWeapon(const char* name)
    {
        if (!name)
            return false;

        constexpr const char* classes[] =
        {
            "C_BaseCSGrenade",
            "C_DecoyGrenade",
            "C_Flashbang",
            "C_HEGrenade",
            "C_MolotovGrenade",
            "C_IncendiaryGrenade",
            "C_SmokeGrenade"
        };

        for (const auto candidate : classes)
        {
            if (!std::strcmp(name, candidate))
                return true;
        }

        return false;
    }

    void ClearRadioTicket()
    {
        AcquireSRWLockExclusive(&g_contextLock);
        g_radioTicket = {};
        ReleaseSRWLockExclusive(&g_contextLock);
    }

    // Game-thread callers own the current pawn. Sound-side callers use IsSpeaker's guard.
    bool Capture(C_CSPlayerPawn* pawn, Actor& actor)
    {
        if (!pawn)
            return false;

        auto* identity = pawn->m_pEntityIdentity();
        if (!identity)
            return false;

        const auto handle = identity->handle();
        if (!handle.valid() || handle.index() == ENT_ENTRY_MASK)
            return false;

        actor = {pawn, identity, handle, pawn->m_flLastSpawnTimeIndex(), pawn->m_iTeamNum()};
        return actor.team == 2 || actor.team == 3;
    }

    // Sound callbacks do not own entity lifetime. The sound mutex does not protect
    // entity destruction, so keep the access-violation fallback around these reads.
    bool IsSpeaker(int index, bool jump, bool allowDead)
    {
        if (!g_actor.pawn || index <= 0 || index >= ENT_ENTRY_MASK || !I::GameEntity || !I::GameEntity->Instance)
            return false;

        // Ordinary speech must originate on the local pawn. Jumpthrow sounds
        // may instead originate on an owned weapon or projectile.
        if (!jump && index != g_actor.handle.index())
            return false;

        __try
        {
            Actor current;
            if (!Capture(I::GetLocalPawn(), current) || current != g_actor)
                return false;

            if (current.pawn->m_iHealth() <= 0 && !allowDead)
                return false;

            auto* source = I::GameEntity->Instance->Get(index);
            if (source == current.pawn)
                return current.handle.index() == index;

            if (!jump || !source)
                return false;

            const char* className = static_cast<CEntityInstance*>(source)->get_entity_class_name();

            // Native jumpthrow starts on the held grenade weapon, before projectile emission.
            // Weapon owner and projectile thrower are distinct schema fields.
            if (IsGrenadeWeapon(className))
                return static_cast<C_BaseEntity*>(source)->m_hOwnerEntity() == current.handle;

            return IsGrenadeProjectile(className) && static_cast<C_BaseGrenade*>(source)->m_hThrower() == current.handle;
        }
        __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH)
        {
            return false;
        }
    }

    RadioContext RadioTicket(int command, std::uint32_t now, std::uint32_t epoch)
    {
        return command >= 0 && static_cast<unsigned>(command) < g_radioCommands.size()
            ? RadioContext{static_cast<unsigned>(command) + 1, now, epoch, g_loadedGeneration}
            : RadioContext{};
    }

    void StoreRadioTicket(RadioContext ticket)
    {
        if (!TryAcquireSRWLockExclusive(&g_contextLock))
            return;

        g_radioTicket = ticket;
        ReleaseSRWLockExclusive(&g_contextLock);
    }

    int ConsumeRadioTicket(std::uint32_t now)
    {
        if (!TryAcquireSRWLockExclusive(&g_contextLock))
            return -1;

        const auto ticket = g_radioTicket;
        g_radioTicket = {};
        ReleaseSRWLockExclusive(&g_contextLock);

        const unsigned command = ticket.command;
        if (!command || command > g_radioCommands.size() ||
            now - ticket.time > RadioTicketLifetimeMs || g_radioEpoch - ticket.epoch > RadioTicketFrameLifetime ||
            ticket.generation != g_loadedGeneration)
            return -1;

        return static_cast<int>(command - 1);
    }

    // The callback owns the message lifetime. Its private release-string representation
    // has no schema/native accessor, so guard that view and the cross-thread sender reads.
    bool ReadLocalRadio(const RadioTextMessage* message, int& command)
    {
        bool local = false;
        command = -1;

        __try
        {
            if (!message || !IsSpeaker(g_actor.handle.index(), false, false))
                return false;

            const auto controllerHandle = g_actor.pawn->m_hController();
            const int controllerIndex = controllerHandle.index();

            if (!controllerHandle.valid() || controllerIndex <= 0 || controllerIndex >= ENT_ENTRY_MASK || message->entityIndex != controllerIndex - 1)
                return false;

            auto* controller = I::GameEntity->Instance->Get(controllerIndex);
            if (!controller || controller->handle() != controllerHandle)
                return false;

            local = true; // Any newer local radio text replaces or cancels the old context.

            // The native RadioText handler uses format and parameters, not destination,
            // to interpret this message. Only current entries are valid context.
            if (message->argumentCount < RadioArgumentCount || message->argumentCount > message->argumentCapacity)
                return true;

            const auto* arguments = message->arguments;
            if (!arguments || arguments->allocatedCount < message->argumentCount || arguments->allocatedCount > message->argumentCapacity)
                return true;

            char format[40];
            char mode[16];
            char text[80];
            const auto* formatString = message->Format();

            if (!formatString || !formatString->Copy(format, sizeof(format)) ||
                !arguments->entries[RadioModeArgument] || !arguments->entries[RadioModeArgument]->Copy(mode, sizeof(mode)))
                return true;

            const bool automatic = !std::strcmp(mode, "auto");
            if (*mode && !automatic)
                return true;

            int parameter = -1;
            if (!std::strcmp(format, "#Game_radio"))
                parameter = 1;
            else if (!std::strcmp(format, "#Game_radio_location"))
                parameter = 2;

            if (parameter < 0 || !arguments->entries[parameter] || !arguments->entries[parameter]->Copy(text, sizeof(text)))
                return true;

            for (unsigned i = 0; i < g_radioCommands.size(); ++i)
            {
                if (automatic == g_radioCommands[i].automatic && !std::strcmp(text, g_radioCommands[i].token.c_str()))
                {
                    command = static_cast<int>(i);
                    break;
                }
            }

            return true;
        }
        __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH)
        {
            return local;
        }
    }

    // The caller holds the shared lock and must consume the returned event before unlocking.
    const Hash* Replacement(Hash source, int index, unsigned seed)
    {
        if (!g_activeVoice || !g_actor.pawn || g_appliedGeneration != g_generation.load(std::memory_order_acquire))
            return nullptr;

        const auto mapping = g_activeVoice->general.find(source);
        const bool speech = g_speechHashes.contains(source);

        if ((mapping == g_activeVoice->general.end() && !speech) || g_collisions.contains(source))
            return nullptr;

        const bool jump = source == g_genderHashes[JumpThrow][0] || source == g_genderHashes[JumpThrow][1];
        const bool allowDead = g_postDeathEvents.contains(source);

        if (!IsSpeaker(index, jump, allowDead))
            return nullptr;

        const std::vector<unsigned>* selected = mapping != g_activeVoice->general.end() ? &mapping->second : nullptr;
        if (speech)
        {
            // The server sends RadioText before its speech request. Consume at most the next
            // local speech event. Stale, mismatched and unsupported contexts are discarded.
            const int command = ConsumeRadioTicket(GetTickCount());
            if (command >= 0)
            {
                const auto radio = g_activeVoice->radio[command].find(source);
                if (radio != g_activeVoice->radio[command].end())
                    selected = &radio->second;
            }
        }

        if (!selected || selected->empty())
            return nullptr;

        const auto& choices = *selected;
        for (const auto id : choices)
        {
            if (g_events[id] == source)
                return nullptr;
        }

        const std::size_t first = (seed ^ source) % choices.size();
        for (std::size_t i = 0; i < choices.size(); ++i)
        {
            const auto& event = g_events[choices[(first + i) % choices.size()]];

            // The verified native lookup is a read-only dictionary probe: no locks,
            // callbacks or resource loading while the caller holds the sound mutex.
            if (!g_collisions.contains(event) && g_sound->IsValid(event))
                return &event;
        }

        return nullptr; // Keep the original when no target event is registered.
    }

    void PublishRules(std::unique_ptr<AgentVoiceRules::Index> rules, void* root, std::uint64_t generation, const EventHashes& hashes)
    {
        std::vector<Hash> events;
        std::unordered_set<Hash> collisions{InvalidHash};
        std::unordered_set<Hash> speech;
        std::unordered_set<Hash> postDeath;
        std::unordered_set<Hash> seen;

        events.reserve(rules->events.size() + GenderEventCount);
        seen.reserve(rules->events.size() + GenderEventCount);
        speech.reserve(rules->events.size());

        Hash genders[GenderPairCount][2]{};
        for (unsigned id = 0; id < rules->events.size(); ++id)
        {
            const auto& name = rules->events[id];
            const auto hash = hashes.at(name);

            if (!seen.insert(hash).second)
                collisions.insert(hash);

            events.push_back(hash);
            speech.insert(hash);

            bool death = !rules->sources[id].empty();
            for (auto key : rules->sources[id])
                death &= rules->meanings[key].conceptName == "deathcry";

            if (death)
                postDeath.insert(hash);
        }

        for (unsigned pair = 0; pair < GenderPairCount; ++pair)
        {
            for (unsigned sex = 0; sex < 2; ++sex)
            {
                const auto name = GenderEvents[pair][sex];
                const auto hash = hashes.at(name);

                if (!seen.insert(hash).second)
                    collisions.insert(hash);

                genders[pair][sex] = hash;
                events.push_back(hash);

                // The server emits the fall vocal after applying damage, including
                // lethal damage. Keep post-death eligibility limited to these categories.
                if (pair == Death || pair == DeathTaser || pair == DamageFall)
                    postDeath.insert(hash);
            }
        }

        auto commands = rules->commands;
        decltype(g_voiceCache) retiredVoices;
        retiredVoices.reserve(rules->voices.size()); // Becomes the new empty cache at publication.

        {
            ExclusiveLock lock;
            if (g_stopping || generation != g_generation.load(std::memory_order_acquire) || root != g_refreshRoot)
                return;

            g_actor = {};
            g_definition = 0;
            g_appliedGeneration = 0;
            ClearRadioTicket();

            g_events.swap(events);
            g_collisions.swap(collisions);
            g_speechHashes.swap(speech);
            g_postDeathEvents.swap(postDeath);
            g_radioCommands.swap(commands);
            g_activeVoice = nullptr;
            g_voiceCache.swap(retiredVoices);
            g_prepareFailed = g_warmComplete = false;
            std::memcpy(g_genderHashes, genders, sizeof(genders));
            g_rules.swap(rules);
            g_loadedGeneration = generation;
            g_loadedRoot = root;
        } // Retired strings/maps are destroyed here, outside playback and its lock.
    }

    void RetireCapture()
    {
        if (!g_capture)
            return;

        ExclusiveLock lock;
        g_retiredCaptures.push_back(std::move(g_capture));
    }

    void CaptureRules()
    {
        // Called from FRAME_RENDER_END even in menus/while disabled. Native
        // pointers never reach the preparation thread for dereferencing.
        if (!TryAcquireSRWLockShared(&g_nativeLock))
            return;

        struct Unlock
        {
            ~Unlock()
            {
                ReleaseSRWLockShared(&g_nativeLock);
            }
        } unlock;

        const auto generation = g_generation.load(std::memory_order_acquire);
        auto* manager = I::ResponseManager ? *I::ResponseManager : nullptr;
        auto* root = manager ? manager->Root() : nullptr;

        {
            ExclusiveLock lock;
            if (g_refreshRoot != root || g_refreshGeneration != generation)
            {
                g_refreshRoot = root;
                g_refreshGeneration = generation;
                g_refreshFailed = false;
                g_lastBuildAttempt = 0;
                g_captureSubmitted = false;
                g_prepareFailed = false;

                if (g_capture)
                    g_retiredCaptures.push_back(std::move(g_capture));

                if (g_pendingCapture)
                    g_retiredCaptures.push_back(std::move(g_pendingCapture));

                g_activeVoice = nullptr;
                g_actor = {};
                g_definition = 0;
                g_appliedGeneration = 0;
                ClearRadioTicket();
            }

            if (g_loadedGeneration == generation && g_loadedRoot == root && root)
                return;
        }

        if (!root || g_refreshFailed || g_captureSubmitted)
            return;

        try
        {
            const auto started = std::chrono::steady_clock::now();

            if (!g_capture)
            {
                const auto now = GetTickCount();
                if (g_lastBuildAttempt && now - g_lastBuildAttempt < 1000)
                    return;

                g_lastBuildAttempt = now;

                if (!I::WeaponRegistry)
                    throw std::runtime_error("Native weapon registry binding unavailable");

                auto commands = AgentVoiceRules::ReadCommands(I::WheelBegin, I::WheelEnd, I::WeaponRegistry ? *I::WeaponRegistry : nullptr);
                auto reader = std::make_unique<AgentVoiceRules::Reader>(root, g_symbols, ResponseRules::BucketCount);

                g_capture = std::make_unique<CaptureJob>();
                g_capture->root = root;
                g_capture->generation = generation;
                g_capture->commands = std::move(commands);
                g_capture->reader = std::move(reader);

                for (const auto& pair : GenderEvents)
                {
                    for (const auto name : pair)
                        g_capture->hashes.emplace(name, g_sound->Hash(name, true));
                }
            }

            // 1 millisecond target, checked between bounded batches. A single
            // native entry or call can exceed the target. There is no full copy loop.
            for (unsigned batch = 0; batch < 32; ++batch)
            {
                if (!g_capture->copied)
                {
                    if (g_capture->reader->Step(32))
                    {
                        g_capture->snapshot = g_capture->reader->Take();
                        g_capture->hashes.reserve(g_capture->snapshot.groups.size() * 4 + GenderEventCount);
                        g_capture->copied = true;
                    }
                }
                else
                {
                    for (unsigned n = 0; n < 32 && g_capture->group < g_capture->snapshot.groups.size(); ++n)
                    {
                        const auto& group = g_capture->snapshot.groups[g_capture->group];
                        if (g_capture->response == group.responses.size())
                        {
                            ++g_capture->group;
                            g_capture->response = 0;
                            continue;
                        }

                        const auto& response = group.responses[g_capture->response++];
                        if (response.type != 1)
                            continue;

                        auto name = response.value;
                        for (char& c : name)
                        {
                            if (c >= 'A' && c <= 'Z')
                                c += 'a' - 'A';
                        }

                        if (!name.empty() && !g_capture->hashes.contains(name))
                            g_capture->hashes.emplace(name, g_sound->Hash(name.c_str(), true));
                    }

                    if (g_capture->group == g_capture->snapshot.groups.size())
                    {
                        ExclusiveLock lock;
                        g_pendingCapture = std::move(g_capture);
                        g_captureSubmitted = true;
                        break;
                    }
                }

                if (std::chrono::steady_clock::now() - started >= std::chrono::milliseconds(1))
                    break;
            }
        }
        catch (const AgentVoiceRules::NotReady&)
        {
            RetireCapture();
        }
        catch (const std::exception&)
        {
            RetireCapture();
            g_refreshFailed = true;
        }
    }

    void PrepareVoice(const std::string& voice, std::uint64_t generation, void* root)
    {
        auto selection = AgentVoiceRules::Select(*g_rules, voice);
        auto prepared = std::make_unique<PreparedVoice>();
        prepared->general.reserve(selection.general.size() + GenderEventCount);
        prepared->radio.resize(selection.commands.size());

        // Selection is temporary. Transfer each response list into its final
        // cache entry instead of allocating and copying the list again.
        for (auto& [source, targets] : selection.general)
            prepared->general.emplace(g_events.at(source), std::move(targets));

        for (unsigned command = 0; command < prepared->radio.size(); ++command)
        {
            prepared->radio[command].reserve(selection.commands[command].size());

            for (auto& [source, targets] : selection.commands[command])
                prepared->radio[command].emplace(g_events.at(source), std::move(targets));
        }

        const bool female = voice.find("_fem") != std::string::npos || voice == "fbihrt_epic" || voice == "swat_epic";
        const auto firstGender = static_cast<unsigned>(g_events.size()) - GenderEventCount;

        for (unsigned pair = 0; pair < GenderPairCount; ++pair)
        {
            for (unsigned sex = 0; sex < 2; ++sex)
                prepared->general[g_genderHashes[pair][sex]] = {firstGender + pair * 2 + (female ? 1u : 0u)};
        }

        {
            ExclusiveLock lock;
            if (g_stopping || generation != g_generation.load(std::memory_order_acquire) || root != g_refreshRoot)
                return;

            g_voiceCache.emplace(voice, std::move(prepared));

            if (g_requestedVoice == voice)
                g_requestedVoice.clear();

            g_warmComplete = std::all_of(g_rules->voices.begin(), g_rules->voices.end(), [](const auto& entry)
            {
                return g_voiceCache.contains(entry.first);
            });
        }
    }

}

void AgentVoice::ObserveRadio(const RadioTextMessage* message)
{
    if (!g_stopping.load(std::memory_order_acquire) && TryAcquireSRWLockShared(&g_lock))
    {
        int command;
        if (ReadLocalRadio(message, command))
            StoreRadioTicket(RadioTicket(command, GetTickCount(), g_radioEpoch));

        ReleaseSRWLockShared(&g_lock);
    }
}

std::uint32_t AgentVoice::SoundEvent(std::uint32_t eventHash, int source, std::uint16_t seed)
{
    // All three callers already hold the native recursive sound mutex. Never
    // wait for our state lock, and release it before forwarding to the engine.
    if (!g_stopping.load(std::memory_order_acquire) && TryAcquireSRWLockShared(&g_lock))
    {
        if (const auto event = Replacement(eventHash, source, seed))
            eventHash = *event;

        ReleaseSRWLockShared(&g_lock);
    }

    return eventHash;
}

AgentVoice::LevelChange::LevelChange()
{
    AcquireSRWLockExclusive(&g_nativeLock);
    g_generation.fetch_add(1, std::memory_order_acq_rel);
    Reset();
}

AgentVoice::LevelChange::~LevelChange()
{
    ReleaseSRWLockExclusive(&g_nativeLock);
}

void AgentVoice::Initialize()
{
    g_stopping = false;
    g_sound = I::SoundOpSystem ? I::SoundOpSystem->Events() : nullptr;
    g_symbols = I::ResponseRulesCache ? I::ResponseRulesCache->Symbols() : nullptr;
    g_initialized = g_sound && g_symbols && I::SymbolString && I::ResponseManager && I::EngineClient && I::GameEntity;
}

void AgentVoice::Reset()
{
    AcquireSRWLockExclusive(&g_lock);
    g_actor = {};
    g_definition = 0;
    g_appliedGeneration = 0;
    ClearRadioTicket();
    g_activeVoice = nullptr; // Keep prepared mappings for agent changes/respawns.
    ReleaseSRWLockExclusive(&g_lock);
}

void AgentVoice::Frame(C_CSPlayerPawn* pawn, bool enabled, unsigned short selectedDefinition)
{
    if (!g_initialized || g_stopping.load(std::memory_order_acquire))
        return;

    Actor current;
    const bool valid = enabled && selectedDefinition && Capture(pawn, current);

    if (!valid)
    {
        Reset();
        return;
    }

    AcquireSRWLockExclusive(&g_lock);
    ++g_radioEpoch;

    if (current != g_actor || selectedDefinition != g_definition)
    {
        g_actor = {};
        g_definition = 0;
        ClearRadioTicket();
    }

    ReleaseSRWLockExclusive(&g_lock);
}

void AgentVoice::Applied(C_CSPlayerPawn* pawn, unsigned short definition, const std::string& voice)
{
    if (!g_initialized || g_stopping.load(std::memory_order_acquire))
        return;

    Actor current;
    if (voice.empty() || !Capture(pawn, current))
    {
        Reset();
        return;
    }

    const auto generation = g_generation.load(std::memory_order_acquire);
    bool ready;

    {
        ExclusiveLock lock;
        ready = g_rules && g_loadedGeneration == generation && g_loadedRoot && g_loadedRoot == g_refreshRoot;
        if (ready && g_actor == current && g_definition == definition && g_appliedGeneration == generation)
            return;
    }

    if (!ready)
    {
        Reset();
        return;
    }

    ExclusiveLock lock;

    if (g_stopping || generation != g_generation.load(std::memory_order_acquire) || g_loadedRoot != g_refreshRoot)
        return;

    const auto found = g_voiceCache.find(voice);
    if (found == g_voiceCache.end())
    {
        // The management thread prioritizes this voice during startup warming.
        g_requestedVoice = voice;
        g_activeVoice = nullptr;
        g_actor = {};
        g_definition = 0;
        return;
    }

    ClearRadioTicket();
    g_activeVoice = found->second.get();
    g_actor = current;
    g_definition = definition;
    g_appliedGeneration = generation;
}

void AgentVoice::Update()
{
    if (g_initialized.load(std::memory_order_acquire) && !g_stopping.load(std::memory_order_acquire))
        CaptureRules();
}

void AgentVoice::Prepare()
{
    // Only the DLL's existing MainThread calls this. It owns CPU preparation
    // and disposal, and stops calling before the normal Restore() teardown.
    if (!g_initialized.load(std::memory_order_acquire) || g_stopping.load(std::memory_order_acquire))
        return;

    std::unique_ptr<CaptureJob> job;
    std::vector<std::unique_ptr<CaptureJob>> retired;
    std::uint64_t failureGeneration = 0;
    void* failureRoot = nullptr;

    {
        ExclusiveLock lock;
        job.swap(g_pendingCapture);
        retired.swap(g_retiredCaptures);

        if (!job && (g_prepareFailed || (g_warmComplete && (g_requestedVoice.empty() || g_voiceCache.contains(g_requestedVoice)))))
            return;
    }

    retired.clear(); // Reclaim obsolete captures before building the next generation.

    try
    {
        if (job)
        {
            bool valid;
            {
                ExclusiveLock lock;
                valid = job->generation == g_generation.load() && job->root == g_refreshRoot;
            }

            if (valid)
            {
                failureGeneration = job->generation;
                failureRoot = job->root;

                auto rules = std::make_unique<AgentVoiceRules::Index>(AgentVoiceRules::Build(job->snapshot, std::move(job->commands)));
                if (rules->voices.empty())
                    throw std::runtime_error("Native rules contain no supported voices");

                PublishRules(std::move(rules), job->root, job->generation, job->hashes);
            }
        }

        // The index owns its data. Release staging storage before warming voices.
        job.reset();

        std::string voice;
        std::uint64_t generation;
        void* root;

        {
            ExclusiveLock lock;
            generation = g_loadedGeneration;
            root = g_loadedRoot;

            if (!g_rules || g_prepareFailed || generation != g_generation.load() || root != g_refreshRoot)
                return;

            if (!g_requestedVoice.empty() && !g_voiceCache.contains(g_requestedVoice))
                voice = g_requestedVoice;
            else if (!g_warmComplete)
            {
                for (const auto& [name, meanings] : g_rules->voices)
                {
                    if (!g_voiceCache.contains(name))
                    {
                        voice = name;
                        break;
                    }
                }
            }
        }

        failureGeneration = generation;
        failureRoot = root;

        if (!voice.empty())
            PrepareVoice(voice, generation, root);
    }
    catch (const std::exception&)
    {
        {
            ExclusiveLock lock;
            if (failureGeneration == g_generation.load() && failureRoot == g_refreshRoot)
                g_prepareFailed = true;
        }

        // Capture remains submitted for this root/generation: no repeated
        // expensive rebuild on failure. A level/root change starts a new job.
    }
}

void AgentVoice::Shutdown()
{
    g_stopping.store(true, std::memory_order_release);
    Reset();
    g_initialized = false;
}

namespace
{
    // An agent's voice: its items_game "vo_prefix", or (none set) the model's name - "ctm_fbi_variantb" -> "ctm_fbi" ->
    // "ctm_swat" (the FBI agents speak with the SWAT voice). Empty = unknown (no voice change). Game thread only.
    std::unordered_map<unsigned short, std::string> g_voiceKeys;

    // 0 = no metadata (or unreadable), 1 = metadata without a vo_prefix, 2 = the vo_prefix (in prefix)
    int ReadVoicePrefix(unsigned short definitionIndex, char (&prefix)[128])
    {
        prefix[0] = '\0';

        if (!I::Source2Client || !I::FindKey || !I::GetKeyValueString)
            return 0;

        __try
        {
            CEconItemSystem* system = I::Source2Client->GetEconItemSystem();
            CEconItemSchema* schema = system ? system->GetEconItemSchema() : nullptr;

            if (!schema)
                return 0;

            auto& map = schema->GetSortedItemDefinitionMap();
            const int count = map.Count();

            if (count <= 0 || count > 10000 || !map.m_Elements)
                return 0;

            for (int i = 0; i < count; ++i)
            {
                CEconItemDefinition* definition = map.Element(i).m_Value;

                if (!definition || definition->m_DefinitionIndex != definitionIndex)
                    continue;

                const void* data = definition->GetKeyValues();

                if (!data)
                    return 0;

                const void* key = I::FindKey(data, "vo_prefix");
                const char* text = key ? I::GetKeyValueString(key, nullptr, nullptr, 0) : nullptr;

                if (!text)
                    return 1;

                for (std::size_t c = 0; c < sizeof(prefix); ++c)
                {
                    prefix[c] = text[c];

                    if (!text[c])
                        return 2;
                }

                prefix[0] = '\0';
                return 0;   // longer than any real prefix: not trusted
            }
        }
        __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH)
        {
            prefix[0] = '\0';
        }

        return 0;
    }

    const std::string& VoiceKeyFor(const ItemDefinitionInfo& item)
    {
        if (const auto found = g_voiceKeys.find(item.definitionIndex); found != g_voiceKeys.end())
            return found->second;

        char prefix[128]{};
        const int metadata = ReadVoicePrefix(item.definitionIndex, prefix);
        std::string voice;

        if (metadata == 2)
        {
            voice = prefix;
        }
        else if (metadata == 1)
        {
            voice = item.modelName.substr(item.modelName.find_last_of("/\\") + 1);

            if (const auto dot = voice.find('.'); dot != std::string::npos)
                voice.resize(dot);

            if (const auto variant = voice.find("_var"); variant != std::string::npos)
                voice.resize(variant);

            for (char& c : voice)
            {
                if (c >= 'A' && c <= 'Z')
                    c += 'a' - 'A';
            }

            if (voice == "ctm_fbi")
                voice = "ctm_swat";
        }

        for (char& c : voice)
        {
            if (c >= 'A' && c <= 'Z')
                c += 'a' - 'A';
        }

        return g_voiceKeys.emplace(item.definitionIndex, std::move(voice)).first->second;
    }

    // the model the pawn has on right now
    const char* CurrentModelName(C_CSPlayerPawn* pawn)
    {
        __try
        {
            CGameSceneNode* sceneNode = pawn ? pawn->m_pGameSceneNode() : nullptr;
            CSkeletonInstance* skeleton = sceneNode ? sceneNode->GetSkeletonInstance() : nullptr;
            C_ModelState* modelState = skeleton ? skeleton->m_model_state() : nullptr;
            C_Model* model = modelState ? modelState->m_model() : nullptr;

            return model && model->perm_model_data.name && *model->perm_model_data.name ? model->perm_model_data.name : nullptr;
        }
        __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH)
        {
            return nullptr;
        }
    }
}

void AgentVoice::Drive()
{
    if (!g_initialized.load(std::memory_order_acquire) || g_stopping.load(std::memory_order_acquire))
        return;

    if (!I::EngineClient || !I::EngineClient->connected() || !I::EngineClient->in_game())
    {
        Reset();
        return;
    }

    C_CSPlayerPawn* pawn = I::GetLocalPawn();
    const int team = pawn ? pawn->m_iTeamNum() : 0;
    const bool enabled = Config::Game().enabled;

    // the side's Lootline agent (agents are always per side: T on T, CT on CT)
    unsigned short definition = 0;
    if (enabled && (team == 2 || team == 3))
        definition = Config::GameAgentFor(team == 3 ? Config::TEAM_CT : Config::TEAM_T);

    Frame(pawn, enabled, definition);

    if (!pawn || !definition)
        return;   // Frame already reset it

    if (pawn->m_iHealth() <= 0)
        return;   // dead: keep the voice (the death cry plays after the death)

    const ItemDefinitionInfo* item = Skins::g_ItemSchema.GetItem(definition);

    if (!item || !item->isAgent || !item->hasUsableAgentModel || item->modelName.empty() ||
        (team == 2 && !item->isTerroristAgent) || (team == 3 && !item->isCounterTerroristAgent))
    {
        Reset();
        return;
    }

    // only once the agent changer has put that agent's model on (like Chadware: Applied after a successful SetModel)
    const char* model = CurrentModelName(pawn);

    if (!model || item->modelName != model)
    {
        Reset();
        return;
    }

    Applied(pawn, definition, VoiceKeyFor(*item));
}
