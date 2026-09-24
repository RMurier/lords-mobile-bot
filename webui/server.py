#!/usr/bin/env python3
"""
Web interface to configure and run the Lords Mobile bot.

    python3 webui/server.py            (Windows: webui.bat)

Standard library only. The server listens on 127.0.0.1 and hands out a random
access token in the URL it opens, because the configuration files contain live
account credentials.

Accounts are the `accounts/<name>.cfg` files. Every account has its own bot
process and its own log file (`logs/<name>.log`), so adding accounts later only
means adding config files.
"""

import argparse
import base64
import json
import mimetypes
import os
import re
import secrets
import shutil
import subprocess
import sys
import tempfile
import threading
import time
import urllib.error
import urllib.request
import webbrowser
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import parse_qs, urlparse

HERE = Path(__file__).resolve().parent
REPO = HERE.parent
# Accounts, logs and interface settings live under ROOT (overridable for tests).
ROOT = Path(os.environ.get("LMBOT_UI_ROOT") or REPO).resolve()
sys.path.insert(0, str(HERE))
sys.path.insert(0, str(REPO / "tools"))

import config_file as cf  # noqa: E402
import extract_credentials as ec  # noqa: E402
import schema  # noqa: E402
import store as storage  # noqa: E402

STATIC_DIR = HERE / "static"

ACCOUNT_ID = re.compile(r"^[A-Za-z0-9][A-Za-z0-9_-]{0,63}$")
MAX_JSON = 1 << 20
MAX_UPLOAD = 1 << 30
IS_WINDOWS = os.name == "nt"


class ApiError(Exception):
    def __init__(self, status, message, **extra):
        super().__init__(message)
        self.status, self.message, self.extra = status, message, extra


# --------------------------------------------------------------------------
# Settings of the interface itself (not of the bot)
# --------------------------------------------------------------------------

class Settings:
    def __init__(self):
        self.lock = threading.Lock()
        self.data = {"client_path": "", "stagger": 5, "aliases": {}, "tags": {}, "autostart": [], "remote_url": "", "remote_token": ""}
        self.data.update(store.get_settings())

    def save(self):
        store.put_settings(self.data)

    def get(self, key):
        with self.lock:
            return self.data[key]

    def update(self, **values):
        with self.lock:
            self.data.update(values)
            self.save()


def detect_clients():
    """Existing client executables, best candidates first."""
    exe = "client.exe" if IS_WINDOWS else "client"
    candidates = [Path(os.environ["LMBOT_CLIENT"])] if os.environ.get("LMBOT_CLIENT") else []
    candidates += [
        ROOT / "build" / exe,
        ROOT / "build" / "Release" / exe,
        ROOT / exe,
        Path.home() / exe,
    ]
    found = shutil.which("client")
    if found:
        candidates.append(Path(found))
    seen, result = set(), []
    for path in candidates:
        if path.is_file() and str(path) not in seen:
            seen.add(str(path))
            result.append(str(path))
    return result


# --------------------------------------------------------------------------
# Accounts
# --------------------------------------------------------------------------

def account_path(account_id):
    """Checks that the account exists and gives its id back."""
    if not ACCOUNT_ID.match(account_id or ""):
        raise ApiError(400, "Identifiant de compte invalide.")
    if not store.has_account(account_id):
        raise ApiError(404, "Compte introuvable.")
    return account_id


def list_account_ids():
    return store.list_accounts(ACCOUNT_ID)


def merged_admins(present):
    """Administrators of a config: the legacy admin.name and admin.names are merged, like the bot does."""
    names = []
    for raw in (present.get("admin.name", ""), present.get("admin.names", "")):
        for part in raw.split(","):
            part = part.strip()
            if part and part not in names:
                names.append(part)
    return ", ".join(names)


def account_payload(account_id):
    text = store.get_cfg(account_path(account_id))
    present = cf.read_values(text)
    values, secrets_info, defaults_used = {}, {}, []
    for key, field in schema.FIELDS.items():
        if key == "admin.names":
            if "admin.name" in present or key in present:
                values[key] = merged_admins(present)
            else:
                values[key] = field["default"]
                defaults_used.append(key)
        elif field["type"] == "secret":
            value = present.get(key, "")
            secrets_info[key] = {"set": bool(value), "length": len(value),
                                 "preview": value[:4] + "…" if len(value) > 8 else ""}
            values[key] = ""
        elif key in present:
            value = present[key]
            values[key] = ("true" if value.lower() in ("true", "1") else "false") if field["type"] == "bool" else value
        else:
            values[key] = field["default"]
            defaults_used.append(key)
    return {
        "id": account_id,
        "alias": settings.get("aliases").get(account_id, ""),
        "tag": settings.get("tags").get(account_id, ""),
        "values": values,
        "secrets": secrets_info,
        "defaults_used": defaults_used,
        "extra": cf.extra_keys(text),
        "status": bots.status(account_id),
    }


def account_summary(account_id):
    try:
        present = cf.read_values(store.get_cfg(account_id))
    except (OSError, storage.StoreError):
        present = {}
    return {
        "id": account_id,
        "alias": settings.get("aliases").get(account_id, ""),
        "tag": settings.get("tags").get(account_id, ""),
        "igg_id": present.get("account.igg_id", ""),
        "has_key": bool(present.get("account.access_key")),
        "status": bots.status(account_id),
    }


def save_changes(account_id, changes):
    account_path(account_id)
    normalised, errors = {}, {}
    for key, raw in changes.items():
        field = schema.FIELDS.get(key)
        if field is None:
            errors[key] = "Réglage inconnu."
            continue
        if field["type"] == "secret" and (raw is None or raw == ""):
            continue  # an empty secret means "keep the current one"
        try:
            normalised[key] = schema.validate(field, raw)
        except ValueError as e:
            errors[key] = str(e)
    if errors:
        raise ApiError(400, "Certains réglages sont invalides.", errors=errors)
    if "admin.names" in normalised:
        normalised["admin.name"] = ""  # the legacy single-admin key is now part of admin.names
    if normalised:
        store.put_cfg(account_id, cf.apply_changes(store.get_cfg(account_id), normalised))
    return account_payload(account_id)


def new_account_id(preferred):
    base = re.sub(r"[^A-Za-z0-9_-]+", "-", preferred or "").strip("-")[:48] or "compte"
    if not base[0].isalnum():
        base = "compte-" + base
    candidate, n = base, 2
    while store.has_account(candidate):
        candidate, n = f"{base}-{n}", n + 1
    return candidate


def create_account(name, copy_from=None):
    account_id = new_account_id(name)
    values = {}
    if copy_from:
        source = cf.read_values(store.get_cfg(account_path(copy_from)))
        values = {k: v for k, v in source.items() if k in schema.FIELDS and not k.startswith("account.")}
        admins = merged_admins(source)
        if admins:
            values["admin.names"] = admins
    values["data.path"] = f"./data/{account_id}/"
    store.put_cfg(account_id, cf.render_new(values), keep_backup=False)
    return account_id


def credential_updates(account):
    updates = {
        "account.igg_id": str(account["igg_id"]),
        "account.access_key": account["access_key"],
        "account.device_uuid": account.get("device_uuid", ""),
    }
    for key, field in (("client.version_major", "version_major"), ("client.version_minor", "version_minor"),
                       ("client.version_patch", "version_patch"), ("client.language_code", "language_code"),
                       ("client.platform", "platform"), ("server.addr", "server_addr"),
                       ("server.port", "server_port")):
        if field in account:
            updates[key] = str(account[field])
    return updates


BUNDLE_VERSION = 1
BUNDLE_SETTINGS = ("stagger", "aliases", "tags")     # what belongs to the accounts, not to this computer


def export_bundle():
    """Everything needed to rebuild the accounts elsewhere: configurations, in-game administrators, names."""
    accounts = {}
    for account_id in list_account_ids():
        accounts[account_id] = {"cfg": store.get_cfg(account_id), "admins": store.get_admins(account_id)}
    return {"format": "lmbot-export", "version": BUNDLE_VERSION, "exported_at": int(time.time()),
            "settings": {key: settings.get(key) for key in BUNDLE_SETTINGS}, "accounts": accounts}


def import_bundle(bundle, overwrite=False):
    if not isinstance(bundle, dict) or bundle.get("format") != "lmbot-export" \
            or not isinstance(bundle.get("accounts"), dict):
        raise ApiError(400, "Ce fichier n'est pas une sauvegarde de la console.")
    if bundle.get("version") != BUNDLE_VERSION:
        raise ApiError(400, f"Version de sauvegarde inconnue : {bundle.get('version')}.")
    imported, skipped = [], []
    aliases = dict(settings.get("aliases"))
    tags = dict(settings.get("tags"))
    for account_id, entry in bundle["accounts"].items():
        if not ACCOUNT_ID.match(str(account_id)) or not isinstance(entry, dict) or not isinstance(entry.get("cfg"), str):
            skipped.append({"id": str(account_id)[:64], "reason": "entrée invalide"})
            continue
        exists = store.has_account(account_id)
        if exists and not overwrite:
            skipped.append({"id": account_id, "reason": "existe déjà"})
            continue
        if exists and bots.status(account_id)["state"] == "running":
            skipped.append({"id": account_id, "reason": "son bot tourne : arrêtez-le d'abord"})
            continue
        # a data folder of the old computer (C:\...) means nothing here: use the standard one
        text = cf.apply_changes(entry["cfg"], {"data.path": f"./data/{account_id}/"})
        store.put_cfg(account_id, text, keep_backup=exists)
        if isinstance(entry.get("admins"), str):
            store.put_admins(account_id, entry["admins"])
        alias = (bundle.get("settings", {}).get("aliases") or {}).get(account_id)
        if isinstance(alias, str) and alias:
            aliases[account_id] = alias[:40]
        tag = (bundle.get("settings", {}).get("tags") or {}).get(account_id)
        if isinstance(tag, str) and tag:
            tags[account_id] = tag[:20]
        imported.append(account_id)
    updates = {"aliases": aliases, "tags": tags}
    stagger = bundle.get("settings", {}).get("stagger")
    if isinstance(stagger, int) and 0 <= stagger <= 600 and not skipped and imported:
        updates["stagger"] = stagger
    settings.update(**updates)
    return {"imported": imported, "skipped": skipped}


def remote_call(path, payload):
    """POST JSON to the other console (the server)."""
    base = str(settings.get("remote_url") or "").strip()
    if not base:
        raise ApiError(409, "Aucun serveur distant n'est configuré (Paramètres → Serveur distant).")
    data = json.dumps(payload).encode("utf-8")
    request = urllib.request.Request(base.rstrip("/") + path, data=data, method="POST", headers={
        "X-Token": str(settings.get("remote_token") or ""), "Content-Type": "application/json"})
    try:
        with urllib.request.urlopen(request, timeout=60) as response:
            return json.loads(response.read().decode("utf-8"))
    except urllib.error.HTTPError as e:
        try:
            message = json.loads(e.read().decode("utf-8")).get("error", "")
        except (ValueError, OSError):
            message = ""
        if e.code == 401:
            message = "Le serveur distant refuse le jeton : vérifiez-le dans les paramètres."
        raise ApiError(502, f"Console distante : {message or 'erreur HTTP %d' % e.code}")
    except (urllib.error.URLError, OSError, ValueError) as e:
        raise ApiError(502, f"Console distante injoignable ({base}) : {getattr(e, 'reason', e)}")


def push_capture(capture_path, base_url, token):
    """Sends the capture to another console (the one on the server), which does the import."""
    url = base_url.rstrip("/") + "/api/import"
    size = os.path.getsize(capture_path)
    with open(capture_path, "rb") as body:
        request = urllib.request.Request(url, data=body, method="POST", headers={
            "X-Token": token, "Content-Type": "application/octet-stream", "Content-Length": str(size)})
        try:
            with urllib.request.urlopen(request, timeout=120) as response:
                result = json.loads(response.read().decode("utf-8"))
        except urllib.error.HTTPError as e:
            try:
                message = json.loads(e.read().decode("utf-8")).get("error", "")
            except (ValueError, OSError):
                message = ""
            if e.code == 401:
                message = "Le serveur distant refuse le jeton : vérifiez-le dans les paramètres."
            raise ApiError(502, f"Console distante : {message or 'erreur HTTP %d' % e.code}")
        except (urllib.error.URLError, OSError, ValueError) as e:
            raise ApiError(502, f"Console distante injoignable ({base_url}) : {getattr(e, 'reason', e)}")
    accounts = result.get("accounts") or []
    for account in accounts:
        account["remote"] = True
    return accounts


def import_capture(capture_path):
    remote = str(settings.get("remote_url") or "").strip()
    if remote:
        # this computer has the game, the server has the bots: the identifiers go straight to the server
        return push_capture(capture_path, remote, str(settings.get("remote_token") or ""))
    try:
        found = ec.extract(capture_path)
    except (OSError, ValueError) as e:
        raise ApiError(400, f"Capture illisible : {e}")
    if not found:
        raise ApiError(422, "Aucun login trouvé dans la capture. Lancez la capture AVANT d'ouvrir le jeu et "
                            "arrêtez-la seulement une fois en jeu.")
    results = []
    for account in found:
        account_id = str(account["igg_id"])
        updates = credential_updates(account)
        created = not store.has_account(account_id)
        if created:
            values = {k: v for k, v in updates.items() if v != ""}
            values["data.path"] = f"./data/{account_id}/"
            store.put_cfg(account_id, cf.render_new(values), keep_backup=False)
        else:
            store.put_cfg(account_id, cf.apply_changes(store.get_cfg(account_id), updates))
        results.append({
            "id": account_id, "created": created,
            "version": "{}.{}.{}".format(*(account.get(k, "?") for k in
                                            ("version_major", "version_minor", "version_patch"))),
            "gateway": f"{account['server_addr']}:{account['server_port']}" if "server_addr" in account else "",
            "key_length": len(account["access_key"]),
        })
    return results


# --------------------------------------------------------------------------
# Bot processes
# --------------------------------------------------------------------------

class Bots:
    """One bot process per account; what it prints goes to the store (a file or the database)."""

    def __init__(self):
        self.lock = threading.Lock()
        self.procs = {}   # account id -> {"proc", "since", "pump"}
        self.exited = {}  # account id -> exit code of the last run

    def _alive(self, account_id):
        entry = self.procs.get(account_id)
        if entry and entry["proc"].poll() is None:
            return entry
        if entry:
            self.exited[account_id] = entry["proc"].returncode
            del self.procs[account_id]
        return None

    def status(self, account_id):
        with self.lock:
            entry = self._alive(account_id)
            if entry:
                return {"state": "running", "pid": entry["proc"].pid, "since": entry["since"]}
            status = {"state": "stopped"}
            if account_id in self.exited:
                status["exit_code"] = self.exited[account_id]
            last = store.last_log_line(account_id)
            if last:
                status["last_log"] = last
            return status

    def start(self, account_id):
        account_path(account_id)
        client = client_path()
        if not client:
            raise ApiError(409, "Exécutable du bot introuvable. Compilez le bot (build.bat) ou indiquez son "
                                "chemin dans les paramètres.")
        present = cf.read_values(store.get_cfg(account_id))
        missing = [label for key, label in (("account.igg_id", "IGG ID"), ("account.access_key", "clé d'accès"))
                   if not present.get(key)]
        if missing:
            raise ApiError(409, "Compte incomplet : " + " et ".join(missing) + " manquant(e).")
        with self.lock:
            if self._alive(account_id):
                raise ApiError(409, "Ce bot est déjà démarré.")
            path = store.cfg_path(account_id)
            store.prepare_start(account_id)
            store.append_log(account_id, f"\n--- {time.strftime('%Y-%m-%d %H:%M:%S')} démarrage ---\n")
            flags = subprocess.CREATE_NO_WINDOW if IS_WINDOWS else 0
            try:
                proc = subprocess.Popen([client, str(path)], cwd=str(ROOT), stdin=subprocess.DEVNULL,
                                        stdout=subprocess.PIPE, stderr=subprocess.STDOUT, bufsize=0,
                                        creationflags=flags)
            except OSError as e:
                raise ApiError(500, f"Impossible de lancer le bot : {e}")
            pump = threading.Thread(target=self._pump, args=(account_id, proc), daemon=True)
            pump.start()
            self.exited.pop(account_id, None)
            self.procs[account_id] = {"proc": proc, "since": time.time(), "pump": pump}
        return self.status(account_id)

    def stop(self, account_id):
        with self.lock:
            entry = self._alive(account_id)
            if not entry:
                return
            proc = entry["proc"]
            proc.terminate()
            try:
                proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.wait()
            entry["pump"].join(timeout=2)
            store.append_log(account_id, f"--- {time.strftime('%Y-%m-%d %H:%M:%S')} arrêté depuis l'interface ---\n")
        with self.lock:
            self._alive(account_id)
            self.exited.pop(account_id, None)

    def stop_all(self):
        for account_id in list(self.procs):
            self.stop(account_id)

    @staticmethod
    def _pump(account_id, proc):
        """Copies what the bot prints into the store until it exits."""
        try:
            while True:
                chunk = proc.stdout.read(4096)
                if not chunk:
                    break
                store.append_log(account_id, chunk.decode("utf-8", "replace"))
        except (OSError, ValueError):
            pass
        finally:
            try:
                proc.stdout.close()
            except OSError:
                pass


def client_path():
    configured = settings.get("client_path")
    if configured and Path(configured).is_file():
        return configured
    found = detect_clients()
    return found[0] if found else ""


try:
    store = storage.open_store(ROOT)
except storage.StoreError as e:
    sys.exit(f"[webui] {e}")
settings = Settings()
bots = Bots()


def remember_autostart(account_id, wanted):
    """Which bots were running when the console stopped: they are started again with it."""
    with settings.lock:
        current = list(settings.data.get("autostart") or [])
    if wanted and account_id not in current:
        current.append(account_id)
    elif not wanted and account_id in current:
        current.remove(account_id)
    else:
        return
    settings.update(autostart=current)


def sync_loop():
    """Every second, group the bot output into the database; every 5 s, save what the bots keep in their folder."""
    tick = 0
    while True:
        time.sleep(1)
        tick += 1
        try:
            flush = getattr(store, "flush_logs", None)
            if flush:
                flush()
            if tick % 5 == 0 and (store.persists_game or store.persists_admins):
                for account_id in list(bots.procs):
                    folder = ROOT / "data" / account_id
                    for name, persist, put in (("status.json", store.persists_game, store.put_game),
                                               ("admins.txt", store.persists_admins, store.put_admins)):
                        if not persist:
                            continue
                        try:
                            text = (folder / name).read_text(encoding="utf-8")
                        except OSError:
                            continue
                        try:
                            put(account_id, json.loads(text) if name.endswith(".json") else text)
                        except ValueError:
                            pass    # the bot was writing the file: next time
        except Exception as e:  # noqa: BLE001 - a database hiccup must not stop the loop
            print(f"[webui] synchronisation : {type(e).__name__}: {e}", file=sys.stderr, flush=True)


# --------------------------------------------------------------------------
# HTTP
# --------------------------------------------------------------------------

class Handler(BaseHTTPRequestHandler):
    server_version = "LordsBotUI"
    protocol_version = "HTTP/1.1"

    routes = []  # (method, regex, function)

    def log_message(self, fmt, *args):
        pass  # keep the console readable; errors are printed explicitly

    # -- plumbing -----------------------------------------------------------

    def _send(self, status, body, content_type="application/json; charset=utf-8", extra=None):
        if isinstance(body, (dict, list)):
            body = json.dumps(body).encode("utf-8")
        elif isinstance(body, str):
            body = body.encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.send_header("X-Content-Type-Options", "nosniff")
        self.send_header("X-Frame-Options", "DENY")
        self.send_header("Content-Security-Policy",
                         "default-src 'self'; style-src 'self' 'unsafe-inline'; img-src 'self' data:; "
                         "frame-ancestors 'none'")
        for key, value in (extra or {}).items():
            self.send_header(key, value)
        self.end_headers()
        self.wfile.write(body)

    def _host_ok(self):
        """Refuses unknown Host names (DNS rebinding): this machine, plus the addresses of LMBOT_ALLOWED_ORIGINS."""
        host = (self.headers.get("Host") or "").lower()
        name = host.rsplit(":", 1)[0] if not host.endswith("]") else host
        if name in ("127.0.0.1", "localhost"):     # any local port: kubectl port-forward, ssh -L...
            return True
        allowed = {urlparse(o.strip() if "//" in o else f"//{o.strip()}").netloc.lower() for o in os.environ.get("LMBOT_ALLOWED_ORIGINS", "").split(",")
                   if o.strip()}
        return host in allowed or name in {a.rsplit(":", 1)[0] for a in allowed}

    def _dispatch(self, method):
        parsed = urlparse(self.path)
        try:
            if not self._host_ok():
                raise ApiError(403, "Hôte non autorisé.")
            if parsed.path.startswith("/api/"):
                self._check_api(method)
                for route_method, regex, function in self.routes:
                    match = regex.match(parsed.path)
                    if route_method == method and match:
                        result = function(self, parse_qs(parsed.query), *match.groups())
                        self._send(200, result if result is not None else {"ok": True})
                        return
                raise ApiError(404, "Route inconnue.")
            if method != "GET":
                raise ApiError(405, "Méthode non autorisée.")
            self._static(parsed.path)
        except ApiError as e:
            self._send(e.status, {"error": e.message, **e.extra})
        except (BrokenPipeError, ConnectionResetError):
            pass
        except Exception as e:  # noqa: BLE001 - never leave a request hanging
            print(f"[webui] {method} {parsed.path}: {type(e).__name__}: {e}", file=sys.stderr)
            self._send(500, {"error": "Erreur interne du serveur."})

    def _check_api(self, method):
        supplied = self.headers.get("X-Token") or ""
        if not secrets.compare_digest(supplied, self.server.token):
            raise ApiError(401, "Jeton d'accès invalide. Rouvrez le lien affiché dans la console.")
        if method != "GET":
            origin = self.headers.get("Origin")
            allowed = {f"http://127.0.0.1:{self.server.server_address[1]}",
                       f"http://localhost:{self.server.server_address[1]}"}
            allowed.update(o.strip().lower().rstrip("/") for o in os.environ.get("LMBOT_ALLOWED_ORIGINS", "").split(",")
                           if o.strip())
            if origin and origin.lower() not in allowed:
                raise ApiError(403, "Origine non autorisée.")

    def _static(self, path):
        name = "index.html" if path in ("/", "") else path.lstrip("/")
        target = (STATIC_DIR / name).resolve()
        if STATIC_DIR.resolve() not in target.parents or not target.is_file():
            raise ApiError(404, "Introuvable.")
        content_type = mimetypes.guess_type(target.name)[0] or "application/octet-stream"
        if content_type.startswith("text/") or content_type.endswith("javascript"):
            content_type += "; charset=utf-8"
        self._send(200, target.read_bytes(), content_type)

    def read_json(self):
        length = int(self.headers.get("Content-Length") or 0)
        if length > MAX_JSON:
            raise ApiError(413, "Requête trop grande.")
        try:
            data = json.loads(self.rfile.read(length) or b"{}")
        except ValueError:
            raise ApiError(400, "JSON invalide.")
        if not isinstance(data, dict):
            raise ApiError(400, "Objet JSON attendu.")
        return data

    def do_GET(self):
        self._dispatch("GET")

    def do_POST(self):
        self._dispatch("POST")

    def do_PUT(self):
        self._dispatch("PUT")

    def do_DELETE(self):
        self._dispatch("DELETE")


def route(method, pattern):
    def decorator(function):
        Handler.routes.append((method, re.compile(f"^{pattern}$"), function))
        return function
    return decorator


@route("GET", "/api/state")
def api_state(h, query):
    detected = detect_clients()
    return {
        "accounts": [account_summary(a) for a in list_account_ids()],
        "schema": schema.public_schema(),
        "settings": {"client_path": settings.get("client_path"), "client_used": client_path(),
                     "client_detected": detected, "stagger": settings.get("stagger"),
                     "remote_url": settings.get("remote_url"), "remote_token_set": bool(settings.get("remote_token"))},
        "os": "windows" if IS_WINDOWS else "posix",
    }


@route("GET", "/api/accounts/([^/]+)")
def api_account_get(h, query, account_id):
    return account_payload(account_id)


@route("PUT", "/api/accounts/([^/]+)")
def api_account_put(h, query, account_id):
    changes = h.read_json().get("changes")
    if not isinstance(changes, dict):
        raise ApiError(400, "Champ « changes » attendu.")
    return save_changes(account_id, changes)


@route("DELETE", "/api/accounts/([^/]+)")
def api_account_delete(h, query, account_id):
    account_path(account_id)
    if bots.status(account_id)["state"] == "running":
        raise ApiError(409, "Arrêtez le bot avant de supprimer le compte.")
    store.delete_account(account_id)
    remember_autostart(account_id, False)
    aliases = dict(settings.get("aliases"))
    if aliases.pop(account_id, None) is not None:
        settings.update(aliases=aliases)
    tags = dict(settings.get("tags"))
    if tags.pop(account_id, None) is not None:
        settings.update(tags=tags)


@route("POST", "/api/accounts")
def api_account_create(h, query):
    body = h.read_json()
    copy_from = body.get("copy_from") or None
    name = str(body.get("name") or "").strip()
    account_id = create_account(name, copy_from)
    if name and name != account_id:
        # the typed name is the display name; the file name is a safe version of it
        aliases = dict(settings.get("aliases"))
        aliases[account_id] = name[:40]
        settings.update(aliases=aliases)
    return account_payload(account_id)


@route("PUT", "/api/accounts/([^/]+)/alias")
def api_account_alias(h, query, account_id):
    account_path(account_id)
    alias = str(h.read_json().get("alias") or "").strip()[:40]
    aliases = dict(settings.get("aliases"))
    if alias:
        aliases[account_id] = alias
    else:
        aliases.pop(account_id, None)
    settings.update(aliases=aliases)
    return {"alias": alias}


@route("PUT", "/api/accounts/([^/]+)/tag")
def api_account_tag(h, query, account_id):
    account_path(account_id)
    tag = str(h.read_json().get("tag") or "").strip()[:20]
    tags = dict(settings.get("tags"))
    if tag:
        tags[account_id] = tag
    else:
        tags.pop(account_id, None)
    settings.update(tags=tags)
    return {"tag": tag}


@route("POST", "/api/accounts/([^/]+)/(start|stop|restart)")
def api_account_control(h, query, account_id, action):
    account_path(account_id)
    if action in ("stop", "restart"):
        bots.stop(account_id)
        remember_autostart(account_id, False)
    if action in ("start", "restart"):
        status = bots.start(account_id)
        remember_autostart(account_id, True)
        return status
    return bots.status(account_id)


@route("GET", "/api/accounts/([^/]+)/logs")
def api_account_logs(h, query, account_id):
    account_path(account_id)
    try:
        offset = int(query.get("offset", ["-1"])[0])
    except ValueError:
        offset = -1
    return {**store.read_log(account_id, offset), "status": bots.status(account_id)}


@route("GET", "/api/accounts/([^/]+)/game")
def api_account_game(h, query, account_id):
    """What the bot knows about the account in game: the status.json it writes every few seconds."""
    account_path(account_id)
    running = bots.status(account_id)["state"] == "running"
    data = store.get_game(account_id)
    if data is None:
        return {"available": False, "running": running}
    age = time.time() - data.get("written_at", 0)
    # the file outlives the bot: it only counts as live while the process runs and keeps writing
    return {"available": True, "running": running, "live": running and age < 30 and bool(data.get("connected")),
            "age": max(0, int(age)), "data": data}


@route("POST", "/api/accounts/([^/]+)/chat/send")
def api_account_chat_send(h, query, account_id):
    """Queues a guild chat message: dropped where the bot polls for it (like admins.txt, the folder
    it actually reads from at runtime is the same for both stores - see SqlStore.prepare_start)."""
    account_path(account_id)
    if bots.status(account_id)["state"] != "running":
        raise ApiError(409, "Le bot n'est pas démarré.")
    message = str(h.read_json().get("message") or "").strip()
    if not message:
        raise ApiError(400, "Message vide.")
    message = message.splitlines()[0][:240]
    folder = ROOT / "data" / account_id
    folder.mkdir(parents=True, exist_ok=True)
    (folder / "chat_outbox.txt").write_text(message, encoding="utf-8")
    return {"ok": True}


@route("POST", "/api/import")
def api_import(h, query):
    length = int(h.headers.get("Content-Length") or 0)
    if length <= 0 or length > MAX_UPLOAD:
        raise ApiError(413, "Capture vide ou trop grande (1 Go maximum).")
    fd, tmp = tempfile.mkstemp(prefix="capture-", suffix=".bin")
    try:
        with os.fdopen(fd, "wb") as out:
            remaining = length
            while remaining:
                chunk = h.rfile.read(min(1 << 20, remaining))
                if not chunk:
                    raise ApiError(400, "Envoi interrompu.")
                out.write(chunk)
                remaining -= len(chunk)
        return {"accounts": import_capture(tmp)}
    finally:
        os.unlink(tmp)


@route("GET", "/api/data/export")
def api_data_export(h, query):
    return export_bundle()


@route("POST", "/api/data/import")
def api_data_import(h, query):
    overwrite = query.get("overwrite", ["0"])[0] == "1"
    return import_bundle(h.read_json(), overwrite)


@route("POST", "/api/data/push")
def api_data_push(h, query):
    """Sends this console's accounts to the server's console."""
    overwrite = query.get("overwrite", ["0"])[0] == "1"
    if not list_account_ids():
        raise ApiError(409, "Aucun compte à envoyer.")
    return remote_call("/api/data/import" + ("?overwrite=1" if overwrite else ""), export_bundle())


@route("PUT", "/api/settings")
def api_settings(h, query):
    body = h.read_json()
    updates = {}
    if "client_path" in body:
        path = str(body["client_path"] or "").strip().strip('"')
        if path and not Path(path).is_file():
            raise ApiError(400, "Ce fichier n'existe pas.", errors={"client_path": "Fichier introuvable."})
        updates["client_path"] = path
    if "stagger" in body:
        try:
            stagger = int(body["stagger"])
        except (TypeError, ValueError):
            raise ApiError(400, "Délai invalide.", errors={"stagger": "Nombre entier attendu."})
        if not 0 <= stagger <= 600:
            raise ApiError(400, "Délai invalide.", errors={"stagger": "Entre 0 et 600 secondes."})
        updates["stagger"] = stagger
    if "remote_url" in body:
        remote = str(body["remote_url"] or "").strip().rstrip("/")
        if remote and not re.match(r"^https?://[^\s/]+(/\S*)?$", remote):
            raise ApiError(400, "Adresse invalide.", errors={"remote_url": "Adresse du type https://bot.exemple.com"})
        updates["remote_url"] = remote
    if body.get("remote_token"):
        updates["remote_token"] = str(body["remote_token"]).strip()
    if "remote_url" in updates and not updates["remote_url"]:
        updates["remote_token"] = ""      # no server, no reason to keep its token
    settings.update(**updates)
    return api_state(h, {})["settings"]


# --------------------------------------------------------------------------
# Capture the game's login from this computer (Windows, pktmon)
# --------------------------------------------------------------------------
#
# A helper started with administrator rights runs pktmon: it starts the capture, waits for a
# "stop" file, stops, converts to pcapng and writes done.json. The console talks to it only through
# files in a private temporary directory:
#
#   started    the capture is running          stop / cancel   asked by the console
#   done.json  {"status": "ok" | "error" | "cancelled", "message": "..."}
#
# Because the helper is elevated, its code is passed on its command line (-EncodedCommand), never as a
# file someone else could edit between the launch and the elevation, it calls pktmon by absolute
# path and it refuses a working directory that is a link. It only ever reads whether "stop" and
# "cancel" exist. The capture holds the whole game login, so it is deleted as soon as it is imported.

CAPTURE_START_TIMEOUT = 90     # seconds to accept the Windows administrator prompt
CAPTURE_STOP_TIMEOUT = 120     # seconds to stop, convert and read the capture
CAPTURE_MAX_MINUTES = 30       # the helper stops by itself after this

POWERSHELL_HELPER = r"""
$ErrorActionPreference = 'Stop'
$dir  = '@DIR@'
$etl  = Join-Path $dir 'capture.etl'
$pcap = Join-Path $dir 'capture.pcapng'
$pkt  = Join-Path $env:SystemRoot 'System32\pktmon.exe'
function Done($status, $message) {
  $text = '{"status":"' + $status + '","message":"' + ($message -replace '["\\\r\n]', ' ') + '"}'
  Set-Content -LiteralPath (Join-Path $dir 'done.json') -Value $text -Encoding ASCII
}
try {
  if ((Get-Item -LiteralPath $dir -Force).Attributes -band [IO.FileAttributes]::ReparsePoint) { throw 'unsafe directory' }
  $filterArgs = @(@FILTER@)
  & $pkt filter remove | Out-Null
  & $pkt filter add LordsBot @filterArgs | Out-Null
  if ($LASTEXITCODE -ne 0) { throw 'pktmon filter add failed' }
  & $pkt start --capture --pkt-size 0 -f $etl | Out-Null
  if ($LASTEXITCODE -ne 0) { throw 'pktmon start failed (is another capture already running? try: pktmon stop)' }
  Set-Content -LiteralPath (Join-Path $dir 'started') -Value '1'
  $deadline = (Get-Date).AddMinutes(@MINUTES@)
  while (-not (Test-Path -LiteralPath (Join-Path $dir 'stop')) -and -not (Test-Path -LiteralPath (Join-Path $dir 'cancel')) -and (Get-Date) -lt $deadline) {
    Start-Sleep -Milliseconds 500
  }
  & $pkt stop | Out-Null
  & $pkt filter remove | Out-Null
  if (Test-Path -LiteralPath (Join-Path $dir 'cancel')) { Done 'cancelled' ''; exit 0 }
  & $pkt etl2pcap $etl -o $pcap | Out-Null
  if ($LASTEXITCODE -ne 0) { throw 'pktmon etl2pcap failed' }
  Done 'ok' ''
} catch {
  try { & $pkt stop | Out-Null; & $pkt filter remove | Out-Null } catch {}
  Done 'error' $_.Exception.Message
}
"""


def powershell_helper_script(directory, port=None):
    """The elevated helper. `port` limits the capture to that TCP port, None captures all TCP."""
    quote = lambda text: str(text).replace("'", "''")
    filter_args = "'-t','TCP'" + (f",'-p','{int(port)}'" if port else "")
    return (POWERSHELL_HELPER.replace("@DIR@", quote(directory)).replace("@FILTER@", filter_args)
            .replace("@MINUTES@", str(CAPTURE_MAX_MINUTES)))


def encode_powershell(script):
    return base64.b64encode(script.encode("utf-16-le")).decode("ascii")


class Capture:
    """At most one capture at a time. Everything happens in a private directory, deleted afterwards."""

    def __init__(self):
        self.lock = threading.Lock()
        self.dir = None
        self.proc = None
        self.started_at = 0.0
        self.finishing = False
        self.error = ""

    def _file(self, name):
        return os.path.join(self.dir, name)

    def _read_done(self):
        try:
            with open(self._file("done.json"), encoding="ascii", errors="replace") as f:
                return json.loads(f.read())
        except (OSError, ValueError, TypeError):
            return None

    def _cleanup(self):
        if self.dir:
            shutil.rmtree(self.dir, ignore_errors=True)
        self.dir = None
        self.proc = None
        self.finishing = False

    def available(self):
        return IS_WINDOWS or bool(os.environ.get("LMBOT_CAPTURE_HELPER"))

    def start(self, all_tcp=False, port=5999):
        if not self.available():
            raise ApiError(501, "La capture automatique n'est disponible que sous Windows (elle utilise pktmon). "
                                "Faites la capture avec Wireshark ou tcpdump puis importez le fichier.")
        with self.lock:
            if self.dir:
                raise ApiError(409, "Une capture est déjà en cours.")
            self.error = ""
            directory = tempfile.mkdtemp(prefix="lmbot-capture-")
            self.dir = directory
            self.started_at = time.time()
            self.finishing = False
            capture_port = None if all_tcp else port
            helper = os.environ.get("LMBOT_CAPTURE_HELPER")  # test hook: a script following the same file protocol
            try:
                if helper:
                    command = [sys.executable, helper, directory, str(capture_port or 0)]
                    flags = 0
                else:
                    script = encode_powershell(powershell_helper_script(directory, capture_port))
                    shell = os.path.join(os.environ.get("SystemRoot", r"C:\Windows"), "System32", "WindowsPowerShell",
                                         "v1.0", "powershell.exe")
                    launcher = ("Start-Process -FilePath '{}' -Verb RunAs -WindowStyle Hidden -ArgumentList "
                                "'-NoProfile','-ExecutionPolicy','Bypass','-EncodedCommand','{}'").format(
                                    shell.replace("'", "''"), script)
                    command = [shell, "-NoProfile", "-NonInteractive", "-Command", launcher]
                    flags = subprocess.CREATE_NO_WINDOW
                self.proc = subprocess.Popen(command, stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL,
                                             stderr=subprocess.DEVNULL, creationflags=flags)
            except OSError as e:
                self._cleanup()
                raise ApiError(500, f"Impossible de lancer la capture : {e}")
        return self.status()

    def status(self):
        with self.lock:
            if not self.dir:
                return {"state": "error", "message": self.error} if self.error else {"state": "idle"}
            if self.finishing:
                return {"state": "processing", "since": self.started_at}
            done = self._read_done()
            if done and done.get("status") in ("error", "cancelled"):
                self.error = done.get("message") or "La capture a échoué." if done["status"] == "error" else ""
                self._cleanup()
                return {"state": "error", "message": self.error} if self.error else {"state": "idle"}
            if os.path.exists(self._file("started")):
                return {"state": "recording", "since": self.started_at}
            elapsed = time.time() - self.started_at
            if self.proc is not None and self.proc.poll() not in (None, 0):
                self.error = ("L'autorisation administrateur a été refusée ou n'a pas pu être demandée. "
                              "La capture réseau en a besoin.")
                self._cleanup()
                return {"state": "error", "message": self.error}
            if elapsed > CAPTURE_START_TIMEOUT:
                open(self._file("cancel"), "w").close()
                self.error = "Windows n'a pas confirmé l'autorisation administrateur à temps. Recommencez."
                self._cleanup()
                return {"state": "error", "message": self.error}
            return {"state": "starting", "since": self.started_at}

    def stop(self):
        with self.lock:
            if not self.dir or self.finishing or not os.path.exists(self._file("started")):
                raise ApiError(409, "Aucune capture en cours.")
            self.finishing = True
            open(self._file("stop"), "w").close()
        deadline = time.time() + CAPTURE_STOP_TIMEOUT
        done = None
        while time.time() < deadline:
            done = self._read_done()
            if done:
                break
            time.sleep(0.3)
        try:
            if not done:
                raise ApiError(504, "L'arrêt de la capture a pris trop de temps.")
            if done.get("status") != "ok":
                raise ApiError(500, done.get("message") or "La capture a échoué.")
            return import_capture(self._file("capture.pcapng"))
        finally:
            with self.lock:
                self._cleanup()

    def cancel(self):
        with self.lock:
            if not self.dir:
                self.error = ""
                return
            try:
                open(self._file("cancel"), "w").close()
            except OSError:
                pass
            # give the helper a moment to stop pktmon before its directory disappears
            deadline = time.time() + 15
            while time.time() < deadline and not self._read_done():
                time.sleep(0.2)
            self._cleanup()
            self.error = ""


capture = Capture()


@route("GET", "/api/capture")
def api_capture_status(h, query):
    return {**capture.status(), "available": capture.available()}


@route("POST", "/api/capture/start")
def api_capture_start(h, query):
    body = h.read_json()
    return capture.start(all_tcp=bool(body.get("all_tcp")))


@route("POST", "/api/capture/stop")
def api_capture_stop(h, query):
    return {"accounts": capture.stop()}


@route("POST", "/api/capture/cancel")
def api_capture_cancel(h, query):
    capture.cancel()


def start_accounts(account_ids):
    """--start: start these accounts' bots, one after the other, and say what happened for each."""
    delay = int(settings.get("stagger") or 0)
    for number, account_id in enumerate(account_ids):
        if number and delay:
            time.sleep(delay)
        try:
            status = bots.start(account_id)
            print(f"[webui] bot {account_id} started (pid {status.get('pid')})", flush=True)
        except ApiError as e:
            print(f"[webui] bot {account_id} NOT started: {e.message}", flush=True)


class Server(ThreadingHTTPServer):
    daemon_threads = True
    allow_reuse_address = True


def main():
    parser = argparse.ArgumentParser(description="Web interface for the Lords Mobile bot")
    parser.add_argument("--port", type=int, default=8765, help="port to listen on (default 8765)")
    parser.add_argument("--no-browser", action="store_true", help="do not open the browser automatically")
    parser.add_argument("--start", nargs="+", metavar="ACCOUNT", default=[],
                        help="start the bots of these accounts as soon as the console is up (one after the other, "
                             "with the start delay of the settings)")
    parser.add_argument("--host", default=os.environ.get("LMBOT_HOST", "127.0.0.1"),
                        help="address to listen on (default 127.0.0.1; 0.0.0.0 in a container, with LMBOT_TOKEN)")
    args = parser.parse_args()

    local = args.host in ("127.0.0.1", "localhost")
    fixed_token = os.environ.get("LMBOT_TOKEN", "")
    if not local and len(fixed_token) < 16:
        sys.exit("[webui] Hors de cette machine, définissez LMBOT_TOKEN (16 caractères au moins) : "
                 "sans lui le jeton change à chaque démarrage.")

    server = None
    for port in range(args.port, args.port + (10 if local else 1)):
        try:
            server = Server((args.host, port), Handler)
            break
        except OSError:
            continue
    if server is None:
        sys.exit(f"Port {args.port} is already in use.")

    server.token = fixed_token or secrets.token_urlsafe(24)
    print("Lords Mobile Bot - web interface", flush=True)
    print(f"Stockage : {store.describe()}", flush=True)
    if isinstance(store, storage.SqlStore):
        imported = store.import_files(ROOT, ACCOUNT_ID)
        if imported:
            print(f"[webui] {imported} compte(s) importé(s) depuis les fichiers vers SQL Server", flush=True)
            settings.data.update(store.get_settings())
    if fixed_token:
        url = f"http://{'127.0.0.1' if local else args.host}:{server.server_address[1]}/"
        print(f"Interface : {url} (jeton : celui de LMBOT_TOKEN, à passer dans l'adresse : #t=<jeton>)", flush=True)
    else:
        url = f"http://127.0.0.1:{server.server_address[1]}/#t={server.token}"
        print(f"Open this link (it contains your access token, do not share it):\n\n    {url}\n", flush=True)
    print("Bots started from the interface run as long as this process runs. Ctrl+C stops everything.", flush=True)
    if not args.no_browser and local and not fixed_token:
        threading.Timer(0.5, lambda: webbrowser.open(url)).start()
    threading.Thread(target=sync_loop, daemon=True).start()
    wanted = list(args.start)
    if os.environ.get("LMBOT_AUTOSTART", "1" if isinstance(store, storage.SqlStore) else "0") == "1":
        wanted += [a for a in (settings.get("autostart") or []) if a not in wanted and store.has_account(a)]
    if wanted:
        threading.Thread(target=start_accounts, args=(wanted,), daemon=True).start()
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print("\nStopping bots...")
    finally:
        capture.cancel()
        keep = list(settings.data.get("autostart") or [])
        bots.stop_all()          # stopping with the console must not forget which bots were wanted
        settings.update(autostart=keep)
        flush = getattr(store, "flush_logs", None)
        if flush:
            flush()
        server.server_close()


if __name__ == "__main__":
    main()
