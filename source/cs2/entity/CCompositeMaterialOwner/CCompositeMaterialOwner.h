#pragma once

#include "../../../chadware2/utils/memory/rtti/rtti.h"

class CCompositeMaterialOwner;

[[nodiscard]] inline CCompositeMaterialOwner* GetCompositeMaterialOwner(void* entity)
{
    return static_cast<CCompositeMaterialOwner*>(M::Rtti::FindNonVirtualBase(entity, ".?AVCCompositeMaterialOwner@@"));
}
