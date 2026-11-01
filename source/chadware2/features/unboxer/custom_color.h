#pragma once

#include <cstdint>
#include <vector>

// Custom skin colours (ported from Nerv): a gun's / knife's / gloves' paint drawn in colours you pick.
//
// CS2 puts a skin's material together from the paint's own palette (the material's g_vColor0..3). While the game
// builds it, four COLOR4 "loose variables" with those names are added to its list - they win over the paint's own.
// The three builders are hooked (in-match guns + knives, CS2's screens + the skin changer's own builds, gloves) and so
// is the list's append: the first append a builder makes gets the colours behind it.
//
// Lootline items carry their colours (Unboxer::Item::paint): CS2's own screens (inventory, inspect, loadout) find them
// by item id, a match by definition + side (the skin changer's items have item id high -1), the menu's 3D preview by
// its temporary item.
namespace CustomColor
{
    enum Mode : int
    {
        MODE_SOLID = 0,       // the whole palette one colour
        MODE_TINT = 1,        // only the base colour - the pattern's accents stay
        MODE_TWO_TONE = 2,    // the base colour + an accent colour on the pattern
        MODE_GRADIENT = 3,    // the first colour fading into the second over the palette
        MODE_COUNT = 4,
    };

    // Colours are bytes R, G, B, A (0xAABBGGRR as a number) - how the game keeps a Color.
    struct Paint
    {
        bool on = false;
        int mode = MODE_SOLID;
        std::uint32_t primary = 0xFFFF00FFu;     // magenta
        std::uint32_t secondary = 0xFFFFFF00u;   // cyan: the two-tone accent / the gradient's end

        bool operator==(const Paint&) const = default;
    };

    struct Entry
    {
        std::uint64_t id = 0;              // the Lootline item (CS2's inventory, inspect, loadout screens)
        std::uint16_t definition = 0;      // in a match: the skin changer's gun / knife / gloves of this definition ...
        std::uint8_t sides = 0;            // ... on these sides (Unboxer::EQUIP_T 1 / EQUIP_CT 2)
        Paint paint;
    };

    // H::Hooks::init (the start-up thread). False = a signature didn't match: no colours, nothing else changes.
    bool Install();
    void Uninstall();                      // H::Hooks::shutdown

    // Game thread, every frame (FSN NET_UPDATE_END, before the skin changer): your pawn (null = none). A match colours only
    // the items of the weapons you carry and your gloves - CS2 gives every DEFAULT item id high -1 as well (bots' guns,
    // another player's stock knife), so the definition alone coloured theirs too.
    void UpdateYourItems(void* pawn);

    // Any thread: every coloured Lootline item (replaces the last list).
    void Publish(std::vector<Entry> entries);
    // The menu's 3D preview (ModelPreviewItem, main thread): the temporary item CS2 made for it and its colours.
    void SetPreview(std::uint64_t itemId, const void* itemView, const Paint& paint);
}
