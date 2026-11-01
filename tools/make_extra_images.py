# Makes data\extra_images.bin: the pictures CS2 itself doesn't ship (checked against the game's pak01_dir.vpk) -
# containers, map coins / collectibles and charms. 2026-10-07: the Budapest 2025 Overpass Souvenir Package (def 5221),
# the (Redacted) Map Coin (def 5180) and the Budapest 2025 souvenir charm (keychain 83). Like the other tools this only
# reads saved files (Lootline never goes online); the file is compiled into the DLL.
#
#   python make_extra_images.py <items_game.txt> <pak01_dir.vpk> <pictures folder> <out extra_images.bin> [crates.json]
#
# pictures folder (data\extra_pictures): <definition>.png per missing item (e.g. 5221.png), kc<id>.png per missing charm
# (kc83.png) - saved by hand from the item's Steam market / inventory picture. 5180.png (the (Redacted) Map Coin) was made
# from the game's own CS2 contributor coins: their median (the map names cancel out) with "(REDACTED)" written round
# the bottom like the others. With crates.json (https://raw.githubusercontent.com/ByMykel/CSGO-API/main/public/api/en/crates.json)
# the script prints where each missing container picture can be saved from.
# Format (little endian): "LLIMG1", then per picture [u16 length][image_inventory path, e.g.
# econ/tournaments/bud2025/crate_bud2025_promo_de_overpass][u32 size][PNG].
import json, os, re, struct, sys

PNG_SIGNATURE = b"\x89PNG\r\n\x1a\n"


def vpk_files(path):
    def cstr(f):
        out = bytearray()
        while True:
            c = f.read(1)
            if not c or c == b"\0":
                return out.decode("utf-8", "replace")
            out += c

    files = set()
    with open(path, "rb") as f:
        _, version, _ = struct.unpack("<III", f.read(12))
        if version == 2:
            f.read(16)
        while True:
            ext = cstr(f)
            if not ext:
                break
            while True:
                folder = cstr(f)
                if not folder:
                    break
                while True:
                    name = cstr(f)
                    if not name:
                        break
                    _, preload, _, _, _, _ = struct.unpack("<IHHIIH", f.read(18))
                    f.read(preload)
                    files.add((f"{folder}/{name}.{ext}" if folder != " " else f"{name}.{ext}").lower())
    return files


def main(items_game_path, vpk_path, pictures, out_path, crates_path=None):
    text = open(items_game_path, encoding="utf-8", errors="replace").read()
    files = vpk_files(vpk_path)

    def shipped(image):
        return ("panorama/images/" + image + "_png.vtex_c").lower() in files

    urls = {}
    if crates_path:
        for crate in json.load(open(crates_path, encoding="utf-8")):
            if crate.get("def_index") and crate.get("image") and not str(crate.get("id", "")).endswith("_highlight"):
                urls.setdefault(str(crate["def_index"]), crate["image"] + "/512fx384f")

    # (picture file name without .png, image_inventory path, what it is)
    wanted = []

    # items: containers and map coins / collectibles
    for m in re.finditer(r'\n\t\t"(\d+)"\n\t\t\{(.*?)\n\t\t\}', text, re.S):
        body = m.group(2)
        container = '"set supply crate series"' in body
        collectible = re.search(r'"prefab"\s*"[^"]*(map_token|collectible|coin|pin)[^"]*"', body) is not None
        if not container and not collectible:
            continue
        image = re.search(r'"image_inventory"\s*"([^"]+)"', body)
        if image and not shipped(image.group(1)):
            wanted.append((m.group(1), image.group(1), "container" if container else "collectible"))

    # charms (keychain_definitions blocks; a highlight charm with a "base" uses its base's picture)
    for section in re.finditer(r'"keychain_definitions"\s*\{(.*?)\n\t\}', text, re.S):
        for m in re.finditer(r'\n\t\t"(\d+)"\n\t\t\{(.*?)\n\t\t\}', section.group(1), re.S):
            image = re.search(r'"image_inventory"\s*"([^"]+)"', m.group(2))
            if image and not shipped(image.group(1)):
                wanted.append(("kc" + m.group(1), image.group(1), "charm"))

    out = bytearray(b"LLIMG1")
    for key, path, what in wanted:
        picture = os.path.join(pictures, key + ".png")
        if not os.path.isfile(picture):
            print(f"  {what} {key} {path}: no {key}.png - save it from {urls.get(key, 'its Steam market page')}")
            continue
        png = open(picture, "rb").read()
        if not png.startswith(PNG_SIGNATURE):
            print(f"  {what} {key} {path}: {key}.png isn't a PNG - skipped")
            continue
        name = path.encode("utf-8")
        out += struct.pack("<H", len(name)) + name + struct.pack("<I", len(png)) + png
        print(f"  {what} {key} {path}: {len(png)} bytes")

    open(out_path, "wb").write(out)
    print(f"{len(wanted)} missing, {out_path}: {len(out)} bytes")


if __name__ == "__main__":
    main(*sys.argv[1:6])
