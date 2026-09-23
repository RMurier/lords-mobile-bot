/*
 * Tests of the bot's command logic: permissions, administrators (and their persistence),
 * channels, replies, $stop, $help, delivery distance, choice of bag items, configuration
 * parsing and the chat buffer overflow fix.
 *
 * It links the real sources except main.c and connection.c: send_packet() is replaced so the
 * replies can be inspected, so nothing touches the network. Linux / macOS (uses mkdtemp).
 *
 * Build and run from the repository root:
 *
 *   gcc -std=gnu11 -Iinclude -o /tmp/bot_logic_test tests/bot_logic_test.c \
 *       src/log.c src/protocol.c src/des.c src/map_point.c src/command.c src/config.c
 *   /tmp/bot_logic_test
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>
#include <unistd.h>
#include "connection.h"
#include "command.h"
#include "config.h"
#include "protocol.h"
#include "map_point.h"
#include "items.h"
#include "packet_enum.h"

/* ---- test doubles ---------------------------------------------------- */

static uint8_t sent[64][4096];
static size_t  sent_size[64];
static int     sent_count;
static int     useitem_count;

bool send_packet(Connection *c, bool enc)
{
	(void)enc;
	if (sent_count < 64) {
		memcpy(sent[sent_count], c->data, c->size);
		sent_size[sent_count] = c->size;
	}
	if (c->size >= 4 && (uint16_t)(c->data[2] | (c->data[3] << 8)) == _MSG_REQUEST_USEITEM)
		useitem_count++;
	sent_count++;
	return true;
}
int  set_nonblocking(Connection *c) { (void)c; return 0; }
int  connect_server(const char *ip, unsigned short port) { (void)ip; (void)port; return -1; }
void disconnect(Connection *c) { (void)c; }

void dump_data(const char *filename, const char *str, void *data, size_t size) { (void)filename; (void)str; (void)data; (void)size; }
uint64_t GetBagFood(Connection *c);
uint64_t GetBagRock(Connection *c);
uint64_t GetBagWood(Connection *c);
uint64_t GetBagOre(Connection *c);
uint64_t GetBagGold(Connection *c);

static bool replied(const char *needle)
{
	size_t n = strlen(needle);
	for (int i = 0; i < sent_count && i < 64; i++)
		for (size_t j = 0; j + n <= sent_size[i]; j++)
			if (memcmp(sent[i] + j, needle, n) == 0)
				return true;
	return false;
}

static int find_packet(uint16_t type)
{
	for (int i = 0; i < sent_count && i < 64; i++)
		if (sent_size[i] >= 4 && (uint16_t)(sent[i][2] | (sent[i][3] << 8)) == type)
			return i;
	return -1;
}

static void reset_sent(void) { sent_count = 0; useitem_count = 0; }

static int failures = 0;
#define CHECK(cond, msg) do { if (cond) printf("PASS %s\n", msg); else { printf("FAIL %s\n", msg); failures++; } } while (0)

static Connection *fresh(const char *admins)
{
	Connection *c = calloc(1, sizeof(*c));
	c->sock = -1;
	c->bot.command_prefix = '$';
	c->bot.command_input_mask = (1u << COMMAND_CHANNEL_GUILD) | (1u << COMMAND_CHANNEL_MAIL);
	c->bot.command_output = COMMAND_CHANNEL_MAIL;
	snprintf(c->bot.data_path, sizeof(c->bot.data_path), "/nonexistent-readonly/");
	char copy[128];
	snprintf(copy, sizeof(copy), "%s", admins);
	for (char *t = strtok(copy, ","); t; t = strtok(NULL, ","))
		AdminAdd(c, t);
	c->bot.admin_config_count = c->bot.admin_count;
	c->resources.gold = 100000000;
	c->resources.food = 100000000;
	c->supply_capacity = 100000000; // Trading Post loaded: bank tests are not about its capacity
	c->player.max_marches = 6; // marches loaded: bank tests are not about march availability
	reset_sent();
	return c;
}

static void say(Connection *c, const char *who, const char *msg, CommandChannel ch) { command_handler(c, who, msg, ch); }

int main(void)
{
	Connection *c;

	/* ---- bank permissions with an administrator LIST ------------------ */
	c = fresh("boss,alice");
	say(c, "eve", "$gold 1M", COMMAND_CHANNEL_MAIL);
	CHECK(c->transfer.state == TRANSFER_IDLE, "bank off: stranger refused");
	say(c, "boss", "$gold 1M", COMMAND_CHANNEL_MAIL);
	CHECK(c->transfer.state == TRANSFER_FIND_TARGET, "first administrator can use the bank");
	memset(&c->transfer, 0, sizeof(c->transfer));
	say(c, "alice", "$gold 1M", COMMAND_CHANNEL_MAIL);
	CHECK(c->transfer.state == TRANSFER_FIND_TARGET, "second administrator can use the bank");
	free(c);

	c = fresh("boss");
	c->bank.enabled = true; c->bank.send_gold = true;
	say(c, "eve", "$food 1M", COMMAND_CHANNEL_MAIL);
	CHECK(c->transfer.state == TRANSFER_IDLE, "send_gold does not allow food");
	say(c, "eve", "$gold 1M", COMMAND_CHANNEL_MAIL);
	CHECK(c->transfer.state == TRANSFER_FIND_TARGET && c->transfer.amount == 1000000, "enabled + send_gold: allowed");
	free(c);

	/* ---- administrators: add / remove / list / su --------------------- */
	c = fresh("boss");
	say(c, "eve", "$admin add eve", COMMAND_CHANNEL_MAIL);
	CHECK(!IsAdmin(c, "eve"), "stranger cannot add himself");
	CHECK(replied("permission"), "stranger is told he has no permission");
	reset_sent();
	say(c, "boss", "$admin add eve", COMMAND_CHANNEL_MAIL);
	CHECK(IsAdmin(c, "eve"), "administrator can add an administrator");
	say(c, "eve", "$admin add mallory", COMMAND_CHANNEL_MAIL);
	CHECK(IsAdmin(c, "mallory"), "an added administrator can add others");
	say(c, "boss", "$admin remove mallory", COMMAND_CHANNEL_MAIL);
	CHECK(!IsAdmin(c, "mallory"), "administrator can remove an added administrator");
	reset_sent();
	say(c, "eve", "$admin remove boss", COMMAND_CHANNEL_MAIL);
	CHECK(IsAdmin(c, "boss") && replied("fichier de configuration"), "config administrators cannot be removed by commands");
	reset_sent();
	say(c, "boss", "$admin list", COMMAND_CHANNEL_MAIL);
	CHECK(replied("boss (config), eve"), "list shows every administrator and where it comes from");
	say(c, "boss", "$su alice", COMMAND_CHANNEL_MAIL);
	CHECK(IsAdmin(c, "alice"), "$su still works as 'admin add'");
	say(c, "zed", "$su zed", COMMAND_CHANNEL_MAIL);
	CHECK(!IsAdmin(c, "zed"), "stranger cannot use $su");
	CHECK(!AdminAdd(c, "waytoolongadminname"), "names longer than 12 characters are rejected");
	CHECK(!AdminAdd(c, ""), "empty names are rejected");
	for (int i = 0; i < 40; i++) { char n[16]; snprintf(n, sizeof(n), "p%d", i); AdminAdd(c, n); }
	CHECK(c->bot.admin_count == MAX_ADMINS, "the list is capped at MAX_ADMINS");
	free(c);

	c = fresh("");
	say(c, "eve", "$su eve", COMMAND_CHANNEL_MAIL);
	CHECK(c->bot.admin_count == 0, "no configured admin: nobody can claim the role");
	say(c, "eve", "$gold 1M", COMMAND_CHANNEL_MAIL);
	CHECK(c->transfer.state == TRANSFER_IDLE, "no configured admin: bank closed by default");
	free(c);

	/* ---- persistence of administrators added in game ------------------ */
	{
		char dir[] = "/tmp/lmbot-admins-XXXXXX";
		mkdtemp(dir);
		char data[300];
		snprintf(data, sizeof(data), "%s/deep/er/", dir);

		c = fresh("boss");
		snprintf(c->bot.data_path, sizeof(c->bot.data_path), "%s", data);
		say(c, "boss", "$admin add eve", COMMAND_CHANNEL_MAIL);
		say(c, "boss", "$admin add mallory", COMMAND_CHANNEL_MAIL);
		say(c, "boss", "$admin remove mallory", COMMAND_CHANNEL_MAIL);
		free(c);

		c = fresh("boss");
		snprintf(c->bot.data_path, sizeof(c->bot.data_path), "%s", data);
		AdminLoadRuntime(c);
		CHECK(IsAdmin(c, "eve") && !IsAdmin(c, "mallory") && IsAdmin(c, "boss"), "added administrators survive a restart (and removals too)");
		CHECK(c->bot.admin_count == 2, "config administrators are not duplicated in the file");
		free(c);
	}

	/* ---- command.input: channels ------------------------------------- */
	c = fresh("boss");
	c->bot.command_input_mask = 1u << COMMAND_CHANNEL_GUILD;
	say(c, "boss", "$gold 1M", COMMAND_CHANNEL_MAIL);
	CHECK(c->transfer.state == TRANSFER_IDLE, "input GUILD only: mail command ignored");
	say(c, "boss", "$gold 1M", COMMAND_CHANNEL_WORLD);
	CHECK(c->transfer.state == TRANSFER_IDLE, "input GUILD only: world chat command ignored");
	say(c, "boss", "$gold 1M", COMMAND_CHANNEL_GUILD);
	CHECK(c->transfer.state == TRANSFER_FIND_TARGET, "input GUILD only: alliance chat command accepted");
	free(c);
	c = fresh("boss");
	c->bot.command_input_mask = (1u << COMMAND_CHANNEL_WORLD) | (1u << COMMAND_CHANNEL_MAIL);
	say(c, "boss", "$gold 1M", COMMAND_CHANNEL_WORLD);
	CHECK(c->transfer.state == TRANSFER_FIND_TARGET, "input WORLD+MAIL: world accepted");
	free(c);

	/* ---- command.output: mail vs chat --------------------------------- */
	c = fresh("boss");
	say(c, "boss", "$stop", COMMAND_CHANNEL_MAIL);
	CHECK(sent_count == 1 && replied("Aucune livraison en cours"), "output MAIL: one reply");
	reset_sent();
	c->bot.command_output = COMMAND_CHANNEL_GUILD;
	say(c, "boss", "$stop", COMMAND_CHANNEL_MAIL);
	CHECK(sent_count == 1 && replied("@boss Aucune livraison en cours"), "output GUILD: replies in chat addressed to the player");
	free(c);

	/* ---- $stop ------------------------------------------------------- */
	c = fresh("boss");
	c->bank.enabled = true; c->bank.send_gold = true;
	say(c, "eve", "$gold 1M", COMMAND_CHANNEL_MAIL);
	say(c, "mallory", "$stop", COMMAND_CHANNEL_MAIL);
	CHECK(c->transfer.state == TRANSFER_FIND_TARGET, "$stop: someone else cannot cancel your transfer");
	say(c, "eve", "$stop", COMMAND_CHANNEL_MAIL);
	CHECK(c->transfer.state == TRANSFER_IDLE, "$stop: the requester can cancel");
	say(c, "eve", "$gold 1M", COMMAND_CHANNEL_MAIL);
	say(c, "boss", "$stop", COMMAND_CHANNEL_MAIL);
	CHECK(c->transfer.state == TRANSFER_IDLE, "$stop: an administrator can cancel any transfer");
	free(c);

	/* ---- $help ------------------------------------------------------- */
	c = fresh("boss");
	say(c, "eve", "$help", COMMAND_CHANNEL_MAIL);
	CHECK(replied("$help") && replied("$stop") && !replied("$admin") && !replied("<food"), "help for a stranger: no admin commands, no bank while it is closed");
	reset_sent();
	say(c, "boss", "$help", COMMAND_CHANNEL_MAIL);
	CHECK(replied("$admin list|add") && replied("$bank bal") && replied("food|stone|wood|ore|gold"), "help for an administrator lists everything");
	reset_sent();
	c->bank.enabled = true; c->bank.send_gold = true; c->bank.send_ore = true;
	say(c, "eve", "$help", COMMAND_CHANNEL_MAIL);
	CHECK(replied("<ore|gold>") && replied("$ore 5M"), "help lists only the resources the bank allows, with an example");
	free(c);

	/* ---- bag planning ------------------------------------------------- */
	{
		BagUse plan[BAG_PLAN_MAX];
		c = fresh("boss");
		c->items[FOOD_2M].quantity = 2; c->items[FOOD_500K].quantity = 3; c->items[FOOD_5K].quantity = 10;
		CHECK(BagTotal(c, RESOURCE_FOOD) == GetBagFood(c), "BagTotal matches GetBagFood");
		c->items[GOLD_6M].quantity = 1; c->items[GOLD_3K].quantity = 4;
		CHECK(BagTotal(c, RESOURCE_GOLD) == GetBagGold(c), "BagTotal matches GetBagGold");
		CHECK(BagPlan(c, RESOURCE_FOOD, 0, plan) == 0, "nothing needed -> nothing used");
		CHECK(BagPlan(c, RESOURCE_FOOD, 99000000, plan) == -1, "bag too small -> refuse, use nothing");
		int n = BagPlan(c, RESOURCE_FOOD, 4000000, plan);
		CHECK(n == 1 && plan[0].item_id == FOOD_2M && plan[0].quantity == 2, "4M = two 2M items, no waste");
		/* 2.1M with 2 x 2M, 3 x 500K, 10 x 5K: 2M + one 500K (waste 400K) beats 2M + ten 5K + one 500K (waste 450K) */
		n = BagPlan(c, RESOURCE_FOOD, 2100000, plan);
		uint64_t gained = 0;
		for (int i = 0; i < n; i++) gained += (uint64_t)plan[i].quantity * (plan[i].item_id == FOOD_2M ? 2000000 : plan[i].item_id == FOOD_500K ? 500000 : 5000);
		CHECK(gained == 2500000 && n == 2, "2.1M: 2M + one 500K, the 5K items are not wasted for nothing");
		n = BagPlan(c, RESOURCE_FOOD, 2001000, plan);
		gained = 0;
		for (int i = 0; i < n; i++) gained += (uint64_t)plan[i].quantity * (plan[i].item_id == FOOD_2M ? 2000000 : plan[i].item_id == FOOD_500K ? 500000 : 5000);
		CHECK(gained == 2005000 && n == 2, "2.001M: one 2M item plus one 5K item (waste 4K)");
		n = BagPlan(c, RESOURCE_FOOD, 1200000, plan);
		gained = 0;
		for (int i = 0; i < n; i++) gained += (uint64_t)plan[i].quantity * (plan[i].item_id == FOOD_2M ? 2000000 : plan[i].item_id == FOOD_500K ? 500000 : 5000);
		CHECK(gained == 1500000, "1.2M: three 500K would be 1.5M, the 2M item is 2M: the least waste wins");
		int items_before = c->items[FOOD_2M].quantity;
		n = BagPlan(c, RESOURCE_FOOD, 2500000, plan);
		CHECK(c->items[FOOD_2M].quantity == items_before, "planning does not touch the bag");
		free(c);
	}

	/* ---- bank with the bag -------------------------------------------- */
	c = fresh("boss");
	c->resources.gold = 1000000;
	c->items[GOLD_2M].quantity = 2;
	say(c, "boss", "$gold 3M", COMMAND_CHANNEL_MAIL);
	CHECK(c->transfer.state == TRANSFER_IDLE && replied("(or) : 1.00M disponible"), "bag not allowed: refused with what is available");
	free(c);
	c = fresh("boss");
	c->resources.gold = 1000000;
	c->items[GOLD_2M].quantity = 2;
	c->bank.use_bag_rss = true; c->bank.use_bag_gold = false;
	say(c, "boss", "$gold 3M", COMMAND_CHANNEL_MAIL);
	CHECK(c->transfer.state == TRANSFER_IDLE && useitem_count == 0, "use_bag_rss alone is not enough: the resource flag is needed too");
	free(c);
	c = fresh("boss");
	c->resources.gold = 1000000;
	c->items[GOLD_2M].quantity = 2;
	c->bank.use_bag_rss = true; c->bank.use_bag_gold = true;
	say(c, "boss", "$gold 3M", COMMAND_CHANNEL_MAIL);
	CHECK(c->transfer.state == TRANSFER_FIND_TARGET && useitem_count == 1 && c->items[GOLD_2M].quantity == 1,
		"bag allowed: the missing 2M is covered by one item");
	CHECK(c->transfer.not_before > now_ms(), "the transfer waits for the resources to be credited");
	free(c);
	c = fresh("boss");
	c->resources.gold = 1000000;
	c->items[GOLD_2M].quantity = 1;
	c->bank.use_bag_rss = true; c->bank.use_bag_gold = true;
	say(c, "boss", "$gold 9M", COMMAND_CHANNEL_MAIL);
	CHECK(c->transfer.state == TRANSFER_IDLE && useitem_count == 0 && replied("(or) : 3.00M disponible"), "bag too small: nothing is used, message counts the bag");
	free(c);

	/* ---- delivery distance ------------------------------------------- */
	{
		map_pos_t home = { 100, 100 }, near_pos = { 105, 104 }, far_pos = { 400, 100 };
		uint16_t hz, tz; uint8_t hp, tp;
		MapPosToPointCode(home, &hz, &hp);

		CHECK(MapDistance(home, near_pos) == 7 && MapDistance(home, far_pos) == 300, "distance in tiles (rounded up)");

		uint8_t packet[8];
		c = fresh("boss");
		c->player.zone_id = hz; c->player.point_id = hp;
		c->bank.max_delivery_distance = 100;
		snprintf(c->transfer.target_name, sizeof(c->transfer.target_name), "eve");

		MapPosToPointCode(far_pos, &tz, &tp);
		packet[0] = 0; packet[1] = tz & 0xff; packet[2] = tz >> 8; packet[3] = tp;
		c->transfer.state = TRANSFER_WAIT_TARGET;
		RecvAllyPoint(c, packet);
		CHECK(c->transfer.state == TRANSFER_FAILED && replied("300 cases"), "target beyond max_delivery_distance: refused with the distance");

		MapPosToPointCode(near_pos, &tz, &tp);
		packet[1] = tz & 0xff; packet[2] = tz >> 8; packet[3] = tp;
		c->transfer.state = TRANSFER_WAIT_TARGET;
		RecvAllyPoint(c, packet);
		CHECK(c->transfer.state == TRANSFER_SEND_MARCH, "target within the limit: delivery goes on");

		c->bank.max_delivery_distance = 0;
		MapPosToPointCode(far_pos, &tz, &tp);
		packet[1] = tz & 0xff; packet[2] = tz >> 8; packet[3] = tp;
		c->transfer.state = TRANSFER_WAIT_TARGET;
		RecvAllyPoint(c, packet);
		CHECK(c->transfer.state == TRANSFER_SEND_MARCH, "max_delivery_distance = 0 means no limit");
		free(c);
	}

	/* ---- chat channel and the buffer overflow fix ---------------------- */
	{
		enum { MSG = 3000 };
		static uint8_t packet[64 + MSG];
		size_t o = 0;
		c = fresh("boss");
		strcpy(c->mail.sender_name, "canary");
		c->app.version_major = 0; /* skips the extra byte */

		packet[o++] = 0;                               /* b2: always 0 in captures, not the channel */
		packet[o++] = 1; packet[o++] = 0;              /* one message */
		o += 24;                                       /* three u64 */
		packet[o++] = 1; packet[o++] = 0;              /* alli_or_king = alliance, num8 = plain text */
		o += 2;                                        /* pic id */
		memcpy(packet + o, "eve", 4); o += 13;         /* name */
		o += 1 + 3 + 1 + 1 + 1;                        /* vip, title, block id, title id, arabic */
		packet[o++] = MSG & 0xff; packet[o++] = MSG >> 8;
		memset(packet + o, 'A', MSG);
		RecvChatMessage(c, packet);
		CHECK(c->chat.channel == 1, "the chat channel is recorded (1 = alliance)");
		CHECK(strcmp(c->mail.sender_name, "canary") == 0, "a 3000 byte chat message no longer overwrites memory after the buffer");
		CHECK(strlen(c->chat.message) == sizeof(c->chat.message) - 1, "the message is truncated to the buffer");
		free(c);
	}

	/* ---- configuration ------------------------------------------------ */
	{
		char dir[] = "/tmp/lmbot-cfg-XXXXXX";
		mkdtemp(dir);
		char cfg[300];
		snprintf(cfg, sizeof(cfg), "%s/t.cfg", dir);

		FILE *fp = fopen(cfg, "w");
		fprintf(fp, "admin.name = legacy\nadmin.names = boss, alice ,bob\ncommand.input = WORLD, MAIL\ncommand.output = GUILD\n"
			"reconnect.kicked_delay = 300\nreconnect.delay = 45\ndata.path = %s/data/\nbank.use_bag_rss = true\n", dir);
		fclose(fp);

		c = calloc(1, sizeof(*c));
		bool ok = LoadConfig(c, cfg);
		CHECK(ok, "config with the new keys loads");
		CHECK(c->bot.admin_count == 4 && IsAdmin(c, "legacy") && IsAdmin(c, "alice") && IsAdmin(c, "bob"), "admin.name and admin.names are merged, spaces trimmed");
		CHECK(c->bot.command_input_mask == ((1u << COMMAND_CHANNEL_WORLD) | (1u << COMMAND_CHANNEL_MAIL)), "command.input accepts a list");
		CHECK(c->bot.command_output == COMMAND_CHANNEL_GUILD, "command.output");
		CHECK(c->reconnect.kicked_delay == 300 && c->reconnect.delay == 45, "kicked_delay is separate from delay");
		CHECK(c->bot.admin_config_count == 4, "administrators from the file are marked as config administrators");
		free(c);

		fp = fopen(cfg, "w"); fprintf(fp, "reconnect.delay = 30\n"); fclose(fp);
		c = calloc(1, sizeof(*c));
		LoadConfig(c, cfg);
		CHECK(c->reconnect.kicked_delay == 60 && c->bot.command_input_mask == ((1u << COMMAND_CHANNEL_GUILD) | (1u << COMMAND_CHANNEL_MAIL)) && c->bot.command_output == COMMAND_CHANNEL_MAIL,
			"defaults: kicked_delay 60, input GUILD+MAIL, output MAIL");
		free(c);

		fp = fopen(cfg, "w"); fprintf(fp, "command.input = GUILD, FAX\n"); fclose(fp);
		c = calloc(1, sizeof(*c));
		CHECK(!LoadConfig(c, cfg), "an unknown input channel is a configuration error");
		free(c);

		fp = fopen(cfg, "w"); fprintf(fp, "admin.names = thisnameiswaytoolong\n"); fclose(fp);
		c = calloc(1, sizeof(*c));
		CHECK(!LoadConfig(c, cfg), "an administrator name longer than 12 characters is a configuration error");
		free(c);

		fp = fopen(cfg, "w"); fprintf(fp, "migration.scrolls_needed = 4\n"); fclose(fp);
		c = calloc(1, sizeof(*c));
		CHECK(LoadConfig(c, cfg) && c->migration_scrolls_needed == 4, "migration.scrolls_needed is read");
		free(c);
		fp = fopen(cfg, "w"); fprintf(fp, "reconnect.delay = 30\n"); fclose(fp);
		c = calloc(1, sizeof(*c));
		LoadConfig(c, cfg);
		CHECK(c->migration_scrolls_needed == 1, "migration.scrolls_needed defaults to 1");
		free(c);
		fp = fopen(cfg, "w"); fprintf(fp, "migration.scrolls_needed = 0\n"); fclose(fp);
		c = calloc(1, sizeof(*c));
		CHECK(!LoadConfig(c, cfg), "migration.scrolls_needed = 0 is a configuration error");
		free(c);

		fp = fopen(cfg, "w"); fprintf(fp, "reconnect.kicked_delay = 0\n"); fclose(fp);
		c = calloc(1, sizeof(*c));
		LoadConfig(c, cfg);
		CHECK(c->reconnect.kicked_delay == 0, "kicked_delay = 0 is accepted (do not reconnect)");
		free(c);
	}


	/* ---- texts are French, command words stay English ------------------- */
	c = fresh("boss");
	c->bank.enabled = true; c->bank.send_gold = true;
	say(c, "eve", "$help", COMMAND_CHANNEL_MAIL);
	CHECK(replied("Commandes (préfixe $)") && replied("recevoir des ressources, ex. $gold 5M") && replied("annuler votre livraison en cours") && replied("cette liste"),
		"help for a player is entirely in French, with the English command words");
	reset_sent();
	say(c, "boss", "$help", COMMAND_CHANNEL_MAIL);
	CHECK(replied("$relocate random|<x> <y> - déplacer le château") && replied("$migrate <royaume> <x> <y> - migrer vers un autre royaume") && !replied("$confirm") && replied("gérer les administrateurs")
		&& replied("solde de la banque, du sac et total"), "help for an administrator lists the new commands in French");
	reset_sent();
	say(c, "boss", "$aide", COMMAND_CHANNEL_MAIL);
	CHECK(sent_count == 0, "French command names do not exist: only the texts are French");
	say(c, "eve", "$stop", COMMAND_CHANNEL_MAIL);
	CHECK(replied("Aucune livraison en cours."), "replies are in French");
	free(c);

	/* ---- relocation: administrators only, acts at once, writes back only on a problem ---- */
	{
		map_pos_t home = { 200, 200 };
		uint16_t hz; uint8_t hp;
		MapPosToPointCode(home, &hz, &hp);

		c = fresh("boss,alice");
		c->items_loaded = true;
		c->player.zone_id = hz; c->player.point_id = hp; c->player.current_kingdom_id = 42;

		say(c, "eve", "$relocate random", COMMAND_CHANNEL_MAIL);
		CHECK(useitem_count == 0 && replied("Seuls les administrateurs"), "relocation refused for a stranger");
		reset_sent();
		say(c, "boss", "$relocate random", COMMAND_CHANNEL_MAIL);
		CHECK(useitem_count == 0 && replied("aucun relocalisateur aléatoire"), "no random relocator in the bag: error message, nothing sent");

		reset_sent();
		c->items[RANDOM_RELOCATOR].quantity = 1;
		say(c, "boss", "$relocate random", COMMAND_CHANNEL_MAIL);
		CHECK(useitem_count == 1 && replied("Relocalisation aléatoire demandée"), "random relocation is sent at once, without confirmation");
		CHECK((uint16_t)(sent[0][8] | (sent[0][9] << 8)) == RANDOM_RELOCATOR, "the packet uses the random relocator item");

		/* the server answers: item, quantity, unknown, zone, point, kingdom */
		reset_sent();
		{
			map_pos_t landed = { 304, 410 };   /* the map works on even tiles */
			uint16_t lz; uint8_t lp;
			MapPosToPointCode(landed, &lz, &lp);
			uint8_t answer[16] = { 0, RANDOM_RELOCATOR & 0xff, RANDOM_RELOCATOR >> 8, 0, 0, 0, 0, (uint8_t)(lz & 0xff), (uint8_t)(lz >> 8), lp, 42, 0 };
			RecvUseItem(c, answer, sizeof(answer));
			CHECK(replied("Château déplacé : royaume 42, X:304 Y:410"), "the administrator is told where the castle landed");
			CHECK(c->player.zone_id == lz && c->player.point_id == lp, "the bot's own position is updated");
			reset_sent();
			RecvUseItem(c, answer, sizeof(answer));
			CHECK(sent_count == 0, "the result is reported once");
		}

		/* coordinates with the advanced relocator */
		reset_sent();
		say(c, "boss", "$relocate 100 100", COMMAND_CHANNEL_MAIL);
		CHECK(useitem_count == 0 && replied("aucun relocalisateur avancé"), "no advanced relocator: error message, nothing sent");
		c->items[ADVANCE_RELOCATOR].quantity = 2;
		reset_sent();
		say(c, "boss", "$relocate 99999 5", COMMAND_CHANNEL_MAIL);
		CHECK(useitem_count == 0 && replied("Coordonnées invalides"), "coordinates outside the map are refused");
		reset_sent();
		say(c, "boss", "$relocate abc", COMMAND_CHANNEL_MAIL);
		CHECK(useitem_count == 0 && replied("Usage : $relocate random | $relocate <x> <y>"), "wrong arguments show the usage");
		reset_sent();
		c->player.zone_id = hz; c->player.point_id = hp;
		say(c, "boss", "$relocate 200 200", COMMAND_CHANNEL_MAIL);
		CHECK(useitem_count == 0 && replied("déjà en X:200 Y:200"), "already there: refused");
		reset_sent();
		say(c, "boss", "$relocate 100 100", COMMAND_CHANNEL_MAIL);
		{
			map_pos_t target = { 100, 100 };
			uint16_t tz; uint8_t tp;
			MapPosToPointCode(target, &tz, &tp);
			int k = find_packet(_MSG_REQUEST_USEITEM);
			CHECK(k >= 0 && useitem_count == 1 && replied("X:100 Y:100 demandée (royaume 42)"), "coordinates relocation is sent at once");
			if (k >= 0) {
				const uint8_t *pk = sent[k];
				CHECK((uint16_t)(pk[8] | (pk[9] << 8)) == ADVANCE_RELOCATOR, "the advanced relocator is used");
				CHECK((uint16_t)(pk[12] | (pk[13] << 8)) == 42 && (uint16_t)(pk[14] | (pk[15] << 8)) == tz && pk[16] == tp,
					"the packet carries the current kingdom and the target zone and point");
			}
		}
		/* the server refuses */
		reset_sent();
		{
			uint8_t refused[2] = { 5, 0 };
			RecvUseItem(c, refused, sizeof(refused));
			CHECK(replied("échoué (code 5)"), "a refusal by the server is reported");
			reset_sent();
			c->relocation.report_until = time(NULL) - 1;
			snprintf(c->relocation.report_to, sizeof(c->relocation.report_to), "boss");
			RecvUseItem(c, refused, sizeof(refused));
			CHECK(sent_count == 0, "a late answer is not reported");
		}
		reset_sent();
		say(c, "boss", "$confirm", COMMAND_CHANNEL_MAIL);
		say(c, "boss", "$cancel", COMMAND_CHANNEL_MAIL);
		CHECK(sent_count == 0, "$confirm and $cancel do not exist any more");
		free(c);
	}

	/* ---- kingdom migration: acts at once, the packets match the ones of the official client ---- */
	{
		/* as sent by the real client for 796 / X:301 Y:491: kingdom 0x031c, zone 0x01e9, point 0xb6 */
		map_pos_t landing = { 301, 491 };
		uint16_t lz; uint8_t lp;
		MapPosToPointCode(landing, &lz, &lp);
		CHECK(lz == 489 && lp == 182, "X:301 Y:491 is zone 489 / point 182, like in the capture");

		c = fresh("boss");
		c->items_loaded = true;
		c->player.current_kingdom_id = 232;
		snprintf(c->auth.session, sizeof(c->auth.session), "SESSIONKEY-FOR-TEST");

		say(c, "eve", "$migrate 796 301 491", COMMAND_CHANNEL_MAIL);
		CHECK(find_packet(1011) < 0 && replied("Seuls les administrateurs"), "migration refused for a stranger");
		reset_sent();
		say(c, "boss", "$migrate 796", COMMAND_CHANNEL_MAIL);
		CHECK(find_packet(1011) < 0 && replied("Usage : $migrate <royaume> <x> <y>"), "missing coordinates show the usage");
		reset_sent();
		say(c, "boss", "$migrate 232 301 491", COMMAND_CHANNEL_MAIL);
		CHECK(find_packet(1011) < 0 && replied("déjà dans le royaume 232") && replied("$relocate"), "migrating to the current kingdom points to $relocate");
		reset_sent();
		say(c, "boss", "$migrate 796 99999 5", COMMAND_CHANNEL_MAIL);
		CHECK(find_packet(1011) < 0 && replied("Coordonnées invalides"), "coordinates outside the map are refused");
		reset_sent();
		say(c, "boss", "$migrate 0 301 491", COMMAND_CHANNEL_MAIL);
		CHECK(find_packet(1011) < 0 && replied("Numéro de royaume invalide"), "kingdom 0 is refused");

		/* no scroll: it goes out at once, with the warning the administrator asked for */
		reset_sent();
		say(c, "boss", "$migrate 796 301 491", COMMAND_CHANNEL_MAIL);
		{
			int k = find_packet(1011);
			CHECK(k >= 0 && c->migration.state == MIGRATION_WAIT_SERVER, "the migration starts at once, without confirmation: the kingdom is looked up");
			CHECK(replied("aucun vélin de migration dans le sac") && replied("royaume 796 en X:301 Y:491"), "no scroll in the bag: the administrator is told");
			if (k >= 0) {
				const uint8_t *pk = sent[k];
				CHECK((uint16_t)(pk[0] | (pk[1] << 8)) == 4 + 4 + 2 + 512, "kingdom lookup: same size as the real client (522 bytes)");
				CHECK((uint16_t)(pk[8] | (pk[9] << 8)) == 796 && memcmp(pk + 10, "SESSIONKEY-FOR-TEST", 19) == 0 && pk[10 + 19] == 0 && pk[10 + 511] == 0,
					"kingdom lookup: kingdom, then the session key padded with zeros");
			}
			CHECK(find_packet(3156) < 0, "the teleport is not sent before the server answered");
		}

		/* the real answer to that lookup: status 0, port 10796 (u32), ip */
		reset_sent();
		{
			uint8_t toc[21] = { 0x00, 0x2c, 0x2a, 0x00, 0x00, '2','0','4','.','1','4','1','.','1','7','7','.','1','3','5', 0 };
			RecvKingdomServer(c, toc, sizeof(toc));
		}
		{
			int k = find_packet(3156);
			CHECK(k >= 0 && c->migration.state == MIGRATION_WAIT_RESULT, "server found: the teleport request goes out");
			if (k >= 0) {
				const uint8_t *pk = sent[k];
				const uint8_t captured[5] = { 0x1c, 0x03, 0xe9, 0x01, 0xb6 };   /* payload of the real request */
				CHECK((uint16_t)(pk[0] | (pk[1] << 8)) == 4 + 4 + 5 && memcmp(pk + 8, captured, 5) == 0,
					"the teleport packet is byte for byte the one of the official client (1c03 e901 b6)");
			}
		}

		/* accepted */
		reset_sent();
		{ uint8_t ok = 0; RecvFreeCrossTeleport(c, &ok); }
		CHECK(c->migration.state == MIGRATION_IDLE && replied("Migration acceptée : le château part vers le royaume 796 en X:301 Y:491"), "an accepted migration is reported");

		/* enough scrolls in the bag: a scroll is used at once, before even trying the free offer
		   (it is rarely available, not worth trying first when a scroll can just be used) */
		c->items[MIGRATION_SCROLL].quantity = 2;
		reset_sent();
		say(c, "boss", "$migrate 796 301 491", COMMAND_CHANNEL_MAIL);
		CHECK(!replied("aucun vélin") && replied("Vélins de migration dans le sac : 2 (1 nécessaire(s))"), "enough scrolls: the count is shown, no warning");
		reset_sent();
		{ uint8_t toc[21] = { 0, 0x2c, 0x2a, 0, 0, '1', 0 }; RecvKingdomServer(c, toc, sizeof(toc)); }
		{
			int k = find_packet(_MSG_REQUEST_USEITEM);
			CHECK(k >= 0 && useitem_count == 1 && find_packet(3156) < 0 && c->migration.state == MIGRATION_WAIT_SCROLL_RESULT,
				"enough scrolls: a migration scroll is used at once, the free offer is not even tried");
			if (k >= 0) {
				const uint8_t *pk = sent[k];
				CHECK((uint16_t)(pk[8] | (pk[9] << 8)) == MIGRATION_SCROLL, "the migration scroll is used");
				CHECK((uint16_t)(pk[12] | (pk[13] << 8)) == 796, "the packet carries the target kingdom");
			}
		}
		reset_sent();
		{
			uint8_t ok[12] = { 0, 0xfb, 0x04, 2, 0, 0, 0, 0x0d, 0x00, 0x3a, 0x2c, 0x01 };
			RecvUseItem(c, ok, sizeof(ok));
		}
		CHECK(c->migration.state == MIGRATION_IDLE &&
			replied("Migration acceptée (vélin de migration utilisé)") && replied("royaume 796 en X:301 Y:491"),
			"the migration completes once the server confirms the scroll was used");

		/* the server refuses the scroll itself: reported as such, not as a relocation failure */
		say(c, "boss", "$migrate 796 301 491", COMMAND_CHANNEL_MAIL);
		reset_sent();
		{ uint8_t toc[21] = { 0, 0x2c, 0x2a, 0, 0, '1', 0 }; RecvKingdomServer(c, toc, sizeof(toc)); }
		reset_sent();
		{ uint8_t refused[1] = { 5 }; RecvUseItem(c, refused, sizeof(refused)); }
		CHECK(c->migration.state == MIGRATION_IDLE && replied("Migration par vélin refusée par le serveur (code 5)"),
			"a scroll refused by the server is reported, not mistaken for a relocation failure");

		/* no scroll left: the free offer is tried, and its refusal is reported as such */
		c->items[MIGRATION_SCROLL].quantity = 0;
		say(c, "boss", "$migrate 796 301 491", COMMAND_CHANNEL_MAIL);
		reset_sent();
		{ uint8_t toc[21] = { 0, 0x2c, 0x2a, 0, 0, '1', 0 }; RecvKingdomServer(c, toc, sizeof(toc)); }
		CHECK(find_packet(3156) >= 0 && c->migration.state == MIGRATION_WAIT_RESULT, "no scroll: the free offer is tried instead");
		reset_sent();
		{ uint8_t refused = 7; RecvFreeCrossTeleport(c, &refused); }
		CHECK(replied("vous n'avez plus de vélin de migration") && replied("code 7"), "free migration unavailable, no scroll left: the bot says there is none");

		/* a big account needs several scrolls: having some is not enough, the free offer is tried */
		c->migration_scrolls_needed = 3;
		c->items[MIGRATION_SCROLL].quantity = 1;
		reset_sent();
		say(c, "boss", "$migrate 796 301 491", COMMAND_CHANNEL_MAIL);
		CHECK(replied("Il faut 3 vélin(s) de migration et le sac n'en contient que 1 (il en manque 2)"), "the command warns when the bag holds some scrolls but not enough");
		reset_sent();
		{ uint8_t toc[21] = { 0, 0x2c, 0x2a, 0, 0, '1', 0 }; RecvKingdomServer(c, toc, sizeof(toc)); }
		CHECK(find_packet(3156) >= 0, "not enough scrolls: the free offer is tried instead");
		reset_sent();
		{ uint8_t refused = 7; RecvFreeCrossTeleport(c, &refused); }
		CHECK(replied("Il faut 3 vélin(s) de migration et vous n'en avez que 1 : il vous en manque 2"), "free migration unavailable, not enough scrolls: it says how many are missing");
		c->items[MIGRATION_SCROLL].quantity = 3;
		reset_sent();
		say(c, "boss", "$migrate 796 301 491", COMMAND_CHANNEL_MAIL);
		CHECK(replied("Vélins de migration dans le sac : 3 (3 nécessaire(s))"), "exactly enough scrolls: no warning");
		reset_sent();
		{ uint8_t toc[21] = { 0, 0x2c, 0x2a, 0, 0, '1', 0 }; RecvKingdomServer(c, toc, sizeof(toc)); }
		CHECK(find_packet(_MSG_REQUEST_USEITEM) >= 0 && c->migration.state == MIGRATION_WAIT_SCROLL_RESULT, "exactly enough scrolls: used at once");
		{
			uint8_t ok[12] = { 0, 0xfb, 0x04, 0, 0, 0, 0, 0x0d, 0x00, 0x3a, 0x2c, 0x01 };
			RecvUseItem(c, ok, sizeof(ok));
		}
		c->migration_scrolls_needed = 1;
		c->items[MIGRATION_SCROLL].quantity = 0; /* back to no scroll, so the following refusals go through the free offer */

		/* precise refusals come with their reason (values read from the client's own enumeration) */
		say(c, "boss", "$migrate 796 301 491", COMMAND_CHANNEL_MAIL);
		reset_sent();
		{ uint8_t toc[21] = { 0, 0x2c, 0x2a, 0, 0, '1', 0 }; RecvKingdomServer(c, toc, sizeof(toc)); }
		reset_sent();
		{ uint8_t refused = 6; RecvFreeCrossTeleport(c, &refused); }
		CHECK(replied("le royaume de destination est plein (KINGDOM_FULL)") && !replied("vélin"), "a full kingdom is explained, scrolls are not mentioned");
		say(c, "boss", "$migrate 796 301 491", COMMAND_CHANNEL_MAIL);
		reset_sent();
		{ uint8_t toc[21] = { 0, 0x2c, 0x2a, 0, 0, '1', 0 }; RecvKingdomServer(c, toc, sizeof(toc)); }
		reset_sent();
		{ uint8_t refused = 0xFB; RecvFreeCrossTeleport(c, &refused); }
		CHECK(replied("des troupes sont hors du château") && replied("TROOP_OUTSIDE"), "signed codes are decoded: 0xFB is -5, troops outside");

		/* the three success variants are all successes */
		say(c, "boss", "$migrate 796 301 491", COMMAND_CHANNEL_MAIL);
		reset_sent();
		{ uint8_t toc[21] = { 0, 0x2c, 0x2a, 0, 0, '1', 0 }; RecvKingdomServer(c, toc, sizeof(toc)); }
		reset_sent();
		{ uint8_t forest = 0xFF; RecvFreeCrossTeleport(c, &forest); }
		CHECK(replied("Migration acceptée") && replied("réussite en forêt"), "code -1 (SUCCESS_IN_FOREST) is a success");
		say(c, "boss", "$migrate 796 301 491", COMMAND_CHANNEL_MAIL);
		reset_sent();
		{ uint8_t toc[21] = { 0, 0x2c, 0x2a, 0, 0, '1', 0 }; RecvKingdomServer(c, toc, sizeof(toc)); }
		reset_sent();
		{ uint8_t expire = 1; RecvFreeCrossTeleport(c, &expire); }
		CHECK(replied("Migration acceptée") && replied("arrive à expiration"), "code 1 (SUCCESS_EXPIRE) is a success");

		/* unknown or closed kingdom */
		say(c, "boss", "$migrate 9999 301 491", COMMAND_CHANNEL_MAIL);
		reset_sent();
		{ uint8_t missing[1] = { 1 }; RecvKingdomServer(c, missing, sizeof(missing)); }
		CHECK(c->migration.state == MIGRATION_IDLE && replied("royaume 9999 est introuvable ou fermé") && find_packet(3156) < 0, "an unknown kingdom is reported and nothing is teleported");

		/* only one at a time, and a silent server is given up on */
		say(c, "boss", "$migrate 796 301 491", COMMAND_CHANNEL_MAIL);
		reset_sent();
		say(c, "boss", "$migrate 800 301 491", COMMAND_CHANNEL_MAIL);
		CHECK(find_packet(1011) < 0 && replied("Une migration est déjà en cours"), "a second migration is refused while one runs");
		reset_sent();
		MigrationTick(c);
		CHECK(c->migration.state == MIGRATION_WAIT_SERVER && sent_count == 0, "no answer yet: keep waiting");
		c->migration.deadline = time(NULL) - 1;
		MigrationTick(c);
		CHECK(c->migration.state == MIGRATION_IDLE && replied("Pas de réponse du serveur pour le royaume 796"), "a silent server is given up on with a message");
		free(c);
	}

	printf("%s\n", failures ? "SOME TESTS FAILED" : "ALL BOT LOGIC TESTS PASSED");
	return failures ? 1 : 0;
}
