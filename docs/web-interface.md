# Web console

A local web interface to configure the bot, import credentials, start and stop it and
read its logs. It needs only Python 3 (standard library, nothing to install).

```bash
python3 webui/server.py        # Windows: webui.bat
```

The server prints its address and opens it in your browser. There is no login or access
token: anyone who can reach the port can read and change every account (the config files
hold live credentials), so keep it on a machine only you use, or restrict network access
to it some other way if it runs somewhere shared. Bots you start from the console run as
long as the server window stays open; closing it (Ctrl+C) stops them all.

Options: `--port 8765` (the next free port is used if it is busy), `--no-browser`.

## Accounts

Each account is a file `accounts/<name>.cfg` with its own bot process and its own log
(`logs/<name>.log`), so adding accounts means adding files. The sidebar lists them with
their status; **Start all** starts them one by one with a delay (Settings) so they do not
all connect at the same instant.

**Add an account** offers three ways:

- **Capture from this computer** (Windows): the console runs the network capture for you.
  Close the game, click **Start**, accept the Windows administrator prompt, start the game and log
  in to the account (for several accounts: log out in the game, then log in to the next one), then
  click **Finish and import**. The console stops the capture, creates or refreshes the accounts and
  deletes the capture. See below for what it does with your system.
- **Import a network capture**: drop the `.pcap` / `.pcapng` file. The console extracts
  the accounts, keys, client version, platform and gateway by itself. An account that
  already exists only gets its credentials refreshed. See [credentials.md](credentials.md).
- **Create an empty account**: optionally copy the settings (never the credentials) of
  another account, then fill in the credentials by hand.

**Naming accounts.** Click the pencil next to an account's name (or in the account list) and type
what you want, for example `Bank` or `Filler`, or pick a suggestion. It is a display name only:
it appears in the list, the header and the notifications, and an empty name goes back to the
default. The name you type when you create an empty account is used the same way. The file
`accounts/<id>.cfg` keeps its name. Deleting an account deletes its config file.

## Settings by category

Account, Connection, Reconnection, Commands, Bank, Protection, Cargo, Alliance, Advanced.

- Master switches (for example *Enable the bank*) grey out the options they control.
- A **default** badge means the option is not in the file and the default applies.
- Values are validated before anything is written; errors appear next to the field and on
  the tab. Sizes accept `500K`, `20M`, `1B`.
- The access key is never sent back to the browser. Leave the field empty to keep it.
- Saving keeps your comments and unknown keys, preserves the line endings, and keeps the
  previous version as `<name>.cfg.bak`.
- Ctrl+S saves. **Save and restart** applies changes to a running bot; otherwise they apply
  at the next start.
- Every option is applied by the bot. Should one ever not be, it carries a **not yet active** badge.
- Administrators are one comma separated list; the old single `admin.name` key is merged into it
  when you save. **Command channels** are check boxes: the bot only reads the ticked ones.
- **Reconnection** has two delays: after a dropped connection, and after you log in yourself
  (`reconnect.kicked_delay`, the time you get to play; 0 = the bot stops instead of reconnecting).

## What the automatic capture does

- It needs administrator rights for `pktmon`, the packet capture tool built into Windows. A helper
  is started with the usual Windows prompt (UAC); it starts a capture limited to TCP port 5999, waits
  for you, then stops it, converts it and removes the capture filter it created.
- The helper receives its instructions on its command line rather than from a file, calls `pktmon`
  by absolute path, and works in a private temporary folder that it refuses to use if it is a link.
- Only the gateway traffic (port 5999) is recorded. If no login is found, tick *Capture all TCP
  traffic* and try again: that records everything TCP the computer does during the capture, so keep
  it short and do nothing else in the meantime.
- The capture holds session keys, so it is deleted as soon as it has been imported (or cancelled).
  Ctrl+C on the console also cancels a running capture.
- `pktmon` allows one capture at a time: if you already started one yourself, stop it first
  (`pktmon stop`). Filters are also global: the helper removes all `pktmon` filters when it starts
  and when it ends.
- On Linux and macOS the card explains that it is not available: use Wireshark or tcpdump and import
  the file.

## Commands page

The **Commands** button (top bar) lists every in-game command with its usage, who may use it,
examples with copy buttons and the rules that apply (reserve, distance, bag items, channels). The
prefix shown is the one of the account you opened last. See [commands.md](commands.md).

## Status tab

The first tab of an account shows what the bot knows about it in game: online or not, **shield** (type and time left,
counting down live), player name, power, kills, gems, VIP, kingdom, position, marches, alliance rank, resources
(stock, bag, production per hour) and troops. The bot writes these to `data/<account>/status.json` every 5 seconds and
the page reads it every 4 seconds. The values are the last ones the server sent; the page says how old they are.
If the bot is stopped, the page shows the last known values marked as offline.

## Research tab

Two parts. **The settings**: the automatic research (a switch, the categories to work on in priority order, and how much of each
resource to leave alone), see [configuration.md](configuration.md#automatic-research). **Where the bot is**, refreshed every few
seconds from what the bot reports:

- the research in progress, with its progress bar and time left, and what the automatic mode is doing or why it does nothing
  (Academy too low, prerequisites, not enough resources...);
- the 16 categories in the order of the game's tabs, each with how many researches are finished and how many are started; the ones
  the bot works on are marked *auto 1*, *auto 2*...; click one to open it;
- the researches of the open category, each with its level out of its maximum, and for the ones not finished: *Prête* (the bot could start
  it now), *Ressources* (only the stock is short), *Prérequis* with what has to be done first (a research and its level, with its category when it is another one) or *Académie* with the level it asks for,
  plus the duration (before your speed bonuses) and cost of the next level.

The names, maximum levels, costs and prerequisites come from the game's own table (`gamedata/game_research.json`, made by
`tools/extract_game_tables.py`); the levels come from the bot. Nothing is shown until the bot has started and been sent your researches.

## Construction tab

The same shape as the research tab. **The settings**: the automatic construction (a switch, the buildings to work on in priority order,
the resources to leave alone), see [configuration.md](configuration.md#automatic-construction). **Where the
account is**, from what the bot reports (the levels follow each construction that finishes while the bot runs):

- what is under construction, with its progress and time left, and what the automatic mode did or why it does nothing;
- the buildings the automation can work on, each with how many the account has and the highest level, and *auto 1*, *auto 2*... on the chosen
  ones; click one to open it;
- each building of the open type: its level (`25 + mana 1, 2/5 vers mana 2`, out of 55), and for the ones not finished *Prête*, *Ressources*,
  *Prérequis* (with the buildings to raise first and their level), *Recherche* (a research is short) or *En construction*, plus the next level's
  duration (before speed bonuses) and cost, mana costs included, and a note when the game also asks for a step of its quests, which the bot cannot see.

The names, maximum levels, costs and prerequisites come from the game's own table (`gamedata/game_buildings.json`).

## Guild bank tab

Shows the [guild bank](commands.md#the-guild-bank) of the account: **every member of the guild with their balance** of food,
stone, wood, ore and gold, and the totals. The members are the list the bot last received (`alliance.member_names` of its
`status.json`, kept in the database too, so the list is there when the bot is stopped); a player with a balance who is no longer in
the guild stays listed, marked *hors guilde*. A filter narrows the list.

- **Change a balance**: *Modifier* on a row turns its five amounts into fields (`500000`, `2,5M`, `1B` are understood), *Enregistrer*
  sets them exactly. A player who is not listed can be given a balance by name through the API (below).
- **Reset everything**: *Tout remettre à zéro* asks for a confirmation that says how many players hold a balance and that it cannot be
  undone; the API refuses it unless the request says it was confirmed. Deliveries the game reports as older than the reset are not
  counted afterwards.
- **Where it is stored**: with SQL Server, in the tables `guild_bank` (one row per account and player, names compared exactly) and
  `guild_bank_state`, written in one transaction; without it, in the bot's own `data/<account>/guild_bank.txt`. The page says which.
- **While the bot runs it owns the balances**, so the console does not write them: it queues the change in
  `guild_bank_edits.txt` and the bot applies it within a second, then the page shows the result. If the bot cannot (disconnected, off)
  the answer is *pending* and the change is applied when it can. **While the bot is stopped** the change goes straight to the store and
  reaches the bot when it starts. A running bot whose account has `guildbank.enabled = false` would ignore the change: the console
  refuses it and says so.
- The backup file (*Paramètres → Télécharger une sauvegarde*) carries the balances.

API: `GET /api/accounts/<id>/bank`, `PUT /api/accounts/<id>/bank/<player>` with `{"food": 1000000, "gold": 0}` (only the resources
given change), `POST /api/accounts/<id>/bank/reset` with `{"confirm": true}`. Tests: `python3 tests/webui_bank_test.py`.

## Logs

The **Log** tab follows the bot's output live. When a bot has stopped, the console shows
the last log line and, for the common cases, what to do (expired key, outdated client, ...).
The full history is in `logs/<name>.log` (rotated at 5 MB).

## Security model

Config files hold live session keys, so:

- **There is no login or access token.** Anyone who can reach the console's port can read
  and change every account. This is a deliberate choice (see `webui/server.py`'s docstring),
  made for a single-user machine - it is not a login system with the auth stripped out for
  convenience, there simply is none.
- The server listens on `127.0.0.1` only by default (`--host`/`LMBOT_HOST` to change that,
  e.g. `0.0.0.0` in the container image), and checks the `Host` header (protection against DNS
  rebinding) and the `Origin` of writes (`LMBOT_ALLOWED_ORIGINS` for the addresses behind an
  ingress/reverse proxy) regardless of that setting.
- **Do not expose it to a network or the internet without restricting access some other
  way** (firewall, private network, VPN, an ingress that itself requires auth...). The
  `Host`/`Origin` checks above stop drive-by attacks from a browser tab open on something
  else, not a deliberate visit to the console's own address.

## Files

| Path | Content |
|---|---|
| `accounts/*.cfg` | One config per account (ignored by git) |
| `logs/*.log` | One log per account (ignored by git) |
| `webui_settings.json` | Console settings: bot executable path, start delay, display names (ignored by git) |

The bot executable is detected in `build/`, the repository root and your home directory
(`build.bat` copies it there); set another path in **Settings**.
