#pragma once

#include <cstdint>

class C_CSWeaponBase;
class C_EconItemView;

namespace KnifeModelChanger
{
    [[nodiscard]] bool IsKnifeDefinition(std::uint16_t definitionIndex);
    bool Apply(C_CSWeaponBase* weapon, C_EconItemView* item, std::uint16_t targetDefinitionIndex);
}