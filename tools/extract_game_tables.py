#!/usr/bin/env python3
"""Reads the research tables the game itself ships and writes what the bot needs.

The tables are Unity text assets in `Loading/Table.unity3d` (in the APK, and in the PC client's
`Lords Mobile PC_Data/Download/6000/Loading/`, which is the more recent copy). Names are in the
string bundles next to it (`String.unity3d` English, `StringFre.unity3d` French...). Record layouts
come from an Il2CppDumper dump of the game (docs/apk-analysis.md): TechKindTbl, TechDataTbl,
TechLevelTbl. Nothing is guessed: each table's size is checked against its record count.

    pip install UnityPy
    python3 tools/extract_game_tables.py \\
        --table   "C:/Lords Mobile PC/Game/Lords Mobile PC_Data/Download/6000/Loading/Table.unity3d" \\
        --strings "C:/Lords Mobile PC/Game/Lords Mobile PC_Data/Download/6000/Loading/StringFre.unity3d" \\
        --strings-en Downloads/apk/assets/Loading/String.unity3d        # optional, English

Writes gamedata/game_research.json (everything) and src/research_table.c (what the bot uses), and the same for the
buildings: gamedata/game_buildings.json and src/building_table.c (building types, per-level cost/duration, prerequisites).
"""
import argparse
import collections
import json
import os
import struct
import sys

try:
    import UnityPy
except ImportError:
    sys.exit("UnityPy is needed: pip install UnityPy")

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# English names the string bundle lacks (the APK's English strings predate the category): the game's own tab name.
KIND_NAME_EN_FALLBACK = {18: "Guild Duel"}


def text_assets(path):
    """name -> bytes of every TextAsset of a bundle."""
    out = {}
    for obj in UnityPy.load(path).objects:
        if obj.type.name != "TextAsset":
            continue
        d = obj.read()
        s = d.m_Script
        out[d.m_Name] = s if isinstance(s, bytes) else s.encode("utf-8", "surrogateescape")
    return out


def records(assets, name, size):
    """Table = u16 version, u16 record count, then fixed-size records."""
    b = assets[name]
    _ver, count = struct.unpack("<HH", b[:4])
    if len(b) != 4 + count * size:
        sys.exit(f"{name}: {len(b)} bytes is not 4 + {count} x {size}, the layout changed")
    return [b[4 + i * size:4 + (i + 1) * size] for i in range(count)]


def string_lookup(path):
    """id -> text. StringTable = (u32 size of the index, then (offset, length) pairs) + the text blob;
    StringTable2 = u16 per string id, the 1-based index of that id in the first table."""
    a = text_assets(path)
    main, idx = a["StringTable"], a["StringTable2"]
    index_size = struct.unpack("<I", main[:4])[0]
    base = 4 + index_size
    strings = []
    for i in range(index_size // 8):
        off, ln = struct.unpack("<II", main[4 + 8 * i:12 + 8 * i])
        strings.append(main[base + off:base + off + ln].decode("utf-8", "replace"))
    ids = struct.unpack("<%dH" % ((len(idx) - 4) // 2), idx[4:4 + (len(idx) - 4) // 2 * 2])

    def get(string_id):
        if string_id < len(ids) and 0 < ids[string_id] <= len(strings):
            return strings[ids[string_id] - 1]
        return None
    return get


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--table", required=True, help="Table.unity3d")
    ap.add_argument("--strings", required=True, help="String bundle of the language the bot talks (French: StringFre.unity3d)")
    ap.add_argument("--strings-en", help="English String.unity3d (fallback for names, wiki names)")
    args = ap.parse_args()

    tables = text_assets(args.table)
    fr = string_lookup(args.strings)
    en = string_lookup(args.strings_en) if args.strings_en else (lambda i: None)

    # TechKindSP2 / TechSP / TechLv are the tables the game reads for its current 16 categories
    # (the un-suffixed Tech* are the first 8 categories only, older layouts of the same data).
    kinds = {}
    for r in records(tables, "TechKindSP2", 34):
        kind, name, _gr, order, academy = struct.unpack("<HHBBB", r[:7])
        kinds[kind] = dict(kind=kind, order=order, academy=academy, name_fr=fr(name), name_en=en(name) or KIND_NAME_EN_FALLBACK.get(kind))

    techs = {}
    for r in records(tables, "TechSP", 26):
        tid, kind, name, icon, max_level, locked, locked_ui = struct.unpack("<HBHHBBB", r[:10])
        techs[tid] = dict(id=tid, kind=kind, name_fr=fr(name), name_en=en(name), icon=icon,
                          max_level=max_level, locked=bool(locked), locked_ui=bool(locked_ui), levels=[])

    for r in records(tables, "TechLv", 52):
        (_id, tid, level, time, food, stone, wood, ore, gold, academy, r1, l1, r2, l2, r3, l3, r4, l4,
         might, effect, value) = struct.unpack("<HHBIIIIIIBHBHBHBHBIHI", r)
        req = [[a, b] for a, b in ((r1, l1), (r2, l2), (r3, l3), (r4, l4)) if a]
        techs[tid]["levels"].append(dict(level=level, time=time, food=food, stone=stone, wood=wood, ore=ore,
                                         gold=gold, academy=academy, might=might, effect=effect, value=value, req=req))

    for t in techs.values():
        t["levels"].sort(key=lambda x: x["level"])
        if [x["level"] for x in t["levels"]] != list(range(1, t["max_level"] + 1)):
            sys.exit(f"research {t['id']}: level rows do not match its maximum level")

    ordered = sorted(kinds.values(), key=lambda k: k["order"])
    out = {
        "source": "Tables shipped with the Lords Mobile client (TechKindSP2, TechSP, TechLv), names from its string tables. "
                  "time is the base duration in seconds, before any speed bonus.",
        "kinds": ordered,
        "techs": [techs[i] for i in sorted(techs)],
    }
    path = os.path.join(ROOT, "gamedata", "game_research.json")
    with open(path, "w", encoding="utf-8") as f:
        json.dump(out, f, ensure_ascii=False, separators=(",", ":"))
    print(f"{path}: {len(techs)} researches, {sum(len(t['levels']) for t in techs.values())} levels, {len(kinds)} categories")

    write_c(techs, ordered)
    extract_buildings(tables, fr, en)


def c_str(s):
    if s is None:
        return "NULL"
    return '"' + s.replace("\\", "\\\\").replace('"', '\\"').replace("\n", " ") + '"'


def write_c(techs, kinds):
    rows = []      # flat level rows, techs in id order
    first_row = {}
    for tid in sorted(techs):
        first_row[tid] = len(rows)
        rows.extend((tid, lv) for lv in techs[tid]["levels"])

    kind_name = {k["kind"]: k for k in kinds}
    lines = [
        "/* GENERATED by tools/extract_game_tables.py from the game's own tables - do not edit by hand.",
        " * Research categories, researches and per-level cost/duration/prerequisites: see research_table.h. */",
        '#include "research_table.h"',
        "",
        "const ResearchKindInfo RESEARCH_KINDS[] = {",
    ]
    for k in kinds:
        lines.append(f"\t{{ {k['kind']}, {k['order']}, {k['academy']}, {c_str(k['name_fr'])}, {c_str(k['name_en'] or k['name_fr'])} }},")
    lines += ["};", f"const uint16_t RESEARCH_KIND_COUNT = {len(kinds)};", "", "const ResearchLevelInfo RESEARCH_LEVELS[] = {"]
    for _tid, lv in rows:
        req = (lv["req"] + [[0, 0]] * 4)[:4]
        lines.append("\t{ %d, {%d,%d,%d,%d,%d}, %d, {%d,%d,%d,%d}, {%d,%d,%d,%d} }," % (
            lv["time"], lv["food"], lv["stone"], lv["wood"], lv["ore"], lv["gold"], lv["academy"],
            req[0][0], req[1][0], req[2][0], req[3][0], req[0][1], req[1][1], req[2][1], req[3][1]))
    lines += ["};", "", "const ResearchTechInfo RESEARCH_TECHS[] = {"]
    for tid in sorted(techs):
        t = techs[tid]
        name_en = t["name_en"] or t["name_fr"]
        label = f"{kind_name[t['kind']]['name_en'] or kind_name[t['kind']]['name_fr']}: {name_en}"
        lines.append("\t{ %d, %d, %d, %d, %d, %s, %s, %s }," % (
            tid, t["kind"], t["max_level"], 1 if t["locked"] else 0, first_row[tid],
            c_str(t["name_fr"]), c_str(name_en), c_str(label)))
    lines += ["};", f"const uint16_t RESEARCH_TECH_COUNT = {len(techs)};"]
    if sorted(techs) != list(range(1, len(techs) + 1)):
        sys.exit("research ids are not 1..N: ResearchTech() looks them up by index")
    path = os.path.join(ROOT, "src", "research_table.c")
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(lines) + "\n")
    print(f"{path}: {len(kinds)} categories, {len(techs)} researches, {len(rows)} level rows")


# ------------------------------------------------------------------ buildings

# The variant with the mana levels and every building ("_MB"; the plain and "_M" ones differ by two level-1 rows and a cost).
BUILD_TYPE_TABLE, BUILD_LEVEL_TABLE = "buildkind_new_MB", "buildUP_NEW_MB"
# BuildLevelRequest, packed (the file is not laid out like the struct in the dump): ID, BuildID, Level, BuildTime, GroupID,
# then 8 costs (food, stone, wood, ore, gold, mana ore, mana crystal, manasteel - checked against the wiki's Castle mana level 1:
# 23,029 / 242 / 5,333,046 / 7,272,336 ...), army, might, effects.
BUILD_LEVEL_FORMAT = "<HHBIH" + "8I" + "II" + "HI" + "HI"
BUILD_REQ_MAX = 9      # the most buildings one level asks for (the Academy)
COST_NAMES = ("food", "stone", "wood", "ore", "gold", "mana_ore", "mana_crystal", "manasteel")


def extract_buildings(tables, fr, en):
    kinds = {}
    for r in records(tables, BUILD_TYPE_TABLE, 29):
        bid, name, kind, _graphic, _sid, _cid, _ui, _side, upgradeable, movable = struct.unpack("<HHBBHHHBBB", r[:15])
        kinds[bid] = dict(id=bid, kind=kind, name_fr=fr(name), name_en=en(name), upgradeable=bool(upgradeable),
                          movable=bool(movable), levels=[])

    groups = collections.defaultdict(list)          # GroupID -> [(condition type, condition, number)]
    for r in records(tables, "BuildRequestGroup", 9):
        _id, group, ctype, cond, num = struct.unpack("<HHBHH", r)
        groups[group].append((ctype, cond, num))

    size = struct.calcsize(BUILD_LEVEL_FORMAT)
    for r in records(tables, BUILD_LEVEL_TABLE, 106):
        v = struct.unpack(BUILD_LEVEL_FORMAT, r[:size])
        _id, bid, level, time, group = v[:5]
        conds = groups.get(group, [])
        building = [[c, n] for t, c, n in conds if t == 1]      # a building of that type at that level
        research = [[c, n] for t, c, n in conds if t == 3]      # a research at that level
        quests = [c for t, c, n in conds if t == 2]             # a step of the game's own quests: not visible to the bot
        if len(building) > BUILD_REQ_MAX or len(research) > 1 or len(quests) > 2 or any(t not in (1, 2, 3) for t, _, _ in conds):
            sys.exit(f"building {bid} level {level}: a prerequisite shape this tool does not know")
        kinds[bid]["levels"].append(dict(level=level, time=time, cost=dict(zip(COST_NAMES, v[5:13])), army=v[13], might=v[14],
                                         req_building=building, req_research=research, req_quest=quests))

    for t in kinds.values():
        t["levels"].sort(key=lambda x: x["level"])
        levels = [x["level"] for x in t["levels"]]
        # the Mana Lode and the Mana Chamber are built at the mana stage: their first level is 25, not 1
        t["min_level"] = levels[0] if levels else 0
        t["max_level"] = levels[-1] if levels else 0
        if levels and levels != list(range(levels[0], levels[-1] + 1)):
            sys.exit(f"building {t['id']}: level rows are not contiguous")

    out = {
        "source": "Tables shipped with the Lords Mobile client (buildkind_new_MB, buildUP_NEW_MB, BuildRequestGroup), names from its string "
                  "tables. Levels count the mana levels (25 + 5 per mana level, up to 55). time is the base duration in seconds. "
                  "req_building: a building of that type must be at that level; req_research: a research at that level; req_quest: a "
                  "step of the game's quests, not visible to the bot.",
        "types": [kinds[i] for i in sorted(kinds)],
    }
    path = os.path.join(ROOT, "gamedata", "game_buildings.json")
    with open(path, "w", encoding="utf-8") as f:
        json.dump(out, f, ensure_ascii=False, separators=(",", ":"))
    print(f"{path}: {len(kinds)} building types, {sum(len(t['levels']) for t in kinds.values())} levels")
    write_buildings_c(kinds)


def write_buildings_c(kinds):
    lines = [
        "/* GENERATED by tools/extract_game_tables.py from the game's own tables - do not edit by hand.",
        " * Building types, per-level cost/duration/prerequisites: see building_table.h. */",
        '#include "building_table.h"',
        "",
        "const BuildingLevelInfo BUILDING_LEVELS[] = {",
    ]
    first, row = {}, 0
    for bid in sorted(kinds):
        first[bid] = row
        for lv in kinds[bid]["levels"]:
            rb = (lv["req_building"] + [[0, 0]] * BUILD_REQ_MAX)[:BUILD_REQ_MAX]
            rr = (lv["req_research"] + [[0, 0]])[0]
            c = lv["cost"]
            lines.append("\t{ %d, {%s}, {%s}, {%s}, %d, %d, %d }," % (
                lv["time"], ",".join(str(c[n]) for n in COST_NAMES),
                ",".join(str(x[0]) for x in rb), ",".join(str(x[1]) for x in rb),
                rr[0], rr[1], (lv["req_quest"] + [0])[0]))
            row += 1
    lines += ["};", "", "const BuildingTypeInfo BUILDING_TYPES[] = {"]
    for bid in sorted(kinds):
        t = kinds[bid]
        lines.append("\t{ %d, %d, %d, %d, %d, %d, %s, %s }," % (
            bid, t["kind"], t["min_level"], t["max_level"], 1 if t["upgradeable"] else 0, first[bid],
            c_str(t["name_fr"]), c_str(t["name_en"] or t["name_fr"])))
    lines += ["};", f"const uint16_t BUILDING_TYPE_COUNT = {len(kinds)};"]
    path = os.path.join(ROOT, "src", "building_table.c")
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(lines) + "\n")
    print(f"{path}: {len(kinds)} types, {row} level rows")


if __name__ == "__main__":
    main()
