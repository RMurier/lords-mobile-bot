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

### Every id, from the game's own table

The ids used to be worked out by comparing accounts (*inferred*). The game has them: its table `buildkind_new_MB` (in
`Loading/Table.unity3d`, the same file as the researches', see [research.md](research.md#the-games-own-tables-every-id-its-category-its-levels))
gives each `build_id` its name, its kind and whether it can be upgraded. Read with `tools/extract_game_tables.py` into
`gamedata/game_buildings.json` and `src/building_table.c` (`include/building_table.h`). **44 building types**, and the ones nobody had identified are:

| `build_id` | Building | | `build_id` | Building |
|---|---|---|---|---|
| 11 | Battle Hall | | 21 | Spring |
| 16 | Treasure Trove (was *inferred*, now confirmed) | | 22 | Mystic Spire |
| 18 | Prison | | 23 | Gym |
| 19 | Altar | | 24 | Lunar Foundry |
| 20 | Monsterhold | | 27 | Mana Chamber |

The Gym, Mystic Spire and Spring (21, 22, 23) are the three that share a limit of 8, as suspected. The ids **100 to 112** are not scenery
of the turf but the game's other buildings, most with a single level: 100 Hero Stages, 101 Colosseum, 102 Shelter, 103 Garrisoned Troops,
104 Cargo Ship, 105 Transmutation Lab, 106 Labyrinth, 107 Kingdom Tycoon, 108 Bazaar, 109 Sanctuary, 110 Vergeway, 111 Artifact Hall, 112
Trial By Fire. 25 is an empty slot of the table; 28 (Residence) and 29, 30, 31 (Defense Towers) are special kinds the bot leaves alone.

Levels: `buildUP_NEW_MB`, **1,375 rows**, one per building and level, and 55 levels for most (25, then the mana levels, as above; the Mana Lode
and the Mana Chamber start at 25). Per row: the base duration in seconds, the costs (food, stone, wood, ore, gold and the mana ore, mana
crystal, manasteel of the mana levels), the might and the prerequisites. Checked against the wiki: the Castle's first mana level reads
2,649,349 s, 5,333,046 food, 7,272,336 stone and wood, 4,363,401 ore, 23,029 mana ore, 242 mana crystals in both.

**Prerequisites** (`BuildRequestGroup`, one group per level): a list of conditions, of three kinds. *Type 1*: **a building of a type at a
level** (Barracks 6 needs the Castle at 6; up to 9 for the Academy). *Type 3*: **a research at a level** (one row: the Lunar Foundry asks for
research #288). *Type 2*: **a step of the game's own quests** (a number the bot cannot see: the early Castle levels, and the last normal
level of many buildings, ask for one). The server enforces the quest steps; the bot does not know them, so it does not check them.
`docs/research.md` explains the same idea for researches (prerequisites per level, followed in a chain).

## What is being built

`_MSG_RESP_BUILDINGEVENT` (2002), at login, 44 bytes: 2 entries of 18 bytes, then 8 bytes: an `i64` that is **the second queue's end date**
(the game's `BuildingQueueData.ExpireTime`; the second queue is rentable, `_MSG_REQUEST_RENT_EXTRA_BUILDING_QUEUE`). `INT64_MAX` = **permanent**:
it is what every capture shows, on accounts whose player confirmed a permanent second queue. A date ahead = rented and still running; 0 or past = no
second queue - *inferred*, no capture of such an account. `Connection.construction_extra_expires`; the planner counts the second queue only
while that date is ahead.

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

## Starting, finishing, cancelling

*Confirmed* from a capture of the PC client (one account, a farm from level 23 to 24 and another farm, with the second queue free; payloads are what
follows the 4-byte header, requests as they are before encryption). Decoded with `tools/decode_capture.py`.

| | Message | Payload |
|---|---|---|
| **Start** (request) | 2003 `_MSG_REQUEST_BUILDBEGIN` | `u32 seq, u16 slot, u16 build_id, u8 2` (9 bytes, `14000000 d2f2 0400 02`). No level: the server builds the next one. The last byte was `2` in both starts; it is **not the queue** (two constructions ran on the two queues with the same byte), meaning unknown. |
| Start (answer) | 2004 `_MSG_RESP_BUILDBEGIN`, 40 bytes | `u16 slot, u16 build_id, u8 target level, i64 start time, u32 duration (s)`, then 23 bytes: the five stocks after paying (food, stone, wood, ore, gold, u32 each - scrambled in the PC client's capture, plain in the bot's answers, where they match the login's stocks) and **the queue the construction went to (u8, offset 37)**, then 2 bytes. **No status byte**: a refusal is 2013 `_MSG_RESP_BUILDINGERROR`. |
| Help | 2852 `_MSG_RESP_ALLIANCE_SOMEBODY_NEEDHELP` (client), 2853 (answer) | The client asks the alliance for help by itself after every start: payload `u32 seq, u8 1` (the research one sent 0: most likely the kind). No building in it: the server helps the one just started; the answer is `00 01 <build_id> <level> 1e`. `RequestBuildHelp()`. **Send the number 2852, not the enum constant**: `_MSG_RESP_ALLIANCE_SOMEBODY_NEEDHELP` in `packet_enum.h` evaluates to 2854 (the enum drifts by 2 there), which the server does not know: it closed the connection a few seconds after. |
| Speed-up items | 1406 `_MSG_REQUEST_USEITEM`, answer 1407 | One request per item (here 1084 = Speed Up 24 h, 1082 = 8 h twice, 1042 = 60 min, 1040 = 15 min); the answer carries the building's start time and duration. |
| **Done** (push) | 2005 `_MSG_RESP_BUILDCOMPLETE`, 6 bytes | `u16 slot, u16 build_id, u8 level reached, u8 0`. Sent when the timer ends, also when items finished it. |
| **Cancel** (request) | 2006 `_MSG_REQUEST_BUILDCANCEL` | `u32 seq, u8 0` (5 bytes): the queue number, 0 = the base queue (the farm it cancelled ran in queue 0 while the Mana Lode ran in queue 1). Answer 2007, 23 bytes of scrambled values (the refund). `RequestBuildCancel()`, used only by `$askhelp`. |

The construction packet at login (2002) puts what is being built in the entry of the queue it runs on: the Mana Lode of the captured account
sits in entry 1, entry 0 is empty. **The server picks the queue itself, and it is not always the first free one**: the bot's two live starts of the same farm went
to queue 1 with queue 0 empty (the login then listed that farm in entry 1), the capture's two starts went to queue 0. The answer says which (byte 37, values 1, 1, 0, 0). The first version guessed
"first free entry", cancelled the empty queue 0, got no answer and left the farm building.

In the bot: `RequestBuildStart()`, `RecvBuildBegin()` (fills `Connection.construction`), `RecvBuildComplete()` (raises the building's level in
`Connection.building`, which used to keep the login's levels for the whole session, and frees the queue), `RecvBuildingError()`.
`tests/bot_logic_test.c` compares both captured starts byte for byte.

## Automatic construction

Configuration: [configuration.md](configuration.md#automatic-construction). The planner (`BuildPickNext()` in `src/protocol.c`; the test takes each of
the 26 buildings it works on from its first level to its maximum, prerequisites first, each step valid when started) and the tick that starts
what it picks (`BuildAutoTick()`).

The order: every building of a chosen type is a goal, taken to its maximum level. For a goal, the step is its next level, or, when that level
needs another building at a level (type 1 prerequisite), the step of the highest building of that type, recursively. A prerequisite type the
account has no building of ("build it first") or a research that is short blocks the goal and is reported. The steps that can be started (a free
construction queue, the five basic resources covered by the stock - the mana costs are not checked, the server refuses what is short) compete:
the one that unblocks the most goals first, then the shortest. Only upgrades are planned: placing a new building on a free slot is not.

### `$askhelp`: many help requests

The farm is always the account's lowest-level one (`BuildingReservedFarm()`, the lowest slot on a tie). **The automatic construction never upgrades it**, neither as a
goal nor as a prerequisite: with several farms the highest one is used for the prerequisites of other buildings, with a single farm the farms are
left alone and a building that needs a higher farm says so ("construisez-en une autre"). So every bot account keeps one low-level farm.

`$askhelp <times>` (docs/commands.md) loops on the account's **low-level farm**: start (2003), wait for the answer (2004), 1 to 2 s, ask for help (2852), wait 3 to 4 s, cancel the queue the
start went to (2006), wait for the answer (2007), a 3 to 6 s pause, and 1.5 to 3 s before the first start. The pacing follows the game's own timings measured on the
capture (server answers about 0.5 s after a request; the help request 1.2 s after the start's answer; the cancel about 3 s after it): the first version sent
the help request within milliseconds of the answer and the server closed the connection 0.7 s later. `AskHelpTick()` in `src/protocol.c`. What it does not do is guess: a missing
answer (10 s), a server error (2013) or two busy queues stop the series, and it cancels only the queue entry its own start filled - with the other
queue busy that entry is the only free one, so the server had no other choice; with both free the entry is the first free one (the rule the
capture shows), and a wrong guess would cancel an empty queue, not a running construction. The end message gives the stock difference since the start:
the cancel's answer is scrambled, so how much a cancel refunds is not known - this tells.

**A farm left under construction** (the connection dropped between the start and the cancel): at the start of a series, and before each cycle, the bot looks in the queue entries
from the login (`Connection.construction`, kept up to date by 2004/2005): if the kept farm is in one, it sends the cancel for that entry (the queue number is the entry's index,
as the capture shows) and goes on when the answer comes. It cancels nothing else - another building in the queue is never touched.

**The refusal** (`_MSG_RESP_BUILDINGERROR`, 2013): first sample, a start of the kept farm sent by `$askhelp` while a previous series had left that farm under construction (the
connection had dropped): the payload is **2 bytes, `03 01`**. The code has no name in the APK (the check is in native code) and what the two bytes mean is not known:
hypotheses are "queue busy" (3) with the queue number (1) or a resource (1 = second in the list). The bot now writes, with every refusal, what it asked for, the two queue
entries it holds and the five stocks against the level's base cost, to tell them apart.

**Not seen yet**: the refusal's other codes (`_MSG_RESP_BUILDINGERROR` 2013) - its layout is logged as it comes and the building is left alone for 10 minutes -
the cancel's answer, the instant/free finishes (2008, 2009, 2011), and starting a building that does not exist yet.

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
