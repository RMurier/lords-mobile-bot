# Web console

A local web interface to configure the bot, import credentials, start and stop it and
read its logs. It needs only Python 3 (standard library, nothing to install).

```bash
python3 webui/server.py        # Windows: webui.bat
```

The server prints a link that contains a random access token and opens it in your
browser. Use that link: the API refuses requests without the token. Bots you start
from the console run as long as the server window stays open; closing it (Ctrl+C)
stops them all.

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

## Logs

The **Log** tab follows the bot's output live. When a bot has stopped, the console shows
the last log line and, for the common cases, what to do (expired key, outdated client, ...).
The full history is in `logs/<name>.log` (rotated at 5 MB).

## Security model

Config files hold live session keys, so:

- The server listens on `127.0.0.1` only, checks the `Host` header (protection against DNS
  rebinding) and the `Origin` of writes, and requires the token on every API call.
- The token travels in the URL fragment, which is never sent to the server or logged, and
  is removed from the address bar.
- **Do not expose it to a network or the internet** (reverse proxy, container port
  mapping, tunnel). There is no user authentication behind the token, and it refuses
  requests whose `Host` is not `localhost` anyway.

## Files

| Path | Content |
|---|---|
| `accounts/*.cfg` | One config per account (ignored by git) |
| `logs/*.log` | One log per account (ignored by git) |
| `webui_settings.json` | Console settings: bot executable path, start delay, display names (ignored by git) |

The bot executable is detected in `build/`, the repository root and your home directory
(`build.bat` copies it there); set another path in **Settings**.
