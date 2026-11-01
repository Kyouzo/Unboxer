# Makes data\case_rares.txt: every weapon case's ★ items (knives / gloves, each finish and Doppler phase), which the
# game files don't have (Valve keeps those lists on its servers). Lootline never goes online: this runs by hand, the
# file is compiled into the DLL.
#
#   python make_case_rares.py <crates.json> <items_game.txt> <csgo_english.txt> <out case_rares.txt>
#
# crates.json: https://raw.githubusercontent.com/ByMykel/CSGO-API/main/public/api/en/crates.json (each case's
# "contains_rare": "★ Navaja Knife | Doppler" + paint_index). items_game.txt / csgo_english.txt: from CS2's VPK.
# The knife / glove names are matched to definitions with the game's own English names.
import json, re, sys, time


def kv_blocks(text, section):
    """Every depth-1 block named `section` (items_game repeats sections), as {key: body text}."""
    out = {}
    for m in re.finditer(r'\n\t"%s"\s*\n\t\{' % re.escape(section), text):
        depth, i = 1, m.end()
        start = i
        while depth and i < len(text):
            c = text[i]
            if c == '"':
                i = text.index('"', i + 1)
            elif c == '{':
                depth += 1
            elif c == '}':
                depth -= 1
            i += 1
        body = text[start:i - 1]
        for e in re.finditer(r'\n\t\t"([^"]+)"\s*\n\t\t\{(.*?)\n\t\t\}', body, re.S):
            out[e.group(1)] = e.group(2)
    return out


def main(crates_path, items_game_path, english_path, out_path):
    crates = json.load(open(crates_path, encoding="utf-8"))
    ig = open(items_game_path, encoding="utf-8", errors="replace").read()
    en_text = open(english_path, encoding="utf-8", errors="replace").read()

    english = {}
    for m in re.finditer(r'^\s*"([^"]+)"\s+"((?:[^"\\]|\\.)*)"', en_text, re.M):
        english.setdefault(m.group(1).lower(), m.group(2))

    items = kv_blocks(ig, "items")
    prefabs = kv_blocks(ig, "prefabs")
    paint_kits = kv_blocks(ig, "paint_kits")

    def value(body, key):
        m = re.search(r'\n\t\t\t"%s"\s+"([^"]*)"' % re.escape(key), body)
        return m.group(1) if m else None

    def inherited(body, key, depth=0):
        v = value(body, key)
        if v is not None or depth > 8:
            return v
        for p in (value(body, "prefab") or "").split():
            if p in prefabs:
                r = inherited(prefabs[p], key, depth + 1)
                if r is not None:
                    return r
        return None

    # "navaja knife" -> 520, "broken fang gloves" -> 4725 ...
    by_name = {}
    for d, body in items.items():
        if not d.isdigit():
            continue
        name = value(body, "name") or ""
        if not (name.startswith("weapon_knife") or name == "weapon_bayonet" or "glove" in name or "handwrap" in name):
            continue
        token = (inherited(body, "item_name") or "").lstrip("#").lower()
        if token in english:
            by_name.setdefault(english[token].lower(), int(d))

    known_paints = {int(k) for k in paint_kits if k.isdigit()}
    lines, problems, total = [], [], 0

    for crate in crates:
        rares = crate.get("contains_rare") or []
        if not rares:
            continue
        entries = []
        for rare in rares:
            full = rare["name"].replace("★", "").strip()
            weapon = full.split(" | ")[0].strip().lower()
            definition = by_name.get(weapon)
            paint = int(rare.get("paint_index") or 0) if " | " in full else 0
            if not definition or (paint and paint not in known_paints):
                problems.append(f'{crate["name"]}: {rare["name"]} (paint {paint})')
                continue
            entry = f"{definition}:{paint}"
            if entry not in entries:
                entries.append(entry)
        total += len(entries)
        lines.append(f'{crate["def_index"]}\t{" ".join(entries)}\t# {crate["name"]}')

    header = f"LOOTLINE-CASE-RARES 1 {int(time.time())} {time.strftime('%Y-%m-%d')}"
    with open(out_path, "w", encoding="utf-8", newline="\n") as f:
        f.write(header + "\n" + "\n".join(lines) + "\n")

    print(f"{len(lines)} cases, {total} items -> {out_path}")
    for p in problems:
        print("  NOT MATCHED:", p)
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main(*sys.argv[1:5]))
