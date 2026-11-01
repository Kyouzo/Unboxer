#pragma once
#include <memory>

class C_CSPlayerPawn;

struct Globals_t
{
    C_CSPlayerPawn* local_pawn = nullptr;
};

inline const auto g_ctx = std::make_unique<Globals_t>();
