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
#include "guildbank.h"
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

void ShowBankBalance(Connection *c, const char *player_name, CommandChannel channel);
static void ResourceCommandHandler(
    Connection *c,
    const char *player_name,
    const char *message,
    ResourceType type,
    const char *name
);
static void BalanceCommand(Connection *c, const char *player_name, bool is_admin, const char *args);
static void AdminResourceCommand(Connection *c, const char *player_name, bool is_admin, const char *args, ResourceType type);
static void AdminRssCommand(Connection *c, const char *player_name, bool is_admin, const char *args);
static void RssCommand(Connection *c, const char *player_name, const char *args);

/* ------------------------------------------------------------------------
 * Replies: mail, alliance chat or world chat, depending on command.output
 * ------------------------------------------------------------------------ */

static void BotReplyV(Connection *c, const char *player_name, const char *subject, CommandChannel channel, const char *fmt, va_list args)
{
	// 2048, not 1024: $help's text alone (ShowHelp's buffer, up to 1700) was getting silently
	// truncated by vsnprintf here once $adminrss/$rss were added to it.
	char text[2048];

	vsnprintf(text, sizeof(text), fmt, args);

	if (channel == COMMAND_CHANNEL_MAIL) {
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

	RequestSendChat(c, channel == COMMAND_CHANNEL_GUILD ? 1 : 0, line);
}

/* Same as BotReply, but the channel is picked by the caller instead of always
 * following command.output - for a command like "$bank bal chat" that lets
 * whoever asks override where just this one answer goes. */
void BotReplyTo(Connection *c, const char *player_name, const char *subject, CommandChannel channel, const char *fmt, ...)
{
	va_list args;
	va_start(args, fmt);
	BotReplyV(c, player_name, subject, channel, fmt, args);
	va_end(args);
}

void BotReply(Connection *c, const char *player_name, const char *subject, const char *fmt, ...)
{
	va_list args;
	va_start(args, fmt);
	BotReplyV(c, player_name, subject, c->bot.command_output, fmt, args);
	va_end(args);
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
	char text[1700];
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

	if (c->guildbank.enabled) {
		n += (size_t)snprintf(text + n, sizeof(text) - n, "%c<ressource> <montant>|all - retirer votre solde, ex. %cfood 1M\n", p, p);
		n += (size_t)snprintf(text + n, sizeof(text) - n,
			"%crss <food> <stone> <wood> <ore> <gold> - retirer plusieurs ressources d'un coup (0 = aucune), ex. %crss 0 0 0 0 5M\n", p, p);
		n += (size_t)snprintf(text + n, sizeof(text) - n, "%cbal - votre solde (les dépôts se font en envoyant des ressources au bot)\n", p);
	}
	n += (size_t)snprintf(text + n, sizeof(text) - n, "%cstop - annuler votre livraison en cours\n", p);
	n += (size_t)snprintf(text + n, sizeof(text) - n, "%chelp - cette liste", p);

	if (is_admin) {
		n += (size_t)snprintf(text + n, sizeof(text) - n, "\n%cbank bal [chat|mail] - solde de la banque, du sac et total", p);
		n += (size_t)snprintf(text + n, sizeof(text) - n, "\n%cadmin list|add <joueur>|remove <joueur> - gérer les administrateurs", p);
		n += (size_t)snprintf(text + n, sizeof(text) - n, "\n%cadmin<ressource> <joueur> <montant> - envoyer depuis le stock, ex. %cadminfood Bob 5M", p, p);
		n += (size_t)snprintf(text + n, sizeof(text) - n,
			"\n%cadminrss <food> <stone> <wood> <ore> <gold> <joueur> - envoyer plusieurs ressources d'un coup (0 = aucune), ex. %cadminrss 0 0 0 0 5M Bob", p, p);
		n += (size_t)snprintf(text + n, sizeof(text) - n, "\n%crecall - rappeler toutes les troupes, aucune marche ensuite pendant un moment", p);
		n += (size_t)snprintf(text + n, sizeof(text) - n, "\n%crelocate random|<x> <y> - déplacer le château", p);
		n += (size_t)snprintf(text + n, sizeof(text) - n, "\n%cmigrate <royaume> <x> <y> - migrer vers un autre royaume", p);
		n += (size_t)snprintf(text + n, sizeof(text) - n, "\n%cjoin <tag> - rejoindre une guilde (tag sur 3 caractères)", p);
		n += (size_t)snprintf(text + n, sizeof(text) - n, "\n%cleave - quitter la guilde actuelle", p);
		n += (size_t)snprintf(text + n, sizeof(text) - n, "\n%csu <joueur> - équivaut à %cadmin add", p, p);
	}

	(void)n;
	BotReply(c, player_name, "Commandes", "%s", text);
}

/* $recall: take every march back, then no march for recall.pause_seconds. */
static void RecallCommand(Connection *c, const char *player_name, bool is_admin)
{
	if (!is_admin) {
		BotReply(c, player_name, "Non autorisé", "Seuls les administrateurs peuvent rappeler les troupes.");
		return;
	}

	char pause[32], extra[128] = "";
	FormatDurationFr(c->recall.pause_seconds, pause, sizeof(pause));

	// a delivery in progress cannot go on: its next marches would leave during the pause
	if (c->transfer.state != TRANSFER_IDLE) {
		char target[sizeof(c->transfer.target_name)], asker[sizeof(c->transfer.target_name)];
		snprintf(target, sizeof(target), "%s", c->transfer.target_name);
		snprintf(asker, sizeof(asker), "%s", TransferRequester(c));
		if (asker[0] != '\0' && strcmp(asker, player_name) != 0)
			BotReply(c, asker, "Livraison interrompue",
				"Les troupes du bot sont rappelées, votre livraison est annulée. Les marches déjà parties arriveront quand même.");
		snprintf(extra, sizeof(extra), " La livraison en cours%s%s est annulée.", target[0] ? " à " : "", target);
		AbortTransfer(c);
	}

	// the requests waiting behind it would start when the pause ends: cancel them too, and say so
	while (c->transfer_queue_count > 0) {
		if (strcmp(c->transfer_queue[0].requester, player_name) != 0)
			BotReply(c, c->transfer_queue[0].requester, "Livraison interrompue",
				"Les troupes du bot sont rappelées, votre demande en attente est annulée. Renouvelez-la après la pause.");
		TransferQueueRemove(c, c->transfer_queue[0].requester);
	}

	switch (StartRecall(c, player_name)) {
		case RECALL_STARTED:
			if (c->recall.pause_seconds)
				BotReply(c, player_name, "Rappel",
					"Rappel des troupes lancé (%u marche(s) sortie(s)). Aucune marche ne sera envoyée pendant %s.%s",
					c->player.current_marches, pause, extra);
			else
				BotReply(c, player_name, "Rappel",
					"Rappel des troupes lancé (%u marche(s) sortie(s)). Pause désactivée (recall.pause_seconds = 0).%s",
					c->player.current_marches, extra);
			break;
		case RECALL_RUNNING:
			BotReply(c, player_name, "Rappel", "Un rappel est déjà en cours. Aucune marche pendant %s à partir de maintenant.%s", pause, extra);
			break;
		case RECALL_NO_MARCHES:
			BotReply(c, player_name, "Rappel", "Aucune marche sortie, rien à rappeler. Aucune marche ne sera envoyée pendant %s.%s", pause, extra);
			break;
		case RECALL_NO_DATA:
			BotReply(c, player_name, "Rappel",
				"Le nombre de marches n'est pas encore reçu du serveur, aucun rappel envoyé. Aucune marche ne sera envoyée pendant %s.%s",
				pause, extra);
			break;
	}
}

static void StopTransfer(Connection *c, const char *player_name, bool is_admin)
{
	// a request that is still waiting its turn is cancelled first: nothing was sent, nothing was debited
	if (c->transfer.state == TRANSFER_IDLE || (!is_admin && strcmp(TransferRequester(c), player_name) != 0)) {
		if (TransferQueueRemove(c, player_name)) {
			BotReply(c, player_name, "Livraison", "Votre demande en attente est annulée.");
			return;
		}
	}

	if (c->transfer.state == TRANSFER_IDLE) {
		BotReply(c, player_name, "Livraison", "Aucune livraison en cours.");
		return;
	}

	if (!is_admin && strcmp(TransferRequester(c), player_name) != 0) {
		BotReply(c, player_name, "Livraison", "Cette livraison n'est pas la vôtre.");
		return;
	}

	AbortTransfer(c);

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

	if (IsCommand(message, "recall", &args)) {
		RecallCommand(c, player_name, is_admin);
		return;
	}

	if (IsCommand(message, "bal", &args)) {
		BalanceCommand(c, player_name, is_admin, args);
		return;
	}

	if (IsCommand(message, "adminrss", &args)) {
		AdminRssCommand(c, player_name, is_admin, args);
		return;
	}

	if (IsCommand(message, "rss", &args)) {
		RssCommand(c, player_name, args);
		return;
	}

	for (size_t i = 0; i < RESOURCE_COMMAND_COUNT; i++) {
		char admin_name[16];
		snprintf(admin_name, sizeof(admin_name), "admin%s", RESOURCE_COMMANDS[i].name);
		if (IsCommand(message, admin_name, &args)) {
			AdminResourceCommand(c, player_name, is_admin, args, RESOURCE_COMMANDS[i].type);
			return;
		}
	}

	for (size_t i = 0; i < RESOURCE_COMMAND_COUNT; i++) {
		if (IsCommand(message, RESOURCE_COMMANDS[i].name, &args)) {
			ResourceCommandHandler(c, player_name, args, RESOURCE_COMMANDS[i].type, RESOURCE_COMMANDS[i].label);
			return;
		}
	}

	if (IsCommand(message, "bank", &args) && strncmp(args, "bal", 3) == 0 && (args[3] == '\0' || args[3] == ' ')) {
		const char *channel_arg = args + 3;
		while (*channel_arg == ' ') channel_arg++;

		CommandChannel channel = c->bot.command_output;
		if (strcmp(channel_arg, "chat") == 0)
			channel = COMMAND_CHANNEL_GUILD;
		else if (strcmp(channel_arg, "mail") == 0)
			channel = COMMAND_CHANNEL_MAIL;

		ShowBankBalance(c, player_name, channel);
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


/* What every delivery needs before it can even be queued; the refusal goes to `player_name`. */
static bool DeliveryReady(Connection *c, const char *player_name)
{
	/* after $recall the bot sends no march for a while: a delivery would need some */
	if (MarchesPaused()) {
		char left[32];
		FormatDurationFr(MarchesPauseSecondsLeft(), left, sizeof(left));
		BotReply(c, player_name, "Indisponible",
			"Les marches du bot sont suspendues (troupes rappelées), réessayez dans %s.", left);
		return false;
	}

	/* c->resources is zeroed by the reconnect memset until the server sends fresh values
	 * (resource_loaded). Without this, a command arriving right after a reconnect sees 0
	 * and wrongly refuses as "not enough", however large the real balance actually is. */
	if (!c->resource_loaded) {
		BotReply(c, player_name, "Indisponible",
			"Le solde n'est pas encore reçu du serveur (reconnexion en cours ?). Réessayez dans un instant.");
		return false;
	}

	/* ResourceTransferTick() waits forever for supply_capacity > 0 with no
	 * timeout - refuse up front instead of silently swallowing the command
	 * (no Trading Post, or its level hasn't been received from the server yet). */
	if (c->supply_capacity == 0) {
		BotReply(c, player_name, "Indisponible",
			"Le Poste de Commerce n'a pas de capacité de livraison disponible (bâtiment absent, niveau trop bas, ou pas encore chargé). Réessayez plus tard.");
		return false;
	}

	/* max_marches is 0 on a real account only before the server has sent it (SendResourceMarch()
	 * would then read 0 >= 0 as "all marches busy" and retry forever, silently, with no report). */
	if (c->player.max_marches == 0) {
		BotReply(c, player_name, "Indisponible",
			"Nombre de marches disponibles pas encore reçu du serveur. Réessayez plus tard.");
		return false;
	}

	return true;
}

/* "1 234 567" - a balance is read digit by digit, unlike the 1.23M the rest of the bot prints. */
static void FormatExact(uint64_t value, char *out, size_t size)
{
	char digits[32];
	snprintf(digits, sizeof(digits), "%llu", (unsigned long long)value);
	size_t len = strlen(digits), o = 0;
	for (size_t i = 0; i < len && o + 2 < size; i++) {
		if (i > 0 && (len - i) % 3 == 0)
			out[o++] = ' ';
		out[o++] = digits[i];
	}
	out[o] = '\0';
}

static const char *ResourceLabel(ResourceType type)
{
	for (size_t i = 0; i < RESOURCE_COMMAND_COUNT; i++)
		if (RESOURCE_COMMANDS[i].type == type)
			return RESOURCE_COMMANDS[i].label;
	return "?";
}

/* Gross amount to send so that `net` arrives once the delivery tax is taken off. */
static uint64_t GrossUp(const Connection *c, uint64_t net)
{
	double tax = DeliveryTaxPercent(c);
	if (tax > 0.0 && tax < 100.0)
		return (uint64_t)((double)net * 100.0 / (100.0 - tax) + 0.5);
	return net;
}

/* $adminrss/$rss: one field of the command line. 0 means "skip this resource" (not an error) -
 * only a non-zero amount that overflows once grossed up is rejected. */
static bool ParseGrossAmount(const Connection *c, const char *str, uint32_t *out)
{
	uint64_t net = parse_number_u64(str);
	if (net == 0) {
		*out = 0;
		return true;
	}
	uint64_t gross = GrossUp(c, net);
	if (gross > UINT32_MAX)
		return false;
	*out = (uint32_t)gross;
	return true;
}

/* $adminrss/$rss: orders the (already grossed up) amounts - gross[] indexed by ResourceType, same
 * order as RESOURCE_COMMANDS/the command's own arguments (food, stone, wood, ore, gold) - into the
 * priority the resources must leave in: gold, ore (minerai), wood (bois), stone (pierre), then food
 * last (the one a recipient is least likely to be short on). Resources left at 0 are skipped.
 * Returns how many lines were filled. */
static uint8_t BuildPriorityLines(const uint32_t gross[5], TransferLine lines[TRANSFER_BATCH_MAX])
{
	static const ResourceType priority[5] = { RESOURCE_GOLD, RESOURCE_ORE, RESOURCE_WOOD, RESOURCE_ROCK, RESOURCE_FOOD };
	uint8_t count = 0;
	for (int i = 0; i < 5; i++) {
		ResourceType type = priority[i];
		if (gross[type] > 0)
			lines[count++] = (TransferLine){ .type = type, .amount = gross[type] };
	}
	return count;
}

/* What arrives of a gross amount once the tax is taken off. */
static uint64_t NetOf(const Connection *c, uint64_t gross)
{
	double tax = DeliveryTaxPercent(c);
	if (tax > 0.0 && tax < 100.0)
		return (uint64_t)((double)gross * (100.0 - tax) / 100.0);
	return gross;
}

/* ---- guild bank (guildbank.h): a player's balance, taken back with the resource commands ---- */

/* True when `name` is in the bot's guild. The list comes from the server at login and is refreshed, at most every
 * 30 seconds, when somebody is not in it (a player who joined after). The refusal goes to `reply_to`. */
static bool GuildMemberOrReply(Connection *c, const char *name, const char *reply_to, bool is_self)
{
	static time_t last_refresh;

	if (c->alliance_member.count == 0) {
		BotReply(c, reply_to, "Guilde", "La liste des membres de la guilde n'est pas encore reçue, réessayez dans un instant.");
		return false;
	}

	for (uint32_t i = 0; i < c->alliance_member.count && i < MAX_ALLIANCE_MEMBER; i++)
		if (strcmp(c->alliance_member.member[i].name, name) == 0)
			return true;

	time_t now = time(NULL);
	if (now - last_refresh >= 30) {
		last_refresh = now;
		RequestAllianceMemberInfo(c);
	}

	if (is_self)
		BotReply(c, reply_to, "Guilde", "Cette commande est réservée aux membres de la guilde. Si vous venez d'y entrer, réessayez dans un instant.");
	else
		BotReply(c, reply_to, "Guilde", "%s n'est pas dans la guilde (ou la liste des membres n'est pas à jour, réessayez dans un instant).", name);
	return false;
}

/* Puts a request in the queue and tells the requester where they are. */
static void QueueDelivery(Connection *c, const TransferRequest *request, const char *what)
{
	bool busy = c->transfer.state != TRANSFER_IDLE || c->transfer_queue_count > 0;

	if (c->transfer.state != TRANSFER_IDLE && strcmp(TransferRequester(c), request->requester) == 0) {
		BotReply(c, request->requester, "Occupé",
			"Votre livraison précédente est encore en cours. Attendez sa fin ou utilisez %cstop.", c->bot.command_prefix);
		return;
	}

	if (!TransferQueuePush(c, request)) {
		BotReply(c, request->requester, "Occupé", "La file d'attente est pleine (%u demandes), réessayez plus tard.", TRANSFER_QUEUE_MAX);
		return;
	}

	if (busy)
		BotReply(c, request->requester, "En file", "%s : demande enregistrée, position %d dans la file.", what,
			TransferQueuePosition(c, request->requester));
}

/* $food 1M / $food all: takes resources back from the player's own balance. */
static void GuildWithdrawCommand(Connection *c, const char *player_name, const char *args, ResourceType type)
{
	GuildBankLoad(c);

	if (!GuildMemberOrReply(c, player_name, player_name, true))
		return;
	if (!DeliveryReady(c, player_name))
		return;

	const char *label = ResourceLabel(type);
	char amount_str[32] = {0};
	if (sscanf(args, "%31s", amount_str) != 1) {
		BotReply(c, player_name, "Retrait", "Usage : %c<ressource> <montant>|all, ex. %cfood 1M. %cbal montre votre solde.",
			c->bot.command_prefix, c->bot.command_prefix, c->bot.command_prefix);
		return;
	}

	uint64_t balance = GuildBankBalance(player_name, type);
	uint64_t gross, net;
	if (strcmp(amount_str, "all") == 0) {
		gross = balance;
		net = NetOf(c, gross);
	} else {
		net = parse_number_u64(amount_str);
		gross = GrossUp(c, net);
	}

	char have[32], have_net[32];
	FormatExact(balance, have, sizeof(have));
	FormatExact(NetOf(c, balance), have_net, sizeof(have_net));

	if (balance == 0) {
		BotReply(c, player_name, "Solde", "Votre solde de %s est vide.", label);
		return;
	}
	if (net == 0 || gross == 0 || gross > UINT32_MAX) {
		BotReply(c, player_name, "Retrait", "Montant invalide. Votre solde de %s : %s (%s reçus après la taxe).", label, have, have_net);
		return;
	}
	if (gross > balance) {
		BotReply(c, player_name, "Solde insuffisant",
			"Solde de %s insuffisant : %s, soit %s reçus après la taxe de %.1f%%. Demandez au plus %s, ou %call.",
			label, have, have_net, DeliveryTaxPercent(c), have_net, c->bot.command_prefix);
		return;
	}

	uint32_t stock = StockAvailable(c, type, true);
	if (gross > stock) {
		char in_stock[32];
		FormatExact(stock, in_stock, sizeof(in_stock));
		BotReply(c, player_name, "Indisponible",
			"La banque n'a actuellement que %s de %s en stock, moins que votre demande. Prévenez un administrateur.", in_stock, label);
		return;
	}

	TransferRequest request = { .lines = { { type, (uint32_t)gross } }, .line_count = 1, .from_balance = true };
	snprintf(request.requester, sizeof(request.requester), "%s", player_name);
	snprintf(request.target, sizeof(request.target), "%s", player_name);
	QueueDelivery(c, &request, "Retrait");
}

/* $bal [player]: the balance of the requester, or of another player for an administrator. */
static void BalanceCommand(Connection *c, const char *player_name, bool is_admin, const char *args)
{
	if (!c->guildbank.enabled)
		return;

	char who[13];
	snprintf(who, sizeof(who), "%s", player_name);
	if (*args != '\0') {
		if (!is_admin) {
			BotReply(c, player_name, "Non autorisé", "Seuls les administrateurs peuvent voir le solde d'un autre joueur.");
			return;
		}
		if (strlen(args) >= sizeof(who)) {
			BotReply(c, player_name, "Solde", "Pseudo trop long (12 caractères au plus).");
			return;
		}
		snprintf(who, sizeof(who), "%s", args);
	}

	GuildBankLoad(c);

	char text[600];
	size_t n = (size_t)snprintf(text, sizeof(text), "Solde de %s :", who);
	bool any = false;
	for (size_t i = 0; i < RESOURCE_COMMAND_COUNT; i++) {
		uint64_t balance = GuildBankBalance(who, RESOURCE_COMMANDS[i].type);
		if (balance == 0)
			continue;
		char exact[32], net[32];
		FormatExact(balance, exact, sizeof(exact));
		FormatExact(NetOf(c, balance), net, sizeof(net));
		n += (size_t)snprintf(text + n, sizeof(text) - n, "\n%s : %s (%s reçus après la taxe de %.1f%%)",
			RESOURCE_COMMANDS[i].label, exact, net, DeliveryTaxPercent(c));
		any = true;
	}
	if (!any)
		snprintf(text + n, sizeof(text) - n, " vide.");

	BotReply(c, player_name, "Solde", "%s", text);
}

/* $adminfood <player> <amount> and the same for the other resources: gives from the bot's stock, never from the members' deposits. */
static void AdminResourceCommand(Connection *c, const char *player_name, bool is_admin, const char *args, ResourceType type)
{
	if (!is_admin) {
		BotReply(c, player_name, "Non autorisé", "Seuls les administrateurs peuvent envoyer des ressources à un autre joueur.");
		return;
	}
	if (!DeliveryReady(c, player_name))
		return;

	// "Little Zyco 5M": the amount is the last word, the rest is the name (names can hold spaces)
	char line[64];
	snprintf(line, sizeof(line), "%s", args);
	char *end = line + strlen(line);
	while (end > line && end[-1] == ' ')
		*--end = '\0';
	char *amount_str = strrchr(line, ' ');
	if (!amount_str) {
		BotReply(c, player_name, "Envoi", "Usage : %cadmin<ressource> <pseudo> <montant>, ex. %cadminfood Bob 5M",
			c->bot.command_prefix, c->bot.command_prefix);
		return;
	}
	*amount_str++ = '\0';
	while (end > line && line[strlen(line) - 1] == ' ')
		line[strlen(line) - 1] = '\0';

	if (strlen(line) == 0 || strlen(line) >= 13) {
		BotReply(c, player_name, "Envoi", "Pseudo invalide (12 caractères au plus).");
		return;
	}

	uint64_t net = parse_number_u64(amount_str);
	uint64_t gross = GrossUp(c, net);
	if (net == 0 || gross > UINT32_MAX) {
		BotReply(c, player_name, "Envoi", "Montant invalide.");
		return;
	}

	if (c->guildbank.enabled) {
		GuildBankLoad(c);
		if (!GuildMemberOrReply(c, line, player_name, false))
			return;
	}

	uint32_t available = StockAvailable(c, type, false);
	if (gross > available) {
		char have[32];
		FormatExact(available, have, sizeof(have));
		BotReply(c, player_name, "Ressources insuffisantes",
			"Ressources insuffisantes (%s) : %s disponible, sans toucher à la réserve ni aux dépôts des membres.", ResourceLabel(type), have);
		return;
	}

	TransferRequest request = { .lines = { { type, (uint32_t)gross } }, .line_count = 1, .from_balance = false };
	snprintf(request.requester, sizeof(request.requester), "%s", player_name);
	snprintf(request.target, sizeof(request.target), "%s", line);
	QueueDelivery(c, &request, "Envoi");
}

/* $adminrss <food> <stone> <wood> <ore> <gold> <pseudo>: like $admin<ressource>, but sends up to
 * five resources to the same player in one command - gives from the bot's stock, never from the
 * members' deposits. 0 skips a resource; they leave in priority order (BuildPriorityLines), not
 * the order they are typed in. */
static void AdminRssCommand(Connection *c, const char *player_name, bool is_admin, const char *args)
{
	if (!is_admin) {
		BotReply(c, player_name, "Non autorisé", "Seuls les administrateurs peuvent envoyer des ressources à un autre joueur.");
		return;
	}
	if (!DeliveryReady(c, player_name))
		return;

	char food_s[32], stone_s[32], wood_s[32], ore_s[32], gold_s[32];
	int consumed = 0;
	if (sscanf(args, " %31s %31s %31s %31s %31s%n", food_s, stone_s, wood_s, ore_s, gold_s, &consumed) != 5) {
		BotReply(c, player_name, "Envoi",
			"Usage : %cadminrss <food> <stone> <wood> <ore> <gold> <pseudo>, ex. %cadminrss 0 0 0 0 5M Bob (0 = rien de cette ressource)",
			c->bot.command_prefix, c->bot.command_prefix);
		return;
	}

	char name[64];
	snprintf(name, sizeof(name), "%s", args + consumed);
	char *start = name;
	while (*start == ' ') start++;
	char *end = start + strlen(start);
	while (end > start && end[-1] == ' ')
		*--end = '\0';

	if (strlen(start) == 0 || strlen(start) >= 13) {
		BotReply(c, player_name, "Envoi", "Pseudo invalide (12 caractères au plus).");
		return;
	}

	const char *fields[5] = { food_s, stone_s, wood_s, ore_s, gold_s };
	uint32_t gross[5];
	for (int i = 0; i < 5; i++) {
		if (!ParseGrossAmount(c, fields[i], &gross[i])) {
			BotReply(c, player_name, "Envoi", "Montant invalide : %s.", RESOURCE_COMMANDS[i].label);
			return;
		}
	}

	TransferLine lines[TRANSFER_BATCH_MAX];
	uint8_t line_count = BuildPriorityLines(gross, lines);
	if (line_count == 0) {
		BotReply(c, player_name, "Envoi", "Rien à envoyer (tous les montants sont à 0).");
		return;
	}

	if (c->guildbank.enabled) {
		GuildBankLoad(c);
		if (!GuildMemberOrReply(c, start, player_name, false))
			return;
	}

	for (uint8_t i = 0; i < line_count; i++) {
		uint32_t available = StockAvailable(c, lines[i].type, false);
		if (lines[i].amount > available) {
			char have[32];
			FormatExact(available, have, sizeof(have));
			BotReply(c, player_name, "Ressources insuffisantes",
				"Ressources insuffisantes (%s) : %s disponible, sans toucher à la réserve ni aux dépôts des membres.",
				ResourceLabel(lines[i].type), have);
			return;
		}
	}

	TransferRequest request = { .line_count = line_count, .from_balance = false };
	memcpy(request.lines, lines, sizeof(lines));
	snprintf(request.requester, sizeof(request.requester), "%s", player_name);
	snprintf(request.target, sizeof(request.target), "%s", start);
	QueueDelivery(c, &request, "Envoi");
}

/* $rss <food> <stone> <wood> <ore> <gold>: like $<ressource>, but withdraws up to five resources
 * from the caller's own guild bank balance in one command. 0 skips a resource; they leave in
 * priority order (BuildPriorityLines), not the order they are typed in. Guild bank only - without
 * it there is no personal balance to withdraw from. */
static void RssCommand(Connection *c, const char *player_name, const char *args)
{
	if (!c->guildbank.enabled) {
		BotReply(c, player_name, "Indisponible", "Cette commande n'existe qu'avec la banque de guilde activée.");
		return;
	}

	GuildBankLoad(c);
	if (!GuildMemberOrReply(c, player_name, player_name, true))
		return;
	if (!DeliveryReady(c, player_name))
		return;

	char food_s[32], stone_s[32], wood_s[32], ore_s[32], gold_s[32];
	if (sscanf(args, " %31s %31s %31s %31s %31s", food_s, stone_s, wood_s, ore_s, gold_s) != 5) {
		BotReply(c, player_name, "Retrait",
			"Usage : %crss <food> <stone> <wood> <ore> <gold>, ex. %crss 0 0 0 0 5M (0 = rien de cette ressource)",
			c->bot.command_prefix, c->bot.command_prefix);
		return;
	}

	const char *fields[5] = { food_s, stone_s, wood_s, ore_s, gold_s };
	uint32_t gross[5];
	for (int i = 0; i < 5; i++) {
		if (!ParseGrossAmount(c, fields[i], &gross[i])) {
			BotReply(c, player_name, "Retrait", "Montant invalide : %s.", RESOURCE_COMMANDS[i].label);
			return;
		}
	}

	TransferLine lines[TRANSFER_BATCH_MAX];
	uint8_t line_count = BuildPriorityLines(gross, lines);
	if (line_count == 0) {
		BotReply(c, player_name, "Retrait", "Rien à retirer (tous les montants sont à 0).");
		return;
	}

	for (uint8_t i = 0; i < line_count; i++) {
		ResourceType type = lines[i].type;
		const char *label = ResourceLabel(type);
		uint64_t balance = GuildBankBalance(player_name, type);

		if (lines[i].amount > balance) {
			char have[32], have_net[32];
			FormatExact(balance, have, sizeof(have));
			FormatExact(NetOf(c, balance), have_net, sizeof(have_net));
			BotReply(c, player_name, "Solde insuffisant",
				"Solde de %s insuffisant : %s, soit %s reçus après la taxe de %.1f%%.",
				label, have, have_net, DeliveryTaxPercent(c));
			return;
		}

		uint32_t stock = StockAvailable(c, type, true);
		if (lines[i].amount > stock) {
			char in_stock[32];
			FormatExact(stock, in_stock, sizeof(in_stock));
			BotReply(c, player_name, "Indisponible",
				"La banque n'a actuellement que %s de %s en stock, moins que votre demande. Prévenez un administrateur.",
				in_stock, label);
			return;
		}
	}

	TransferRequest request = { .line_count = line_count, .from_balance = true };
	memcpy(request.lines, lines, sizeof(lines));
	snprintf(request.requester, sizeof(request.requester), "%s", player_name);
	snprintf(request.target, sizeof(request.target), "%s", player_name);
	QueueDelivery(c, &request, "Retrait");
}

static void ResourceCommandHandler(
    Connection *c,
    const char *player_name,
    const char *message,
    ResourceType type,
    const char *name
)
{
	// guild bank: everybody takes back their own balance
	if (c->guildbank.enabled) {
		GuildWithdrawCommand(c, player_name, message, type);
		return;
	}

	if (!BankAllows(c, player_name, type))
		return;

	if (!DeliveryReady(c, player_name))
		return;

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
	double tax = DeliveryTaxPercent(c);
	if (tax > 0.0 && tax < 100.0) {
		uint64_t gross = (uint64_t)((double)amount * 100.0 / (100.0 - tax) + 0.5);
		LOGD("[TRANSFER] Taxe %.2f%% : %llu demandé -> %llu envoyé\n",
			tax, (unsigned long long)amount, (unsigned long long)gross);
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


void ShowBankBalance(Connection *c, const char *player_name, CommandChannel channel) {
	if (!IsAdmin(c, player_name)) {
		// Return message if necessary
		BotReplyTo(c, player_name, "Non autorisé", channel, "Vous n'avez pas la permission de voir le solde de la banque.");
		return;
	}

	if (!c->items_loaded) {
		BotReplyTo(c, player_name, "Problème", channel, "Un problème est survenu, réessayez dans un instant.");
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
	
	BotReplyTo(c, player_name, "Solde de la banque", channel,
		"[BANQUE] Nourriture : %s | Pierre : %s | Bois : %s | Minerai : %s | Or : %s\n"
		"[SAC] Nourriture : %s | Pierre : %s | Bois : %s | Minerai : %s | Or : %s\n"
		"[TOTAL] Nourriture : %s | Pierre : %s | Bois : %s | Minerai : %s | Or : %s",
		bank_food, bank_rock, bank_wood, bank_ore, bank_gold,
		bag_food, bag_rock, bag_wood, bag_ore, bag_gold,
		sum_food, sum_rock, sum_wood, sum_ore, sum_gold
	);
	return;
}