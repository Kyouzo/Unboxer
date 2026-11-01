#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include "../fnv1a/fnv1a.h"
#include "../../../cs2/datatypes/schema/ISchemaClass/ISchemaClass.h"

#define SCHEMA_ADD_OFFSET(TYPE, NAME, OFFSET) \
    [[nodiscard]] inline std::add_lvalue_reference_t<TYPE> NAME() \
    { \
        const std::uint32_t uOffset = OFFSET; \
        return *reinterpret_cast<std::add_pointer_t<TYPE>>(reinterpret_cast<std::uint8_t*>(this) + uOffset); \
    }

#define SCHEMA_ARRAY(TYPE, NAME, FIELD) \
    [[nodiscard]] inline TYPE* NAME() \
    { \
        const std::uint32_t uOffset = SchemaFinder::GetCached<hash_32_fnv1a_const(FIELD)>(); \
        return reinterpret_cast<TYPE*>(reinterpret_cast<std::uint8_t*>(this) + uOffset); \
    }

#define schema(TYPE, NAME, FIELD) SCHEMA_ADD_OFFSET(TYPE, NAME, SchemaFinder::GetCached<hash_32_fnv1a_const(FIELD)>())

class Schema
{
public:
    bool init(const char* moduleName, int moduleType);
    ISchemaSystem* schema_system = nullptr;
};

namespace SchemaFinder
{
    [[nodiscard]] std::uint32_t Get(std::uint32_t hashedName);

    template <std::uint32_t FieldHash>
    [[nodiscard]] std::uint32_t GetCached()
    {
        // Schema::init completes before hooks are installed. The cache itself
        // needs no dynamic local-static initialization on a game-owned thread.
        static constinit std::atomic<std::uint32_t> cached{ 0 };
        std::uint32_t offset = cached.load(std::memory_order_acquire);
        if (offset == 0)
        {
            offset = Get(FieldHash);
            if (offset != 0)
                cached.store(offset, std::memory_order_release);
        }
        return offset;
    }
}
