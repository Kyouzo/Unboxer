#pragma once

#include <string>
#include <utility>
#include <vector>

// Prices (USD) for the upgrader. Lootline never goes online: the list is compiled into the DLL (data\prices.txt,
// made by tools\make_prices.py from CSFloat's lowest listings + market.csgo.com for what CSFloat has none of,
// ~30 000 items, + Doppler phases from csgotrader's copy of CSFloat). A newer CSFloat price list saved by hand as prices_csfloat.json in the data folder is used instead
// of the built-in one. Looked up by the Steam market name, lowercase (Cases::MarketName).
namespace Prices
{
    void Start();    // init: reads the list (a fraction of a second)

    [[nodiscard]] bool Ready();
    [[nodiscard]] double Find(const std::string& marketNameLower);   // < 0: not listed
    // A Doppler / Gamma Doppler sells every phase (Ruby, Sapphire, Phase 2...) under one market name, so that name's price
    // is its cheapest phase's. For such a paint kit: the phase's own price ("<name> - sapphire" lines, from csgotrader's
    // CSFloat phase prices - tools\make_prices.py), else the name's price times the phase's usual share (estimated =
    // true). Any other paint kit: Find. < 0: not listed.
    [[nodiscard]] double FindPhased(const std::string& marketNameLower, int paintKit, bool& estimated);
    // The cheapest item whose name starts with this ("sealed graffiti | 1g (" = every colour of it). < 0: none.
    // Goes through the whole list: cache the answer.
    [[nodiscard]] double FindLowest(const std::string& prefixLower);
    // Every listed item whose name starts with this, with its price (a copy). Goes through the whole list: cache it.
    [[nodiscard]] std::vector<std::pair<std::string, double>> WithPrefix(const std::string& prefixLower);
    [[nodiscard]] int Generation();                                  // changes each time a list is loaded

    // ── the currency prices are shown in (Settings -> Prices; currency.cpp) ──
    // The list is in US dollars; the others use the exchange rates of the list's day, built in (never online).
    [[nodiscard]] int CurrencyCount();
    [[nodiscard]] const char* CurrencyCode(int index);               // "USD", "EUR", "RUB"...
    [[nodiscard]] const char* CurrencyName(int index);               // "US dollar" (English: tr() it)
    void SetCurrency(int index);                                     // any thread (the menu sets it every frame)
    [[nodiscard]] int CurrencyIndex();
    // A dollar amount in the picked currency, written the way Steam writes it: "$1.25", "1,12€", "107,50 руб."... Any thread.
    [[nodiscard]] std::string Format(double dollars);

    enum class State { NotLoaded, Ready, Missing };

    struct Info
    {
        State state = State::NotLoaded;
        int count = 0;               // prices in the list
        bool fromFile = false;       // prices_csfloat.json from the data folder, not the built-in list
        std::string date;            // "2026-10-01": when the list was made
    };

    [[nodiscard]] Info GetInfo();                                    // for the menu's status line

    // The one-click update (Settings -> Prices). Lootline never goes online: your browser opens CSFloat's price list,
    // you save it (Ctrl+S, into Downloads) and Lootline takes the saved file by itself - checked, kept as the data
    // folder's prices_csfloat.json and used right away. Render thread.
    void BeginUpdate();     // a list saved in Downloads in the last half hour: taken right away; else the browser opens
    void CancelUpdate();
    void TickUpdate();      // every frame (cheap): while an update waits, looks for the saved page every 1.5 s

    enum class UpdateState { Idle, Waiting, Done, Failed };

    struct UpdateStatus
    {
        UpdateState state = UpdateState::Idle;
        std::string text;    // what happened (English: tr() it)
        int count = 0;       // Done: prices in the new list
    };

    [[nodiscard]] UpdateStatus GetUpdateStatus();
}
