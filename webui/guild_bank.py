"""The guild bank as the console sees it: the bot's file (guild_bank.txt), and the changes the console asks for.

The bot (src/guildbank.c, include/guildbank.h) keeps a balance per player in <data.path>/guild_bank.txt:

    lmbot-guild-bank 1
    activated <unix time>          deliveries dated before this do not count
    seen <report number>           delivery reports already counted (so a report sent again is not counted twice)
    acct<TAB>name<TAB>food<TAB>stone<TAB>wood<TAB>ore<TAB>gold

Everything here works on a "state" dictionary made of that file:

    {"activated": 1790000000, "seen": [20707, ...], "accounts": {"Zyco": [0, 925000, 0, 0, 0], ...}}

Where the state lives is the store's business (store.py): the bot's file with the default file storage, the
database with SQL Server. To change a balance while the bot runs the console does not write that file (the bot owns
it): it queues requests in guild_bank_edits.txt, one per line, that the bot applies within a second. Their format is
built by edit_line(), which src/guildbank.c reads.
"""

import re

RESOURCES = ("food", "stone", "wood", "ore", "gold")     # the file's columns, in the bot's ResourceType order
LABELS = {"food": "Nourriture", "stone": "Pierre", "wood": "Bois", "ore": "Minerai", "gold": "Or"}
NAME_MAX = 12                                            # the game's player names
AMOUNT_MAX = 10 ** 15                                    # far above any real stock, and exact in JavaScript
SEEN_MAX = 1024
HEADER = "lmbot-guild-bank 1"

_CONTROL = re.compile(r"[\x00-\x1f\x7f]")


class BankError(ValueError):
    pass


def empty_state(now):
    return {"activated": int(now), "seen": [], "accounts": {}}


def valid_name(name):
    return isinstance(name, str) and 1 <= len(name) <= NAME_MAX and not _CONTROL.search(name)


def parse(text):
    """The state of a guild_bank.txt. Lines it does not know are ignored, like the bot does."""
    state = {"activated": 0, "seen": [], "accounts": {}}
    for line in str(text).splitlines():
        line = line.rstrip("\r")
        if line.startswith("activated "):
            try:
                state["activated"] = int(line.split(" ", 1)[1])
            except ValueError:
                pass
        elif line.startswith("seen "):
            try:
                state["seen"].append(int(line.split(" ", 1)[1]))
            except ValueError:
                pass
        elif line.startswith("acct\t"):
            parts = line.split("\t")
            if len(parts) < 3 or not valid_name(parts[1]):
                continue
            amounts = []
            for value in parts[2:2 + len(RESOURCES)]:
                try:
                    amounts.append(max(0, int(value)))
                except ValueError:
                    amounts.append(0)
            amounts += [0] * (len(RESOURCES) - len(amounts))
            state["accounts"][parts[1]] = amounts
    state["seen"] = state["seen"][-SEEN_MAX:]
    return state


def serialize(state):
    """The file the bot reads (the bot writes the same format)."""
    lines = [HEADER, f"activated {int(state.get('activated') or 0)}"]
    lines += [f"seen {int(n)}" for n in state.get("seen", [])[-SEEN_MAX:]]
    for name in sorted(state.get("accounts", {})):
        amounts = state["accounts"][name]
        if any(amounts):
            lines.append("acct\t" + name + "\t" + "\t".join(str(int(v)) for v in amounts))
    return "\n".join(lines) + "\n"


def check_amount(value):
    """A non-negative whole number, or BankError."""
    if isinstance(value, bool) or not isinstance(value, int):
        raise BankError("Le montant doit être un nombre entier.")
    if value < 0 or value > AMOUNT_MAX:
        raise BankError(f"Le montant doit être compris entre 0 et {AMOUNT_MAX:,}.".replace(",", " "))
    return value


def set_balance(state, name, resource, amount):
    """The balance of `name` for `resource` becomes exactly `amount` (the account is dropped when it ends up empty)."""
    if not valid_name(name):
        raise BankError(f"Pseudo invalide (1 à {NAME_MAX} caractères, sans caractère de contrôle).")
    if resource not in RESOURCES:
        raise BankError(f"Ressource inconnue : {resource}.")
    check_amount(amount)
    amounts = state["accounts"].setdefault(name, [0] * len(RESOURCES))
    amounts[RESOURCES.index(resource)] = amount
    if not any(amounts):
        del state["accounts"][name]


def reset(state, now):
    """Every balance back to zero; deliveries older than now do not come back (the bot does the same)."""
    state["accounts"] = {}
    state["activated"] = int(now)


def edit_line(action, name=None, resource=None, amount=None):
    """One line of guild_bank_edits.txt, for the bot (GuildBankApplyEdits in src/guildbank.c)."""
    if action == "reset":
        return "reset\n"
    if action != "set" or not valid_name(name) or resource not in RESOURCES:
        raise BankError("Demande de modification invalide.")
    return f"set\t{name}\t{RESOURCES.index(resource)}\t{check_amount(amount)}\n"


def totals(state):
    sums = [0] * len(RESOURCES)
    for amounts in state.get("accounts", {}).values():
        for i, value in enumerate(amounts):
            sums[i] += value
    return dict(zip(RESOURCES, sums))
