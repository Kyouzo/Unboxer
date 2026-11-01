// The knife inspect fix from the new Chadware source (2026-10-07): inspecting a changed knife again while its inspect is
// still playing (the karambit spin) could break the animation - the viewmodel graph missed the re-entry (no action
// reset) or replayed an older inspect state (a stale reset). Only the local player's changed knife, only on the game
// thread; every native read is guarded.
#include "knife_animation.h"

#include <Windows.h>
#include <atomic>
#include <cstddef>
#include <cstdint>

#include "knife_model_changer.h"
#include "../../hooks/hooks.h"
#include "../../../cs2/entity/C_BaseAnimGraph/C_BaseAnimGraph.h"
#include "../../../cs2/entity/C_CSPlayerPawn/C_CSPlayerPawn.h"
#include "../../config/config.h"
#include "../../interfaces/interfaces.h"

namespace
{
    constexpr int FRAME_RENDER_END = 6;
    constexpr int FRAME_NET_UPDATE_END = 7;
    constexpr int Inspecting = 1000;
    constexpr int InspectOutro = 1001;
    constexpr int InspectIntroPhase = 1;
    constexpr int InspectOutroPhase = 3;
    constexpr int ResetClear = 0;
    constexpr int ResetPulse = 1;
    using KnifeAnimation::Snapshot;
    using KnifeAnimation::ViewmodelIdentity;

    std::atomic_bool g_ready{false};
    std::atomic_bool g_stopping{false};
    std::atomic<DWORD> g_gameThread{0};

    // Only the observed game thread owns this state. Saved native addresses are
    // identity keys and are never dereferenced after their callback returns.
    struct ReentryState
    {
        std::uintptr_t pawn = 0;
        std::uint32_t pawnHandle = 0xFFFFFFFF;
        float spawnTime = 0;
        ViewmodelIdentity viewmodel;
        Snapshot previousViewmodel;
        Snapshot request;
        float inspectStartTime = 0;
        bool hasViewmodel = false;
        bool hasRequest = false;
        bool hasInspectStart = false;
    };

    ReentryState g_reentry;

    struct LocalKnife
    {
        C_CSPlayerPawn* pawn = nullptr;
        CCSPlayer_WeaponServices* services = nullptr;
        C_CSWeaponBase* weapon = nullptr;
    };

    LocalKnife ResolveLocalKnife()
    {
        if (GetCurrentThreadId() != g_gameThread.load(std::memory_order_acquire))
            return {};

        __try
        {
            if (!I::EngineClient || !I::EngineClient->in_game() || !I::GameEntity || !I::GameEntity->Instance)
                return {};

            auto* pawn = I::GetLocalPawn();
            auto* services = pawn ? pawn->m_pWeaponServices() : nullptr;

            if (!services)
                return {};

            const auto handle = services->m_hActiveWeapon();
            auto* weapon = I::GameEntity->Instance->Get<C_CSWeaponBase>(handle);

            if (!weapon || weapon->handle() != handle)
                return {};

            const auto definition = weapon->m_AttributeManager()->m_Item()->m_iItemDefinitionIndex();
            return KnifeModelChanger::IsKnifeDefinition(definition) ? LocalKnife{pawn, services, weapon} : LocalKnife{};
        }
        __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH)
        {
            return {};
        }
    }

    void ReadSnapshot(Snapshot& row, C_CSWeaponBase* weapon, WeaponGraphController* controller, CCSPlayer_WeaponServices* services)
    {
        // Read borrowed objects in their native callback. Save only values and identity keys.
        __try
        {
            if (weapon)
            {
                row.weapon = reinterpret_cast<std::uintptr_t>(weapon);
                row.handle = weapon->handle().value();
                row.definition = weapon->m_AttributeManager()->m_Item()->m_iItemDefinitionIndex();
                row.subclass = weapon->m_nSubclassID();
                row.state = weapon->State();
                row.stateTime = weapon->StateTime();
                row.cancelTime = weapon->CancelTime();
                row.pending = weapon->Pending();
                row.shouldLoop = weapon->ShouldLoop();

                if (services)
                    row.serviceBlocked = services->InspectBlocked();
            }

            if (controller)
            {
                row.controller = reinterpret_cast<std::uintptr_t>(controller);
                const auto* results = reinterpret_cast<const ControllerResults*>(controller);
                row.graph = reinterpret_cast<std::uintptr_t>(results->graph);
                row.cachedState = results->cachedState;
                row.cachedTime = results->cachedTime;
                row.phase = results->phase;
                row.complete = results->complete;
                row.graphBlocked = results->blockInspect;

                if (const auto* node = controller->Action()->Node())
                    row.action = node->value.symbol;

                if (const auto* node = controller->Reset()->Node())
                    row.reset = node->value.boolean;

                if (const auto* node = controller->Extra()->Node())
                    row.extra = node->value.symbol;
            }
        }
        __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH)
        {
            row.readFault = 1;
        }
    }

    bool SameKnife(const Snapshot& a, const Snapshot& b)
    {
        return a.weapon == b.weapon && a.handle == b.handle && a.definition == b.definition && a.subclass == b.subclass;
    }

    bool CanCorrectKnife(C_CSWeaponBase* weapon, const LocalKnife& local)
    {
        if (!g_ready.load(std::memory_order_acquire) || g_stopping.load(std::memory_order_acquire) ||
            GetCurrentThreadId() != g_gameThread.load(std::memory_order_acquire))
            return false;

        __try
        {
            // a demo (IEngineClient vfunc 42 = IsPlayingDemo): the recording plays the knife's animation - never touched
            if (M::CallVFunc<bool, 42U>(I::EngineClient))
                return false;

            // Lootline's changer: the side you're playing, the game thread's copy
            const std::uint16_t knifeDefinition = Config::GameLive().knifeDefinition;

            if (!Config::Game().enabled || knifeDefinition < 500 || !weapon || weapon != local.weapon)
                return false;

            auto* pawn = local.pawn;
            if (!pawn || pawn->m_iHealth() <= 0 || weapon->m_hOwnerEntity() != pawn->handle() ||
                weapon->m_AttributeManager()->m_Item()->m_iItemDefinitionIndex() != knifeDefinition)
                return false;

            const auto pawnHandle = pawn->handle().value();
            const float spawnTime = pawn->m_flLastSpawnTimeIndex();

            if (g_reentry.pawn != reinterpret_cast<std::uintptr_t>(pawn) ||
                g_reentry.pawnHandle != pawnHandle || g_reentry.spawnTime != spawnTime)
            {
                g_reentry = {};
                g_reentry.pawn = reinterpret_cast<std::uintptr_t>(pawn);
                g_reentry.pawnHandle = pawnHandle;
                g_reentry.spawnTime = spawnTime;
            }

            return true;
        }
        __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH)
        {
            return false;
        }
    }

    void ObserveInspect(ReentryState& state, const Snapshot& before, const Snapshot& after, bool accepted)
    {
        state.hasRequest = accepted && !before.readFault && !after.readFault && SameKnife(before, after) &&
            before.state == Inspecting && after.state == Inspecting && before.stateTime == after.stateTime &&
            after.cancelTime == 0 && after.pending == 0 && after.serviceBlocked == 0;
        state.request = after;
    }

    bool SameViewmodel(const ReentryState& state, const Snapshot& before, ViewmodelIdentity viewmodel)
    {
        return state.hasViewmodel && state.viewmodel == viewmodel && SameKnife(state.previousViewmodel, before) &&
            state.previousViewmodel.controller == before.controller && state.previousViewmodel.graph == before.graph;
    }

    bool NeedsStaleResetSuppression(const ReentryState& state, const Snapshot& before, const Snapshot& after, ViewmodelIdentity viewmodel)
    {
        const bool knownInspect = state.hasInspectStart && !state.hasRequest && SameViewmodel(state, before, viewmodel) &&
            SameKnife(before, after) && before.state == Inspecting && after.state == Inspecting &&
            before.stateTime == state.inspectStartTime && after.stateTime == state.inspectStartTime;
        const bool rewoundOutro = before.cachedState == InspectOutro && before.cachedTime > after.stateTime &&
            state.previousViewmodel.cachedState == before.cachedState && state.previousViewmodel.cachedTime == before.cachedTime &&
            after.cachedState == Inspecting && after.cachedTime == after.stateTime;
        const bool nativeRestart = before.reset == ResetClear && after.reset == ResetPulse && before.action != 0 && before.action == after.action &&
            state.previousViewmodel.action == before.action && before.extra == 0 && after.extra == 0;

        // Inspect blocks reject new requests. They do not justify restarting an
        // existing animation when its older gameplay state is restored.
        const bool uninterrupted = before.shouldLoop == 0 && after.shouldLoop == 0 && after.cancelTime == 0 &&
            after.pending == 0 && after.serviceBlocked >= 0 && after.graphBlocked >= 0 &&
            before.phase >= InspectIntroPhase && before.phase <= InspectOutroPhase && after.phase == before.phase;

        return knownInspect && rewoundOutro && nativeRestart && uninterrupted && !before.readFault && !after.readFault &&
            before.graph != 0 && before.graph == after.graph && before.controller == after.controller;
    }

    bool NeedsReentry(ReentryState& state, const Snapshot& before, const Snapshot& after, ViewmodelIdentity viewmodel)
    {
        const bool sameGraph = SameViewmodel(state, before, viewmodel);
        const bool freshLoop = sameGraph && state.previousViewmodel.shouldLoop == 0 && after.shouldLoop == 1;
        const bool matchingRequest = state.hasRequest && SameKnife(state.request, after) && state.request.stateTime == after.stateTime;
        const bool unchangedInspect = SameKnife(before, after) && before.state == Inspecting && after.state == Inspecting &&
            before.stateTime == after.stateTime && before.cachedState == Inspecting &&
            before.cachedTime == before.stateTime && after.cachedState == Inspecting && after.cachedTime == after.stateTime;

        // Viewmodel completion can precede gameplay idle. A fresh inspect request still
        // needs the native re-entry transition while the old inspect state persists.
        const bool unblockedOutro = before.phase == InspectOutroPhase && after.phase == InspectOutroPhase && after.complete >= 0 && after.graphBlocked == 0 &&
            after.cancelTime == 0 && after.pending == 0 && after.serviceBlocked == 0;
        const bool missingReset = before.reset == ResetClear && after.reset == ResetClear && before.action != 0 && before.action == after.action && after.extra != 0;

        const bool correct = freshLoop && matchingRequest && unchangedInspect && unblockedOutro && missingReset &&
            !before.readFault && !after.readFault && before.graph != 0 &&
            before.graph == after.graph && before.controller == after.controller;

        // Consume at the next viewmodel reflection, including a rejected request. A press
        // during the middle must not become a delayed restart when the outro begins.
        state.hasRequest = false;

        if (!sameGraph || after.readFault || after.graph == 0 || (after.state != Inspecting && after.state != InspectOutro) ||
            after.cancelTime != 0 || after.pending != 0)
            state.hasInspectStart = false;

        if (!after.readFault && after.graph != 0 && after.state == Inspecting && after.cachedState == Inspecting &&
            after.cachedTime == after.stateTime && after.action != 0 && after.cancelTime == 0 && after.pending == 0 &&
            (!state.hasInspectStart || after.stateTime >= state.inspectStartTime))
        {
            state.inspectStartTime = after.stateTime;
            state.hasInspectStart = true;
        }

        state.hasViewmodel = !after.readFault && after.graph != 0;
        state.viewmodel = viewmodel;
        state.previousViewmodel = after;

        return correct;
    }

    bool WriteReentry(WeaponGraphController* controller)
    {
        __try
        {
            auto* reset = controller->Reset()->Node();
            auto* extra = controller->Extra()->Node();

            if (!reset || !extra || reset->value.boolean || !extra->value.symbol)
                return false;

            // Match the native inspect handler's re-entry inputs. Only the viewmodel
            // controller is supplied here. Weapon state and variant selection stay native.
            extra->value.symbol = 0;
            reset->value.boolean = true;
            return true;
        }
        __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH)
        {
            return false;
        }
    }

    bool ClearStaleReset(WeaponGraphController* controller, const Snapshot& expected)
    {
        __try
        {
            const auto* results = reinterpret_cast<const ControllerResults*>(controller);
            auto* reset = controller->Reset()->Node();
            const auto* action = controller->Action()->Node();
            const auto* extra = controller->Extra()->Node();

            if (expected.reset != ResetPulse || expected.extra != 0 || !reset || !reset->value.boolean || !action || !extra ||
                results->graph != reinterpret_cast<void*>(expected.graph) || results->cachedState != expected.cachedState ||
                results->cachedTime != expected.cachedTime || action->value.symbol != expected.action || extra->value.symbol != 0)
                return false;

            // Reflection treats an older restored inspect state as a new action.
            // Suppress only its reset pulse. Keep native cache and weapon state intact.
            reset->value.boolean = false;
            return true;
        }
        __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH)
        {
            return false;
        }
    }

    ViewmodelIdentity LocalViewmodel(WeaponGraphController* controller, C_CSPlayerPawn* pawn)
    {
        if (!controller || GetCurrentThreadId() != g_gameThread.load(std::memory_order_acquire))
            return {};

        __try
        {
            if (!pawn || !I::GameEntity || !I::GameEntity->Instance)
                return {};

            const auto handle = pawn->m_hHudModelArms();
            auto* arms = I::GameEntity->Instance->Get<C_BaseAnimGraph>(handle);
            // Native viewmodel update reflects into this exact main controller.
            // Validate the serial and owner before accepting the borrowed entity.
            if (!arms || arms->handle() != handle || arms->m_hOwnerEntity() != pawn->handle() ||
                arms->MainController() != controller)
                return {};

            return {arms, handle.value()};
        }
        __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH)
        {
            return {};
        }
    }

    bool PrepareReflection(ReentryState& state, const Snapshot& before)
    {
        // Native forced resynchronization resets the controller, reflects the
        // weapon, then restores a graph snapshot. Do not seed history from that
        // intermediate reflection. The next ordinary reflection starts it anew.
        const bool reset = before.cachedState == 0 && before.cachedTime == 0 && before.action == 0 &&
            before.reset == ResetClear && before.phase == 0;
        if (before.readFault || before.graph == 0 || reset)
        {
            state = {};
            return false;
        }

        return true;
    }

}

void KnifeAnimation::Initialize()
{
    g_stopping.store(false, std::memory_order_release);
    g_ready.store(I::EngineClient && I::GameEntity, std::memory_order_release);
}

KnifeAnimation::InspectContext KnifeAnimation::BeforeInspect(C_CSWeaponBase* weapon)
{
    const auto local = ResolveLocalKnife();
    InspectContext context;
    context.correct = CanCorrectKnife(weapon, local);

    if (context.correct)
        ReadSnapshot(context.before, weapon, nullptr, local.services);

    return context;
}

void KnifeAnimation::AfterInspect(C_CSWeaponBase* weapon, const InspectContext& context, char result)
{
    if (!context.correct)
        return;

    const auto local = ResolveLocalKnife();
    Snapshot after;
    ReadSnapshot(after, weapon, nullptr, local.services);
    ObserveInspect(g_reentry, context.before, after, result != 0);
}

KnifeAnimation::ReflectionContext KnifeAnimation::BeforeReflection(WeaponGraphController* controller, C_CSWeaponBase* weapon)
{
    const auto local = ResolveLocalKnife();
    ReflectionContext context;

    if (CanCorrectKnife(weapon, local))
        context.viewmodel = LocalViewmodel(controller, local.pawn);

    if (context.viewmodel.entity)
    {
        ReadSnapshot(context.before, weapon, controller, local.services);
        context.correct = PrepareReflection(g_reentry, context.before);
    }

    return context;
}

void KnifeAnimation::AfterReflection(WeaponGraphController* controller, C_CSWeaponBase* weapon, const ReflectionContext& context)
{
    if (!context.correct || g_stopping.load(std::memory_order_acquire))
        return;

    const auto local = ResolveLocalKnife();
    Snapshot after;
    ReadSnapshot(after, weapon, controller, local.services);

    const bool staleReset = NeedsStaleResetSuppression(g_reentry, context.before, after, context.viewmodel);
    const bool reentry = NeedsReentry(g_reentry, context.before, after, context.viewmodel);

    if (staleReset)
        ClearStaleReset(controller, after);
    else if (reentry)
        WriteReentry(controller);
}

void KnifeAnimation::FrameStage(int stage)
{
    if (stage == FRAME_RENDER_END || stage == FRAME_NET_UPDATE_END)
        g_gameThread.store(GetCurrentThreadId(), std::memory_order_release);

    if (stage != FRAME_RENDER_END && stage != FRAME_NET_UPDATE_END)
        return;

    if (!g_ready.load(std::memory_order_acquire) || g_stopping.load(std::memory_order_acquire))
        return;

    const auto local = ResolveLocalKnife();
    auto* weapon = local.weapon;

    if (!CanCorrectKnife(weapon, local))
        g_reentry = {};
    else if (g_reentry.hasViewmodel)
    {
        const auto* previousWeapon = reinterpret_cast<C_CSWeaponBase*>(g_reentry.previousViewmodel.weapon);
        auto* previousController = reinterpret_cast<WeaponGraphController*>(g_reentry.previousViewmodel.controller);

        if (previousWeapon != weapon || LocalViewmodel(previousController, local.pawn) != g_reentry.viewmodel)
            g_reentry = {};
    }
}

void KnifeAnimation::Shutdown()
{
    g_stopping.store(true, std::memory_order_release);
    g_ready.store(false, std::memory_order_release);
}
