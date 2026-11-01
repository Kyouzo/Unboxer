# Builds data\prices.txt, the price list compiled into Lootline.dll (the DLL itself never goes online).
#
# 1. Open these pages in your browser and save each one as a file (Ctrl+S):
#      https://csfloat.com/api/v1/listings/price-list          -> csfloat.json
#      https://market.csgo.com/api/v2/prices/USD.json          -> marketcsgo.json   (optional)
#      https://prices.csgotrader.app/latest/csfloat.json       -> phases.json       (optional: Doppler phase prices)
# 2. python tools\make_prices.py csfloat.json marketcsgo.json phases.json   (any order: each file is recognised)
#    Only the phases (no new csfloat.json): python tools\make_prices.py data\prices.txt phases.json
#    - the old list stays as it is (its date too), the phases are added.
# 3. Rebuild (BUILD.bat).
#
# CSFloat (lowest listing) wins where both lists have the item; market.csgo.com fills the gaps.
# The market sells every Doppler / Gamma Doppler phase under one name, so that name's price is its cheapest phase's.
# csgotrader's CSFloat list has each phase's lowest listing: written as "<name> - <phase>" lines (Prices::FindPhased).
# Names are stored lowercase (ASCII letters only, like the DLL's lookup), prices in cents.

import datetime
import json
import os
import sys


def lower_ascii(name):
    return bytes(b + 32 if 65 <= b <= 90 else b for b in name.encode("utf-8")).decode("utf-8")


def to_cents(dollars):
    try:
        value = float(dollars)
    except (TypeError, ValueError):
        return 0
    return max(1, int(round(value * 100))) if value > 0 else 0


def read_csfloat(root):
    # [{"market_hash_name": "...", "quantity": 36, "min_price": 65}, ...]   (cents)
    out = {}
    for item in root:
        if not isinstance(item, dict):
            continue
        name, cents = item.get("market_hash_name"), item.get("min_price")
        if isinstance(name, str) and isinstance(cents, (int, float)) and cents > 0:
            out[lower_ascii(name)] = int(round(cents))
    return out


def read_marketcsgo(root):
    # {"success": true, "items": [{"market_hash_name": "...", "price": "12.840"}, ...]}   (dollars)
    out = {}
    for item in root.get("items", []):
        if not isinstance(item, dict):
            continue
        name, cents = item.get("market_hash_name"), to_cents(item.get("price"))
        if isinstance(name, str) and cents > 0:
            out[lower_ascii(name)] = cents
    return out


def read_csgotrader(root):
    # {"<market name>": {"starting_at": {"price": 1900.5, "doppler": {"Sapphire": 9000, "Phase 2": ...}}, ...}}   (dollars)
    # -> (prices by name, phase prices by "<name> - <phase>")
    prices, phases = {}, {}
    for name, entry in root.items():
        if not isinstance(name, str) or not isinstance(entry, dict):
            continue
        starting = entry.get("starting_at")
        if not isinstance(starting, dict):
            continue
        key = lower_ascii(name)
        cents = to_cents(starting.get("price"))
        if cents > 0:
            prices[key] = cents
        doppler = starting.get("doppler")
        if isinstance(doppler, dict):
            for phase, value in doppler.items():
                cents = to_cents(value)
                if isinstance(phase, str) and cents > 0:
                    phases[key + " - " + lower_ascii(phase)] = cents
    return prices, phases


def read_lootline(path):
    # data\prices.txt itself: "LOOTLINE-PRICES 1 <unix time> <yyyy-mm-dd>", then "<name>\t<cents>" lines
    # -> (prices by name, (unix time, date)), or None if it isn't one
    with open(path, encoding="utf-8") as f:
        header = f.readline().split()
        if len(header) != 4 or header[0] != "LOOTLINE-PRICES" or header[1] != "1":
            return None
        out = {}
        for line in f:
            name, tab, cents = line.rstrip("\n").rpartition("\t")
            if tab and cents.isdigit() and int(cents) > 0:
                out[name] = int(cents)
    return out, (int(header[2]), header[3])


def kind_of(root):
    if isinstance(root, list):
        return "csfloat"
    if isinstance(root, dict) and isinstance(root.get("items"), list):
        return "marketcsgo"
    if isinstance(root, dict) and any(isinstance(v, dict) and "starting_at" in v for v in list(root.values())[:50]):
        return "csgotrader"
    return None


def main():
    if len(sys.argv) < 2:
        print("usage: python tools\\make_prices.py csfloat.json [marketcsgo.json] [phases.json]")
        return 1

    csfloat, marketcsgo, phases = {}, {}, {}
    base, made = {}, None

    for path in sys.argv[1:]:
        if (lootline := read_lootline(path)) is not None:
            base, made = lootline
            continue
        with open(path, encoding="utf-8") as f:
            root = json.load(f)
        kind = kind_of(root)
        if kind == "csfloat":
            csfloat = read_csfloat(root)
        elif kind == "marketcsgo":
            marketcsgo = read_marketcsgo(root)
        elif kind == "csgotrader":
            _, phases = read_csgotrader(root)
        else:
            print("%s: not a price list this script knows" % os.path.basename(path))
            return 1

    if len(csfloat) < 1000 and len(base) < 1000:
        print("csfloat.json has only %d prices - not the price list?" % len(csfloat))
        return 1

    # weakest first: market.csgo.com, then the old list, then CSFloat's own list. csgotrader's copy gives only the
    # phases: its other prices include items with nothing but joke listings ($99 999 for a Souvenir AK-47), which
    # CSFloat's own list leaves out
    prices = dict(marketcsgo)
    prices.update(base)
    prices.update(csfloat)

    # a phase line only where its weapon is listed too (the DLL scales from that name otherwise), and not a joke listing:
    # more than 4x what that phase usually costs next to the weapon's listed price -> left out (the DLL's estimate)
    phases = {name: cents for name, cents in phases.items() if name.rsplit(" - ", 1)[0] in prices}
    ratios = {}
    for name, cents in phases.items():
        stem, _, phase = name.rpartition(" - ")
        ratios.setdefault(phase, []).append(cents / prices[stem])
    usual = {phase: sorted(values)[len(values) // 2] for phase, values in ratios.items()}
    jokes = {name for name, cents in phases.items()
             if cents > 4 * usual[name.rpartition(" - ")[2]] * prices[name.rpartition(" - ")[0]]}
    phases = {name: cents for name, cents in phases.items() if name not in jokes}
    prices.update(phases)
    for name in sorted(jokes):
        print("left out (joke listing?): %s" % name.encode("ascii", "replace").decode())

    # a fresh CSFloat list is today's; only phases added to the old list: still the old list's day (the DLL takes a
    # prices_csfloat.json saved after that day over it)
    if csfloat or not made:
        now = datetime.datetime.now(datetime.timezone.utc)
        made = (int(now.timestamp()), now.strftime("%Y-%m-%d"))

    out = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "data", "prices.txt")
    lines = ["LOOTLINE-PRICES 1 %d %s" % made]
    lines += ["%s\t%d" % (name, cents) for name, cents in sorted(prices.items()) if "\t" not in name and "\n" not in name]

    with open(out, "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(lines) + "\n")

    print("data\\prices.txt: %d prices (%d from CSFloat, %d Doppler phases), %s" %
          (len(lines) - 1, len(csfloat), len(phases), made[1]))
    return 0


if __name__ == "__main__":
    sys.exit(main())
