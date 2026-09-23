#include "command.h"
#include <ctype.h>
#include <stdarg.h>
#include <time.h>
#ifdef _WIN32
  #include <direct.h>
#else
  #include <sys/stat.h>
#endif
#include "items.h"
#include "protocol.h"
#include "map_point.h"
#include "log.h"

/*
 * NOTE:
 *
 * This module is currently a work in progress.
 * The implementation is functional but not considered final.
 * Additional optimizations, structural improvements, and new features
 * are expected as the project continues to evolve.
 */
 
/*
void format_number2(uint64_t num, char *out, size_t size) {
    if (num >= 1000000000ULL) {
    	snprintf(out, size, "%.*fB", 2, num / 1000000000.0);
        // snprintf(out, size, "%lluB", num / 1000000000ULL);
    } else if (num >= 1000000ULL) {
    	snprintf(out, size, "%.*fM", 2, num / 1000000.0);
        // snprintf(out, size, "%lluM", num / 1000000ULL);
    } else if (num >= 1000ULL) {
    	snprintf(out, size, "%.*fK", 2, num / 1000.0);
        // snprintf(out, size, "%lluK", num / 1000ULL);
    } else {
        snprintf(out, size, "%lu", num);
    }
}*/

void ShowBankBalance(Connection *c, const char *player_name);
static void ResourceCommandHandler(
    Connection *c,
    const char *player_name,
    const char *message,
    ResourceType type,
    const char *name
);

/* ------------------------------------------------------------------------
 * Replies: mail, alliance chat or world chat, depending on command.output
 * ------------------------------------------------------------------------ */

void BotReply(Connection *c, const char *player_name, const char *subject, const char *fmt, ...)
{
	char text[1024];
	va_list args;

	va_start(args, fmt);
	vsnprintf(text, sizeof(text), fmt, args);
	va_end(args);

	if (c->bot.command_output == COMMAND_CHANNEL_MAIL) {
		RequestSendMail(c, player_name, subject, text);
		return;
	}

	/* chat messages are a single short line */
	for (char *p = text; *p; p++) {
		if (*p == '\n' || *p == '\r')
			*p = ' ';
	}

	char line[300];
	snprintf(line, sizeof(line), "@%s %.240s", player_name, text); // chat lines are kept short on purpose

	RequestSendChat(c, c->bot.command_output == COMMAND_CHANNEL_GUILD ? 1 : 0, line);
}

/* ------------------------------------------------------------------------
 * Administrators: the config file (admin.names), plus the ones added in game,
 * which are kept in <data.path>/admins.txt
 * ------------------------------------------------------------------------ */

bool IsAdmin(const Connection *c, const char *name)
{
	for (int i = 0; i < c->bot.admin_count; i++) {
		if (strcmp(c->bot.admin_names[i], name) == 0)
			return true;
	}

	return false;
}

/* Adds an administrator (or does nothing if already there). False if the name is invalid or the list is full. */
bool AdminAdd(Connection *c, const char *name)
{
	size_t length = strlen(name);

	if (length == 0 || length > 12)
		return false;

	for (size_t i = 0; i < length; i++) {
		if ((unsigned char)name[i] < 0x20 || name[i] == 0x7f)
			return false;
	}

	if (IsAdmin(c, name))
		return true;

	if (c->bot.admin_count >= MAX_ADMINS)
		return false;

	memcpy(c->bot.admin_names[c->bot.admin_count], name, length + 1);
	c->bot.admin_count++;

	return true;
}

/* Removes an administrator added in game. Administrators from the config file cannot be removed. */
bool AdminRemove(Connection *c, const char *name)
{
	for (int i = c->bot.admin_config_count; i < c->bot.admin_count; i++) {
		if (strcmp(c->bot.admin_names[i], name) == 0) {
			for (int j = i; j < c->bot.admin_count - 1; j++)
				memcpy(c->bot.admin_names[j], c->bot.admin_names[j + 1], sizeof(c->bot.admin_names[j]));

			c->bot.admin_count--;
			return true;
		}
	}

	return false;
}

static bool AdminFilePath(const Connection *c, char *out, size_t size)
{
	size_t length = strlen(c->bot.data_path);

	if (length == 0)
		return false;

	char last = c->bot.data_path[length - 1];
	int written = snprintf(out, size, "%s%sadmins.txt", c->bot.data_path, (last == '/' || last == '\\') ? "" : "/");

	return written > 0 && (size_t)written < size;
}

void MakeDirectories(const char *path)
{
	char buffer[300];

	snprintf(buffer, sizeof(buffer), "%s", path);

	for (char *p = buffer + 1; *p; p++) {
		if (*p == '/' || *p == '\\') {
			char saved = *p;
			*p = '\0';
#ifdef _WIN32
			_mkdir(buffer);
#else
			mkdir(buffer, 0700);
#endif
			*p = saved;
		}
	}
}

/* Stores the administrators added in game so they survive a reconnection or restart. */
bool AdminSaveRuntime(const Connection *c)
{
	char path[400];

	if (!AdminFilePath(c, path, sizeof(path)))
		return false;

	MakeDirectories(path);

	FILE *fp = fopen(path, "w");

	if (!fp)
		return false;

	for (int i = c->bot.admin_config_count; i < c->bot.admin_count; i++)
		fprintf(fp, "%s\n", c->bot.admin_names[i]);

	fclose(fp);
	return true;
}

/* Loads the administrators added in game. Called after the configuration file was read. */
void AdminLoadRuntime(Connection *c)
{
	char path[400];
	char line[64];

	if (!AdminFilePath(c, path, sizeof(path)))
		return;

	FILE *fp = fopen(path, "r");

	if (!fp)
		return;

	while (fgets(line, sizeof(line), fp)) {
		line[strcspn(line, "\r\n")] = '\0';
		AdminAdd(c, line);
	}

	fclose(fp);
}

/* ------------------------------------------------------------------------
 * Guild chat outbox: the web console writes a message to <data.path>/chat_outbox.txt,
 * this polls for it and forwards it to the alliance channel. One pending message at a
 * time (the console only ever writes one), consumed and deleted once sent.
 * ------------------------------------------------------------------------ */

static bool ChatOutboxPath(const Connection *c, char *out, size_t size)
{
	size_t length = strlen(c->bot.data_path);

	if (length == 0)
		return false;

	char last = c->bot.data_path[length - 1];
	int written = snprintf(out, size, "%s%schat_outbox.txt", c->bot.data_path, (last == '/' || last == '\\') ? "" : "/");

	return written > 0 && (size_t)written < size;
}

void ChatOutboxTick(Connection *c)
{
	char path[400];
	char message[241] = {0};

	if (!ChatOutboxPath(c, path, sizeof(path)))
		return;

	FILE *fp = fopen(path, "r");
	if (!fp)
		return; // nothing pending: the common case, checked every tick

	size_t read = fread(message, 1, sizeof(message) - 1, fp);
	message[read] = '\0';
	fclose(fp);
	remove(path); // consumed whether or not it turns out empty

	message[strcspn(message, "\r\n")] = '\0'; // a single line

	if (message[0] == '\0')
		return;

	RequestSendChat(c, 1 /* alliance */, message);
}

/* True when `text` starts with `word` followed by the end of the string or a space. args = the rest. */
static bool StartsWithWord(const char *text, const char *word, const char **args)
{
	size_t length = strlen(word);

	if (strncmp(text, word, length) != 0)
		return false;

	if (text[length] != '\0' && text[length] != ' ')
		return false;

	const char *rest = text + length;

	while (*rest == ' ')
		rest++;

	*args = rest;
	return true;
}

static void AdminCommand(Connection *c, const char *player_name, bool is_admin, const char *args)
{
	char list[512] = {0};
	const char *rest;

	if (StartsWithWord(args, "list", &rest)) {
		if (!is_admin) {
			BotReply(c, player_name, "Non autorisé", "Vous n'avez pas la permission de voir les administrateurs.");
			return;
		}

		size_t used = 0;

		for (int i = 0; i < c->bot.admin_count; i++) {
			used += (size_t)snprintf(list + used, sizeof(list) - used, "%s%s%s", i ? ", " : "", c->bot.admin_names[i],
				i < c->bot.admin_config_count ? " (config)" : "");

			if (used >= sizeof(list))
				break;
		}

		BotReply(c, player_name, "Administrateurs", "%s", c->bot.admin_count ? list : "Aucun administrateur configuré.");
		return;
	}

	bool add    = StartsWithWord(args, "add", &rest);
	bool remove = !add && StartsWithWord(args, "remove", &rest);

	if (!add && !remove) {
		BotReply(c, player_name, "Administrateurs", "Usage : %cadmin list | %cadmin add <joueur> | %cadmin remove <joueur>",
			c->bot.command_prefix, c->bot.command_prefix, c->bot.command_prefix);
		return;
	}

	if (!is_admin) {
		BotReply(c, player_name, "Non autorisé", "Vous n'avez pas la permission de gérer les administrateurs.");
		return;
	}

	const char *target = rest;

	if (add) {
		if (!AdminAdd(c, target)) {
			BotReply(c, player_name, "Administrateurs", "Impossible d'ajouter « %s » (nom invalide, ou %d administrateurs au maximum).", target, MAX_ADMINS);
			return;
		}

		BotReply(c, player_name, "Administrateurs", "%s est maintenant administrateur.%s", target,
			AdminSaveRuntime(c) ? "" : " (non enregistré : data.path n'est pas accessible en écriture)");
		return;
	}

	if (!AdminRemove(c, target)) {
		BotReply(c, player_name, "Administrateurs", IsAdmin(c, target)
			? "%s est défini dans le fichier de configuration : retirez-le là."
			: "%s n'est pas administrateur.", target);
		return;
	}

	BotReply(c, player_name, "Administrateurs", "%s n'est plus administrateur.%s", target,
		AdminSaveRuntime(c) ? "" : " (non enregistré : data.path n'est pas accessible en écriture)");
}

/* ------------------------------------------------------------------------
 * Bank permissions
 * ------------------------------------------------------------------------ */

/*
 * Banking is opt-in. Administrators can always use it; everybody else needs
 * bank.enabled and the matching bank.send_* flag. Without this check any player
 * able to write in a channel the bot reads could drain the account.
 */
static bool BankAllows(Connection *c, const char *player_name, ResourceType type)
{
	if (IsAdmin(c, player_name))
		return true;

	if (!c->bank.enabled)
		return false;

	switch (type) {
		case RESOURCE_FOOD: return c->bank.send_food;
		case RESOURCE_ROCK: return c->bank.send_rock;
		case RESOURCE_WOOD: return c->bank.send_wood;
		case RESOURCE_ORE:  return c->bank.send_ore;
		case RESOURCE_GOLD: return c->bank.send_gold;
	}

	return false;
}

/* bank.use_bag_rss is the master switch, bank.use_bag_<resource> chooses which resources may use the bag. */
static bool BankMayUseBag(const Connection *c, ResourceType type)
{
	if (!c->bank.use_bag_rss)
		return false;

	switch (type) {
		case RESOURCE_FOOD: return c->bank.use_bag_food;
		case RESOURCE_ROCK: return c->bank.use_bag_rock;
		case RESOURCE_WOOD: return c->bank.use_bag_wood;
		case RESOURCE_ORE:  return c->bank.use_bag_ore;
		case RESOURCE_GOLD: return c->bank.use_bag_gold;
	}

	return false;
}

/* ------------------------------------------------------------------------
 * Relocation and migration. Administrators only. They act at once and only
 * write back when something is wrong (no item, invalid coordinates, refusal),
 * or when the server has answered.
 * ------------------------------------------------------------------------ */

/* The answer of the server to a relocator is reported to `player_name` for the next 30 seconds. */
static void WatchRelocation(Connection *c, const char *player_name)
{
	snprintf(c->relocation.report_to, sizeof(c->relocation.report_to), "%s", player_name);
	c->relocation.report_until = time(NULL) + 30;
}

static void RelocateCommand(Connection *c, const char *player_name, bool is_admin, const char *args)
{
	char p = c->bot.command_prefix;
	const char *rest;
	unsigned x, y;
	
	if (!is_admin) {
		BotReply(c, player_name, "Non autorisé", "Seuls les administrateurs peuvent déplacer le château.");
		return;
	}
	
	if (!c->items_loaded) {
		BotReply(c, player_name, "Relocalisation", "Le sac n'est pas encore chargé, réessayez dans un instant.");
		return;
	}
	
	if (StartsWithWord(args, "random", &rest)) {
		if (c->items[RANDOM_RELOCATOR].quantity == 0) {
			BotReply(c, player_name, "Relocalisation", "Il n'y a aucun relocalisateur aléatoire dans le sac.");
			return;
		}
		
		WatchRelocation(c, player_name);
		RequestSimpleUseItem(c, RANDOM_RELOCATOR, 1);
		BotReply(c, player_name, "Relocalisation", "Relocalisation aléatoire demandée, le résultat arrive dans un instant.");
		return;
	}
	
	if (sscanf(args, "%u %u", &x, &y) != 2) {
		BotReply(c, player_name, "Relocalisation", "Usage : %crelocate random | %crelocate <x> <y>", p, p);
		return;
	}
	
	if (!CheckTileMapPos((int)x, (int)y)) {
		BotReply(c, player_name, "Relocalisation", "Coordonnées invalides : X:%u Y:%u n'est pas une case du royaume.", x, y);
		return;
	}
	
	map_pos_t here = getTileMapPosbyPointCode(c->player.zone_id, c->player.point_id);
	
	if (here.x == x && here.y == y) {
		BotReply(c, player_name, "Relocalisation", "Le château est déjà en X:%u Y:%u.", x, y);
		return;
	}
	
	if (c->items[ADVANCE_RELOCATOR].quantity == 0) {
		BotReply(c, player_name, "Relocalisation", "Il n'y a aucun relocalisateur avancé dans le sac.");
		return;
	}
	
	map_pos_t target = { (uint16_t)x, (uint16_t)y };
	uint16_t zone;
	uint8_t point;
	
	MapPosToPointCode(target, &zone, &point);
	WatchRelocation(c, player_name);
	RequestUseAdvancedRelocator(c, c->player.current_kingdom_id, zone, point);
	BotReply(c, player_name, "Relocalisation", "Relocalisation vers X:%u Y:%u demandée (royaume %u), le résultat arrive dans un instant.",
		x, y, c->player.current_kingdom_id);
}

static void MigrateCommand(Connection *c, const char *player_name, bool is_admin, const char *args)
{
	char p = c->bot.command_prefix;
	unsigned kingdom, x, y;

	if (!is_admin) {
		BotReply(c, player_name, "Non autorisé", "Seuls les administrateurs peuvent faire migrer le château.");
		return;
	}

	if (!c->items_loaded) {
		BotReply(c, player_name, "Migration", "Le sac n'est pas encore chargé, réessayez dans un instant.");
		return;
	}
	
	if (sscanf(args, "%u %u %u", &kingdom, &x, &y) != 3) {
		BotReply(c, player_name, "Migration", "Usage : %cmigrate <royaume> <x> <y>", p);
		return;
	}
	
	if (kingdom < 1 || kingdom > 65535) {
		BotReply(c, player_name, "Migration", "Numéro de royaume invalide : %u.", kingdom);
		return;
	}
	
	if (kingdom == c->player.current_kingdom_id) {
		BotReply(c, player_name, "Migration", "Le château est déjà dans le royaume %u : utilisez %crelocate pour changer de coordonnées.", kingdom, p);
		return;
	}
	
	if (!CheckTileMapPos((int)x, (int)y)) {
		BotReply(c, player_name, "Migration", "Coordonnées invalides : X:%u Y:%u n'est pas une case du royaume.", x, y);
		return;
	}
	
	map_pos_t target = { (uint16_t)x, (uint16_t)y };
	uint16_t zone;
	uint8_t point;
	
	MapPosToPointCode(target, &zone, &point);
	
	if (!MigrationStart(c, player_name, (uint16_t)kingdom, (uint16_t)x, (uint16_t)y, zone, point)) {
		BotReply(c, player_name, "Migration", "Une migration est déjà en cours.");
		return;
	}
	
	// the free migration (offered to returning players) is tried; without it migration scrolls are needed
	char note[240];
	
	MigrationScrollStatus(c, note, sizeof(note));
	BotReply(c, player_name, "Migration", "Migration vers le royaume %u en X:%u Y:%u demandée. %s", kingdom, x, y, note);
}

/* Called when the server answers the use of a relocator: tells whoever asked. */
void ReportRelocation(Connection *c, bool ok, uint8_t status)
{
	if (c->relocation.report_to[0] == '\0' || time(NULL) > c->relocation.report_until)
		return;

	char who[13];

	snprintf(who, sizeof(who), "%s", c->relocation.report_to);
	c->relocation.report_to[0] = '\0';

	if (ok) {
		map_pos_t pos = getTileMapPosbyPointCode(c->player.zone_id, c->player.point_id);

		BotReply(c, who, "Relocalisation", "Château déplacé : royaume %u, X:%u Y:%u.", c->player.current_kingdom_id, pos.x, pos.y);
	} else {
		BotReply(c, who, "Relocalisation", "L'utilisation de l'objet a échoué (code %u) : le château n'a peut-être pas été déplacé.", status);
	}
}

/* $join <tag> / $leave. Administrators only, one operation in flight at a time.
 * Outcome (success or refusal) is always reported back to whoever asked. */
static void AllianceJoinCommand(Connection *c, const char *player_name, bool is_admin, const char *args)
{
	char p = c->bot.command_prefix;

	if (!is_admin) {
		BotReply(c, player_name, "Non autorisé", "Seuls les administrateurs peuvent faire rejoindre une guilde.");
		return;
	}

	if (c->alliance_op.state != ALLIANCE_OP_NONE) {
		BotReply(c, player_name, "Guilde", "Une opération de guilde est déjà en cours, réessayez dans un instant.");
		return;
	}

	size_t len = strlen(args);
	while (len > 0 && args[len - 1] == ' ') len--;

	if (len != 3) {
		BotReply(c, player_name, "Guilde", "Usage : %cjoin <tag sur 3 caractères, sensible à la casse>", p);
		return;
	}

	snprintf(c->alliance_op.tag, sizeof(c->alliance_op.tag), "%.3s", args);
	snprintf(c->alliance_op.requester, sizeof(c->alliance_op.requester), "%s", player_name);

	if (c->RoleAlliance.Rank != NONE) {
		c->alliance_op.state = ALLIANCE_OP_LEAVING_TO_JOIN;
		RequestAllianceQuit(c);
		BotReply(c, player_name, "Guilde", "Départ de la guilde actuelle avant de rejoindre \"%s\"...", c->alliance_op.tag);
		return;
	}

	c->alliance_op.state = ALLIANCE_OP_JOIN_SEARCHING;
	RequestAllianceSearchByTag(c, c->alliance_op.tag);
	BotReply(c, player_name, "Guilde", "Recherche de la guilde \"%s\"...", c->alliance_op.tag);
}

static void AllianceLeaveCommand(Connection *c, const char *player_name, bool is_admin)
{
	if (!is_admin) {
		BotReply(c, player_name, "Non autorisé", "Seuls les administrateurs peuvent faire quitter une guilde.");
		return;
	}

	if (c->alliance_op.state != ALLIANCE_OP_NONE) {
		BotReply(c, player_name, "Guilde", "Une opération de guilde est déjà en cours, réessayez dans un instant.");
		return;
	}

	snprintf(c->alliance_op.requester, sizeof(c->alliance_op.requester), "%s", player_name);
	c->alliance_op.state = ALLIANCE_OP_LEAVING;

	RequestAllianceQuit(c);
}

/* ------------------------------------------------------------------------
 * Commands
 * ------------------------------------------------------------------------ */

/* The command words stay English; `label` is the French name used in the answers. */
static const struct {
	const char   *name;
	ResourceType  type;
	const char   *label;
} RESOURCE_COMMANDS[] = {
	{ "food",  RESOURCE_FOOD, "nourriture" },
	{ "stone", RESOURCE_ROCK, "pierre"     },
	{ "wood",  RESOURCE_WOOD, "bois"       },
	{ "ore",   RESOURCE_ORE,  "minerai"    },
	{ "gold",  RESOURCE_GOLD, "or"         }
};

#define RESOURCE_COMMAND_COUNT (sizeof(RESOURCE_COMMANDS) / sizeof(RESOURCE_COMMANDS[0]))

/* True when `message` is exactly the command `name` or starts with it followed by a space. args = the rest, without leading spaces. */
static bool IsCommand(const char *message, const char *name, const char **args)
{
	return StartsWithWord(message, name, args);
}

static void ShowHelp(Connection *c, const char *player_name, bool is_admin)
{
	char text[1100];
	char names[64] = {0};
	char p = c->bot.command_prefix;
	const char *first = "";
	size_t used = 0, count = 0;

	for (size_t i = 0; i < RESOURCE_COMMAND_COUNT; i++) {
		if (BankAllows(c, player_name, RESOURCE_COMMANDS[i].type)) {
			used += (size_t)snprintf(names + used, sizeof(names) - used, "%s%s", count ? "|" : "", RESOURCE_COMMANDS[i].name);

			if (count == 0)
				first = RESOURCE_COMMANDS[i].name;

			count++;
		}
	}

	size_t n = (size_t)snprintf(text, sizeof(text), "Commandes (préfixe %c) :\n", p);

	if (count)
		n += (size_t)snprintf(text + n, sizeof(text) - n, "%c<%s> <montant> - recevoir des ressources, ex. %c%s 5M\n",
			p, names, p, first);

	n += (size_t)snprintf(text + n, sizeof(text) - n, "%cstop - annuler votre livraison en cours\n", p);
	n += (size_t)snprintf(text + n, sizeof(text) - n, "%chelp - cette liste", p);

	if (is_admin) {
		n += (size_t)snprintf(text + n, sizeof(text) - n, "\n%cbank bal - solde de la banque, du sac et total", p);
		n += (size_t)snprintf(text + n, sizeof(text) - n, "\n%cadmin list|add <joueur>|remove <joueur> - gérer les administrateurs", p);
		n += (size_t)snprintf(text + n, sizeof(text) - n, "\n%crelocate random|<x> <y> - déplacer le château", p);
		n += (size_t)snprintf(text + n, sizeof(text) - n, "\n%cmigrate <royaume> <x> <y> - migrer vers un autre royaume", p);
		n += (size_t)snprintf(text + n, sizeof(text) - n, "\n%cjoin <tag> - rejoindre une guilde (tag sur 3 caractères)", p);
		n += (size_t)snprintf(text + n, sizeof(text) - n, "\n%cleave - quitter la guilde actuelle", p);
		n += (size_t)snprintf(text + n, sizeof(text) - n, "\n%csu <joueur> - équivaut à %cadmin add", p, p);
	}

	(void)n;
	BotReply(c, player_name, "Commandes", "%s", text);
}

static void StopTransfer(Connection *c, const char *player_name, bool is_admin)
{
	if (c->transfer.state == TRANSFER_IDLE) {
		BotReply(c, player_name, "Livraison", "Aucune livraison en cours.");
		return;
	}

	if (!is_admin && strcmp(c->transfer.target_name, player_name) != 0) {
		BotReply(c, player_name, "Livraison", "Cette livraison n'est pas la vôtre.");
		return;
	}

	memset(&c->transfer, 0, sizeof(c->transfer));
	c->transfer.state = TRANSFER_IDLE;

	BotReply(c, player_name, "Livraison annulée", "Livraison annulée. Les marches déjà parties arriveront quand même.");
}

void command_handler(Connection *c, const char *player_name, const char *message, CommandChannel source)
{
	if (c->bot.command_prefix == 0) return;

	if (message[0] != c->bot.command_prefix) return;

	// command.input: only the listed channels are read
	if (!(c->bot.command_input_mask & (1u << source))) return;

	message++; // skip prefix

	const char *args;
	bool is_admin = IsAdmin(c, player_name);

	if (IsCommand(message, "help", &args)) {
		ShowHelp(c, player_name, is_admin);
		return;
	}

	if (IsCommand(message, "stop", &args)) {
		StopTransfer(c, player_name, is_admin);
		return;
	}

	for (size_t i = 0; i < RESOURCE_COMMAND_COUNT; i++) {
		if (IsCommand(message, RESOURCE_COMMANDS[i].name, &args)) {
			ResourceCommandHandler(c, player_name, args, RESOURCE_COMMANDS[i].type, RESOURCE_COMMANDS[i].label);
			return;
		}
	}

	if (IsCommand(message, "bank", &args) && strncmp(args, "bal", 3) == 0 && (args[3] == '\0' || args[3] == ' ')) {
		ShowBankBalance(c, player_name);
		return;
	}

	if (IsCommand(message, "admin", &args)) {
		AdminCommand(c, player_name, is_admin, args);
		return;
	}

	if (IsCommand(message, "relocate", &args)) {
		RelocateCommand(c, player_name, is_admin, args);
		return;
	}

	if (IsCommand(message, "migrate", &args)) {
		MigrateCommand(c, player_name, is_admin, args);
		return;
	}

	if (IsCommand(message, "join", &args)) {
		AllianceJoinCommand(c, player_name, is_admin, args);
		return;
	}

	if (IsCommand(message, "leave", &args)) {
		AllianceLeaveCommand(c, player_name, is_admin);
		return;
	}

	if (IsCommand(message, "su", &args)) {
		if (*args == '\0') {
			BotReply(c, player_name, "Administrateurs", "Usage : %csu <joueur>", c->bot.command_prefix);
			return;
		}

		// kept for compatibility: same as "admin add"
		char add[64];

		snprintf(add, sizeof(add), "add %s", args);
		AdminCommand(c, player_name, is_admin, add);
		return;
	}
}

uint64_t parse_number_u64(const char *str) {
    double value = 0.0;
    char suffix = '\0';

    // Read numeric part and optional suffix
    sscanf(str, "%lf%c", &value, &suffix);
    suffix = tolower(suffix); // handle both lowercase and uppercase

    // Apply multiplier
    switch (suffix) {
        case 'k': value *= 1000ULL; break;
        case 'm': value *= 1000000ULL; break;
        case 'b': value *= 1000000000ULL; break;
        default: break;//return 0;//break; // no suffix
    }

    if (value < 0) value = 0;

    return (uint64_t)value;
}


static void ResourceCommandHandler(
    Connection *c,
    const char *player_name,
    const char *message,
    ResourceType type,
    const char *name
)
{
	
	if (!BankAllows(c, player_name, type))
		return;

	/* ResourceTransferTick() waits forever for supply_capacity > 0 with no
	 * timeout - refuse up front instead of silently swallowing the command
	 * (no Trading Post, or its level hasn't been received from the server yet). */
	if (c->supply_capacity == 0) {
		BotReply(c, player_name, "Indisponible",
			"Le Poste de Commerce n'a pas de capacité de livraison disponible (bâtiment absent, niveau trop bas, ou pas encore chargé). Réessayez plus tard.");
		return;
	}

	/* max_marches is 0 on a real account only before the server has sent it (SendResourceMarch()
	 * would then read 0 >= 0 as "all marches busy" and retry forever, silently, with no report). */
	if (c->player.max_marches == 0) {
		BotReply(c, player_name, "Indisponible",
			"Nombre de marches disponibles pas encore reçu du serveur. Réessayez plus tard.");
		return;
	}

	if (c->transfer.state != TRANSFER_IDLE) {
		// Same player -> replace current pending request 
		if (strcmp(c->transfer.target_name, player_name) == 0) {
			memset(&c->transfer, 0, sizeof(c->transfer));
			c->transfer.state = TRANSFER_IDLE;
			// Continue below and assign the new request 
		} else {
			// Different player -> reject
			BotReply(
				c,
				player_name,
				"Occupé",
				"Envoi de ressources en cours à %s. Utilisez %cstop pour annuler.",
				c->transfer.target_name,
				c->bot.command_prefix
			);
			
			return;
		}
	}
	
	char amount_str[32] = {0};
	
	if (sscanf(message, "%31s", amount_str) != 1)
		return;
	
	uint64_t amount = parse_number_u64(amount_str);

	if (amount == 0 || amount > UINT32_MAX)
		return;

	/* The game deducts bank.delivery_tax_percent on arrival (not visible anywhere in the
	 * march packets themselves - observed only in the recipient's actual stock change).
	 * Gross the request up so what arrives matches what was asked for. */
	if (c->bank.delivery_tax_percent > 0.0 && c->bank.delivery_tax_percent < 100.0) {
		uint64_t gross = (uint64_t)((double)amount * 100.0 / (100.0 - c->bank.delivery_tax_percent) + 0.5);
		LOGD("[TRANSFER] Taxe %.2f%% : %llu demandé -> %llu envoyé\n",
			c->bank.delivery_tax_percent, (unsigned long long)amount, (unsigned long long)gross);
		amount = gross > UINT32_MAX ? UINT32_MAX : gross;
	}

	uint32_t current = 0;
	uint32_t reserve = 0;
	
	switch (type) {
		case RESOURCE_FOOD:
			current = c->resources.food;
			reserve = c->bank.reserve.food;
			break;
		case RESOURCE_ROCK:
			current = c->resources.rock;
			reserve = c->bank.reserve.rock;
			break;
		case RESOURCE_WOOD:
			current = c->resources.wood;
			reserve = c->bank.reserve.wood;
			break;
		case RESOURCE_ORE:
			current = c->resources.ore;
			reserve = c->bank.reserve.ore;
			break;
		case RESOURCE_GOLD:
			current = c->resources.gold;
			reserve = c->bank.reserve.gold;
			break;
	}
	
	uint32_t available = current > reserve ? current - reserve : 0;
	uint64_t not_before = 0;

	if (amount > available) {
		// bank.use_bag_*: cover the difference with resource items from the bag
		BagUse plan[BAG_PLAN_MAX];
		int used = BankMayUseBag(c, type) ? BagPlan(c, type, amount - available, plan) : -1;

		if (used > 0) {
			BagApply(c, plan, used);
			not_before = now_ms() + 3000; // wait for the resources to be credited
		} else {
			char available_str[20];
			uint64_t reachable = available;
			
			if (BankMayUseBag(c, type))
				reachable += BagTotal(c, type);
			
			format_number2(reachable, available_str, sizeof(available_str));
			
			BotReply(
				c,
				player_name,
				"Ressources insuffisantes",
				"Ressources insuffisantes (%s) : %s disponible.",
				name,
				available_str
			);
			return;
		}
	}
	
	c->transfer.amount = (uint32_t)amount;
	c->transfer.remaining = (uint32_t)amount;
	c->transfer.resource_type = type;
	c->transfer.not_before = not_before;
	
	snprintf(c->transfer.target_name, sizeof(c->transfer.target_name), "%s", player_name);
	
	c->transfer.state = TRANSFER_FIND_TARGET;
}



uint64_t GetBagFood(Connection *c) {
	return 
		((uint64_t)c->items[FOOD_5K].quantity * 5000) + // FOOD 5K
		((uint64_t)c->items[FOOD_30K].quantity * 30000) + // FOOD 30K
		((uint64_t)c->items[FOOD_150K].quantity * 150000) + // FOOD 150K
		((uint64_t)c->items[FOOD_500K].quantity * 500000) + // FOOD 500K
		((uint64_t)c->items[FOOD_2M].quantity * 2000000) + // FOOD 2M
		((uint64_t)c->items[FOOD_6M].quantity * 6000000) + // FOOD 6M
		((uint64_t)c->items[FOOD_20M].quantity * 20000000) + // FOOD 20M
		((uint64_t)c->items[FOOD_60M].quantity * 60000000); // FOOD 60M
}


uint64_t GetBagRock(Connection *c) {
	return 
		((uint64_t)c->items[STONE_3K].quantity   * 3000) + // STONE 3K
		((uint64_t)c->items[STONE_10K].quantity  * 10000) + // STONE 10K
		((uint64_t)c->items[STONE_50K].quantity  * 50000) + // STONE 50K
		((uint64_t)c->items[STONE_150K].quantity * 150000) + // STONE 150K
		((uint64_t)c->items[STONE_500K].quantity * 500000) + // STONE 500K
		((uint64_t)c->items[STONE_1_5M].quantity * 1500000) + // STONE 1.5M
		((uint64_t)c->items[STONE_5M].quantity   * 5000000) + // STONE 5M
		((uint64_t)c->items[STONE_15M].quantity  * 15000000); // STONE 15M
}


uint64_t GetBagWood(Connection *c) {
	return 
		((uint64_t)c->items[TIMBER_3K].quantity * 3000) + // WOOD 3K
		((uint64_t)c->items[TIMBER_10K].quantity * 10000) + // WOOD 10K
		((uint64_t)c->items[TIMBER_50K].quantity * 50000) + // WOOD 50K
		((uint64_t)c->items[TIMBER_150K].quantity * 150000) + // WOOD 150K
		((uint64_t)c->items[TIMBER_500K].quantity * 500000) + // WOOD 500K
		((uint64_t)c->items[TIMBER_1_5M].quantity * 1500000) + // WOOD 1.5M
		((uint64_t)c->items[TIMBER_5M].quantity * 5000000) + // WOOD 5M
		((uint64_t)c->items[TIMBER_15M].quantity * 15000000); // WOOD 15M
}

uint64_t GetBagOre(Connection *c) {
	return 
		((uint64_t)c->items[ORE_3K].quantity * 3000) + // ORE 3K
		((uint64_t)c->items[ORE_10K].quantity * 10000) + // ORE 10K
		((uint64_t)c->items[ORE_50K].quantity * 50000) + // ORE 50K
		((uint64_t)c->items[ORE_150K].quantity * 150000) + // ORE 150K
		((uint64_t)c->items[ORE_500K].quantity * 500000) + // ORE 500K
		((uint64_t)c->items[ORE_1_5M].quantity * 1500000) + // ORE 1.5M
		((uint64_t)c->items[ORE_5M].quantity * 5000000) + // ORE 5M
		((uint64_t)c->items[ORE_15M].quantity * 15000000); // ORE 15M
}

uint64_t GetBagGold(Connection *c) {
	return 
		((uint64_t)c->items[GOLD_3K].quantity   * 3000) + // FOOD 5K
		((uint64_t)c->items[GOLD_15K].quantity  * 15000) + // FOOD 30K
		((uint64_t)c->items[GOLD_50K].quantity  * 50000) + // FOOD 150K
		((uint64_t)c->items[GOLD_200K].quantity * 200000) + // FOOD 500K
		((uint64_t)c->items[GOLD_600K].quantity * 600000) + // FOOD 2M
		((uint64_t)c->items[GOLD_2M].quantity   * 2000000) + // FOOD 6M
		((uint64_t)c->items[GOLD_6M].quantity   * 6000000); // FOOD 20M
}


void ShowBankBalance(Connection *c, const char *player_name) {
	if (!IsAdmin(c, player_name)) {
		// Return message if necessary 
		BotReply(c, player_name, "Non autorisé", "Vous n'avez pas la permission de voir le solde de la banque.");
		return;
	}
	
	if (!c->items_loaded) {
		BotReply(c, player_name, "Problème", "Un problème est survenu, réessayez dans un instant.");
		return;
	}
	
	
	char bank_food[20];
	char bank_rock[20];
	char bank_wood[20];
	char bank_ore [20];
	char bank_gold[20];
	
	char bag_food[20];
	char bag_rock[20];
	char bag_wood[20];
	char bag_ore [20];
	char bag_gold[20];
	
	char sum_food[20];
	char sum_rock[20];
	char sum_wood[20];
	char sum_ore [20];
	char sum_gold[20];
	
	
	// Format BANK
	format_number2(c->resources.food, bank_food, sizeof(bank_food));
	format_number2(c->resources.rock, bank_rock, sizeof(bank_rock));
	format_number2(c->resources.wood, bank_wood, sizeof(bank_wood));
	format_number2(c->resources.ore,  bank_ore,  sizeof(bank_ore));
	format_number2(c->resources.gold, bank_gold, sizeof(bank_gold));
	
	uint64_t BagFood = GetBagFood(c);
	uint64_t BagRock = GetBagRock(c);
	uint64_t BagWood = GetBagWood(c);
	uint64_t BagOre = GetBagOre(c);
	uint64_t BagGold= GetBagGold(c);
	
	// Format BAG
	format_number2(BagFood, bag_food, sizeof(bag_food));
	format_number2(BagRock, bag_rock, sizeof(bag_rock));
	format_number2(BagWood, bag_wood, sizeof(bag_wood));
	format_number2(BagOre,  bag_ore,  sizeof(bag_ore));
	format_number2(BagGold, bag_gold, sizeof(bag_gold));
		
	// Format TOTAL
	format_number2(c->resources.food + BagFood,  sum_food, sizeof(sum_food));
	format_number2(c->resources.rock + BagRock,  sum_rock, sizeof(sum_rock));
	format_number2(c->resources.wood + BagWood,  sum_wood, sizeof(sum_wood));
	format_number2(c->resources.ore  + BagOre,   sum_ore,  sizeof(sum_ore));
	format_number2(c->resources.gold + BagGold,  sum_gold, sizeof(sum_gold));
	
	BotReply(c, player_name, "Solde de la banque", 
		"[BANQUE] Nourriture : %s | Pierre : %s | Bois : %s | Minerai : %s | Or : %s\n"
		"[SAC] Nourriture : %s | Pierre : %s | Bois : %s | Minerai : %s | Or : %s\n"
		"[TOTAL] Nourriture : %s | Pierre : %s | Bois : %s | Minerai : %s | Or : %s",
		bank_food, bank_rock, bank_wood, bank_ore, bank_gold,
		bag_food, bag_rock, bag_wood, bag_ore, bag_gold,
		sum_food, sum_rock, sum_wood, sum_ore, sum_gold
	);
	return;
}