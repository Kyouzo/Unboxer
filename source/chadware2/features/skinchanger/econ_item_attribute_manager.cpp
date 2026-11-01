#include "econ_item_attribute_manager.h"
#include "sticker_placement.h"

#include <algorithm>
#include <atomic>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include "../../interfaces/interfaces.h"
#include "../../../cs2/entity/C_EconEntity/C_EconEntity.h"

namespace
{
    struct EconItemAttribute_t
    {
        char pad_0000[0x30];
        std::uint16_t defIndex;
        char pad_0032[2];
        float value;
        float initialValue;
        std::int32_t refundableCurrency;
        bool setBonus;
        char pad_0041[7];
    };

    // CUtlVector<CEconItemAttribute> layout used by CAttributeList::m_Attributes.
    struct AttributeVector_t
    {
        std::int32_t size;
        std::int32_t pad;
        std::uintptr_t data;
        std::int32_t allocationCount;
        std::int32_t growSizeAndFlags;
    };

    constexpr std::int32_t UTL_VECTOR_FIXED_FLAGS = static_cast<std::int32_t>(0x40000000u);

    constexpr std::uint16_t ATTR_PAINT = 6;
    constexpr std::uint16_t ATTR_SEED = 7;
    constexpr std::uint16_t ATTR_WEAR = 8;

    constexpr std::uint16_t ATTR_KILL_EATER = 80;
    constexpr std::uint16_t ATTR_KILL_EATER_SCORE_TYPE = 81;

    constexpr std::uint16_t ATTR_STICKER_SLOT_0_ID = 113;
    constexpr std::uint16_t ATTR_STICKER_SLOT_STRIDE = 4;

    constexpr std::uint16_t STICKER_ID_OFFSET = 0;
    constexpr std::uint16_t STICKER_WEAR_OFFSET = 1;
    constexpr std::uint16_t STICKER_SCALE_OFFSET = 2;
    constexpr std::uint16_t STICKER_ROTATION_OFFSET = 3;

    constexpr std::uint16_t ATTR_STICKER_SLOT_0_OFFSET_X = 278;
    constexpr std::uint16_t ATTR_STICKER_OFFSET_STRIDE = 2;

    constexpr std::uint16_t ATTR_STICKER_SLOT_0_SCHEMA = 290;

    // Keychain slot-0 attribute definitions from the econ schema.
    constexpr std::uint16_t ATTR_KEYCHAIN_SLOT_0_ID = 299;
    constexpr std::uint16_t ATTR_KEYCHAIN_SLOT_0_OFFSET_X = 300;
    constexpr std::uint16_t ATTR_KEYCHAIN_SLOT_0_OFFSET_Y = 301;
    constexpr std::uint16_t ATTR_KEYCHAIN_SLOT_0_OFFSET_Z = 302;
    constexpr std::uint16_t ATTR_KEYCHAIN_SLOT_0_STICKER = 321;   // a Sticker Slab's sealed sticker kit
    constexpr std::uint16_t ATTR_KEYCHAIN_SLOT_0_SEED = 306;   // items_game: 303 is "pet food expiration date"

    AttributeVector_t* GetAttributeVector(C_EconItemView* item)
    {
        if (!item)
            return nullptr;

        CAttributeList* attributeList = item->m_AttributeList();

        if (!attributeList)
            return nullptr;

        return reinterpret_cast<AttributeVector_t*>(&attributeList->m_Attributes());
    }

    EconItemAttribute_t* GetAttributes(AttributeVector_t* vector)
    {
        if (!vector || vector->size <= 0 || !vector->data)
            return nullptr;

        return reinterpret_cast<EconItemAttribute_t*>(vector->data);
    }

    void SetFloatAttribute(EconItemAttribute_t& attribute, std::uint16_t definitionIndex, float value)
    {
        attribute.defIndex = definitionIndex;
        attribute.value = value;
        attribute.initialValue = value;
    }

    void SetIntegerAttribute(EconItemAttribute_t& attribute, std::uint16_t definitionIndex, std::int32_t value)
    {
        const float rawValue = std::bit_cast<float>(value);

        attribute.defIndex = definitionIndex;
        attribute.value = rawValue;
        attribute.initialValue = rawValue;
    }

    bool IsEnabledSticker(const EconItemAttributeManager::StickerData& sticker)
    {
        return sticker.enabled && sticker.kit > 0;
    }

    bool IsEnabledKeychain(const EconItemAttributeManager::KeychainData* keychain)
    {
        return keychain && keychain->enabled && keychain->id > 0;
    }

    bool HasCustomStickerOffset(const EconItemAttributeManager::StickerData& sticker)
    {
        return sticker.offsetX != 0.0f || sticker.offsetY != 0.0f;
    }

    // "sticker slot N schema" is written for a sticker with its own position, or with offsets (relative to a position)
    bool HasStickerSchema(const EconItemAttributeManager::StickerData& sticker)
    {
        return sticker.schema >= 0 || HasCustomStickerOffset(sticker);
    }

    std::size_t GetStickerAttributeCount(const EconItemAttributeManager::StickerArray* stickers)
    {
        if (!stickers)
            return 0;

        std::size_t count = 0;

        for (const auto& sticker : *stickers)
        {
            if (!IsEnabledSticker(sticker))
                continue;

            count += 4;

            if (HasCustomStickerOffset(sticker))
                count += 2;   // offset x / y

            if (HasStickerSchema(sticker))
                count += 1;   // schema
        }

        return count;
    }

    void AddStickerBaseAttributes(EconItemAttribute_t* attributes, std::size_t& index, const EconItemAttributeManager::StickerArray& stickers)
    {
        for (std::size_t slot = 0; slot < stickers.size(); ++slot)
        {
            const auto& sticker = stickers[slot];

            if (!IsEnabledSticker(sticker))
                continue;

            const std::uint16_t baseDefinition = static_cast<std::uint16_t>(ATTR_STICKER_SLOT_0_ID + static_cast<std::uint16_t>(slot) * ATTR_STICKER_SLOT_STRIDE);

            SetIntegerAttribute(attributes[index++], baseDefinition + STICKER_ID_OFFSET, static_cast<std::int32_t>(sticker.kit));
            SetFloatAttribute(attributes[index++], baseDefinition + STICKER_WEAR_OFFSET, std::clamp(sticker.wear, 0.0f, 1.0f));
            SetFloatAttribute(attributes[index++], baseDefinition + STICKER_SCALE_OFFSET, std::clamp(sticker.scale, 0.1f, 5.0f));
            SetFloatAttribute(attributes[index++], baseDefinition + STICKER_ROTATION_OFFSET, std::clamp(sticker.rotation, -180.0f, 180.0f));
        }
    }

    void AddStickerOffsetAttributes(EconItemAttribute_t* attributes, std::size_t& index, const EconItemAttributeManager::StickerArray& stickers)
    {
        for (std::size_t slot = 0; slot < stickers.size(); ++slot)
        {
            const auto& sticker = stickers[slot];

            if (!IsEnabledSticker(sticker) || !HasCustomStickerOffset(sticker))
                continue;

            const std::uint16_t offsetXDefinition = static_cast<std::uint16_t>(ATTR_STICKER_SLOT_0_OFFSET_X + static_cast<std::uint16_t>(slot) * ATTR_STICKER_OFFSET_STRIDE);
            const StickerPlacement::Vec2 offset = StickerPlacement::ClampOffsets(sticker.offsetX, sticker.offsetY);

            SetFloatAttribute(attributes[index++], offsetXDefinition, offset.x);
            SetFloatAttribute(attributes[index++], static_cast<std::uint16_t>(offsetXDefinition + 1), offset.y);
        }
    }

    std::atomic_bool g_stickerSchemaFromSlot{ false };   // see EconItemAttributeManager::SetStickerSchemaFromSlot

    void AddStickerSchemaAttributes(EconItemAttribute_t* attributes, std::size_t& index, const EconItemAttributeManager::StickerArray& stickers)
    {
        for (std::size_t slot = 0; slot < stickers.size(); ++slot)
        {
            const auto& sticker = stickers[slot];

            if (!IsEnabledSticker(sticker) || !HasStickerSchema(sticker))
                continue;

            const std::uint16_t schemaDefinition = static_cast<std::uint16_t>(ATTR_STICKER_SLOT_0_SCHEMA + static_cast<std::uint16_t>(slot));
            const bool fromSlot = g_stickerSchemaFromSlot.load(std::memory_order_relaxed);
            const std::int32_t schema = sticker.schema >= 0 ? static_cast<std::int32_t>(sticker.schema) : fromSlot ? static_cast<std::int32_t>(slot) : 0;
            SetIntegerAttribute(attributes[index++], schemaDefinition, schema);
        }
    }

    void AddKeychainAttributes(EconItemAttribute_t* attributes, std::size_t& index, const EconItemAttributeManager::KeychainData& keychain)
    {
        SetIntegerAttribute(attributes[index++], ATTR_KEYCHAIN_SLOT_0_ID, static_cast<std::int32_t>(keychain.id));
        SetFloatAttribute(attributes[index++], ATTR_KEYCHAIN_SLOT_0_OFFSET_X, keychain.offsetX);
        SetFloatAttribute(attributes[index++], ATTR_KEYCHAIN_SLOT_0_OFFSET_Y, keychain.offsetY);
        SetFloatAttribute(attributes[index++], ATTR_KEYCHAIN_SLOT_0_OFFSET_Z, keychain.offsetZ);
        SetIntegerAttribute(attributes[index++], ATTR_KEYCHAIN_SLOT_0_SEED, static_cast<std::int32_t>(std::clamp(keychain.seed, 0, 100000)));

        if (keychain.sticker > 0)
            SetIntegerAttribute(attributes[index++], ATTR_KEYCHAIN_SLOT_0_STICKER, static_cast<std::int32_t>(keychain.sticker));
    }
}

void EconItemAttributeManager::Create(C_EconItemView* item, int paintKit, float wear, int seed, int statTrak, int statTrakScoreType, const StickerArray* stickers, const KeychainData* keychain)
{
    if (!item || !I::MemAlloc)
        return;

    AttributeVector_t* attributeVector = GetAttributeVector(item);

    if (!attributeVector || attributeVector->size != 0 || attributeVector->data != 0)
        return;

    const bool hasPaintKit = paintKit > 0;
    const bool hasStatTrak = statTrak >= 0;
    const bool hasKeychain = IsEnabledKeychain(keychain);

    const std::size_t paintAttributeCount = hasPaintKit ? 3 : 0;
    const std::size_t statTrakAttributeCount = hasStatTrak ? 2 : 0;
    const std::size_t stickerAttributeCount = GetStickerAttributeCount(stickers);
    const std::size_t keychainAttributeCount = hasKeychain ? (keychain->sticker > 0 ? 6 : 5) : 0;

    const std::size_t attributeCount = paintAttributeCount + statTrakAttributeCount + stickerAttributeCount + keychainAttributeCount;

    if (attributeCount == 0)
        return;

    // Keep a small fixed-capacity margin; this list is mutated only by this manager.
    constexpr std::size_t EXTRA_ATTRIBUTE_CAPACITY = 8;
    const std::size_t attributeCapacity = attributeCount + EXTRA_ATTRIBUTE_CAPACITY;

    auto* attributes = static_cast<EconItemAttribute_t*>(
        I::MemAlloc(attributeCapacity * sizeof(EconItemAttribute_t)));

    if (!attributes)
        return;

    std::memset(attributes, 0, attributeCapacity * sizeof(EconItemAttribute_t));

    std::size_t index = 0;

    if (hasPaintKit)
    {
        SetFloatAttribute(attributes[index++], ATTR_PAINT, static_cast<float>(paintKit));
        SetFloatAttribute(attributes[index++], ATTR_SEED, static_cast<float>(seed >= 0 ? seed : 0));
        SetFloatAttribute(attributes[index++], ATTR_WEAR, wear >= 0.0f ? wear : 0.01f);
    }

    if (hasStatTrak)
    {
        SetIntegerAttribute(attributes[index++], ATTR_KILL_EATER, static_cast<std::int32_t>(statTrak));
        SetIntegerAttribute(attributes[index++], ATTR_KILL_EATER_SCORE_TYPE, static_cast<std::int32_t>(statTrakScoreType));
    }

    if (stickers)
    {
        AddStickerBaseAttributes(attributes, index, *stickers);
        AddStickerOffsetAttributes(attributes, index, *stickers);
        AddStickerSchemaAttributes(attributes, index, *stickers);
    }

    if (hasKeychain)
        AddKeychainAttributes(attributes, index, *keychain);

    attributeVector->size = static_cast<std::int32_t>(attributeCount);
    attributeVector->pad = 0;
    attributeVector->data = reinterpret_cast<std::uintptr_t>(attributes);
    attributeVector->allocationCount = static_cast<std::int32_t>(attributeCapacity);
    attributeVector->growSizeAndFlags = UTL_VECTOR_FIXED_FLAGS;
}

bool EconItemAttributeManager::SyncStickers(C_EconItemView* item, const StickerArray& stickers)
{
    if (!item || !I::MemAlloc || !I::MemFree)
        return false;

    AttributeVector_t* attributeVector = GetAttributeVector(item);

    if (!attributeVector || attributeVector->size < 0)
        return false;

    EconItemAttribute_t* attributes = GetAttributes(attributeVector);

    if (attributeVector->size > 0 && !attributes)
        return false;

    constexpr std::size_t MAX_STICKER_ATTRIBUTES = STICKER_SLOT_COUNT * 7;
    std::array<EconItemAttribute_t, MAX_STICKER_ATTRIBUTES> desired{};
    std::size_t desiredCount = 0;

    AddStickerBaseAttributes(desired.data(), desiredCount, stickers);
    AddStickerOffsetAttributes(desired.data(), desiredCount, stickers);
    AddStickerSchemaAttributes(desired.data(), desiredCount, stickers);

    if (desiredCount == 0)
        return true;

    std::array<bool, MAX_STICKER_ATTRIBUTES> found{};
    std::size_t missingCount = 0;

    for (std::size_t wanted = 0; wanted < desiredCount; ++wanted)
    {
        for (std::int32_t existing = 0; existing < attributeVector->size; ++existing)
        {
            if (attributes[existing].defIndex != desired[wanted].defIndex)
                continue;

            attributes[existing].value = desired[wanted].value;
            attributes[existing].initialValue = desired[wanted].initialValue;
            found[wanted] = true;
            break;
        }

        if (!found[wanted])
            ++missingCount;
    }

    if (missingCount == 0)
        return true;

    const std::size_t requiredCount = static_cast<std::size_t>(attributeVector->size) + missingCount;

    if (attributes && attributeVector->allocationCount >= static_cast<std::int32_t>(requiredCount))
    {
        std::size_t index = static_cast<std::size_t>(attributeVector->size);

        for (std::size_t wanted = 0; wanted < desiredCount; ++wanted)
        {
            if (!found[wanted])
                attributes[index++] = desired[wanted];
        }

        attributeVector->size = static_cast<std::int32_t>(requiredCount);
        return true;
    }

    constexpr std::size_t EXTRA_ATTRIBUTE_CAPACITY = 8;
    const std::size_t newCapacity = requiredCount + EXTRA_ATTRIBUTE_CAPACITY;

    auto* newAttributes = static_cast<EconItemAttribute_t*>(
        I::MemAlloc(newCapacity * sizeof(EconItemAttribute_t)));

    if (!newAttributes)
        return false;

    std::memset(newAttributes, 0, newCapacity * sizeof(EconItemAttribute_t));

    if (attributes && attributeVector->size > 0)
    {
        std::memcpy(
            newAttributes,
            attributes,
            static_cast<std::size_t>(attributeVector->size) * sizeof(EconItemAttribute_t));
    }

    std::size_t index = static_cast<std::size_t>(attributeVector->size);

    for (std::size_t wanted = 0; wanted < desiredCount; ++wanted)
    {
        if (!found[wanted])
            newAttributes[index++] = desired[wanted];
    }

    void* oldData = reinterpret_cast<void*>(attributeVector->data);

    attributeVector->size = static_cast<std::int32_t>(requiredCount);
    attributeVector->pad = 0;
    attributeVector->data = reinterpret_cast<std::uintptr_t>(newAttributes);
    attributeVector->allocationCount = static_cast<std::int32_t>(newCapacity);
    attributeVector->growSizeAndFlags = UTL_VECTOR_FIXED_FLAGS;

    if (oldData)
        I::MemFree(oldData);

    return true;
}

void EconItemAttributeManager::Remove(C_EconItemView* item)
{
    if (!item)
        return;

    AttributeVector_t* attributeVector = GetAttributeVector(item);

    if (!attributeVector || (attributeVector->size == 0 && attributeVector->data == 0))
        return;

    void* data = reinterpret_cast<void*>(attributeVector->data);

    attributeVector->size = 0;
    attributeVector->pad = 0;
    attributeVector->data = 0;
    attributeVector->allocationCount = 0;
    attributeVector->growSizeAndFlags = 0;

    if (data && I::MemFree)
        I::MemFree(data);
}

bool EconItemAttributeManager::SetKeychainId(C_EconItemView* item, int id)
{
    AttributeVector_t* attributeVector = GetAttributeVector(item);
    EconItemAttribute_t* attributes = GetAttributes(attributeVector);

    if (!attributeVector || !attributes)
        return false;

    for (std::int32_t i = 0; i < attributeVector->size; ++i)
    {
        EconItemAttribute_t& attribute = attributes[i];

        if (attribute.defIndex != ATTR_KEYCHAIN_SLOT_0_ID)
            continue;

        SetIntegerAttribute(attribute, ATTR_KEYCHAIN_SLOT_0_ID, static_cast<std::int32_t>(id));
        return true;
    }

    return false;
}

namespace
{
    int FindKeychainId(CAttributeList* list)
    {
        if (!list)
            return 0;

        const AttributeVector_t* vector = reinterpret_cast<const AttributeVector_t*>(&list->m_Attributes());

        if (!vector || vector->size <= 0 || vector->size > 256 || !vector->data)
            return 0;

        const EconItemAttribute_t* attributes = reinterpret_cast<const EconItemAttribute_t*>(vector->data);

        for (std::int32_t i = 0; i < vector->size; ++i)
        {
            if (attributes[i].defIndex != ATTR_KEYCHAIN_SLOT_0_ID)
                continue;

            const std::int32_t id = std::bit_cast<std::int32_t>(attributes[i].value);

            if (id > 0)
                return id;
        }

        return 0;
    }
}

void EconItemAttributeManager::SetStickerSchemaFromSlot(bool fromSlot)
{
    g_stickerSchemaFromSlot.store(fromSlot, std::memory_order_relaxed);
}

namespace
{
    std::atomic_bool g_replaceRealItem{ false };   // see EconItemAttributeManager::SetReplaceRealItem

    AttributeVector_t* NetworkedVector(C_EconItemView* item)
    {
        CAttributeList* list = item ? item->m_NetworkedDynamicAttributes() : nullptr;
        return list ? reinterpret_cast<AttributeVector_t*>(&list->m_Attributes()) : nullptr;
    }
}

void EconItemAttributeManager::SetReplaceRealItem(bool replace)
{
    g_replaceRealItem.store(replace, std::memory_order_relaxed);
}

bool EconItemAttributeManager::ReplaceRealItem()
{
    return g_replaceRealItem.load(std::memory_order_relaxed);
}

bool EconItemAttributeManager::HasNetworkedAttributes(C_EconItemView* item)
{
    const AttributeVector_t* vector = NetworkedVector(item);
    return vector && vector->size > 0 && vector->size <= 256 && vector->data;
}

bool EconItemAttributeManager::ClearNetworkedAttributes(C_EconItemView* item)
{
    if (!HasNetworkedAttributes(item))
        return false;

    const bool hadCharm = FindKeychainId(item->m_NetworkedDynamicAttributes()) > 0;
    NetworkedVector(item)->size = 0;
    return hadCharm;
}

int EconItemAttributeManager::GetKeychainId(C_EconItemView* item, bool includeMainList)
{
    if (!item)
        return 0;

    if (const int id = FindKeychainId(item->m_NetworkedDynamicAttributes()); id > 0)
        return id;

    return includeMainList ? FindKeychainId(item->m_AttributeList()) : 0;
}
