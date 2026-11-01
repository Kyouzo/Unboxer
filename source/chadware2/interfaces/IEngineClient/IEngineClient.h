#pragma once

#include "../../utils/memory/vfunc/vfunc.h"

class IEngineClient
{
public:
    bool in_game()
    {
        return M::CallVFunc<bool, 39U>(this);
    }

    bool connected()
    {
        return M::CallVFunc<bool, 40U>(this);
    }
};
