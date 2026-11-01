#pragma once

#include <cstddef>
#include <cstdint>

struct ResourceBinding_t
{
    void* pData;
    std::byte unknown08[0x18];
    std::int32_t strongReferences;
};

template<typename T>
class C_StrongHandle
{
public:
    operator T*() const
    {
        return pBinding ? static_cast<T*>(pBinding->pData) : nullptr;
    }

    const ResourceBinding_t* pBinding;
};
