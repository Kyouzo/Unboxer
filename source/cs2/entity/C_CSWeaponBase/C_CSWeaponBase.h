#pragma once

#include <cstdint>

#include "../C_EconEntity/C_EconEntity.h"
#include "../handle.h"
#include "../../../chadware2/utils/math/utlvector/utlvector.h"
#include "../../../chadware2/utils/memory/patternscan/patternscan.h"
#include "../../../chadware2/utils/schema/schema.h"
#include "../../../patterns.h"

class CCSPlayer_WeaponServices
{
public:
    schema(c_utl_vector<CBaseHandle>, m_hMyWeapons, "CPlayer_WeaponServices->m_hMyWeapons");
    schema(CBaseHandle, m_hActiveWeapon, "CPlayer_WeaponServices->m_hActiveWeapon");
    schema(bool, InspectBlocked, "CCSPlayer_WeaponServices->m_bBlockInspectUntilNextGraphUpdate");
};

class C_CSWeaponBase : public C_EconEntity
{
public:
    // inspect state (the knife inspect fix, knife_animation.cpp)
    schema(std::uint16_t, State, "C_CSWeaponBase->m_iWeaponGameplayAnimState");
    schema(float, StateTime, "C_CSWeaponBase->m_flWeaponGameplayAnimStateTimestamp");
    schema(float, CancelTime, "C_CSWeaponBase->m_flInspectCancelCompleteTime");
    schema(bool, Pending, "C_CSWeaponBase->m_bInspectPending");
    schema(bool, ShouldLoop, "C_CSWeaponBase->m_bInspectShouldLoop");

    bool AddKeyChainEntity()
    {
        using Fn = bool(__fastcall*)(C_CSWeaponBase*, C_EconItemView*, bool);
        const auto fn = M::ResolveCached([] { return reinterpret_cast<Fn>(M::scan("client.dll", Patterns::AddKeyChainEntity)); });
        if (!fn)
            return false;

        C_AttributeContainer* attributeManager = m_AttributeManager();
        C_EconItemView* item = attributeManager ? attributeManager->m_Item() : nullptr;
        return item ? fn(this, item, false) : false;
    }
};
