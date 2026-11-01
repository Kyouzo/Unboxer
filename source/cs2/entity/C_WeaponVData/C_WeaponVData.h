#pragma once

#include <cstdint>
#include "../../../chadware2/utils/schema/schema.h"
#include "../../datatypes/response_rules.h"

struct WeaponShootSound
{
    uint8_t links[0x10];
    unsigned id;
    uint8_t pad_14[0x8];
    unsigned flags;
    union
    {
        const char* heap;
        char local[8];
    } text;

    const char* Text() const
    {
        return flags & 0x40000000 ? text.local : text.heap;
    }
};

struct WeaponShootSounds
{
    uint8_t pad_00[0x8];
    ResponseRules::Tree tree;
};

class C_WeaponVData
{
    uint8_t pad_00[0x8];
    int type;
    uint8_t pad_0C[0x14];
    bool isTemplate;

public:
    bool IsWeapon() const
    {
        return type == 1 && !isTemplate;
    }

    schema(const char*, m_szUseRadioSubtitle, "CCSWeaponBaseVData->m_szUseRadioSubtitle");
    SCHEMA_ARRAY(WeaponShootSounds, m_aShootSounds, "CBasePlayerWeaponVData->m_aShootSounds");
};

struct WeaponDefinitionSlot
{
    std::int32_t hash;
    uint8_t pad_04[0xC];
    C_WeaponVData* data;
};

struct WeaponDefinitionTable
{
    unsigned unknown000;
    unsigned capacityFlags;
    WeaponDefinitionSlot* data;
    unsigned count;
    unsigned buckets;

    unsigned Capacity() const
    {
        return capacityFlags & 0x7fffffff;
    }
};

struct WeaponDefinitionRegistry
{
    uint8_t pad_00[0x68];
    WeaponDefinitionTable definitions;
};
