#!/usr/bin/env python3
"""
Import the buildings of the Lords Mobile fandom wiki into gamedata/buildings.json.

Like the researches, the buildings' data (costs, times, effects per level) is not sent by the game server,
only by the client's own files. The wiki lists it, through the public MediaWiki API (tools/wiki_common.py):

    https://lordsmobile.fandom.com/wiki/Category:Buildings   -> the building pages (5 sub-categories)
    https://lordsmobile.fandom.com/wiki/<building>           -> per level: effect, might, requirement,
                                                                duration and cost, then the "Mana upgrade"

and writes:

    gamedata/buildings.json          the data
    gamedata/building_images.json    where each picture is (url, size), for tools/fetch_research_images.py

Usage, from the repository root (standard library only, about 40 requests, one per second):

    python3 tools/import_wiki_buildings.py

Levels. The game stores ONE number per building (BUILDINGINFO, docs/buildings.md): 1 to 25 are the
normal levels; above 25 comes the Mana upgrade, 6 mana levels of 5 steps each, so the number goes up to
55 (25 + 6 x 5): level 30 is mana 1, 32 is mana 1 with 2 of the 5 steps of mana 2 done, 35 is mana 2.
The wiki's "Mana upgrade" table has one row per mana level, and says "you need to build it 5 times at the
current stage".

A value the wiki gives in a way that cannot be read without guessing is stored as null and listed in the
table's "warnings", never corrected here. The wiki text is CC BY-SA 3.0; the pictures are game artwork
that belongs to IGG.
"""

import argparse
import datetime
import json
import os
import re
import sys

from wiki_common import (WIKI, category_members, clean, fetch_image_info, fetch_wikitext, intro, requirements,
                         to_int, to_seconds, wikitables)

# The building's number in the game's protocol (BUILDINGINFO build_id) by wiki page.
# "confirmed": the name is the one the bot has had since it decoded BUILDINGINFO (GetBuildingName, protocol.c),
# or was read next to the number in the game. "inferred": deduced, the reason is in docs/buildings.md.
KNOWN_GAME_IDS = {
    "Lumber Mill": (1, "confirmed"),
    "Quarry": (2, "confirmed"),
    "Mine": (3, "confirmed"),
    "Farm": (4, "confirmed"),
    "Manor": (5, "confirmed"),
    "Barrack": (6, "confirmed"),
    "Infirmary": (7, "confirmed"),
    "Castle": (8, "confirmed"),
    "Vault": (9, "confirmed"),
    "Academy": (10, "confirmed"),
    "Castle Wall": (12, "confirmed"),
    "Watchtower": (13, "confirmed"),
    "Embassy": (14, "confirmed"),
    "Workshop": (15, "confirmed"),
    "Treasure Trove": (16, "inferred"),
    "Trading Post": (17, "confirmed"),
    # the player named the building under construction (mana 1 -> 1 + 1/5): it is 26 in the construction packet
    "Mana Lode": (26, "confirmed"),
}

FAMILIES = {
    "Standard_Buildings": "Standard",
    "Military_Buildings": "Military",
    "Resource_Buildings": "Resource",
    "Advanced_Buildings": "Advanced",
    "Familiar_Buildings": "Familiar",
}

MIGHT_COLUMNS = ("Might Bonus", "Might", "MightBonus", "Might Might Bonus")
TIME_COLUMNS = ("Orig. Time", "Original Time", "Time", "Orig. Research Time")
REQUIREMENT_COLUMNS = ("Requirements", "Required", "Requir.", "Req.", "Requires")
RESOURCES = ("Food", "Stone", "Timber", "Ore", "Gold")
# what a building costs besides the five resources (the wiki's column names)
SPECIAL_COSTS = {
    "Mana Ore": "mana_ore", "Mana Crystal": "mana_crystal", "Manasteel": "manasteel", "manasteel": "manasteel",
    "Mana Steel": "manasteel", "Archaic Tome": "archaic_tome", "War Tome": "war_tome",
    "Soul Crystal": "soul_crystal", "Crystal Pickaxe Crystal Pickaxe": "crystal_pickaxe",
    "Steel Cuff": "steel_cuff", "Steel Cuffs": "steel_cuff", "Anima": "anima", "Gold Hammer": "gold_hammer",
}
LEVEL_HEADERS = ("Level", "Lvl.", "Lvl", "Lv.", "Lv")


def cost_key(header):
    """A column of resources -> the resources it stands for, or None.

    Two icons in one header cell come out as one word ("StoneTimber", "FoodStoneOre"): the wiki gives one
    amount for all the resources it names, which is what those columns mean.
    """
    if header in RESOURCES:
        return [header.lower()]
    stripped = re.sub(r" ?Cost$", "", header)
    if stripped in RESOURCES:
        return [stripped.lower()]
    words = re.findall("|".join(RESOURCES), header)
    if words and "".join(words) == header.replace(" ", ""):
        return [w.lower() for w in words]
    return None


def parse_table(rows, section):
    header = [text for _, text in rows[0]]
    warnings, levels, effect_columns, totals = [], {}, [], {}
    for row in rows[1:]:
        values = [text for _, text in row]
        if values[0].lower().startswith(("total", "sum")):
            totals.update(dict(zip(header, values)))
            continue
        level = to_int(values[0])
        if level is None:
            # a row of headers, or a range such as "1-24": not a level
            if row[0][0] != "h":
                warnings.append("level not a number: %r" % values[0])
            continue
        entry = levels.setdefault(level, {"level": level, "effects": {}, "cost": {}})
        if len(values) != len(header):
            warnings.append("level %d: %d cells for %d columns" % (level, len(values), len(header)))

        for column, value in zip(header[1:], values[1:]):
            keys = cost_key(column)
            if column in MIGHT_COLUMNS:
                entry["might"] = to_int(value)
                if entry["might"] is None and value:
                    warnings.append("level %d: might %r" % (level, value))
            elif column in TIME_COLUMNS:
                entry["time"] = to_seconds(value)
                if entry["time"] is None and value:
                    warnings.append("level %d: time %r" % (level, value))
            elif column in REQUIREMENT_COLUMNS:
                entry["requirements"] = requirements(value)
                entry["requirements_text"] = value
            elif keys:
                amount = to_int(value)
                if amount is None and value:
                    warnings.append("level %d: %s cost %r" % (level, column, value))
                for key in keys:
                    entry["cost"][key] = amount
            elif column in SPECIAL_COSTS:
                amount = to_int(value)
                if amount is None and value:
                    warnings.append("level %d: %s cost %r" % (level, column, value))
                entry["cost"][SPECIAL_COSTS[column]] = amount
            elif column:
                entry["effects"][column] = value
                if column not in effect_columns:
                    effect_columns.append(column)

    numbers = sorted(levels)
    if numbers and numbers != list(range(1, numbers[-1] + 1)):
        warnings.append("levels not contiguous: %s" % numbers)

    lowered = section.lower()
    if "mana upgrade" in lowered:
        kind = "mana"
    elif any(word in lowered for word in ("upgrade", "construction", "requirements", "boost stats")):
        kind = "upgrade"
    else:
        kind = "other"
    return {
        "section": section,
        "kind": kind,
        "effect_columns": effect_columns,
        "levels": [levels[n] for n in numbers],
        "totals": totals,
        "warnings": warnings,
    }


def parse_building(wikitext):
    tables = []
    for match in re.finditer(r"\{\|(.*?)\n\|\}", wikitext, re.S):
        headings = re.findall(r"^=+\s*(.*?)\s*=+\s*$", wikitext[:match.start()], re.M)
        section = clean(headings[-1]) if headings else ""
        for rows in wikitables(match.group(0)):
            # some tables start with a row of groups ("Requirements" / "Results"): the header is the row
            # whose first cell is the level column
            start = next((i for i, row in enumerate(rows) if row and row[0][1] in LEVEL_HEADERS), None)
            if start is not None:
                tables.append(parse_table(rows[start:], section))

    upgrade = [t for t in tables if t["kind"] == "upgrade" and t["levels"]]
    mana = [t for t in tables if t["kind"] == "mana" and t["levels"]]

    # "... as long as the total number of Gym, Mystic Spire and Spring does not exceed 8"
    limit = re.search(r"total number of (.+?) does not exceed (\d+)", wikitext)
    shared = None
    if limit:
        names = [n.strip() for n in re.split(r",| and ", re.sub(r"\[\[(?:[^|\]]*\|)?([^\]]*)\]\]", r"\1", limit.group(1)))
                 if n.strip()]
        shared = {"buildings": names, "max": int(limit.group(2))}

    gallery = re.search(r"<gallery>(.*?)</gallery>", wikitext, re.S)
    pictures = []
    if gallery:
        for line in gallery.group(1).strip().splitlines():
            name, _, label = line.partition("|")
            if name.strip():
                pictures.append({"file": name.strip().replace(" ", "_"), "levels": label.strip() or None})

    return {
        "description": intro(wikitext),
        "max_level": max((t["levels"][-1]["level"] for t in upgrade), default=0),
        "mana_levels": max((t["levels"][-1]["level"] for t in mana), default=0),
        "shared_limit": shared,
        "pictures": pictures,
        "tables": tables,
        "warnings": [w for t in tables for w in t["warnings"]],
    }


def main():
    parser = argparse.ArgumentParser(description="Import the buildings from the Lords Mobile wiki.")
    parser.add_argument("--out-dir", default="gamedata", help="output folder (default: gamedata)")
    args = parser.parse_args()

    print("Reading the building categories...")
    _, subcategories = category_members("Buildings")
    families, wanted = {}, []
    for sub in subcategories:
        key = sub.replace(" ", "_")
        if key not in FAMILIES:
            print("  unknown sub-category %r, skipped" % sub)
            continue
        pages, _ = category_members(key)
        for title in pages:
            families[title] = FAMILIES[key]
            wanted.append(title)
        print("  %-10s %d buildings" % (FAMILIES[key], len(pages)))
    if not wanted:
        sys.exit("No building found: the wiki's Category:Buildings changed, check the tool.")

    print("Reading %d building pages..." % len(wanted))
    texts = fetch_wikitext(sorted(wanted))

    buildings = []
    for title in sorted(wanted, key=lambda t: (list(FAMILIES.values()).index(families[t]), t)):
        entry = {"name": title, "page": title, "family": families[title]}
        if title not in texts:
            entry["warnings"] = ["page not found on the wiki"]
        else:
            entry.update(parse_building(texts[title]))
        game_id, status = KNOWN_GAME_IDS.get(title, (None, None))
        entry["game_id"] = game_id
        entry["game_id_status"] = status
        buildings.append(entry)

    files = {p["file"] for b in buildings for p in b.get("pictures", [])}
    print("Reading %d picture descriptions..." % len(files))
    images = fetch_image_info(files)
    missing = sorted(files - set(images))
    if missing:
        print("  %d pictures without a file description: %s" % (len(missing), ", ".join(missing[:8])))

    source = {
        "site": WIKI + "/wiki/Category:Buildings",
        "fetched": datetime.date.today().isoformat(),
        "tool": "tools/import_wiki_buildings.py",
        "license": "Text: CC BY-SA 3.0, Lords Mobile Wiki (Fandom) contributors. Pictures: game artwork, (c) IGG.",
        "note": "Edited by players: values that could not be read are null and listed in 'warnings'. "
                "game_id is the build_id of the game protocol, null until identified (docs/buildings.md).",
    }

    os.makedirs(args.out_dir, exist_ok=True)
    with open(os.path.join(args.out_dir, "buildings.json"), "w", encoding="utf-8") as out:
        out.write('{\n"source": %s,\n"buildings": [\n%s\n]\n}\n'
                  % (json.dumps(source, ensure_ascii=False),
                     ",\n".join(json.dumps(b, ensure_ascii=False) for b in buildings)))
    with open(os.path.join(args.out_dir, "building_images.json"), "w", encoding="utf-8") as out:
        out.write('{\n"source": %s,\n"images": {\n' % json.dumps(source, ensure_ascii=False))
        out.write(",\n".join("%s: %s" % (json.dumps(k), json.dumps(images[k])) for k in sorted(images)))
        out.write("\n}\n}\n")

    print("Done: %d buildings, %d with warnings, %d pictures."
          % (len(buildings), sum(1 for b in buildings if b.get("warnings")), len(images)))


if __name__ == "__main__":
    main()
