"""Where the console keeps its data: on disk (default, local use) or in SQL Server (containers, k3s).

Both stores answer the same questions, so server.py does not care which one it talks to:

    accounts   one configuration text per account (the text of the .cfg file, comments included)
    settings   the console's own settings (bot path, start delay, account names)
    logs       the lines the bots print
    game       the last status.json a bot wrote (shield, resources...)
    admins     the administrators added in game with $admin add (admins.txt of the bot)
    bank       the guild bank's balances (guild_bank.txt of the bot), see guild_bank.py

The bot itself is unchanged: it still reads a .cfg file and writes status.json / admins.txt in its data folder.
With SQL Server the console writes that file from the database just before starting the bot, and copies what the
bot writes back into the database while it runs, so the folder can be thrown away (emptyDir) without losing anything.

Select SQL Server with LMBOT_DB_HOST (and LMBOT_DB_PASSWORD, see SqlStore.from_env).
"""

import json
import os
import threading
import time
from pathlib import Path

import config_file as cf
import guild_bank as gb

LOG_KEEP_LINES = 30000         # per account, older lines are deleted
LOG_TAIL_LINES = 300           # what a first read of the journal returns
LOG_LINE_MAX = 2000


class StoreError(Exception):
    pass


# --------------------------------------------------------------------------
# Files (what the console always did)
# --------------------------------------------------------------------------

class FileStore:
    name = "files"
    persists_game = False       # status.json is read where the bot writes it
    persists_admins = False
    persists_bank = False       # guild_bank.txt is the bank: the bot writes it, the console reads and edits it

    ROTATE_BYTES = 5 << 20

    def __init__(self, root):
        self.root = Path(root)
        self.accounts_dir = self.root / "accounts"
        self.logs_dir = self.root / "logs"
        self.settings_file = self.root / "webui_settings.json"

    def describe(self):
        return f"fichiers ({self.root})"

    # -- accounts
    def list_accounts(self, valid):
        if not self.accounts_dir.is_dir():
            return []
        return sorted(p.stem for p in self.accounts_dir.glob("*.cfg") if valid.match(p.stem))

    def has_account(self, account_id):
        return (self.accounts_dir / f"{account_id}.cfg").is_file()

    def get_cfg(self, account_id):
        return cf.read_file(self.accounts_dir / f"{account_id}.cfg")

    def put_cfg(self, account_id, text, keep_backup=True):
        cf.write_private(self.accounts_dir / f"{account_id}.cfg", text, keep_backup=keep_backup)

    def delete_account(self, account_id):
        path = self.accounts_dir / f"{account_id}.cfg"
        path.unlink(missing_ok=True)
        path.with_suffix(".cfg.bak").unlink(missing_ok=True)

    def cfg_path(self, account_id):
        """The file the bot reads."""
        return self.accounts_dir / f"{account_id}.cfg"

    # -- settings
    def get_settings(self):
        try:
            return json.loads(self.settings_file.read_text(encoding="utf-8"))
        except (OSError, ValueError):
            return {}

    def put_settings(self, data):
        self.settings_file.write_text(json.dumps(data, indent=2), encoding="utf-8")

    # -- logs
    def _log_path(self, account_id):
        return self.logs_dir / f"{account_id}.log"

    def append_log(self, account_id, text):
        self.logs_dir.mkdir(parents=True, exist_ok=True)
        path = self._log_path(account_id)
        if path.exists() and path.stat().st_size > self.ROTATE_BYTES:
            os.replace(path, self.logs_dir / f"{account_id}.log.1")
        with open(path, "ab") as f:
            f.write(text.encode("utf-8"))

    def read_log(self, account_id, offset):
        path = self._log_path(account_id)
        try:
            size = path.stat().st_size
        except OSError:
            return {"offset": 0, "text": ""}
        if offset is None or offset < 0 or offset > size:
            offset = max(0, size - 32768)  # first read or rotated file: the last 32 KiB
        with open(path, "rb") as f:
            f.seek(offset)
            chunk = f.read(131072)
        return {"offset": offset + len(chunk), "text": chunk.decode("utf-8", "replace")}

    def last_log_line(self, account_id):
        try:
            with open(self._log_path(account_id), "rb") as f:
                f.seek(0, os.SEEK_END)
                f.seek(max(0, f.tell() - 4096))
                lines = [ln.strip() for ln in f.read().decode("utf-8", "replace").splitlines() if ln.strip()]
        except OSError:
            return ""
        lines = [ln for ln in lines if not ln.startswith("---")]
        return lines[-1][:300] if lines else ""

    # -- game status and runtime administrators: the bot's own files are the source
    def get_game(self, account_id):
        try:
            return json.loads((self.root / "data" / account_id / "status.json").read_text(encoding="utf-8"))
        except (OSError, ValueError):
            return None

    def put_game(self, account_id, data):
        pass

    def get_admins(self, account_id):
        try:
            return (self.root / "data" / account_id / "admins.txt").read_text(encoding="utf-8")
        except OSError:
            return None

    def put_admins(self, account_id, content):
        folder = self.root / "data" / account_id
        folder.mkdir(parents=True, exist_ok=True)
        (folder / "admins.txt").write_text(content, encoding="utf-8")

    def get_bank(self, account_id):
        try:
            return gb.parse((self.root / "data" / account_id / "guild_bank.txt").read_text(encoding="utf-8"))
        except OSError:
            return None

    def put_bank(self, account_id, state):
        """Only used while the bot is stopped (it owns the file when it runs): written whole, then moved in place."""
        folder = self.root / "data" / account_id
        folder.mkdir(parents=True, exist_ok=True)
        temp = folder / "guild_bank.txt.tmp"
        temp.write_text(gb.serialize(state), encoding="utf-8")
        os.replace(temp, folder / "guild_bank.txt")

    def prepare_start(self, account_id):
        pass


# --------------------------------------------------------------------------
# SQL Server
# --------------------------------------------------------------------------

SCHEMA = [
    """IF OBJECT_ID(N'dbo.accounts', N'U') IS NULL
       CREATE TABLE dbo.accounts (
           id          NVARCHAR(64)  NOT NULL PRIMARY KEY,
           cfg         NVARCHAR(MAX) NOT NULL,
           created_at  DATETIME2     NOT NULL DEFAULT SYSUTCDATETIME(),
           updated_at  DATETIME2     NOT NULL DEFAULT SYSUTCDATETIME()
       )""",
    """IF OBJECT_ID(N'dbo.settings', N'U') IS NULL
       CREATE TABLE dbo.settings (
           [key]   NVARCHAR(64)  NOT NULL PRIMARY KEY,
           value   NVARCHAR(MAX) NOT NULL
       )""",
    """IF OBJECT_ID(N'dbo.bot_logs', N'U') IS NULL
       CREATE TABLE dbo.bot_logs (
           id          BIGINT IDENTITY(1,1) NOT NULL PRIMARY KEY,
           account_id  NVARCHAR(64)   NOT NULL,
           ts          DATETIME2      NOT NULL DEFAULT SYSUTCDATETIME(),
           line        NVARCHAR(2000) NOT NULL
       )""",
    """IF NOT EXISTS (SELECT 1 FROM sys.indexes WHERE name = N'ix_bot_logs_account')
       CREATE INDEX ix_bot_logs_account ON dbo.bot_logs (account_id, id)""",
    """IF OBJECT_ID(N'dbo.game_status', N'U') IS NULL
       CREATE TABLE dbo.game_status (
           account_id  NVARCHAR(64)  NOT NULL PRIMARY KEY,
           data        NVARCHAR(MAX) NOT NULL,
           updated_at  DATETIME2     NOT NULL DEFAULT SYSUTCDATETIME()
       )""",
    """IF OBJECT_ID(N'dbo.account_admins', N'U') IS NULL
       CREATE TABLE dbo.account_admins (
           account_id  NVARCHAR(64)  NOT NULL PRIMARY KEY,
           content     NVARCHAR(MAX) NOT NULL
       )""",
    # one row per player and account. Binary collation: the bot compares names exactly, and a case-insensitive
    # key would merge "Bob" and "bob" into one balance.
    """IF OBJECT_ID(N'dbo.guild_bank', N'U') IS NULL
       CREATE TABLE dbo.guild_bank (
           account_id  NVARCHAR(64)  NOT NULL,
           player      NVARCHAR(64)  COLLATE Latin1_General_100_BIN2 NOT NULL,
           food        BIGINT        NOT NULL DEFAULT 0,
           stone       BIGINT        NOT NULL DEFAULT 0,
           wood        BIGINT        NOT NULL DEFAULT 0,
           ore         BIGINT        NOT NULL DEFAULT 0,
           gold        BIGINT        NOT NULL DEFAULT 0,
           updated_at  DATETIME2     NOT NULL DEFAULT SYSUTCDATETIME(),
           PRIMARY KEY (account_id, player)
       )""",
    # what the bot needs besides the balances: when counting started, and the reports already counted
    """IF OBJECT_ID(N'dbo.guild_bank_state', N'U') IS NULL
       CREATE TABLE dbo.guild_bank_state (
           account_id  NVARCHAR(64)  NOT NULL PRIMARY KEY,
           activated   BIGINT        NOT NULL,
           seen        NVARCHAR(MAX) NOT NULL DEFAULT N''
       )""",
]


class SqlStore:
    name = "sqlserver"
    persists_game = True
    persists_admins = True
    persists_bank = True

    def __init__(self, host, port, user, password, database, runtime_dir, root, wait=120):
        try:
            import pymssql  # noqa: F401
        except ImportError:
            raise StoreError("Le module pymssql est nécessaire pour SQL Server (pip install pymssql).")
        self.params = dict(server=host, port=str(port), user=user, password=password)
        self.database = database
        self.runtime = Path(runtime_dir)
        # Where the bot process itself runs (Bots.start()'s cwd=ROOT in server.py): its .cfg's
        # relative "data.path = ./data/<id>/" resolves from there, so prepare_start() must write
        # to the same place, not to self.runtime (a different scratch subfolder).
        self.root = Path(root)
        self.host, self.port = host, port
        self.lock = threading.RLock()
        self.conn = None
        self.bank_seen = {}           # account id -> what was last written, to skip identical writes
        self.log_buffer = {}          # account id -> [lines]
        self.log_lock = threading.Lock()
        self._connect_retry(wait)
        self._create_schema()

    @classmethod
    def from_env(cls, root):
        host = os.environ.get("LMBOT_DB_HOST")
        if not host:
            return None
        password = os.environ.get("LMBOT_DB_PASSWORD")
        if not password:
            raise StoreError("LMBOT_DB_PASSWORD manquant (mot de passe de l'utilisateur SQL Server).")
        return cls(host, int(os.environ.get("LMBOT_DB_PORT", "1433")),
                   os.environ.get("LMBOT_DB_USER", "sa"), password,
                   os.environ.get("LMBOT_DB_NAME", "lordsbot"),
                   os.environ.get("LMBOT_RUNTIME_DIR") or Path(root) / "runtime",
                   root)

    def describe(self):
        return f"SQL Server ({self.host}:{self.port}/{self.database})"

    # -- connection
    def _open(self, database):
        import pymssql
        return pymssql.connect(database=database, autocommit=True, login_timeout=10, timeout=30,
                               charset="UTF-8", **self.params)

    def _connect_retry(self, wait):
        """SQL Server takes a while to start in a fresh pod: wait for it, then create the database."""
        deadline = time.time() + wait
        while True:
            try:
                master = self._open("master")
                try:
                    cur = master.cursor()
                    cur.execute("IF DB_ID(%s) IS NULL EXEC('CREATE DATABASE [' + %s + ']')",
                                (self.database, self.database))
                finally:
                    master.close()
                self.conn = self._open(self.database)
                return
            except Exception as e:  # noqa: BLE001 - the driver raises many types while the server boots
                if time.time() > deadline:
                    raise StoreError(f"SQL Server injoignable ({self.host}:{self.port}) : {e}")
                print(f"[webui] en attente de SQL Server ({e.__class__.__name__})...", flush=True)
                time.sleep(3)

    def _create_schema(self):
        for statement in SCHEMA:
            self._execute(statement)

    def _run(self, sql, params=(), fetch=None):
        """One statement, reconnecting once if the connection was lost (pod restart, idle timeout)."""
        with self.lock:
            for attempt in (1, 2):
                try:
                    if self.conn is None:
                        self.conn = self._open(self.database)
                    cur = self.conn.cursor()
                    cur.execute(sql, params)
                    if fetch == "all":
                        return cur.fetchall()
                    if fetch == "one":
                        return cur.fetchone()
                    return cur.rowcount
                except Exception:  # noqa: BLE001
                    try:
                        self.conn.close()
                    except Exception:  # noqa: BLE001
                        pass
                    self.conn = None
                    if attempt == 2:
                        raise

    def _many(self, sql, rows):
        with self.lock:
            for attempt in (1, 2):
                try:
                    if self.conn is None:
                        self.conn = self._open(self.database)
                    self.conn.cursor().executemany(sql, rows)
                    return
                except Exception:  # noqa: BLE001
                    try:
                        self.conn.close()
                    except Exception:  # noqa: BLE001
                        pass
                    self.conn = None
                    if attempt == 2:
                        raise

    def _execute(self, sql, params=()):
        return self._run(sql, params)

    def _all(self, sql, params=()):
        return self._run(sql, params, "all")

    def _one(self, sql, params=()):
        return self._run(sql, params, "one")

    # -- accounts
    def list_accounts(self, valid):
        return sorted(r[0] for r in self._all("SELECT id FROM dbo.accounts") if valid.match(r[0]))

    def has_account(self, account_id):
        return self._one("SELECT 1 FROM dbo.accounts WHERE id = %s", (account_id,)) is not None

    def get_cfg(self, account_id):
        row = self._one("SELECT cfg FROM dbo.accounts WHERE id = %s", (account_id,))
        if row is None:
            raise FileNotFoundError(account_id)
        return row[0]

    def put_cfg(self, account_id, text, keep_backup=True):
        changed = self._execute("UPDATE dbo.accounts SET cfg = %s, updated_at = SYSUTCDATETIME() WHERE id = %s",
                                (text, account_id))
        if not changed:
            self._execute("INSERT INTO dbo.accounts (id, cfg) VALUES (%s, %s)", (account_id, text))

    def delete_account(self, account_id):
        self.bank_seen.pop(account_id, None)
        for table, column in (("accounts", "id"), ("bot_logs", "account_id"), ("game_status", "account_id"),
                              ("account_admins", "account_id"), ("guild_bank", "account_id"),
                              ("guild_bank_state", "account_id")):
            self._execute(f"DELETE FROM dbo.{table} WHERE {column} = %s", (account_id,))

    def cfg_path(self, account_id):
        """The database has no file: write the one the bot reads, in the runtime folder, private."""
        path = self.runtime / f"{account_id}.cfg"
        cf.write_private(path, self.get_cfg(account_id), keep_backup=False)
        return path

    # -- settings
    def get_settings(self):
        data = {}
        for key, value in self._all("SELECT [key], value FROM dbo.settings"):
            try:
                data[key] = json.loads(value)
            except ValueError:
                pass
        return data

    def put_settings(self, data):
        for key, value in data.items():
            text = json.dumps(value)
            if not self._execute("UPDATE dbo.settings SET value = %s WHERE [key] = %s", (text, key)):
                self._execute("INSERT INTO dbo.settings ([key], value) VALUES (%s, %s)", (key, text))

    # -- logs
    def append_log(self, account_id, text):
        """Lines are grouped and written by flush_logs() so a chatty bot does not cost one query per line."""
        with self.log_lock:
            self.log_buffer.setdefault(account_id, []).append(text)

    def flush_logs(self):
        with self.log_lock:
            pending, self.log_buffer = self.log_buffer, {}
        for account_id, chunks in pending.items():
            lines = "".join(chunks).split("\n")
            rows = [(account_id, line[:LOG_LINE_MAX]) for line in lines if line.strip("\r")]
            if rows:
                self._many("INSERT INTO dbo.bot_logs (account_id, line) VALUES (%s, %s)", rows)
            self._execute(
                "DELETE FROM dbo.bot_logs WHERE account_id = %s AND id <= "
                "(SELECT MAX(id) - %s FROM dbo.bot_logs WHERE account_id = %s)",
                (account_id, LOG_KEEP_LINES, account_id))

    def read_log(self, account_id, offset):
        """The offset is the id of the last line already sent."""
        self.flush_logs()
        if offset is None or offset < 0:
            rows = self._all("SELECT TOP (%s) id, line FROM dbo.bot_logs WHERE account_id = %s ORDER BY id DESC",
                             (LOG_TAIL_LINES, account_id))
            rows.reverse()
        else:
            rows = self._all("SELECT TOP 2000 id, line FROM dbo.bot_logs WHERE account_id = %s AND id > %s "
                             "ORDER BY id", (account_id, offset))
        if not rows:
            latest = self._one("SELECT MAX(id) FROM dbo.bot_logs WHERE account_id = %s", (account_id,))
            newest = latest[0] if latest and latest[0] is not None else 0
            # nothing new; if the client is ahead (lines were purged) send it back to the start
            return {"offset": newest if (offset is None or offset < 0 or offset > newest) else offset, "text": ""}
        return {"offset": rows[-1][0], "text": "".join(line + "\n" for _, line in rows)}

    def last_log_line(self, account_id):
        self.flush_logs()
        rows = self._all("SELECT TOP 5 line FROM dbo.bot_logs WHERE account_id = %s ORDER BY id DESC", (account_id,))
        for (line,) in rows:
            if line.strip() and not line.startswith("---"):
                return line.strip()[:300]
        return ""

    # -- game status and runtime administrators
    def get_game(self, account_id):
        row = self._one("SELECT data FROM dbo.game_status WHERE account_id = %s", (account_id,))
        try:
            return json.loads(row[0]) if row else None
        except ValueError:
            return None

    def put_game(self, account_id, data):
        text = json.dumps(data)
        if not self._execute("UPDATE dbo.game_status SET data = %s, updated_at = SYSUTCDATETIME() "
                             "WHERE account_id = %s", (text, account_id)):
            self._execute("INSERT INTO dbo.game_status (account_id, data) VALUES (%s, %s)", (account_id, text))

    def get_admins(self, account_id):
        row = self._one("SELECT content FROM dbo.account_admins WHERE account_id = %s", (account_id,))
        return row[0] if row else None

    def put_admins(self, account_id, content):
        if not self._execute("UPDATE dbo.account_admins SET content = %s WHERE account_id = %s",
                             (content, account_id)):
            self._execute("INSERT INTO dbo.account_admins (account_id, content) VALUES (%s, %s)",
                          (account_id, content))

    def get_bank(self, account_id):
        state = self._one("SELECT activated, seen FROM dbo.guild_bank_state WHERE account_id = %s", (account_id,))
        rows = self._all("SELECT player, food, stone, wood, ore, gold FROM dbo.guild_bank WHERE account_id = %s",
                         (account_id,))
        if state is None and not rows:
            return None
        return {"activated": int(state[0]) if state else 0,
                "seen": [int(n) for n in (state[1] or "").split(",") if n.strip()] if state else [],
                "accounts": {r[0]: [int(v) for v in r[1:6]] for r in rows}}

    def put_bank(self, account_id, state):
        """Replaces the balances of the account in ONE batch: a lost connection rolls the whole change back and the
        retry of _run runs it again whole, never half of it."""
        key = json.dumps(state, sort_keys=True)
        if self.bank_seen.get(account_id) == key:
            return
        rows = [(name, *amounts) for name, amounts in state.get("accounts", {}).items() if any(amounts)]
        seen = ",".join(str(int(n)) for n in state.get("seen", [])[-gb.SEEN_MAX:])
        statements, params = ["SET XACT_ABORT ON", "BEGIN TRANSACTION",
                              "DELETE FROM dbo.guild_bank WHERE account_id = %s"], [account_id]
        for start in range(0, len(rows), 200):
            chunk = rows[start:start + 200]
            statements.append("INSERT INTO dbo.guild_bank (account_id, player, food, stone, wood, ore, gold) VALUES "
                              + ", ".join(["(%s, %s, %s, %s, %s, %s, %s)"] * len(chunk)))
            for row in chunk:
                params += [account_id, *row]
        statements.append("MERGE dbo.guild_bank_state AS t USING (SELECT %s AS account_id) AS s "
                          "ON t.account_id = s.account_id "
                          "WHEN MATCHED THEN UPDATE SET activated = %s, seen = %s "
                          "WHEN NOT MATCHED THEN INSERT (account_id, activated, seen) VALUES (%s, %s, %s);")
        params += [account_id, int(state.get("activated") or 0), seen, account_id, int(state.get("activated") or 0), seen]
        statements.append("COMMIT")
        self._execute("; ".join(statements), tuple(params))
        self.bank_seen[account_id] = key

    def prepare_start(self, account_id):
        """Give the bot back what it kept in its data folder (administrators added in game, the guild bank)."""
        folder = self.root / "data" / account_id
        content = self.get_admins(account_id)
        if content is not None:
            folder.mkdir(parents=True, exist_ok=True)
            (folder / "admins.txt").write_text(content, encoding="utf-8")
        bank = self.get_bank(account_id)
        if bank is not None:
            folder.mkdir(parents=True, exist_ok=True)
            (folder / "guild_bank.txt").write_text(gb.serialize(bank), encoding="utf-8")

    # -- first start: bring the files of a local installation into the database
    def import_files(self, root, valid):
        """Copies accounts/*.cfg and webui_settings.json when the database is still empty. Returns the count."""
        files = FileStore(root)
        if self.list_accounts(valid) or not files.list_accounts(valid):
            return 0
        count = 0
        for account_id in files.list_accounts(valid):
            self.put_cfg(account_id, files.get_cfg(account_id))
            admins = Path(root) / "data" / account_id / "admins.txt"
            if admins.is_file():
                self.put_admins(account_id, admins.read_text(encoding="utf-8"))
            bank = files.get_bank(account_id)
            if bank is not None:
                self.put_bank(account_id, bank)
            count += 1
        if not self.get_settings():
            saved = files.get_settings()
            saved.pop("client_path", None)   # a Windows path means nothing in a container
            self.put_settings(saved)
        return count


def open_store(root):
    """SQL Server when LMBOT_DB_HOST is set, the files otherwise."""
    sql = SqlStore.from_env(root)
    return sql if sql else FileStore(root)
