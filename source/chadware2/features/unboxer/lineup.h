#pragma once

#include <string>

// The lineups: the team intro, the team select and the end of the match show every player as a character
// (C_CSGO_TeamPreviewCharacterPosition and the classes built on it - TeamSelect T/CT, TeamIntro T/CT, WingmanIntro,
// EndOfMatch). The server fills a position with your REAL agent, gloves and weapon; yours gets your Lootline agent,
// gloves and the Lootline skin of the weapon it holds instead. Only you see it. Lootline only.
namespace Lineup
{
    // The builder hook, on the thread the game builds on: before - your position's items become your Lootline ones
    // (only your own position, only what Lootline has something for); after - your character and its gun are queued
    // for FrameTick. SEH-guarded.
    void BeforeBuild(void* position);
    void AfterBuild(void* position);

    // FrameStageNotify, FRAME_NET_UPDATE_END (where the skin changer works): the skin changer's recipe on your lineup
    // character - the gun's skin material and the gloves' rebuild flag on 4 frames in a row and again 0.4 / 1.2 / 3 s
    // later, then the game's RegenerateSkins. SEH-guarded.
    void FrameTick();

    // Not the lineup - the match (it reads skin materials the same way): one line about each weapon the pawn carries -
    // the skin it should have and has, whether the game put the real item back, and whether its paint is done on the
    // world model and on the first-person one ("knife 507: skin 568 (wanted 568), paint 1 done / 0 waiting, first
    // person 1 / 0"). For the log's spawn check. Game thread.
    std::string DescribeYourWeapons(void* pawn);

    // Not the lineup - the match: a skin material of a weapon you carry (world or first-person model) that waits for an
    // identical request that never finishes is made on its own after 1 s (the lineups' Unshare). Without it one knife /
    // gun stayed unpainted - or vanished in first person - for a whole session, and reapplying didn't help (the same
    // request waits again). Logs "in match: your N's skin ... made on its own now". FSN NET_UPDATE_END, 4x a second.
    void UnstickYourWeapons(void* pawn);

    // Everywhere (menus, the 3D preview, any model): a skin material request waiting > 2 s for an identical one that is
    // gone or stopped advancing is made on its own (one still working is left alone - sharing works as before). The
    // preview's new colours waited for the in-match knife's identical request. FrameStageNotify, 4x a second.
    void UnstickDeadWaits();
}
