#pragma once

#include "../C_BaseEntity/C_BaseEntity.h"

class C_BaseGrenade : public C_BaseEntity
{
public:
    schema(CBaseHandle, m_hThrower, "C_BaseGrenade->m_hThrower");
};
