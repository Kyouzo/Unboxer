#pragma once

#include <string>
#include <string_view>

// CS2's chat, for lines only you see: they're printed by your own client, nothing goes to the server or to the other
// players (a client can't put anything in their chats).
namespace GameChat
{
    // One line in your chat (main thread, in a match). `html` may colour parts with <font color="#rrggbb">. False when
    // CS2's chat function wasn't found or the call failed.
    bool Print(const std::string& html);

    // Your name in this match ("" without a local player). Main thread.
    [[nodiscard]] std::string LocalPlayerName();

    // CS2's own "<name> has opened a container and found: <item>" (#Item_FoundInCrate, in the game's language), the
    // item coloured by its rarity like the real one. Plain text in, HTML out.
    [[nodiscard]] std::string FoundInCrateLine(std::string_view playerName, std::string_view itemName, int rarity);
    // CS2's own "<name> has fulfilled a contract and received: <item>" (#Item_Crafted): a trade-up.
    [[nodiscard]] std::string CraftedLine(std::string_view playerName, std::string_view itemName, int rarity);

    [[nodiscard]] std::string EscapeHtml(std::string_view text);
}
