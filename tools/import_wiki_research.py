#!/usr/bin/env python3
"""
Import the research tree from the Lords Mobile fandom wiki into gamedata/.

The research window's data (names, effects, levels, costs, prerequisites) is not sent by the game
server, only by the client's own files, so it cannot be read from the network (docs/research.md).
The wiki lists it, and its public MediaWiki API answers plain HTTP clients, so this tool reads:

    https://lordsmobile.fandom.com/wiki/Research      -> the 16 categories
    https://lordsmobile.fandom.com/wiki/<category>    -> the grid of researches (row, column, icon)
    https://lordsmobile.fandom.com/wiki/<research>    -> per level: effect, might, prerequisites,
                                                         duration, Technolabe, resource costs

and writes:

    gamedata/research.json         the data
    gamedata/research_images.json  where each icon is (url, size), for tools/fetch_research_images.py

Usage, from the repository root (standard library only, about 40 requests, one per second):

    python3 tools/import_wiki_research.py

The wiki is edited by players: some values have typos (a cost written "503,9695", a level 25 in
a list that stops at 15...). A value that cannot be read without guessing is stored as null and
listed in that research's "warnings" instead of being corrected here. Check those in the game.

The wiki text is licensed CC BY-SA 3.0 (see the wiki's page footer); the icons are game artwork
that belongs to IGG. Keep the attribution written in the "source" block of the output.
"""

import argparse
import collections
import datetime
import html
import json
import os
import re
import sys
import time
import urllib.parse

from wiki_common import (WIKI, api, clean, fetch_image_info, fetch_wikitext, intro, requirements,
                         to_int, to_seconds, wikitables)

# The order the game shows the categories in (the wiki's Research page lists them the same way).
COST_COLUMNS = {
    "Food": "food", "Stone": "stone", "Timber": "timber", "Ore": "ore", "Gold": "gold",
    # what some researches cost besides the five resources
    "Archaic Tome": "archaic_tome", "Anima": "anima", "Mana Ore": "mana_ore", "Mana ore": "mana_ore",
    "Mana Crystal": "mana_crystal",
}
# Protocol id of the researches identified so far, by wiki page. "confirmed": the name was read in the
# game's own UI next to the id (include/tech_research.h). "inferred": deduced, see include/research.h.
# The ids the game sends (levels, research in progress) are not on the wiki: docs/research.md.
KNOWN_GAME_IDS = {
    "Construction Speed": (6, "confirmed"),
    "Gem Harvesting I": (8, "confirmed"),
    "Energy Recovery I": (74, "confirmed"),
    "More Gatherers": (123, "confirmed"),
    "Bigger Bags I": (125, "confirmed"),
    "Tax Break": (126, "inferred"),
    "Gold Storage I": (143, "confirmed"),
    "Gem Harvesting II": (229, "confirmed"),
    "Bigger Bags II": (234, "confirmed"),
    "Barracks Expansion II": (299, "confirmed"),
    "Ration Run IV": (301, "confirmed"),
    "Forced March III": (302, "confirmed"),
    "Bigger Bags III": (303, "confirmed"),
    "Quick Maneuvers III": (305, "confirmed"),
}

# The wiki does not name a column the same way on every page.
REQUIREMENT_COLUMNS = ("Requirements", "Requires", "Requirement")
TIME_COLUMNS = ("Orig. Time", "Orig. Research Time")
TECHNOLABE_COLUMNS = ("Technolabe", "Technolabes")


# --------------------------------------------------------------------------
# One research page
# --------------------------------------------------------------------------

def parse_research(wikitext):
    warnings, levels, effect_columns, totals = [], collections.OrderedDict(), [], {}

    for rows in wikitables(wikitext):
        if not rows or not rows[0] or rows[0][0][1] != "Level":
            continue
        header = [text for _, text in rows[0]]

        for row in rows[1:]:
            values = [text for _, text in row]
            if values[0].lower().startswith(("total", "sum")):
                totals.update(dict(zip(header, values)))
                continue
            if row[0][0] == "h":
                continue

            level = to_int(values[0])
            if level is None:
                warnings.append("level not a number: %r" % values[0])
                continue
            entry = levels.setdefault(level, {"level": level, "effects": {}, "cost": {}})
            if len(values) != len(header):
                warnings.append("level %d: %d cells for %d columns" % (level, len(values), len(header)))

            for column, value in zip(header[1:], values[1:]):
                if column == "Might":
                    entry["might"] = to_int(value)
                    if entry["might"] is None and value:
                        warnings.append("level %d: might %r" % (level, value))
                elif column in REQUIREMENT_COLUMNS:
                    entry["requirements"] = requirements(value)
                    entry["requirements_text"] = value
                elif column in TIME_COLUMNS:
                    entry["time"] = to_seconds(value)
                    if entry["time"] is None and value:
                        warnings.append("level %d: time %r" % (level, value))
                elif column in TECHNOLABE_COLUMNS:
                    entry["technolabe"] = to_int(value)
                elif column in COST_COLUMNS:
                    entry["cost"][COST_COLUMNS[column]] = to_int(value)
                    if entry["cost"][COST_COLUMNS[column]] is None and value:
                        warnings.append("level %d: %s cost %r" % (level, column, value))
                elif column:
                    entry["effects"][column] = value
                    if column not in effect_columns:
                        effect_columns.append(column)

    numbers = sorted(levels)
    if numbers and numbers != list(range(1, numbers[-1] + 1)):
        warnings.append("levels not contiguous: %s" % numbers)

    return {
        "description": intro(wikitext),
        "max_level": len(numbers),
        "effect_columns": effect_columns,
        "levels": [levels[n] for n in numbers],
        "totals": totals,
        "warnings": warnings,
    }


# --------------------------------------------------------------------------
# Category pages
# --------------------------------------------------------------------------

def parse_category(page_html):
    """The grid of a category page: [(row, column, research title, icon file)]."""
    cells = []
    tables = re.findall(r"<table.*?</table>", page_html, re.S)
    for table in tables:
        for row_index, row in enumerate(re.findall(r"<tr.*?</tr>", table, re.S)):
            for column_index, cell in enumerate(re.findall(r"<td[^>]*>(.*?)</td>", row, re.S)):
                link = re.search(r'<a href="/wiki/([^"#]+)" title="([^"]+)"><img[^>]*data-image-key="([^"]+)"', cell)
                if link:
                    cells.append((row_index, column_index, html.unescape(link.group(2)),
                                  urllib.parse.unquote(link.group(3)).replace(" ", "_")))
    return cells


def category_intro(page_html):
    text = re.sub(r"<aside.*?</aside>", "", page_html, flags=re.S)
    text = text.split('id="Available_Research"')[0]
    text = re.sub(r"<[^>]+>", " ", text)
    return re.sub(r"\s+", " ", html.unescape(text)).strip()


def main():
    parser = argparse.ArgumentParser(description="Import the research tree from the Lords Mobile wiki.")
    parser.add_argument("--out-dir", default="gamedata", help="output folder (default: gamedata)")
    args = parser.parse_args()

    print("Reading the research tree...")
    tree = api(action="parse", page="Research", prop="links")
    names = [link["*"] for link in tree["parse"]["links"] if link.get("ns") == 0]
    try:
        first, last = names.index("Economy"), names.index("Guild Duel")
    except ValueError:
        sys.exit("The wiki's Research page no longer lists Economy .. Guild Duel: it changed, check the tool.")
    category_names = names[first:last + 1]
    print("  %d categories: %s" % (len(category_names), ", ".join(category_names)))

    categories, cells = [], []
    for order, name in enumerate(category_names, 1):
        page = api(action="parse", page=name.replace(" ", "_"), prop="text")["parse"]["text"]["*"]
        grid = parse_category(page)
        image = re.search(r"<aside.*?data-image-key=\"([^\"]+)\"", page, re.S)
        categories.append({
            "order": order, "name": name,
            "image": urllib.parse.unquote(image.group(1)).replace(" ", "_") if image else None,
            "description": category_intro(page),
            "research_count": len(grid),
            # the game has this category too, but the wiki page is about the event, not a tree
            "wiki_tree": bool(grid),
        })
        for row, column, title, icon in grid:
            cells.append((name, row, column, title, icon))
        print("  %-24s %d researches" % (name, len(grid)))

    titles = sorted({title for _, _, _, title, _ in cells})
    print("Reading %d research pages..." % len(titles))
    texts = fetch_wikitext(titles)

    researches, parsed = [], {}
    for category, row, column, title, icon in cells:
        entry = {"category": category, "row": row, "col": column, "name": title, "page": title, "image": icon}
        if title not in texts:
            entry["warnings"] = ["page not found on the wiki"]
        else:
            if title not in parsed:
                parsed[title] = parse_research(texts[title])
                if not parsed[title]["levels"]:
                    parsed[title]["warnings"].append("no per-level table on the wiki page")
            entry.update(parsed[title])
        # the game's own number for this research (protocol id), null until identified
        game_id, status = KNOWN_GAME_IDS.get(title, (None, None))
        entry["game_id"] = game_id
        entry["game_id_status"] = status
        researches.append(entry)

    files = {c["image"] for c in categories if c["image"]} | {r["image"] for r in researches}
    print("Reading %d image descriptions..." % len(files))
    images = fetch_image_info(files)
    missing = sorted(files - set(images))
    if missing:
        print("  %d icons without a file description: %s" % (len(missing), ", ".join(missing[:8])))

    today = datetime.date.today().isoformat()
    source = {
        "site": WIKI + "/wiki/Research",
        "fetched": today,
        "tool": "tools/import_wiki_research.py",
        "license": "Text: CC BY-SA 3.0, Lords Mobile Wiki (Fandom) contributors. Icons: game artwork, (c) IGG.",
        "note": "Edited by players: values that could not be read are null and listed in 'warnings'. "
                "game_id is the number the game protocol uses, null until identified; game_id_status says whether it is confirmed or inferred (docs/research.md).",
    }

    os.makedirs(args.out_dir, exist_ok=True)
    with open(os.path.join(args.out_dir, "research.json"), "w", encoding="utf-8") as out:
        out.write('{\n"source": %s,\n' % json.dumps(source, ensure_ascii=False))
        out.write('"categories": [\n%s\n],\n' % ",\n".join(json.dumps(c, ensure_ascii=False) for c in categories))
        out.write('"researches": [\n%s\n]\n}\n' % ",\n".join(json.dumps(r, ensure_ascii=False) for r in researches))

    with open(os.path.join(args.out_dir, "research_images.json"), "w", encoding="utf-8") as out:
        out.write('{\n"source": %s,\n"images": {\n' % json.dumps(source, ensure_ascii=False))
        out.write(",\n".join("%s: %s" % (json.dumps(k), json.dumps(images[k])) for k in sorted(images)))
        out.write("\n}\n}\n")

    full = [r for r in researches if "levels" in r]
    print("Done: %d researches (%d distinct pages), %d with warnings, %d icons."
          % (len(researches), len(parsed), sum(1 for r in full if r["warnings"]), len(images)))


if __name__ == "__main__":
    main()
