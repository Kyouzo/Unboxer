#pragma once

#include <cstdint>
#include <string>

class C_CSPlayerPawn;
struct RadioTextMessage;

// Agent voice lines, from the new Chadware source (2026-10-07): with a Lootline agent on, your own voice lines (radio
// commands, callouts, death cries, the jump-throw grunt) are that agent's instead of the map's default voice.
namespace AgentVoice
{
    // Optional feature: failure never disables the model changer.
    void Initialize();
    std::uint32_t SoundEvent(std::uint32_t eventHash, int source, std::uint16_t seed);
    void ObserveRadio(const RadioTextMessage* message);

    class LevelChange
    {
    public:
        LevelChange();
        ~LevelChange();
        LevelChange(const LevelChange&) = delete;
        LevelChange& operator=(const LevelChange&) = delete;
    };

    void Shutdown();
    void Reset();
    void Update(); // Game thread: bounded native capture, independent of enabled state.
    void Prepare(); // Existing DLL management thread: owned-data preparation/cache warming.
    void Frame(C_CSPlayerPawn* pawn, bool enabled, unsigned short selectedDefinition);
    void Applied(C_CSPlayerPawn* pawn, unsigned short definition, const std::string& voice);

    // Lootline: game thread, FRAME_RENDER_END right after the agent changer - the side's Lootline agent, and once your
    // model is that agent's, its voice (Chadware calls Frame / Applied from inside its agent changer instead).
    void Drive();
}
