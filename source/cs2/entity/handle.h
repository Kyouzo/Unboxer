#pragma once

#include <cstdint>

inline constexpr std::uint32_t INVALID_EHANDLE_INDEX = 0xFFFFFFFFu;
inline constexpr std::uint32_t ENT_ENTRY_MASK = 0x7FFFu;

class CBaseHandle
{
public:
    CBaseHandle() noexcept = default;

    CBaseHandle(int entry, int serial) noexcept : nIndex(static_cast<std::uint32_t>(entry) | (static_cast<std::uint32_t>(serial) << 15)) {}

    [[nodiscard]] std::uint32_t value() const noexcept
    {
        return nIndex;
    }

    bool operator==(const CBaseHandle& other) const noexcept
    {
        return nIndex == other.nIndex;
    }

    [[nodiscard]] bool valid() const noexcept
    {
        return nIndex != INVALID_EHANDLE_INDEX;
    }

    [[nodiscard]] int index() const noexcept
    {
        return static_cast<int>(nIndex & ENT_ENTRY_MASK);
    }

private:
    std::uint32_t nIndex = INVALID_EHANDLE_INDEX;
};
