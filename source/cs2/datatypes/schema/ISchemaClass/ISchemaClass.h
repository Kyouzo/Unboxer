#pragma once

#include <cstddef>
#include <cstdint>

struct SchemaClassFieldData_t
{
    const char* szName = nullptr;
    void* pSchemaType = nullptr;
    std::uint32_t nSingleInheritanceOffset = 0;
    std::int32_t nMetadataSize = 0;
    void* pMetaData = nullptr;
};

struct SchemaClassInfoData_t
{
    void* pVtable = nullptr;
    const char* szName = nullptr;
    const char* szModuleName = nullptr;
    char pad_0x18[0x8]{};
    int m_nSize = 0;
    std::int16_t nFieldSize = 0;
    std::int16_t pad_0x26 = 0;
    std::int16_t nStaticSize = 0;
    std::int16_t nMetadataSize = 0;
    std::uint8_t nAlignOf = 0;
    std::uint8_t nBaseClassesCount = 0;
    std::int16_t pad_0x2E = 0;
    SchemaClassFieldData_t* pFields = nullptr;
};

struct SchemaClassDeclaration
{
    void* vtable;
    const char* name;
    std::byte unknown10[0x10];
    SchemaClassInfoData_t* classInfo;
};

struct SchemaClassEntry
{
    std::byte unknown00[0x10];
    SchemaClassDeclaration* declaration;
};

class CSchemaSystemTypeScope
{
public:
    std::byte unknown000[0x470];
    std::uint16_t classCount;
    std::byte unknown472[0x06];
    SchemaClassEntry* classes;
};

class ISchemaSystem
{
public:
    CSchemaSystemTypeScope* FindTypeScopeForModule(const char* moduleName);
};
