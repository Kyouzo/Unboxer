#pragma once

#include <D3D11.h>
#include <string>
#include <vector>

#include "../../../features/skinchanger/item_schema.h"

namespace SkinImages
{
    // Render thread, at the start of every menu frame: the picture-loading budget starts again (new pictures are loaded
    // ~4 ms a frame at most; one that has to wait is null for now and comes a frame or two later).
    void BeginFrame();

    struct Image
    {
        ID3D11ShaderResourceView* texture = nullptr;
        unsigned int width = 0;
        unsigned int height = 0;

        [[nodiscard]] explicit operator bool() const
        {
            return texture != nullptr;
        }
    };

    bool Initialize(ID3D11Device* device);
    void Shutdown();

    void PrecacheAll();

    const Image* GetItemIcon(const ItemDefinitionInfo& item);
    const Image* GetPaintKitImage(const ItemDefinitionInfo& item, const PaintKitInfo& paintKit);
    const Image* GetModelPreview(const ItemDefinitionInfo& item);

    void UpdateStickerCache(const std::vector<const StickerKitInfo*>& stickers);
    void ClearStickerCache();
    const Image* GetStickerImage(const StickerKitInfo& sticker);

    // Music kit artwork: panorama/images/<image_inventory>_png.vtex_c (e.g. econ/music_kits/valve_01).
    const Image* GetMusicKitImage(const MusicKitInfo& kit);
    const Image* GetKeychainImage(const KeychainInfo& keychain);

    // Any items_game "image_inventory" path (cases: econ/weapon_cases/<name>, keys: econ/tools/<name>).
    const Image* GetInventoryImage(const std::string& image);
    // A PNG in memory (Lootline: container pictures the game doesn't ship, compiled into the DLL), cached under `key`.
    const Image* GetEncodedImage(const std::string& key, const std::uint8_t* data, std::size_t size);
    // A panorama UI icon by name ("pet_chick" -> panorama/images/icons/ui/pet_chick.vsvg_c), white.
    const Image* GetUiIcon(const std::string& name);

}
