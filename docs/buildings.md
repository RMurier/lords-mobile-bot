# Buildings

What the bot knows about buildings: how the game numbers their levels, which building is which number,
the wiki's table of every building, and what the Trading Post's level gives. Code:
[`include/buildings.h`](../include/buildings.h) (level scheme, Trading Post capacity and tax) and
`RecvAllBuildData()` in `src/protocol.c`. The researches have their own page: [research.md](research.md).

Everything marked *confirmed* comes from packet captures of the official PC client on two accounts, or from
what the player who owns them read in the game. The wiki table is a different source, kept apart.

## What the server sends

`_MSG_RESP_BUILDINGINFO` (2001), once, at login:

| Offset | Size | Field |
|---|---|---|
| 0 | u8 | number of buildings `n` |
| 1 + 5 i | u16 | slot (where the building stands) |
| 3 + 5 i | u16 | `build_id`: which building it is |
| 5 + 5 i | u8 | **level** |

69 buildings on the account with Castle 35, 68 on the one with Castle 25, 36 different `build_id` (1 to 27, and
100 to 112).

## Levels: 25, then the Mana upgrade

*Confirmed* by the player and by a capacity read in the game (below). The level is ONE number and goes on after
25, the normal maximum, because the **Mana upgrade** comes next: 6 mana levels, each reached in **5 steps**
(the wiki: "you need to build it 5 times at the current stage").

| Level sent | Reads as |
|---|---|
| 25 | normal maximum, no mana |
| 26 to 29 | still mana 0, 1 to 4 steps done toward mana 1 |
| **30** | **mana 1** |
| 32 | mana 1, 2 steps of 5 done toward mana 2 |
| 34 | still mana 1 (4 steps done) |
| **35** | **mana 2** ("level 25 + mana 2") |
| 55 | mana 6, the maximum (25 + 6 x 5) |

The player also gave the Castle Wall at 33: mana 1 + 3/5 toward mana 2, and the Mana Lode being built from mana 1 to mana 1 + 1/5
(30 to 31): both match.

`mana level = (level - 25) / 5`, steps toward the next `(level - 25) % 5`; a level only counts once reached, so 34
is still mana 1. `BuildingManaLevel()`, `BuildingManaSteps()` and `BuildingNormalLevel()` in `buildings.h`.
Two buildings have no Mana upgrade: the Watchtower (25 on both accounts) and the Treasure Trove (9 levels). The Mana Lode follows
the same numbering (25 on the Castle 25 account, 30 then 31 on the other): the 6 rows of its wiki page ("Castle Mana lv N" as
requirement) are its 6 mana levels, not levels 1 to 6 of the building.
The bot logs a level as `niveau 32 (25 + mana 1, 2/5 vers mana 2)`.

## Which building is which number

| `build_id` | Building | Status |
|---|---|---|
| 1, 2, 3, 4 | Lumber Mill, Quarry, Mine, Farm | confirmed (the bot's names) |
| 5, 6, 7 | Manor, Barracks, Infirmary | confirmed (the player: the building he owns 12 times at different levels is the Manor, the one owned 4 times is the Infirmary) |
| 8, 9, 10 | Castle, Vault, Academy | confirmed |
| 12, 13, 14, 15 | Castle Wall, Watchtower, Embassy, Workshop | confirmed |
| 17 | Trading Post | confirmed |
| **26** | **Mana Lode** | **confirmed**: the player's building under construction (mana 1 to mana 1 + 1/5) is `build_id 26` in the construction packet (below) |
| **16** | **Treasure Trove** | **inferred**: the only building with 9 levels, and the accounts have it at level 9 and level 4 |

Not identified yet (nine ids for nine wiki buildings), with what the two accounts show (account 1 = Castle 35, account 2 = Castle 25):

| `build_id` | Account 1 | Account 2 | Slot |
|---|---|---|---|
| 11 | 1 at 30 | 1 at 20 | same |
| 18 | 1 at 35 | 1 at 25 | same |
| 19 | 1 at 35 | 1 at 25 | different |
| 20 | 1 at 35 | 1 at 24 | same |
| 21 | 5: 30, 24, 24, 19, 16 | 1 at 24 | |
| 22 | 2: 30, 24 | 6 at 24 | |
| 23 | 1 at 30 | 1 at 24 | different |
| 24 | 1 at 30 | **absent** | |
| 27 | 1 at 30 | 1 at 25 | same |
| 100, 101, 102, 105, 106, 107, 109, 110, 111, 112 | level 1 | level 1 | 100, 109, 110, 111 same |

against the nine wiki buildings still without a number: Altar, Battle Hall, Gym, Lunar Foundry, Mana Chamber, Monsterhold,
Mystic Spire, Prison, Spring.

What can be said without guessing:

- The wiki says the **Gym, Mystic Spire and Spring share a limit of 8**. Ids 21 and 22 are two of them, and each account
  ends up at exactly 8 with one more building of the third kind (account 1: 5 + 2 + 1, account 2: 1 + 6 + 1). So a third id
  with one building on both accounts is the third of the three. Which id and which name is not known.
- Id 24 exists on account 1 only: it is a building account 2 has not unlocked (the wiki says the Lunar Foundry is unlocked by a
  research). Which one is not known.
- The levels alone do not tell them apart: (30, 20) is unique, but 18 and 19 share (35, 25).
- Ids 100 to 112 are level 1 on both accounts; none of the wiki's buildings stays at level 1, so they are probably scenery
  or fixed objects of the turf, not buildings of the list. Not identified.

**How the ids get identified**: the game never shows them, the server sends them when a building changes. So at every
login the bot compares the buildings with the ones saved at the previous login (`<data.path>/building_levels.bin`) and
logs each one built, upgraded or removed:

    [BUILD] #24 (emplacement 62716) : niveau 30 (25 + mana 1, 0/5 vers mana 2) -> niveau 31 (...) depuis la dernière connexion
    [BUILD] #22 (emplacement 9000) : construit, niveau 1

Anyone who upgrades a building between two logins and notes which one gives one id per line. Add the pair to
`KNOWN_GAME_IDS` in `tools/import_wiki_buildings.py` and run it again. Or, faster: for each of the ten wiki buildings above,
the level the game shows on **both** accounts (as "25 + mana N, k/5"), which narrows it to the few ids of the same signature.

## What is being built

`_MSG_RESP_BUILDINGEVENT` (2002), at login, 44 bytes: 2 entries of 18 bytes, then 8 bytes (an `i64` that is `INT64_MAX` in every
capture, meaning unknown).

| Offset | Size | Field |
|---|---|---|
| 0 | u8 | 1 when the entry is used, 0 when empty |
| 1 | u16 | slot, the same number BUILDINGINFO gives |
| 3 | u16 | `build_id` |
| 5 | u8 | level being built: the **target** level (current + 1) |
| 6 | u64 | start time, server clock, seconds |
| 14 | u32 | duration, seconds |

*Confirmed* on the account that was building its Mana Lode: `{ 1, slot 62967, build_id 26, level 31 }`, started 243 h before the
capture for 340.8 h, so about 4 days (97.5 h) left. The other account sends two empty entries. This is where `build_id 26` was read:
the same slot and number are in BUILDINGINFO, at level 30. Only seen at login; whether an update during the game has the same layout is not
known. The bot keeps it in `Connection.construction` and logs `[BUILD] En construction : #26 ...`.

## The Trading Post: capacity and tax

Both *confirmed* on the account at Trading Post 30 (mana 1) and the one at 25.

- **Capacity of one march** = the building + the three "Bigger Bags" researches. The building gives its level's amount
  (3,000,000 at 25) **plus 6,000 per mana step** (the wiki's Mana upgrade table): level 30 gives 3,000,000 + 5 x 6,000 =
  3,030,000. With Bigger Bags at 5, 0 and 10 (350,000 + 0 + 1,000,000) that is **4,380,000, exactly the total the game
  showed** when the building was clicked. The bot used to give 0 above level 25 (no delivery at all for that account) and
  ignored the researches; `RecomputeSupplyCapacity()` does both now, and again when a Bigger Bags finishes.
- **Supply tax** = what the building sets (30% at level 1 down to **8% at level 25**, the mana levels do not change it) minus
  0.1% per level of the Tax Break research (docs/research.md). The two accounts: Trading Post 30 and 25, both 8%, Tax Break
  5 and 4: 7.5% and 7.6%, as the game reported. The wiki's whole numbers below level 25 look rounded (30, 29, 29, 28, ...),
  so only the 8% is exact; the bot reads the real rate from the delivery report anyway.

## The buildings' table (from the wiki)

`gamedata/buildings.json` holds the 26 buildings of the [wiki's category](https://lordsmobile.fandom.com/wiki/Category:Buildings):

| Family | Buildings |
|---|---|
| Standard (7) | Academy, Castle, Embassy, Trading Post, Vault, Watchtower, Workshop |
| Military (4) | Barrack, Castle Wall, Infirmary, Manor |
| Resource (6) | Farm, Lumber Mill, Lunar Foundry, Mana Lode, Mine, Quarry |
| Advanced (5) | Altar, Battle Hall, Mana Chamber, Prison, Treasure Trove |
| Familiar (4) | Gym, Monsterhold, Mystic Spire, Spring |

It is produced by a tool, not typed:

```
python3 tools/import_wiki_buildings.py     # about 40 requests to the wiki's public API, one per second
```

For each building: the description, `max_level` (25 for all but the Treasure Trove, 9, and the Mana Lode and Mana Chamber, 6 rows that are mana levels),
`mana_levels` (6, none for the Watchtower, Treasure Trove, Mana Lode and Mana Chamber), the limit it shares with others
(`shared_limit`: Farm + Lumber Mill + Mine + Quarry at most 18, Barrack + Infirmary + Manor 17, Gym + Mystic Spire + Spring 8) and its
tables. Each table is one of the wiki's: `kind` `upgrade` (level 1 to 25), `mana` (one row per mana level) or `other`, and per level
the effect columns, `might`, `requirements` (e.g. `Castle Lv25`), `time` (seconds) and `cost` (`food`, `stone`, `timber`, `ore`, plus
`mana_ore`, `mana_crystal`, `manasteel`, `war_tome`, `steel_cuff`... where a building uses them). When the wiki gives one amount for two
resources in one column ("Stone/Timber") it is stored for both.

**How reliable it is.**

- **Cross-checked**: the Trading Post's capacity for the 25 levels is identical, level for level, to the table the bot already had; its
  mana step (6,000) and the tax at level 25 (8%) match what the accounts show.
- **Typos on the wiki**: 10 of the 26 pages have a value that cannot be read without guessing (a cost `17.5k`, a `-`, ranges such as `1-24`); it is
  `null` with the reason in `warnings`. Nothing is corrected. The Mana Chamber page is the worst (21 warnings).
- **The Mana upgrade tables are incomplete on the wiki**: several rows are empty (the Trading Post's mana levels 3, 5 and 6 have no cost).
- **Outdated numbers**: as for the researches, the durations and costs of the wiki predate the current game version; do not rely on them to
  identify a building from what the bot sees.

**Pictures.** `gamedata/building_images.json` lists the 96 pictures of the pages. Stored the same way as the researches' icons, in
`webui/static/img/buildings/` (`python3 tools/fetch_research_images.py --set buildings`); the wiki's image server asks scripts for a
browser check and the tool does not get around it (see research.md for the alternative).

**Licence.** Text: CC BY-SA 3.0 (Lords Mobile Wiki contributors). Pictures: game artwork belonging to IGG. The `source` block of the files says so.
