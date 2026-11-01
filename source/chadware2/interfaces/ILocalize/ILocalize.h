#pragma once

#include "../../utils/memory/vfunc/vfunc.h"

class ILocalize
{
public:
    [[nodiscard]] const char* FindSafe(const char* tokenName)
    {
        if (!tokenName || !*tokenName)
            return "";

        return M::CallVFunc<const char*, 17U>(this, tokenName);
    }
};