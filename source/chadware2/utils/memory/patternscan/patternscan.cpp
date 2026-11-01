#include "patternscan.h"

#include <Windows.h>
#include <algorithm>
#include <array>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "../../offsets/offsets.h"

namespace
{
    // How often each byte value occurs in a module's code, counted once per module. A scan then jumps (memchr) from
    // one occurrence of the pattern's rarest byte to the next instead of comparing at every address - the same first
    // match, a lot faster (most signatures have a byte that's 50-500x rarer than 48 / 8B / 89).
    struct CodeStats
    {
        const std::uint8_t* begin = nullptr;
        const std::uint8_t* end = nullptr;
        std::array<std::uint32_t, 256> counts{};
    };

    std::mutex g_statsMutex;
    std::vector<CodeStats> g_stats;   // a handful of modules: client.dll, panorama.dll, ...

    // RememberHooked: built-in signature -> the function's address from before it was hooked
    std::mutex g_hookedMutex;
    std::vector<std::pair<std::string, std::uint8_t*>> g_hooked;

    // the counts for [begin, end), counted the first time (g_statsMutex held)
    const std::array<std::uint32_t, 256>& CountsLocked(const std::uint8_t* begin, const std::uint8_t* end)
    {
        for (const CodeStats& stats : g_stats)
            if (stats.begin == begin && stats.end == end)
                return stats.counts;

        CodeStats stats;
        stats.begin = begin;
        stats.end = end;

        for (const std::uint8_t* p = begin; p < end; ++p)
            ++stats.counts[*p];

        g_stats.push_back(stats);
        return g_stats.back().counts;
    }
}

void M::RememberHooked(const char* builtInSequence, std::uint8_t* address)
{
    if (!builtInSequence || !address)
        return;

    std::lock_guard lock(g_hookedMutex);

    for (auto& [pattern, known] : g_hooked)
    {
        if (pattern == builtInSequence)
        {
            known = address;
            return;
        }
    }

    g_hooked.emplace_back(builtInSequence, address);
}

std::uint8_t* M::FindPattern(const char* moduleName, const std::string& builtInSequence)
{
    {
        std::lock_guard lock(g_hookedMutex);

        for (const auto& [pattern, address] : g_hooked)
            if (pattern == builtInSequence)
                return address;
    }

    // offsets\patterns.txt can replace any built-in signature after a CS2 update (see offsets\README.txt)
    const std::string* override = Offsets::PatternOverride(builtInSequence);
    const std::string& byteSequence = override ? *override : builtInSequence;

    const HMODULE module = GetModuleHandleA(moduleName);
    if (!module)
        return nullptr;

    const auto dos = reinterpret_cast<PIMAGE_DOS_HEADER>(module);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE)
        return nullptr;

    const auto nt = reinterpret_cast<PIMAGE_NT_HEADERS>(reinterpret_cast<std::uint8_t*>(module) + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE)
        return nullptr;

    auto* codeBegin = reinterpret_cast<std::uint8_t*>(module) + nt->OptionalHeader.BaseOfCode;
    auto* codeEnd = codeBegin + nt->OptionalHeader.SizeOfCode;

    using PatternByte = std::pair<std::uint8_t, bool>;
    std::vector<PatternByte> sequence;
    std::stringstream stream(byteSequence);
    std::string token;

    while (stream >> token)
    {
        if (!token.empty() && token[0] == '?')
        {
            sequence.emplace_back(0, true);
            continue;
        }

        if (token.size() < 2 || !std::isxdigit(static_cast<unsigned char>(token[0])) || !std::isxdigit(static_cast<unsigned char>(token[1])))
            continue;

        sequence.emplace_back(static_cast<std::uint8_t>(std::strtoul(token.c_str(), nullptr, 16)), false);
    }

    const std::size_t length = sequence.size();

    if (length == 0 || static_cast<std::size_t>(codeEnd - codeBegin) < length)
        return nullptr;

    // the anchor: the pattern's rarest fixed byte in this module's code
    std::size_t anchor = length;
    {
        std::lock_guard lock(g_statsMutex);
        const auto& counts = CountsLocked(codeBegin, codeEnd);

        for (std::size_t i = 0; i < length; ++i)
        {
            if (!sequence[i].second && (anchor == length || counts[sequence[i].first] < counts[sequence[anchor].first]))
                anchor = i;
        }
    }

    if (anchor == length)   // only wildcards: matches right away (what std::search did)
        return codeBegin;

    const std::uint8_t anchorByte = sequence[anchor].first;
    const std::uint8_t* last = codeEnd - length;   // the last address a match can start at

    // candidates in address order, so the first one that matches is the first match
    for (const std::uint8_t* start = codeBegin; start <= last;)
    {
        const auto* hit = static_cast<const std::uint8_t*>(std::memchr(start + anchor, anchorByte, static_cast<std::size_t>(last - start) + 1));

        if (!hit)
            break;

        const std::uint8_t* candidate = hit - anchor;
        bool match = true;

        for (std::size_t i = 0; i < length; ++i)
        {
            if (!sequence[i].second && candidate[i] != sequence[i].first)
            {
                match = false;
                break;
            }
        }

        if (match)
            return const_cast<std::uint8_t*>(candidate);

        start = candidate + 1;
    }

    return nullptr;
}

std::uint8_t* M::scan(const char* moduleName, const char* pattern)
{
    return FindPattern(moduleName, pattern ? pattern : "");
}

std::uint8_t* M::scan_absolute(const char* moduleName, const char* pattern, int preOffset, int postOffset)
{
    return GetAbsoluteAddress(FindPattern(moduleName, pattern ? pattern : ""), preOffset, postOffset);
}
