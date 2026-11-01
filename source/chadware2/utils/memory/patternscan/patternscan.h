#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>

namespace M
{
    // Keep lookup caches independent of the CRT's per-thread static-init epoch.
    // Each resolver type owns a constant-initialized cache. Concurrent first
    // calls may repeat a read-only lookup; a null result remains retryable.
    template <typename Resolver>
    auto ResolveCached(Resolver resolver)
    {
        using Pointer = decltype(resolver());
        static constinit std::atomic<Pointer> cached{ nullptr };

        Pointer value = cached.load(std::memory_order_acquire);
        if (value)
            return value;

        value = resolver();
        if (value)
        {
            Pointer expected = nullptr;
            if (!cached.compare_exchange_strong(expected, value,
                std::memory_order_acq_rel, std::memory_order_acquire))
                value = expected;
        }

        return value;
    }

    std::uint8_t* FindPattern(const char* moduleName, const std::string& byteSequence);
    // A function one of our hooks starts with a jump now (custom colours hook BuildModernSkin, which the skin changer
    // and the lineups scan for later): its signature can't match live memory any more, so the address found before
    // hooking is answered for that built-in signature from then on.
    void RememberHooked(const char* builtInSequence, std::uint8_t* address);
    std::uint8_t* scan(const char* moduleName, const char* pattern);
    std::uint8_t* scan_absolute(const char* moduleName, const char* pattern, int preOffset = 0, int postOffset = 0);

    template <typename T = std::uint8_t>
    T* GetAbsoluteAddress(T* relativeAddress, int preOffset = 0, int postOffset = 0)
    {
        if (!relativeAddress)
            return nullptr;

        relativeAddress += preOffset;
        relativeAddress += sizeof(std::int32_t) + *reinterpret_cast<std::int32_t*>(relativeAddress);
        relativeAddress += postOffset;
        return relativeAddress;
    }
}
