#pragma once

#include <cstdint>
#include "../C_EntityInstance/C_EntityInstance.h"
#include "../handle.h"
#include "../../../chadware2/utils/math/vector/vector.h"
#include "../../../chadware2/utils/schema/schema.h"

class CGameSceneNode;

class CCollisionProperty
{
public:
    schema(Vector_t, m_vecMins, "CCollisionProperty->m_vecMins");
    schema(Vector_t, m_vecMaxs, "CCollisionProperty->m_vecMaxs");
};

class C_BaseEntity : public CEntityInstance
{
public:
    schema(std::int32_t, m_iHealth, "C_BaseEntity->m_iHealth");
    schema(std::uint8_t, m_iTeamNum, "C_BaseEntity->m_iTeamNum");
    schema(CGameSceneNode*, m_pGameSceneNode, "C_BaseEntity->m_pGameSceneNode");
    schema(CCollisionProperty*, m_pCollision, "C_BaseEntity->m_pCollision");
    schema(CBaseHandle, m_hOwnerEntity, "C_BaseEntity->m_hOwnerEntity");
    schema(int, m_nSubclassID, "C_BaseEntity->m_nSubclassID");
};
