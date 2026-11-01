#include "weapon_cosmetics.h"

#include "../../config/config.h"

bool WeaponCosmetics::HasAny(const Config::item_skin_t& config)
{
    for (const auto& sticker : config.stickers)
    {
        if (sticker.enabled && sticker.kit > 0)
            return true;
    }

    return config.keychain.enabled && config.keychain.id > 0;
}
