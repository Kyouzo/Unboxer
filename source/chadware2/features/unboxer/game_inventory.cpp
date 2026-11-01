#include "game_inventory.h"

#include <Windows.h>

#include <algorithm>
#include <cmath>
#include <ctime>
#include <cstddef>
#include <cstring>
#include <mutex>
#include <string>

#include "../../../patterns.h"
#include "../../interfaces/interfaces.h"
#include "../../utils/memory/patternscan/patternscan.h"
#include "../../utils/offsets/offsets.h"
#include "../../utils/schema/schema.h"
#include "../../../../external/kiero/minhook/src/hde/hde64.h"

// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
// Everything below was read out of the 2026-09-29 client.dll (offline, disassembled + RTTI):
//   - CEconItem factory @+0x10D5E40: allocates 0x48, vtables at +0 / +8. vtable[0] = destructor, vtable[1] =
//     GetTypeID (returns 1). (Older changers "destroyed" items with vfunc 1 — that only ever called GetTypeID.)
//   - CCSPlayerInventory: owner SOID at +0x10 (id) / +0x18 (type) — its SOCacheSubscribed compares exactly those.
//     vtable [0] SOCreated / [1] SOUpdated / [2] SODestroyed (const SOID&, CSharedObject*, event), each checks
//     GetTypeID() == 1. The local inventory is read the way UseTool reads it: [InventoryManager + disp].
//   - GCSDK::CGCClientSharedObjectCache: its own AddObject = CreateBaseTypeCache(typeId) + typeCache vfunc 1,
//     RemoveObject = find type cache + typeCache vfunc 3. We call the same two slots.
//   - SetDynamicAttributeValue<T>: one per attribute type; CSchemaAttributeType_Uint32 / _Float vtable[6] calls it.
//     Resolved through RTTI below so the float and the uint32 one can't be mixed up (they have identical code).
//   - Attribute definition by index: CCStrike15ItemSchema vfunc 27 → attributes[index] (bounds-checked).
// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
namespace
{
    struct EconItem
    {
        void* vtable;                    // 0x00
        void* vtable2;                   // 0x08
        std::uint64_t id;                // 0x10
        std::uint64_t originalId;        // 0x18
        void* customData;                // 0x20
        std::uint32_t accountId;         // 0x28
        std::uint32_t inventory;         // 0x2C  position (bit 30 = not acknowledged, "NEW")
        std::uint16_t definition;        // 0x30
        std::uint16_t origin : 5;        // 0x32
        std::uint16_t quality : 4;
        std::uint16_t level : 2;
        std::uint16_t rarity : 4;
        std::uint16_t dirty : 1;
    };
    static_assert(offsetof(EconItem, id) == 0x10 && offsetof(EconItem, accountId) == 0x28 && offsetof(EconItem, definition) == 0x30);

    struct SOID
    {
        std::uint64_t id;
        std::uint32_t type;
        std::uint32_t padding;
    };

    using CreateEconItemFn = EconItem * (__fastcall*)();
    using GetManagerFn = void* (__fastcall*)();
    using CreateTypeCacheFn = void* (__fastcall*)(void* soCache, int classId);
    using SetAttributeFn = void(__fastcall*)(EconItem* item, void* attributeDefinition, const void* value);
    using GetAttributeDefinitionFn = void* (__fastcall*)(void* schema, int index);
    using ListenerFn = void(__fastcall*)(void* inventory, const SOID* owner, void* object, int event);
    using AddObjectFn = bool(__fastcall*)(void* typeCache, void* object);
    using RemoveObjectFn = void* (__fastcall*)(void* typeCache, void* object);

    constexpr int SO_EVENT_INCREMENTAL = 4;
    constexpr std::uint32_t NEW_ITEM_FLAG = 0x40000000u;   // an item's position: not acknowledged yet ("NEW")
    constexpr int CLASS_ECON_ITEM = 1;
    constexpr int VFUNC_SO_CREATED = 0;
    constexpr int VFUNC_SO_UPDATED = 1;
    constexpr int VFUNC_SO_DESTROYED = 2;
    constexpr int VFUNC_ADD_OBJECT = 1;
    constexpr int VFUNC_REMOVE_OBJECT = 3;
    constexpr int VFUNC_ATTRIBUTE_DEFINITION = 27;

    constexpr int ATTR_PAINT_KIT = 6;       // "set item texture prefab"  float
    constexpr int ATTR_PAINT_SEED = 7;      // "set item texture seed"    float
    constexpr int ATTR_PAINT_WEAR = 8;      // "set item texture wear"    float
    constexpr int ATTR_KILL_EATER = 80;     // "kill eater"               uint32
    constexpr int ATTR_KILL_EATER_TYPE = 81;// "kill eater score type"    uint32
    constexpr int ATTR_STICKER_0_ID = 113;  // "sticker slot 0 id"        integer (stickers, patches, graffiti)
    constexpr int ATTR_MUSIC_ID = 166;      // "music id"                 integer
    // a gun's stickers, per slot: id (integer), wear / scale / rotation (floats) at 113 + 4 * slot; offset x / y
    // (floats) at 278 + 2 * slot; "sticker slot N schema" (integer) at 290 + slot
    constexpr int ATTR_STICKER_STRIDE = 4;
    constexpr int ATTR_CUSTOM_NAME = 111;   // "custom name attr"        string (name tag)
    constexpr int ATTR_CUSTOM_NAME_2 = 328; // "custom name attr 2"      a pet's name as a pullet
    constexpr int ATTR_CUSTOM_NAME_3 = 329; // "custom name attr 3"      ... and as a hen
    constexpr int ATTR_UPGRADE_LEVEL = 268; // "upgrade level"           integer (pets: 0 egg ... 3 hen)
    constexpr int ATTR_PET_ID = 296;        // "pet id"                  integer (pet_definitions)
    constexpr int ATTR_KEYCHAIN_ID = 299;   // "keychain slot 0 id"      integer; offset x / y / z floats at 300-302
    constexpr int ATTR_PET_FOOD = 303;      // "pet food expiration date" unix time
    constexpr int ATTR_PET_UPGRADE = 304;   // "pet next upgrade date"    unix time
    constexpr int ATTR_KEYCHAIN_SEED = 306; // "keychain slot 0 seed"    integer
    constexpr int ATTR_KEYCHAIN_STICKER = 321; // "keychain slot 0 sticker": a Sticker Slab's sealed sticker, integer
    constexpr int ATTR_KEYCHAIN_HIGHLIGHT = 314; // "keychain slot 0 highlight": a Major highlight clip (highlight_reels number)
    constexpr int ATTR_SPRAY_TINT = 233;    // "spray tint id": a graffiti's colour (graffiti_tints 1-19), integer
    constexpr int ATTR_SPRAYS_REMAINING = 232;   // "sprays remaining": an unsealed graffiti's charges, integer
    constexpr int ATTR_PET_SEED = 313;      // "pet seed"                integer
    constexpr int ATTR_DEPLOYMENT_DATE = 180; // "deployment date"       unix time (a pet: when it hatched)
    constexpr int ATTR_STICKER_0_OFFSET_X = 278;
    constexpr int ATTR_STICKER_0_SCHEMA = 290;
    // an unsealed Uplink Terminal and its offers (all "stored_as_integer": raw 32 bits)
    constexpr int ATTR_QUEST_POINTS_REMAINING = 169;   // the terminal's offer number (the final one negative)
    constexpr int ATTR_EXPIRATION_DATE = 183;          // the terminal's end (unix time)
    constexpr int ATTR_CASKET_ID_LOW = 272;            // the container an item is inside: low / high 32 bits of its id
    constexpr int ATTR_CASKET_ID_HIGH = 273;
    constexpr int ATTR_ITEMS_COUNT = 270;              // a storage unit: how many items are in it
    constexpr int ATTR_MODIFICATION_DATE = 271;        // a storage unit: named / last changed (unix time) - CS2 treats one
                                                       // without it as new (asks for a name first)
    constexpr int ATTR_PURCHASE_PRICE = 316;           // an offer's price, cents

    // C_EconItemView vfunc 7: GetStickerAttributeBySlotIndexInt(slot, attribute (0 = id), default) — the call
    // InventoryAPI.WearItemSticker reads a sticker's kit with (client.dll+0xFFCD45, 2026-10-01).
    constexpr int VFUNC_ITEM_STICKER_INT = 7;
    using ItemViewByIdFn = void* (__fastcall*)(std::uint64_t id);
    using StickerIntFn = int(__fastcall*)(void* itemView, int slot, int attribute, int fallback);

    // where the sticker popup's preview keeps the placement (see Patterns::StickerPlacement)
    const std::int32_t* g_stickerSlot = nullptr;
    const float* g_stickerOffsetX = nullptr;
    const float* g_stickerOffsetY = nullptr;
    const float* g_stickerRotation = nullptr;
    const float* g_stickerWear = nullptr;
    ItemViewByIdFn g_itemViewById = nullptr;

    // where the charm popup's preview keeps the charm (Patterns::KeychainPlacement), the patch popup's slot (PatchSlot)
    const float* g_keychainOffsetX = nullptr;
    const float* g_keychainOffsetY = nullptr;
    const float* g_keychainOffsetZ = nullptr;
    const std::int32_t* g_patchSlot = nullptr;

    constexpr const char* RTTI_INVENTORY = ".?AVCCSPlayerInventory@@";
    constexpr const char* RTTI_SO_CACHE = ".?AVCGCClientSharedObjectCache@GCSDK@@";

    CreateEconItemFn g_createItem = nullptr;
    GetManagerFn g_getManager = nullptr;
    CreateTypeCacheFn g_createTypeCache = nullptr;
    SetAttributeFn g_setUint32 = nullptr;
    SetAttributeFn g_setFloat = nullptr;
    void* g_useTool = nullptr;
    std::uint32_t g_inventoryOffset = 0;    // [manager + this] = local CCSPlayerInventory*
    std::uint32_t g_soCacheOffset = 0;      // [inventory + this] = its CGCClientSharedObjectCache*
    std::string g_error;

    // ── module / RTTI helpers ──

    struct Section
    {
        std::uint8_t* start = nullptr;
        std::size_t size = 0;
    };

    bool GetSection(HMODULE module, const char* name, Section& out)
    {
        if (!module)
            return false;

        auto* base = reinterpret_cast<std::uint8_t*>(module);
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
        const IMAGE_SECTION_HEADER* section = IMAGE_FIRST_SECTION(nt);

        for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++section)
        {
            if (std::strncmp(reinterpret_cast<const char*>(section->Name), name, IMAGE_SIZEOF_SHORT_NAME) == 0)
            {
                out.start = base + section->VirtualAddress;
                out.size = section->Misc.VirtualSize;
                return true;
            }
        }

        return false;
    }

    bool InSection(const Section& section, const void* address)
    {
        const auto* p = static_cast<const std::uint8_t*>(address);
        return section.start && p >= section.start && p < section.start + section.size;
    }

    // True if the object's most-derived class is `name` (MSVC RTTI, ".?AV...@@").
    bool RttiIs(const void* object, const char* name)
    {
        if (!object || !name)
            return false;

        __try
        {
            const auto* vtable = *reinterpret_cast<const std::uintptr_t* const*>(object);

            if (!vtable)
                return false;

            const auto* locator = reinterpret_cast<const std::uint32_t*>(vtable[-1]);

            if (!locator || locator[0] != 1)
                return false;

            const std::uintptr_t imageBase = reinterpret_cast<std::uintptr_t>(locator) - locator[5];
            const char* typeName = reinterpret_cast<const char*>(imageBase + locator[3] + 0x10);
            return std::strcmp(typeName, name) == 0;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    // vtable of the class whose RTTI name is `name` (complete object, offset 0) in the module, or null.
    void** FindVTable(HMODULE module, const char* name)
    {
        Section data{}, rdata{};

        if (!GetSection(module, ".data", data) || !GetSection(module, ".rdata", rdata))
            return nullptr;

        const std::size_t nameLength = std::strlen(name);
        const auto moduleBase = reinterpret_cast<std::uintptr_t>(module);

        // 1) the TypeDescriptor: { vtable, spare, "name" } — in .data
        std::uint8_t* typeDescriptor = nullptr;

        for (std::size_t i = 16; i + nameLength + 1 < data.size; ++i)
        {
            if (data.start[i] == '.' && std::memcmp(data.start + i, name, nameLength + 1) == 0)
            {
                typeDescriptor = data.start + i - 16;
                break;
            }
        }

        if (!typeDescriptor)
            return nullptr;

        const auto typeDescriptorRva = static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(typeDescriptor) - moduleBase);

        // 2) its CompleteObjectLocator (signature 1, offset 0, self RVA matches) — in .rdata
        std::uint8_t* locator = nullptr;

        for (std::size_t i = 0; i + 24 <= rdata.size; i += 4)
        {
            const auto* candidate = reinterpret_cast<const std::uint32_t*>(rdata.start + i);

            if (candidate[3] != typeDescriptorRva || candidate[0] != 1 || candidate[1] != 0)
                continue;

            if (candidate[5] != static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(candidate) - moduleBase))
                continue;

            locator = rdata.start + i;
            break;
        }

        if (!locator)
            return nullptr;

        // 3) the vtable: the slot right after a pointer to that locator
        const auto locatorAddress = reinterpret_cast<std::uintptr_t>(locator);

        for (std::size_t i = 0; i + 16 <= rdata.size; i += 8)
        {
            if (*reinterpret_cast<const std::uintptr_t*>(rdata.start + i) == locatorAddress)
                return reinterpret_cast<void**>(rdata.start + i + 8);
        }

        return nullptr;
    }

    // CSchemaAttributeType_<T>::vtable[6] loads the value and calls SetDynamicAttributeValue<T> — the first
    // `call rel32` in it. Its head is checked so a changed layout fails loudly instead of calling something else.
    SetAttributeFn SetterFromAttributeType(HMODULE module, const char* typeName)
    {
        Section text{};

        if (!GetSection(module, ".text", text))
            return nullptr;

        void** vtable = FindVTable(module, typeName);

        if (!vtable)
            return nullptr;

        const auto* dispatcher = static_cast<const std::uint8_t*>(vtable[6]);

        if (!InSection(text, dispatcher))
            return nullptr;

        // mov [rsp+?],rbx / push rdi / sub rsp,? / mov rax,[rcx] / mov rbx,r8 / mov rdi,rdx
        static constexpr std::uint8_t head[] = { 0x48, 0x89, 0x5C, 0x24 };
        if (std::memcmp(dispatcher, head, sizeof(head)) != 0 || dispatcher[5] != 0x57)
            return nullptr;

        for (int i = 6; i < 0x60; ++i)
        {
            if (dispatcher[i] != 0xE8)
                continue;

            const std::int32_t relative = *reinterpret_cast<const std::int32_t*>(dispatcher + i + 1);
            const std::uint8_t* target = dispatcher + i + 5 + relative;

            // mov [rsp+?],rbp — the head every SetDynamicAttributeValue<T> shares
            if (InSection(text, target) && target[0] == 0x48 && target[1] == 0x89 && target[2] == 0x6C && target[3] == 0x24)
                return reinterpret_cast<SetAttributeFn>(const_cast<std::uint8_t*>(target));

            return nullptr;
        }

        return nullptr;
    }

    // `mov r14, [rax + disp32]` (4C 8B B0 disp32) right after UseTool calls CCSInventoryManager::GetInstance.
    std::uint32_t InventoryOffsetFromUseTool(const std::uint8_t* useTool)
    {
        if (!useTool)
            return 0;

        for (int i = 0; i < 0x90; ++i)
        {
            if (useTool[i] == 0xE8 && useTool[i + 5] == 0x4C && useTool[i + 6] == 0x8B && useTool[i + 7] == 0xB0)
            {
                const std::uint32_t offset = *reinterpret_cast<const std::uint32_t*>(useTool + i + 8);
                return offset > 0 && offset < 0x200000 ? offset : 0;
            }
        }

        return 0;
    }

    // ── guarded game calls (no C++ objects in these: __try) ──

    void* CallGetManager()
    {
        __try
        {
            return g_getManager ? g_getManager() : nullptr;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return nullptr;
        }
    }

    void* ReadPointer(const void* base, std::uint32_t offset)
    {
        if (!base)
            return nullptr;

        __try
        {
            return *reinterpret_cast<void* const*>(static_cast<const std::uint8_t*>(base) + offset);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return nullptr;
        }
    }

    bool ReadOwner(const void* inventory, SOID& owner)
    {
        __try
        {
            owner = *reinterpret_cast<const SOID*>(static_cast<const std::uint8_t*>(inventory) + 0x10);
            return owner.id != 0;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    void* CallCreateTypeCache(void* soCache)
    {
        __try
        {
            return g_createTypeCache(soCache, CLASS_ECON_ITEM);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return nullptr;
        }
    }

    EconItem* CallCreateItem()
    {
        __try
        {
            return g_createItem();
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return nullptr;
        }
    }

    void* CallAttributeDefinition(void* schema, int index)
    {
        __try
        {
            const auto fn = reinterpret_cast<GetAttributeDefinitionFn>((*reinterpret_cast<void***>(schema))[VFUNC_ATTRIBUTE_DEFINITION]);
            return fn(schema, index);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return nullptr;
        }
    }

    bool CallSetAttribute(SetAttributeFn setter, EconItem* item, void* definition, const void* value)
    {
        __try
        {
            setter(item, definition, value);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    bool CallAddObject(void* typeCache, void* object)
    {
        __try
        {
            const auto fn = reinterpret_cast<AddObjectFn>((*reinterpret_cast<void***>(typeCache))[VFUNC_ADD_OBJECT]);
            return fn(typeCache, object);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    bool CallRemoveObject(void* typeCache, void* object)
    {
        __try
        {
            const auto fn = reinterpret_cast<RemoveObjectFn>((*reinterpret_cast<void***>(typeCache))[VFUNC_REMOVE_OBJECT]);
            return fn(typeCache, object) != nullptr;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    bool CallListener(void* inventory, int index, const SOID* owner, void* object)
    {
        __try
        {
            const auto fn = reinterpret_cast<ListenerFn>((*reinterpret_cast<void***>(inventory))[index]);
            fn(inventory, owner, object, SO_EVENT_INCREMENTAL);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    // ── the live objects ──

    void* LocalInventory()
    {
        void* manager = CallGetManager();

        if (!manager || !g_inventoryOffset)
            return nullptr;

        void* inventory = ReadPointer(manager, g_inventoryOffset);
        return RttiIs(inventory, RTTI_INVENTORY) ? inventory : nullptr;
    }

    void* SharedObjectCache(void* inventory)
    {
        if (!inventory)
            return nullptr;

        if (g_soCacheOffset)
        {
            void* cache = ReadPointer(inventory, g_soCacheOffset);

            if (RttiIs(cache, RTTI_SO_CACHE))
                return cache;
        }

        // +0x68 on the builds Berserk / the June invchanger ran on; otherwise look for it among the members
        for (std::uint32_t offset = 0x68; ; offset = offset == 0x68 ? 0x20 : offset + 8)
        {
            if (offset >= 0x200)
                break;

            void* cache = ReadPointer(inventory, offset);

            if (RttiIs(cache, RTTI_SO_CACHE))
            {
                g_soCacheOffset = offset;
                return cache;
            }
        }

        return nullptr;
    }

    void* ItemTypeCache(void* inventory)
    {
        void* cache = SharedObjectCache(inventory);
        return cache && g_createTypeCache ? CallCreateTypeCache(cache) : nullptr;
    }

    // The item vector of the type cache: count at +0x8, elements at +0x10 (typeCache vfunc 1 walks exactly these).
    template <typename Visitor>
    void ForEachItem(void* typeCache, Visitor&& visit)
    {
        if (!typeCache)
            return;

        int count = 0;
        EconItem** elements = nullptr;

        __try
        {
            count = *reinterpret_cast<const int*>(static_cast<const std::uint8_t*>(typeCache) + 0x8);
            elements = *reinterpret_cast<EconItem** const*>(static_cast<const std::uint8_t*>(typeCache) + 0x10);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return;
        }

        if (count <= 0 || count > 200000 || !elements)
            return;

        for (int i = 0; i < count; ++i)
        {
            EconItem* item = nullptr;
            std::uint64_t id = 0;
            std::uint32_t position = 0;

            __try
            {
                item = elements[i];
                if (item)
                {
                    id = item->id;
                    position = item->inventory;
                }
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                return;
            }

            if (item && !visit(item, id, position))
                return;
        }
    }

    // RTTI name of the attribute's type object (definition +0x18), e.g. ".?AVCSchemaAttributeType_Uint32@@".
    bool AttributeTypeName(void* definition, char* out, std::size_t size)
    {
        out[0] = '\0';
        void* type = ReadPointer(definition, 0x18);

        if (!type)
            return false;

        __try
        {
            const auto* vtable = *reinterpret_cast<const std::uintptr_t* const*>(type);
            const auto* locator = reinterpret_cast<const std::uint32_t*>(vtable[-1]);

            if (!locator || locator[0] != 1)
                return false;

            const std::uintptr_t imageBase = reinterpret_cast<std::uintptr_t>(locator) - locator[5];
            const char* name = reinterpret_cast<const char*>(imageBase + locator[3] + 0x10);
            std::size_t i = 0;

            for (; i + 1 < size && name[i]; ++i)
                out[i] = name[i];

            out[i] = '\0';
            return i > 0;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            out[0] = '\0';
            return false;
        }
    }

    void* AttributeDefinition(int index)
    {
        if (!I::Source2Client)
            return nullptr;

        CEconItemSystem* system = I::Source2Client->GetEconItemSystem();
        void* schema = system ? system->GetEconItemSchema() : nullptr;
        return schema ? CallAttributeDefinition(schema, index) : nullptr;
    }

    // SetDynamicAttributeValue<T> dynamic_casts the attribute's type to ISchemaAttributeTypeBase<T> and silently does
    // nothing when it doesn't match — so the setter has to fit the attribute. In the 2026-09-29 game all four are
    // CSchemaAttributeType_Default (a 32-bit value; the uint32 setter fits it).
    //   _Float            -> float setter, the value as a float
    //   _Uint32 / _Default -> uint32 setter: the 32 bits as the game stores them — `floatValue` = the bits of a float
    // What the game stores (its own item-preview code, client.dll+0x113AF5D): paint kit and seed go through cvtdq2ps
    // (int -> float) before they're set, wear is a float, kill eater is the raw integer. items_game calls paint kit /
    // seed "float_floor_to_integer": a float that's read back as an integer. Writing 1187 as an integer reads back as
    // a denormal float = paint kit 0 = no skin (test 2).
    bool SetAttribute(EconItem* item, int index, double value, bool floatValue)
    {
        void* definition = AttributeDefinition(index);
        char typeName[96];

        if (!definition || !AttributeTypeName(definition, typeName, sizeof(typeName)))
            return false;

        if (std::strcmp(typeName, ".?AVCSchemaAttributeType_Float@@") == 0)
        {
            if (!g_setFloat)
                return false;

            const float asFloat = static_cast<float>(value);
            return CallSetAttribute(g_setFloat, item, definition, &asFloat);
        }

        if (std::strcmp(typeName, ".?AVCSchemaAttributeType_Uint32@@") != 0 && std::strcmp(typeName, ".?AVCSchemaAttributeType_Default@@") != 0)
            return false;   // some other type (string, vector...): not one of ours

        if (!g_setUint32)
            return false;

        std::uint32_t raw = 0;

        if (floatValue)
        {
            const float asFloat = static_cast<float>(value);
            std::memcpy(&raw, &asFloat, sizeof(raw));
        }
        else
        {
            raw = static_cast<std::uint32_t>(std::max(0.0, std::floor(value)));
        }

        return CallSetAttribute(g_setUint32, item, definition, &raw);
    }

    // An integer attribute's raw 32 bits (negative numbers too: a terminal's final offer is -5).
    bool SetAttributeRaw(EconItem* item, int index, std::uint32_t raw)
    {
        void* definition = AttributeDefinition(index);
        char typeName[96];

        if (!definition || !g_setUint32 || !AttributeTypeName(definition, typeName, sizeof(typeName)))
            return false;

        if (std::strcmp(typeName, ".?AVCSchemaAttributeType_Uint32@@") != 0 && std::strcmp(typeName, ".?AVCSchemaAttributeType_Default@@") != 0)
            return false;

        return CallSetAttribute(g_setUint32, item, definition, &raw);
    }

    // One sticker slot of a gun, every attribute of it (so nothing of a sticker that was there before stays behind).
    // An empty slot keeps only id 0: the game skips a slot whose id is 0.
    bool WriteSticker(EconItem* item, int slot, const GameInv::StickerSlot& sticker)
    {
        const int base = ATTR_STICKER_0_ID + slot * ATTR_STICKER_STRIDE;

        if (sticker.kit <= 0)
            return SetAttribute(item, base, 0, false);

        bool ok = SetAttribute(item, base, sticker.kit, false);
        ok = SetAttribute(item, base + 1, std::clamp(sticker.wear, 0.0f, 1.0f), true) && ok;
        ok = SetAttribute(item, base + 2, 1.0, true) && ok;   // scale: CS2 doesn't change it
        ok = SetAttribute(item, base + 3, sticker.rotation, true) && ok;
        ok = SetAttribute(item, ATTR_STICKER_0_OFFSET_X + slot * 2, sticker.offsetX, true) && ok;
        ok = SetAttribute(item, ATTR_STICKER_0_OFFSET_X + slot * 2 + 1, sticker.offsetY, true) && ok;
        // the gun's sticker position it's drawn at (its offsets are relative to it): where CS2's popup placed it, which
        // isn't the slot it's kept in (0 drew every sticker relative to slot 0 - the AK's receiver)
        ok = SetAttribute(item, ATTR_STICKER_0_SCHEMA + slot, sticker.schema >= 0 ? sticker.schema : slot, false) && ok;
        return ok;
    }

    // CSchemaAttributeType_<T> vfunc 6 = (type, item, definition, const char* text): the text turned into the value
    // (vfunc 0x60) and SetDynamicAttributeValue<T> with it — checked on the 2026-10-01 client for String and Uint32.
    // Only used for strings (name tags): the numbers keep going through SetAttribute.
    using SetFromTextFn = void(__fastcall*)(void* type, EconItem* item, void* definition, const char* text);
    constexpr int VFUNC_ATTRIBUTE_FROM_TEXT = 6;

    bool CallSetFromText(void* type, EconItem* item, void* definition, const char* text)
    {
        __try
        {
            const auto fn = reinterpret_cast<SetFromTextFn>((*reinterpret_cast<void***>(type))[VFUNC_ATTRIBUTE_FROM_TEXT]);
            fn(type, item, definition, text);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    bool SetAttributeText(EconItem* item, int index, const char* text)
    {
        void* definition = AttributeDefinition(index);
        char typeName[96];

        if (!definition || !text || !AttributeTypeName(definition, typeName, sizeof(typeName)) ||
            std::strcmp(typeName, ".?AVCSchemaAttributeType_String@@") != 0)
            return false;

        void* type = ReadPointer(definition, 0x18);
        return type && CallSetFromText(type, item, definition, text);
    }

    // A gun's charm / a charm item, a pet, a name tag (CreateItem).
    void WriteExtras(EconItem* item, const GameInv::NewItem& request)
    {
        if (request.sprayTint > 0)
            SetAttribute(item, ATTR_SPRAY_TINT, request.sprayTint, false);

        if (request.sprayCharges > 0)
            SetAttribute(item, ATTR_SPRAYS_REMAINING, request.sprayCharges, false);

        if (request.keychain > 0)
        {
            SetAttribute(item, ATTR_KEYCHAIN_ID, request.keychain, false);
            SetAttribute(item, ATTR_KEYCHAIN_ID + 1, request.keychainX, true);
            SetAttribute(item, ATTR_KEYCHAIN_ID + 2, request.keychainY, true);
            SetAttribute(item, ATTR_KEYCHAIN_ID + 3, request.keychainZ, true);
            SetAttribute(item, ATTR_KEYCHAIN_SEED, std::max(0, request.keychainSeed), false);

            if (request.keychainSticker > 0)
                SetAttribute(item, ATTR_KEYCHAIN_STICKER, request.keychainSticker, false);

            if (request.keychainHighlight > 0)
                SetAttribute(item, ATTR_KEYCHAIN_HIGHLIGHT, request.keychainHighlight, false);
        }

        if (request.petId > 0)
        {
            // fed for a week and not growing on its own: the dates are made again every time Lootline loads it
            const double now = static_cast<double>(std::time(nullptr));
            SetAttribute(item, ATTR_PET_ID, request.petId, false);
            SetAttribute(item, ATTR_PET_SEED, std::max(0, request.petSeed), false);
            SetAttribute(item, ATTR_UPGRADE_LEVEL, std::clamp(request.petLevel, 0, 3), false);
            // fed until after its next growth: CS2 shows "needs food" when the food runs out before that
            SetAttribute(item, ATTR_PET_FOOD, now + 400.0 * 86400.0, false);
            SetAttribute(item, ATTR_PET_UPGRADE, now + 365.0 * 86400.0, false);
            SetAttributeRaw(item, ATTR_DEPLOYMENT_DATE, request.petHatch ? request.petHatch : static_cast<std::uint32_t>(now));
        }

        if (!request.customName.empty())
            SetAttributeText(item, request.petId > 0 && request.petLevel >= 3 ? ATTR_CUSTOM_NAME_3 :
                request.petId > 0 && request.petLevel == 2 ? ATTR_CUSTOM_NAME_2 : ATTR_CUSTOM_NAME, request.customName.c_str());

        // an unsealed terminal / one of its offers
        bool terminal = true;

        if (request.expirationDate)
            terminal = SetAttributeRaw(item, ATTR_EXPIRATION_DATE, request.expirationDate) && terminal;

        if (request.offerNumber != 0)
            terminal = SetAttributeRaw(item, ATTR_QUEST_POINTS_REMAINING, static_cast<std::uint32_t>(request.offerNumber)) && terminal;

        if (request.casketId)
        {
            terminal = SetAttributeRaw(item, ATTR_CASKET_ID_LOW, static_cast<std::uint32_t>(request.casketId & 0xFFFFFFFFull)) && terminal;
            terminal = SetAttributeRaw(item, ATTR_CASKET_ID_HIGH, static_cast<std::uint32_t>(request.casketId >> 32)) && terminal;
        }

        if (request.purchasePrice > 0)
            SetAttributeRaw(item, ATTR_PURCHASE_PRICE, static_cast<std::uint32_t>(request.purchasePrice));   // only shown

        // a named storage unit: when, and how many items are in it; a Charm Detachment Pack: how many it gives
        if (request.modificationDate)
        {
            SetAttributeRaw(item, ATTR_MODIFICATION_DATE, request.modificationDate);
            SetAttributeRaw(item, ATTR_ITEMS_COUNT, static_cast<std::uint32_t>(std::max(0, request.itemsCount)));
        }
        else if (request.itemsCount > 0)
        {
            SetAttributeRaw(item, ATTR_ITEMS_COUNT, static_cast<std::uint32_t>(request.itemsCount));
        }

        if (!terminal)
            g_error = "a terminal attribute couldn't be set (see AttributeTypes in the log)";
    }

    template <typename T>
    const T* RipTarget(const std::uint8_t* instruction, int displacementAt, int instructionLength)
    {
        return reinterpret_cast<const T*>(instruction + instructionLength + *reinterpret_cast<const std::int32_t*>(instruction + displacementAt));
    }

    // ── the placement fallback: when a CS2 update breaks StickerPlacement / KeychainPlacement / PatchSlot ──
    // Each tool's "apply" (CEconTool_Sticker / _Keychain / _Patch; vfunc 8 on the 2026-10-14 build, 9 before) copies the
    // popup's globals into its apply message: mov r32 / movss xmm, [rip + global], then mov / movss [reg + field], r.
    // The tool's vtable comes from its class name (RTTI); its first virtual functions are read with MinHook's length
    // decoder, and the one that fills every wanted field gives field -> global. Checked against the patterns' readings
    // on the 2026-10-14 build: sticker slot +0x28, x +0x3C, y +0x40, rotation +0x34, wear +0x30; charm x / y / z
    // +0x3C / +0x40 / +0x44; patch slot +0x28.
    struct FieldGlobal
    {
        std::int32_t field = 0;
        const void* global = nullptr;
        bool integer = false;   // mov r32 (else movss)
    };

    struct WantedField
    {
        std::int32_t field;
        bool integer;
    };

    int DecodeFieldGlobals(const std::uint8_t* function, FieldGlobal* out, int max)
    {
        const void* loaded[32]{};   // what a register holds: 0-15 general (32-bit), 16-31 xmm (scalar); null = not a global
        int count = 0;
        const std::uint8_t* at = function;

        for (int n = 0; n < 512 && at < function + 0x800; ++n)
        {
            hde64s hs{};
            const unsigned int length = hde64_disasm(at, &hs);

            if (length == 0 || (hs.flags & F_ERROR) || hs.opcode == 0xCC)
                break;   // undecodable, or the padding after the function

            const std::uint8_t* next = at + length;
            const bool modrm = (hs.flags & F_MODRM) != 0;
            const int reg = modrm ? ((hs.modrm_reg & 7) | (hs.rex_r ? 8 : 0)) : 0;
            const bool ripMemory = modrm && hs.modrm_mod == 0 && hs.modrm_rm == 5;
            const bool fieldMemory = modrm && (hs.modrm_mod == 1 || hs.modrm_mod == 2) && hs.modrm_rm != 4;   // [reg + disp]
            const std::int32_t displacement = hs.modrm_mod == 1 ? static_cast<std::int8_t>(hs.disp.disp8) : static_cast<std::int32_t>(hs.disp.disp32);
            const bool scalar = hs.p_rep == 0xF3 && hs.opcode == 0x0F;
            const bool dword = !hs.rex_w && !hs.p_66;

            if (scalar && hs.opcode2 == 0x10 && modrm)                                   // movss xmm, ...
                loaded[16 + reg] = ripMemory ? next + static_cast<std::int32_t>(hs.disp.disp32) : nullptr;
            else if (scalar && hs.opcode2 == 0x11 && fieldMemory && loaded[16 + reg])    // movss [reg + field], xmm
            {
                if (count < max)
                    out[count++] = { displacement, loaded[16 + reg], false };
            }
            else if (hs.opcode == 0x8B && modrm)                                         // mov r32 / r64, ...
                loaded[reg] = ripMemory && dword ? next + static_cast<std::int32_t>(hs.disp.disp32) : nullptr;
            else if (hs.opcode == 0x89 && fieldMemory && dword && loaded[reg])           // mov [reg + field], r32
            {
                if (count < max)
                    out[count++] = { displacement, loaded[reg], true };
            }
            else if ((hs.opcode == 0x8D || hs.opcode == 0x31 || hs.opcode == 0x33) && modrm)   // lea / xor: something else
                loaded[reg] = nullptr;

            at = next;
        }

        return count;
    }

    // globals[i] = the global stored into wanted[i]; false if no virtual function of the class (the first 16) fills all.
    bool DecodeToolPlacement(const char* rttiName, const WantedField* wanted, int wantedCount, const void** globals)
    {
        const HMODULE client = GetModuleHandleA("client.dll");
        void** vtable = client ? FindVTable(client, rttiName) : nullptr;

        if (!vtable)
            return false;

        const auto* base = reinterpret_cast<const std::uint8_t*>(client);
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + reinterpret_cast<const IMAGE_DOS_HEADER*>(base)->e_lfanew);
        const std::uint8_t* end = base + nt->OptionalHeader.SizeOfImage;

        for (int slot = 0; slot < 16; ++slot)
        {
            const auto* function = static_cast<const std::uint8_t*>(vtable[slot]);

            if (function < base || function >= end)
                break;   // past the vtable

            FieldGlobal found[32];
            const int count = DecodeFieldGlobals(function, found, 32);
            bool all = true;

            for (int i = 0; i < wantedCount && all; ++i)
            {
                globals[i] = nullptr;

                for (int k = 0; k < count && !globals[i]; ++k)
                {
                    const auto* global = static_cast<const std::uint8_t*>(found[k].global);

                    if (found[k].field == wanted[i].field && found[k].integer == wanted[i].integer && global >= base && global < end)
                        globals[i] = global;
                }

                all = globals[i] != nullptr;
            }

            if (all)
                return true;
        }

        return false;
    }

    bool TryDecodeToolPlacement(const char* rttiName, const WantedField* wanted, int wantedCount, const void** globals)
    {
        __try
        {
            return DecodeToolPlacement(rttiName, wanted, wantedCount, globals);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    const char* g_placementSource = "signatures";   // for the log: "signatures", or what was read from the tools

    void ResolveStickers()
    {
        int decoded = 0;

        if (const std::uint8_t* p = M::FindPattern("client.dll", Patterns::StickerPlacement))
        {
            g_stickerSlot = RipTarget<std::int32_t>(p + 0x05, 2, 6);       // mov eax, [rip]
            g_stickerOffsetX = RipTarget<float>(p + 0x1C, 4, 8);           // movss xmm0, [rip]
            g_stickerOffsetY = RipTarget<float>(p + 0x35, 4, 8);
            g_stickerRotation = RipTarget<float>(p + 0x4C, 4, 8);
            g_stickerWear = RipTarget<float>(p + 0x62, 4, 8);
        }
        else
        {
            static constexpr WantedField fields[] = { { 0x28, true }, { 0x3C, false }, { 0x40, false }, { 0x34, false }, { 0x30, false } };
            const void* globals[5]{};

            if (TryDecodeToolPlacement(".?AVCEconTool_Sticker@@", fields, 5, globals))
            {
                g_stickerSlot = static_cast<const std::int32_t*>(globals[0]);
                g_stickerOffsetX = static_cast<const float*>(globals[1]);
                g_stickerOffsetY = static_cast<const float*>(globals[2]);
                g_stickerRotation = static_cast<const float*>(globals[3]);
                g_stickerWear = static_cast<const float*>(globals[4]);
                Offsets::MarkCovered("StickerPlacement");
                ++decoded;
            }
        }

        if (const std::uint8_t* p = M::FindPattern("client.dll", Patterns::KeychainPlacement))
        {
            g_keychainOffsetX = RipTarget<float>(p, 4, 8);                 // movss xmm0, [rip]
            g_keychainOffsetY = RipTarget<float>(p + 0x27, 4, 8);
            g_keychainOffsetZ = RipTarget<float>(p + 0x40, 4, 8);
        }
        else
        {
            static constexpr WantedField fields[] = { { 0x3C, false }, { 0x40, false }, { 0x44, false } };
            const void* globals[3]{};

            if (TryDecodeToolPlacement(".?AVCEconTool_Keychain@@", fields, 3, globals))
            {
                g_keychainOffsetX = static_cast<const float*>(globals[0]);
                g_keychainOffsetY = static_cast<const float*>(globals[1]);
                g_keychainOffsetZ = static_cast<const float*>(globals[2]);
                Offsets::MarkCovered("KeychainPlacement");
                ++decoded;
            }
        }

        if (const std::uint8_t* p = M::FindPattern("client.dll", Patterns::PatchSlot))
        {
            g_patchSlot = RipTarget<std::int32_t>(p + 0x05, 2, 6);         // mov eax, [rip]
        }
        else
        {
            static constexpr WantedField fields[] = { { 0x28, true } };
            const void* globals[1]{};

            if (TryDecodeToolPlacement(".?AVCEconTool_Patch@@", fields, 1, globals))
            {
                g_patchSlot = static_cast<const std::int32_t*>(globals[0]);
                Offsets::MarkCovered("PatchSlot");
                ++decoded;
            }
        }

        if (decoded > 0)
            g_placementSource = "read from CS2's tools (their signatures broke after an update)";

        g_itemViewById = reinterpret_cast<ItemViewByIdFn>(M::FindPattern("client.dll", Patterns::ItemViewById));
    }

    bool ReadPlacement(int& slot, GameInv::StickerSlot& placement)
    {
        __try
        {
            slot = *g_stickerSlot;
            placement.offsetX = *g_stickerOffsetX;
            placement.offsetY = *g_stickerOffsetY;
            placement.rotation = *g_stickerRotation;
            placement.wear = *g_stickerWear;
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    // C_EconItemView's virtual functions (client.dll 2026-10-03, IEconItemInterface): 3 / 4 / 5 the paint kit index,
    // seed, wear(fallback); 6 / 7 GetStickerAttributeBySlotIndexFloat / Int(slot, attribute, fallback); 19 GetQuality;
    // 26 GetCustomName(stage). Sticker attributes: 0 id, 1 wear, 2 scale, 3 rotation, 4 offset x, 5 offset y, 6 schema.
    constexpr int VFUNC_ITEM_PAINT_KIT = 3;
    constexpr int VFUNC_ITEM_PAINT_SEED = 4;
    constexpr int VFUNC_ITEM_PAINT_WEAR = 5;
    constexpr int VFUNC_ITEM_STICKER_FLOAT = 6;
    constexpr int VFUNC_ITEM_QUALITY = 19;
    constexpr int VFUNC_ITEM_CUSTOM_NAME = 26;
    using ItemIntFn = int(__fastcall*)(void* itemView);
    using ItemWearFn = float(__fastcall*)(void* itemView, float fallback);
    using StickerFloatFn = float(__fastcall*)(void* itemView, int slot, int attribute, float fallback);
    using CustomNameFn = const char*(__fastcall*)(void* itemView, int stage);

    // plain data only (it's filled inside __try)
    struct RawItem
    {
        std::uint16_t definition = 0;
        int quality = 0;
        int paintKit = 0;
        int seed = 0;
        float wear = 0.0f;
        char name[161]{};
        GameInv::StickerSlot stickers[GameInv::STICKER_SLOTS]{};
    };

    bool CallReadItem(std::uint64_t id, std::uint32_t definitionOffset, RawItem& out)
    {
        __try
        {
            void* view = g_itemViewById(id);

            if (!view)
                return false;

            void** vtable = *reinterpret_cast<void***>(view);
            out.definition = *reinterpret_cast<const std::uint16_t*>(static_cast<const std::uint8_t*>(view) + definitionOffset);
            out.quality = reinterpret_cast<ItemIntFn>(vtable[VFUNC_ITEM_QUALITY])(view);
            out.paintKit = reinterpret_cast<ItemIntFn>(vtable[VFUNC_ITEM_PAINT_KIT])(view);
            out.seed = reinterpret_cast<ItemIntFn>(vtable[VFUNC_ITEM_PAINT_SEED])(view);
            out.wear = reinterpret_cast<ItemWearFn>(vtable[VFUNC_ITEM_PAINT_WEAR])(view, 0.0f);

            if (const char* name = reinterpret_cast<CustomNameFn>(vtable[VFUNC_ITEM_CUSTOM_NAME])(view, -1))
                strncpy_s(out.name, name, _TRUNCATE);

            const auto stickerInt = reinterpret_cast<StickerIntFn>(vtable[VFUNC_ITEM_STICKER_INT]);
            const auto stickerFloat = reinterpret_cast<StickerFloatFn>(vtable[VFUNC_ITEM_STICKER_FLOAT]);

            for (int slot = 0; slot < GameInv::STICKER_SLOTS; ++slot)
            {
                GameInv::StickerSlot& sticker = out.stickers[slot];
                sticker.kit = stickerInt(view, slot, 0, 0);

                if (sticker.kit <= 0)
                    continue;

                sticker.wear = stickerFloat(view, slot, 1, 0.0f);
                sticker.rotation = stickerFloat(view, slot, 3, 0.0f);
                sticker.offsetX = stickerFloat(view, slot, 4, 0.0f);
                sticker.offsetY = stickerFloat(view, slot, 5, 0.0f);
                sticker.schema = stickerInt(view, slot, 6, slot);
            }

            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    int CallStickerKit(std::uint64_t id, int slot = 0)
    {
        __try
        {
            void* view = g_itemViewById(id);

            if (!view)
                return 0;

            const auto fn = reinterpret_cast<StickerIntFn>((*reinterpret_cast<void***>(view))[VFUNC_ITEM_STICKER_INT]);
            return fn(view, slot, 0, 0);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return 0;
        }
    }
}

std::string GameInv::AttributeTypes()
{
    std::string text;

    for (const int index : { ATTR_PAINT_KIT, ATTR_PAINT_SEED, ATTR_PAINT_WEAR, ATTR_KILL_EATER, ATTR_STICKER_0_ID, ATTR_MUSIC_ID })
    {
        char typeName[96] = "?";
        void* definition = AttributeDefinition(index);

        if (definition)
            AttributeTypeName(definition, typeName, sizeof(typeName));

        std::string name = definition ? typeName : "no definition";
        const std::size_t start = name.find("AttributeType_");
        if (start != std::string::npos)
            name = name.substr(start + 14, name.find('@', start) - start - 14);

        text += std::to_string(index) + "=" + name + " ";
    }

    return text;
}

bool GameInv::Initialize()
{
    g_error.clear();

    const HMODULE client = GetModuleHandleA("client.dll");

    if (!client)
    {
        g_error = "client.dll isn't loaded";
        return false;
    }

    g_useTool = M::FindPattern("client.dll", Patterns::UseTool);
    g_getManager = reinterpret_cast<GetManagerFn>(M::FindPattern("client.dll", Patterns::InventoryManager));
    g_createItem = reinterpret_cast<CreateEconItemFn>(M::FindPattern("client.dll", Patterns::CreateEconItem));
    g_createTypeCache = reinterpret_cast<CreateTypeCacheFn>(M::FindPattern("client.dll", Patterns::CreateBaseTypeCache));
    g_inventoryOffset = InventoryOffsetFromUseTool(static_cast<const std::uint8_t*>(g_useTool));
    g_setUint32 = SetterFromAttributeType(client, ".?AVCSchemaAttributeType_Uint32@@");
    g_setFloat = SetterFromAttributeType(client, ".?AVCSchemaAttributeType_Float@@");

    if (!g_useTool) g_error += "UseTool not found. ";
    if (!g_getManager) g_error += "InventoryManager not found. ";
    if (!g_createItem) g_error += "CreateEconItem not found. ";
    if (!g_createTypeCache) g_error += "CreateBaseTypeCache not found. ";
    if (!g_inventoryOffset) g_error += "Inventory offset not found in UseTool. ";
    if (!g_setUint32 || !g_setFloat) g_error += "Attribute setters not found (RTTI). ";

    ResolveStickers();   // optional: without it only stickers on Lootline guns don't work
    return g_error.empty();
}

const char* GameInv::LastError()
{
    return g_error.c_str();
}

bool GameInv::InventoryReady()
{
    void* inventory = LocalInventory();
    return inventory && ItemTypeCache(inventory) != nullptr;
}

bool GameInv::CreateItem(const NewItem& request)
{
    if (!g_createItem || !request.id || !request.definition)
        return false;

    void* inventory = LocalInventory();
    void* typeCache = ItemTypeCache(inventory);
    SOID owner{};

    if (!typeCache || !ReadOwner(inventory, owner) || HasItem(request.id))
        return false;

    EconItem* item = CallCreateItem();

    if (!item)
        return false;

    item->id = request.id;
    item->originalId = request.id;
    item->accountId = static_cast<std::uint32_t>(owner.id & 0xFFFFFFFFull);
    // NEW: bit 30 + how you got it (CS2 lists it in the new item popup until it's acknowledged - locally, Acknowledge)
    item->inventory = request.pickup > 0 ? NEW_ITEM_FLAG | static_cast<std::uint32_t>(request.pickup & 0xFF) : NextPosition();
    item->definition = request.definition;
    item->origin = static_cast<std::uint16_t>(request.origin & 0x1F);
    item->quality = static_cast<std::uint16_t>(request.quality & 0xF);
    item->level = 1;
    item->rarity = static_cast<std::uint16_t>(request.rarity & 0xF);

    if (request.paintKit > 0)
    {
        if (!SetAttribute(item, ATTR_PAINT_KIT, request.paintKit, true) ||
            !SetAttribute(item, ATTR_PAINT_SEED, request.seed, true) ||
            !SetAttribute(item, ATTR_PAINT_WEAR, request.wear, true))
            g_error = "a paint attribute couldn't be set (see AttributeTypes in the log)";
    }

    // items_game: both "stored_as_integer" "1" — plain integers (like kill eater), not float bits
    if (request.stickerKit > 0 && !SetAttribute(item, ATTR_STICKER_0_ID, request.stickerKit, false))
        g_error = "the sticker attribute couldn't be set (see AttributeTypes in the log)";

    if (request.musicKit > 0 && !SetAttribute(item, ATTR_MUSIC_ID, request.musicKit, false))
        g_error = "the music kit attribute couldn't be set (see AttributeTypes in the log)";

    if (request.statTrak)
    {
        SetAttribute(item, ATTR_KILL_EATER, request.statTrakKills, false);
        SetAttribute(item, ATTR_KILL_EATER_TYPE, request.statTrakType, false);
    }

    for (int slot = 0; slot < STICKER_SLOTS; ++slot)
    {
        if (request.stickers[slot].kit > 0 && !WriteSticker(item, slot, request.stickers[slot]))
            g_error = "a sticker attribute couldn't be set (see AttributeTypes in the log)";
    }

    WriteExtras(item, request);

    if (!CallAddObject(typeCache, item))
        return false;   // the item object is left alone (the game's allocator; a few bytes)

    CallListener(inventory, VFUNC_SO_CREATED, &owner, item);
    return true;
}

bool GameInv::Acknowledge(std::uint64_t id, bool& wasNew)
{
    wasNew = false;
    void* inventory = LocalInventory();
    void* typeCache = ItemTypeCache(inventory);
    SOID owner{};

    if (!typeCache || !ReadOwner(inventory, owner))
        return false;

    EconItem* found = nullptr;
    std::uint32_t position = 0;
    ForEachItem(typeCache, [&](EconItem* item, std::uint64_t itemId, std::uint32_t itemPosition)
    {
        if (itemId != id)
            return true;

        found = item;
        position = itemPosition;
        return false;
    });

    if (!found)
        return false;

    if (position != 0 && !(position & NEW_ITEM_FLAG))
        return true;   // acknowledged already

    // a place in the inventory, then the update the Game Coordinator's answer would bring (CS2 copies the position into
    // its own view of the item: the new count and the popup's list are rebuilt from those)
    found->inventory = NextPosition();
    wasNew = true;
    CallListener(inventory, VFUNC_SO_UPDATED, &owner, found);
    return true;
}

int GameInv::NewItemCount()
{
    int count = 0;
    ForEachItem(ItemTypeCache(LocalInventory()), [&](EconItem*, std::uint64_t, std::uint32_t position)
    {
        if (position & NEW_ITEM_FLAG)
            ++count;

        return true;
    });
    return count;
}

std::vector<std::uint64_t> GameInv::NewItemIds()
{
    std::vector<std::uint64_t> ids;
    ForEachItem(ItemTypeCache(LocalInventory()), [&](EconItem*, std::uint64_t id, std::uint32_t position)
    {
        if (position & NEW_ITEM_FLAG)
            ids.push_back(id);

        return true;
    });
    return ids;
}

bool GameInv::RemoveItem(std::uint64_t id)
{
    void* inventory = LocalInventory();
    void* typeCache = ItemTypeCache(inventory);
    SOID owner{};

    if (!typeCache || !ReadOwner(inventory, owner))
        return false;

    EconItem* found = nullptr;
    ForEachItem(typeCache, [&](EconItem* item, std::uint64_t itemId, std::uint32_t)
    {
        if (itemId != id)
            return true;

        found = item;
        return false;
    });

    if (!found)
        return false;

    // Same order as the Game Coordinator's own destroy: listeners first, then out of the cache. The object itself
    // is not deleted — the case popup / the inventory UI may still point at it for a moment.
    CallListener(inventory, VFUNC_SO_DESTROYED, &owner, found);
    CallRemoveObject(typeCache, found);
    return true;
}

bool GameInv::SetSticker(std::uint64_t id, int slot, const StickerSlot& sticker)
{
    if (slot < 0 || slot >= STICKER_SLOTS)
        return false;

    void* inventory = LocalInventory();
    void* typeCache = ItemTypeCache(inventory);
    SOID owner{};

    if (!typeCache || !ReadOwner(inventory, owner))
        return false;

    EconItem* found = nullptr;
    ForEachItem(typeCache, [&](EconItem* item, std::uint64_t itemId, std::uint32_t)
    {
        if (itemId != id)
            return true;

        found = item;
        return false;
    });

    if (!found)
        return false;

    const bool written = WriteSticker(found, slot, sticker);
    CallListener(inventory, VFUNC_SO_UPDATED, &owner, found);   // what the Game Coordinator's update does
    return written;
}

namespace
{
    // One of the items in your inventory: `write` changes its attributes, then the inventory is told (what a Game
    // Coordinator update does). False = not found / not written.
    template <typename Write>
    bool UpdateItem(std::uint64_t id, Write&& write)
    {
        void* inventory = LocalInventory();
        void* typeCache = ItemTypeCache(inventory);
        SOID owner{};

        if (!typeCache || !ReadOwner(inventory, owner))
            return false;

        EconItem* found = nullptr;
        ForEachItem(typeCache, [&](EconItem* item, std::uint64_t itemId, std::uint32_t)
        {
            if (itemId != id)
                return true;

            found = item;
            return false;
        });

        if (!found)
            return false;

        const bool written = write(found);
        CallListener(inventory, VFUNC_SO_UPDATED, &owner, found);
        return written;
    }

    bool ReadKeychainPlacement(float& x, float& y, float& z)
    {
        __try
        {
            x = *g_keychainOffsetX;
            y = *g_keychainOffsetY;
            z = *g_keychainOffsetZ;
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    int ReadPatchSlot()
    {
        __try
        {
            return *g_patchSlot;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return -1;
        }
    }
}

bool GameInv::SetKeychain(std::uint64_t id, int keychain, int seed, float x, float y, float z, int sticker, int highlight)
{
    return UpdateItem(id, [&](EconItem* item)
    {
        if (keychain <= 0)
        {
            SetAttribute(item, ATTR_KEYCHAIN_STICKER, 0, false);       // no slab sticker either
            SetAttribute(item, ATTR_KEYCHAIN_HIGHLIGHT, 0, false);     // no clip
            return SetAttribute(item, ATTR_KEYCHAIN_ID, 0, false);   // id 0 = no charm (like an empty sticker slot)
        }

        bool ok = SetAttribute(item, ATTR_KEYCHAIN_ID, keychain, false);
        ok = SetAttribute(item, ATTR_KEYCHAIN_ID + 1, x, true) && ok;
        ok = SetAttribute(item, ATTR_KEYCHAIN_ID + 2, y, true) && ok;
        ok = SetAttribute(item, ATTR_KEYCHAIN_ID + 3, z, true) && ok;
        ok = SetAttribute(item, ATTR_KEYCHAIN_SEED, std::max(0, seed), false) && ok;
        ok = SetAttribute(item, ATTR_KEYCHAIN_STICKER, std::max(0, sticker), false) && ok;   // a slab's sticker, 0 = none
        ok = SetAttribute(item, ATTR_KEYCHAIN_HIGHLIGHT, std::max(0, highlight), false) && ok;   // a highlight's clip, 0 = none
        return ok;
    });
}

bool GameInv::KeychainPlacement(float& x, float& y, float& z)
{
    x = y = z = 0.0f;
    return g_keychainOffsetX && g_keychainOffsetY && g_keychainOffsetZ && ReadKeychainPlacement(x, y, z);
}

int GameInv::PatchSlot()
{
    return g_patchSlot ? ReadPatchSlot() : -1;
}

bool GameInv::SetName(std::uint64_t id, const std::string& name, int petLevel)
{
    const int attribute = petLevel >= 3 ? ATTR_CUSTOM_NAME_3 : petLevel == 2 ? ATTR_CUSTOM_NAME_2 : ATTR_CUSTOM_NAME;
    return UpdateItem(id, [&](EconItem* item) { return SetAttributeText(item, attribute, name.c_str()); });
}

bool GameInv::SetSprayCharges(std::uint64_t id, int charges)
{
    return UpdateItem(id, [&](EconItem* item) { return SetAttribute(item, ATTR_SPRAYS_REMAINING, std::max(0, charges), false); });
}

bool GameInv::SetKills(std::uint64_t id, int kills)
{
    return UpdateItem(id, [&](EconItem* item) { return SetAttribute(item, ATTR_KILL_EATER, std::max(0, kills), false); });
}

bool GameInv::SetCasketInfo(std::uint64_t id, int itemsCount, std::uint32_t modificationDate)
{
    return UpdateItem(id, [&](EconItem* item)
    {
        return SetAttributeRaw(item, ATTR_MODIFICATION_DATE, modificationDate) &&
            SetAttributeRaw(item, ATTR_ITEMS_COUNT, static_cast<std::uint32_t>(std::max(0, itemsCount)));
    });
}

bool GameInv::SetOfferNumber(std::uint64_t id, int offerNumber)
{
    return UpdateItem(id, [&](EconItem* item) { return SetAttributeRaw(item, ATTR_QUEST_POINTS_REMAINING, static_cast<std::uint32_t>(offerNumber)); });
}

bool GameInv::StickerPlacement(int& slot, StickerSlot& placement)
{
    if (!g_stickerSlot || !g_stickerOffsetX || !g_stickerOffsetY || !g_stickerRotation || !g_stickerWear)
        return false;

    return ReadPlacement(slot, placement);
}

int GameInv::StickerKitOf(std::uint64_t id)
{
    return g_itemViewById && id ? CallStickerKit(id) : 0;
}

int GameInv::StickerKitAt(std::uint64_t id, int slot)
{
    return g_itemViewById && id && slot >= 0 && slot < STICKER_SLOTS ? CallStickerKit(id, slot) : 0;
}

bool GameInv::ReadItem(std::uint64_t id, ItemSnapshot& out)
{
    const std::uint32_t definitionOffset = SchemaFinder::GetCached<hash_32_fnv1a_const("C_EconItemView->m_iItemDefinitionIndex")>();
    RawItem raw;

    if (!g_itemViewById || !id || !definitionOffset || !CallReadItem(id, definitionOffset, raw) || raw.definition == 0)
        return false;

    out.definition = raw.definition;
    out.quality = raw.quality;
    out.paintKit = std::max(raw.paintKit, 0);
    out.seed = std::max(raw.seed, 0);
    out.wear = std::clamp(raw.wear, 0.0f, 1.0f);
    out.customName = raw.name;

    for (int slot = 0; slot < STICKER_SLOTS; ++slot)
        out.stickers[slot] = raw.stickers[slot];

    return true;
}

bool GameInv::StickersResolved()
{
    return g_stickerSlot && g_itemViewById;
}

const char* GameInv::PlacementSource()
{
    return g_placementSource;
}

bool GameInv::HasItem(std::uint64_t id)
{
    bool found = false;
    ForEachItem(ItemTypeCache(LocalInventory()), [&](EconItem*, std::uint64_t itemId, std::uint32_t)
    {
        found = itemId == id;
        return !found;
    });
    return found;
}

std::uint16_t GameInv::ItemDefinition(std::uint64_t id)
{
    std::uint16_t definition = 0;
    ForEachItem(ItemTypeCache(LocalInventory()), [&](EconItem* item, std::uint64_t itemId, std::uint32_t)
    {
        if (itemId != id)
            return true;

        __try
        {
            definition = item->definition;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
        }

        return false;
    });
    return definition;
}
std::uint32_t GameInv::NextPosition()
{
    std::uint32_t highest = 0;
    ForEachItem(ItemTypeCache(LocalInventory()), [&](EconItem*, std::uint64_t id, std::uint32_t position)
    {
        if ((id & 0xF000000000000000ull) == 0)   // default items have no position
        {
            const std::uint32_t slot = position & 0x3FFFFFFFu;
            if (slot > highest && slot < 0x10000000u)
                highest = slot;
        }
        return true;
    });
    return highest + 1;
}

std::uint32_t GameInv::AccountId()
{
    SOID owner{};
    void* inventory = LocalInventory();
    return inventory && ReadOwner(inventory, owner) ? static_cast<std::uint32_t>(owner.id & 0xFFFFFFFFull) : 0;
}

void* GameInv::UseToolFunction()
{
    return g_useTool;
}

namespace
{
    // LoadoutAPI.EquipItemInSlot(team, itemId, slot) -> bool  /  InventoryAPI.GetDefaultSlot(itemId) -> slot name.
    // Neither reads its first argument (the API object), checked on the 2026-10-01 build.
    using LoadoutEquipFn = bool(__fastcall*)(void* api, const char* team, const char* itemId, const char* slot);
    using DefaultSlotFn = const char* (__fastcall*)(void* api, const char* itemId);

    LoadoutEquipFn g_loadoutEquip = nullptr;
    DefaultSlotFn g_defaultSlot = nullptr;
    bool g_loadoutResolved = false;

    void ResolveLoadout()
    {
        if (g_loadoutResolved)
            return;

        g_loadoutResolved = true;
        g_loadoutEquip = reinterpret_cast<LoadoutEquipFn>(GameInv::InventoryApiFunction("EquipItemInSlot"));
        g_defaultSlot = reinterpret_cast<DefaultSlotFn>(GameInv::InventoryApiFunction("GetDefaultSlot"));
    }

    bool CallLoadoutEquip(const char* team, const char* itemId, const char* slot)
    {
        __try
        {
            return g_loadoutEquip(nullptr, team, itemId, slot);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    const char* CallDefaultSlot(const char* itemId)
    {
        __try
        {
            return g_defaultSlot(nullptr, itemId);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return nullptr;
        }
    }
}

namespace
{
    // InventoryAPI.GetCraftIngredientCount is "mov eax, [rcx + disp32]; ret": the contract's CUtlVector<uint64> sits at
    // that offset of the API object (count, then the item ids at +8; AddCraftIngredient caps it at 10).
    using ClearIngredientsFn = void(__fastcall*)(void* api);

    std::uint32_t g_ingredientsOffset = 0;
    ClearIngredientsFn g_clearIngredients = nullptr;
    void* g_craftIngredients = nullptr;
    bool g_craftResolved = false;

    void ResolveCrafting()
    {
        if (g_craftResolved)
            return;

        g_craftResolved = true;
        const auto* count = static_cast<const std::uint8_t*>(GameInv::InventoryApiFunction("GetCraftIngredientCount"));

        if (count && count[0] == 0x8B && count[1] == 0x81 && count[6] == 0xC3)
        {
            const std::uint32_t offset = *reinterpret_cast<const std::uint32_t*>(count + 2);
            if (offset >= 0x40 && offset < 0x10000)
                g_ingredientsOffset = offset;
        }

        g_clearIngredients = reinterpret_cast<ClearIngredientsFn>(GameInv::InventoryApiFunction("ClearCraftIngredients"));
        g_craftIngredients = GameInv::InventoryApiFunction("CraftIngredients");
    }

    int ReadIngredients(const void* api, std::uint32_t offset, std::uint64_t (&ids)[10])
    {
        __try
        {
            const auto* base = static_cast<const std::uint8_t*>(api);
            const int count = *reinterpret_cast<const int*>(base + offset);
            const std::uint64_t* data = *reinterpret_cast<const std::uint64_t* const*>(base + offset + 8);

            if (count <= 0 || count > 10 || !data)
                return count == 0 ? 0 : -1;

            for (int i = 0; i < count; ++i)
                ids[i] = data[i];

            return count;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return -1;
        }
    }

    bool CallClearIngredients(void* api)
    {
        __try
        {
            g_clearIngredients(api);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }
}

bool GameInv::CraftIngredients(void* api, std::vector<std::uint64_t>& out)
{
    out.clear();
    ResolveCrafting();

    if (!api || !g_ingredientsOffset)
        return false;

    std::uint64_t ids[10] = {};
    const int count = ReadIngredients(api, g_ingredientsOffset, ids);

    if (count < 0)
        return false;

    out.assign(ids, ids + count);
    return true;
}

bool GameInv::ClearCraftIngredients(void* api)
{
    ResolveCrafting();
    return api && g_clearIngredients && CallClearIngredients(api);
}

void* GameInv::CraftIngredientsFunction()
{
    ResolveCrafting();
    return g_ingredientsOffset ? g_craftIngredients : nullptr;   // no list offset: nothing to hook it for
}

int GameInv::CraftListIds(const void* list, std::uint64_t* ids, int max)
{
    if (!list || !ids)
        return -1;

    __try
    {
        const auto* base = static_cast<const std::uint8_t*>(list);
        const int count = *reinterpret_cast<const int*>(base);
        const EconItem* const* items = *reinterpret_cast<const EconItem* const* const*>(base + 8);

        if (count < 0 || count > max || (count > 0 && !items))
            return -1;

        for (int i = 0; i < count; ++i)
        {
            if (!items[i])
                return -1;

            ids[i] = items[i]->id;
        }

        return count;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return -1;
    }
}

void* GameInv::LoadoutEquipFunction()
{
    ResolveLoadout();
    return reinterpret_cast<void*>(g_loadoutEquip);
}

bool GameInv::LoadoutEquip(const char* team, std::uint64_t id, const char* slot)
{
    ResolveLoadout();

    if (!g_loadoutEquip || !team || !slot || !*slot)
        return false;

    char text[32];
    snprintf(text, sizeof(text), "%llu", static_cast<unsigned long long>(id));
    return CallLoadoutEquip(team, text, slot);
}

std::string GameInv::DefaultSlot(std::uint64_t id)
{
    ResolveLoadout();

    if (!g_defaultSlot)
        return {};

    char text[32];
    snprintf(text, sizeof(text), "%llu", static_cast<unsigned long long>(id));
    const char* slot = CallDefaultSlot(text);
    return slot ? std::string(slot) : std::string();
}

namespace
{
    // client.dll's .text / .rdata, and every "lea rcx, [rip + x]" in its code that loads something from .rdata (sorted
    // by what it loads). Built once (one pass over the code), after which every InventoryAPI / StoreAPI lookup is a
    // binary search - it used to be two passes over the whole DLL per name, twenty-odd names at start-up.
    struct ClientIndex
    {
        const std::uint8_t* text = nullptr;
        std::size_t textSize = 0;
        const std::uint8_t* rdata = nullptr;
        std::size_t rdataSize = 0;
        std::vector<std::pair<std::uint32_t, std::uint32_t>> loads;   // (.rdata offset loaded, .text offset of the lea)
        bool built = false;
    };

    std::mutex g_clientIndexMutex;
    ClientIndex g_clientIndex;

    // g_clientIndexMutex held. Null when client.dll isn't loaded (yet): tried again next time.
    const ClientIndex* ClientIndexLocked()
    {
        ClientIndex& index = g_clientIndex;

        if (index.built)
            return &index;

        const auto* base = reinterpret_cast<const std::uint8_t*>(GetModuleHandleA("client.dll"));

        if (!base)
            return nullptr;

        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
        const IMAGE_SECTION_HEADER* section = IMAGE_FIRST_SECTION(nt);

        for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++section)
        {
            const char* sectionName = reinterpret_cast<const char*>(section->Name);

            // the first of each, like the lookups always took
            if (!index.text && std::strncmp(sectionName, ".text", 5) == 0)
            {
                index.text = base + section->VirtualAddress;
                index.textSize = section->Misc.VirtualSize;
            }
            else if (!index.rdata && std::strncmp(sectionName, ".rdata", 6) == 0)
            {
                index.rdata = base + section->VirtualAddress;
                index.rdataSize = section->Misc.VirtualSize;
            }
        }

        if (!index.text || !index.rdata || index.textSize < 16)
        {
            index = ClientIndex{};
            return nullptr;
        }

        index.loads.reserve(1u << 18);

        for (std::size_t i = 0; i + 7 <= index.textSize; ++i)
        {
            const std::uint8_t* lea = index.text + i;

            if (lea[2] != 0x0D || lea[1] != 0x8D || lea[0] != 0x48)
                continue;

            std::int32_t displacement = 0;
            std::memcpy(&displacement, lea + 3, sizeof(displacement));
            const std::uint8_t* target = lea + 7 + displacement;

            if (target >= index.rdata && target < index.rdata + index.rdataSize)
                index.loads.emplace_back(static_cast<std::uint32_t>(target - index.rdata), static_cast<std::uint32_t>(i));
        }

        std::sort(index.loads.begin(), index.loads.end());   // by target, then by address (the old scans' order)
        index.built = true;
        return &index;
    }

    // the first zero-terminated "name" in .rdata (not the end of a longer string), like the old byte-by-byte search
    const std::uint8_t* FindStringLocked(const ClientIndex& index, const char* name)
    {
        const std::size_t length = std::strlen(name);

        if (!length || index.rdataSize < length + 2)
            return nullptr;

        const std::uint8_t* rdata = index.rdata;
        const std::uint8_t* last = rdata + index.rdataSize - length - 1;   // the last start that leaves room for the 0
        const auto first = static_cast<std::uint8_t>(name[0]);

        for (const std::uint8_t* p = rdata + 1; p <= last;)
        {
            const auto* hit = static_cast<const std::uint8_t*>(std::memchr(p, first, static_cast<std::size_t>(last - p) + 1));

            if (!hit)
                break;

            if (hit[-1] == 0 && hit[length] == 0 && std::memcmp(hit, name, length) == 0)
                return hit;

            p = hit + 1;
        }

        return nullptr;
    }
}

bool GameInv::ClientCode(const std::uint8_t*& text, std::size_t& textSize)
{
    std::lock_guard lock(g_clientIndexMutex);
    const ClientIndex* index = ClientIndexLocked();

    text = index ? index->text : nullptr;
    textSize = index ? index->textSize : 0;
    return index != nullptr;
}

const std::uint8_t* GameInv::ClientString(const char* name)
{
    if (!name || !*name)
        return nullptr;

    std::lock_guard lock(g_clientIndexMutex);
    const ClientIndex* index = ClientIndexLocked();
    return index ? FindStringLocked(*index, name) : nullptr;
}

void* GameInv::StoreApiFunction(const char* name)
{
    if (!name || !*name)
        return nullptr;

    std::lock_guard lock(g_clientIndexMutex);
    const ClientIndex* index = ClientIndexLocked();
    const std::uint8_t* found = index ? FindStringLocked(*index, name) : nullptr;

    if (!found)
        return nullptr;

    const std::uint8_t* text = index->text;
    const std::size_t textSize = index->textSize;
    const auto key = static_cast<std::uint32_t>(found - index->rdata);
    const auto range = std::equal_range(index->loads.begin(), index->loads.end(), std::make_pair(key, 0u),
        [](const std::pair<std::uint32_t, std::uint32_t>& a, const std::pair<std::uint32_t, std::uint32_t>& b) { return a.first < b.first; });

    // lea rcx, [rip + name] (48 8D 0D rel32), and up to 0x40 bytes before it lea rax, [rip + function] (48 8D 05 rel32)
    // / mov [rbp-10h], rax (48 89 45 F0) - StoreAPI's registration (client.dll+0x108F680, 2026-10-03)
    for (auto it = range.first; it != range.second; ++it)
    {
        if (it->second < 0x40)
            continue;

        const std::uint8_t* lea = text + it->second;

        for (std::size_t back = 11; back <= 0x40; ++back)
        {
            const std::uint8_t* p = lea - back;

            if (p[0] != 0x48 || p[1] != 0x8D || p[2] != 0x05 || p[7] != 0x48 || p[8] != 0x89 || p[9] != 0x45 || p[10] != 0xF0)
                continue;

            std::int32_t displacement = 0;
            std::memcpy(&displacement, p + 3, sizeof(displacement));
            const std::uint8_t* function = p + 7 + displacement;

            if (function >= text && function < text + textSize)
                return const_cast<std::uint8_t*>(function);

            break;
        }
    }

    return nullptr;
}

void* GameInv::InventoryApiFunction(const char* name)
{
    if (!name || !*name)
        return nullptr;

    std::lock_guard lock(g_clientIndexMutex);
    const ClientIndex* index = ClientIndexLocked();

    // the name, as its own zero-terminated string
    const std::uint8_t* found = index ? FindStringLocked(*index, name) : nullptr;

    if (!found)
        return nullptr;

    const std::uint8_t* text = index->text;
    const std::size_t textSize = index->textSize;
    const auto key = static_cast<std::uint32_t>(found - index->rdata);
    const auto range = std::equal_range(index->loads.begin(), index->loads.end(), std::make_pair(key, 0u),
        [](const std::pair<std::uint32_t, std::uint32_t>& a, const std::pair<std::uint32_t, std::uint32_t>& b) { return a.first < b.first; });

    // lea rdx, [rip + function]  (48 8D 15 rel32)  followed by  lea rcx, [rip + name]  (48 8D 0D rel32)
    for (auto it = range.first; it != range.second; ++it)
    {
        if (it->second < 7)
            continue;

        const std::uint8_t* previous = text + it->second - 7;

        if (previous[0] != 0x48 || previous[1] != 0x8D || previous[2] != 0x15)
            continue;

        std::int32_t displacement = 0;
        std::memcpy(&displacement, previous + 3, sizeof(displacement));
        const std::uint8_t* function = previous + 7 + displacement;

        if (function >= text && function < text + textSize)
            return const_cast<std::uint8_t*>(function);
    }

    return nullptr;
}
