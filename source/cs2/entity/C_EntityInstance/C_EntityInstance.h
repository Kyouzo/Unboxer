#pragma once

#include <cstdint>
#include <cstddef>

#include "../handle.h"
#include "../../../chadware2/utils/memory/vfunc/vfunc.h"
#include "../../../chadware2/utils/schema/schema.h"

struct CEntityIdentity
{
    uint8_t pad_00[0x10];
    std::uint32_t index; // 0x10

    schema(std::uint32_t, flags, "CEntityIdentity->m_flags");

    [[nodiscard]] CBaseHandle handle()
    {
        const auto value = index;
        return value == INVALID_EHANDLE_INDEX ? CBaseHandle{} : CBaseHandle(value & ENT_ENTRY_MASK, (value >> 15) - (flags() & 1));
    }
};

class CEntityInstance
{
public:
    schema(CEntityIdentity*, m_pEntityIdentity, "CEntityInstance->m_pEntity");

    // Native entity slot 47 writes the engine-owned schema class through its output argument.
    void dump_class_info(SchemaClassInfoData_t** result) const
    {
        M::CallVFunc<void, 47U>(this, result);
    }

    [[nodiscard]] const char* get_entity_class_name() const
    {
        SchemaClassInfoData_t* info = nullptr;
        dump_class_info(&info);
        return info ? info->szName : nullptr;
    }

    [[nodiscard]] CBaseHandle handle()
    {
        auto* identity = m_pEntityIdentity();
        return identity ? identity->handle() : CBaseHandle{};
    }
};
