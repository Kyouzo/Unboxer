#pragma once

#include <atomic>

#include "../../../external/kiero/minhook/include/MinHook.h"

namespace H
{
    // Game threads currently inside one of our detours. Disabling a hook doesn't stop a thread that is already in
    // it (FrameStageNotify spends a whole frame stage inside the original) — freeing the DLL then made it return
    // into unloaded code. Every detour holds a HookCallGuard; unload waits for zero before tearing anything down.
    inline std::atomic_int g_ActiveHookCalls{ 0 };

    struct HookCallGuard
    {
        HookCallGuard() { g_ActiveHookCalls.fetch_add(1, std::memory_order_acq_rel); }
        ~HookCallGuard() { g_ActiveHookCalls.fetch_sub(1, std::memory_order_acq_rel); }
        HookCallGuard(const HookCallGuard&) = delete;
        HookCallGuard& operator=(const HookCallGuard&) = delete;
    };

    // After the hooks are disabled: wait until no thread has been inside one for a while (or give up after timeoutMs).
    inline bool WaitForHookCalls(unsigned timeoutMs)
    {
        int quietMs = 0;

        for (unsigned waited = 0; waited < timeoutMs; ++waited)
        {
            quietMs = g_ActiveHookCalls.load(std::memory_order_acquire) == 0 ? quietMs + 1 : 0;

            if (quietMs >= 50)
                return true;

            Sleep(1);
        }

        return false;
    }
}

template <typename T>
class CInlineHookObj
{
public:
    bool Add(void* function, void* detour)
    {
        if (!function || !detour)
            return false;

        m_Function = function;
        if (MH_CreateHook(function, detour, &m_Original) != MH_OK)
            return false;

        return Replace();
    }

    bool Replace()
    {
        if (!m_Function || m_Hooked)
            return false;

        if (MH_EnableHook(m_Function) != MH_OK)
            return false;

        m_Hooked = true;
        return true;
    }

    bool Remove()
    {
        if (!m_Function)
            return false;

        if (m_Hooked && !Restore())
            return false;

        if (MH_RemoveHook(m_Function) != MH_OK)
            return false;

        m_Function = nullptr;
        m_Original = nullptr;

        return true;
    }

    bool Restore()
    {
        if (!m_Hooked)
            return false;

        if (MH_DisableHook(m_Function) != MH_OK)
            return false;

        m_Hooked = false;
        return true;
    }

    [[nodiscard]] T GetOriginal() const
    {
        return reinterpret_cast<T>(m_Original);
    }

    [[nodiscard]] bool IsHooked() const
    {
        return m_Hooked;
    }

private:
    bool m_Hooked = false;
    void* m_Function = nullptr;
    void* m_Original = nullptr;
};
