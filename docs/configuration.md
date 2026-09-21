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

## Future Configuration

The following features are planned or under development:

- Additional protection options
- Shelter automation
- More resource management options
- Additional bot modules
- More runtime configuration controls
