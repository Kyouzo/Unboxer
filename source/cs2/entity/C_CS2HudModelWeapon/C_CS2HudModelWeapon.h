#pragma once

#include <cstddef>
#include <cstdint>
#include "../handle.h"

// Partial layouts for private HUD data not exposed by the entity schema.
struct HudAddonEntry
{
    std::byte unknown[0x0C];
    CBaseHandle entityHandle;
};

struct HudAddonDictionary
{
    std::uint32_t count;
    std::uint32_t allocationFlags;
    HudAddonEntry* entries;

    [[nodiscard]] std::uint32_t Capacity() const { return allocationFlags & 0x7FFFFFFFu; }
};

struct HudWeaponAddonLayout
{
    std::byte unknown0000[0x1328];
    HudAddonDictionary addons;
    std::byte unknown1338[0x20];
    std::uint8_t keychainState;
};
