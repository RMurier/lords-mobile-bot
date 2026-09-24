## Project Status

The Lords Mobile Bot project is now on hold.

Unfortunately, I can no longer continue maintaining and developing it on my own. The biggest challenge has been the lack of contributors, and I don't have enough free time to continue the project alone.

The repository will remain public, and I'd be happy to see the project continue if anyone is interested in contributing or taking over development.

Thank you to everyone who tested the bot, reported issues, suggested improvements, and supported the project throughout its development. Your support has been greatly appreciated.

# Lords Mobile Bot

A modular, high-performance Lords Mobile bot written in C.

The project is designed with a strong focus on clean architecture, efficient packet processing, and configurable automation. It aims to provide a stable, maintainable, and extensible codebase that can be easily expanded as new game features and automation modules are developed.

> **Project Status**
>
> This project is under active development. Features, APIs, and internal structures may change between versions.

## Features

- Modular architecture
- High-performance binary protocol implementation
- Efficient packet processing
- Configurable automation system
- Event-driven networking
- Cross-platform build system (CMake)
- Automatic reconnection
- Credential extraction from a network capture
- Local web console: configuration, accounts, start/stop, live logs
- MIT licensed

## Requirements

- CMake
- A C compiler:
  - **Linux / macOS:** GCC or Clang
  - **Windows:** MinGW-w64 GCC (recommended) or MSVC

## Building

Clone the repository:

```bash
git clone https://github.com/halloweeks/lords-mobile-bot.git
cd lords-mobile-bot
```

The build system is cross-platform. The resulting executable is named `client` on
Linux/macOS and `client.exe` on Windows.

### Linux / macOS

```bash
mkdir build
cd build

cmake ..
cmake --build .
```

Or simply use the provided build script:

```bash
./build.sh
```

### Windows (MinGW-w64)

Using CMake:

```bat
mkdir build
cd build

cmake -G "MinGW Makefiles" ..
cmake --build .
```

Or simply use the provided build script:

```bat
build.bat
```

Or compile directly with GCC:

```bat
gcc -O2 -Iinclude src\main.c src\connection.c src\log.c src\protocol.c src\des.c src\map_point.c src\command.c src\config.c -o client.exe -lws2_32
```

The Windows build links against `ws2_32` (Winsock); CMake and `build.bat` handle
this automatically.

## Getting Started

> On Windows, use `client.exe` instead of `./client` in the commands below.

### 1. Get your account credentials

The bot logs in with your IGG ID and a session key. Extract them from a network capture
of **your own device** while the official game starts, instead of copying them by hand.

**Easiest, on Windows:** start the web console (below), choose *Add an account*, then *Capture from
this computer*: it runs the capture for you, you log in to your accounts in the game, and it imports them.

By hand, Windows, official PC client, nothing to install (PowerShell as administrator):

```powershell
pktmon start --capture --pkt-size 0 -f capture.etl
# start the game, wait until you are in game, then:
pktmon stop
pktmon etl2pcap capture.etl -o capture.pcapng
```

Start the capture **before** opening the game. Then extract:

```bash
./client --create-config
python3 tools/extract_credentials.py capture.pcapng --template config.cfg --out-dir accounts
```

This writes `accounts/<igg_id>.cfg` with the account, session key, client version,
platform and gateway already filled in. Delete the capture afterwards: it contains your
session key. Full guide, several accounts and troubleshooting: [docs/credentials.md](docs/credentials.md).

### 2. Run the bot

**Web console (recommended)**: configure everything, import captures, start and stop
bots and read their logs.

```bash
python3 webui/server.py      # Windows: webui.bat
```

**Command line**:

```bash
./client accounts/<igg_id>.cfg
```

Close the game on that account first: an account cannot be logged in twice.
If the connection drops, or you log in elsewhere, the bot reconnects by itself
(see [reconnection](docs/configuration.md#automatic-reconnection)).

Before you rely on it, set `admin.names` to your in-game name in the config: administrators are
the only players allowed to use every command, and nobody is one until you set it.

## Command Line Options

```text
client <config_file> [--debug]   Run the bot with a configuration file.
client --create-config, -c       Create a default config.cfg.
client --help, -h                Display help.
client --version, -v             Display version information.
```

Debug output (every packet received, with a hexdump) is **on by default**, so a refused login can be
diagnosed from the journal. Turn it off with `log.debug = false`; `--debug` on the command line forces it on.
Debug output can contain session data: do not share it.

## In-game commands

Commands start with the prefix `$` by default and are answered by mail.

| Command | Who | What it does |
|---|---|---|
| `$help` | everybody | Lists the commands the requester may use |
| `$stop` | requester, or an administrator | Cancels the transfer in progress |
| `$food` / `$stone` / `$wood` / `$ore` / `$gold <amount>` | administrators; others only if the bank allows it | Sends the resource to the player who asked (`$gold 5M`) |
| `$bank bal` | administrators | Mails the bank, bag and total balance |
| `$admin list` / `add <player>` / `remove <player>` | administrators | Manages administrators (added ones are saved) |
| `$relocate random` / `$relocate <x> <y>` | administrators | Moves the castle at once (no confirmation) |
| `$migrate <kingdom> <x> <y>` | administrators | Migrates to another kingdom (free migration) at once (no confirmation) |
| `$su <player>` | administrators | Same as `$admin add` |

Command words are English; the bot's answers and `$help` are in French. Commands are read from the channels in `command.input` (alliance chat and mail by default) and
answered in `command.output` (mail by default). The bank is **off by default**. Every command with
its usage, the permissions and the security notes: [docs/commands.md](docs/commands.md).
The web console also lists them (Commands button).

## Project Structure

```
include/        Header files
src/            Bot source code (C)
tools/          extract_credentials.py: build account configs from a network capture
webui/          Web console (Python, standard library only)
accounts/       One config file per account (not committed)
logs/           One log per account (not committed)
docs/           Documentation
CMakeLists.txt  CMake build configuration
build.sh        Build helper script (Linux / macOS)
build.bat       Build helper script (Windows / MinGW)
webui.sh        Start the web console (Linux / macOS)
webui.bat       Start the web console (Windows)
```

## Documentation

- [Getting your account credentials](docs/credentials.md)
- [Web console](docs/web-interface.md)
- [Deployment: SQL Server, Docker, k3s](docs/deployment.md)
- [Configuration reference](docs/configuration.md)
- [In-game commands](docs/commands.md)
- [Research (technologies): protocol and what is known](docs/research.md)
- [Buildings: levels (Mana upgrade), ids, Trading Post capacity and tax](docs/buildings.md)

## License

This project is licensed under the MIT License. See the [LICENSE](LICENSE) file for details.
