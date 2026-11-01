#include "custom_color.h"

#include <Windows.h>
#include <intrin.h>
#include <atomic>
#include <cstdio>
#include <cstring>

#include "unboxer.h"
#include "../../../patterns.h"
#include "../../config/config.h"
#include "../../hooks/includeHooks.h"
#include "../../utils/memory/patternscan/patternscan.h"
#include "../../utils/schema/schema.h"
#include "../../interfaces/interfaces.h"
#include "../skinchanger/skin_application.h"
#include "../../../cs2/entity/C_CSPlayerPawn/C_CSPlayerPawn.h"
#include "../../../cs2/entity/C_CSWeaponBase/C_CSWeaponBase.h"

#pragma intrinsic(_ReturnAddress)

namespace
{
    constexpr std::int32_t LOOSE_VARIABLE_TYPE_COLOR4 = 9;   // CompositeMaterialInputLooseVariableType_t

    // The builders (client.dll, build 14192):
    //   legacy  @+0x8004B0 (entity, model, bool, bool, ?)    - "weapons/paints/legacy/": in-match guns + knives,
    //                                                           RegenerateSkins
    //   modern  @+0xE44930 (item, model, ...8 args)          - CS2's screens ("ui menu composite"), the skin changer's
    //                                                           and the lineups' own builds (Patterns::BuildModernSkin)
    //   gloves  @+0xC66210 (the pawn's glove helper)         - "gloves/paints/": gloves in a match (the pawn at +0xB0)
    // Each fills a CUtlVector of CompositeMaterialInputLooseVariable_t (0x288 bytes) with append @+0x8002A0 and makes
    // every variable like the game's wear one: a float constructor @+0x782230 (name, value), the append (a copy), the
    // destructor @+0x782CE0 on the temporary.
    using BuildLegacyFn = void*(__fastcall*)(void* entity, void* model, std::uint64_t a3, std::uint64_t a4, void* a5);
    using BuildModernFn = void*(__fastcall*)(void* item, void* model, std::uint64_t a3, std::uint64_t a4, std::uint64_t a5,
        std::uint64_t a6, void* a7, std::uint64_t a8);
    using BuildGlovesFn = void*(__fastcall*)(void* gloves, void* a2, void* a3, void* a4);
    using AppendFn = std::int64_t(__fastcall*)(void* variables, const void* variable);
    using MakeFloatFn = void*(__fastcall*)(void* variable, const char* name, float value);
    using DestroyFn = void(__fastcall*)(void* variable);

    void* __fastcall hkBuildLegacy(void* entity, void* model, std::uint64_t a3, std::uint64_t a4, void* a5);
    void* __fastcall hkBuildModern(void* item, void* model, std::uint64_t a3, std::uint64_t a4, std::uint64_t a5,
        std::uint64_t a6, void* a7, std::uint64_t a8);
    void* __fastcall hkBuildGloves(void* gloves, void* a2, void* a3, void* a4);
    std::int64_t __fastcall hkAppend(void* variables, const void* variable);

    CInlineHookObj<BuildLegacyFn> g_buildLegacy{};
    CInlineHookObj<BuildModernFn> g_buildModern{};
    CInlineHookObj<BuildGlovesFn> g_buildGloves{};
    CInlineHookObj<AppendFn> g_append{};

    MakeFloatFn g_makeFloat = nullptr;
    DestroyFn g_destroy = nullptr;

    std::uintptr_t g_legacyStart = 0;
    std::uintptr_t g_modernStart = 0;
    std::uintptr_t g_glovesStart = 0;
    std::int32_t g_glovesPawn = 0;   // the glove helper's pawn (mov r14, [rcx+disp32] in the glove builder)

    // schema offsets, read once at Install (the hooks only use these)
    std::uint32_t g_offType = 0;
    std::uint32_t g_offColor = 0;
    std::uint32_t g_offDefinition = 0;
    std::uint32_t g_offItemId = 0;
    std::uint32_t g_offItemIdHigh = 0;
    std::uint32_t g_offAttributeManager = 0;
    std::uint32_t g_offItem = 0;
    std::uint32_t g_offGloves = 0;

    // the coloured items (Publish) + the preview's temporary item (SetPreview)
    SRWLOCK g_lock = SRWLOCK_INIT;
    std::vector<CustomColor::Entry> g_entries;
    std::uint64_t g_previewId = 0;
    const void* g_previewItem = nullptr;   // CS2 keeps it in its preview slot until 2 previews later - by then it's replaced
    CustomColor::Paint g_previewPaint{};

    // the item views of the weapons you carry (and their first-person models') and of your gloves, every game frame
    // (UpdateYourItems): a match colours only these. CS2 gives EVERY default item id high -1 - bots' guns, anyone's stock
    // knife - so "id high -1 + definition" alone coloured other players' items of the same kind too.
    constexpr int MAX_YOUR_ITEMS = 40;
    const void* g_yourItems[MAX_YOUR_ITEMS]{};
    int g_yourItemCount = 0;
    std::atomic_bool g_anything{ false };   // nothing coloured: the hooks only pass through

    enum class Subject : std::uint8_t
    {
        Item,      // a C_EconItemView
        Entity,    // a C_EconEntity (its item: m_AttributeManager.m_Item)
        Gloves,    // the glove helper (its pawn's m_EconGloves)
    };

    // The skin material this thread is building right now (nested builders keep the outer one).
    struct Build
    {
        const void* subject = nullptr;
        Subject kind = Subject::Item;
        std::uintptr_t function = 0;   // only the builder's own appends count (not a name tag's built inside it)
        bool done = false;             // the colours go in once, behind its first variable
    };

    // Per thread, in a Windows TLS slot (TlsAlloc) - not C++ thread_local: the game's threads were made before this DLL
    // and the code here never leans on the CRT's per-thread setup on them (see M::ResolveCached). The slot is never
    // freed (a game thread may still be inside a builder while unloading) - one slot per load.
    DWORD g_buildSlot = TLS_OUT_OF_INDEXES;

    Build* CurrentBuild()
    {
        return static_cast<Build*>(TlsGetValue(g_buildSlot));
    }

    struct BuildScope
    {
        Build build;
        Build* previous;

        BuildScope(const void* subject, Subject kind, std::uintptr_t function)
            : build{ subject, kind, function }, previous(CurrentBuild())
        {
            TlsSetValue(g_buildSlot, &build);
        }

        ~BuildScope() { TlsSetValue(g_buildSlot, previous); }

        BuildScope(const BuildScope&) = delete;
        BuildScope& operator=(const BuildScope&) = delete;
    };

    // The function an address is in (its first part - big functions are split into chained parts in .pdata).
    std::uintptr_t FunctionStart(std::uintptr_t address)
    {
        DWORD64 base = 0;
        const RUNTIME_FUNCTION* function = RtlLookupFunctionEntry(address, &base, nullptr);

        for (int depth = 0; function && depth < 16; ++depth)
        {
            const auto* unwind = reinterpret_cast<const std::uint8_t*>(base + function->UnwindData);

            if (((unwind[0] >> 3) & UNW_FLAG_CHAININFO) == 0)
                return static_cast<std::uintptr_t>(base + function->BeginAddress);

            // a chained part: the RUNTIME_FUNCTION of the part before it follows the unwind codes
            function = reinterpret_cast<const RUNTIME_FUNCTION*>(unwind + 4 + ((unwind[2] + 1) & ~1) * 2);
        }

        return 0;
    }

    struct ItemInfo
    {
        const void* item = nullptr;
        std::uint16_t definition = 0;
        std::uint64_t id = 0;
        std::uint32_t idHigh = 0;
    };

    // SEH only (no C++ objects): the subject comes straight from the game
    bool ReadItem(const Build& build, ItemInfo& out)
    {
        __try
        {
            const auto* item = static_cast<const std::uint8_t*>(build.subject);

            if (item && build.kind == Subject::Entity)
                item += g_offAttributeManager + g_offItem;
            else if (item && build.kind == Subject::Gloves)
            {
                const auto* pawn = *reinterpret_cast<const std::uint8_t* const*>(item + g_glovesPawn);
                item = pawn ? pawn + g_offGloves : nullptr;
            }

            if (!item)
                return false;

            out.item = item;
            out.definition = *reinterpret_cast<const std::uint16_t*>(item + g_offDefinition);
            out.id = *reinterpret_cast<const std::uint64_t*>(item + g_offItemId);
            out.idHigh = *reinterpret_cast<const std::uint32_t*>(item + g_offItemIdHigh);
            return out.definition != 0;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    bool FindPaint(const ItemInfo& info, CustomColor::Paint& out)
    {
        const std::uint8_t side = Config::liveTeam.load(std::memory_order_relaxed) == Config::TEAM_CT ? 2 : 1;
        bool found = false;

        AcquireSRWLockShared(&g_lock);

        // the menu's 3D preview: CS2 hands its panel the very item in the preview slot (it finds 0x7FFF7FFF7FFF7FFF / ..FE by
        // slot, not by the item's id field) - by that item, or by the id if the item carries it
        if (g_previewPaint.on && ((g_previewItem && info.item == g_previewItem) || (g_previewId != 0 && info.id == g_previewId)))
        {
            out = g_previewPaint;
            found = true;
        }

        // CS2's screens: the Lootline item itself
        for (std::size_t i = 0; !found && i < g_entries.size(); ++i)
        {
            if (g_entries[i].id == info.id)
            {
                out = g_entries[i].paint;
                found = true;
            }
        }

        // a match: what the skin changer put on (item id high -1) on one of YOUR items - the equipped item of that
        // definition on your side
        bool yours = false;
        for (int i = 0; !found && !yours && info.idHigh == 0xFFFFFFFFu && i < g_yourItemCount; ++i)
            yours = g_yourItems[i] == info.item;

        for (std::size_t i = 0; !found && yours && i < g_entries.size(); ++i)
        {
            if (g_entries[i].definition == info.definition && (g_entries[i].sides & side))
            {
                out = g_entries[i].paint;
                found = true;
            }
        }

        ReleaseSRWLockShared(&g_lock);
        return found && out.on;
    }

    std::uint32_t MixColor(std::uint32_t from, std::uint32_t to, int part, int parts)
    {
        std::uint32_t mixed = 0;

        for (int shift = 0; shift < 32; shift += 8)
        {
            const int a = static_cast<int>((from >> shift) & 0xFF);
            const int b = static_cast<int>((to >> shift) & 0xFF);
            mixed |= static_cast<std::uint32_t>(a + (b - a) * part / parts) << shift;
        }

        return mixed;
    }

    // One COLOR4 variable, made like the game makes its own: its float constructor (name, value), then a colour. The
    // append keeps a copy, the temporary is destroyed by the game's destructor - no memory of ours ever reaches it.
    void Emit(void* variables, const char* name, std::uint32_t color)
    {
        alignas(16) std::uint8_t variable[0x400]{};   // the game's is 0x288; room to spare if it grows

        g_makeFloat(variable, name, 0.0f);
        *reinterpret_cast<std::int32_t*>(variable + g_offType) = LOOSE_VARIABLE_TYPE_COLOR4;
        *reinterpret_cast<std::uint32_t*>(variable + g_offColor) = color;
        g_append.GetOriginal()(variables, variable);
        g_destroy(variable);
    }

    void EmitPaint(void* variables, const CustomColor::Paint& paint)
    {
        const std::uint32_t a = paint.primary;
        const std::uint32_t b = paint.secondary;

        switch (paint.mode)
        {
        case CustomColor::MODE_TINT:
            Emit(variables, "g_vColor0", a);
            break;

        case CustomColor::MODE_TWO_TONE:   // base slots 0 / 2, the pattern's 1 / 3
            Emit(variables, "g_vColor0", a);
            Emit(variables, "g_vColor2", a);
            Emit(variables, "g_vColor1", b);
            Emit(variables, "g_vColor3", b);
            break;

        case CustomColor::MODE_GRADIENT:
            Emit(variables, "g_vColor0", a);
            Emit(variables, "g_vColor1", MixColor(a, b, 1, 3));
            Emit(variables, "g_vColor2", MixColor(a, b, 2, 3));
            Emit(variables, "g_vColor3", b);
            break;

        default:
            Emit(variables, "g_vColor0", a);
            Emit(variables, "g_vColor1", a);
            Emit(variables, "g_vColor2", a);
            Emit(variables, "g_vColor3", a);
            break;
        }
    }

    std::int64_t __fastcall hkAppend(void* variables, const void* variable)
    {
        const H::HookCallGuard guard;
        const std::int64_t result = g_append.GetOriginal()(variables, variable);
        Build* build = CurrentBuild();

        if (!build || build->done || !variables)
            return result;

        if (FunctionStart(reinterpret_cast<std::uintptr_t>(_ReturnAddress())) != build->function)
            return result;   // a list some other code is filling while the builder runs

        build->done = true;

        ItemInfo info;
        CustomColor::Paint paint;

        if (ReadItem(*build, info) && FindPaint(info, paint))
            EmitPaint(variables, paint);

        return result;
    }

    void* __fastcall hkBuildLegacy(void* entity, void* model, std::uint64_t a3, std::uint64_t a4, void* a5)
    {
        const H::HookCallGuard guard;

        if (!g_anything.load(std::memory_order_relaxed))
            return g_buildLegacy.GetOriginal()(entity, model, a3, a4, a5);

        const BuildScope scope(entity, Subject::Entity, g_legacyStart);
        return g_buildLegacy.GetOriginal()(entity, model, a3, a4, a5);
    }

    void* __fastcall hkBuildModern(void* item, void* model, std::uint64_t a3, std::uint64_t a4, std::uint64_t a5,
        std::uint64_t a6, void* a7, std::uint64_t a8)
    {
        const H::HookCallGuard guard;

        if (!g_anything.load(std::memory_order_relaxed))
            return g_buildModern.GetOriginal()(item, model, a3, a4, a5, a6, a7, a8);

        const BuildScope scope(item, Subject::Item, g_modernStart);
        return g_buildModern.GetOriginal()(item, model, a3, a4, a5, a6, a7, a8);
    }

    void* __fastcall hkBuildGloves(void* gloves, void* a2, void* a3, void* a4)
    {
        const H::HookCallGuard guard;

        if (!g_anything.load(std::memory_order_relaxed))
            return g_buildGloves.GetOriginal()(gloves, a2, a3, a4);

        const BuildScope scope(gloves, Subject::Gloves, g_glovesStart);
        return g_buildGloves.GetOriginal()(gloves, a2, a3, a4);
    }

    bool InClient(const void* address)
    {
        const auto client = reinterpret_cast<const std::uint8_t*>(GetModuleHandleA("client.dll"));
        if (!client || !address)
            return false;

        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(client + reinterpret_cast<const IMAGE_DOS_HEADER*>(client)->e_lfanew);
        const auto* at = static_cast<const std::uint8_t*>(address);
        return at >= client && at < client + nt->OptionalHeader.SizeOfImage;
    }

    // The resolved float constructor must be one: it sets the type to 5 (FLOAT1) itself - `mov dword ptr [rcx+40h], 5`
    // (or [rbx+40h] at its end) within its first 0x140 bytes.
    bool LooksLikeFloatConstructor(const std::uint8_t* code)
    {
        static constexpr std::uint8_t byRcx[] = { 0xC7, 0x41, 0x40, 0x05, 0x00, 0x00, 0x00 };
        static constexpr std::uint8_t byRbx[] = { 0xC7, 0x43, 0x40, 0x05, 0x00, 0x00, 0x00 };

        for (int i = 0; i + static_cast<int>(sizeof(byRcx)) <= 0x140; ++i)
            if (std::memcmp(code + i, byRcx, sizeof(byRcx)) == 0 || std::memcmp(code + i, byRbx, sizeof(byRbx)) == 0)
                return true;

        return false;
    }

    void Fail(const char* why)
    {
        char line[200];
        snprintf(line, sizeof(line), "custom colours: NOT installed (%s) - skins keep their own colours", why);
        Unboxer::LogLine(line);
    }
}

bool CustomColor::Install()
{
    g_offType = SchemaFinder::GetCached<hash_32_fnv1a_const("CompositeMaterialInputLooseVariable_t->m_nVariableType")>();
    g_offColor = SchemaFinder::GetCached<hash_32_fnv1a_const("CompositeMaterialInputLooseVariable_t->m_cValueColor4")>();
    g_offDefinition = SchemaFinder::GetCached<hash_32_fnv1a_const("C_EconItemView->m_iItemDefinitionIndex")>();
    g_offItemId = SchemaFinder::GetCached<hash_32_fnv1a_const("C_EconItemView->m_iItemID")>();
    g_offItemIdHigh = SchemaFinder::GetCached<hash_32_fnv1a_const("C_EconItemView->m_iItemIDHigh")>();
    g_offAttributeManager = SchemaFinder::GetCached<hash_32_fnv1a_const("C_EconEntity->m_AttributeManager")>();
    g_offItem = SchemaFinder::GetCached<hash_32_fnv1a_const("C_AttributeContainer->m_Item")>();
    g_offGloves = SchemaFinder::GetCached<hash_32_fnv1a_const("C_CSPlayerPawn->m_EconGloves")>();

    if (!g_offType || !g_offColor || !g_offDefinition || !g_offItemId || !g_offItemIdHigh || !g_offAttributeManager || !g_offItem || !g_offGloves ||
        g_offColor + sizeof(std::uint32_t) > 0x400 || g_offType + sizeof(std::int32_t) > 0x400)
    {
        Fail("schema fields");
        return false;
    }

    // the game's own loose-variable functions, from the modern builder's wear variable
    std::uint8_t* site = M::scan("client.dll", Patterns::CompositeLooseVariable);
    auto* makeFloat = M::GetAbsoluteAddress(site, 18);
    auto* append = M::GetAbsoluteAddress(site, 31);
    auto* destroy = M::GetAbsoluteAddress(site, 43);

    if (!site || !InClient(makeFloat) || !InClient(append) || !InClient(destroy) || !LooksLikeFloatConstructor(makeFloat))
    {
        Fail("signature CompositeLooseVariable");
        return false;
    }

    std::uint8_t* legacy = M::scan("client.dll", Patterns::CompositeLegacyWeapon);
    std::uint8_t* modern = M::scan("client.dll", Patterns::BuildModernSkin);
    std::uint8_t* gloves = M::scan("client.dll", Patterns::CompositeGloves);

    if (!legacy || !modern || !gloves)
    {
        Fail(!legacy ? "signature CompositeLegacyWeapon" : !modern ? "signature BuildModernSkin" : "signature CompositeGloves");
        return false;
    }

    if (g_buildSlot == TLS_OUT_OF_INDEXES)
        g_buildSlot = TlsAlloc();

    if (g_buildSlot == TLS_OUT_OF_INDEXES)
    {
        Fail("no TLS slot");
        return false;
    }

    std::memcpy(&g_glovesPawn, gloves + 24, sizeof(g_glovesPawn));   // mov r14, [rcx+disp32]
    if (g_glovesPawn <= 0 || g_glovesPawn > 0x1000)
    {
        Fail("glove helper layout");
        return false;
    }

    g_makeFloat = reinterpret_cast<MakeFloatFn>(makeFloat);
    g_destroy = reinterpret_cast<DestroyFn>(destroy);
    g_legacyStart = reinterpret_cast<std::uintptr_t>(legacy);
    g_modernStart = reinterpret_cast<std::uintptr_t>(modern);
    g_glovesStart = reinterpret_cast<std::uintptr_t>(gloves);

    // Hooked, their signatures can't match live memory any more: the skin changer and the lineups scan BuildModernSkin
    // after this, Settings' signature check all three.
    M::RememberHooked(Patterns::BuildModernSkin, modern);
    M::RememberHooked(Patterns::CompositeLegacyWeapon, legacy);
    M::RememberHooked(Patterns::CompositeGloves, gloves);

    const bool hooked = g_append.Add(append, reinterpret_cast<void*>(&hkAppend)) &&
        g_buildLegacy.Add(legacy, reinterpret_cast<void*>(&hkBuildLegacy)) &&
        g_buildModern.Add(modern, reinterpret_cast<void*>(&hkBuildModern)) &&
        g_buildGloves.Add(gloves, reinterpret_cast<void*>(&hkBuildGloves));

    if (!hooked)
    {
        g_buildGloves.Remove();
        g_buildModern.Remove();
        g_buildLegacy.Remove();
        g_append.Remove();
        Fail("MinHook");
        return false;
    }

    Unboxer::LogLine("custom colours: ok (guns, knives and gloves in matches, CS2's screens and the preview)");
    return true;
}

void CustomColor::Uninstall()
{
    if (g_buildGloves.IsHooked())
        g_buildGloves.Restore();

    if (g_buildModern.IsHooked())
        g_buildModern.Restore();

    if (g_buildLegacy.IsHooked())
        g_buildLegacy.Restore();

    if (g_append.IsHooked())
        g_append.Restore();
}

namespace
{
    // Your gloves', weapons' and their first-person models' item views. SEH: all of it comes from the game.
    __declspec(noinline) int CollectYourItems(C_CSPlayerPawn* pawn, const void** out, int capacity)
    {
        int count = 0;

        __try
        {
            if (C_EconItemView* gloves = pawn->m_EconGloves())
                out[count++] = gloves;

            CCSPlayer_WeaponServices* services = pawn->m_pWeaponServices();

            if (!services)
                return count;

            auto& handles = services->m_hMyWeapons();

            for (int i = 0; i < handles.count() && count + 2 <= capacity; ++i)
            {
                const CBaseHandle handle = handles.element(i);
                auto* weapon = handle.valid() ? I::GameEntity->Instance->Get<C_CSWeaponBase>(handle) : nullptr;

                if (!weapon)
                    continue;

                out[count++] = reinterpret_cast<const std::uint8_t*>(weapon) + g_offAttributeManager + g_offItem;

                if (C_BaseEntity* hud = SkinApplication::HudWeaponOf(weapon, pawn))
                    out[count++] = reinterpret_cast<const std::uint8_t*>(hud) + g_offAttributeManager + g_offItem;
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
        }

        return count;
    }
}

void CustomColor::UpdateYourItems(void* pawn)
{
    const void* items[MAX_YOUR_ITEMS]{};
    const int count = pawn && g_offAttributeManager && I::GameEntity && I::GameEntity->Instance ?
        CollectYourItems(static_cast<C_CSPlayerPawn*>(pawn), items, MAX_YOUR_ITEMS) : 0;

    AcquireSRWLockExclusive(&g_lock);
    std::memcpy(g_yourItems, items, sizeof(items));
    g_yourItemCount = count;
    ReleaseSRWLockExclusive(&g_lock);
}

void CustomColor::Publish(std::vector<Entry> entries)
{
    AcquireSRWLockExclusive(&g_lock);
    g_entries.swap(entries);
    g_anything.store(!g_entries.empty() || g_previewPaint.on, std::memory_order_relaxed);
    ReleaseSRWLockExclusive(&g_lock);
}

void CustomColor::SetPreview(std::uint64_t itemId, const void* itemView, const Paint& paint)
{
    AcquireSRWLockExclusive(&g_lock);
    g_previewId = itemId;
    g_previewItem = itemView;
    g_previewPaint = paint;
    g_anything.store(!g_entries.empty() || g_previewPaint.on, std::memory_order_relaxed);
    ReleaseSRWLockExclusive(&g_lock);
}
