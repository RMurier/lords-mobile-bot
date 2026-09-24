"""Tests of the guild bank in the web console: the file format shared with the bot, the storage, the HTTP routes.

Run from the repository root (standard library only):

    python3 tests/webui_bank_test.py

The console runs in this process, on a temporary folder. A bot is faked where it matters: a thread plays the C
bot's part (src/guildbank.c) - it moves guild_bank_edits.txt aside, applies it, rewrites guild_bank.txt - so that
the console's waiting and its change of a running bot are exercised. The SQL Server store cannot be run here (no
server): its statements are checked against a recording stand-in for the connection.
"""

import json
import os
import shutil
import sys
import tempfile
import threading
import time
import unittest
import urllib.error
import urllib.parse
import urllib.request
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
ROOT = Path(tempfile.mkdtemp(prefix="lmbot-ui-test-"))
os.environ["LMBOT_UI_ROOT"] = str(ROOT)
os.environ.pop("LMBOT_DB_HOST", None)
sys.path.insert(0, str(REPO / "webui"))

import guild_bank as gb  # noqa: E402
import server  # noqa: E402
import store as storage  # noqa: E402

ACCOUNT = "1001"


def make_account(account_id, enabled=True):
    (ROOT / "accounts").mkdir(exist_ok=True)
    (ROOT / "accounts" / f"{account_id}.cfg").write_text(
        f"account.igg_id = {account_id}\naccount.access_key = key\nguildbank.enabled = {'true' if enabled else 'false'}\n"
        f"data.path = ./data/{account_id}/\n", encoding="utf-8")


class FakeProc:
    """Enough of a subprocess for the console to believe a bot is running."""
    pid = 4242
    returncode = None

    def poll(self):
        return None


class FakeBot(threading.Thread):
    """What src/guildbank.c does with the console's requests, once a moment."""

    def __init__(self, folder):
        super().__init__(daemon=True)
        self.folder = Path(folder)
        self.stop = False
        self.lag = 0.0

    def run(self):
        queue, working, bank = (self.folder / "guild_bank_edits.txt", self.folder / "guild_bank_edits.applying",
                                self.folder / "guild_bank.txt")
        while not self.stop:
            time.sleep(0.05)
            if not queue.exists():
                continue
            time.sleep(self.lag)
            os.replace(queue, working)
            state = gb.parse(bank.read_text(encoding="utf-8")) if bank.exists() else gb.empty_state(1)
            for line in working.read_text(encoding="utf-8").splitlines():
                parts = line.split("\t")
                if parts[0] == "reset":
                    gb.reset(state, time.time())
                elif parts[0] == "set":
                    gb.set_balance(state, parts[1], gb.RESOURCES[int(parts[2])], int(parts[3]))
            bank.write_text(gb.serialize(state), encoding="utf-8")
            working.unlink()


class Api:
    def __init__(self, port):
        self.base = f"http://127.0.0.1:{port}"

    def call(self, method, path, body=None):
        data = json.dumps(body).encode() if body is not None else None
        request = urllib.request.Request(self.base + path, data=data, method=method,
                                         headers={"Content-Type": "application/json"})
        try:
            with urllib.request.urlopen(request, timeout=20) as response:
                return response.status, json.load(response)
        except urllib.error.HTTPError as error:
            return error.code, json.load(error)


class FormatTests(unittest.TestCase):
    def test_round_trip_and_what_the_bot_writes(self):
        # the exact text the C bot writes (src/guildbank.c, Save)
        text = ("lmbot-guild-bank 1\nactivated 1790000000\nseen 20707\nseen 20708\n"
                "acct\tZyco\t0\t925000\t0\t0\t0\nacct\tLittle Zyco\t5\t0\t0\t0\t7\n")
        state = gb.parse(text)
        self.assertEqual(state["activated"], 1790000000)
        self.assertEqual(state["seen"], [20707, 20708])
        self.assertEqual(state["accounts"]["Zyco"], [0, 925000, 0, 0, 0])
        self.assertEqual(state["accounts"]["Little Zyco"], [5, 0, 0, 0, 7])
        self.assertEqual(gb.parse(gb.serialize(state)), state)

    def test_bad_lines_are_ignored_like_the_bot_does(self):
        state = gb.parse("junk\nacct\tbad\x01name\t1\t2\t3\t4\t5\nacct\tBob\tx\t7\nseen abc\nactivated 12\n")
        self.assertEqual(state["accounts"], {"Bob": [0, 7, 0, 0, 0]})
        self.assertEqual(state["activated"], 12)

    def test_set_balance_and_empty_accounts_disappear(self):
        state = gb.empty_state(10)
        gb.set_balance(state, "Bob", "food", 3000000)
        gb.set_balance(state, "Bob", "stone", 2000000)
        self.assertEqual(state["accounts"]["Bob"], [3000000, 2000000, 0, 0, 0])
        gb.set_balance(state, "Bob", "food", 0)
        gb.set_balance(state, "Bob", "stone", 0)
        self.assertNotIn("Bob", state["accounts"])

    def test_refused_values(self):
        state = gb.empty_state(10)
        for name, resource, amount in (("", "food", 1), ("x" * 13, "food", 1), ("a\tb", "food", 1), ("Bob", "iron", 1),
                                       ("Bob", "food", -1), ("Bob", "food", 1.5), ("Bob", "food", True),
                                       ("Bob", "food", "5"), ("Bob", "food", 10 ** 16)):
            with self.assertRaises(gb.BankError, msg=repr((name, resource, amount))):
                gb.set_balance(state, name, resource, amount)

    def test_edit_lines_are_what_the_bot_reads(self):
        self.assertEqual(gb.edit_line("set", "Little Zyco", "stone", 42), "set\tLittle Zyco\t1\t42\n")
        self.assertEqual(gb.edit_line("set", "Bob", "gold", 0), "set\tBob\t4\t0\n")
        self.assertEqual(gb.edit_line("reset"), "reset\n")

    def test_reset(self):
        state = {"activated": 5, "seen": [1, 2], "accounts": {"Bob": [1, 0, 0, 0, 0]}}
        gb.reset(state, 900)
        self.assertEqual(state["accounts"], {})
        self.assertEqual(state["activated"], 900)
        self.assertEqual(state["seen"], [1, 2])   # the reports already counted stay counted


class SqlStatements(unittest.TestCase):
    """SqlStore.put_bank / get_bank against a recording stand-in for the connection."""

    def make(self):
        store = object.__new__(storage.SqlStore)
        store.bank_seen = {}
        store.calls = []
        store.rows = {"state": None, "accounts": []}
        store._execute = lambda sql, params=(): store.calls.append((sql, params))
        store._one = lambda sql, params=(): store.rows["state"]
        store._all = lambda sql, params=(): store.rows["accounts"]
        return store

    def test_one_atomic_batch_with_matching_placeholders(self):
        for count in (0, 1, 199, 200, 201, 450):
            store = self.make()
            state = {"activated": 77, "seen": [1, 2, 3],
                     "accounts": {f"P{i}": [i + 1, 0, 0, 0, 0] for i in range(count)}}
            store.put_bank("1001", state)
            self.assertEqual(len(store.calls), 1, "the whole change must be ONE statement so a lost connection cannot leave half of it")
            sql, params = store.calls[0]
            self.assertEqual(sql.count("%s"), len(params), f"{count} players")
            self.assertTrue(sql.startswith("SET XACT_ABORT ON; BEGIN TRANSACTION"))
            self.assertTrue(sql.rstrip().endswith("COMMIT"))
            self.assertEqual(sql.count("INSERT INTO dbo.guild_bank ("), (count + 199) // 200)
            self.assertIn("1,2,3", params)

    def test_identical_state_is_not_written_again(self):
        store = self.make()
        state = {"activated": 1, "seen": [], "accounts": {"Bob": [1, 0, 0, 0, 0]}}
        store.put_bank("1001", state)
        store.put_bank("1001", json.loads(json.dumps(state)))
        self.assertEqual(len(store.calls), 1)
        state["accounts"]["Bob"][0] = 2
        store.put_bank("1001", state)
        self.assertEqual(len(store.calls), 2)

    def test_read_back(self):
        store = self.make()
        self.assertIsNone(store.get_bank("1001"))
        store.rows = {"state": (1790000000, "5,6"), "accounts": [("Zyco", 0, 925000, 0, 0, 0)]}
        self.assertEqual(store.get_bank("1001"), {"activated": 1790000000, "seen": [5, 6],
                                                  "accounts": {"Zyco": [0, 925000, 0, 0, 0]}})

    def test_the_table_compares_names_exactly(self):
        text = " ".join(storage.SCHEMA)
        self.assertIn("Latin1_General_100_BIN2", text)
        self.assertIn("dbo.guild_bank_state", text)


class ConsoleTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        make_account(ACCOUNT)
        cls.httpd = server.Server(("127.0.0.1", 0), server.Handler)
        threading.Thread(target=cls.httpd.serve_forever, daemon=True).start()
        cls.api = Api(cls.httpd.server_address[1])
        cls.folder = ROOT / "data" / ACCOUNT

    @classmethod
    def tearDownClass(cls):
        cls.httpd.shutdown()
        shutil.rmtree(ROOT, ignore_errors=True)

    def setUp(self):
        for name in ("guild_bank.txt", "guild_bank_edits.txt", "guild_bank_edits.applying", "status.json"):
            (self.folder / name).unlink(missing_ok=True)
        server.bots.procs.pop(ACCOUNT, None)
        server.BANK_EDIT_WAIT = 8.0

    def put_status(self, names):
        self.folder.mkdir(parents=True, exist_ok=True)
        (self.folder / "status.json").write_text(json.dumps({"written_at": time.time(), "connected": True,
                                                              "alliance": {"members": len(names), "member_names": names}}))

    def test_list_shows_every_member_and_who_left(self):
        self.put_status(["alice", "Bob"])
        self.folder.mkdir(parents=True, exist_ok=True)
        (self.folder / "guild_bank.txt").write_text("lmbot-guild-bank 1\nactivated 1\nacct\tZyco\t0\t925000\t0\t0\t0\n")
        status, data = self.api.call("GET", f"/api/accounts/{ACCOUNT}/bank")
        self.assertEqual(status, 200)
        self.assertTrue(data["enabled"] and data["members_known"] and not data["running"])
        self.assertEqual(data["storage"], "files")
        self.assertEqual([p["name"] for p in data["players"]], ["alice", "Bob", "Zyco"])
        by = {p["name"]: p for p in data["players"]}
        self.assertTrue(by["alice"]["in_guild"] and by["Bob"]["in_guild"])
        self.assertFalse(by["Zyco"]["in_guild"], "a balance of somebody no longer in the guild is still listed, and marked")
        self.assertEqual(by["Zyco"]["stone"], 925000)
        self.assertEqual(by["alice"]["food"], 0)
        self.assertEqual(data["totals"]["stone"], 925000)

    def test_list_without_the_member_list(self):
        status, data = self.api.call("GET", f"/api/accounts/{ACCOUNT}/bank")
        self.assertEqual(status, 200)
        self.assertFalse(data["members_known"])
        self.assertEqual(data["players"], [])

    def test_edit_while_the_bot_is_stopped(self):
        self.put_status(["alice", "Bob"])
        status, data = self.api.call("PUT", f"/api/accounts/{ACCOUNT}/bank/Bob", {"food": 3000000, "stone": 2000000})
        self.assertEqual(status, 200, data)
        self.assertTrue(data["applied"] and not data["running"])
        bob = next(p for p in data["players"] if p["name"] == "Bob")
        self.assertEqual((bob["food"], bob["stone"], bob["wood"]), (3000000, 2000000, 0))
        # the bot will find it in its file when it starts
        state = gb.parse((self.folder / "guild_bank.txt").read_text(encoding="utf-8"))
        self.assertEqual(state["accounts"]["Bob"], [3000000, 2000000, 0, 0, 0])
        # only the resources given change
        status, data = self.api.call("PUT", f"/api/accounts/{ACCOUNT}/bank/Bob", {"gold": 5})
        bob = next(p for p in data["players"] if p["name"] == "Bob")
        self.assertEqual((bob["food"], bob["gold"]), (3000000, 5))

    def test_a_name_with_a_space_and_one_who_is_not_listed(self):
        name = urllib.parse.quote("Little Zyco")
        status, data = self.api.call("PUT", f"/api/accounts/{ACCOUNT}/bank/{name}", {"ore": 12})
        self.assertEqual(status, 200, data)
        self.assertEqual(next(p for p in data["players"] if p["name"] == "Little Zyco")["ore"], 12)

    def test_refused_changes(self):
        base = f"/api/accounts/{ACCOUNT}/bank"
        for path, body in ((base + "/Bob", {"food": -5}), (base + "/Bob", {"food": "5M"}), (base + "/Bob", {"food": 1.5}),
                           (base + "/Bob", {"iron": 5}), (base + "/Bob", {}), (base + "/" + "x" * 13, {"food": 1}),
                           (base + "/Bob", {"food": 10 ** 16}), (base + "/Bob", {"food": True})):
            status, data = self.api.call("PUT", path, body)
            self.assertEqual(status, 400, (path, body, data))
        self.assertEqual(self.api.call("PUT", "/api/accounts/9999/bank/Bob", {"food": 1})[0], 404)
        self.assertEqual(self.api.call("GET", "/api/accounts/9999/bank")[0], 404)

    def test_reset_needs_a_confirmation(self):
        self.api.call("PUT", f"/api/accounts/{ACCOUNT}/bank/Bob", {"food": 10})
        base = f"/api/accounts/{ACCOUNT}/bank/reset"
        for body in ({}, {"confirm": False}, {"confirm": "true"}, {"confirm": 1}):
            self.assertEqual(self.api.call("POST", base, body)[0], 400, body)
        self.assertEqual(self.api.call("GET", f"/api/accounts/{ACCOUNT}/bank")[1]["totals"]["food"], 10)
        status, data = self.api.call("POST", base, {"confirm": True})
        self.assertEqual(status, 200, data)
        self.assertEqual(data["totals"]["food"], 0)
        state = gb.parse((self.folder / "guild_bank.txt").read_text(encoding="utf-8"))
        self.assertEqual(state["accounts"], {})
        self.assertGreater(state["activated"], 1_700_000_000, "old deliveries must not come back after a reset")

    def test_no_token_is_required(self):
        # The console has no login of its own (see webui/server.py's docstring): a bare
        # request, with no header at all, must be served rather than rejected.
        request = urllib.request.Request(self.api.base + f"/api/accounts/{ACCOUNT}/bank", method="GET")
        with urllib.request.urlopen(request, timeout=10) as response:
            self.assertEqual(response.status, 200)

    # ---- a running bot owns the file: the console queues requests and waits for it

    def start_fake_bot(self, lag=0.0):
        self.folder.mkdir(parents=True, exist_ok=True)
        bot = FakeBot(self.folder)
        bot.lag = lag
        bot.start()
        server.bots.procs[ACCOUNT] = {"proc": FakeProc(), "since": time.time(), "pump": None}
        self.addCleanup(lambda: (setattr(bot, "stop", True), server.bots.procs.pop(ACCOUNT, None)))
        return bot

    def test_edit_while_the_bot_runs_goes_through_the_bot(self):
        self.put_status(["alice"])
        self.start_fake_bot()
        (self.folder / "guild_bank.txt").write_text(gb.serialize({"activated": 1, "seen": [9], "accounts": {"alice": [1, 0, 0, 0, 0]}}))
        status, data = self.api.call("PUT", f"/api/accounts/{ACCOUNT}/bank/alice", {"food": 777, "gold": 5})
        self.assertEqual(status, 200, data)
        self.assertTrue(data["applied"] and data["running"] and not data.get("pending"))
        alice = next(p for p in data["players"] if p["name"] == "alice")
        self.assertEqual((alice["food"], alice["gold"]), (777, 5))
        self.assertFalse((self.folder / "guild_bank_edits.txt").exists())
        state = gb.parse((self.folder / "guild_bank.txt").read_text(encoding="utf-8"))
        self.assertEqual(state["seen"], [9], "the bot's own memory of counted reports survives an edit")

    def test_reset_while_the_bot_runs(self):
        self.start_fake_bot()
        (self.folder / "guild_bank.txt").write_text(gb.serialize({"activated": 1, "seen": [], "accounts": {"Bob": [5, 0, 0, 0, 0]}}))
        status, data = self.api.call("POST", f"/api/accounts/{ACCOUNT}/bank/reset", {"confirm": True})
        self.assertEqual(status, 200, data)
        self.assertTrue(data["applied"] and data["totals"]["food"] == 0)

    def test_a_bot_that_does_not_answer_leaves_the_change_pending(self):
        self.put_status(["alice"])
        server.BANK_EDIT_WAIT = 0.6
        server.bots.procs[ACCOUNT] = {"proc": FakeProc(), "since": time.time(), "pump": None}
        self.addCleanup(lambda: server.bots.procs.pop(ACCOUNT, None))
        status, data = self.api.call("PUT", f"/api/accounts/{ACCOUNT}/bank/alice", {"food": 5})
        self.assertEqual(status, 200, data)
        self.assertTrue(data.get("pending") and not data["applied"])
        self.assertTrue((self.folder / "guild_bank_edits.txt").exists(), "the request stays queued for when the bot ticks")

    def test_two_requests_queue_up_in_order(self):
        self.put_status(["alice", "Bob"])
        self.start_fake_bot(lag=0.2)
        (self.folder / "guild_bank.txt").write_text(gb.serialize(gb.empty_state(1)))
        results = []
        threads = [threading.Thread(target=lambda n=n: results.append(
            self.api.call("PUT", f"/api/accounts/{ACCOUNT}/bank/{n}", {"food": 10})[1]["applied"])) for n in ("alice", "Bob")]
        for t in threads:
            t.start()
        for t in threads:
            t.join()
        self.assertEqual(results, [True, True])
        state = gb.parse((self.folder / "guild_bank.txt").read_text(encoding="utf-8"))
        self.assertEqual({n: v[0] for n, v in state["accounts"].items()}, {"alice": 10, "Bob": 10})

    def test_a_running_bot_with_the_bank_off_refuses(self):
        make_account("1002", enabled=False)
        self.addCleanup(lambda: (ROOT / "accounts" / "1002.cfg").unlink(missing_ok=True))
        server.bots.procs["1002"] = {"proc": FakeProc(), "since": time.time(), "pump": None}
        self.addCleanup(lambda: server.bots.procs.pop("1002", None))
        status, data = self.api.call("PUT", "/api/accounts/1002/bank/Bob", {"food": 1})
        self.assertEqual(status, 409, data)
        self.assertIn("pas activée", data["error"])

    def test_the_bots_file_is_copied_into_the_database_while_it_runs(self):
        """SQL Server keeps the balances: what the bot writes must reach put_bank, and only what changed."""
        recorded = []

        class Stub:
            persists_game = persists_admins = False
            persists_bank = True

            def put_game(self, account_id, data):
                pass

            def put_admins(self, account_id, content):
                pass

            def put_bank(self, account_id, state):
                recorded.append((account_id, state))

        real = server.store
        server.store = Stub()
        try:
            self.folder.mkdir(parents=True, exist_ok=True)
            (self.folder / "guild_bank.txt").write_text("lmbot-guild-bank 1\nactivated 3\nseen 8\nacct\tBob\t9\t0\t0\t0\t0\n")
            server.sync_bot_files(ACCOUNT, ("guild_bank.txt",))
            self.assertEqual(recorded, [(ACCOUNT, {"activated": 3, "seen": [8], "accounts": {"Bob": [9, 0, 0, 0, 0]}})])
            server.sync_bot_files(ACCOUNT, ("status.json",))   # not asked for: untouched
            self.assertEqual(len(recorded), 1)
            (self.folder / "guild_bank.txt").unlink()
            server.sync_bot_files(ACCOUNT)                     # no file yet: nothing to copy, no error
            self.assertEqual(len(recorded), 1)
        finally:
            server.store = real

    def test_the_backup_carries_the_balances(self):
        self.api.call("PUT", f"/api/accounts/{ACCOUNT}/bank/Bob", {"wood": 321})
        status, bundle = self.api.call("GET", "/api/data/export")
        self.assertEqual(status, 200)
        self.assertEqual(gb.parse(bundle["accounts"][ACCOUNT]["bank"])["accounts"]["Bob"], [0, 0, 321, 0, 0])
        # restored into an empty console
        (self.folder / "guild_bank.txt").unlink()
        status, result = self.api.call("POST", "/api/data/import?overwrite=1", bundle)
        self.assertEqual(status, 200, result)
        self.assertEqual(self.api.call("GET", f"/api/accounts/{ACCOUNT}/bank")[1]["totals"]["wood"], 321)


if __name__ == "__main__":
    unittest.main(verbosity=1)
