#pragma once

class C_CSPlayerPawn;

namespace GloveChanger
{
    void Run(C_CSPlayerPawn* localPawn);
    void ForceUpdate();
    void Reset();

    // One line in glove_log.txt with the glove item's state (pawn may be null: then just the text).
    void LogEvent(C_CSPlayerPawn* pawn, const char* what);
}