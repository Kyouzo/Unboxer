#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

#include "../../utils/memory/vfunc/vfunc.h"

template<typename K, typename V>
class C_EconUtlMap
{
public:
    struct Node_t
    {
        int m_Left;
        int m_Right;
        V m_Value;
        int m_Flag;
        K m_Key;
    };

    [[nodiscard]] int Count() const
    {
        return m_Count;
    }

    [[nodiscard]] Node_t& Element(int index)
    {
        return m_Elements[index];
    }

    int m_Count;
    int m_CapacityFlags;
    Node_t* m_Elements;
};

class CPaintKit
{
public:
    [[nodiscard]] bool UsesOldModel() const
    {
        return m_UsesOldModel;
    }

    int m_ID;                         // 0x0000
    std::uint32_t m_Pad04;            // 0x0004
    const char* m_Name;               // 0x0008
    const char* m_DescriptionString;  // 0x0010
    const char* m_DescriptionTag;     // 0x0018
    std::byte m_Pad20[0x24];          // 0x0020
    int m_Rarity;                     // 0x0044
    std::byte m_Pad48[0x66];
    bool m_UsesOldModel;
};

template<>
class C_EconUtlMap<int, CPaintKit*>
{
public:
    struct Node_t
    {
        int m_Left;
        int m_Right;
        int m_Pad08;
        int m_Pad0C;
        int m_Key;
        int m_Pad14;
        CPaintKit* m_Value;
    };

    [[nodiscard]] int Count() const
    {
        return m_Count;
    }

    [[nodiscard]] Node_t& Element(int index)
    {
        return m_Elements[index];
    }

    int m_Count;
    int m_CapacityFlags;
    Node_t* m_Elements;
};

class CEconItemDefinition
{
public:
    // its items_game KeyValues (the engine owns it) - the agent voice reads "vo_prefix" from it
    [[nodiscard]] const void* GetKeyValues() const
    {
        return M::CallVFunc<const void*, 9U>(this);
    }

    [[nodiscard]] bool IsKnife(bool excludeDefault = false) const
    {
        if (!m_ItemTypeName || std::strcmp(m_ItemTypeName, "#CSGO_Type_Knife") != 0)
            return false;

        return !excludeDefault || m_DefinitionIndex >= 500;
    }

    [[nodiscard]] bool IsGlove(bool excludeDefault = false) const
    {
        if (!m_ItemTypeName || std::strcmp(m_ItemTypeName, "#Type_Hands") != 0)
            return false;

        return !excludeDefault || (m_DefinitionIndex != 5028 && m_DefinitionIndex != 5029);
    }

    [[nodiscard]] bool IsAgent() const
    {
        return m_ItemTypeName && std::strcmp(m_ItemTypeName, "#Type_CustomPlayer") == 0;
    }

    [[nodiscard]] const char* GetModelName() const
    {
        return m_ModelName;
    }

    [[nodiscard]] const char* GetSimpleWeaponName() const
    {
        return m_SimpleWeaponName;
    }

    [[nodiscard]] const char* GetWeaponName() const
    {
        return m_WeaponName;
    }

    [[nodiscard]] const char* GetItemName() const
    {
        return m_ItemName;
    }

    std::byte m_Pad00[0x10];          // 0x0000
    std::uint16_t m_DefinitionIndex;  // 0x0010
    std::byte m_Pad12[0x5E];          // 0x0012
    char* m_ItemBaseName;             // 0x0070
    std::byte m_Pad78[0x8];           // 0x0078
    char* m_ItemTypeName;             // 0x0080
    std::byte m_Pad88[0xC0];
    const char* m_ModelName;
    std::byte m_Pad150[0xE0];
    const char* m_SimpleWeaponName;
    std::byte m_Pad238[0x10];
    const char* m_WeaponName;
    std::byte m_Pad250[0x10];
    const char* m_ItemName;
};

class CEconItemSchema
{
public:
    [[nodiscard]] C_EconUtlMap<int, CEconItemDefinition*>& GetSortedItemDefinitionMap()
    {
        return m_SortedItemDefinitions;
    }

    [[nodiscard]] C_EconUtlMap<int, CPaintKit*>& GetPaintKits()
    {
        return m_PaintKits;
    }

    std::byte m_Pad000[0xF8];
    C_EconUtlMap<int, CEconItemDefinition*> m_SortedItemDefinitions;
    std::byte m_Pad108[0x198];
    // +0x2A0 on build 14190 (+0x2F0 before): ItemSchema reads it at the offset the game's own lookup uses
    // (item_schema.cpp, PaintKitMapOffset) - this is only the fallback.
    C_EconUtlMap<int, CPaintKit*> m_PaintKits;
};

class CEconItemSystem
{
public:
    [[nodiscard]] CEconItemSchema* GetEconItemSchema()
    {
        return m_Schema;
    }

    void* m_Vtable;
    CEconItemSchema* m_Schema;
};

class CSource2Client
{
public:
    [[nodiscard]] CEconItemSystem* GetEconItemSystem()
    {
        return M::CallVFunc<CEconItemSystem*, 129U>(this);
    }
};
