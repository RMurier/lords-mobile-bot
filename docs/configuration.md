# Lords Mobile Bot Configuration

> **Note:** Configuration support is currently under development.  
> Not all options are available or fully supported yet. More configuration options will be added as development continues.

This document describes the available configuration options for **Lords Mobile Bot**.

## Creating Configuration

A default configuration file can be generated using:

```bash
./client --create-config
```

This creates:

```text
config.cfg
```

in the current working directory.

After creation, edit `config.cfg` to configure the bot.

The bot loads the configuration file from the current directory when starting:

```bash
./client
```

## Configuration Format

The configuration file uses a simple key-value format:

```cfg
key = value
```

Lines beginning with `#` are comments.

Example:

```cfg
server.addr = 192.243.44.63
server.port = 5999
command.prefix = $
```

---

## Gateway Server

Controls the gateway server connection.

```cfg
server.addr = 192.243.44.63
server.port = 5999
```

| Option | Description |
|---|---|
| `server.addr` | Gateway server address |
| `server.port` | Gateway server port |

---

## Client Version

Defines the game client version and language.

```cfg
client.version_major = 2
client.version_minor = 197
client.version_patch = 308
client.language_code = 1
```

| Option | Description |
|---|---|
| `client.version_major` | Major client version |
| `client.version_minor` | Minor client version |
| `client.version_patch` | Patch version |
| `client.language_code` | Client language code |
| `client.platform` | Platform byte of the gateway login: `1` = mobile (default), `9` = official PC client. Use the same platform as the device your access key was captured from |

---

## Data Path

The bot stores the administrators added in game there (`admins.txt`), so they survive a
reconnection or a restart. The directory is created when needed.

```cfg
data.path = ./data/
```

### Packet capture

`log.packets = true` writes **every** packet, received (`<-`) and sent (`->`, before encryption),
in full, to `<data.path>/packets.log`. Unlike `log.debug` it is neither truncated (256 bytes) nor
lost with the console, so a whole session can be searched afterwards for a value the bot does
not decode yet (e.g. the delivery tax). Off by default: the file grows quickly and contains
session data from the login, so never share it as is.

```cfg
log.packets = false
```

---

## Administrators

Players who can use every command (see [commands.md](commands.md)). Comma separated, at most
16 names of at most 12 characters.

```cfg
admin.names = Alice, Bob
```

- Nobody is administrator until you set this. Names are case-sensitive.
- The old single-name key `admin.name` still works and is merged into the list.
- An administrator can add or remove others in game with `$admin add <name>` and
  `$admin remove <name>`. Those are stored in `<data.path>/admins.txt`. Administrators listed
  in the configuration file can only be removed by editing it.

---

## Account

Account information used for authentication.

```cfg
account.igg_id = YOUR_IGG_ID
account.device_uuid = YOUR_DEVICE_UUID
account.access_key = YOUR_ACCESS_KEY
```

> **Warning:** Do not share your account credentials publicly.

---

## Getting Account Credentials

See [credentials.md](credentials.md) for the complete guide. In short, `account.igg_id`,
`account.device_uuid` and `account.access_key` can be extracted from a network capture of
**your own** device while the official game starts:

```bash
./client --create-config
python3 tools/extract_credentials.py capture.pcap --template config.cfg --out-dir accounts/
```

One config file is written per IGG ID found in the capture (`accounts/<igg_id>.cfg`),
with the gateway address, client version, language and platform taken from the capture as well.
The official PC client has an empty device uuid, in which case `account.device_uuid` is left out.

On Windows, `pktmon` captures without installing anything (administrator PowerShell):

```powershell
pktmon start --capture --pkt-size 0 -f capture.etl
# start the game and wait until you are in game, then:
pktmon stop
pktmon etl2pcap capture.etl -o capture.pcapng
```
These files contain live session credentials: never share or commit them
(`accounts/` and `*.pcap` are in `.gitignore`).

---

## Automatic Reconnection

When the connection drops, or when the account is logged in from another device
(for example when you play on your phone), the bot waits and reconnects by itself.

```cfg
reconnect.enabled = true
reconnect.delay = 60
reconnect.kicked_delay = 60
reconnect.max_attempts = 0
```

| Option | Description |
|---|---|
| `reconnect.enabled` | Reconnect automatically (`true` by default) |
| `reconnect.delay` | Seconds to wait before reconnecting after a dropped connection (minimum 10, default 60) |
| `reconnect.kicked_delay` | Seconds to wait when the account was logged in from another device, i.e. **you started the game** and the bot was disconnected (default 60). `0` = do not reconnect: the bot stops. Otherwise minimum 10 |
| `reconnect.max_attempts` | Consecutive failed attempts before giving up, `0` = never give up |

Notes:

- The delay doubles after repeated failures, up to 8 times the configured delay.
- The configuration file is reloaded on every reconnection, so a refreshed
  `account.access_key` is picked up without restarting the bot.
- If the server **rejects the credentials** (invalid or expired access key, client
  version too old) the bot stops instead of retrying, since retrying cannot succeed.
- `reconnect.kicked_delay` is the time you get to play: when the bot is disconnected because you
  logged in, it waits that long before reconnecting, which logs you out again. Set it to about
  how long you play, or to `0` to make the bot stop and restart it yourself when you are done.
  It grows with repeated failures like `reconnect.delay`.

---

## Command System

> The available commands and who may use them are described in [commands.md](commands.md).

Controls bot command handling.

```cfg
command.prefix = $
```

### Command Channels

Available channels:

```
WORLD
GUILD
MAIL
```

Example:

```cfg
command.input = GUILD, MAIL
command.output = MAIL
```

| Option | Description |
|---|---|
| `command.prefix` | Prefix used for bot commands |
| `command.input` | Channels where commands are read, one or several separated by commas (default `GUILD, MAIL`). A command written elsewhere is ignored |
| `command.output` | Channel where the bot answers (default `MAIL`). In chat, the answer is one line addressed to the player (`@name ...`) |

`WORLD` is public chat: only tick it if you want everybody there to be able to reach the bot.

---

## Banking System

> The bank is **off by default**. When enabled, any player who can write to the bot can ask
> for the resources whose `bank.send_*` flag is on, up to the reserve; the administrator can
> always use it. See [commands.md](commands.md).

The banking system handles resource transfer commands.

### Enable Banking

Master switch for the banking system.

```cfg
bank.enabled = false
```

When disabled, all banking commands are ignored.

### Allowed Resources

Controls which resources can be delivered.

```cfg
bank.send_food = false
bank.send_rock = false
bank.send_wood = false
bank.send_ore = false
bank.send_gold = false
```

### Resource Reserve

Reserved resources that the bot will not send.

```cfg
bank.reserve_food = 20M
bank.reserve_rock = 50M
bank.reserve_wood = 50M
bank.reserve_ore = 30M
bank.reserve_gold = 0
```

### Delivery Distance

Maximum distance, in tiles (straight line), between the bot and the player asking for resources.
Farther players are refused and told the distance. `0` = no limit.

```cfg
bank.max_delivery_distance = 100
```

### Delivery Tax

The game deducts a percentage of a resource march on arrival. The rate depends on the account and
changes from time to time. The game never sends it as a number, but the delivery report it sends
after each march carries the net amount, so **the bot reads the real rate from it** (e.g. 1M sent,
925000 arrived = 7.5%) and logs it (`[TAX]`). From then on requests are grossed up with that rate,
so `$food 1M` still delivers exactly 1M net, and it follows the rate when it changes.

Before the first delivery of a session, the bot uses the rate the game itself works out: what your **Trading Post**
sets (8% from its level 25, mana levels included) minus 0.1% per level of your **Tax Break** research, both taken from the levels
the game sends at login ([research.md](research.md#the-delivery-tax-the-trading-post-minus-tax-break); the Tax Break research
number is inferred from two accounts). `bank.delivery_tax_percent` is only used if those levels have not been
received yet; the value read from a delivery always wins over both, and is dropped when Tax Break is completed.
`0` (default) sends the amount as requested, no adjustment, until one of them is known.

```cfg
bank.delivery_tax_percent = 0
```

### Use Resource Items

When a command asks for more than is available above the reserve, the bot uses resource items from
the bag to cover the difference (wasting as little as possible), waits for them to be credited,
then delivers. If the bag cannot cover it either, nothing is used and the player is told how much
is available.

`bank.use_bag_rss` is the master switch; the per-resource options choose which resources may use
the bag.

```cfg
bank.use_bag_rss = false

bank.use_bag_food = false
bank.use_bag_rock = false
bank.use_bag_wood = false
bank.use_bag_ore = false
bank.use_bag_gold = false
```

---

## Protection System

Controls automatic defensive features.

### Enable Protection

```cfg
protection.enabled = false
```

### Shield Settings

```cfg
protection.shield_always_on = false
protection.shield_on_incoming_attack = false
protection.shield_on_incoming_scout = false
```

### Shield Priority

The bot uses the first available shield in this order.

```cfg
protection.shield_priority = SHIELD_4H, SHIELD_8H, SHIELD_12H, SHIELD_1D
```

Available shields:

```
SHIELD_4H
SHIELD_8H
SHIELD_12H
SHIELD_1D
SHIELD_3D
SHIELD_7D
SHIELD_14D
```

### March Recall

Automatically recalls marches when threats are detected.

```cfg
protection.recall_on_incoming_attack = false
protection.recall_on_incoming_scout = false
protection.recall_on_incoming_conflict = false
```

---

## Shelter System

Currently unavailable.

Future options:

```cfg
# protection.shelter_always = false
# protection.shelter_leader = true
# protection.shelter_troops = false
# protection.shelter_on_incoming_attack = true
# protection.shelter_on_incoming_scout = true
```

---

## Guild Bank

Members deposit resources by sending them to the bot, the bot keeps a balance per player, and the resource commands take them back
(`$food 1M`, `$stone all`); `$bal` shows a balance and administrators give from the stock with `$adminfood <player> <amount>`.
Everything about how it behaves is in [commands.md](commands.md#the-guild-bank).

```cfg
guildbank.enabled = false
```

Off by default. On, it replaces who-may-take-what of the bank (`bank.enabled`, `bank.send_*`): every guild member takes their own
balance. `bank.reserve_*` and `bank.max_delivery_distance` still apply (the reserve to what administrators give from the stock, the distance
to every delivery). The balances are in `<data.path>/guild_bank.txt`: back it up with the rest of the data folder.

---

## Recall

After the `$recall` command ([commands.md](commands.md#recalling-the-troops), administrators only) has taken every march back, the
bot sends no march at all (automatic gathering, resource deliveries, rally joins) for this many seconds. A new `$recall` restarts the time.

```cfg
recall.pause_seconds = 300
```

`300` (default) is 5 minutes; `0` recalls the troops without any pause. The pause is kept across a reconnection.

---

## Migration

**Provisional.** Number of migration scrolls the game asks for this account. It depends on the power and on the
kingdom, and the game gets it from the server; the bot cannot read the answer yet. Until it can, type here the
number shown by the migration screen.

```cfg
migration.scrolls_needed = 1
```

The bot compares it with the scrolls in the bag when you use `$migrate` and says when some are missing
(see [commands.md](commands.md)). Between 1 and 9999.

---

## Cargo Ship

Automatically completes Cargo Ship trades.

### Enable Trading

```cfg
cargo_ship.auto_trade = false
```

### Allowed Resources

```cfg
cargo_ship.spend_food = false
cargo_ship.spend_rock = false
cargo_ship.spend_wood = false
cargo_ship.spend_ore = false
cargo_ship.spend_gold = false
```

### Use Resource Items

```cfg
cargo_ship.use_bag_rss = false
```

### Resource Reserve

The bot keeps these amounts and only spends excess resources.

```cfg
cargo_ship.reserve_food = 10M
cargo_ship.reserve_rock = 10M
cargo_ship.reserve_wood = 10M
cargo_ship.reserve_ore = 10M
cargo_ship.reserve_gold = 10M
```

---

## Automatic Gathering

Scans the zones around the castle (one at a time) for resource tiles and sends gather marches to them.

```cfg
gather.enabled = false
gather.kind = INFANTRY, RANGED, CAVALRY, SIEGE
gather.max_marches = 1
gather.radius = 30
gather.max_troop_count = 0
```

A gather march is 4 fixed slots (one per troop kind). `gather.kind` is a priority list: the march is filled from the first
kind's free troops, and when that is not enough for what the tile needs, the next kinds top it up in the same march (e.g.
infantry first, then ranged for the remainder) - only what each kind really has free is sent, never more. What is out on
marches is tracked per kind. If every kind in the list is empty, the march waits (~30s backoff, logged) instead of retrying
every tick. Which *tier* gets used within the chosen kind is
left to the server's own auto-pick (confirmed live: it already picks the account's lowest available tier on its own, e.g. T2 when
T1 is empty) - not something this bot chooses. The troop *count* per march is still an experimental estimate
(`GATHER_DEFAULT_TROOP_CAPACITY`, one captured sample) - it can ask for far more than you have; `gather.max_troop_count` (0 = no
cap) and the chosen kind's own known troop total both cap it, whichever is lower.

If the server refuses a march ("Marche refusée"), that tile is left alone for 60s, and after 3 refusals in a row the bot 
halves its per-march troop cap (learned at runtime, raised 25% after each accepted march) - a refusal with code 2 on a free tile 
usually means the march is bigger than your commander's march capacity. Set `gather.max_troop_count` to your real capacity to skip the learning phase.

Also skips any tile currently occupied (confirmed live: an occupied tile's map data embeds the occupier's name and alliance tag)
- it is never targeted until it shows free again. If every known tile is occupied, the bot says so (with how many) and backs off
~30s instead of retrying every tick with no explanation.

The full scan reruns automatically every 5 minutes (not just once at startup): occupied-tile state only updates from whatever the
server pushes passively, and confirmed live that this can go stale - a tile can stay marked occupied long after the real occupier
has left, with no push ever correcting it. The periodic rescan (same mechanism as the first one) refreshes it and also picks up any
new tiles; it never interrupts a march that is already mid-send.

`gather.max_marches` reserves that many of the account's total marches for gathering. `gather.radius` is how far around the castle
(in tiles) to scan for resource tiles.

---

## Automatic Training

One box per (kind, tier) pair, like the game's own barracks/range/stable/workshop screen: a 4x5 grid, 20 independent targets.

```cfg
autotrain.enabled = false
autotrain.infantry_t2 = 10M
autotrain.infantry_t4 = 5M
autotrain.ranged_t2 = 12M
autotrain.ranged_t4 = 5M
autotrain.cavalry_t2 = 10M
autotrain.cavalry_t4 = 5M
```

Key format: `autotrain.<kind>_t<1-5>` (`kind`: `infantry`, `ranged`, `cavalry`, `siege`). Each box is independent: 0 or unset means
"do not train that exact (kind, tier) pair". Only **one (kind, tier) pair trains at a time for the whole account** - confirmed live,
requesting a second kind while a first one is still training gets refused outright, same as trying it manually in game. The bot
round-robins across the four kinds so one that always needs a huge amount doesn't starve the others of a turn; within a kind, the
lowest tier not yet at its own target is always tried first (e.g. `T2` fills before `T4` starts). Accepts a plain number or a
`K`/`M`/`B` suffix (`10M` = 10000000), same as the bank/cargo ship reserve settings. A tier that keeps getting refused (e.g. the
research for it is not done yet) backs off automatically instead of retrying every tick - a few times a minute at first, then once
every 30 minutes if it still fails, without ever giving up on it for the session (the research could finish later).

**Refusals:** the game trains one (kind, tier) at a time. After any refusal the bot leaves *every* kind alone for 2 minutes instead of
trying the next one a second later (which would be refused for the same reason), and the next order asks for half the refused amount
(first order: what the account's Barracks hold - 20 at level 1 up to 5000 at level 25 each, added up over every Barracks, before
research bonuses, so a floor - then grows again by 50% from what was granted). The
login packet `_MSG_RESP_TRAININGINFO_` (2402) tells whether something is already training (type, tier, quantity, begin time,
duration - the layout of the game client's own `SoldierKind/SoldierRank/SoldierBeginTime/SoldierNeedTime` fields, confirmed by a
capture of 2472 infantry T2 the owner was training): while it runs the bot sends nothing, and it says so in the log (`Formation deja
en cours ... encore N min`). With nothing training the packet is not sent at all. Refusal **code 1** is that same "already training"
(it never shrinks the amount); another code (2 was seen with 0 food) does.

**Never more than the stock pays for:** the price of one troop comes from the game's `Soldier` table (`include/troop_table.h`, base
price, bonuses only lower it). The order is cut down to what the scarcest resource pays for, **stock and bag together**.

**The bag pays only what the stock lacks, the way the game does it:** when the stock does not cover an order but the stock plus the
bag's resource items does, the bot sends the game's own "train and use the bag" request (`_MSG_REQUEST_SMARTUSE_FOR_TRAINING`, 1434:
type, tier, amount and the list of items, layout read from a capture): one packet that trains and pays what is missing, with the
smallest cover of items (the same choice the game's button made in that capture: 250K + 150K + 30K + 5K of food for a 433560 shortfall).
If the server refuses it, the bot goes back for 10 minutes to the older way below. The bag's items include the event ones the game
also uses (250K, 100K, 50K... items besides the plain 3K-60M ones). The older way: the bot uses the strict minimum of items for the missing part of each resource (same planning as the trade and `$askhelp` top-ups,
smallest cover), **one item at a time, food last** (stone, wood, ore, gold first) with a 3-4 s wait and the server's answer checked before the next (three used at once got
two refused with status `0x44` in a capture, while the bot counted all three as credited: the order came out at 15 troops instead of
5000). A refused or unanswered item takes its credit back and leaves the bag alone for 10 minutes; the order is then cut to what the
stock alone pays for. At most 8 items per order.

**What is really trained:** the server's answer to an order gives the amount it granted (it caps by its own stock and capacity) and the
stock left after it. The log says `Demande de formation : N ...` when the order goes out and `Formation acceptee : G ...` (a warning if
G is less than N) when the server answers; the stock left replaces the bot's count. A barracks always holds at least its floor, so the
next order never asks for less than that, whatever a limited order taught. With not one troop payable (bag included) the bot logs `Pas assez de
<ressource> (sac compris) ...` and leaves that box alone for 10 minutes.

Note: the game can silently grant far less than requested even when accepting the order (e.g. asking for 3.7M and receiving 29) -
confirmed by the account's own owner that this per-order cap is the same for every kind and every tier at any given moment (only
price and duration differ) and changes over time (29 at one point, 7226 at another, same account). Rather than ask for the raw
gap outright - an amount no human would ever type - the bot starts from a modest guess and grows ~50% from whatever was actually
granted last time, sharing that learned value across all four kinds so it only needs discovering once, not four times over.

**Speeding up a training queue with items, cheaply:** research/observation from live play - use **one 50% reduction item first**,
then fill the remainder of the timer with **25% items**. Using several 50% items back to back wastes more of each one's reduction
than mixing in 25% items for the tail end.

---

## Automatic Monster Hunt

Hunts the map monsters by itself (protocol and what is known: [monster-hunt.md](monster-hunt.md)).

```cfg
monster.enabled = false
monster.level = 3
monster.energy_max = 64540
monster.chat_report = true
```

- `monster.level` (1-9, the map has 1 to 5): the level of the monsters to hunt. Some levels need the "Monster Hunt" researches; when the server refuses
  3 attacks in a row the bot pauses 30 minutes and says why in the log.
- `monster.energy_max`: **your energy bar's maximum**, as the game shows it (it depends on the hunting gear and the researches: the server does not send it,
  the client computes it). 0 (the default) = the bot does not hunt. The bot knows the current energy from the login (stored value + the recovery since,
  1 point per 1.2 s) and from each attack's answer.
- The bot starts a series when the energy reaches that maximum: it attacks the **nearest** monster of that level (the map is scanned like the gather tiles, a
  scan only when a hunt is near), one attack at a time, until it dies. Five heroes are sent: the ones that fight with the damage the monster is weak to first
  (a "High PDEF" monster like the Gorzilla gets the mages, a "High MDEF" one the physical heroes, a balanced one the highest levels), then by level, star and
  rank; the team is completed with the best of the others. An account with fewer than five heroes does not hunt.
- The cost of an attack is learned from the answers (a level 3 attack costs 4240 on the account of the captures) and depends on the account's gear and researches.
  When the energy no longer covers an attack and the monster is still alive, the bot writes its name, level and coordinates in the guild chat
  (`Gorzilla niveau 3 K:13 X:281 Y:471`, the game makes the coordinates clickable) - `monster.chat_report = false` only logs it - and leaves it alone for
  30 minutes. **The bag's energy items are never used.**
- The event monsters (Astra...) are not hunted. The hunt has its own march in the game: it does not take one of the gather marches.

---

## Automatic Research

Researches the categories you pick, by itself. Whenever no research is running, the bot starts the next one it can, in the first
category of the list that has one. It knows the prerequisites: when a research needs another one at some level, that one comes first,
even from another category. Among what it can start, the research that unblocks the most others comes first, then the shortest.
The game runs one research at a time.

```cfg
research.enabled = false
research.categories = Sigils, Gear
research.reserve_food = 0
research.reserve_rock = 0
research.reserve_wood = 0
research.reserve_ore = 0
research.reserve_gold = 0
```

`research.categories` is the priority order. Each entry is a category name - the English name of the game's tab (`Economy`,
`Defense`, `Military`, `Monster Hunt`, `Upgrade Defenses`, `Upgrade Military`, `Army Leadership`, `Military Command`, `Familiars`,
`Familiar Battles`, `Sigils`, `Wonder Battles`, `Gear`, `Advanced Wonder Battles`, `Mana Awakening`, `Guild Duel`), or the French
one, accents optional - or its number in the tabs (1 to 16). An entry that matches nothing, or several categories, is a configuration
error. The web console's *Recherche* tab writes it for you.

**The order.** Every research of the chosen categories is a goal, taken to its maximum level. For each one, the bot looks at the next
level it needs: if that level asks for another research at a level the account has not reached (the tables give each level its own
prerequisites), that research's step comes first, and so on down the chain - in any category, 43 of the game's 2,365 prerequisites
point to another one, so a category can pull a few researches from a neighbouring one. Among the steps it can start now, the one that
unblocks the most goals wins (a prerequisite several researches wait for goes before one nothing waits for), then the shortest. A step
can be started when the Academy is high enough for that level - counted with its mana levels, the last researches ask for 30 to 55 -
and the stock covers its cost, the game's base cost (the tables do not know your cost reductions). What counts as available is the
stock minus `research.reserve_*`, which is never spent. **The guild bank's deposits are not held back**: the research spends the
stock like anything else, so keep a reserve if members' deposits must stay. The bot checks every 5 minutes when nothing can be started, and again a few seconds after a research finishes. A research
the server refuses is left alone for 10 minutes and the next candidate is tried. Which one is being researched, and why nothing is,
is shown in the *Recherche* tab and in the log (`[RESEARCH]`).

Start one by hand with `$research start <category>` (docs/commands.md). What the protocol does, and what the tables hold: [research.md](research.md).

---

## Automatic Construction

Upgrades the buildings you pick, by itself. Whenever a construction queue is free, the bot starts the next upgrade the planner picks, in the
first building type of the list that has one. It only upgrades buildings that exist (it does not place new ones). The account's second
queue is used when it has one (permanent, or rented and still running: read from the login, see [buildings.md](buildings.md)).

```cfg
build.enabled = false
build.buildings = Castle, Barracks
build.reserve_food = 0
build.reserve_rock = 0
build.reserve_wood = 0
build.reserve_ore = 0
build.reserve_gold = 0
```

`build.buildings` is the priority order. Each entry is a building name - English (`Lumber Mill`, `Quarry`, `Mines`, `Farm`, `Manor`, `Barracks`,
`Infirmary`, `Castle`, `Vault`, `Academy`, `Battle Hall`, `Castle Wall`, `Watchtower`, `Embassy`, `Workshop`, `Treasure Trove`, `Trading Post`,
`Prison`, `Altar`, `Monsterhold`, `Spring`, `Mystic Spire`, `Gym`, `Lunar Foundry`, `Mana Lode`, `Mana Chamber`), French, accents optional - or its
`build_id`. A whole name wins over a part of one (`Castle` is the Castle, not the Castle Wall); an entry that matches nothing or several buildings
is a configuration error. Every building of a chosen type is taken to its maximum level; the buildings it needs first (prerequisites) come first,
even if they are not in the list, in the same order as the researches: what unblocks the most goes first, then the shortest. Costs are the tables'
base costs and only the five basic resources are checked (the mana costs are not: the server refuses what is short). What counts as available is
the stock minus `build.reserve_*`, which is never spent - **the guild bank's deposits are not held back**. **One farm is never upgraded**: the lowest-level one is kept for `$askhelp` (docs/commands.md), whatever the list says. A start the server refuses
(`_MSG_RESP_BUILDINGERROR`, whose layout has not been seen: it is logged as it comes) leaves that building alone for 10 minutes. See
[buildings.md](buildings.md#automatic-construction) for the order and for what is confirmed.

## Future Configuration

The following features are planned or under development:

- Additional protection options
- Shelter automation
- More resource management options
- Additional bot modules
- More runtime configuration controls

---

## Dead lord

An account whose lord (the leader) is dead gets `_MSG_RESP_LORD_BEINGEXECUTED` (4408) at its login - three healthy accounts' captures do not have it. 13 bytes:
`time u64 | wait u32 | flag u8`, from a capture `2026-09-12 23:05:59 | 604800 s (7 days) | 1`: the lord was captured, the 7 day execution wait was over two weeks
before the capture, so he is dead and waits for a resurrection. The bot logs it (`[SEIGNEUR] Le chef est mort : ...`), the console shows a "Chef mort" card in the
status, and, with a Discord webhook, `notify.on_lord_dead = true` (the default) sends the alert once per run. The resurrection itself (`_MSG_REQUEST_LORD_REVIVE`,
4410, answer 4411) is not implemented: its layout is not known, it needs a capture of the game's own resurrection.
