#pragma once

#include <array>
#include <cstddef>

class C_EconItemView;

namespace EconItemAttributeManager
{
    constexpr std::size_t STICKER_SLOT_COUNT = 6;   // "sticker slot 0-5" (CS2 guns have up to 6 positions)

    struct StickerData
    {
        bool enabled = false;
        int kit = 0;

        float wear = 0.0f;
        float scale = 1.0f;
        float rotation = 0.0f;

        float offsetX = 0.0f;
        float offsetY = 0.0f;

        int schema = -1;            // the gun's sticker position it's drawn relative to; -1: SetStickerSchemaFromSlot decides
    };

    struct KeychainData
    {
        bool enabled = false;
        int id = 0;
        int seed = 0;

        float offsetX = 0.0f;
        float offsetY = 0.0f;
        float offsetZ = 0.0f;

        int sticker = 0;            // a Sticker Slab's sealed sticker kit ("keychain slot 0 sticker"), 0 = none
    };

    using StickerArray = std::array<StickerData, STICKER_SLOT_COUNT>;

    void Create(C_EconItemView* item, int paintKit, float wear, int seed, int statTrak = -1, int statTrakScoreType = 0, const StickerArray* stickers = nullptr, const KeychainData* keychain = nullptr);
    void Remove(C_EconItemView* item);

    // Replaces only sticker-related attributes on an existing item while preserving
    // paint, StatTrak, keychain and any other attributes already populated by the game.
    bool SyncStickers(C_EconItemView* item, const StickerArray& stickers);

    // Updates the existing raw keychain-ID attribute without reallocating the list.
    bool SetKeychainId(C_EconItemView* item, int id);

    // Charm id the item carries in its attribute lists (0 = none). `includeMainList` = also look at m_AttributeList,
    // which only holds the real inventory values until the skin changer rewrites it; m_NetworkedDynamicAttributes is
    // never touched by us.
    int GetKeychainId(C_EconItemView* item, bool includeMainList);

    // "sticker slot N schema" is the slot whose POSITION a sticker is drawn at (its offsets are relative to it; the game
    // reads it as the sticker's slot when it's there). Off (the default, Chadware): 0, which the click-to-place
    // calibration was learned against. On (Lootline): the sticker's own slot, like CS2's sticker popup places it.
    void SetStickerSchemaFromSlot(bool fromSlot);

    // A skin put on a real gun replaces the WHOLE item: the real item's own stickers, charm, StatTrak and name - the
    // server sends them in the item's networked attributes - don't show through. Off (the default, Chadware): kept, as
    // before. On (Lootline).
    void SetReplaceRealItem(bool replace);
    [[nodiscard]] bool ReplaceRealItem();
    // The item has networked attributes (the real item's, from the server).
    [[nodiscard]] bool HasNetworkedAttributes(C_EconItemView* item);
    // Empties them (the vector keeps its memory: the game frees / refills it as usual). True = a charm was among them.
    bool ClearNetworkedAttributes(C_EconItemView* item);
}
