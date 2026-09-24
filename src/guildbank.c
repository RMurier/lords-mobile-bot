#include "guildbank.h"
#include "log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define NAME_SIZE 13   // 12 characters and the end of string, like the game's names

typedef struct {
	char     name[NAME_SIZE];
	uint64_t amount[GUILDBANK_RESOURCE_COUNT];
} Account;

/* Global on purpose: Connection is wiped at every reconnection, the balances must not be. */
static Account  g_accounts[GUILDBANK_MAX_ACCOUNTS];
static uint32_t g_account_count;
static uint32_t g_seen[GUILDBANK_MAX_SEEN];     // report numbers already counted, oldest first
static uint32_t g_seen_count;
static uint32_t g_activated;                    // reports dated before this do not count
static char     g_loaded_path[320];             // the file the memory comes from, "" = nothing loaded

static void BuildPath(const Connection *c, char *out, size_t size)
{
	size_t length = strlen(c->bot.data_path);
	snprintf(out, size, "%s%sguild_bank.txt", c->bot.data_path,
		(length > 0 && (c->bot.data_path[length - 1] == '/' || c->bot.data_path[length - 1] == '\\')) ? "" : "/");
}

/* A name that could break the file (tab, line break...) is not a name of the game. */
static bool ValidName(const char *name)
{
	if (name[0] == '\0' || strlen(name) >= NAME_SIZE)
		return false;
	for (const char *p = name; *p; p++)
		if ((unsigned char)*p < 0x20)
			return false;
	return true;
}

static bool Save(const Connection *c)
{
	char path[320], temp[330];
	BuildPath(c, path, sizeof(path));
	snprintf(temp, sizeof(temp), "%s.tmp", path);

	FILE *f = fopen(temp, "w");
	if (!f) {
		ensure_directory(c->bot.data_path);
		f = fopen(temp, "w");
	}
	if (!f) {
		LOGE("[BANK] Impossible d'écrire %s : les soldes ne sont pas enregistrés !\n", temp);
		return false;
	}

	fprintf(f, "lmbot-guild-bank 1\nactivated %u\n", g_activated);
	for (uint32_t i = 0; i < g_seen_count; i++)
		fprintf(f, "seen %u\n", g_seen[i]);
	for (uint32_t i = 0; i < g_account_count; i++) {
		const Account *a = &g_accounts[i];
		fprintf(f, "acct\t%s", a->name);
		for (int r = 0; r < GUILDBANK_RESOURCE_COUNT; r++)
			fprintf(f, "\t%llu", (unsigned long long)a->amount[r]);
		fputc('\n', f);
	}
	bool ok = fclose(f) == 0;

	// write the whole file aside, then swap it in: a crash never leaves half a bank
#ifdef _WIN32
	remove(path);   // rename() does not replace an existing file on Windows
#endif
	if (!ok || rename(temp, path) != 0) {
		LOGE("[BANK] Impossible d'enregistrer %s : les soldes ne sont pas enregistrés !\n", path);
		return false;
	}
	return true;
}

void GuildBankReset(void)
{
	memset(g_accounts, 0, sizeof(g_accounts));
	memset(g_seen, 0, sizeof(g_seen));
	g_account_count = 0;
	g_seen_count = 0;
	g_activated = 0;
	g_loaded_path[0] = '\0';
}

bool GuildBankLoad(Connection *c)
{
	char path[320];
	BuildPath(c, path, sizeof(path));

	if (strcmp(g_loaded_path, path) == 0)
		return true;

	GuildBankReset();
	snprintf(g_loaded_path, sizeof(g_loaded_path), "%s", path);

	FILE *f = fopen(path, "r");
	if (!f) {
		// first use: from today on, deliveries count
		g_activated = c->server_time ? (uint32_t)c->server_time : (uint32_t)time(NULL);
		LOGI("[BANK] Banque de guilde créée (%s) : seuls les dépôts postérieurs comptent\n", path);
		return Save(c);
	}

	char line[256];
	while (fgets(line, sizeof(line), f)) {
		line[strcspn(line, "\r\n")] = '\0';
		unsigned value;
		if (sscanf(line, "activated %u", &value) == 1) {
			g_activated = value;
		} else if (sscanf(line, "seen %u", &value) == 1) {
			if (g_seen_count < GUILDBANK_MAX_SEEN)
				g_seen[g_seen_count++] = value;
		} else if (strncmp(line, "acct\t", 5) == 0 && g_account_count < GUILDBANK_MAX_ACCOUNTS) {
			Account *a = &g_accounts[g_account_count];
			char *name = line + 5;
			char *tab = strchr(name, '\t');
			if (!tab)
				continue;
			*tab = '\0';
			if (!ValidName(name))
				continue;
			snprintf(a->name, sizeof(a->name), "%s", name);
			char *rest = tab + 1;
			for (int r = 0; r < GUILDBANK_RESOURCE_COUNT; r++) {
				a->amount[r] = strtoull(rest, &rest, 10);
				if (*rest == '\t')
					rest++;
			}
			g_account_count++;
		}
	}
	fclose(f);

	LOGI("[BANK] Banque de guilde chargée : %u joueur(s)\n", g_account_count);
	return true;
}

static Account *Find(const char *name)
{
	for (uint32_t i = 0; i < g_account_count; i++)
		if (strcmp(g_accounts[i].name, name) == 0)
			return &g_accounts[i];
	return NULL;
}

/* Table full: drop the accounts that hold nothing. */
static void Compact(void)
{
	uint32_t kept = 0;
	for (uint32_t i = 0; i < g_account_count; i++) {
		bool empty = true;
		for (int r = 0; r < GUILDBANK_RESOURCE_COUNT; r++)
			if (g_accounts[i].amount[r] > 0)
				empty = false;
		if (!empty)
			g_accounts[kept++] = g_accounts[i];
	}
	g_account_count = kept;
}

uint64_t GuildBankBalance(const char *name, ResourceType type)
{
	const Account *a = Find(name);
	return a && (int)type >= 0 && (int)type < GUILDBANK_RESOURCE_COUNT ? a->amount[type] : 0;
}

uint64_t GuildBankTotal(ResourceType type)
{
	uint64_t sum = 0;
	if ((int)type < 0 || (int)type >= GUILDBANK_RESOURCE_COUNT)
		return 0;
	for (uint32_t i = 0; i < g_account_count; i++)
		sum += g_accounts[i].amount[type];
	return sum;
}

uint32_t GuildBankAccountCount(void)
{
	return g_account_count;
}

bool GuildBankCredit(Connection *c, const char *name, ResourceType type, uint64_t amount)
{
	if (!ValidName(name) || (int)type < 0 || (int)type >= GUILDBANK_RESOURCE_COUNT)
		return false;
	GuildBankLoad(c);

	Account *a = Find(name);
	if (!a) {
		if (g_account_count >= GUILDBANK_MAX_ACCOUNTS)
			Compact();
		if (g_account_count >= GUILDBANK_MAX_ACCOUNTS) {
			LOGE("[BANK] Plus de place pour un nouveau joueur (%u), crédit de %s refusé\n", GUILDBANK_MAX_ACCOUNTS, name);
			return false;
		}
		a = &g_accounts[g_account_count++];
		memset(a, 0, sizeof(*a));
		snprintf(a->name, sizeof(a->name), "%s", name);
	}

	a->amount[type] += amount;
	return Save(c);
}

bool GuildBankDebit(Connection *c, const char *name, ResourceType type, uint64_t amount)
{
	if ((int)type < 0 || (int)type >= GUILDBANK_RESOURCE_COUNT)
		return false;
	GuildBankLoad(c);

	Account *a = Find(name);
	if (!a || a->amount[type] < amount)
		return false;

	a->amount[type] -= amount;
	Save(c);
	return true;
}

bool GuildBankDeposit(Connection *c, uint32_t report_id, uint32_t report_time, const char *sender,
                      const uint32_t net[GUILDBANK_RESOURCE_COUNT])
{
	if (!ValidName(sender))
		return false;
	GuildBankLoad(c);

	for (uint32_t i = 0; i < g_seen_count; i++)
		if (g_seen[i] == report_id)
			return false;   // already counted: the game sends its reports again at every login

	// remember it, dropping the oldest when the list is full
	if (g_seen_count >= GUILDBANK_MAX_SEEN) {
		memmove(g_seen, g_seen + 1, (GUILDBANK_MAX_SEEN - 1) * sizeof(g_seen[0]));
		g_seen_count = GUILDBANK_MAX_SEEN - 1;
	}
	g_seen[g_seen_count++] = report_id;

	if (report_time < g_activated) {
		Save(c);
		return false;   // older than the bank: not a deposit
	}

	bool credited = false;
	for (int r = 0; r < GUILDBANK_RESOURCE_COUNT; r++) {
		if (net[r] == 0)
			continue;
		Account *a = Find(sender);
		if (!a) {
			if (g_account_count >= GUILDBANK_MAX_ACCOUNTS)
				Compact();
			if (g_account_count >= GUILDBANK_MAX_ACCOUNTS) {
				LOGE("[BANK] Plus de place pour un nouveau joueur, dépôt de %s non crédité !\n", sender);
				break;
			}
			a = &g_accounts[g_account_count++];
			memset(a, 0, sizeof(*a));
			snprintf(a->name, sizeof(a->name), "%s", sender);
		}
		a->amount[r] += net[r];
		credited = true;
	}

	Save(c);
	if (credited)
		LOGI("[BANK] Dépôt de %s : %u / %u / %u / %u / %u (nourriture, pierre, bois, minerai, or), rapport %u\n",
			sender, net[0], net[1], net[2], net[3], net[4], report_id);
	return credited;
}

bool GuildBankSet(Connection *c, const char *name, ResourceType type, uint64_t amount)
{
	if (!ValidName(name) || (int)type < 0 || (int)type >= GUILDBANK_RESOURCE_COUNT)
		return false;
	GuildBankLoad(c);

	Account *a = Find(name);
	if (!a) {
		if (amount == 0)
			return true;   // nothing to keep
		if (g_account_count >= GUILDBANK_MAX_ACCOUNTS)
			Compact();
		if (g_account_count >= GUILDBANK_MAX_ACCOUNTS)
			return false;
		a = &g_accounts[g_account_count++];
		memset(a, 0, sizeof(*a));
		snprintf(a->name, sizeof(a->name), "%s", name);
	}

	a->amount[type] = amount;
	return Save(c);
}

bool GuildBankResetAll(Connection *c)
{
	GuildBankLoad(c);
	memset(g_accounts, 0, sizeof(g_accounts));
	g_account_count = 0;
	// deliveries the game may send again whose numbers are no longer remembered must not come back to life
	g_activated = c->server_time ? (uint32_t)c->server_time : (uint32_t)time(NULL);
	return Save(c);
}

static void EditsPath(const Connection *c, const char *suffix, char *out, size_t size)
{
	size_t length = strlen(c->bot.data_path);
	snprintf(out, size, "%s%sguild_bank_edits.%s", c->bot.data_path,
		(length > 0 && (c->bot.data_path[length - 1] == '/' || c->bot.data_path[length - 1] == '\\')) ? "" : "/", suffix);
}

/* Reads a file of requests and applies each line; returns how many were understood. */
static uint32_t ApplyEditsFile(Connection *c, const char *path)
{
	FILE *f = fopen(path, "r");
	if (!f)
		return 0;

	uint32_t applied = 0;
	char line[256];
	while (fgets(line, sizeof(line), f)) {
		line[strcspn(line, "\r\n")] = '\0';

		if (strcmp(line, "reset") == 0) {
			if (GuildBankResetAll(c)) {
				LOGI("[BANK] Tous les soldes remis à zéro depuis la console\n");
				applied++;
			}
		} else if (strncmp(line, "set\t", 4) == 0) {
			char *name = line + 4;
			char *tab = strchr(name, '\t');
			if (!tab)
				continue;
			*tab++ = '\0';
			char *end = NULL;
			unsigned long index = strtoul(tab, &end, 10);
			if (!end || *end != '\t' || index >= GUILDBANK_RESOURCE_COUNT)
				continue;
			unsigned long long amount = strtoull(end + 1, &end, 10);
			if (GuildBankSet(c, name, (ResourceType)index, amount)) {
				LOGI("[BANK] Solde de %s modifié depuis la console : ressource %lu = %llu\n", name, index, amount);
				applied++;
			}
		}
	}
	fclose(f);
	return applied;
}

/* The console never writes to guild_bank.txt while the bot runs: it queues requests, and the bot moves the queue
 * aside (so the console can start a new one at once) before reading it. */
uint32_t GuildBankApplyEdits(Connection *c)
{
	char queue[330], working[330];
	EditsPath(c, "txt", queue, sizeof(queue));
	EditsPath(c, "applying", working, sizeof(working));

	uint32_t applied = 0;

	// left over by a stop in the middle of a batch: the requests are idempotent, apply them again
	applied += ApplyEditsFile(c, working);
	remove(working);

	if (rename(queue, working) == 0) {
		applied += ApplyEditsFile(c, working);
		remove(working);
	}
	return applied;
}

void GuildBankTick(Connection *c)
{
	static time_t last;
	time_t now = time(NULL);

	if (now == last)
		return;
	last = now;
	GuildBankApplyEdits(c);
}
