# In-game commands

Every command starts with the prefix (`command.prefix`, `$` by default). Write it in a channel the
bot reads (`command.input`, by default alliance chat and mail); the bot answers in the channel
chosen by `command.output` (mail by default). The web console has a page listing all of this with
copy buttons, and in game `$help` gives the list that matches the rights of whoever asks.

The command words are English (`$help`, `$gold`, `$relocate`...); the texts the bot writes back, `$help`
included, are in French.

Amounts accept the suffixes `K`, `M` and `B`: `500K`, `1.5M`, `2B`.

## All commands

| Command | Who | What it does |
|---|---|---|
| `$help` | everybody | Lists the commands the requester may use, with their usage |
| `$stop` | the requester of the current transfer, or an administrator | Cancels the transfer in progress |
| `$food <amount>` | administrators; others if the bank allows food | Sends food to the player who wrote the command |
| `$stone <amount>` | same, for stone | |
| `$wood <amount>` | same, for wood | |
| `$ore <amount>` | same, for ore | |
| `$gold <amount>` | same, for gold | |
| `$bank bal` | administrators | Sends the bank, bag and total balance of each resource, in `command.output`'s channel |
| `$bank bal chat` | administrators | Same, forced into alliance chat regardless of `command.output` |
| `$bank bal mail` | administrators | Same, forced into mail regardless of `command.output` |
| `$admin list` | administrators | Lists the administrators, marking those from the configuration file |
| `$admin add <player>` | administrators | Adds an administrator |
| `$admin remove <player>` | administrators | Removes an administrator added in game |
| `$relocate random` | administrators | Moves the castle to a place chosen by the game (uses a random relocator) |
| `$relocate <x> <y>` | administrators | Moves the castle to these coordinates in the current kingdom (uses an advanced relocator) |
| `$migrate <kingdom> <x> <y>` | administrators | Migrates the castle to another kingdom at these coordinates |
| `$join <tag>` | administrators | Searches for an alliance by its 3-character tag (case-sensitive) and applies to join it |
| `$leave` | administrators | Quits the current alliance |
| `$su <player>` | administrators | Old command, same as `$admin add` |

### Examples

```text
$help
$gold 5M           the bot sends 5 million gold to you
$food 250K
$stop              cancel the delivery that is on its way
$admin add Bob     Bob can now use every command
$admin remove Bob
$bank bal
$bank bal chat     forces the answer into alliance chat for this one command
$relocate random
$relocate 100 100
$migrate 796 301 491
$join JfK          3-character tag, case-sensitive
$leave
```

## Who can do what

**Administrators** are the players listed in `admin.names`, plus the ones added in game with
`$admin add`. They can use every command. Nobody is administrator until you configure it.

Everybody else can only use `$help`, `$stop` (for their own transfer) and the resource commands
**if the bank allows it**: `bank.enabled = true` **and** the matching flag (`bank.send_food`,
`send_rock`, `send_wood`, `send_ore`, `send_gold`). Both are off by default. A command a player
may not use is ignored without an answer.

### Administrators

- `admin.names` in the configuration file are permanent for the bot: `$admin remove` cannot remove
  them, edit the file (or the web console) instead.
- Administrators added in game are written to `<data.path>/admins.txt`, so they survive a
  reconnection or a restart. `$admin remove` deletes them from that file.
- At most 16 administrators, names of at most 12 characters.

## Relocation and migration

`$relocate` and `$migrate` are for administrators only. **They act at once, without confirmation**, and write back
only when there is a problem or when the server has answered. Mind the typos: a wrong coordinate moves the castle.

Relocation, within the kingdom:

- `$relocate random` needs a random relocator in the bag; the game picks the destination.
- `$relocate <x> <y>` needs an advanced relocator in the bag. The coordinates must be on the map and different
  from the current position.
- The bot then reports where the castle landed, or the code the server refused with. Rallies and marches are
  affected like in the game.

Migration to another kingdom, `$migrate <kingdom> <x> <y>`:

1. The bot asks the game for the server of the target kingdom, as the official client does. An unknown or closed
   kingdom is reported and nothing is sent.
2. If there are enough **migration scrolls** in the bag, one is used at once (captured from the official client: a
   migration scroll is used through the same generic "use item" request as the advanced relocator). The free
   migration offered to returning players is *not* tried first: it is rarely available, so a scroll (when there is
   one) is used directly instead of wasting a round trip on an offer that will probably be refused.
3. Only when there are not enough scrolls does the bot fall back to the **free migration**. A refusal is reported
   with the server's reason (kingdom full, troops outside, an event lock...), or, when the reason is not a precise
   one, with how many scrolls would be needed instead.
4. The server accepts (or refuses) and closes the connection; the bot reconnects by itself, into the new kingdom.

**Scrolls.** How many scrolls a migration needs depends on the account's power and on the kingdom; the game reads it
from the server, but the bot cannot yet. `migration.scrolls_needed` (default 1) is a **provisional manual value**:
type the number shown by the migration screen. The message that follows `$migrate` compares the bag with that number:

- no scroll: *"Il n'y a aucun vélin de migration dans le sac : seule la migration gratuite peut aboutir."*;
- some but not enough: *"Il faut 3 vélin(s) de migration et le sac n'en contient que 1 (il en manque 2)…"*;
- enough: *"...un sera utilisé directement."*

**Refusals.** The server's reason is given in French with the game's own code name, for example
*"le royaume de destination est plein (KINGDOM_FULL)"*. The codes were read from the client's enumeration:

| Code | Name | Meaning |
|---|---|---|
| 6 | `KINGDOM_FULL` | the target kingdom is full |
| -6 | `KINGDOM_PROTECT` | the target kingdom is protected |
| -7 | `KINGDOM_ALLIANCE_LIMIT` | the target kingdom reached its alliance limit |
| -5 | `TROOP_OUTSIDE` | troops are outside the castle |
| 5 | `NOT_FIELD` | the destination tile cannot be used |
| 2 | `NEWBIE_ERROR` | newbie account |
| 3 | `INDEMNIFY` | a compensation is pending |
| 4 | `WAR_BUFF_CD` | a war effect is active |
| -2 | `CROSSTELEPORT_IN_PROGRESS` | a migration is already running |
| -3 | `UNABLE_CHANGEHOME` | the kingdom cannot be changed for this account |
| -4, -9, 8 | `DWZ_`, `PBF_`, `ABF_NO_CROSSKINGDOM` | not possible during that event or zone |
| -8 | `FLAG_LIMIT` | flag limit |
| 7 | `UNKNOWN` | unknown: the free migration is probably not available, see the scrolls |

`0` (`SUCCESS`), `-1` (`SUCCESS_IN_FOREST`) and `1` (`SUCCESS_EXPIRE`) are all successes. The English names are the
game's; the French wording of the less obvious ones (`PBF`, `ABF`, `DWZ`, `FLAG_LIMIT`, `INDEMNIFY`) is an interpretation,
which is why the name always follows.

Other errors: unknown kingdom, invalid coordinates, already in that kingdom, a migration already running, or a server
that does not answer.

## Joining and leaving an alliance

`$join <tag>` and `$leave` are for administrators only, one operation at a time (a second command while one is
still running is refused: *"Une opération de guilde est déjà en cours"*).

- `$join` takes the alliance's **3-character tag, case-sensitive** (not its full name) — the game's own search is
  case-insensitive and can match several alliances that only differ by case (for example `JFK` and `JfK`), so an
  exact tag is what picks the right one. Only an exact-case match is used; anything else is reported as
  *"Guilde "xyz" introuvable."*.
- If the castle is already in an alliance, `$join` first leaves it (*"Départ de la guilde actuelle avant de
  rejoindre "xyz"..."*), waits 3-4 seconds (so it doesn't type the new tag the instant it leaves), then searches
  for and applies to the new one. If leaving fails, `$join` stops there and reports the failure instead of
  searching.
- The bot applies (`$join` never joins instantly by itself): depending on the target alliance's own settings, that
  application either joins right away or waits for one of its officers to accept it. **The bot cannot tell which one
  happened** — both are reported the same way: *"Candidature envoyée à la guilde "xyz"."*. Check in game (or with
  `$admin list` once a member) if it actually went through.
- `$leave` quits the current alliance at once, no confirmation asked.

## The bank

A resource command sends resources to the player who wrote it, subject to:

- **Reserve**: the bot never goes below `bank.reserve_*` of each resource.
- **Delivery tax**: the game deducts a percentage on arrival that never shows up in any message.
  Set `bank.delivery_tax_percent` (varies per account) so `$food 1M` still delivers exactly 1M net.
- **Distance**: a player farther than `bank.max_delivery_distance` tiles (straight line) is refused
  and told how far they are. `0` = no limit.
- **Bag items**: with `bank.use_bag_rss` and `bank.use_bag_<resource>` on, when the resource is
  short the bot first uses resource items from the bag, wasting as little as possible, waits for them
  to be credited, then delivers. If even the bag cannot cover the amount, nothing is used and the
  answer says how much is available.
- **One transfer at a time**: while one is in progress, another player receives an "Occupé"
  answer. The requester can write a new command to replace theirs, or `$stop` to cancel.
- **Trading Post**: delivery is limited by the Trading Post's supply capacity. Without one built
  (or if its level hasn't been received from the server yet, right after connecting), a resource
  command is refused with *"Le Poste de Commerce n'a pas de capacité de livraison disponible..."*.
- **Marches**: if the number of available marches hasn't been received from the server yet (same
  timing issue, right after connecting), the command is refused with *"Nombre de marches disponibles
  pas encore reçu du serveur..."*. If every march is genuinely busy, the delivery just waits for one
  to free up; if the target can't be found, a march is refused by the server, or a march that already
  left never comes back, the requester is told and the delivery is cancelled instead of stalling
  silently.
- **Pacing**: when an amount needs several marches, the bot waits 1-2 seconds (random) between each
  one instead of sending them back to back.

`$stop` cancels the transfer, but marches that have already left still arrive.

## Channels

- `command.input` lists where commands are read: `WORLD` (world chat), `GUILD` (alliance chat)
  and `MAIL`. Default `GUILD, MAIL`. A command from another channel is ignored.
- `command.output` is where the bot answers: `MAIL` (default) or a chat. In chat the answer is a
  single short line starting with `@player`.

> **Security.** When the bank is enabled, any player who can write to a channel the bot reads can
> ask for the resources you allowed, up to the reserve. Mail and alliance chat are limited audiences;
> world chat is public. Keep the bank off unless you want a public bank, and set generous reserves.

## Verification status

The command logic (permissions, administrators and their persistence, channels, replies, `$stop`,
`$help`, distance, choice of bag items, configuration) is covered by automated tests. What has **not**
been run against the live game yet:

- using bag items (`RequestSimpleUseItem` is what the bot already uses for shields);
- answering in chat rather than mail, and the world / alliance channel numbers (taken from the
  bot's own chat code: 0 = world, 1 = alliance);
- the delivery distance against real player positions;
- accented characters (é, è, à...) in the bot's French mails and chat lines: they are sent as UTF-8
  like any text, but if the game shows them wrongly, tell me and I will remove the accents;
- migration: the free-migration request and its refusal codes come from a capture of a free migration to kingdom
  796. The scroll fallback (`_MSG_REQUEST_USEITEM` with `MIGRATION_SCROLL`) comes from a separate capture of a
  real scroll migration to kingdom 300, accepted case only: a refusal of the scroll itself is reported with its
  raw code, whose meaning is not decoded yet. Try both on a spare account first;
- relocation: the packets are the ones the bot already had for the two relocators, and the answer is
  decoded by the existing code, but no relocation was run on a live account. Try it on a spare account.

Start with `command.output = MAIL` and a small test before relying on them.
