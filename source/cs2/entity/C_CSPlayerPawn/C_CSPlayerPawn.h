#pragma once

#include <cstdint>
#include "../C_BaseEntity/C_BaseEntity.h"
#include "../C_CSWeaponBase/C_CSWeaponBase.h"
#include "../../../chadware2/utils/memory/vfunc/vfunc.h"
#include "../../../chadware2/utils/schema/schema.h"
#include "../../../cs2/datatypes/utlstronghandle/utlstronghandle.h"
#include "../../../patterns.h"

class C_EconItemView;

enum class Team : std::uint8_t
{
    UNASSIGNED = 0,
    T = 2,
    CT = 3
};

class C_PermModelData
{
public:
    const char* name = nullptr;
};

class C_Model
{
public:
    void* vtable = nullptr;
    C_PermModelData perm_model_data{};
};

class C_ModelState
{
public:
    schema(C_StrongHandle<C_Model>, m_model, "CModelState->m_hModel");
};

class CSkeletonInstance;

// Private invalidation byte; the schema exposes the rotation values separately.
struct SceneNodeTransformLayout
{
    std::byte unknown000[0x110];
    std::uint8_t dirty;
};

class CGameSceneNode
{
public:
    schema(CEntityInstance*, GetOwner, "CGameSceneNode->m_pOwner");
    schema(CGameSceneNode*, GetChild, "CGameSceneNode->m_pChild");
    schema(CGameSceneNode*, GetNextSibling, "CGameSceneNode->m_pNextSibling");
    schema(Vector_t, m_angWrappedLocalRotation, "CGameSceneNode->m_angWrappedLocalRotation");
    schema(Vector_t, m_angAbsRotation, "CGameSceneNode->m_angAbsRotation");

    [[nodiscard]] std::uint8_t& TransformDirty() { return reinterpret_cast<SceneNodeTransformLayout*>(this)->dirty; }

    CSkeletonInstance* GetSkeletonInstance()
    {
        return M::CallVFunc<CSkeletonInstance*, 13U>(this);
    }

    void SetMeshGroupMask(std::uint64_t mask)
    {
        using Fn = void(__fastcall*)(CGameSceneNode*, std::uint64_t);
        const auto fn = M::ResolveCached([] { return reinterpret_cast<Fn>(M::scan("client.dll", Patterns::SetMeshGroupMask)); });
        if (fn)
            fn(this, mask);
    }
};

class CSkeletonInstance : public CGameSceneNode
{
public:
    SCHEMA_ARRAY(C_ModelState, m_model_state, "CSkeletonInstance->m_modelState");
};

class C_CSPlayerPawn : public C_BaseEntity
{
public:
    schema(CCSPlayer_WeaponServices*, m_pWeaponServices, "C_BasePlayerPawn->m_pWeaponServices");
    schema(CBaseHandle, m_hHudModelArms, "C_CSPlayerPawn->m_hHudModelArms");
    SCHEMA_ARRAY(C_EconItemView, m_EconGloves, "C_CSPlayerPawn->m_EconGloves");
    schema(bool, m_bNeedToReApplyGloves, "C_CSPlayerPawn->m_bNeedToReApplyGloves");
    schema(float, m_flLastSpawnTimeIndex, "C_CSPlayerPawnBase->m_flLastSpawnTimeIndex");
    schema(CBaseHandle, m_hController, "C_BasePlayerPawn->m_hController");

    Team getTeam()
    {
        return static_cast<Team>(m_iTeamNum());
    }
};
