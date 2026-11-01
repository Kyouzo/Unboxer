#include "ISchemaClass.h"
#include "../../../../chadware2/utils/memory/vfunc/vfunc.h"

CSchemaSystemTypeScope* ISchemaSystem::FindTypeScopeForModule(const char* moduleName)
{
    return M::CallVFunc<CSchemaSystemTypeScope*, 13U>(this, moduleName, nullptr);
}
