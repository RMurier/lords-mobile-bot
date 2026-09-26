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

ANTISCOUT = [
    ("ANTISCOUT_4H", "Anti-espionnage 4 heures"),
    ("ANTISCOUT_8H", "Anti-espionnage 8 heures"),
    ("ANTISCOUT_1D", "Anti-espionnage 1 jour"),
    ("ANTISCOUT_3D", "Anti-espionnage 3 jours"),
    ("ANTISCOUT_7D", "Anti-espionnage 7 jours"),
]

CHANNELS = [
    ("WORLD", "Chat du monde"),
    ("GUILD", "Chat d'alliance"),
    ("MAIL", "Courrier"),
]

RESOURCES = [("food", "Nourriture"), ("rock", "Pierre"), ("wood", "Bois"), ("ore", "Minerai"), ("gold", "Or")]

# The game's 16 research categories, in the order of its tabs: (name written in the config file, label). The config
# keeps the English name so it stays readable and stable; the bot also accepts the French one or the tab number.
# Checked against gamedata/game_research.json by tests.
RESEARCH_CATEGORIES = [
    ("Economy", "Économie"), ("Defense", "Défense"), ("Military", "Militaire"), ("Monster Hunt", "Chasse au monstre"),
    ("Upgrade Defenses", "Améliorer la défense"), ("Upgrade Military", "Améliorer l'armée"),
    ("Army Leadership", "Direction armée"), ("Military Command", "Commandement militaire"),
    ("Familiars", "Familiers"), ("Familiar Battles", "Batailles de familiers"), ("Sigils", "Sceaux"),
    ("Wonder Battles", "Batailles de merveille"), ("Gear", "Équipement"),
    ("Advanced Wonder Battles", "Batailles de merveille avancée"), ("Mana Awakening", "Éveil du mana"),
    ("Guild Duel", "Duel de guilde"),
]

# The buildings the automatic construction can work on (build_id order): (name written in the config file, label).
# Checked against gamedata/game_buildings.json by tests. The special ones (residence, towers, hero statue...) are left out.
BUILD_BUILDINGS = [
    ("Lumber Mill", "Scierie"), ("Quarry", "Carrière"), ("Mines", "Mines"), ("Farm", "Ferme"), ("Manor", "Manoir"),
    ("Barracks", "Caserne"), ("Infirmary", "Infirmerie"), ("Castle", "Château"), ("Vault", "Chambre forte"),
    ("Academy", "Académie"), ("Battle Hall", "Hall de bataille"), ("Castle Wall", "Remparts"), ("Watchtower", "Tour de guet"),
    ("Embassy", "Ambassade"), ("Workshop", "Atelier"), ("Treasure Trove", "Salle au trésor"), ("Trading Post", "Comptoir"),
    ("Prison", "Prison"), ("Altar", "Autel"), ("Monsterhold", "Hall des Monstres"), ("Spring", "Source"),
    ("Mystic Spire", "Cime Mystique"), ("Gym", "Camp"), ("Lunar Foundry", "Fonderie lunaire"), ("Mana Lode", "Filon de mana"),
    ("Mana Chamber", "Chambre de mana"),
]

TROOP_TIERS = ["T1", "T2", "T3", "T4", "T5"]
TROOP_KINDS_FR = [("infantry", "Infanterie"), ("ranged", "Distance"), ("cavalry", "Cavalerie"), ("siege", "Siège")]
GATHER_KINDS = ["INFANTRY", "RANGED", "CAVALRY", "SIEGE"]


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
            _bool("protection.antiscout_always_on", "Toujours garder l'anti-espionnage actif",
                  "Le maintient actif en permanence, indépendamment du bouclier.",
                  depends="protection.enabled"),
            _bool("protection.antiscout_on_no_shield", "Anti-espionnage si pas de bouclier",
                  "Masque le compte des rapports d'espionnage quand aucun bouclier n'est actif (le bouclier reste "
                  "prioritaire sur ce mode : il protège aussi des attaques, l'anti-espionnage seul ne bloque qu'un "
                  "espionnage). Inutile en plus de l'option précédente, qui couvre déjà ce cas.",
                  depends="protection.enabled"),
            {"key": "protection.antiscout_priority", "label": "Ordre de priorité anti-espionnage", "type": "antiscout",
             "default": "ANTISCOUT_4H, ANTISCOUT_8H, ANTISCOUT_1D", "depends": "protection.enabled",
             "help": "Le bot utilise le premier objet disponible de cette liste."},
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
        "id": "activity",
        "label": "Activité",
        "description": "Réclamations automatiques, retentées toutes les 5 minutes tant qu'elles ne sont pas disponibles.",
        "fields": [
            _bool("activity.auto_double_ticket", "Récupérer automatiquement le coupon trésor (1 acheté = 1 offert)", default=True),
            _bool("activity.auto_online_gift", "Récupérer automatiquement le cadeau périodique (boîte mystère)", default=True),
        ],
    },
    {
        "id": "war",
        "technical": True,      # experimental: reverse-engineered from a single sample, needs field validation
        "label": "War (scan de royaume)",
        "description": "Scanne tout le royaume une fois, puis prévient (onglet Notifications) quand un joueur "
                      "suivi perd son bouclier. Expérimental : détecté à partir d'un seul échantillon capturé.",
        "fields": [
            _bool("war.enabled", "Activer le scan de royaume"),
        ],
    },
    {
        "id": "notify",
        "label": "Notifications Discord",
        "description": "Une seule URL de webhook, partagée par toutes les alertes ci-dessous : chacune a son "
                      "propre interrupteur, et n'affecte que le compte sur lequel elle est réglée.",
        "fields": [
            {"key": "notify.discord_webhook", "label": "Webhook Discord", "type": "text", "maxlen": 255,
             "default": "", "optional": True,
             "help": "URL du webhook Discord (Paramètres du salon → Intégrations → Webhooks)."},
            _bool("notify.on_war", "Un joueur suivi (War) perd son bouclier", default=True, depends="war.enabled"),
            _bool("notify.on_antiscout_report", "Quelqu'un a tenté de vous espionner", depends="protection.enabled"),
            _bool("notify.on_shield_expiring", "Bouclier bientôt expiré, plus aucun en stock", depends="protection.enabled"),
            _bool("notify.on_antiscout_expiring", "Anti-espionnage bientôt expiré, plus aucun en stock", depends="protection.enabled"),
            _bool("notify.on_transfer_done", "Une livraison de ressources ($bank) est terminée"),
        ],
    },
    {
        "id": "gather",
        "technical": True,      # experimental: troop count formula derived from a single capture
        "label": "Récolte automatique",
        "description": "Scanne les tuiles de ressources autour du château (une zone à la fois) et y envoie des "
                      "marches de récolte. Expérimental : la formule du nombre de troupes vient d'un seul "
                      "échantillon capturé.",
        "fields": [
            _bool("gather.enabled", "Activer la récolte automatique"),
            {"key": "gather.kind", "label": "Type de troupe envoyé (ordre de repli)", "type": "gather_kind_priority",
             "default": "INFANTRY, RANGED, CAVALRY, SIEGE", "depends": "gather.enabled",
             "help": "Une marche de récolte n'utilise qu'un seul type de troupe à la fois, mais si le premier "
                     "de la liste n'a plus de troupes libres, le suivant est essayé automatiquement. "
                     "Ex : INFANTRY, RANGED, CAVALRY, SIEGE."},
            {"key": "gather.max_marches", "label": "Marches réservées à la récolte", "type": "int",
             "min": 1, "max": 30, "default": "1", "depends": "gather.enabled",
             "help": "Sur le total de marches du compte, combien peuvent être utilisées pour la récolte en même temps."},
            {"key": "gather.radius", "label": "Rayon de recherche", "type": "int", "min": 5, "max": 200,
             "default": "30", "unit": "cases", "depends": "gather.enabled",
             "help": "Distance autour du château dans laquelle chercher des tuiles de ressources."},
            {"key": "gather.max_troop_count", "label": "Troupes max par marche", "type": "int",
             "min": 0, "max": 10000000, "default": "0", "depends": "gather.enabled",
             "help": "Plafonne le nombre de troupes envoyées en une marche de récolte à votre nombre de troupes "
                     "réellement disponibles (0 = pas de plafond - déconseillé, la formule du nombre de troupes "
                     "est expérimentale et peut largement dépasser ce que vous avez réellement pour une grosse tuile)."},
        ],
    },
    {
        "id": "autotrain",
        "label": "Formation automatique",
        "description": "Comme l'écran caserne/champ de tir/écurie/atelier du jeu : une case par palier, pour "
                      "chacun des quatre types (chacun sa propre file, en parallèle). 0 ou vide = ne pas former "
                      "cette case. Pour un même type, le palier le plus bas non atteint est toujours formé en "
                      "premier (ex : T2 rempli avant que T4 démarre).",
        "fields": [_bool("autotrain.enabled", "Activer la formation automatique")] + [
            {"key": f"autotrain.{kind}_t{tier}", "label": f"T{tier}", "type": "size",
             "default": "0", "depends": "autotrain.enabled", "group": label}
            for kind, label in TROOP_KINDS_FR for tier in range(1, 6)
        ],
    },
    {
        "id": "research",
        "label": "Recherche",
        "description": "Le bot lance tout seul, dès que la file est libre, la prochaine recherche possible des catégories "
                      "choisies, la première de la liste d'abord. Il connaît les prérequis : quand une recherche en demande une "
                      "autre, il fait d'abord celle-là (même d'une autre catégorie), en commençant par ce qui débloque le plus de "
                      "recherches, puis la plus courte. Le jeu ne fait qu'une recherche à la fois. En dessous : où en sont toutes "
                      "vos recherches, catégorie par catégorie.",
        "fields": [
            _bool("research.enabled", "Activer la recherche automatique",
                  "Le bot dépense le stock du compte pour lancer les recherches, y compris ce que les membres ont déposé à la "
                  "banque de guilde : gardez une partie avec les réserves ci-dessous."),
            {"key": "research.categories", "label": "Catégories à rechercher (par ordre de priorité)", "type": "ordered_list",
             "options": RESEARCH_CATEGORIES, "default": "", "optional": True, "depends": "research.enabled",
             "help": "Le bot cherche une recherche à lancer dans la première catégorie ; s'il n'y en a aucune de possible "
                     "(Académie trop basse, ressources), il passe à la suivante."},
        ] + _resource_sizes("research.reserve_", {}, "research.enabled"),
    },
    {
        "id": "build",
        "label": "Construction",
        "description": "Le bot prépare la construction automatique : il choisit, dans les bâtiments listés (le premier d'abord), la "
                      "prochaine amélioration possible, en suivant les prérequis (un bâtiment qui en demande un autre à un certain "
                      "niveau passe après lui), en commençant par ce qui débloque le plus de bâtiments, puis la plus courte. Il lance "
                      "une construction dès qu'une file est libre (deux files si la deuxième est active). Il n'améliore que les "
                      "bâtiments existants. En dessous : vos bâtiments, où en est chacun.",
        "fields": [
            _bool("build.enabled", "Activer la construction automatique",
                  "Le bot dépense le stock du compte pour lancer les constructions, y compris ce que les membres ont déposé à la "
                  "banque de guilde : gardez une partie avec les réserves ci-dessous."),
            {"key": "build.buildings", "label": "Bâtiments à améliorer (par ordre de priorité)", "type": "ordered_list",
             "options": BUILD_BUILDINGS, "default": "", "optional": True, "depends": "build.enabled",
             "help": "Chaque bâtiment de ce type est mené jusqu'à son niveau maximum. S'il en faut d'autres d'abord (prérequis), "
                     "ils passent avant, même s'ils ne sont pas dans la liste."},
        ] + _resource_sizes("build.reserve_", {}, "build.enabled"),
    },
    {
        "id": "guildbank",
        "label": "Banque de guilde",
        "description": "Chaque membre dépose des ressources en les envoyant au bot ; le bot note ce qu'il a réellement reçu "
                      "(après la taxe de l'expéditeur) dans le solde du joueur. Les commandes de ressources (food, stone...) "
                      "servent alors à reprendre son propre solde, et bal l'affiche. Pour donner depuis le stock du bot, les "
                      "administrateurs utilisent adminfood <pseudo> <montant> (et les autres ressources). Les soldes sont dans "
                      "guild_bank.txt (dossier de données). Désactivée, le réglage « Banque » ci-dessus décide qui peut quoi.",
        "fields": [
            _bool("guildbank.enabled", "Activer la banque de guilde",
                  "Réservée aux membres de la guilde du bot. Un seul retrait à la fois, les autres membres attendent dans une "
                  "file (leur place leur est annoncée). Au retrait, le joueur paie la taxe de livraison du bot ; le solde baisse "
                  "du montant brut envoyé. La réserve du bot n'est jamais entamée par un envoi administrateur, ni les dépôts."),
        ],
    },
    {
        "id": "recall",
        "label": "Rappel des troupes",
        "description": "La commande $recall (administrateurs) rappelle toutes les troupes. Ensuite le bot n'envoie plus "
                      "aucune marche (récolte, livraisons, ralliements) pendant la durée ci-dessous.",
        "fields": [
            {"key": "recall.pause_seconds", "label": "Pause après un rappel", "type": "int",
             "min": 0, "max": 86400, "default": "300", "unit": "secondes",
             "help": "Temps pendant lequel le bot n'envoie aucune marche après $recall. 300 = 5 minutes. 0 = pas de pause."},
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
            _bool("log.packets", "Capture complète des paquets",
                  "Écrit TOUS les paquets reçus et envoyés, en entier, dans packets.log (dossier de données). "
                  "Sert à retrouver une valeur que le bot ne décode pas encore. Le fichier grossit vite et contient "
                  "des données de session : ne le partagez pas tel quel.", default=False),
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
    {"group": "Banque", "name": "resources", "usage": "<food|stone|wood|ore|gold> <montant>|all",
     "who": "Les administrateurs ; les autres seulement si la banque est activée et la ressource autorisée",
     "summary": "Le bot envoie la ressource au joueur qui écrit la commande. Le montant accepte K, M et B.",
     "details": ["Le bot ne descend jamais sous la réserve de la ressource.",
                 "Un joueur plus loin que la distance maximale de livraison est refusé, avec la distance indiquée.",
                 "Si la ressource manque, le bot peut compléter avec les objets du sac (si activé), sinon il répond "
                 "ce qui est disponible.",
                 "Une seule livraison à la fois : un autre joueur reçoit « Transfer Busy » jusqu'à la fin ou à un stop.",
                 "Avec la banque de guilde activée : la commande reprend votre propre solde (all = tout), pour tous les membres "
                 "administrateurs compris, en file d'attente, et la taxe de livraison est payée par le joueur."],
     "example": "gold 5M"},
    {"group": "Banque de guilde", "name": "bal", "usage": "bal [pseudo]",
     "who": "Tous les membres (le pseudo : les administrateurs)",
     "summary": "Affiche votre solde de la banque de guilde : ce que vous avez déposé en envoyant des ressources au bot.",
     "details": ["Pour chaque ressource, le solde et ce que vous recevriez après la taxe de livraison du bot.",
                 "Un administrateur peut ajouter un pseudo pour voir le solde d'un autre joueur.",
                 "Sans banque de guilde activée, la commande est ignorée."],
     "example": "bal"},
    {"group": "Banque de guilde", "name": "rss", "usage": "rss <food> <stone> <wood> <ore> <gold>",
     "who": "Tous les membres",
     "summary": "Comme la commande simple par ressource, mais retire plusieurs ressources de votre solde en une seule commande.",
     "details": ["Les 5 montants sont dans cet ordre fixe ; 0 = ne rien retirer de cette ressource, all = tout le solde de "
                 "cette ressource, ex. « rss 0 0 0 0 5M » ou « rss 0 0 0 0 all » pour de l'or seul.",
                 "Envoyées dans un ordre de priorité fixe, pas l'ordre tapé : or, minerai, bois, pierre, puis nourriture en dernier.",
                 "Chaque ressource part comme une livraison à part (une ou plusieurs marches), l'une après l'autre.",
                 "Sans banque de guilde activée, la commande est ignorée."],
     "example": "rss 0 0 0 0 all"},
    {"group": "Banque de guilde", "name": "adminresources", "usage": "admin<food|stone|wood|ore|gold> <pseudo> <montant>",
     "who": "Administrateurs",
     "summary": "Envoie des ressources depuis le stock du bot au joueur indiqué (adminfood, adminstone, adminwood, adminore, admingold).",
     "details": ["Le bot ne touche jamais à sa réserve ni aux dépôts des membres : seul le stock au-dessus des deux est donnable.",
                 "Le pseudo peut contenir des espaces ; le montant est le dernier mot.",
                 "Avec la banque de guilde, le joueur doit être dans la guilde. Les messages d'erreur vont à l'administrateur.",
                 "Passe par la même file d'attente que les retraits. Les objets du sac ne sont pas utilisés."],
     "example": "adminfood Bob 5M"},
    {"group": "Banque de guilde", "name": "adminrss", "usage": "adminrss <food> <stone> <wood> <ore> <gold> <pseudo>",
     "who": "Administrateurs",
     "summary": "Comme adminfood/adminstone/..., mais envoie plusieurs ressources depuis le stock du bot en une seule commande.",
     "details": ["Les 5 montants sont dans cet ordre fixe, suivis du pseudo ; 0 = ne rien envoyer de cette ressource, "
                 "all = tout ce que le stock permet de donner pour cette ressource.",
                 "Envoyées dans un ordre de priorité fixe, pas l'ordre tapé : or, minerai, bois, pierre, puis nourriture en dernier.",
                 "Le pseudo peut contenir des espaces.",
                 "Le bot ne touche jamais à sa réserve ni aux dépôts des membres."],
     "example": "adminrss 0 0 0 0 all Bob"},
    {"group": "Banque de guilde", "name": "adminall", "usage": "adminall <pseudo>",
     "who": "Administrateurs",
     "summary": "Envoie tout ce qui est actuellement disponible à ce joueur, les cinq ressources d'un coup, même ordre de priorité que adminrss.",
     "details": ["Contrairement à toutes les autres commandes de ressources, celle-ci envoie aussi la réserve configurée "
                 "(bank.reserve.*) : utile pour vider complètement la banque dans un autre bot avant une migration.",
                 "Les dépôts des membres de la guilde ne sont jamais touchés, migration ou non.",
                 "Rien n'est envoyé si le stock (dépôts exceptés) est vide."],
     "example": "adminall Bob"},
    {"group": "Administration", "name": "bank bal", "usage": "bank bal [chat|mail]", "who": "Administrateurs",
     "summary": "Répond avec le solde de la banque, du sac et le total de chaque ressource, dans le canal choisi par "
                "« Sortie des commandes » (command.output).",
     "details": ["Ajoutez chat ou mail après bal pour forcer la réponse dans ce canal-là, juste pour cette fois, "
                 "sans changer le réglage général."],
     "example": "bank bal chat"},
    {"group": "Administration", "name": "admin list", "usage": "admin list", "who": "Administrateurs",
     "summary": "Liste les administrateurs, en indiquant ceux qui viennent du fichier de configuration.",
     "example": "admin list"},
    {"group": "Administration", "name": "admin add", "usage": "admin add <pseudo>", "who": "Administrateurs",
     "summary": "Ajoute un administrateur. Il est enregistré dans le dossier de données et survit aux redémarrages.",
     "example": "admin add Bob"},
    {"group": "Administration", "name": "admin remove", "usage": "admin remove <pseudo>", "who": "Administrateurs",
     "summary": "Retire un administrateur ajouté en jeu. Ceux du fichier de configuration se retirent dans la console.",
     "example": "admin remove Bob"},
    {"group": "Administration", "name": "recall", "usage": "recall", "who": "Administrateurs",
     "summary": "Rappelle toutes les troupes, puis le bot n'envoie plus aucune marche pendant un moment (5 minutes par défaut).",
     "details": ["Récolte automatique, livraisons de ressources et ralliements sont suspendus pendant la pause ; ils "
                 "reprennent tout seuls ensuite. La durée se règle dans « Rappel des troupes » (recall.pause_seconds, 0 = pas de pause).",
                 "Une livraison en cours est annulée et son demandeur prévenu ; une nouvelle commande de ressources pendant la "
                 "pause est refusée, avec le temps restant.",
                 "Le rappel envoie une demande par marche possible du compte, une toutes les 1 à 2 secondes. Il utilise le retour "
                 "gratuit, jamais un objet « Withdraw Squad ». Refaire la commande relance les 5 minutes.",
                 "La pause survit à une reconnexion du bot."],
     "example": "recall"},
    {"group": "Administration", "name": "heal", "usage": "heal", "who": "Administrateurs",
     "summary": "Soigne tous les blessés à l'infirmerie d'un coup, comme le bouton « tout soigner » en jeu.",
     "details": ["Le bot doit avoir déjà reçu les données de l'infirmerie du serveur (peu après la connexion) ; sinon il le dit.",
                 "Rien n'est envoyé s'il n'y a aucun blessé."],
     "example": "heal"},
    {"group": "Administration", "name": "askhelp", "usage": "askhelp <fois>", "who": "Administrateurs",
     "summary": "Améliore la ferme gardée à bas niveau, demande de l'aide à l'alliance, attend 3 à 4 secondes, annule la construction, et recommence : autant de cycles que demandé (100 au plus).",
     "details": ["C'est toujours la ferme du compte qui a le niveau le plus bas (à égalité, l'emplacement le plus petit) : la construction automatique ne la monte jamais, "
                 "pour qu'elle reste disponible. Pas de ferme, ferme au maximum, ressources ou prérequis manquants : la commande le dit.",
                 "Si le stock ne suffit pas pour le niveau suivant de la ferme, le bot complète avec les objets de ressources du sac (seulement ce qui manque) ; s'il ne peut pas, il dit quelle ressource manque et ce que contiennent le stock et le sac. "
                 "Si la ferme est déjà en construction au départ (série interrompue par une déconnexion, par exemple), il l'annule d'abord, dans la file où elle se trouve, puis reprend : ce n'est pas un cycle. "
                 "Il n'annule que cette ferme et ce qu'il vient de lancer, dans la file où c'est entré ; l'autre file n'est jamais touchée. Si une réponse manque, si le serveur renvoie une erreur ou si les deux files sont occupées, la série s'arrête et le dit.",
                 "À la fin (ou à l'arrêt) le bot indique combien de cycles sont faits et ce que le stock a gagné ou perdu depuis le début : c'est la façon de voir si l'annulation rend tout. Chaque cycle envoie une demande d'aide à toute l'alliance.",
                 "askhelp stop arrête la série. La construction automatique est suspendue pendant la série."],
     "example": "askhelp 20"},
    {"group": "Administration", "name": "research", "usage": "research [catégorie]", "who": "Administrateurs",
     "summary": "Sans argument : la recherche en cours et, pour chaque catégorie, combien de recherches sont terminées. Avec une catégorie (nom ou numéro d'onglet) : ce qu'il y reste à faire.",
     "details": ["Le nom peut être tapé sans accent, en français ou en anglais ; s'il correspond à plusieurs catégories, le bot les liste.",
                 "Réservé aux administrateurs. Le même détail, plus complet, est dans l'onglet « Recherche » de la console."],
     "example": "research sceaux"},
    {"group": "Administration", "name": "research start", "usage": "research start <catégorie>|#<numéro>", "who": "Administrateurs",
     "summary": "Lance tout de suite la prochaine recherche possible de cette catégorie (ou cette recherche précise).",
     "details": ["Vérifie l'Académie, les prérequis et le coût de base contre le stock ; prend la recherche la plus courte. Dit pourquoi si rien ne peut être lancé.",
                 "Ne fait rien pendant qu'une recherche tourne : le jeu n'en accepte qu'une à la fois. La recherche automatique (onglet « Recherche ») fait la même chose toute seule."],
     "example": "research start sceaux"},
    {"group": "Administration", "name": "revive", "usage": "revive", "who": "Administrateurs",
     "summary": "Lance une résurrection gratuite (sanctuaire) pour tous les morts d'un coup - la version qui ne coûte rien mais demande d'attendre, pas la version instantanée avec des points.",
     "details": ["Le bot doit avoir déjà reçu les données du sanctuaire du serveur ; sinon il le dit.",
                 "Rien n'est envoyé s'il n'y a aucun mort.",
                 "La résurrection instantanée (avec des points gagnés en attendant ou via des quêtes) n'est pas encore disponible."],
     "example": "revive"},
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
    {"group": "Administration", "name": "join", "usage": "join <tag>", "who": "Administrateurs",
     "summary": "Cherche une guilde par son tag (3 caractères, sensible à la casse) et envoie une candidature.",
     "details": ["La recherche du jeu n'est pas sensible à la casse et peut trouver plusieurs guildes qui ne "
                 "diffèrent que par la casse (ex. JFK et JfK) : seule une correspondance exacte sur la casse est "
                 "utilisée, sinon le bot répond « Guilde introuvable ».",
                 "Si le château est déjà dans une guilde, il la quitte d'abord, attend 3 à 4 secondes, puis "
                 "recherche et postule à la nouvelle. Si le départ échoue, join s'arrête là et ne cherche pas.",
                 "Le bot envoie une candidature dans tous les cas : selon les réglages de la guilde visée, elle est "
                 "acceptée tout de suite ou mise en attente d'un officier. Le bot ne sait pas distinguer les deux : "
                 "il répond toujours « Candidature envoyée ». Vérifiez dans le jeu si besoin.",
                 "Une seule opération de guilde à la fois (join ou leave) : une deuxième commande pendant que la "
                 "première tourne encore est refusée."],
     "example": "join JfK"},
    {"group": "Administration", "name": "leave", "usage": "leave", "who": "Administrateurs",
     "summary": "Quitte la guilde actuelle tout de suite, sans confirmation.",
     "example": "leave"},
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
        if kind in ("int", "text", "ip", "select", "size", "shields", "antiscout", "secret") and field.get("default", "") == "":
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

    if kind == "antiscout":
        names = [part.strip() for part in text.split(",") if part.strip()]
        known = {name for name, _ in ANTISCOUT}
        if not names:
            raise ValueError("Choisissez au moins un objet anti-espionnage.")
        if any(name not in known for name in names) or len(set(names)) != len(names):
            raise ValueError("Liste anti-espionnage invalide.")
        if len(names) > 8:
            raise ValueError("Maximum 8 objets.")
        return ", ".join(names)

    if kind == "ordered_list":
        names = [part.strip() for part in text.split(",") if part.strip()]
        known = {name for name, _ in field["options"]}
        if any(name not in known for name in names) or len(set(names)) != len(names):
            raise ValueError("Liste invalide.")
        return ", ".join(names)

    if kind == "gather_kind_priority":
        kinds = [part.strip().upper() for part in text.split(",") if part.strip()]
        if not kinds:
            raise ValueError("Choisissez au moins un type de troupe.")
        if any(k not in GATHER_KINDS for k in kinds):
            raise ValueError("Type de troupe inconnu (INFANTRY, RANGED, CAVALRY ou SIEGE).")
        if len(set(kinds)) != len(kinds):
            raise ValueError("Un même type ne peut apparaître qu'une fois.")
        return ", ".join(kinds)

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
        "antiscout": [list(s) for s in ANTISCOUT],
        "commands": COMMANDS,
    }
