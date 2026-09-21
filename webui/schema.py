"""
Description of every setting of the bot, grouped by category.

This is the single source of truth for the web interface: categories, field
types, validation rules and default values. It mirrors what src/config.c
actually parses. Keys that the bot does not read must not be listed here.

Field types:
    bool     true / false
    int      integer with optional min / max
    text     free text with a maximum length
    secret   text that is never sent back to the browser
    ip       IPv4 address (the bot uses inet_pton with AF_INET)
    select   one value out of `options`
    size     number with an optional K / M / B suffix (20M = 20 000 000)
    shields  ordered subset of SHIELDS
    names    comma separated in-game player names
    channels one or several of CHANNELS, comma separated
"""

import re

SHIELDS = [
    ("SHIELD_4H", "Bouclier 4 heures"),
    ("SHIELD_8H", "Bouclier 8 heures"),
    ("SHIELD_12H", "Bouclier 12 heures"),
    ("SHIELD_1D", "Bouclier 1 jour"),
    ("SHIELD_3D", "Bouclier 3 jours"),
    ("SHIELD_7D", "Bouclier 7 jours"),
    ("SHIELD_14D", "Bouclier 14 jours"),
]

CHANNELS = [
    ("WORLD", "Chat du monde"),
    ("GUILD", "Chat d'alliance"),
    ("MAIL", "Courrier"),
]

RESOURCES = [("food", "Nourriture"), ("rock", "Pierre"), ("wood", "Bois"), ("ore", "Minerai"), ("gold", "Or")]


def _bool(key, label, help="", default=False, depends=None):
    return {"key": key, "label": label, "type": "bool", "default": "true" if default else "false",
            "help": help, "depends": depends}


def _resource_flags(prefix, verb, depends):
    return [_bool(f"{prefix}{name}", f"{verb} : {label.lower()}", depends=depends) for name, label in RESOURCES]


def _resource_sizes(prefix, default, depends):
    return [{"key": f"{prefix}{name}", "label": f"Réserve : {label.lower()}", "type": "size",
             "default": default.get(name, "0"), "depends": depends,
             "help": "Quantité conservée, jamais dépensée. Exemples : 500K, 20M, 1B." if i == 0 else ""}
            for i, (name, label) in enumerate(RESOURCES)]


CATEGORIES = [
    {
        "id": "account",
        "technical": True,      # not for everyday use: grouped in the "Technique" tab
        "label": "Compte",
        "description": "Identifiants du compte. Le plus simple est de les importer depuis une capture "
                      "réseau (Ajouter un compte → Importer une capture), plutôt que de les saisir à la main.",
        "fields": [
            {"key": "account.igg_id", "label": "IGG ID", "type": "int", "min": 1, "max": 2**63 - 1,
             "default": "", "help": "Identifiant IGG du compte."},
            {"key": "account.device_uuid", "label": "Device UUID", "type": "text", "maxlen": 49,
             "default": "", "optional": True,
             "help": "Identifiant de l'appareil. À laisser vide avec le client PC officiel."},
            {"key": "account.access_key", "label": "Clé d'accès", "type": "secret", "maxlen": 511,
             "default": "", "optional": True,
             "help": "Clé de session du compte. Elle donne accès au compte sans mot de passe : ne la partagez "
                     "jamais. Elle expire : réimportez une capture pour la renouveler."},
        ],
    },
    {
        "id": "network",
        "technical": True,      # not for everyday use: grouped in the "Technique" tab
        "label": "Connexion",
        "description": "Serveur et version du client à imiter. Ces valeurs viennent de la capture : ne les "
                      "changez que si vous savez pourquoi.",
        "fields": [
            {"key": "server.addr", "label": "Adresse de la passerelle", "type": "ip",
             "default": "192.243.44.63", "help": "Adresse IPv4 de la passerelle du jeu."},
            {"key": "server.port", "label": "Port de la passerelle", "type": "int", "min": 1, "max": 65535,
             "default": "5999"},
            {"key": "client.platform", "label": "Plateforme", "type": "select", "default": "1",
             "options": [("1", "Mobile (Android / iOS)"), ("9", "Client PC officiel")], "allow_other": True,
             "help": "Doit correspondre à l'appareil d'où vient la clé d'accès."},
            {"key": "client.version_major", "label": "Version du client : majeure", "type": "int",
             "min": 0, "max": 255, "default": "2"},
            {"key": "client.version_minor", "label": "Version du client : mineure", "type": "int",
             "min": 0, "max": 255, "default": "197"},
            {"key": "client.version_patch", "label": "Version du client : correctif", "type": "int",
             "min": 0, "max": 65535, "default": "308"},
            {"key": "client.language_code", "label": "Code langue", "type": "int", "min": 0, "max": 255,
             "default": "1", "help": "Valeurs observées : 1 = anglais, 3 = français, 6 = russe."},
        ],
    },
    {
        "id": "reconnect",
        "label": "Reconnexion",
        "description": "Que faire quand la connexion tombe ou que vous vous connectez sur un autre appareil. "
                      "Attention : chaque reconnexion du bot vous déconnecte de l'autre appareil.",
        "fields": [
            _bool("reconnect.enabled", "Reconnexion automatique",
                  "Le bot se reconnecte tout seul après une coupure.", default=True),
            {"key": "reconnect.delay", "label": "Délai avant reconnexion", "type": "int", "min": 10,
             "max": 86400, "default": "60", "unit": "secondes", "depends": "reconnect.enabled",
             "help": "Délai après une coupure de connexion. Minimum 10 s ; il double après des échecs répétés, "
                     "jusqu'à 8 fois sa valeur."},
            {"key": "reconnect.kicked_delay", "label": "Délai après une connexion sur un autre appareil", "type": "int",
             "min": 0, "max": 86400, "default": "60", "unit": "secondes", "depends": "reconnect.enabled",
             "zero_or_min": 10,
             "help": "Quand vous vous connectez au compte (le bot est alors déconnecté), le bot attend ce délai avant de "
                     "se reconnecter : réglez-le à la durée pendant laquelle vous jouez. 0 = ne pas se reconnecter, le "
                     "bot s'arrête. Sinon 10 secondes minimum."},
            {"key": "reconnect.max_attempts", "label": "Échecs consécutifs avant abandon", "type": "int",
             "min": 0, "max": 1000, "default": "0", "depends": "reconnect.enabled",
             "help": "0 = ne jamais abandonner. Un login refusé (clé invalide ou expirée) arrête toujours le bot."},
        ],
    },
    {
        "id": "commands",
        "label": "Commandes",
        "description": "Le bot obéit à des commandes écrites en jeu (liste complète : bouton Commandes en haut). Les administrateurs peuvent tout faire ; sans administrateur, personne n'a ces droits. Le bot ne lit que les canaux cochés.",
        "fields": [
            {"key": "admin.names", "label": "Administrateurs", "type": "names", "default": "", "optional": True,
             "warn_if": "halloweeks",
             "warn": "halloweeks est le pseudo de l'auteur du projet : il aurait les droits administrateur sur votre bot.",
             "help": "Pseudos en jeu autorisés à utiliser toutes les commandes, séparés par des virgules (16 au plus, "
                     "12 caractères chacun). Un administrateur peut aussi en ajouter en jeu avec la commande admin ; "
                     "ceux-là sont enregistrés dans le dossier de données."},
            {"key": "command.prefix", "label": "Préfixe des commandes", "type": "text", "minlen": 1,
             "maxlen": 1, "default": "$", "help": "Un seul caractère, par exemple $."},
            {"key": "command.input", "label": "Canaux de réception", "type": "channels", "default": "GUILD, MAIL",
             "options": CHANNELS, "help": "Où le bot lit les commandes. Un canal non coché est ignoré. Le chat du monde "
                                           "est public : ne le cochez que si vous le voulez."},
            {"key": "migration.scrolls_needed", "label": "Vélins de migration nécessaires", "type": "int", "min": 1,
             "max": 9999, "default": "1", "unit": "vélins",
             "help": "PROVISOIRE. Le nombre de vélins dépend de la puissance du compte et du royaume : le jeu le demande au "
                     "serveur, mais le format de sa réponse n'est pas encore décodé. En attendant, indiquez ici le nombre affiché "
                     "par l'écran de migration ; le bot le compare au sac. Voir la commande migrate cost."},
            {"key": "command.output", "label": "Canal de réponse", "type": "select", "default": "MAIL",
             "options": CHANNELS, "help": "Où le bot répond. Le courrier est privé ; en chat, la réponse est adressée "
                                           "au joueur (@pseudo) sur une seule ligne."},
        ],
    },
    {
        "id": "bank",
        "label": "Banque",
        "description": "Envoi de ressources sur commande ($food, $stone, $wood, $ore, $gold). Désactivé par défaut : une fois activé, tout joueur qui peut écrire au bot peut demander les ressources cochées, jusqu'à la réserve. Le pseudo administrateur peut toujours les demander.",
        "fields": (
            [_bool("bank.enabled", "Activer la banque", "Interrupteur général : désactivé, toutes les commandes "
                                                      "de banque sont ignorées.")]
            + _resource_flags("bank.send_", "Envoyer", "bank.enabled")
            + _resource_sizes("bank.reserve_", {"food": "20M", "rock": "50M", "wood": "50M", "ore": "30M"},
                              "bank.enabled")
            + [{"key": "bank.max_delivery_distance", "label": "Distance maximale de livraison", "type": "int",
                "min": 0, "max": 100000, "default": "100", "unit": "cases", "depends": "bank.enabled",
                "help": "Le bot refuse de livrer à un joueur plus loin que cette distance (en cases, à vol d'oiseau) "
                        "et le lui dit. 0 = pas de limite."},
               _bool("bank.use_bag_rss", "Utiliser les objets de ressources du sac",
                     "Si les ressources manquent pour une commande, le bot utilise les objets du sac (en gaspillant le "
                     "moins possible) puis livre. Choisissez ci-dessous les ressources concernées.",
                     depends="bank.enabled")]
            + _resource_flags("bank.use_bag_", "Sac", "bank.use_bag_rss")
        ),
    },
    {
        "id": "protection",
        "label": "Protection",
        "description": "Boucliers et rappel de troupes quand vous êtes attaqué ou espionné.",
        "fields": [
            _bool("protection.enabled", "Activer la protection",
                  "Interrupteur général de toutes les protections automatiques."),
            _bool("protection.shield_always_on", "Toujours garder un bouclier actif",
                  depends="protection.enabled"),
            _bool("protection.shield_on_incoming_attack", "Bouclier si attaque entrante",
                  depends="protection.enabled"),
            _bool("protection.shield_on_incoming_scout", "Bouclier si espionnage entrant",
                  depends="protection.enabled"),
            {"key": "protection.shield_priority", "label": "Ordre de priorité des boucliers", "type": "shields",
             "default": "SHIELD_4H, SHIELD_8H, SHIELD_12H, SHIELD_1D", "depends": "protection.enabled",
             "help": "Le bot utilise le premier bouclier disponible de cette liste."},
            _bool("protection.recall_on_incoming_attack", "Rappeler les troupes si attaque entrante",
                  depends="protection.enabled"),
            _bool("protection.recall_on_incoming_scout", "Rappeler les troupes si espionnage entrant",
                  depends="protection.enabled"),
            _bool("protection.recall_on_incoming_conflict", "Rappeler les troupes en cas de conflit",
                  "Rappelle un rassemblement ou un camp avant son arrivée. Nécessite des objets de "
                  "retrait de troupes.", depends="protection.enabled"),
        ],
    },
    {
        "id": "cargo",
        "label": "Cargo",
        "description": "Échanges automatiques du navire cargo.",
        "fields": (
            [_bool("cargo_ship.auto_trade", "Échanges automatiques", "Interrupteur général.")]
            + _resource_flags("cargo_ship.spend_", "Dépenser", "cargo_ship.auto_trade")
            + [_bool("cargo_ship.use_bag_rss", "Utiliser les objets de ressources du sac",
                     "Si un échange manque de ressources (réserve comprise), le bot utilise les objets du sac pour le "
                     "compléter. Sinon il ne consomme jamais d'objets du sac.", depends="cargo_ship.auto_trade")]
            + _resource_sizes("cargo_ship.reserve_", {n: "10M" for n, _ in RESOURCES}, "cargo_ship.auto_trade")
        ),
    },
    {
        "id": "alliance",
        "label": "Alliance",
        "description": "Actions automatiques au sein de l'alliance.",
        "fields": [
            _bool("alliance.auto_help", "Aider automatiquement les membres"),
            _bool("alliance.auto_open_gifts", "Ouvrir automatiquement les cadeaux d'alliance"),
        ],
    },
    {
        "id": "advanced",
        "technical": True,      # not for everyday use: grouped in the "Technique" tab
        "label": "Avancé",
        "description": "Dossier de données et diagnostic.",
        "fields": [
            {"key": "data.path", "label": "Dossier de données", "type": "text", "maxlen": 255, "default": "./data/",
             "help": "Le bot y enregistre les administrateurs ajoutés en jeu (admins.txt). Un dossier par compte."},
            _bool("log.debug", "Mode debug",
                  "Affiche chaque paquet reçu (activé par défaut : c'est ce qui permet de comprendre un refus de connexion). "
                  "Les journaux contiennent alors des données de session : ne les partagez pas.", default=True),
        ],
    },
]

# Sub-headings inside a category. The first matching key prefix wins.
_GROUPS = {
    "network": [("server.", "Serveur"), ("client.", "Client à imiter")],
    "commands": [("admin.", "Administrateur"), ("command.", "Canaux et préfixe"), ("migration.", "Migration")],
    "bank": [("bank.send_", "Ressources à envoyer"), ("bank.reserve_", "Réserves (jamais envoyées)"),
             ("bank.max_", "Livraison"), ("bank.use_bag_rss", "Objets du sac"),
             ("bank.use_bag_", "Objets du sac, par ressource")],
    "protection": [("protection.shield_", "Boucliers"), ("protection.recall_", "Rappel de troupes")],
    "cargo": [("cargo_ship.spend_", "Ressources à dépenser"), ("cargo_ship.use_bag_", "Objets du sac"),
              ("cargo_ship.reserve_", "Réserves (jamais dépensées)")],
}
for _category in CATEGORIES:
    for _field in _category["fields"]:
        for _prefix, _name in _GROUPS.get(_category["id"], []):
            if _field["key"].startswith(_prefix):
                _field["group"] = _name
                break

# Settings the bot parses but does not act on yet. The interface flags them instead of promising a behaviour that
# does not exist. Checked against the bot's source; currently every setting is applied.
INACTIVE = {}
for _category in CATEGORIES:
    for _field in _category["fields"]:
        if _field["key"] in INACTIVE:
            _field["inactive"] = INACTIVE[_field["key"]]

FIELDS = {f["key"]: f for c in CATEGORIES for f in c["fields"]}


# In-game commands, for the reference page of the console. `usage` is written without the prefix.
COMMANDS = [
    {"group": "Pour tous", "name": "help", "usage": "help", "who": "Tout le monde",
     "summary": "Le bot répond, en français, avec la liste des commandes que vous avez le droit d'utiliser, avec leur usage.",
     "example": "help"},
    {"group": "Pour tous", "name": "stop", "usage": "stop", "who": "Le joueur qui a demandé la livraison, ou un administrateur",
     "summary": "Annule la livraison de ressources en cours. Les marches déjà parties arrivent quand même.",
     "example": "stop"},
    {"group": "Banque", "name": "resources", "usage": "<food|stone|wood|ore|gold> <montant>",
     "who": "Les administrateurs ; les autres seulement si la banque est activée et la ressource autorisée",
     "summary": "Le bot envoie la ressource au joueur qui écrit la commande. Le montant accepte K, M et B.",
     "details": ["Le bot ne descend jamais sous la réserve de la ressource.",
                 "Un joueur plus loin que la distance maximale de livraison est refusé, avec la distance indiquée.",
                 "Si la ressource manque, le bot peut compléter avec les objets du sac (si activé), sinon il répond "
                 "ce qui est disponible.",
                 "Une seule livraison à la fois : un autre joueur reçoit « Transfer Busy » jusqu'à la fin ou à un stop."],
     "example": "gold 5M"},
    {"group": "Administration", "name": "bank bal", "usage": "bank bal", "who": "Administrateurs",
     "summary": "Répond avec le solde de la banque, du sac et le total de chaque ressource.", "example": "bank bal"},
    {"group": "Administration", "name": "admin list", "usage": "admin list", "who": "Administrateurs",
     "summary": "Liste les administrateurs, en indiquant ceux qui viennent du fichier de configuration.",
     "example": "admin list"},
    {"group": "Administration", "name": "admin add", "usage": "admin add <pseudo>", "who": "Administrateurs",
     "summary": "Ajoute un administrateur. Il est enregistré dans le dossier de données et survit aux redémarrages.",
     "example": "admin add Bob"},
    {"group": "Administration", "name": "admin remove", "usage": "admin remove <pseudo>", "who": "Administrateurs",
     "summary": "Retire un administrateur ajouté en jeu. Ceux du fichier de configuration se retirent dans la console.",
     "example": "admin remove Bob"},
    {"group": "Administration", "name": "relocate random", "usage": "relocate random", "who": "Administrateurs",
     "summary": "Déplace le château à un endroit choisi par le jeu, avec un relocalisateur aléatoire du sac. Aucune confirmation : la commande agit tout de suite.",
     "details": ["Le bot ne répond que s'il y a un problème (pas de relocalisateur dans le sac, sac pas encore chargé…) ou pour dire où le château a atterri.",
                 "Si le serveur refuse, le bot donne le code reçu."],
     "example": "relocate random"},
    {"group": "Administration", "name": "relocate", "usage": "relocate <x> <y>", "who": "Administrateurs",
     "summary": "Déplace le château aux coordonnées données, dans le royaume actuel, avec un relocalisateur avancé. Aucune confirmation.",
     "details": ["Erreur signalée si les coordonnées sont hors de la carte, si le château y est déjà ou s'il n'y a pas de relocalisateur avancé dans le sac."],
     "example": "relocate 100 100"},
    {"group": "Administration", "name": "migrate", "usage": "migrate <royaume> <x> <y>", "who": "Administrateurs",
     "summary": "Fait migrer le château vers un autre royaume, aux coordonnées choisies. Aucune confirmation : la commande agit tout de suite.",
     "details": ["Le bot vérifie d'abord que le royaume existe (sinon il le dit), puis envoie la migration.",
                 "Il utilise la migration gratuite (offerte aux joueurs de retour) si elle est disponible. Le message qui suit la commande compare "
                 "le sac au nombre de vélins nécessaires (réglage « Vélins de migration nécessaires ») : aucun vélin, ou pas assez (« il en manque 2 »).",
                 "Si le serveur refuse, le bot donne la raison (royaume plein, troupes hors du château, royaume protégé, limite d'alliances…). "
                 "Quand la migration gratuite n'est pas disponible, il dit s'il vous reste assez de vélins ; avec assez de vélins, la migration doit "
                 "encore se faire dans le jeu, le bot ne sait pas encore l'utiliser.",
                 "Après la migration, le jeu ferme la connexion et le bot se reconnecte tout seul, dans le nouveau royaume."],
     "example": "migrate 796 301 491"},
    {"group": "Administration", "name": "migrate cost", "usage": "migrate cost", "who": "Administrateurs",
     "summary": "Diagnostic : demande au serveur combien de vélins de migration il faut (il le calcule avec la puissance du compte) "
                "et renvoie sa réponse brute.",
     "details": ["Le nombre dépend de la puissance et du royaume, donc le bot doit le lire dans la réponse du serveur. Le format de cette réponse "
                 "n'est pas encore décodé : cette commande la montre telle quelle pour pouvoir la décoder.",
                 "Comparez avec le nombre affiché par le jeu pour le même royaume."],
     "example": "migrate cost"},
    {"group": "Administration", "name": "su", "usage": "su <pseudo>", "who": "Administrateurs",
     "summary": "Ancienne commande, équivalente à admin add.", "example": "su Bob"},
]

_SIZE = re.compile(r"^(\d+(?:\.\d+)?)([kKmMbB]?)$")
_IPV4 = re.compile(r"^(25[0-5]|2[0-4]\d|1?\d?\d)(\.(25[0-5]|2[0-4]\d|1?\d?\d)){3}$")
_MULTIPLIER = {"": 1, "k": 1_000, "m": 1_000_000, "b": 1_000_000_000}
_U32_MAX = 4_294_967_295


def parse_size(text):
    match = _SIZE.match(text.strip())
    if not match:
        raise ValueError("Format invalide. Exemples : 500K, 20M, 1B.")
    value = int(float(match.group(1)) * _MULTIPLIER[match.group(2).lower()])
    if value > _U32_MAX:
        raise ValueError("Valeur trop grande (maximum 4 294 967 295).")
    return value


def validate(field, raw):
    """Return the normalised string to write in the config file, or raise ValueError."""
    kind = field["type"]
    text = "" if raw is None else str(raw).strip()

    if "\n" in text or "\r" in text:
        raise ValueError("Une seule ligne est autorisée.")

    if text == "":
        if field.get("optional"):
            return ""
        if kind == "bool":
            raise ValueError("Valeur requise.")
        if kind in ("int", "text", "ip", "select", "size", "shields", "secret") and field.get("default", "") == "":
            raise ValueError("Valeur requise.")

    if kind == "bool":
        if text.lower() in ("true", "1"):
            return "true"
        if text.lower() in ("false", "0"):
            return "false"
        raise ValueError("Doit être vrai ou faux.")

    if kind == "int":
        if not re.fullmatch(r"\d+", text):
            raise ValueError("Doit être un nombre entier positif.")
        number = int(text)
        if number < field.get("min", 0) or number > field.get("max", _U32_MAX):
            raise ValueError(f"Doit être entre {field.get('min', 0)} et {field.get('max', _U32_MAX)}.")
        if field.get("zero_or_min") and 0 < number < field["zero_or_min"]:
            raise ValueError(f"Doit être 0 ou au moins {field['zero_or_min']}.")
        return str(number)

    if kind in ("text", "secret"):
        if len(text) < field.get("minlen", 0):
            raise ValueError(f"Au moins {field['minlen']} caractère(s).")
        if len(text) > field.get("maxlen", 255):
            raise ValueError(f"Maximum {field.get('maxlen', 255)} caractères.")
        if kind == "secret" and re.search(r"\s", text):
            raise ValueError("Ne doit contenir aucun espace.")
        if not text.isprintable():
            raise ValueError("Caractères non imprimables.")
        return text

    if kind == "ip":
        if not _IPV4.match(text):
            raise ValueError("Adresse IPv4 invalide (exemple : 192.243.44.63).")
        return text

    if kind == "select":
        allowed = [value for value, _ in field["options"]]
        if text not in allowed and not (field.get("allow_other") and re.fullmatch(r"\d{1,3}", text)):
            raise ValueError("Valeur inconnue.")
        return text

    if kind == "size":
        parse_size(text)
        return text.upper()

    if kind == "shields":
        names = [part.strip() for part in text.split(",") if part.strip()]
        known = {name for name, _ in SHIELDS}
        if not names:
            raise ValueError("Choisissez au moins un bouclier.")
        if any(name not in known for name in names) or len(set(names)) != len(names):
            raise ValueError("Liste de boucliers invalide.")
        if len(names) > 8:
            raise ValueError("Maximum 8 boucliers.")
        return ", ".join(names)

    if kind == "names":
        names = []
        for part in text.split(","):
            name = part.strip()
            if not name:
                continue
            if len(name) > 12:
                raise ValueError(f"« {name} » dépasse 12 caractères.")
            if not name.isprintable():
                raise ValueError(f"« {name} » contient des caractères non imprimables.")
            if name not in names:
                names.append(name)
        if len(names) > 16:
            raise ValueError("16 administrateurs au maximum.")
        return ", ".join(names)

    if kind == "channels":
        wanted = {part.strip() for part in text.split(",") if part.strip()}
        known = [value for value, _ in field["options"]]
        if not wanted:
            raise ValueError("Choisissez au moins un canal.")
        if wanted - set(known):
            raise ValueError("Canal inconnu.")
        return ", ".join(value for value in known if value in wanted)

    raise ValueError(f"Type inconnu : {kind}")


def public_schema():
    """Schema in the shape sent to the browser (tuples become lists)."""
    return {
        "categories": [
            {**c, "fields": [dict(f, options=[list(o) for o in f["options"]]) if "options" in f else dict(f)
                             for f in c["fields"]]}
            for c in CATEGORIES
        ],
        "shields": [list(s) for s in SHIELDS],
        "commands": COMMANDS,
    }
