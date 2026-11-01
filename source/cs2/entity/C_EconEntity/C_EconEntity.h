#pragma once

#include <cstddef>
#include <cstdint>
#include "../C_BaseEntity/C_BaseEntity.h"
#include "../../../chadware2/utils/schema/schema.h"
#include "../../../chadware2/utils/memory/vfunc/vfunc.h"

class CAttributeList
{
public:
    schema(std::uintptr_t, m_Attributes, "CAttributeList->m_Attributes");
};

class C_EconItemView
{
public:
    schema(std::uint16_t, m_iItemDefinitionIndex, "C_EconItemView->m_iItemDefinitionIndex");
    schema(std::int32_t, m_iEntityQuality, "C_EconItemView->m_iEntityQuality");
    schema(std::uint32_t, m_iItemIDHigh, "C_EconItemView->m_iItemIDHigh");
    schema(bool, m_bInitialized, "C_EconItemView->m_bInitialized");
    schema(bool, m_bDisallowSOC, "C_EconItemView->m_bDisallowSOC");
    schema(bool, m_bRestoreCustomMaterialAfterPrecache, "C_EconItemView->m_bRestoreCustomMaterialAfterPrecache");
    SCHEMA_ARRAY(CAttributeList, m_AttributeList, "C_EconItemView->m_AttributeList");
    SCHEMA_ARRAY(CAttributeList, m_NetworkedDynamicAttributes, "C_EconItemView->m_NetworkedDynamicAttributes");
    SCHEMA_ARRAY(char, m_szCustomName, "C_EconItemView->m_szCustomName");
    [[nodiscard]] std::uintptr_t& m_name_description_ptr() { return m_NameDescription; }

    // This cached description pointer is not reflected in the entity schema.
    std::byte m_Unknown000[0x200];
    std::uintptr_t m_NameDescription;
};

class C_AttributeContainer
{
public:
    SCHEMA_ARRAY(C_EconItemView, m_Item, "C_AttributeContainer->m_Item");
};

class C_EconEntity : public C_BaseEntity
{
public:
    SCHEMA_ARRAY(C_AttributeContainer, m_AttributeManager, "C_EconEntity->m_AttributeManager");
    schema(int, m_nFallbackPaintKit, "C_EconEntity->m_nFallbackPaintKit");
    schema(int, m_nFallbackSeed, "C_EconEntity->m_nFallbackSeed");
    schema(float, m_flFallbackWear, "C_EconEntity->m_flFallbackWear");
    schema(int, m_nFallbackStatTrak, "C_EconEntity->m_nFallbackStatTrak");

    // Slot 10 is PostDataUpdate; the native wrapper forwards a 32-bit EDX value.
    void PostDataUpdate(int updateType) { M::CallVFunc<void, 10U>(this, updateType); }
};
