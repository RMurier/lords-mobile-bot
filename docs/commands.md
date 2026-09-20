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
| `$bank bal` | administrators | Mails the bank, bag and total balance of each resource |
| `$admin list` | administrators | Lists the administrators, marking those from the configuration file |
| `$admin add <player>` | administrators | Adds an administrator |
| `$admin remove <player>` | administrators | Removes an administrator added in game |
| `$relocate random` | administrators | Moves the castle to a place chosen by the game (uses a random relocator), after a confirmation |
| `$relocate <x> <y>` | administrators | Moves the castle to these coordinates in the current kingdom (uses an advanced relocator), after a confirmation |
| `$migrate <kingdom> <x> <y>` | administrators | Migrates the castle to another kingdom at these coordinates, after a confirmation |
| `$confirm` | the administrator who asked | Confirms the pending action; it expires after 60 seconds |
| `$cancel` | administrators | Cancels the pending action |
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
$relocate random   then  $confirm   (or $cancel)
$relocate 100 100  then  $confirm
$migrate 796 301 491  then  $confirm
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

## Relocation

`$relocate` is for administrators only and never acts at once: the bot describes what it is about to do
and waits for `$confirm` from the same administrator, within 60 seconds (or `$cancel`). One action
can be pending at a time.

- `$relocate random` needs a random relocator in the bag; the game picks the destination.
- `$relocate <x> <y>` needs an advanced relocator in the bag. The coordinates must be on the map and
  different from the current position. The destination is in the kingdom the castle is in.
- After the confirmation the bot reports where the castle landed, or that the server refused.
  Rallies and marches are affected by a relocation like in the game: think before confirming.

## Migration

`$migrate <kingdom> <x> <y>` moves the castle to another kingdom, at the coordinates you choose. Like
`$relocate` it is for administrators only, describes what it is about to do and waits for `$confirm` (60 seconds).

What the bot does after the confirmation, in the same order as the official client:

1. It asks the game for the server of the target kingdom. An unknown or closed kingdom is reported and nothing is sent.
2. It sends the migration request with the kingdom and the destination tile.
3. The server accepts (or refuses) and closes the connection; the bot reconnects by itself, into the new kingdom.

The bot only knows the **free migration** offered to returning players, because that is the one that was captured
from the official client. When it is not available the bot says so, and tells you whether migration scrolls are left in the bag:

- no scroll: *"Aucune migration gratuite n'est disponible et vous n'avez plus de vélin de migration."*
- scrolls left: it says how many, and that the migration has to be done in the game, because using a scroll goes
  through another request that is not known yet.

The confirmation message already warns when the bag has no migration scroll.

## The bank

A resource command sends resources to the player who wrote it, subject to:

- **Reserve**: the bot never goes below `bank.reserve_*` of each resource.
- **Distance**: a player farther than `bank.max_delivery_distance` tiles (straight line) is refused
  and told how far they are. `0` = no limit.
- **Bag items**: with `bank.use_bag_rss` and `bank.use_bag_<resource>` on, when the resource is
  short the bot first uses resource items from the bag, wasting as little as possible, waits for them
  to be credited, then delivers. If even the bag cannot cover the amount, nothing is used and the
  answer says how much is available.
- **One transfer at a time**: while one is in progress, another player receives an "Occupé"
  answer. The requester can write a new command to replace theirs, or `$stop` to cancel.

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
- migration: both requests are byte for byte the ones captured from the official client (a free migration to
  kingdom 796), and the server's answers are decoded from the same capture. Only the accepted case was seen: the codes
  of a refusal are unknown, so the bot shows the number it receives. Try it on a spare account first;
- relocation: the packets are the ones the bot already had for the two relocators, and the answer is
  decoded by the existing code, but no relocation was run on a live account. Try it on a spare account.

Start with `command.output = MAIL` and a small test before relying on them.
