#pragma once

#include <cstdint>

#include "../../utils/memory/vfunc/vfunc.h"

class ISoundEventSystem
{
public:
    std::uint32_t Hash(const char* name, bool create)
    {
        return M::CallVFunc<std::uint32_t, 0>(this, name, create);
    }

    bool IsValid(std::uint32_t hash)
    {
        return M::CallVFunc<bool, 1>(this, hash);
    }
};

class ISoundOpSystem
{
    void* vtable;

public:
    ISoundEventSystem* Events()
    {
        return reinterpret_cast<ISoundEventSystem*>(this + 1);
    }
};
