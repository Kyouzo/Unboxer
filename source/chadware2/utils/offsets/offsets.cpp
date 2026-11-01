#include "offsets.h"

#include <Windows.h>
#include <atomic>
#include <cctype>
#include <cstring>
#include <mutex>
#include <sstream>
#include <unordered_map>

#include "offsets_resource.h"
#include "../../../patterns.h"
#include "../memory/patternscan/patternscan.h"
#include "../../../../external/json/json.hpp"

namespace
{
    struct Dump
    {
        std::unordered_map<std::string, std::uintptr_t> client;     // offsets.json  "client.dll"
        std::unordered_map<std::string, std::uintptr_t> engine2;    // offsets.json  "engine2.dll"
        std::unordered_map<std::string, std::uintptr_t> buttons;    // buttons.json  "client.dll"
        int build = 0;                                              // info.json     "build_number"
        std::unordered_map<std::string, std::string> overrides;     // patterns.txt  name -> signature
        std::vector<std::string> unknown;                           // patterns.txt  names that aren't ours
    };

    std::once_flag g_LoadOnce;
    Dump g_Dump;                            // filled once, read-only afterwards
    std::atomic_int g_GameBuild{ 0 };

    std::mutex g_CheckMutex;
    std::vector<Offsets::SignatureResult> g_Results;
    std::atomic_bool g_CheckRunning{ false };
    HANDLE g_CheckThread = nullptr;

    bool SafeRead(const void* address, void* out, std::size_t size)
    {
        __try
        {
            std::memcpy(out, address, size);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    HMODULE SelfModule()
    {
        HMODULE self = nullptr;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(&SelfModule), &self);
        return self;
    }

    // one of the files from Chadware\offsets\, compiled into this DLL by offsets.rc
    std::string EmbeddedFile(int id)
    {
        const HMODULE self = SelfModule();
        const HRSRC info = self ? FindResourceW(self, MAKEINTRESOURCEW(id), MAKEINTRESOURCEW(10) /* RT_RCDATA */) : nullptr;
        const HGLOBAL data = info ? LoadResource(self, info) : nullptr;
        const char* bytes = data ? static_cast<const char*>(LockResource(data)) : nullptr;
        const DWORD size = info ? SizeofResource(self, info) : 0;
        return bytes && size ? std::string(bytes, size) : std::string();
    }

    // "client.dll": { "dwCSGOInput": 39262864, ... }
    void ReadModule(const nlohmann::json& root, const char* module, std::unordered_map<std::string, std::uintptr_t>& out)
    {
        const auto it = root.find(module);
        if (it == root.end() || !it->is_object())
            return;

        for (const auto& [name, value] : it->items())
        {
            if (value.is_number_unsigned() || value.is_number_integer())
                out[name] = value.get<std::uintptr_t>();
        }
    }

    nlohmann::json ParseJson(const std::string& text)
    {
        nlohmann::json root = nlohmann::json::parse(text, nullptr, false);
        return root.is_object() ? root : nlohmann::json::object();
    }

    std::string Trim(const std::string& text)
    {
        std::size_t begin = 0, end = text.size();
        while (begin < end && std::isspace(static_cast<unsigned char>(text[begin])))
            ++begin;
        while (end > begin && std::isspace(static_cast<unsigned char>(text[end - 1])))
            --end;
        return text.substr(begin, end - begin);
    }

    bool IsSignatureName(const std::string& name)
    {
        for (const Patterns::Entry& entry : Patterns::All)
        {
            if (name == entry.name)
                return true;
        }
        return false;
    }

    // Name = 48 89 5C 24 ? ...
    // A line pasted straight from a patterns header works too:  inline constexpr const char* Name = "48 89 ...";
    void ReadPatterns(const std::string& text, Dump& dump)
    {
        std::istringstream in(text);
        std::string line;

        while (std::getline(in, line))
        {
            line = Trim(line);
            if (line.empty() || line[0] == '#' || line.rfind("//", 0) == 0)
                continue;

            const std::size_t equals = line.find('=');
            if (equals == std::string::npos)
                continue;

            // the name is the word right before '='
            std::size_t end = equals;
            while (end > 0 && std::isspace(static_cast<unsigned char>(line[end - 1])))
                --end;
            std::size_t begin = end;
            while (begin > 0 && (std::isalnum(static_cast<unsigned char>(line[begin - 1])) || line[begin - 1] == '_'))
                --begin;
            const std::string name = line.substr(begin, end - begin);

            std::string signature = line.substr(equals + 1);
            if (const std::size_t open = signature.find('"'); open != std::string::npos)
            {
                const std::size_t close = signature.find('"', open + 1);
                signature = signature.substr(open + 1, close == std::string::npos ? std::string::npos : close - open - 1);
            }
            signature = Trim(signature);
            while (!signature.empty() && (signature.back() == ';' || signature.back() == ','))
                signature.pop_back();
            signature = Trim(signature);

            if (name.empty() || signature.empty())
                continue;

            if (IsSignatureName(name))
                dump.overrides[name] = signature;
            else
                dump.unknown.push_back(name);
        }
    }

    void Load()
    {
        const nlohmann::json offsets = ParseJson(EmbeddedFile(IDR_OFFSETS_JSON));
        ReadModule(offsets, "client.dll", g_Dump.client);
        ReadModule(offsets, "engine2.dll", g_Dump.engine2);

        ReadModule(ParseJson(EmbeddedFile(IDR_BUTTONS_JSON)), "client.dll", g_Dump.buttons);

        const nlohmann::json info = ParseJson(EmbeddedFile(IDR_INFO_JSON));
        if (const auto it = info.find("build_number"); it != info.end() && it->is_number_integer())
            g_Dump.build = it->get<int>();

        ReadPatterns(EmbeddedFile(IDR_PATTERNS_TXT), g_Dump);
    }

    const Dump& Get()
    {
        std::call_once(g_LoadOnce, Load);
        return g_Dump;
    }

    std::uintptr_t FreshOffset(const std::unordered_map<std::string, std::uintptr_t>& table, const char* module, const char* name)
    {
        if (!Offsets::DumpIsFresh())
            return 0;

        const auto it = table.find(name);
        const HMODULE base = GetModuleHandleA(module);
        return it != table.end() && base ? reinterpret_cast<std::uintptr_t>(base) + it->second : 0;
    }

    std::mutex g_CoveredMutex;
    std::vector<std::string> g_Covered;   // Offsets::MarkCovered: found another way

    // broken, but the fresh dump (or a fallback, MarkCovered) covers what it was used for
    bool DumpCovers(const char* signature)
    {
        {
            std::lock_guard<std::mutex> lock(g_CoveredMutex);
            for (const std::string& covered : g_Covered)
                if (covered == signature)
                    return true;
        }

        if (!std::strcmp(signature, "InputPointer"))
            return Offsets::FreshClient("dwCSGOInput") != 0;
        if (!std::strcmp(signature, "GetLocalPawn"))
            return Offsets::FreshClient("dwLocalPlayerPawn") != 0;
        if (!std::strcmp(signature, "ButtonRegistration"))
            return Offsets::FreshButton("jump") != 0;
        return false;
    }

    // Chadware hooks some of these functions itself (FrameStageNotify, the viewmodel reader, the preview helpers):
    // the hook overwrites the first 5 bytes with a jump, so the full signature can't match live memory any more.
    // Look again with those 5 bytes as wildcards and accept it only if a hook jump really sits there.
    bool FoundButHooked(const Patterns::Entry& entry)
    {
        const std::string* custom = Offsets::PatternOverride(entry.pattern);
        std::istringstream in(custom ? *custom : std::string(entry.pattern));
        std::string token, relaxed;
        for (int i = 0; in >> token; ++i)
            relaxed += (i < 5 ? std::string("?") : token) + " ";

        const std::uint8_t* match = M::FindPattern(entry.module, relaxed);
        std::uint8_t start[2]{};
        if (!match || !SafeRead(match, start, sizeof(start)))
            return false;

        return start[0] == 0xE9 || (start[0] == 0xFF && start[1] == 0x25);   // jmp rel32 / jmp [rip]
    }

    DWORD WINAPI CheckSignatures(LPVOID)
    {
        const Dump& dump = Get();
        std::vector<Offsets::SignatureResult> results;

        for (const Patterns::Entry& entry : Patterns::All)
        {
            Offsets::SignatureResult result;
            result.name = entry.name;
            result.module = entry.module;
            result.overridden = dump.overrides.count(entry.name) != 0;
            result.found = M::FindPattern(entry.module, entry.pattern) != nullptr   // patterns.txt applies inside
                || FoundButHooked(entry);
            result.rescued = !result.found && DumpCovers(entry.name);
            results.push_back(result);
        }

        {
            std::lock_guard<std::mutex> lock(g_CheckMutex);
            g_Results = std::move(results);
        }

        g_CheckRunning.store(false, std::memory_order_release);
        return 0;
    }
}

int Offsets::DumpBuild()
{
    return Get().build;
}

int Offsets::GameBuild()
{
    if (const int cached = g_GameBuild.load(); cached > 0)
        return cached;

    const Dump& dump = Get();
    const auto it = dump.engine2.find("dwBuildNumber");
    const HMODULE engine2 = GetModuleHandleA("engine2.dll");
    if (it == dump.engine2.end() || !engine2)
        return 0;

    // A stale dump points dwBuildNumber at something else: anything that doesn't look like a build number = unknown.
    int build = 0;
    if (!SafeRead(reinterpret_cast<const std::uint8_t*>(engine2) + it->second, &build, sizeof(build)) || build < 10000 || build > 99999)
        return 0;

    g_GameBuild.store(build);
    return build;
}

// The dump matches the running game when the game's own jump button sits exactly where buttons.json says it does
// (the button's name pointer reads "jump"). Build numbers alone can't be trusted: some dumps ship a stale info.json
// (the 14184 GitHub dump still said 14183).
bool Offsets::DumpIsFresh()
{
    static constinit std::atomic_int verdict{ 0 };     // 0 = not checked yet, 1 = matches, 2 = doesn't
    if (const int known = verdict.load(); known != 0)
        return known == 1;

    const Dump& dump = Get();
    const auto jump = dump.buttons.find("jump");
    const HMODULE client = GetModuleHandleA("client.dll");
    if (jump == dump.buttons.end() || !client)
    {
        if (client)
            verdict.store(2);
        return false;
    }

    // input button layout: name pointer at +0x8, state (= buttons.jump) at +0x30
    const std::uint8_t* button = reinterpret_cast<const std::uint8_t*>(client) + jump->second - 0x30;
    const char* name = nullptr;
    char text[5]{};
    const bool matches = SafeRead(button + 0x8, &name, sizeof(name)) && name &&
        SafeRead(name, text, sizeof(text)) && std::memcmp(text, "jump", sizeof(text)) == 0;

    verdict.store(matches ? 1 : 2);
    return matches;
}

std::uintptr_t Offsets::FreshClient(const char* name)
{
    return FreshOffset(Get().client, "client.dll", name);
}

std::uintptr_t Offsets::FreshButton(const char* name)
{
    return FreshOffset(Get().buttons, "client.dll", name);
}

const std::string* Offsets::PatternOverride(const std::string& builtIn)
{
    const Dump& dump = Get();
    if (dump.overrides.empty())
        return nullptr;

    for (const Patterns::Entry& entry : Patterns::All)
    {
        if (builtIn == entry.pattern)
        {
            const auto it = dump.overrides.find(entry.name);
            return it != dump.overrides.end() ? &it->second : nullptr;
        }
    }

    return nullptr;
}

int Offsets::OverrideCount()
{
    return static_cast<int>(Get().overrides.size());
}

std::vector<std::string> Offsets::UnknownOverrideNames()
{
    return Get().unknown;
}

void Offsets::StartSignatureCheck()
{
    if (g_CheckRunning.exchange(true))
        return;

    if (g_CheckThread)
    {
        CloseHandle(g_CheckThread);
        g_CheckThread = nullptr;
    }

    g_CheckThread = CreateThread(nullptr, 0, CheckSignatures, nullptr, 0, nullptr);
    if (!g_CheckThread)
        g_CheckRunning.store(false);
}

void Offsets::MarkCovered(const char* signature)
{
    if (!signature)
        return;

    std::lock_guard<std::mutex> lock(g_CoveredMutex);
    g_Covered.emplace_back(signature);
}

bool Offsets::SignatureCheckRunning()
{
    return g_CheckRunning.load(std::memory_order_acquire);
}

std::vector<Offsets::SignatureResult> Offsets::SignatureResults()
{
    std::lock_guard<std::mutex> lock(g_CheckMutex);
    return g_Results;
}

void Offsets::Shutdown()
{
    if (!g_CheckThread)
        return;

    WaitForSingleObject(g_CheckThread, 20000);
    CloseHandle(g_CheckThread);
    g_CheckThread = nullptr;
}
