#include "knife_model_changer.h"
#include "../../../patterns.h"
#include "item_schema.h"

#include <cstdint>

#include "../../utils/memory/patternscan/patternscan.h"

#include "../../../cs2/entity/C_EconEntity/C_EconEntity.h"
#include "../../../cs2/entity/C_CSWeaponBase/C_CSWeaponBase.h"

namespace
{
    constexpr int ITEM_QUALITY_UNUSUAL = 3;
using SetModelFn = void(__fastcall*)(void*, const char*);
    using UpdateSubclassFn = void(__fastcall*)(void*);

    SetModelFn GetSetModel()
    {
        const auto fn = M::ResolveCached([] { return reinterpret_cast<SetModelFn>(M::scan("client.dll", Patterns::AgentSetModel)); });
        return fn;
    }

    UpdateSubclassFn GetUpdateSubclass()
    {
        const auto fn = M::ResolveCached([] { return reinterpret_cast<UpdateSubclassFn>(M::scan("client.dll", Patterns::KnifeUpdateSubclass)); });
        return fn;
    }

    bool SetModel(C_CSWeaponBase* weapon, const char* modelName)
    {
        if (!weapon || !modelName || !*modelName)
            return false;

        const auto fn = GetSetModel();

        if (!fn)
            return false;

        fn(weapon, modelName);
        return true;
    }

    bool UpdateSubclass(C_CSWeaponBase* weapon)
    {
        if (!weapon)
            return false;

        const auto fn = GetUpdateSubclass();

        if (!fn)
            return false;

        fn(weapon);
        return true;
    }

    std::uint32_t GetKnifeSubclassId(std::uint16_t definitionIndex)
    {
        switch (definitionIndex)
        {
        case 500: return 3933374535u; // Bayonet
        case 503: return 3787235507u; // Classic Knife
        case 505: return 4046390180u; // Flip Knife
        case 506: return 2047704618u; // Gut Knife
        case 507: return 1731408398u; // Karambit
        case 508: return 1638561588u; // M9 Bayonet
        case 509: return 2282479884u; // Huntsman Knife
        case 512: return 3412259219u; // Falchion Knife
        case 514: return 2511498851u; // Bowie Knife
        case 515: return 1353709123u; // Butterfly Knife
        case 516: return 4269888884u; // Shadow Daggers
        case 517: return 1105782941u; // Paracord Knife
        case 518: return 275962944u;  // Survival Knife
        case 519: return 1338637359u; // Ursus Knife
        case 520: return 3230445913u; // Navaja Knife
        case 521: return 3206681373u; // Nomad Knife
        case 522: return 2595277776u; // Stiletto Knife
        case 523: return 4029975521u; // Talon Knife
        case 525: return 365028728u;  // Skeleton Knife
        case 526: return 3845286452u; // Kukri Knife
        default: return 0;
        }
    }
}

bool KnifeModelChanger::IsKnifeDefinition(std::uint16_t definitionIndex)
{
    return definitionIndex == 41 || definitionIndex == 42 || definitionIndex == 59 ||
        (definitionIndex >= 500 && definitionIndex <= 526);
}

bool KnifeModelChanger::Apply(C_CSWeaponBase* weapon, C_EconItemView* item, std::uint16_t targetDefinitionIndex)
{
    if (!weapon || !item)
        return false;

    if (!IsKnifeDefinition(item->m_iItemDefinitionIndex()) || targetDefinitionIndex < 500)
        return false;

    const ItemDefinitionInfo* targetItem = Skins::g_ItemSchema.GetItem(targetDefinitionIndex);

    if (!targetItem || !targetItem->isKnife || targetItem->modelName.empty())
        return false;

    const std::uint32_t targetSubclassId = GetKnifeSubclassId(targetDefinitionIndex);

    if (targetSubclassId == 0)
        return false;

    if (item->m_iItemDefinitionIndex() == targetDefinitionIndex &&
        static_cast<std::uint32_t>(weapon->m_nSubclassID()) == targetSubclassId)
    {
        return false;
    }

    item->m_iEntityQuality() = ITEM_QUALITY_UNUSUAL;
    item->m_iItemDefinitionIndex() = targetDefinitionIndex;

    // The model first, then the subclass (UC knife-inspect recipe): a SetModel after UpdateSubclass resets the animation
    // graph again and forces another transition - the knife animation restarting a few times.
    if (!SetModel(weapon, targetItem->modelName.c_str()))
        return false;

    weapon->m_nSubclassID() = static_cast<int>(targetSubclassId);

    if (!UpdateSubclass(weapon))
        return false;

    return true;
}
