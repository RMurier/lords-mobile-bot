# Getting your account credentials

The bot logs in with the same values as the official game: your **IGG ID** and a
**session key** (`access_key`), plus a device UUID on mobile. Instead of hunting for
them by hand, extract them from a network capture of **your own device** while the
official game logs in. `tools/extract_credentials.py` does the rest.

> The capture contains a live session key that gives access to the account without
> a password. Keep it private, never share it, and delete it once imported.
> Botting is against the game's terms of service: try it on a secondary account first.

## How it works

When the game starts it sends a clear-text login packet to the gateway
(`_MSG_NEWLOGIN_LOGINTOL`, then `_MSG_NEWLOGIN_LOGINTOP` to the game server). The
extractor reads it from the capture and writes a ready-to-use config file:

| Extracted | Written as |
|---|---|
| IGG ID | `account.igg_id` |
| Session key | `account.access_key` |
| Device UUID (empty on the official PC client) | `account.device_uuid` |
| Client version and language | `client.version_*`, `client.language_code` |
| Platform (`1` mobile, `9` official PC client) | `client.platform` |
| Gateway address and port | `server.addr`, `server.port` |

## 1. Capture the game starting

The capture must **start before you open the game** and **stop once you are in game**,
because the login is only sent when the game starts. Close the game completely first.

### Official PC client (Windows, nothing to install)

Open PowerShell **as administrator**:

```powershell
pktmon start --capture --pkt-size 0 -f capture.etl
# start the game and wait until you are in game, then:
pktmon stop
pktmon etl2pcap capture.etl -o capture.pcapng
```

A VPN does not matter: captures taken through a VPN adapter are supported.

### Emulator or other tools

Any tool that produces a `.pcap` or `.pcapng` file works, for example Wireshark on the
network interface used by the game. Some phone capture apps limit or charge for file
export; capturing on the PC with an emulator or the PC client avoids that.

## 2. Extract

### With the web console (recommended)

Open the console (`webui.bat`, or `python3 webui/server.py`), choose **Add an account**
and drop the capture file on **Import a network capture**. See
[web-interface.md](web-interface.md).

### From the command line

```bash
./client --create-config                       # a config.cfg to copy other settings from
python3 tools/extract_credentials.py capture.pcapng --template config.cfg --out-dir accounts
```

One file `accounts/<igg_id>.cfg` is written per account found in the capture. Values are
masked on screen (`--show-secrets` prints them in full). On Windows use `python` or `py`
if `python3` opens the Microsoft Store.

Then start it:

```bash
./client accounts/<igg_id>.cfg
```

## Several accounts

Log in to each account while the capture is running (log out and back in inside the
game): every login produces a packet and the extractor writes one config file per
account. Importing a capture for an account that already exists only refreshes its
credentials and keeps all your other settings.

## When the key stops working

The game asks the server for a key valid **30 days**, but the server decides, and the key
content is encrypted, so the real expiry cannot be read from the file. When the server
refuses the credentials the bot stops (it does not retry, because retrying cannot succeed)
and the console shows the reason. Capture and import again to renew the key.

## Troubleshooting

| Symptom | Cause and fix |
|---|---|
| `No login packet found` | The capture started after the game. Close the game, start the capture, then open the game and wait until you are in game. |
| Only a version-less account is found | The capture contains the game-server login but not the gateway login. The credentials still work; the version and gateway from your template are kept. |
| Login refused right after importing | The key expired, or the game was updated. Capture again. Make sure `client.platform` matches the device the key came from (the importer sets it). |
| The bot and your game keep disconnecting each other | The same account cannot be logged in twice. Close the game before starting the bot, or lengthen `reconnect.delay`. |
| `Could not read capture` | Wrong path or an unsupported format. Use `.pcap` or `.pcapng`. |

## Security

- `accounts/`, `*.pcap`, `*.pcapng`, `config.cfg` and `logs/` are in `.gitignore` so they
  cannot be committed by mistake. Check before pushing a fork.
- Config files are written readable by their owner only where the file system allows it.
- Never paste a key, a capture or a debug log (`--debug` prints session data) in public.
