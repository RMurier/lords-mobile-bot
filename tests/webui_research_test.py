"""Tests of the research and construction tabs of the web console: their settings against the game's tables, the settings the C bot reads.

Run from the repository root (standard library only):

    python3 tests/webui_research_test.py
"""

import json
import sys
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO / "webui"))

import schema  # noqa: E402

TABLE = json.loads((REPO / "gamedata" / "game_research.json").read_text(encoding="utf-8"))
BUILDINGS = json.loads((REPO / "gamedata" / "game_buildings.json").read_text(encoding="utf-8"))


def field(key):
    for category in schema.CATEGORIES:
        for f in category["fields"]:
            if f["key"] == key:
                return f
    raise KeyError(key)


class ResearchSettings(unittest.TestCase):
    def test_categories_are_the_games_own_in_the_order_of_its_tabs(self):
        kinds = sorted(TABLE["kinds"], key=lambda k: k["order"])
        self.assertEqual([name for name, _ in schema.RESEARCH_CATEGORIES], [k["name_en"] for k in kinds])
        self.assertEqual(len(kinds), 16)

    def test_every_setting_is_read_by_the_bot(self):
        config_c = (REPO / "src" / "config.c").read_text(encoding="utf-8")
        research = next(c for c in schema.CATEGORIES if c["id"] == "research")
        for f in research["fields"]:
            self.assertIn(f'"{f["key"]}"', config_c, f["key"])

    def test_categories_validation(self):
        f = field("research.categories")
        self.assertEqual(schema.validate(f, "Sigils,  Gear"), "Sigils, Gear")
        self.assertEqual(schema.validate(f, ""), "")
        for bad in ("Foo", "Gear, Gear", "Sigils, Foo"):
            with self.assertRaises(ValueError, msg=bad):
                schema.validate(f, bad)

    def test_reserve_and_switch(self):
        self.assertEqual(schema.validate(field("research.reserve_gold"), "5m"), "5M")
        self.assertEqual(schema.validate(field("research.enabled"), "true"), "true")

    def test_table_is_complete(self):
        self.assertEqual(len(TABLE["techs"]), 403)
        self.assertEqual(sum(len(t["levels"]) for t in TABLE["techs"]), 3425)
        by_id = {t["id"]: t for t in TABLE["techs"]}
        self.assertEqual(by_id[126]["name_en"], "Tax Break")
        self.assertEqual(by_id[221]["max_level"], 10)

    def test_the_status_carries_what_the_console_reads(self):
        status_c = (REPO / "src" / "status.c").read_text(encoding="utf-8")
        for key in ("research", "in_progress", "remaining", "academy", "levels", "auto", "kinds", "state"):
            self.assertIn('\\"%s\\"' % key, status_c, key)
        app_js = (REPO / "webui" / "static" / "app.js").read_text(encoding="utf-8")
        for key in ("R.levels", "R.in_progress", "R.remaining", "R.academy", "auto.kinds", "auto.state"):
            self.assertIn(key, app_js)


class BuildSettings(unittest.TestCase):
    def test_buildings_are_the_ones_the_bot_can_work_on(self):
        selectable = [t for t in BUILDINGS["types"]
                      if t["upgradeable"] and t["max_level"] > t["min_level"] and t["kind"] <= 3 and t["name_en"]]
        self.assertEqual([name for name, _ in schema.BUILD_BUILDINGS], [t["name_en"] for t in selectable])
        self.assertEqual([label for _, label in schema.BUILD_BUILDINGS], [t["name_fr"] for t in selectable])

    def test_every_setting_is_read_by_the_bot(self):
        config_c = (REPO / "src" / "config.c").read_text(encoding="utf-8")
        build = next(c for c in schema.CATEGORIES if c["id"] == "build")
        for f in build["fields"]:
            self.assertIn(f'"{f["key"]}"', config_c, f["key"])

    def test_validation(self):
        f = field("build.buildings")
        self.assertEqual(schema.validate(f, "Castle,  Barracks"), "Castle, Barracks")
        self.assertEqual(schema.validate(f, ""), "")
        for bad in ("Residence", "Castle, Castle", "Foo"):
            with self.assertRaises(ValueError, msg=bad):
                schema.validate(f, bad)

    def test_table_is_complete(self):
        by_id = {t["id"]: t for t in BUILDINGS["types"]}
        self.assertEqual(len(by_id), 44)
        self.assertEqual(sum(len(t["levels"]) for t in BUILDINGS["types"]), 1375)
        # the numbers the docs listed as unidentified
        self.assertEqual({i: by_id[i]["name_en"] for i in (11, 18, 19, 20, 21, 22, 23, 24, 27)},
                         {11: "Battle Hall", 18: "Prison", 19: "Altar", 20: "Monsterhold", 21: "Spring", 22: "Mystic Spire",
                          23: "Gym", 24: "Lunar Foundry", 27: "Mana Chamber"})
        # the Castle's first mana level, as the wiki gives it
        row = next(r for r in by_id[8]["levels"] if r["level"] == 26)
        self.assertEqual((row["time"], row["cost"]["food"], row["cost"]["mana_ore"], row["cost"]["mana_crystal"]),
                         (2649349, 5333046, 23029, 242))

    def test_the_status_carries_what_the_console_reads(self):
        status_c = (REPO / "src" / "status.c").read_text(encoding="utf-8")
        for key in ("build", "buildings", "queue", "remaining", "auto", "types", "plan", "state"):
            self.assertIn('\\"%s\\"' % key, status_c, key)
        app_js = (REPO / "webui" / "static" / "app.js").read_text(encoding="utf-8")
        for key in ("B.buildings", "B.queue", "auto.types", "auto.plan", "auto.state"):
            self.assertIn(key, app_js)

    def test_the_bag_shown_is_computed_from_the_items(self):
        # it used to print a field nothing filled: the console's "Sac" was always 0
        status_c = (REPO / "src" / "status.c").read_text(encoding="utf-8")
        self.assertIn("BagTotal(c, RESOURCE_ROCK)", status_c)
        self.assertNotIn("c->bag_resources.food", status_c)


if __name__ == "__main__":
    unittest.main()
