#pragma once

#include <cstddef>
#include <cstdint>

struct HudWeaponSelectionEntry
{
    std::byte unknown00[0x38];
    std::int32_t weaponHandle;
    std::byte unknown3C[0x0C];
};

struct HudWeaponSelectionLayout
{
    std::byte unknown00[0x50];
    std::int32_t count;
    std::byte unknown54[0x04];
    HudWeaponSelectionEntry* entries;
    std::byte unknown60[0x38];
    void* hudElementVtable;

    [[nodiscard]] static HudWeaponSelectionLayout* FromHudElement(std::uintptr_t element)
    {
        return reinterpret_cast<HudWeaponSelectionLayout*>(element - offsetof(HudWeaponSelectionLayout, hudElementVtable));
    }
};
