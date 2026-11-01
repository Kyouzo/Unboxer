#pragma once

namespace Config
{
    struct item_skin_t;
}

namespace WeaponCosmetics
{
    [[nodiscard]] bool HasAny(const Config::item_skin_t& config);
}
