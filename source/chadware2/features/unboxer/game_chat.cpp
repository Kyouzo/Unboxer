#include "game_chat.h"

#include <Windows.h>

#include <cstdint>
#include <cstring>
#include <iterator>

#include "../skinchanger/item_schema.h"
#include "../../interfaces/interfaces.h"
#include "../../utils/memory/patternscan/patternscan.h"
#include "../../utils/schema/schema.h"
#include "../../../patterns.h"
#include "../../../cs2/entity/C_CSPlayerPawn/C_CSPlayerPawn.h"

namespace
{
    // client.dll's chat print, the one the real "found in a container" message goes through:
    // (unused, const uint8_t flags[2], int playerIndex, const char* format, ...). Flags { 1, 1 } like that message:
    // the text exactly as given (no "name:" in front), no copy in the console.
    using ChatPrintFn = void (*)(void* unused, const std::uint8_t* flags, int playerIndex, const char* format, ...);

    ChatPrintFn ChatPrint()
    {
        return M::ResolveCached([] { return reinterpret_cast<ChatPrintFn>(M::scan("client.dll", Patterns::ChatPrint)); });
    }

    bool CallChatPrint(ChatPrintFn print, const char* text)
    {
        const std::uint8_t flags[2] = { 1, 1 };

        __try
        {
            print(nullptr, flags, -1, "%s", text);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    // CBasePlayerController::m_iszPlayerName is a char[128].
    bool ReadName(const char* source, char (&out)[128])
    {
        __try
        {
            std::memcpy(out, source, sizeof(out));
            out[sizeof(out) - 1] = '\0';
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    // items_game "rarities" value -> its "colors" hex (desc_default ... desc_immortal).
    const char* RarityColor(int rarity)
    {
        static constexpr const char* colors[] = { "#ded6cc", "#b0c3d9", "#5e98d9", "#4b69ff", "#8847ff", "#d32ce6", "#eb4b4b", "#e4ae39" };
        return rarity >= 0 && rarity < static_cast<int>(std::size(colors)) ? colors[rarity] : colors[0];
    }

    // A localized "%s1 ... %s2" line: the name as it is, the item in its rarity colour.
    std::string ItemLine(const char* token, const char* fallback, std::string_view playerName, std::string_view itemName, int rarity)
    {
        std::string format = ItemSchema::Localize(token);

        if (format.find("%s1") == std::string::npos || format.find("%s2") == std::string::npos)
            format = fallback;

        const std::string name = GameChat::EscapeHtml(playerName);
        const std::string item = std::string("<font color=\"") + RarityColor(rarity) + "\">" + GameChat::EscapeHtml(itemName) + "</font>";

        // one pass, so a name with "%s2" in it stays as it is
        std::string line;
        line.reserve(format.size() + name.size() + item.size());

        for (std::size_t i = 0; i < format.size(); ++i)
        {
            if (format[i] == '%' && i + 2 < format.size() && format[i + 1] == 's' && format[i + 2] >= '1' && format[i + 2] <= '9')
            {
                line += format[i + 2] == '1' ? name : format[i + 2] == '2' ? item : std::string();
                i += 2;
                continue;
            }

            line += format[i];
        }

        return line;
    }
}

bool GameChat::Print(const std::string& html)
{
    const ChatPrintFn print = ChatPrint();
    return print && !html.empty() && CallChatPrint(print, html.c_str());
}

std::string GameChat::LocalPlayerName()
{
    C_CSPlayerPawn* pawn = I::GetLocalPawn();

    if (!pawn || !I::GameEntity || !I::GameEntity->Instance)
        return {};

    void* controller = I::GameEntity->Instance->Get<C_BaseEntity>(pawn->m_hController());
    const std::uint32_t offset = SchemaFinder::GetCached<hash_32_fnv1a_const("CBasePlayerController->m_iszPlayerName")>();

    if (!controller || !offset)
        return {};

    char name[128] = {};
    if (!ReadName(static_cast<const char*>(controller) + offset, name))
        return {};

    return name;
}

std::string GameChat::EscapeHtml(std::string_view text)
{
    std::string out;
    out.reserve(text.size());

    for (const char c : text)
    {
        switch (c)
        {
        case '&': out += "&amp;"; break;
        case '<': out += "&lt;"; break;
        case '>': out += "&gt;"; break;
        case '"': out += "&quot;"; break;
        default: out += c; break;
        }
    }

    return out;
}

std::string GameChat::FoundInCrateLine(std::string_view playerName, std::string_view itemName, int rarity)
{
    // " %s1 has opened a container and found: %s2" / "Игрок %s1 достал из кейса: %s2"
    return ItemLine("#Item_FoundInCrate", " %s1 has opened a container and found: %s2", playerName, itemName, rarity);
}

std::string GameChat::CraftedLine(std::string_view playerName, std::string_view itemName, int rarity)
{
    return ItemLine("#Item_Crafted", " %s1 has fulfilled a contract and received: %s2", playerName, itemName, rarity);
}
