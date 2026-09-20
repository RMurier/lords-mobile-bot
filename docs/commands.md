# In-game commands

The bot reads messages in chat and mail. A message that starts with the command prefix
(`command.prefix`, default `$`) is treated as a command. The bot always answers by mail.

Amounts accept the suffixes `K`, `M` and `B` (`500K`, `1.5M`, `2B`).

| Command | Who | What it does |
|---|---|---|
| `$food <amount>` | administrator, or anyone if the bank allows food | Sends food to the player who wrote the command |
| `$stone <amount>` | same, for stone | |
| `$wood <amount>` | same, for wood | |
| `$ore <amount>` | same, for ore | |
| `$gold <amount>` | same, for gold | |
| `$bank bal` | administrator only | Mails the bank, bag and total balance of each resource |
| `$su <player>` | administrator only | Makes `<player>` the administrator until the bot next reconnects or restarts (the config file is not changed) |
| `$su` | administrator only | Shows the usage |

## Who can do what

The **administrator** is the player named by `admin.name`. They can use every command.
If `admin.name` is not set, nobody can claim the role.

For everybody else, resource commands work only when the **bank is enabled**
(`bank.enabled = true`) **and** the matching flag is on (`bank.send_food`, `send_rock`,
`send_wood`, `send_ore`, `send_gold`). Both are off by default.

The amount is always limited to what remains above the reserve
(`bank.reserve_food`, ...): the bot never sends resources below the reserve. If the
amount is too large it replies with what is available.

Only one transfer runs at a time. While one is in progress, other players get a
"Transfer Busy" mail. (The message mentions `$stop`, which is not implemented yet.)

> **Security.** When the bank is enabled, any player who can write to the bot can ask
> for the resources you allowed, up to the reserve. The bot reads chat and mail regardless
> of the channel; `command.input` and `command.output` are read from the config but not
> applied yet. Keep the bank disabled unless you want a public bank, and set generous
> reserves.

## Settings that are read but not applied yet

These options are accepted in the config file (and shown as "not yet active" in the web
console) but the bot does not act on them yet:
`bank.max_delivery_distance`, `bank.use_bag_*`, `cargo_ship.use_bag_rss`,
`command.input`, `command.output`, `data.path`.
