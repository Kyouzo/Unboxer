#pragma once

#include <cstddef>
#include <cstdint>
#include "../../chadware2/utils/schema/schema.h"

// Private parameter storage verified in ReflectWeaponState and its inspect handler.
// These objects are borrowed only inside the native callback that owns them.
struct ParameterNode
{
    uint8_t pad_00[0x18];
    union
    {
        std::uintptr_t symbol;
        bool boolean;
    } value;
};

struct ParameterGraph
{
    uint8_t pad_00[0x10];
    ParameterNode** nodes;
};

struct ParameterReference
{
    uint8_t pad_00[0x8];
    ParameterGraph* graph;
    std::int16_t index;
    uint8_t pad_12[0x6];

    ParameterNode* Node() const
    {
        return graph && graph->nodes && index >= 0 ? graph->nodes[index] : nullptr;
    }
};

class WeaponGraphController
{
public:
    SCHEMA_ARRAY(ParameterReference, Action, "CCS2WeaponGraphController->m_action");
    SCHEMA_ARRAY(ParameterReference, Reset, "CCS2WeaponGraphController->m_bActionReset");
    SCHEMA_ARRAY(ParameterReference, Extra, "CCS2WeaponGraphController->m_inspectExtraInfo");
};

// Private result/cache layout from the documented client analysis.
// These offsets are native layout assumptions, not schema fields.
struct ControllerResults
{
    uint8_t pad_00[0x8];
    void* graph;
    uint8_t pad_10[0x290];
    std::uint16_t cachedState;
    uint8_t pad_2A2[0xE];
    float cachedTime;
    bool complete;
    bool interrupt;
    bool silent;
    bool unknown2B7;
    bool silencerOn;
    bool silencerOff;
    bool blockInspect;
    uint8_t pad_2BB[0x1];
    int phase;
};
