#pragma once

#include <cstdint>
#include <string>
#include <vector>

// Game-update kit. Everything in Chadware\offsets\ is compiled into the DLL (see offsets\README.txt):
//   offsets.json / buttons.json / info.json   the cs2-dumper dump; paste the new ones after a CS2 update
//   patterns.txt                               your own signatures, replacing the built-in ones by name
// Signatures stay the main way Chadware finds things. The dump is only the backup for what a dump covers (Input,
// local pawn, jump button), and only while it was dumped from the build that is running.
namespace Offsets
{
    int DumpBuild();                    // info.json build_number, 0 = no dump (only shown, some dumps mislabel it)
    int GameBuild();                    // the running game's build (engine2 + the dump's dwBuildNumber), 0 = unknown
    bool DumpIsFresh();                 // the dump's offsets fit the running game (checked on the jump button)

    // module base + offset from a FRESH dump; 0 when the dump is missing, stale or doesn't have it
    std::uintptr_t FreshClient(const char* name);   // offsets.json -> client.dll -> name
    std::uintptr_t FreshButton(const char* name);   // buttons.json -> client.dll -> name

    // patterns.txt replacement for this built-in signature, or nullptr
    const std::string* PatternOverride(const std::string& builtIn);
    int OverrideCount();
    std::vector<std::string> UnknownOverrideNames();    // names in patterns.txt that aren't Chadware signatures (typos)

    // Settings -> Game Update -> "Check Signatures" (runs on its own thread)
    struct SignatureResult
    {
        const char* name = nullptr;
        const char* module = nullptr;
        bool found = false;
        bool overridden = false;        // came from patterns.txt
        bool rescued = false;           // not found, but the fresh dump covers it
    };

    void StartSignatureCheck();
    bool SignatureCheckRunning();
    std::vector<SignatureResult> SignatureResults();

    // A broken signature whose feature found its way without it (a fallback): the check reports it rescued, not broken.
    void MarkCovered(const char* signature);

    void Shutdown();                    // unload: wait for a running check
}
