# Deployment: SQL Server, Docker, k3s

By default the console keeps everything in files (`accounts/*.cfg`, `logs/`, `webui_settings.json`). When the
environment variable `LMBOT_DB_HOST` is set, it keeps it in **SQL Server** instead, and the whole thing runs in
containers.

## What is stored where

| Data | Files (default) | SQL Server |
|---|---|---|
| Account configuration (with the access key) | `accounts/<id>.cfg` | table `accounts` |
| Console settings, account names, bots to restart | `webui_settings.json` | table `settings` |
| Bot journal | `logs/<id>.log` | table `bot_logs` (last 5000 lines per account) |
| Game status (Statut tab) | `data/<id>/status.json` | table `game_status` |
| Administrators added in game (`$admin add`) | `data/<id>/admins.txt` | table `account_admins` |

The bot program itself does not change: it still reads a `.cfg` file. With SQL Server, the console writes that file
into a scratch folder (`/var/lib/lmbot`, an `emptyDir`) from the database just before starting the bot, and copies the
bot's status and administrators back into the database while it runs. The scratch folder can therefore be lost at any
time without losing anything.

The tables are created by the console at startup (`webui/store.py`), and the database `lordsbot` too.

**Persistence.** The console remembers which bots were running. When the pod restarts (update, node reboot), it
starts them again by itself, one after the other with the start delay of the settings. A bot you stopped by hand
stays stopped. Set `LMBOT_AUTOSTART=0` to disable this.

**First start.** If the database is empty and `accounts/*.cfg` exist next to the console, they are imported once
(with `webui_settings.json`, except the path of the bot program).

## Settings of the console (environment)

| Variable | Meaning |
|---|---|
| `LMBOT_DB_HOST`, `LMBOT_DB_PORT` (1433) | SQL Server; selects SQL Server storage |
| `LMBOT_DB_USER` (`sa`), `LMBOT_DB_PASSWORD`, `LMBOT_DB_NAME` (`lordsbot`) | login and database |
| `LMBOT_TOKEN` | access token, 16+ characters. **Required outside localhost.** Give it in the address: `https://host/#t=<token>` |
| `LMBOT_HOST` | address to listen on (`0.0.0.0` in the image) |
| `LMBOT_ALLOWED_ORIGINS` | public address(es) of the console, comma separated, needed behind an ingress |
| `LMBOT_AUTOSTART` | `1` (default with SQL Server) restarts the bots that were running |
| `LMBOT_CLIENT`, `LMBOT_UI_ROOT` | bot program (set in the image), scratch folder |

## Preparing the database on your PC, then taking it to the server

You can build the database on your PC with Docker (`docker compose up`, see below), load your accounts into it, and move
it to the server as a SQL Server backup:

1. On the PC: `docker compose up -d --build` (settings in a git-ignored `.env`: `MSSQL_SA_PASSWORD`, `LMBOT_TOKEN`).
   The data lives in the Docker volume `lmbot_sqldata` and survives `docker compose down`.
2. Load the accounts (*Paramètres → Restaurer une sauvegarde*, or the API), then make the backup file:
   `BACKUP DATABASE [lordsbot] TO DISK = ... WITH INIT, CHECKSUM, FORMAT` (Express edition does not allow COMPRESSION).
3. Copy `lordsbot.bak` to the server (`scp`), and after `deploy/deploy.sh install`:
   `deploy/deploy.sh restore lordsbot.bak`.

The passwords of the two SQL Servers do not have to match: a restore carries the data, not the `sa` login.
Do not start the bots on the PC and on the server at the same time.

## Moving your existing accounts to the server

The database only exists once the server is deployed, so the accounts of your PC travel as a **backup file** that the
new console loads. It holds the configurations (with the **access keys**), the administrators added in game and the
account names, and nothing that belongs to the PC (path of the bot program, remote server settings).

- *Paramètres → Télécharger une sauvegarde* creates the file (`lmbot-sauvegarde.json`; the pattern is git-ignored).
- On the server's console: *Paramètres → Restaurer une sauvegarde*, choose the file. Accounts that already exist are
  kept unless you accept to replace them; an account whose bot is running is never replaced.
- Or, once the server is set up under *Serveur distant*, *Envoyer mes comptes au serveur* does it in one click.

The data folder of each account is reset to `./data/<id>/` on import, since a Windows path means nothing on Linux.
The same file is your backup: download it regularly. The API is `GET /api/data/export` and `POST /api/data/import`.

## The game is on your PC, not on the server

The identifiers (IGG ID and access key) can only be read from the game while it logs in, so the capture is made on
the computer that has the game. The bots run on the server. Two ways to bring the identifiers to it:

**1. Send from your PC's console (recommended).** Keep using the console on your PC (`webui.bat`), which has the
automatic capture button. In *Paramètres → Serveur distant* give the address of the server's console
(`https://bot.example.com`) and its token (`deploy/deploy.sh status` prints the address with `#t=<token>`). From then
on, every capture made or imported on your PC is **sent to the server, which creates or refreshes the account**; the
PC keeps nothing (the capture file is deleted, as usual). Do the capture when the key is about to expire (about every
30 days) or after the account was logged in on another device.

**2. Upload the file to the server's console.** Capture by hand (`docs/credentials.md`), open the server's console,
*Ajouter un compte → Importer une capture réseau*, and choose the `.pcapng`.

The game and the bot cannot use the same account at the same time: opening the game on your PC logs the bot out
(it reconnects after `reconnect.kicked_delay`), like any second device.

## Try it with Docker Compose

```bash
export MSSQL_SA_PASSWORD='Chang3-me-Str0ng!'      # 12+ characters, upper, lower, digit, symbol
export LMBOT_TOKEN="$(openssl rand -hex 24)"
docker compose up --build
# console: http://localhost:8765/#t=<the token>
```

The data is in the `sqldata` volume: `docker compose down` keeps it, `docker compose down -v` erases it.

## Debian server with an existing k3s (recommended way)

`deploy/deploy.sh` does the whole installation and is written so that **what already runs on the cluster is never
touched**. Run it on the server, in a clone of the repository.

```bash
git clone https://github.com/RMurier/lords-mobile-bot && cd lords-mobile-bot
sudo apt install podman openssl            # only if neither docker nor podman is there
LMBOT_HOST=bot.example.com deploy/deploy.sh check      # read-only
LMBOT_HOST=bot.example.com deploy/deploy.sh install
```

Leave `LMBOT_HOST` empty if you have no domain name: no Ingress is created and you reach the console with
`kubectl -n lmbot port-forward svc/lmbot 8765:80` (the `status` command prints the exact line, token included).

What protects your other workloads:

- **Everything lives in the `lmbot` namespace** (change it with `NAMESPACE=...`). No cluster-wide object is created:
  no ClusterRole, no StorageClass, no change to k3s, Traefik or the other namespaces. The script refuses to reuse or
  delete a namespace it did not create itself, in case yours is called `lmbot` too.
- **`check` first**: it lists what runs, the memory and disk left, the default storage class, the existing ingress
  host names (to avoid a conflict) and warns before installing. SQL Server needs 2 GB of RAM: if the node is short, the
  script says so instead of starving your bot.
- **Resource limits** on both pods (SQL Server 2 GB, console 512 MB), so they cannot take more than that.
- **Network policies** inside the namespace: SQL Server only accepts the console, the console only accepts Traefik.
- **The image is imported into k3s' containerd** (`k3s ctr images import`): this restarts nothing.
- **Server-side dry run** before anything is applied.
- **`upgrade`** rebuilds the image and restarts only the console; SQL Server and its volume keep running. The bots that
  were running restart by themselves.
- **`uninstall`** deletes only the `lmbot` namespace, after a confirmation, and warns that the database goes with it.
- Passwords are generated once; an existing secret is never overwritten.

The manual steps below are what the script does, for those who prefer to run them by hand.

## Automatic deployment (GitHub Actions)

`.github/workflows/deploy.yml` runs on every push to `main` (or `master`): tests, builds the image, pushes it to
`ghcr.io/<owner>/<repo>` (tags `latest` and the commit), then runs `deploy/deploy.sh upgrade` with that image
directly on the server. Only the console restarts; SQL Server keeps running.

The `deploy` job runs on a **self-hosted runner** installed on the server itself, not over SSH: this works even
when the server has no inbound port open to the internet (behind Tailscale, no DMZ, etc.), since the runner only
opens an outbound HTTPS connection to GitHub to poll for jobs. Because this workflow only triggers on `push` and
`workflow_dispatch` (never `pull_request`), a fork's pull request can never run code on the server, which is what
makes a self-hosted runner safe to use even on a public repository.

Setup, once k3s and the first `deploy/deploy.sh install` are done:

1. On GitHub: repo → *Settings → Actions → Runners → New self-hosted runner*, choose Linux/x64.
2. On the server, as the `deploy` user (the same one that ran `install`, so it already has `kubectl` and `docker`
   access): follow the download/config commands GitHub shows you. When asked for a name, a label is optional
   (the workflow targets the plain `self-hosted` label).
3. Install it as a service so it survives reboots and keeps polling unattended:
   ```bash
   sudo ./svc.sh install deploy
   sudo ./svc.sh start
   ```
4. The registration token GitHub gives you is single-use and expires quickly; if it errors out, generate a new one
   from the same *Runners* page.

No `DEPLOY_HOST`/`DEPLOY_SSH_KEY`/etc. secrets are needed with this setup.

## k3s

1. **Build the image and give it to k3s** (on the node, or from your machine):

   ```bash
   docker build -t lmbot:latest .
   docker save lmbot:latest | sudo k3s ctr images import -
   ```

   With a registry instead, push the image there and change `image:` in `deploy/k8s/bot.yaml`.

2. **Secret** (not in the repository):

   ```bash
   kubectl create namespace lmbot
   kubectl -n lmbot create secret generic lmbot-secrets \
     --from-literal=sa-password='Chang3-me-Str0ng!' --from-literal=token="$(openssl rand -hex 24)"
   ```

3. **Address**: in `deploy/k8s/bot.yaml`, replace `lmbot.example.com` (the Ingress host and
   `LMBOT_ALLOWED_ORIGINS`). Without an ingress, skip it and use `kubectl -n lmbot port-forward svc/lmbot 8765:80`.

4. **Deploy**:

   ```bash
   kubectl apply -k deploy/k8s
   kubectl -n lmbot get pods -w
   ```

   Open `https://<host>/#t=<the token>`. Read the token back with
   `kubectl -n lmbot get secret lmbot-secrets -o jsonpath='{.data.token}' | base64 -d`.

Points to know:

- **One console only** (`replicas: 1`, strategy `Recreate`): two consoles would start the same bots and each login
  kicks the other.
- **SQL Server needs 2 GB of RAM** on the node, otherwise it does not start. The manifest caps it at 1 GB of buffer.
  Express edition is free, limited to 10 GB per database: far more than this needs.
- **Automatic capture** (the button of the "Add an account" page) only exists on Windows. In a container, capture the
  login on your PC and import the file, or use the extractor: see `docs/credentials.md`.
- **Backups.** The volume is on the node (`/var/lib/rancher/k3s/storage`). The database holds **the access keys of
  your accounts**: back it up like a secret. `deploy/deploy.sh backup [file.bak]` saves it to your machine while
  everything keeps running, and `deploy/deploy.sh restore file.bak` puts it back (it stops the console during the
  restore and starts it again).
- **Security.** Keys are stored in clear in the `accounts` table (like in the `.cfg` files). Do not expose port 1433
  (the Service is headless, so it is not reachable from outside the cluster), and put the console behind HTTPS.
- **Resources.** About 10 MB per bot and one thread each: a `512Mi` limit is enough for a dozen accounts.

## Not tested against a live server

The storage layer and the manifests were checked for syntax and against the file store's test suite. They have not been
run against a real SQL Server or a k3s cluster yet: run the Compose test first and report any error.
