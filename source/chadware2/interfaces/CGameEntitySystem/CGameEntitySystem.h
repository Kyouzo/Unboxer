#pragma once

#include <cstdint>

#include "../../../patterns.h"
#include "../../../cs2/entity/handle.h"
#include "../../utils/memory/memorycommon.h"
#include "../../utils/memory/patternscan/patternscan.h"

class C_BaseEntity;

class CGameEntitySystem
{
public:
    template <typename T = C_BaseEntity>
    T* Get(int index)
    {
        return reinterpret_cast<T*>(GetEntityByIndex(index));
    }

    template <typename T = C_BaseEntity>
    T* Get(CBaseHandle handle)
    {
        return handle.valid() ? Get<T>(handle.index()) : nullptr;
    }

private:
    void* GetEntityByIndex(int index)
    {
        using Fn = void* (__fastcall*)(void*, int);
        const auto fn = M::ResolveCached([] { return reinterpret_cast<Fn>(M::scan("client.dll", Patterns::GetBaseEntity)); });
        return fn ? fn(this, index) : nullptr;
    }
};

class IGameResourceService
{
public:
    MEM_PAD(0x58);
    CGameEntitySystem* Instance = nullptr;
};
