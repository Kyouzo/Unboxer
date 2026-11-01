#pragma once

#include "../C_BaseEntity/C_BaseEntity.h"
#include "../../datatypes/weapon_graph.h"

class C_BaseAnimGraph : public C_BaseEntity
{
public:
    schema(WeaponGraphController*, MainController, "CBaseAnimGraph->m_pMainGraphController");
};
