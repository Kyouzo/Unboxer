#pragma once

#include <cstdint>

#include "../../utils/memory/vfunc/vfunc.h"
#include "../../../cs2/datatypes/response_rules.h"

class CUtlSymbolTable;
using SymbolStringFn = const char*(__fastcall*)(CUtlSymbolTable*, const std::uint16_t*);

namespace I
{
    inline SymbolStringFn SymbolString = nullptr;   // tier0's CUtlSymbolTable::String (interfaces.cpp)
}

class CUtlSymbolTable
{
public:
    const char* String(std::uint16_t id)
    {
        return I::SymbolString && id != 0xffff ? I::SymbolString(this, &id) : nullptr;
    }
};

class IResponseRulesCache
{
public:
    CUtlSymbolTable* Symbols()
    {
        return M::CallVFunc<CUtlSymbolTable*, 15>(this);
    }
};

class IResponseSystemManager
{
public:
    // The provider already points to the manager's secondary interface.
    ResponseRules::Root* Root()
    {
        return M::CallVFunc<ResponseRules::Root*, 2>(this);
    }
};
