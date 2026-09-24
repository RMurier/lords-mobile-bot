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

They are in the client's own data, which is packed in compressed Unity bundles that could not be
read as plain files. The community wiki lists them: see the next section.

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

### Which of these is which number in the game

The wiki has no protocol ids, and the server sends levels by id. So the table cannot yet be read against the levels
the bot receives. `game_id` is filled for the 14 researches whose id is known:

| Id | Research | |
|---|---|---|
| 6 | Construction Speed | confirmed (game UI) |
| 8 | Gem Harvesting I | confirmed |
| 74 | Energy Recovery I | confirmed |
| 123 | More Gatherers | confirmed |
| 125 | Bigger Bags I | confirmed |
| **126** | **Tax Break** | **inferred** (below) |
| 143 | Gold Storage I | confirmed |
| 229, 234 | Gem Harvesting II, Bigger Bags II | confirmed |
| 299, 301, 302, 303, 305 | Barracks Expansion II, Ration Run IV, Forced March III, Bigger Bags III, Quick Maneuvers III | confirmed |

Two identified ones are not mapped because the wiki names them differently: #228 "Wonder March I" (the wiki has
"Wonder March") and #54 "Fire Trebuchet" (the wiki has "Fire Trebuchet Subsidy"), and #57, the rampart defence
research of the captures, has no name yet.

**What was tried to map the rest, and why it does not work** (so it is not tried again):

- *Order in the grid.* The known ids do not follow the tree's layout: in Gear, 299, 301, 302, 303, 305 sit at rows 3,
  1, 1, 2, 0. No reading order of the grid reproduces them.
- *Constraints from the two accounts* (each level must be at most the research's maximum, and the Academy level required
  by each level reached must not exceed the account's Academy, 30 and 24): a wiki research is still compatible with
  438 of the 500 ids on average. The 14 known ones all pass, which validates the check, but it identifies nothing.
- *Level signatures.* The two accounts' levels for an id, (level on account 1, level on account 2), take only 26 different
  values over the 297 started researches (101 of them are (10, 0)), and only 3 are unique.
- *Durations.* The real duration of a research the bot saw start (#57: 3069 s at level 4, 8503 s at level 5) does not
  match any wiki research at a constant speed factor: the wiki's "Orig. Time" is out of date for the current game version.
- *Files of the game.* `Download/Data` only holds per-account saves; the tables are inside the compressed bundles
  (about 4 GB), not extracted.

An id can only be learned **from the game side**: the game never shows it, the server sends it when a research changes
level. So the bot now records it by itself: at every login it compares the levels with the ones saved at the previous
login (`<data.path>/research_levels.bin`) and logs each change:

    [RESEARCH] #57 : niveau 2 -> 5 depuis la dernière connexion

Anyone who plays between two logins and notes what they researched gives one id per line; the same holds for the
`[RESEARCH] Lancée : #<id>` and `Terminée : #<id>` lines the bot logs when it starts or sees a research finish. Add the pair to
`KNOWN_GAME_IDS` in `tools/import_wiki_research.py` and run it again.

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

Économie, Défense, Militaire, Chasse au monstre, Améliorer la défense, Améliorer l'armée,
Direction armée, Commandement militaire, Familiers, Batailles de familiers, Sceaux, Batailles de
merveille, Équipement, Batailles de merveille avancée, Éveil du mana, Duel de guilde.

Ids identified so far (from the game's UI), which also show roughly where the categories sit in the
numbering. The ranges are a deduction from these few points, not a fact:

| Id | Name |
|---|---|
| 6 | Economy: Construction Speed |
| 8 | Economy: Gem Harvesting I |
| 54 | Military: Fire Trebuchet |
| 57 | rampart defence research (the player calls it « déf rempart »); exact name not read |
| 74 | Monster hunt: Energy Recovery I |
| 123 | Army Leadership: More gatherers |
| 125 | Army Leadership: Bigger Bags I (supply capacity) |
| 126 | in progress in the capture, same category (Army Leadership); name not read |
| 143 | Army Leadership: Gold Storage I |
| 228 | Wonder Battles: Wonder March I |
| 229 | Wonder Battles: Gem Harvesting II |
| 234 | Wonder Battles: Bigger Bags II (supply capacity) |
| 299 | Gear: Barrack Expansion II |
| 301 | Gear: Ration Run IV |
| 302 | Gear: Forced March III |
| 303 | Gear: Bigger Bags III (supply capacity) |
| 305 | Gear: Quick Maneuvers III |

Effect values are known only for the researches in `include/tech_research.h`.

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

**Not seen yet** (so not implemented): a start that needs no item at all (all four captured starts used
items, so it is not known whether the game then sends the same request with `n = 0` or the plain
`_MSG_REQUEST_RESEARCH_EVENT_START`, 3202), the "free" (3204/3205) and "instant" (3209/3210) requests, and
the error codes of a refused start or cancel.

In the bot: `RequestResearchStart()`, `RequestResearchCancel()`, and `RecvResearchStart/Cancel/Complete()`
in `src/protocol.c`. The answers keep `Connection.research` up to date (research in progress, level reached
when one completes). Nothing sends a research by itself.

## To start researches from the bot on its own

1. **The missing captures** (one action each, `pktmon`, PC client): start a research when you have enough
   resources so that no item is needed; finish one for free and one instantly with the game's own buttons;
   try a refused start (not enough resources, another research already running) to see the error code.
2. **The research table**: now in `gamedata/research.json` (see above), except the 16th category (Guild Duel)
   and the numbers the game uses for each research (`game_id`, 14 of 347 known). Those numbers are what is left to
   map before the bot can look up a research from what the server sends.
3. With both, "which is done, which is left" is `ResearchLevel()` against the maximum level, and choosing the
   next one is a matter of prerequisites and priorities set in the configuration.

Without 2 the bot can already start and cancel a research it is told to (by id and level), but it cannot
tell what is worth researching, nor whether the maximum level is reached. Without 1 it does not know how to
start one when no item has to be used.
