#include "interfaces.h"

#include "../../patterns.h"
#include "../utils/memory/Interface/Interface.h"
#include "../utils/memory/patternscan/patternscan.h"
#include "../utils/offsets/offsets.h"
#include "../../cs2/entity/C_CSPlayerPawn/C_CSPlayerPawn.h"

#include <atomic>

bool I::Interfaces::init()
{
    const HMODULE tier0Base = GetModuleHandleA("tier0.dll");
    if (!tier0Base)
        return false;

    EngineClient = I::Get<IEngineClient>("engine2.dll", "Source2EngineToClient00");
    GameEntity = I::Get<IGameResourceService>("engine2.dll", "GameResourceServiceClientV00");
    InputSystem = I::Get<void>("inputsystem.dll", "InputSystemVersion001");
    Source2Client = I::Get<CSource2Client>("client.dll", "Source2Client002");
    FileSystem = I::Get<IFileSystem>("filesystem_stdio.dll", "VFileSystem017");
    Localize = I::Get<ILocalize>("localize.dll", "Localize_001");

    const auto inputPattern = M::FindPattern("client.dll", Patterns::InputPointer);
    if (inputPattern)
        Input = *reinterpret_cast<void**>(M::GetAbsoluteAddress(inputPattern, 0x3));
    else if (const std::uintptr_t input = Offsets::FreshClient("dwCSGOInput"))   // signature broke: offsets\offsets.json
        Input = *reinterpret_cast<void**>(input);

    ConstructUtlBuffer = reinterpret_cast<decltype(ConstructUtlBuffer)>(GetProcAddress(tier0Base, "??0CUtlBuffer@@QEAA@HHW4BufferFlags_t@0@@Z"));
    DetachUtlBufferMemory = reinterpret_cast<decltype(DetachUtlBufferMemory)>(GetProcAddress(tier0Base, "?DetachMemory@CUtlBuffer@@QEAAPEAXXZ"));
    MemAlloc = reinterpret_cast<decltype(MemAlloc)>(GetProcAddress(tier0Base, "MemAlloc_AllocFunc"));
    MemFree = reinterpret_cast<decltype(MemFree)>(GetProcAddress(tier0Base, "MemAlloc_FreeFunc"));

    // agent voice lines (from the new Chadware source): sound events, the response rules, the radio wheel
    SymbolString = reinterpret_cast<SymbolStringFn>(GetProcAddress(tier0Base, "?String@CUtlSymbolTable@@QEBAPEBDVCUtlSymbol@@@Z"));
    FindKey = reinterpret_cast<decltype(FindKey)>(GetProcAddress(tier0Base, "?FindKey@KeyValues@@QEBAPEBV1@PEBD@Z"));
    GetKeyValueString = reinterpret_cast<decltype(GetKeyValueString)>(GetProcAddress(tier0Base, "?Internal_GetString@CKeyValues_Data@@IEAAPEBDPEBDPEAD_K@Z"));
    SoundOpSystem = I::Get<ISoundOpSystem>("soundsystem.dll", "SoundOpSystem001");
    ResponseRulesCache = I::Get<IResponseRulesCache>("scenefilecache.dll", "ResponseRulesCache001");

    if (auto* callback = M::FindPattern("client.dll", Patterns::AgentRulesDump))
        ResponseManager = reinterpret_cast<IResponseSystemManager**>(M::GetAbsoluteAddress(callback + 4, 3));

    if (auto* wheel = M::FindPattern("client.dll", Patterns::AgentWheelTable))
    {
        WheelBegin = reinterpret_cast<const ResponseRules::WheelRow*>(M::GetAbsoluteAddress(wheel, 3));
        WheelEnd = reinterpret_cast<const ResponseRules::WheelRow*>(M::GetAbsoluteAddress(wheel + 35, 3));
    }

    if (auto* registry = M::FindPattern("client.dll", Patterns::AgentWeaponRegistry))
        WeaponRegistry = reinterpret_cast<WeaponDefinitionRegistry**>(M::GetAbsoluteAddress(registry + 15, 3));

    return EngineClient && GameEntity && Input && InputSystem && Source2Client && FileSystem && Localize && ConstructUtlBuffer && DetachUtlBufferMemory && MemAlloc && MemFree;
}

C_CSPlayerPawn* I::GetLocalPawn()
{
    using Fn = C_CSPlayerPawn* (__fastcall*)(int);

    // Scan once. (ResolveCached retries a failed scan on every call, which is every frame here.)
    static constinit std::atomic<Fn> function{ nullptr };
    static constinit std::atomic_bool scanned{ false };

    if (!scanned.load(std::memory_order_acquire))
    {
        function.store(reinterpret_cast<Fn>(M::scan("client.dll", Patterns::GetLocalPawn)), std::memory_order_release);
        scanned.store(true, std::memory_order_release);
    }

    if (const Fn fn = function.load(std::memory_order_acquire))
        return fn(0);

    // Signature broke after a CS2 update: the pawn pointer from a fresh offsets\offsets.json.
    if (const std::uintptr_t pawn = Offsets::FreshClient("dwLocalPlayerPawn"))
        return *reinterpret_cast<C_CSPlayerPawn**>(pawn);

    return nullptr;
}
