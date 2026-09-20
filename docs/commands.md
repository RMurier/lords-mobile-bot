# In-game commands

Every command starts with the prefix (`command.prefix`, `$` by default). Write it in a channel the
bot reads (`command.input`, by default alliance chat and mail); the bot answers in the channel
chosen by `command.output` (mail by default). The web console has a page listing all of this with
copy buttons, and in game `$help` gives the list that matches the rights of whoever asks.

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

## The bank

A resource command sends resources to the player who wrote it, subject to:

- **Reserve**: the bot never goes below `bank.reserve_*` of each resource.
- **Distance**: a player farther than `bank.max_delivery_distance` tiles (straight line) is refused
  and told how far they are. `0` = no limit.
- **Bag items**: with `bank.use_bag_rss` and `bank.use_bag_<resource>` on, when the resource is
  short the bot first uses resource items from the bag, wasting as little as possible, waits for them
  to be credited, then delivers. If even the bag cannot cover the amount, nothing is used and the
  answer says how much is available.
- **One transfer at a time**: while one is in progress, another player receives a "Transfer Busy"
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
- the delivery distance against real player positions.

Start with `command.output = MAIL` and a small test before relying on them.
