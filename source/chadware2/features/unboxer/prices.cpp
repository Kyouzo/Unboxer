#include "prices.h"
#include "prices_resource.h"

#include <Windows.h>
#include <ShlObj.h>
#include <shellapi.h>

#include <atomic>
#include <cctype>
#include <charconv>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string_view>
#include <unordered_map>

#include "unboxer.h"
#include "../../config/configmanager.h"
#include "../../../../external/json/json.hpp"

namespace
{
    using PriceMap = std::unordered_map<std::string, double>;

    // A CSFloat price list (https://csfloat.com/api/v1/listings/price-list) saved by hand in the data folder.
    // Used when it's newer than the built-in list.
    constexpr const char* OVERRIDE_FILE = "prices_csfloat.json";
    constexpr std::uintmax_t MAX_FILE_SIZE = 64u * 1024u * 1024u;

    std::mutex g_mutex;                                   // g_prices, g_info
    PriceMap g_prices;
    Prices::Info g_info;

    std::atomic_bool g_ready{ false };
    std::atomic_int g_generation{ 0 };

    // Doppler / Gamma Doppler phases by paint kit (items_game's am_*_marbleized / am_*doppler_phase*), as csgotrader names
    // them (lowercase). share: what the phase costs next to the cheapest phase - the median over every knife / wear with
    // all phases listed (csgotrader's CSFloat list, 2026-10-28); only used when the list has no price of the phase's own.
    struct Phase
    {
        int paintKit;
        const char* name;
        double share;
    };

    constexpr Phase PHASES[] = {
        { 415, "ruby", 3.2 }, { 416, "sapphire", 2.1 }, { 417, "black pearl", 3.9 },
        { 418, "phase 1", 1.03 }, { 419, "phase 2", 1.2 }, { 420, "phase 3", 1.0 }, { 421, "phase 4", 1.06 },
        { 617, "black pearl", 3.9 }, { 618, "phase 2", 1.2 }, { 619, "sapphire", 2.1 },
        { 852, "phase 1", 1.03 }, { 853, "phase 2", 1.2 }, { 854, "phase 3", 1.0 }, { 855, "phase 4", 1.06 },
        { 568, "emerald", 3.3 }, { 569, "phase 1", 1.02 }, { 570, "phase 2", 1.14 }, { 571, "phase 3", 1.03 }, { 572, "phase 4", 1.01 },
        { 1119, "emerald", 3.3 }, { 1120, "phase 1", 1.02 }, { 1121, "phase 2", 1.16 }, { 1122, "phase 3", 1.02 }, { 1123, "phase 4", 1.0 },
    };

    const Phase* PhaseOf(int paintKit)
    {
        for (const Phase& phase : PHASES)
            if (phase.paintKit == paintKit)
                return &phase;

        return nullptr;
    }

    std::filesystem::path DataPath(const char* file)
    {
        return internal_config::ConfigManager::GetConfigDirectory().parent_path() / file;
    }

    HMODULE SelfModule()
    {
        HMODULE self = nullptr;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(&SelfModule), &self);
        return self;
    }

    // data\prices.txt, compiled into this DLL by prices.rc
    std::string_view BuiltInText()
    {
        const HMODULE self = SelfModule();
        const HRSRC info = self ? FindResourceW(self, MAKEINTRESOURCEW(IDR_PRICES_TXT), MAKEINTRESOURCEW(10) /* RT_RCDATA */) : nullptr;
        const HGLOBAL data = info ? LoadResource(self, info) : nullptr;
        const char* bytes = data ? static_cast<const char*>(LockResource(data)) : nullptr;
        const DWORD size = info ? SizeofResource(self, info) : 0;
        return bytes && size ? std::string_view(bytes, size) : std::string_view();
    }

    // "LOOTLINE-PRICES 1 <unix time> <yyyy-mm-dd>", then one "<market name, lowercase>\t<cents>" per line.
    bool ParseBuiltIn(std::string_view text, PriceMap& out, long long& madeAt, std::string& date)
    {
        constexpr std::string_view magic = "LOOTLINE-PRICES 1 ";
        const std::size_t headerEnd = text.find('\n');

        if (headerEnd == std::string_view::npos || !text.starts_with(magic))
            return false;

        std::string_view header = text.substr(magic.size(), headerEnd - magic.size());
        if (!header.empty() && header.back() == '\r')
            header.remove_suffix(1);

        const std::size_t space = header.find(' ');
        if (space == std::string_view::npos || std::from_chars(header.data(), header.data() + space, madeAt).ec != std::errc{})
            return false;

        date = std::string(header.substr(space + 1));
        out.reserve(32768);

        for (std::size_t at = headerEnd + 1; at < text.size();)
        {
            std::size_t end = text.find('\n', at);
            if (end == std::string_view::npos)
                end = text.size();

            std::string_view line = text.substr(at, end - at);
            at = end + 1;

            if (!line.empty() && line.back() == '\r')
                line.remove_suffix(1);

            const std::size_t tab = line.rfind('\t');
            long long cents = 0;

            if (tab == std::string_view::npos || tab == 0 ||
                std::from_chars(line.data() + tab + 1, line.data() + line.size(), cents).ec != std::errc{} || cents <= 0)
                continue;

            out.emplace(std::string(line.substr(0, tab)), static_cast<double>(cents) / 100.0);
        }

        return !out.empty();
    }

    std::string LowerAscii(std::string text)
    {
        for (char& c : text)
            if (static_cast<unsigned char>(c) < 0x80)
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return text;
    }

    // [{"market_hash_name":"...","quantity":36,"min_price":65}, ...]   (min_price in cents)
    bool ParseCsFloat(const std::string& text, PriceMap& out)
    {
        try
        {
            const nlohmann::json root = nlohmann::json::parse(text);

            if (!root.is_array())
                return false;

            out.reserve(root.size());

            for (const nlohmann::json& item : root)
            {
                if (!item.is_object())
                    continue;

                const auto name = item.find("market_hash_name");
                const auto price = item.find("min_price");

                if (name == item.end() || price == item.end() || !name->is_string() || !price->is_number())
                    continue;

                const double cents = price->get<double>();
                if (cents > 0.0)
                    out.emplace(LowerAscii(name->get<std::string>()), cents / 100.0);
            }

            return !out.empty();
        }
        catch (...)
        {
            return false;
        }
    }

    // When the file was last written, as a unix time (0: no such file).
    long long WrittenAt(const std::filesystem::path& path)
    {
        WIN32_FILE_ATTRIBUTE_DATA data{};

        if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data) || (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
            return 0;

        ULARGE_INTEGER time{};
        time.LowPart = data.ftLastWriteTime.dwLowDateTime;
        time.HighPart = data.ftLastWriteTime.dwHighDateTime;

        constexpr unsigned long long EPOCH_DIFFERENCE = 116444736000000000ull;   // 1601-01-01 -> 1970-01-01, 100 ns units
        return time.QuadPart > EPOCH_DIFFERENCE ? static_cast<long long>((time.QuadPart - EPOCH_DIFFERENCE) / 10000000ull) : 0;
    }

    std::string DateOf(long long unixTime)
    {
        const std::time_t time = static_cast<std::time_t>(unixTime);
        std::tm local{};
        char text[16] = {};

        if (localtime_s(&local, &time) == 0)
            std::strftime(text, sizeof(text), "%Y-%m-%d", &local);

        return text;
    }

    // ── the one-click update ──
    constexpr const wchar_t* PRICE_LIST_URL = L"https://csfloat.com/api/v1/listings/price-list";
    constexpr ULONGLONG UPDATE_WAIT_MS = 15ull * 60ull * 1000ull;   // looks for the saved page this long
    constexpr ULONGLONG UPDATE_POLL_MS = 1500;
    constexpr long long RECENT_SECONDS = 30 * 60;                   // a list saved this recently is taken right away
    constexpr std::size_t MIN_PRICES = 1000;                        // fewer: not the price list

    // render thread only
    Prices::UpdateStatus g_update;
    ULONGLONG g_updateUntil = 0;
    ULONGLONG g_updateNextPoll = 0;
    long long g_updateSince = 0;                                    // unix time: only files saved after this
    std::filesystem::path g_updateRejected;                         // a file that wasn't the list (not tried again)
    long long g_updateRejectedAt = 0;

    std::filesystem::path DownloadsFolder()
    {
        PWSTR path = nullptr;
        std::filesystem::path out;

        if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Downloads, 0, nullptr, &path)) && path)
            out = path;

        if (path)
            CoTaskMemFree(path);

        return out;
    }

    // The newest saved CSFloat price list in Downloads written after `since`: "price-list.json", "price-list (1).json",
    // "price-list" (a browser's page save).
    std::filesystem::path FindSavedList(long long since)
    {
        const std::filesystem::path folder = DownloadsFolder();
        std::filesystem::path best;
        long long bestAt = 0;
        std::error_code ec;

        if (folder.empty())
            return best;

        for (std::filesystem::directory_iterator it(folder, std::filesystem::directory_options::skip_permission_denied, ec), end;
             !ec && it != end; it.increment(ec))
        {
            std::wstring name = it->path().filename().wstring();
            for (wchar_t& c : name)
                c = static_cast<wchar_t>(towlower(c));

            const std::wstring extension = it->path().extension().wstring();
            if (name.rfind(L"price-list", 0) != 0 && name.rfind(L"price_list", 0) != 0)
                continue;
            if (!extension.empty() && _wcsicmp(extension.c_str(), L".json") != 0 && _wcsicmp(extension.c_str(), L".txt") != 0)
                continue;

            const long long at = WrittenAt(it->path());
            if (at >= since && at > bestAt && !(it->path() == g_updateRejected && at == g_updateRejectedAt))
            {
                best = it->path();
                bestAt = at;
            }
        }

        return best;
    }

    bool ReadSmallFile(const std::filesystem::path& path, std::string& text)
    {
        std::error_code ec;
        const std::uintmax_t size = std::filesystem::file_size(path, ec);

        if (ec || size == 0 || size > MAX_FILE_SIZE)
            return false;

        std::ifstream file(path, std::ios::binary);
        std::stringstream stream;
        stream << file.rdbuf();
        text = stream.str();
        return file.good() || file.eof();
    }

    // The saved page -> the prices in use (the built-in list fills what it doesn't have) + the data folder's copy.
    // False: not CSFloat's price list (or still being written).
    bool UseSavedList(const std::filesystem::path& path, int& count)
    {
        std::string text;
        PriceMap fresh;

        if (!ReadSmallFile(path, text) || !ParseCsFloat(text, fresh) || fresh.size() < MIN_PRICES)
            return false;

        PriceMap prices;
        long long madeAt = 0;
        std::string unusedDate;
        ParseBuiltIn(BuiltInText(), prices, madeAt, unusedDate);

        for (auto& [name, price] : fresh)
            prices.insert_or_assign(name, price);

        // kept for the next start (Prices::Start reads it when it's newer than the built-in list)
        const std::filesystem::path copy = DataPath(OVERRIDE_FILE);
        {
            std::ofstream out(copy, std::ios::binary | std::ios::trunc);
            out.write(text.data(), static_cast<std::streamsize>(text.size()));
        }

        count = static_cast<int>(prices.size());
        const std::string date = DateOf(static_cast<long long>(std::time(nullptr)));

        {
            std::lock_guard lock(g_mutex);
            g_info = { Prices::State::Ready, count, true, date };
            g_prices = std::move(prices);
        }

        g_ready.store(true, std::memory_order_release);
        g_generation.fetch_add(1, std::memory_order_acq_rel);

        char line[200];
        snprintf(line, sizeof(line), "prices: updated - %d items from %ls (CSFloat, %s)", count, path.filename().c_str(), date.c_str());
        Unboxer::LogLine(line);
        return true;
    }

    // The data folder's prices_csfloat.json, if it's newer than `notBefore` (the built-in list).
    bool LoadOverride(long long notBefore, PriceMap& out, std::string& date)
    {
        const std::filesystem::path path = DataPath(OVERRIDE_FILE);
        const long long writtenAt = WrittenAt(path);

        if (writtenAt <= notBefore)
            return false;   // none, or older than the built-in list (left over from Lootline 1.0, which downloaded it)

        std::error_code ec;
        const std::uintmax_t size = std::filesystem::file_size(path, ec);

        if (ec || size == 0 || size > MAX_FILE_SIZE)
            return false;

        std::ifstream file(path, std::ios::binary);
        std::stringstream text;
        text << file.rdbuf();

        if (!ParseCsFloat(text.str(), out))
        {
            Unboxer::LogLine("prices: prices_csfloat.json in the data folder isn't a CSFloat price list - not used");
            return false;
        }

        date = DateOf(writtenAt);
        return true;
    }
}

void Prices::Start()
{
    if (g_ready.load(std::memory_order_acquire))
        return;

    PriceMap prices;
    long long madeAt = 0;
    std::string date;
    const bool builtIn = ParseBuiltIn(BuiltInText(), prices, madeAt, date);

    PriceMap newer;
    std::string newerDate;
    const bool fromFile = LoadOverride(madeAt, newer, newerDate);

    if (fromFile)
    {
        for (auto& [name, price] : newer)
            prices.insert_or_assign(name, price);   // the built-in list still fills what the file doesn't have
        date = newerDate;
    }

    if (!builtIn && !fromFile)
    {
        Unboxer::LogLine("prices: the built-in list is missing - the upgrader uses estimates");

        std::lock_guard lock(g_mutex);
        g_info.state = State::Missing;
        return;
    }

    char line[160];
    snprintf(line, sizeof(line), "prices: %zu items, %s list from %s", prices.size(), fromFile ? "your prices_csfloat.json" : "built-in",
        date.c_str());
    Unboxer::LogLine(line);

    std::lock_guard lock(g_mutex);
    g_info = { State::Ready, static_cast<int>(prices.size()), fromFile, date };
    g_prices = std::move(prices);
    g_ready.store(true, std::memory_order_release);
    g_generation.fetch_add(1, std::memory_order_acq_rel);
}

bool Prices::Ready()
{
    return g_ready.load(std::memory_order_acquire);
}

double Prices::Find(const std::string& marketNameLower)
{
    if (marketNameLower.empty() || !g_ready.load(std::memory_order_acquire))
        return -1.0;

    std::lock_guard lock(g_mutex);
    const auto it = g_prices.find(marketNameLower);
    return it != g_prices.end() ? it->second : -1.0;
}

double Prices::FindPhased(const std::string& marketNameLower, int paintKit, bool& estimated)
{
    estimated = false;
    const Phase* phase = PhaseOf(paintKit);

    if (!phase || marketNameLower.empty())
        return Find(marketNameLower);

    if (const double price = Find(marketNameLower + " - " + phase->name); price > 0.0)
        return price;

    // the name's price is whichever phase was cheapest: only a guess for this one
    const double cheapest = Find(marketNameLower);

    if (cheapest <= 0.0)
        return cheapest;

    estimated = true;
    return cheapest * phase->share;
}

double Prices::FindLowest(const std::string& prefixLower)
{
    if (prefixLower.empty() || !g_ready.load(std::memory_order_acquire))
        return -1.0;

    std::lock_guard lock(g_mutex);
    double lowest = -1.0;

    for (const auto& [name, price] : g_prices)
        if (price > 0.0 && name.starts_with(prefixLower) && (lowest < 0.0 || price < lowest))
            lowest = price;

    return lowest;
}

std::vector<std::pair<std::string, double>> Prices::WithPrefix(const std::string& prefixLower)
{
    std::vector<std::pair<std::string, double>> out;

    if (prefixLower.empty() || !g_ready.load(std::memory_order_acquire))
        return out;

    std::lock_guard lock(g_mutex);

    for (const auto& [name, price] : g_prices)
        if (price > 0.0 && name.starts_with(prefixLower))
            out.emplace_back(name, price);

    return out;
}

int Prices::Generation()
{
    return g_generation.load(std::memory_order_acquire);
}

Prices::Info Prices::GetInfo()
{
    std::lock_guard lock(g_mutex);
    return g_info;
}

void Prices::BeginUpdate()
{
    const long long now = static_cast<long long>(std::time(nullptr));

    // saved a moment ago (the browser was open already): taken right away
    if (const std::filesystem::path recent = FindSavedList(now - RECENT_SECONDS); !recent.empty())
    {
        int count = 0;

        if (UseSavedList(recent, count))
        {
            g_update = { UpdateState::Done, "Prices updated", count };
            return;
        }

        g_updateRejected = recent;
        g_updateRejectedAt = WrittenAt(recent);
    }

    // the browser shows CSFloat's price list (your browser goes online, Lootline doesn't)
    ShellExecuteW(nullptr, L"open", PRICE_LIST_URL, nullptr, nullptr, SW_SHOWNORMAL);
    g_updateSince = now - 5;
    g_updateUntil = GetTickCount64() + UPDATE_WAIT_MS;
    g_updateNextPoll = GetTickCount64() + UPDATE_POLL_MS;
    g_update = { UpdateState::Waiting, "Waiting for the saved price list", 0 };
    Unboxer::LogLine("prices: update - CSFloat's price list opened in the browser, waiting for it in Downloads");
}

void Prices::CancelUpdate()
{
    if (g_update.state == UpdateState::Waiting)
        g_update = { UpdateState::Idle, std::string(), 0 };
}

void Prices::TickUpdate()
{
    if (g_update.state != UpdateState::Waiting)
        return;

    const ULONGLONG now = GetTickCount64();

    if (now < g_updateNextPoll)
        return;

    g_updateNextPoll = now + UPDATE_POLL_MS;

    if (const std::filesystem::path saved = FindSavedList(g_updateSince); !saved.empty())
    {
        int count = 0;

        if (UseSavedList(saved, count))
        {
            g_update = { UpdateState::Done, "Prices updated", count };
            return;
        }

        // not (yet) the list: maybe still being written - tried again until the time runs out, then reported
        if (now >= g_updateUntil)
        {
            g_update = { UpdateState::Failed, "The saved file isn't CSFloat's price list", 0 };
            return;
        }
    }

    if (now >= g_updateUntil)
        g_update = { UpdateState::Failed, "No saved price list found in Downloads", 0 };
}

Prices::UpdateStatus Prices::GetUpdateStatus()
{
    return g_update;
}
