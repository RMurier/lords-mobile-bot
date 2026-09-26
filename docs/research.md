# Research (technologies)

Everything the bot knows about researches, what is confirmed, what is not, and what has to be
captured before the bot can **start** researches itself. Code: [`include/research.h`](../include/research.h)
(layout, decoding helpers, known names) and `RecvTechnologyInfo()` in `src/protocol.c`.

Two sources, kept apart: what the server sends (*confirmed*, from packet captures of the official PC
client on two accounts, decoded with the bot's own key) and the research table (names, effects, levels,
costs), which comes from the community wiki and is described in [The research table](#the-research-table-from-the-wiki).

## What the server sends

`_MSG_RESP_RESEARCHINFO` (3201), once, at login. 265 bytes:

| Offset | Size | Field |
|---|---|---|
| 0 | u16 | tech id of the research in progress, `0` = none |
| 2 | u8 | unknown (`6` in the capture with a research running) |
| 3 | i64 | start time of that research, server clock, seconds |
| 11 | u32 | total duration of that research, seconds |
| 15 | 250 bytes | current level of every research, 4 bits each |

Levels: id `n` (1..500) lives in byte `(n - 1) / 2`; an **odd** id is the low nibble, an **even** id
the high nibble. Levels go from 0 to 10 in every capture. The nibble could hold up to 15, so the
maximum level of a given research is **not** in this packet.

In the capture the research in progress started 74 h before the server time and lasts 93.1 h, i.e.
19.1 h left. The numbers are consistent with each other, but **the remaining time has not yet been
checked against the one the game displays**, so the meaning of the start time and of the duration
is still to be confirmed that way. The account had 297 of the 500 ids above level 0, 179 of them at
level 10.

`ResearchLevel()`, `ResearchInProgress()`, `ResearchEndTime()` and `ResearchSecondsLeft()` in
`research.h` read it. The bot logs a summary at login (`[RESEARCH] ...`) and keeps the state in
`Connection.research`.

## What the server does NOT send

Opening the research window and each of the 16 categories, one after the other, made the client
send **nothing** but keepalives (`_MSG_REQUEST_ACTIVE`). Clicking the Trading Post building, which
shows its tax and capacity, behaved the same way. So, for researches, none of this is available
from the network:

- names, categories, effects (per level),
- maximum level of each research,
- costs (resources, time) and prerequisites,
- the order in which the game displays them.

They are in the client's own data tables, which the bot ships a copy of: see
[The game's own tables](#the-games-own-tables-every-id-its-category-its-levels). (They were first thought unreadable - packed in
Unity bundles - and taken from the community wiki, next section; the bundles turned out to be readable.)

## The research table (from the wiki)

`gamedata/research.json` holds the research tree of the [Lords Mobile wiki](https://lordsmobile.fandom.com/wiki/Research):
15 categories with 361 places in the trees (347 distinct pages), and for each research its effect, the
number of levels and, per level, the effect value, the might, the prerequisites (e.g. `Academy Lv22`), the
duration, the Technolabes and the cost in resources. It is produced by a tool, not typed:

```
python3 tools/import_wiki_research.py     # about 45 requests to the wiki's public API, one per second
```

The wiki's pages are blocked to scripts, its API is not, so the tool reads the API. The wiki is edited by players.

| Category (game order) | Researches |
|---|---|
| Economy 9, Defense 19, Military 24, Monster Hunt 22 | 74 |
| Upgrade Defenses 14, Upgrade Military 27, Army Leadership 24, Military Command 23 | 88 |
| Familiars 24, Familiar Battles 30, Sigils 35, Wonder Battles 27 | 116 |
| Gear 30, Advanced Wonder Battles 32, Mana Awakening 21 | 83 |
| Guild Duel | **not on the wiki**: the page is about the event, there is no research tree to read |

**What is stored per research**: category, position in the tree (`row`, `col`), name, icon, description,
`max_level`, the names of the effect columns and, per level, `effects`, `might`, `requirements`, `time` (seconds),
`technolabe` and `cost` (`food`, `stone`, `timber`, `ore`, `gold`, plus `archaic_tome`, `anima`, `mana_ore` where
a research uses them). The position in the grid is the tree's layout; the arrows between researches are not read,
so prerequisites between researches are only those written in the `requirements` column.

**How reliable it is.**

- **Cross-checked**: "Bigger Bags I" gives 50,000 supply capacity at level 1 on the wiki, exactly what
  `include/tech_research.h` has, and 13 of the 15 names identified in the game exist on the wiki with a maximum
  level of 10, like the levels seen on the accounts.
- **Typos on the wiki**: 33 of the 347 pages have a value that cannot be read without guessing (a cost written
  `503,9695`, `gfdgfg`, `768k`; a duration `Academy Lv22` in the time column; a column missing). Such a value is
  `null` and the reason is in that research's `warnings`. Nothing is corrected by the tool: check those in the game.
- **Placeholders**: `Trap` and `Troop` appear 7 times each in the trees but their pages have no level table.
- **Levels the game cannot store**: the game's level field is 4 bits (0 to 15), yet `Training Speed II` is
  listed with 35 levels, `Lunar Foundry` 25, `Training Speed I` 16 (a list that stops at 15 and jumps to 25) and three
  "Offense II" researches 20. Those are most likely wiki errors or researches that work differently: do not rely on them.
- **Maximum level**: 299 of the 347 pages have 10 levels, 32 have a single level (the "Phalanx", "Wedge"... unlocks).

**Icons.** `gamedata/research_images.json` lists each icon and where it is. They are meant to be stored in
`webui/static/img/research/` so the web console can show them, and the console's server already serves anything
under `webui/static/`. The wiki's image server asks scripts for a browser check, and this tool does not get around
it, so the icons are stored from your side:

```
python3 tools/fetch_research_images.py --status        # how many are stored
python3 tools/fetch_research_images.py                 # download, stops by itself if the server asks for a browser check
python3 tools/fetch_research_images.py --from-dir D    # otherwise: import icons saved by hand from a browser
```

`webui/static/img/research/index.json` (written by the tool) maps each icon name to its stored file.

**Licence and attribution.** The wiki's text is CC BY-SA 3.0 (Lords Mobile Wiki contributors); the icons are game
artwork belonging to IGG, taken from the wiki. The `source` block of both files says so: keep it.

### The game's own tables: every id, its category, its levels

The wiki cannot say which number is which research, but the game can: the client ships its data tables, and the
ids the server sends are the ones in them. They are Unity text assets in `Loading/Table.unity3d` (in the APK, and in the PC
client's `Download/6000/Loading/`, the more recent copy - identical for research), names in the string bundles next to it.
The record layouts (`TechKindTbl`, `TechDataTbl`, `TechLevelTbl`) come from an Il2CppDumper dump of the game
([apk-analysis.md](apk-analysis.md)), and every table's size is checked against its record count, so nothing here is guessed.

```
python3 tools/extract_game_tables.py --table .../Table.unity3d --strings .../StringFre.unity3d [--strings-en .../String.unity3d]
```

It writes `gamedata/game_research.json` (everything) and `src/research_table.c` (what the bot uses, declared in
`include/research_table.h`). What it holds: **403 researches, 16 categories, 3,425 levels**; per research its category, name
(French and English), maximum level and whether the game marks it not researchable (`locked`: #238, #242, #246, #347); per level the
base duration in seconds (before any speed bonus), the five resource costs, the Academy level needed, the prerequisites
(research + level), the might and the effect (id and value).

**Confirmed against the game**: the 14 ids known from the game's UI all carry the name it showed, and **#126 is Tax Break**
(Army Leadership, prerequisite #142 at level 3, Academy 22, +0.1% per level - what this page had only inferred). Gear, which
holds #299-#305, is category 13. The maximum levels agree with the wiki (Wonder Battles has 27 researchable, Advanced Wonder
Battles 32, as the wiki counts them, once the `locked` ones are left out). **Guild Duel**, which the wiki lacks, is in the game's
table: 26 researches, #378-#403.

**Ids do not follow the categories** (later additions took the numbers after the first 257: Defense is #10-#72 with holes,
Familiar Battles #258-#287, Gear #288-#317...): always go through the table, never through ranges.

| Tab | Category | Wiki name | Ids | Researches | Academy |
|---|---|---|---|---|---|
| 1 | Économie | Economy | 1-9 | 9 | - |
| 2 | Défense | Defense | 10-72 | 19 | - |
| 3 | Militaire | Military | 26-323 | 36 | - |
| 4 | Chasse au monstre | Monster Hunt | 73-94 | 22 | - |
| 5 | Améliorer la défense | Upgrade Defenses | 56-69 | 14 | 10 |
| 6 | Améliorer l'Armée | Upgrade Military | 95-191 | 27 | 10 |
| 7 | Direction Armée | Army Leadership | 121-144 | 24 | 17 |
| 8 | Commandement Militaire | Military Command | 145-227 | 23 | 17 |
| 9 | Familiers | Familiars | 167-190 | 24 | - |
| 10 | Batailles de familiers | Familiar Battles | 258-287 | 30 | 21 |
| 11 | Sceaux | Sigils | 192-226 | 35 | 24 |
| 12 | Batailles de Merveille | Wonder Battles | 228-257 | 30 (27 researchable) | 24 |
| 13 | Équipement | Gear | 288-317 | 30 | 25 |
| 14 | Batailles de merveilles avancée | Advanced Wonder Battles | 324-356 | 33 (32) | 25 |
| 15 | Éveil du mana | Mana Awakening | 357-377 | 21 | 25 |
| 16 | Duel de guildes | *(not on the wiki)* | 378-403 | 26 | 15 |

(The ids given are the lowest and highest of each category, not a range: they interleave.) The `game_id` field of the wiki
table (`tools/import_wiki_research.py`, 14 of 347 filled) is superseded by this; the wiki file is still where its own
effects text comes from.

### The delivery tax: the Trading Post, minus Tax Break

The supply tax is what the **Trading Post** sets, minus what the **Tax Break** research takes off. The wiki's Trading Post
page gives the building's rate per level (30% at level 1 down to **8% at level 25**; the mana levels above 25 do not change it,
see [buildings.md](buildings.md)), and lists **Tax Break** (Army Leadership, row 5 next to Bigger Bags I): effect "Supply
Tax Reduction", 0.1% per level, 1% at level 10. The two accounts captured read **7.5%** and **7.6%**, with a Trading Post at
30 and 25 (both 8%), so Tax Break is at level 5 and 4:

    tax = (rate of the Trading Post's level) - 0.1% x level of #126        (8.0% - 0.5% = 7.5%, 8.0% - 0.4% = 7.6%)

The **8% base is confirmed** by the wiki's table (it was first deduced from the two accounts). What stays **inferred** is that
Tax Break is research **#126**: out of the 500 researches only three are exactly one level higher on the first account than on the
second, #123 and #125 (both identified, and neither is about tax) and #126, which sits right after Bigger Bags I in the tree's row.
A third account, or the level the game shows for Tax Break on either of these two, would confirm it.

The bot uses this before its first delivery (`DeliveryTaxPercent()`), then reads the real rate from the delivery report and
logs `[TAX]` when the two disagree, which would show the guess is wrong. Below level 25 the wiki's whole numbers look rounded, so
the estimate is rougher there and the report corrects it after the first delivery.

## Categories (the game's order)

Économie, Défense, Militaire, Chasse au monstre, Améliorer la défense, Améliorer l'armée, Direction armée,
Commandement militaire, Familiers, Batailles de familiers, Sceaux, Batailles de merveille, Équipement, Batailles de
merveille avancée, Éveil du mana, Duel de guilde. Which id belongs to which one is in the game's table (previous
section); `$research` lists them with how many are finished.

## Starting, cancelling, finishing

*Confirmed* from a capture of the official client on one account, on research #57, levels 3 to 5:
two starts that finished at once for free, one cancelled, one sped up with items. Payloads below are
what follows the 4-byte packet header; requests are given as they are **before** the DES encryption
(`send_packet(c, true)` encrypts them). The bot builds the requests byte for byte like the game
(`tests/bot_logic_test.c` compares them with the captured bytes).

The game does **not** use `_MSG_REQUEST_RESEARCH_EVENT_START` (3202) to start a research: in the four
captured starts it sent `_MSG_REQUEST_SMARTUSE_FOR_RESEARCH` (1433), "smart use", which carries the
resource items to consume to cover what the cost lacks.

| | Opcode | Payload |
|---|---|---|
| **Start** (request) | 1433 `_MSG_REQUEST_SMARTUSE_FOR_RESEARCH` | `u32 seq, u16 tech id, u8 target level, u16 n, n x { u16 item id, u16 quantity }` |
| Start (answer) | 3203 `_MSG_RESP_RESEARCH_EVENT_START`, 44 bytes | `u8 status (0 = ok), u16 tech id, u8 target level, i64 start time, u32 duration (s), u32 unknown, 6 x u32` |
| Items left | 1431 `_MSG_RESP_SMARTUSE_FOR_WORK` | what remains in the bag for each item used (`u16 item id, u16 quantity left`) |
| **Cancel** (request) | 3206 `_MSG_REQUEST_RESEARCH_EVENT_CANCEL` | `u32 seq, u16 tech id, u8 target level, 3 zero bytes` |
| Cancel (answer) | 3207, 32 bytes | `u8 status, u16 tech id, u8 level, u32 unknown, 6 x u32` |
| **Done** (push) | 3208 `_MSG_RESP_RESEARCH_EVENT_COMPLETE` | `u16 tech id, u8 level reached` |
| Speed up (request) | 1427 `_MSG_REQUEST_SMARTUSE_SPEEDUP` | `u32 seq, u16 kind (5 = research), u16 n, n x { u16 item id, u16 quantity }` |
| Speed up (answer) | 1428 | status, kind, then the items left in the bag |
| Use one item | 1406 `_MSG_REQUEST_USEITEM` | the game used it once per speed-up item, kind 5 (research) |

Things to know:

- **The level in a request is the target level**: the current level + 1. Starting level 3 of #57 sends 3
  (the player called it "lvl 2"), and a cancel of the level 4 being researched sends 4.
- **The `6 x u32` at the end of the start and cancel answers** look like the account's resource stocks
  after the operation (5 resources and a sixth constant, 100200). Their order has not been checked, so
  the bot does not use them.
- **A duration of 0** in the start answer means the game finished it at once for free (its "auto
  acceleration" when the research is short); the completion push follows immediately.
- **Item ids in the requests** are resource items from the bag, e.g. 1009 = 30k food, 1018 = 15k gold
  (`items.h`); 1186, 1252 are also resource packs, not named yet.
- After a start the game also sends an alliance help request (`_MSG_RESP_ALLIANCE_SOMEBODY_NEEDHELP`,
  2852, 5 bytes, sent by the client; the answer comes back as 2853). Its meaning was not analysed.

### The plain start (no item), confirmed

Captured from the PC client on research **#221** (Furious Defense (Ranged), level 8 -> 9), with enough resources in stock so that
no item had to be used: `_MSG_REQUEST_RESEARCH_EVENT_START` (**3202**), payload `u32 seq, u16 tech id, u8 target level, 3 zero
bytes` (10 bytes, `1a000000 dd00 09 000000`), the same shape as the cancel. The answer is the same 44-byte 3203 as the smart-use
start's (status 0, tech, level, start time, duration). It was followed at once by the client's alliance help request (2852, 5 bytes)
and the server's 2853 - the game asks for help by itself after every start.

The base duration of that level is 4,258,620 s (49 days); the answer said **926,108 s** (10.7 days): the account's research speed
divides the table's `time` by about 4.6. So the table gives an order of magnitude, the answer gives the real duration.

Also seen in the same capture: an item speed-up (1427, kind 5, five speed-up items: 1257, 1259, 1262 (3 h), 1263, 1265, answered by
1428 with what is left) finishes the research in progress at once, followed by the completion push 3208. Trying to start a second
research **while one is running sent no packet at all**: the client refuses on its own, so the server's error code for that is still
unknown - the bot checks it itself before sending (`ResearchInProgress`).

**Not seen yet**: the "free" (3204/3205) and "instant" (3209/3210) requests, and the error codes of a refused start or cancel.

In the bot: `RequestResearchStartPlain()` (the plain start), `RequestResearchStart()` (with items), `RequestResearchCancel()`, and
`RecvResearchStart/Cancel/Complete()` in `src/protocol.c`. The answers keep `Connection.research` up to date (research in progress,
level reached when one completes). `$research start <category>|#<id>` (commands.md) starts, on request, the next research the account
can start now (`ResearchPickNext()`, see [the order](#prerequisites-and-the-order)). The automatic research
(`research.enabled` / `research.categories`, `ResearchAutoTick()`) does the same by itself whenever no research is running
([configuration.md](configuration.md#automatic-research)).

### Prerequisites and the order

Each level of a research has its own prerequisites in the table: up to 4 `(research, level)` pairs that must be reached, and an Academy
level. **They are per level** (381 of the 403 researches ask something for level 1, and many again for levels 2 to 10), **not just for
the first one**. Checked against a real account (the captured one, 297 researches started): none of the levels it reached breaks
its prerequisites, which is what confirms the reading. 43 of the 2,365 prerequisites point to a research of **another category**, and
some researches need each other at different levels (#14 and #15), so a chain has to be followed level by level, never research by
research. The Academy level counts its mana part: the Mana Awakening researches ask for 30 to 55.

The bot's order (`ResearchPickNext()`): every research of the chosen category is a goal, taken to its maximum. For a goal, the step is
its next level, or, when that level needs a research the account has not reached, the step of that prerequisite (recursively, any
category, a chain deeper than 24 or a loop is dropped). The steps that can be started (Academy, base cost against the stock) compete:
the one that unblocks the most goals first - a prerequisite several researches wait for before one nothing waits for - then the
shortest. `tests/bot_logic_test.c` runs every one of the 16 categories from a blank account to its maximum through this rule: it never
dead-ends and every step it starts has its prerequisites met at that moment.

## To start researches from the bot on its own

1. ~~The missing captures~~ - the plain start was captured (previous section). Still missing, and only useful to handle a refusal
   cleanly: the server's error code for a start it refuses, and the "free"/"instant" finishes.
2. ~~The research table~~ - done: every id, category, maximum level, cost, duration and prerequisite is in
   `src/research_table.c` (see [The game's own tables](#the-games-own-tables-every-id-its-category-its-levels)).
3. With both, choosing the next research of a category is a matter of prerequisites (`req_id`/`req_level`), the
   Academy level, the resources and a priority set in the configuration.

Today the bot can tell which researches are done and what is left in a category (`$research`), start the next one of a category on
request (`$research start`), and work through chosen categories by itself (`research.enabled`, `research.categories`). The automatic
mode spends the stock, guild bank deposits included (`research.reserve_*` keeps some back).
