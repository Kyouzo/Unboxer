#include "schema.h"

#include <string>
#include <vector>
#include "../memory/Interface/Interface.h"

namespace
{
    struct SchemaField
    {
        std::uint32_t hash = 0;
        std::uint32_t offset = 0;
    };

    std::vector<SchemaField> g_Fields;
}

bool Schema::init(const char* moduleName, int)
{
    schema_system = I::Get<ISchemaSystem>("schemasystem.dll", "SchemaSystem_00");
    if (!schema_system)
        return false;

    CSchemaSystemTypeScope* typeScope = schema_system->FindTypeScopeForModule(moduleName);
    if (!typeScope)
        return false;

    g_Fields.clear();

    const std::uint16_t classCount = typeScope->classCount;
    if (!classCount || !typeScope->classes)
        return false;

    for (std::uint16_t i = 0; i < classCount; ++i)
    {
        const SchemaClassDeclaration* declaration = typeScope->classes[i].declaration;
        if (!declaration)
            continue;

        const char* className = declaration->name;
        SchemaClassInfoData_t* classInfo = declaration->classInfo;
        if (!className || !classInfo || !classInfo->pFields)
            continue;

        for (int fieldIndex = 0; fieldIndex < classInfo->nFieldSize; ++fieldIndex)
        {
            const SchemaClassFieldData_t& field = classInfo->pFields[fieldIndex];
            if (!field.szName)
                continue;

            const std::string qualifiedName = std::string(className) + "->" + field.szName;
            g_Fields.push_back({ hash_32_fnv1a_const(qualifiedName.c_str()), field.nSingleInheritanceOffset });
        }
    }

    return !g_Fields.empty();
}

std::uint32_t SchemaFinder::Get(std::uint32_t hashedName)
{
    for (const SchemaField& field : g_Fields)
    {
        if (field.hash == hashedName)
            return field.offset;
    }

    return 0;
}
