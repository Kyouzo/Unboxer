#include "prices.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <iterator>

// The currency prices are shown in. The price list is in US dollars; the rates are the mid-market rates of the list's
// day (2026-10-07: Trading Economics / XE / Wise / the National Bank of Kazakhstan), built in - Lootline never goes
// online. A newer price list doesn't move them; they're only for showing a price in your money.
namespace
{
    struct CurrencyInfo
    {
        const char* code;
        const char* name;       // English (the menu tr()s it)
        double perDollar;
        const char* prefix;
        const char* suffix;
        char decimal;           // '.' or ','
        char group;             // thousands: ',' '.' or ' '
        bool whole;             // no cents (tenge)
    };

    constexpr CurrencyInfo CURRENCIES[] = {
        { "USD", "US dollar",          1.0,    "$",            "",                             '.', ',', false },
        { "EUR", "Euro",               0.8887, "",             "\xE2\x82\xAC",                 ',', '.', false },   // 1,12€
        { "RUB", "Russian ruble",      85.5,   "",             " \xD1\x80\xD1\x83\xD0\xB1.",   ',', ' ', false },   // 107,50 руб.
        { "GBP", "British pound",      0.7545, "\xC2\xA3",     "",                             '.', ',', false },   // £0.94
        { "UAH", "Ukrainian hryvnia",  45.0,   "",             "\xE2\x82\xB4",                 ',', ' ', false },   // 56,25₴
        { "PLN", "Polish zloty",       3.91,   "",             "z\xC5\x82",                    ',', ' ', false },   // 4,89zł
        { "KZT", "Kazakhstani tenge",  453.65, "",             "\xE2\x82\xB8",                 ',', ' ', true },    // 567₸
        { "TRY", "Turkish lira",       49.2,   "\xE2\x82\xBA", "",                             ',', '.', false },   // ₺61,50
        { "BRL", "Brazilian real",     5.0,    "R$ ",          "",                             ',', '.', false },   // R$ 6,25
        { "CNY", "Chinese yuan",       6.705,  "\xC2\xA5 ",    "",                             '.', ',', false },   // ¥ 8.38
        { "ILS", "Israeli shekel",     3.07,   "\xE2\x82\xAA", "",                             '.', ',', false },   // ₪3.84
    };

    constexpr int COUNT = static_cast<int>(std::size(CURRENCIES));
    std::atomic_int g_currency{ 0 };
}

int Prices::CurrencyCount()
{
    return COUNT;
}

const char* Prices::CurrencyCode(int index)
{
    return CURRENCIES[std::clamp(index, 0, COUNT - 1)].code;
}

const char* Prices::CurrencyName(int index)
{
    return CURRENCIES[std::clamp(index, 0, COUNT - 1)].name;
}

void Prices::SetCurrency(int index)
{
    g_currency.store(std::clamp(index, 0, COUNT - 1), std::memory_order_relaxed);
}

int Prices::CurrencyIndex()
{
    return g_currency.load(std::memory_order_relaxed);
}

std::string Prices::Format(double dollars)
{
    const CurrencyInfo& currency = CURRENCIES[std::clamp(g_currency.load(std::memory_order_relaxed), 0, COUNT - 1)];
    const double amount = std::max(0.0, std::isfinite(dollars) ? dollars : 0.0) * currency.perDollar;

    // cents below 100 (0.03 / 42.50), whole numbers above (1 250 / 68 000)
    const bool cents = !currency.whole && amount < 100.0;
    const long long total = std::llround(amount * (cents ? 100.0 : 1.0));
    const long long whole = cents ? total / 100 : total;

    char digits[32];
    snprintf(digits, sizeof(digits), "%lld", whole);

    std::string grouped;
    const int length = static_cast<int>(std::char_traits<char>::length(digits));

    for (int i = 0; i < length; ++i)
    {
        if (i > 0 && (length - i) % 3 == 0)
            grouped += currency.group;
        grouped += digits[i];
    }

    std::string text = currency.prefix;
    text += grouped;

    if (cents)
    {
        char fraction[4];
        snprintf(fraction, sizeof(fraction), "%02lld", total % 100);
        text += currency.decimal;
        text += fraction;
    }

    text += currency.suffix;
    return text;
}
