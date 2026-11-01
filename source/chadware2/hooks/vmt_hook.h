#pragma once

#include <Windows.h>

#include <cstddef>
#include <cstdint>

// One vtable slot swapped for a detour. The vtable belongs to the class (shared by all its objects, in the module's
// read-only data); the function's code isn't touched. So another tool can inline-hook the same function — Chadware
// does, for FrameStageNotify and the mouse functions — and both keep working, whichever of them unloads first.
class VmtSlotHook
{
public:
    // Index of the slot that holds `function` among the first `count` entries, or -1.
    static int FindSlot(void** vtable, const void* function, int count)
    {
        if (!vtable || !function)
            return -1;

        __try
        {
            for (int i = 0; i < count; ++i)
            {
                if (vtable[i] == function)
                    return i;
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
        }

        return -1;
    }

    bool Install(void** vtable, int index, void* detour)
    {
        if (m_slot || !vtable || index < 0 || !detour)
            return false;

        void** slot = vtable + index;
        DWORD oldProtect = 0;

        if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &oldProtect))
            return false;

        m_original = *slot;
        InterlockedExchangePointer(slot, detour);
        VirtualProtect(slot, sizeof(void*), oldProtect, &oldProtect);

        m_slot = slot;
        m_detour = detour;
        return true;
    }

    // Puts the original back — only if the slot still holds our detour (if something swapped it after us, leave
    // it: taking ours out from under it would break that tool).
    void Restore()
    {
        if (!m_slot)
            return;

        DWORD oldProtect = 0;

        if (VirtualProtect(m_slot, sizeof(void*), PAGE_READWRITE, &oldProtect))
        {
            InterlockedCompareExchangePointer(m_slot, m_original, m_detour);
            VirtualProtect(m_slot, sizeof(void*), oldProtect, &oldProtect);
        }

        m_slot = nullptr;
    }

    template <typename T>
    [[nodiscard]] T Original() const
    {
        return reinterpret_cast<T>(m_original);
    }

    [[nodiscard]] bool Installed() const
    {
        return m_slot != nullptr;
    }

private:
    void** m_slot = nullptr;
    void* m_original = nullptr;
    void* m_detour = nullptr;
};
