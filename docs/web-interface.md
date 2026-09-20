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

**Add an account** offers two ways:

- **Import a network capture**: drop the `.pcap` / `.pcapng` file. The console extracts
  the accounts, keys, client version, platform and gateway by itself. An account that
  already exists only gets its credentials refreshed. See [credentials.md](credentials.md).
- **Create an empty account**: optionally copy the settings (never the credentials) of
  another account, then fill in the credentials by hand.

You can rename an account (a display name only), and delete it (which deletes its config
file).

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
- Options that the bot does not act on yet carry a **not yet active** badge. See
  [commands.md](commands.md).

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
