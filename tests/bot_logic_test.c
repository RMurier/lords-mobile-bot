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
 *       src/log.c src/protocol.c src/des.c src/map_point.c src/command.c src/config.c src/guildbank.c
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
#include "guildbank.h"

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
	c->resource_loaded = true; // resources loaded: bank tests are not about the post-reconnect gap
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

	/* ---- $bank bal [chat|mail]: overrides command.output for one answer ---- */
	c = fresh("boss"); // command_output defaults to MAIL
	say(c, "boss", "$bank bal", COMMAND_CHANNEL_MAIL);
	CHECK(sent_count == 1 && !replied("@boss"), "bank bal with no argument follows command.output (mail)");
	reset_sent();
	say(c, "boss", "$bank bal chat", COMMAND_CHANNEL_MAIL);
	CHECK(sent_count == 1 && replied("@boss"), "bank bal chat overrides command.output for this one answer");
	free(c);
	c = fresh("boss");
	c->bot.command_output = COMMAND_CHANNEL_GUILD;
	say(c, "boss", "$bank bal mail", COMMAND_CHANNEL_MAIL);
	CHECK(sent_count == 1 && !replied("@boss"), "bank bal mail overrides command.output (guild) the other way");
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

	/* ---- multi-march pacing: the 2nd+ march must wait too, not just the 1st ---------- */
	{
		uint8_t shelp[72] = {0};
		uint8_t home[21] = {0};

		c = fresh("boss");
		snprintf(c->transfer.target_name, sizeof(c->transfer.target_name), "eve");
		c->transfer.zone_id = 489; c->transfer.point_id = 182;
		c->transfer.remaining = 1000000;
		c->transfer.state = TRANSFER_WAIT_MARCH;
		c->player.current_marches = 0;

		reset_sent();
		/* shelp[0] = b = 0 (accepted), shelp[1] = b2 = 0 (marches count), rest unused by this test */
		RecvSHelp(c, shelp);
		CHECK(c->transfer.state == TRANSFER_SEND_MARCH && c->transfer.not_before > now_ms()
			&& find_packet(_MSG_REQUEST_MAP_ADVANCE) >= 0,
			"a march accepted (RecvSHelp) refreshes the target and waits before the next one");

		c->transfer.state = TRANSFER_WAIT_MARCH;
		c->transfer.not_before = 0;
		c->player.current_marches = 1;
		c->transfer.remaining = 500000;

		reset_sent();
		/* home[0] = b = 0 (success), then food/rock/wood/ore/gold stocks (4 bytes each, unused here) */
		RecvHelp_Home(c, home);
		CHECK(c->transfer.state == TRANSFER_SEND_MARCH && c->transfer.not_before > now_ms()
			&& find_packet(_MSG_REQUEST_MAP_ADVANCE) >= 0,
			"a march returning home (RecvHelp_Home) also refreshes the target and waits");

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

		fp = fopen(cfg, "w"); fprintf(fp, "reconnect.delay = 30\n"); fclose(fp);
		c = calloc(1, sizeof(*c));
		LoadConfig(c, cfg);
		CHECK(c->recall.pause_seconds == 300, "recall.pause_seconds defaults to 300 (5 minutes)");
		free(c);
		fp = fopen(cfg, "w"); fprintf(fp, "recall.pause_seconds = 90\n"); fclose(fp);
		c = calloc(1, sizeof(*c));
		CHECK(LoadConfig(c, cfg) && c->recall.pause_seconds == 90, "recall.pause_seconds is read");
		free(c);
		fp = fopen(cfg, "w"); fprintf(fp, "recall.pause_seconds = 0\n"); fclose(fp);
		c = calloc(1, sizeof(*c));
		CHECK(LoadConfig(c, cfg) && c->recall.pause_seconds == 0, "recall.pause_seconds = 0 turns the pause off");
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

	/* ---- notify: "about to expire, nothing to renew with" fires once, resets later --- */
	{
		c = fresh("boss");
		c->protection.enabled = true;
		c->protection.shield_always_on = true;
		c->shield_info.loaded = true;
		c->shield_info.active = false; // no shield at all: immediately "expiring"
		c->notify.on_shield_expiring = true;
		// c->protection.shield_priority_count stays 0: HasAnyShieldItem() has nothing to check -> false
		ShieldTick(c);
		CHECK(c->shield_info.expiring_notified, "shield expiring with nothing to renew with: notified once");
		ShieldTick(c);
		CHECK(c->shield_info.expiring_notified, "still notified: does not un-notify itself while still expiring");
		c->shield_info.active = true;
		c->shield_info.begin_time = c->server_time;
		c->shield_info.duration = 3600; // plenty of time left
		ShieldTick(c);
		CHECK(!c->shield_info.expiring_notified, "a shield with time to spare re-arms the notification for next time");
		free(c);

		c = fresh("boss");
		c->protection.enabled = true;
		c->protection.antiscout_always_on = true;
		c->antiscout_info.loaded = true;
		c->antiscout_info.active = false;
		c->notify.on_antiscout_expiring = true;
		AntiScoutTick(c);
		CHECK(c->antiscout_info.expiring_notified, "anti-scout expiring with nothing to renew with: notified once");
		free(c);
	}

	/* ---- notify: _MSG_RESP_ANTISCOUTREPORTINFO (3420), real 55-byte payloads from a live
	 * capture - one report where anti-scout blocked the attempt (byte 40 = 0x0c), one where
	 * it did not (byte 40 = 0x00). Only checked for not crashing / not over-reading: the
	 * packet carries no state the bot keeps, so there is nothing else to assert on. */
	{
		c = fresh("boss");
		uint8_t blocked[] = {
			0xc0, 0xed, 0x00, 0x00, 0x00, 0x44, 0x1c, 0x65, 0x6a, 0x00, 0x00, 0x00, 0x00, 0xe8, 0x00,
			0xf0, 0x02, 0xa9, 0x08, 0x00, 0x19, 0xe8, 0x00, 0x54, 0x48, 0x20, 0x46, 0x61, 0x6b, 0x65,
			0x72, 0x00, 0x00, 0x00, 0x00, 0x00, 0x54, 0x5c, 0x48, 0x51, 0x0c, 0x00, 0x00, 0x00, 0x00,
			0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
		};
		uint8_t not_blocked[] = {
			0xc1, 0xed, 0x00, 0x00, 0x00, 0xb4, 0x61, 0x65, 0x6a, 0x00, 0x00, 0x00, 0x00, 0xe8, 0x00,
			0xf0, 0x02, 0xa9, 0x08, 0x00, 0x19, 0xfd, 0x00, 0x6f, 0x6f, 0x4e, 0x6f, 0x6b, 0x5a, 0x61,
			0x6f, 0x6f, 0x00, 0x00, 0x00, 0x00, 0x4b, 0x50, 0x59, 0x3d, 0x00, 0x00, 0x00, 0x00, 0x00,
			0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
		};
		RecvAntiScoutReportInfo(c, blocked, sizeof(blocked));
		RecvAntiScoutReportInfo(c, not_blocked, sizeof(not_blocked));
		RecvAntiScoutReportInfo(c, blocked, 10); // too short: must be ignored, not read out of bounds
		CHECK(1, "anti-scout report packets (blocked, not blocked, truncated) parse without crashing");
		free(c);
	}

	/* Delivery tax read from the game. The three reports below are real _MSG_RESP_RESHELPREPORTINFO
	 * payloads captured from two accounts that each sent 1M stone: the game applied 7.5% on the first
	 * (925000 arrived) and 7.6% on the second (924000). The rate is never sent as such, only the net
	 * amount, so it is rebuilt from the gross amount RecvSHelp recorded in last_gross. */
	{
		uint8_t sent_75[] = {
			0x32, 0x21, 0x01, 0x00, 0x00, 0xdc, 0x59, 0xb5, 0x6a, 0x00, 0x00, 0x00, 0x00, 0x00, 0x4c, 0x69,
			0x74, 0x74, 0x6c, 0x65, 0x20, 0x5a, 0x79, 0x63, 0x6f, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x48,
			0x1d, 0x0e, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
		};
		uint8_t sent_76[] = {
			0xe4, 0x50, 0x00, 0x00, 0x00, 0x4a, 0x5a, 0xb5, 0x6a, 0x00, 0x00, 0x00, 0x00, 0x00, 0x5a, 0x79,
			0x63, 0x6f, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x60,
			0x19, 0x0e, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
		};
		uint8_t received_75[] = { /* flag byte 13 = 1: a delivery this account received */
			0xe3, 0x50, 0x00, 0x00, 0x00, 0xdd, 0x59, 0xb5, 0x6a, 0x00, 0x00, 0x00, 0x00, 0x01, 0x5a, 0x79,
			0x63, 0x6f, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x48,
			0x1d, 0x0e, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
		};

		c = fresh("boss");
		c->bank.delivery_tax_percent = 5.0;
		CHECK(DeliveryTaxPercent(c) == 5.0, "delivery tax: configured value used until the game's is known");

		RecvResHelpReport(c, received_75, sizeof(received_75));
		CHECK(!c->delivery_tax_seen_valid, "delivery tax: a received delivery is not a tax reading");

		c->last_gross = 1000000;
		RecvResHelpReport(c, sent_75, sizeof(sent_75));
		CHECK(c->delivery_tax_seen_valid && c->delivery_tax_seen == 7.5, "delivery tax: 1M -> 925000 reads 7.5%");
		CHECK(DeliveryTaxPercent(c) == 7.5, "delivery tax: the game's rate replaces the configured one");

		say(c, "boss", "$gold 1M", COMMAND_CHANNEL_MAIL);
		CHECK(c->transfer.amount == 1081081, "delivery tax: request grossed up with the rate read from the game (1M net -> 1081081)");
		memset(&c->transfer, 0, sizeof(c->transfer));

		RecvResHelpReport(c, sent_76, sizeof(sent_76)); /* no march sent since: last_gross is spent */
		CHECK(c->delivery_tax_seen == 7.5, "delivery tax: a report without a march waiting is ignored");

		c->last_gross = 1000000;
		RecvResHelpReport(c, sent_76, sizeof(sent_76));
		CHECK(c->delivery_tax_seen == 7.6, "delivery tax: 1M -> 924000 reads 7.6%, the rate is followed when it changes");

		c->last_gross = 500; /* too small to read a 0.1% step */
		RecvResHelpReport(c, sent_75, sizeof(sent_75));
		CHECK(c->delivery_tax_seen == 7.6, "delivery tax: a tiny delivery does not move the rate");

		c->last_gross = 1000000;
		RecvResHelpReport(c, sent_75, 20); /* truncated */
		CHECK(c->delivery_tax_seen == 7.6, "delivery tax: truncated report ignored");
		free(c);
	}

	/* _MSG_RESP_RESEARCHINFO, a real 265-byte payload captured at login: research #126 was being
	 * researched (started 74h before the server time of the capture, lasting 93.1h), 297 of the 500
	 * ids had a level. Nothing here is guessed: the levels are the ones the account had. */
	{
		static const uint8_t research_info[] = {
			0x7e, 0x00, 0x06, 0x56, 0x53, 0xb1, 0x6a, 0x00, 0x00, 0x00, 0x00, 0xd7, 0x1d, 0x05, 0x00, 0xaa,
			0xaa, 0xaa, 0xaa, 0x1a, 0x11, 0x51, 0x55, 0x11, 0x10, 0x01, 0x11, 0xa0, 0xaa, 0xaa, 0xaa, 0xaa,
			0xaa, 0xaa, 0xaa, 0x1a, 0x1a, 0x11, 0x11, 0x11, 0x11, 0x11, 0x41, 0x22, 0x22, 0x21, 0x11, 0x00,
			0x00, 0x50, 0x23, 0xaa, 0x1a, 0x11, 0x11, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa,
			0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0x1a, 0xaa, 0xaa, 0xa1, 0xaa, 0x51, 0xaa, 0xa5, 0x55, 0xa4, 0x76,
			0x07, 0x00, 0x10, 0x11, 0x11, 0xaa, 0x66, 0xaa, 0xaa, 0x01, 0xa0, 0xaa, 0x0a, 0x00, 0x00, 0xaa,
			0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0x1a, 0x11, 0xaa, 0xaa, 0xaa, 0xaa, 0x1a, 0x11, 0xa1, 0xaa,
			0xaa, 0x10, 0x00, 0x00, 0xaa, 0x8a, 0xaa, 0x8a, 0xaa, 0x8a, 0x00, 0x00, 0x10, 0x08, 0x00, 0x00,
			0x11, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10,
			0x11, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0x1a, 0xa1, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0x1a, 0x11,
			0x01, 0xa5, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa,
			0x9a, 0x79, 0x08, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
			0x00, 0x11, 0x00, 0x00, 0x10, 0x02, 0x11, 0x11, 0x01, 0x00, 0x10, 0xa1, 0xaa, 0x5a, 0x4a, 0x3a,
			0xa4, 0x16, 0x51, 0x22, 0x16, 0x40, 0x00, 0x14, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
			0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
			0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
			0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
		};
		c = fresh("boss");
		c->server_time = 1790272536; /* server clock of that capture */
		CHECK(sizeof(research_info) == RESEARCH_INFO_SIZE, "research: captured packet has the documented size");
		RecvTechnologyInfo(c, research_info, sizeof(research_info));
		CHECK(c->research.loaded && ResearchInProgress(&c->research), "research: packet loaded, a research is in progress");
		CHECK(c->research.in_progress == 126 && c->research.total_time == 335319, "research: in progress #126, 335319s long");
		CHECK(c->research.start_time == 1790006102, "research: start time read");
		CHECK(ResearchSecondsLeft(&c->research, (int64_t)c->server_time) == 335319 - (1790272536 - 1790006102),
			"research: time left = duration - elapsed (~19.1h)");
		CHECK(ResearchLevel(&c->research, 125) == 5, "research: Bigger Bags I (#125) is level 5 (odd id, low nibble)");
		CHECK(ResearchLevel(&c->research, 126) == 5, "research: #126 is level 5 (even id, high nibble)");
		CHECK(ResearchLevel(&c->research, 234) == 0, "research: Bigger Bags II (#234) is level 0");
		CHECK(ResearchLevel(&c->research, 303) == 10, "research: Bigger Bags III (#303) is level 10");
		CHECK(ResearchLevel(&c->research, 0) == 0 && ResearchLevel(&c->research, 501) == 0, "research: ids outside 1..500 read as 0");
		CHECK(ResearchStartedCount(&c->research) == 297, "research: 297 researches with a level");
		CHECK(ResearchKnownName(303) != NULL && ResearchKnownName(126) == NULL, "research: known names only for the ids that were identified");

		uint8_t idle[RESEARCH_INFO_SIZE];
		memcpy(idle, research_info, sizeof(idle));
		idle[0] = idle[1] = 0; /* no research running */
		RecvTechnologyInfo(c, idle, sizeof(idle));
		CHECK(!ResearchInProgress(&c->research) && ResearchSecondsLeft(&c->research, 0) == 0, "research: none in progress when the id is 0");

		uint16_t before = c->research.in_progress;
		RecvTechnologyInfo(c, research_info, 20); /* truncated */
		CHECK(c->research.in_progress == before, "research: truncated packet ignored");
		free(c);
	}

	/* Research lifecycle, from a capture of the official client (research #57, levels 3 to 5):
	 * start with resource items, cancel, and the server's answers. Request payloads are the decrypted
	 * ones, so the bytes the bot builds can be compared as they are. */
	{
static const uint8_t req_start_l4_two_items[] = {
			0x94, 0x00, 0x00, 0x00, 0x39, 0x00, 0x04, 0x02, 0x00, 0xf1, 0x03, 0x01, 0x00, 0xa2, 0x04, 0x01,
			0x00
};
static const uint8_t req_cancel_l4[] = {
			0x96, 0x00, 0x00, 0x00, 0x39, 0x00, 0x04, 0x00, 0x00, 0x00
};
static const uint8_t req_start_l5_three_items[] = {
			0x9c, 0x00, 0x00, 0x00, 0x39, 0x00, 0x05, 0x03, 0x00, 0xf1, 0x03, 0x03, 0x00, 0xe4, 0x04, 0x01,
			0x00, 0xfa, 0x03, 0x02, 0x00
};
static const uint8_t resp_start_l4[] = {
			0x00, 0x39, 0x00, 0x04, 0x5d, 0x67, 0xb5, 0x6a, 0x00, 0x00, 0x00, 0x00, 0xfd, 0x0b, 0x00, 0x00,
			0x81, 0x0c, 0x00, 0x00, 0xa0, 0xc0, 0x16, 0x00, 0x9b, 0xe6, 0x36, 0x00, 0x71, 0x29, 0x7b, 0x00,
			0x8c, 0x6b, 0x04, 0x00, 0xcd, 0x0e, 0x42, 0x00, 0x68, 0x87, 0x01, 0x00
};
static const uint8_t resp_start_l3_free[] = {
			0x00, 0x39, 0x00, 0x03, 0x5b, 0x67, 0xb5, 0x6a, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
			0x1b, 0x1e, 0x00, 0x00, 0x79, 0x63, 0x17, 0x00, 0xe2, 0x89, 0x37, 0x00, 0xb8, 0xcc, 0x7b, 0x00,
			0x8b, 0x8f, 0x06, 0x00, 0x96, 0x0e, 0x42, 0x00, 0x68, 0x87, 0x01, 0x00
};
static const uint8_t resp_cancel_l4[] = {
			0x00, 0x39, 0x00, 0x04, 0xa3, 0x51, 0x00, 0x00, 0x20, 0x13, 0x17, 0x00, 0x3e, 0x38, 0x37, 0x00,
			0x14, 0x7b, 0x7b, 0x00, 0xbf, 0x7e, 0x05, 0x00, 0x3c, 0x0f, 0x42, 0x00, 0x68, 0x87, 0x01, 0x00
};
static const uint8_t resp_complete_l4[] = {
			0x39, 0x00, 0x04
};

		c = fresh("boss");

		/* start level 4 with two resource items: 30k food (1009) and item 1186, 1 each */
		c->protocol.seq_id = 0x93;
		ResearchItemUse two[] = { { 1009, 1 }, { 1186, 1 } };
		RequestResearchStart(c, 57, 4, two, 2);
		int at = find_packet(1433);
		CHECK(at >= 0 && sent_size[at] == 4 + sizeof(req_start_l4_two_items)
			&& memcmp(sent[at] + 4, req_start_l4_two_items, sizeof(req_start_l4_two_items)) == 0,
			"research: start request is byte for byte the one the game sent (2 items)");

		reset_sent();
		c->protocol.seq_id = 0x9b;
		ResearchItemUse three[] = { { 1009, 3 }, { 1252, 1 }, { 1018, 2 } };
		RequestResearchStart(c, 57, 5, three, 3);
		at = find_packet(1433);
		CHECK(at >= 0 && sent_size[at] == 4 + sizeof(req_start_l5_three_items)
			&& memcmp(sent[at] + 4, req_start_l5_three_items, sizeof(req_start_l5_three_items)) == 0,
			"research: start request with 3 items is byte for byte the game's");

		reset_sent();
		c->protocol.seq_id = 0x95;
		RequestResearchCancel(c, 57, 4);
		at = find_packet(3206);
		CHECK(at >= 0 && sent_size[at] == 4 + sizeof(req_cancel_l4)
			&& memcmp(sent[at] + 4, req_cancel_l4, sizeof(req_cancel_l4)) == 0,
			"research: cancel request is byte for byte the game's");

		reset_sent();
		ResearchItemUse many[RESEARCH_START_MAX_ITEMS + 4];
		memset(many, 0, sizeof(many));
		RequestResearchStart(c, 57, 4, many, RESEARCH_START_MAX_ITEMS + 4); /* more items than allowed: capped */
		at = find_packet(1433);
		CHECK(at >= 0 && sent_size[at] == 4 + 4 + 2 + 1 + 2 + 4 * RESEARCH_START_MAX_ITEMS, "research: item count capped");

		reset_sent();
		RequestResearchStart(c, 57, 4, NULL, 5); /* no item list: sent with no item, not read from NULL */
		at = find_packet(1433);
		CHECK(at >= 0 && sent_size[at] == 4 + 4 + 2 + 1 + 2, "research: a missing item list is sent as none");
		free(c);

		c = fresh("boss");
		RecvResearchStart(c, resp_start_l4, sizeof(resp_start_l4));
		CHECK(ResearchInProgress(&c->research) && c->research.in_progress == 57, "research: start answer sets the research in progress");
		CHECK(c->research.start_time == 1790273373 && c->research.total_time == 3069, "research: start time and duration read (3069s)");

		RecvResearchCancel(c, resp_cancel_l4, sizeof(resp_cancel_l4));
		CHECK(!ResearchInProgress(&c->research), "research: cancel answer clears the research in progress");

		RecvResearchStart(c, resp_start_l4, sizeof(resp_start_l4));
		CHECK(ResearchLevel(&c->research, 57) == 0, "research: level unchanged while it is running");
		RecvResearchComplete(c, resp_complete_l4, sizeof(resp_complete_l4));
		CHECK(ResearchLevel(&c->research, 57) == 4 && !ResearchInProgress(&c->research),
			"research: completion push sets the level reached and clears the research in progress");
		CHECK(ResearchLevel(&c->research, 56) == 0 && ResearchLevel(&c->research, 58) == 0, "research: neighbours untouched by the level write");

		RecvResearchStart(c, resp_start_l3_free, sizeof(resp_start_l3_free));
		CHECK(c->research.total_time == 0, "research: a start finished at once for free has no duration");

		uint8_t refused[sizeof(resp_start_l4)];
		memcpy(refused, resp_start_l4, sizeof(refused));
		refused[0] = 5;
		uint16_t tech_before = c->research.in_progress;
		RecvResearchStart(c, refused, sizeof(refused));
		CHECK(c->research.in_progress == tech_before, "research: a refused start changes nothing");
		RecvResearchStart(c, resp_start_l4, 5);
		RecvResearchCancel(c, resp_cancel_l4, 2);
		RecvResearchComplete(c, resp_complete_l4, 1);
		CHECK(1, "research: truncated answers are ignored without crashing");
		free(c);
	}

	/* Delivery tax implied by research #126 (Tax Break): the two accounts captured had it at level 5
	 * (tax 7.5%) and level 4 (7.6%). */
	{
		c = fresh("boss");
		c->bank.delivery_tax_percent = 3.0;
		CHECK(DeliveryTaxPercent(c) == 3.0, "tax by research: configured value while the research packet has not arrived");

		c->research.loaded = true;
		CHECK(DeliveryTaxPercent(c) == 3.0, "tax by research: configured value while the Trading Post level is unknown");
		c->trading_post_level = 30; /* mana 1: the tax is the level 25 one, 8% */
		ResearchSetLevel(&c->research, RESEARCH_TAX_BREAK_ID, 5);
		CHECK(ResearchDeliveryTaxPercent(&c->research, 8.0) > 7.49 && ResearchDeliveryTaxPercent(&c->research, 8.0) < 7.51, "tax by research: level 5 -> 7.5%");
		CHECK(DeliveryTaxPercent(c) > 7.49 && DeliveryTaxPercent(c) < 7.51, "tax by research: used before any delivery has been read");
		ResearchSetLevel(&c->research, RESEARCH_TAX_BREAK_ID, 4);
		CHECK(ResearchDeliveryTaxPercent(&c->research, 8.0) > 7.59 && ResearchDeliveryTaxPercent(&c->research, 8.0) < 7.61, "tax by research: level 4 -> 7.6%");
		ResearchSetLevel(&c->research, RESEARCH_TAX_BREAK_ID, 10);
		CHECK(ResearchDeliveryTaxPercent(&c->research, 8.0) > 6.99 && ResearchDeliveryTaxPercent(&c->research, 8.0) < 7.01, "tax by research: level 10 (max) -> 7.0%");

		c->delivery_tax_seen = 7.2;
		c->delivery_tax_seen_valid = true;
		CHECK(DeliveryTaxPercent(c) == 7.2, "tax by research: the rate read from a delivery wins over the research");

		uint8_t done_126[] = { RESEARCH_TAX_BREAK_ID, 0x00, 6 };
		RecvResearchComplete(c, done_126, sizeof(done_126));
		CHECK(!c->delivery_tax_seen_valid && ResearchLevel(&c->research, RESEARCH_TAX_BREAK_ID) == 6,
			"tax by research: finishing Tax Break drops the rate read earlier (it is out of date)");
		CHECK(DeliveryTaxPercent(c) > 7.39 && DeliveryTaxPercent(c) < 7.41, "tax by research: level 6 -> 7.4%");

		uint8_t done_other[] = { 200, 0x00, 3 };
		c->delivery_tax_seen = 7.4;
		c->delivery_tax_seen_valid = true;
		RecvResearchComplete(c, done_other, sizeof(done_other));
		CHECK(c->delivery_tax_seen_valid, "tax by research: another research finishing keeps the rate read");
		free(c);
	}

	/* Levels compared with the previous login: every change names a research by its protocol id. */
	{
		char dir[] = "/tmp/lmbot_research_XXXXXX";
		CHECK(mkdtemp(dir) != NULL, "research snapshot: temporary folder");
		c = fresh("boss");
		snprintf(c->bot.data_path, sizeof(c->bot.data_path), "%s/", dir);
		c->research.loaded = true;
		ResearchSetLevel(&c->research, 57, 2);
		ResearchSetLevel(&c->research, 126, 5);

		CHECK(ResearchSnapshotDiff(c) == 0, "research snapshot: first login has nothing to compare with");
		CHECK(ResearchSnapshotDiff(c) == 0, "research snapshot: unchanged levels report nothing");

		ResearchSetLevel(&c->research, 57, 5);
		ResearchSetLevel(&c->research, 200, 1);
		CHECK(ResearchSnapshotDiff(c) == 2, "research snapshot: two changed researches are reported (#57 2->5, #200 0->1)");
		CHECK(ResearchSnapshotDiff(c) == 0, "research snapshot: reported once, the new levels are the baseline");
		free(c);

		c = fresh("boss"); /* unwritable data folder: must not crash nor report */
		c->research.loaded = true;
		CHECK(ResearchSnapshotDiff(c) == 0, "research snapshot: unwritable folder is harmless");
		free(c);

		char cleanup[64];
		snprintf(cleanup, sizeof(cleanup), "rm -rf %s", dir);
		if (system(cleanup) != 0) { /* best effort */ }
	}

	/* Buildings: the level goes on after 25 as the Mana upgrade (6 levels of 5 steps), and the Trading Post's
	 * capacity and tax follow it. */
	{
		CHECK(BuildingManaLevel(25) == 0 && BuildingManaSteps(25) == 0, "buildings: 25 is the normal maximum, no mana");
		CHECK(BuildingManaLevel(29) == 0 && BuildingManaSteps(29) == 4, "buildings: 29 is still mana 0, 4 steps done");
		CHECK(BuildingManaLevel(30) == 1 && BuildingManaSteps(30) == 0, "buildings: 30 is mana 1");
		CHECK(BuildingManaLevel(32) == 1 && BuildingManaSteps(32) == 2, "buildings: 32 is mana 1 with 2 of 5 steps toward mana 2");
		CHECK(BuildingManaLevel(34) == 1 && BuildingManaSteps(34) == 4, "buildings: 34 is still mana 1");
		CHECK(BuildingManaLevel(35) == 2 && BuildingManaSteps(35) == 0, "buildings: 35 is mana 2 (the player's level 25 + mana 2)");
		CHECK(BuildingManaLevel(55) == 6 && BuildingManaSteps(55) == 0, "buildings: 55 is mana 6, the maximum");
		CHECK(BuildingManaLevel(200) == 6 && BuildingNormalLevel(200) == 25, "buildings: above the maximum nothing grows");
		CHECK(BuildingNormalLevel(9) == 9, "buildings: a level under 25 is its own normal level");

		CHECK(TradingPostCapacity(0) == 0, "trading post: not built, no capacity");
		CHECK(TradingPostCapacity(24) == 2500000, "trading post: level 24 carries 2,500,000");
		CHECK(TradingPostCapacity(25) == 3000000, "trading post: level 25 carries 3,000,000");
		CHECK(TradingPostCapacity(30) == 3030000, "trading post: level 30 (mana 1) carries 3,030,000, not 0");
		CHECK(TradingPostCapacity(32) == 3042000, "trading post: each mana step adds 6,000");
		CHECK(TradingPostCapacity(55) == 3180000, "trading post: capped at the maximum level");
		CHECK(TradingPostSupplyTaxPercent(25) == 8.0 && TradingPostSupplyTaxPercent(30) == 8.0,
			"trading post: 8% from level 25, mana does not change it");
		CHECK(TradingPostSupplyTaxPercent(1) == 30.0, "trading post: 30% at level 1");

		/* supply capacity = building + Bigger Bags I, II, III: the account at Trading Post 30 with the
		 * researches at 5, 0 and 10 was shown 4,380,000 by the game. */
		c = fresh("boss");
		c->trading_post_level = 30;
		c->research.loaded = true;
		ResearchSetLevel(&c->research, 125, 5);
		ResearchSetLevel(&c->research, 234, 0);
		ResearchSetLevel(&c->research, 303, 10);
		RecomputeSupplyCapacity(c);
		CHECK(c->supply_capacity == 4380000, "supply capacity: Trading Post 30 + Bigger Bags 5/0/10 = 4,380,000, as the game showed");

		c->trading_post_level = 25;
		ResearchSetLevel(&c->research, 125, 4);
		ResearchSetLevel(&c->research, 303, 0);
		RecomputeSupplyCapacity(c);
		CHECK(c->supply_capacity == 3000000 + 260000, "supply capacity: Trading Post 25 + Bigger Bags I at 4 (260,000)");

		c->research.loaded = false;
		RecomputeSupplyCapacity(c);
		CHECK(c->supply_capacity == 3000000, "supply capacity: the building alone until the researches are known");

		c->trading_post_level = 0;
		RecomputeSupplyCapacity(c);
		CHECK(c->supply_capacity == 0, "supply capacity: no Trading Post, no capacity (the bank refuses, as before)");

		/* a Bigger Bags finishing raises it */
		c->trading_post_level = 30;
		c->research.loaded = true;
		ResearchSetLevel(&c->research, 125, 4);
		RecomputeSupplyCapacity(c);
		uint32_t before = c->supply_capacity;
		uint8_t done_bags[] = { 125, 0x00, 5 };
		RecvResearchComplete(c, done_bags, sizeof(done_bags));
		CHECK(c->supply_capacity == before + (350000 - 260000), "supply capacity: Bigger Bags I 4 -> 5 adds 90,000 at once");
		free(c);

		/* the tax follows the Trading Post's level: 25% at level 10, minus Tax Break */
		c = fresh("boss");
		c->research.loaded = true;
		c->trading_post_level = 10;
		ResearchSetLevel(&c->research, RESEARCH_TAX_BREAK_ID, 3);
		CHECK(DeliveryTaxPercent(c) > 24.69 && DeliveryTaxPercent(c) < 24.71, "tax by research: Trading Post 10 (25%) with Tax Break 3 -> 24.7%");
		free(c);
	}

	/* Buildings compared with the previous login: an upgrade or a new building names its build_id. */
	{
		char dir[] = "/tmp/lmbot_building_XXXXXX";
		CHECK(mkdtemp(dir) != NULL, "building snapshot: temporary folder");
		c = fresh("boss");
		snprintf(c->bot.data_path, sizeof(c->bot.data_path), "%s/", dir);
		c->building_count = 3;
		c->building[0] = (BuildingInfo){ .position_id = 62230, .build_id = 17, .level = 30 };
		c->building[1] = (BuildingInfo){ .position_id = 10514, .build_id = 18, .level = 35 };
		c->building[2] = (BuildingInfo){ .position_id = 7926,  .build_id = 23, .level = 30 };

		CHECK(BuildingSnapshotDiff(c) == 0, "building snapshot: first login has nothing to compare with");
		CHECK(BuildingSnapshotDiff(c) == 0, "building snapshot: unchanged buildings report nothing");

		c->building[2].level = 31; /* #23 gets one step */
		CHECK(BuildingSnapshotDiff(c) == 1, "building snapshot: an upgrade is reported (#23, 30 -> 31)");
		CHECK(BuildingSnapshotDiff(c) == 0, "building snapshot: reported once");

		c->building_count = 4;
		c->building[3] = (BuildingInfo){ .position_id = 9000, .build_id = 22, .level = 1 };
		CHECK(BuildingSnapshotDiff(c) == 1, "building snapshot: a newly built building is reported with its id");

		c->building_count = 3; /* the last one is gone */
		CHECK(BuildingSnapshotDiff(c) == 1, "building snapshot: a building that disappeared is reported");
		free(c);

		c = fresh("boss"); /* unwritable data folder: harmless */
		c->building_count = 1;
		c->building[0] = (BuildingInfo){ .position_id = 1, .build_id = 1, .level = 1 };
		CHECK(BuildingSnapshotDiff(c) == 0, "building snapshot: unwritable folder is harmless");
		free(c);

		char cleanup[64];
		snprintf(cleanup, sizeof(cleanup), "rm -rf %s", dir);
		if (system(cleanup) != 0) { /* best effort */ }
	}

	/* Buildings under construction, from real _MSG_RESP_BUILDINGEVENT payloads: the account building its Mana
	 * Lode (mana 1 -> mana 1 + 1/5), and the one building nothing. */
	{
static const uint8_t build_event_mana_lode[] = {
			0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
			0x00, 0x00, 0x01, 0xf7, 0xf5, 0x1a, 0x00, 0x1f, 0x80, 0x06, 0xa8, 0x6a, 0x00, 0x00, 0x00, 0x00,
			0x42, 0xb8, 0x12, 0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x7f
};
static const uint8_t build_event_none[] = {
			0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
			0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
			0x00, 0x00, 0x00, 0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x7f
};

		c = fresh("boss");
		c->server_time = 1790272536; /* server clock of that capture */
		CHECK(sizeof(build_event_mana_lode) == BUILDING_QUEUE_INFO_SIZE, "construction: captured packet has the documented size");
		RecvBuildingQueue(c, build_event_mana_lode, sizeof(build_event_mana_lode));
		CHECK(!c->construction[0].used, "construction: the first queue entry is empty");
		CHECK(c->construction[1].used && c->construction[1].build_id == BUILDING_MANA_LODE && c->construction[1].slot == 62967,
			"construction: the Mana Lode (build_id 26, slot 62967) is being built");
		CHECK(c->construction[1].level == 31 && BuildingManaLevel(31) == 1 && BuildingManaSteps(31) == 1,
			"construction: target level 31 = mana 1 + 1/5, what the player said");
		CHECK(c->construction[1].start_time == 1789396608 && c->construction[1].duration == 1226818,
			"construction: start time and duration read");
		CHECK(BuildingConstructionSecondsLeft(&c->construction[1], (int64_t)c->server_time) == 350890,
			"construction: about 4 days left (duration minus elapsed)");
		CHECK(BuildingConstructionSecondsLeft(&c->construction[0], (int64_t)c->server_time) == 0, "construction: an empty entry has no time left");

		RecvBuildingQueue(c, build_event_none, sizeof(build_event_none));
		CHECK(!c->construction[0].used && !c->construction[1].used, "construction: a packet with nothing under construction clears the queue");

		RecvBuildingQueue(c, build_event_mana_lode, 10); /* truncated */
		CHECK(!c->construction[1].used, "construction: a truncated packet is ignored");

		CHECK(BuildingManaLevel(33) == 1 && BuildingManaSteps(33) == 3, "buildings: the Castle Wall at 33 is mana 1 + 3/5, what the player said");
		free(c);
	}

	/* $recall: every march is taken back, then the bot sends none for a while. */
	{
		MarchesPauseEnd();

		c = fresh("boss");
		c->recall.pause_seconds = 300;
		c->player.max_marches = 6;
		c->player.current_marches = 3;
		say(c, "eve", "$recall", COMMAND_CHANNEL_MAIL);
		CHECK(replied("administrateurs") && !c->recall.active && !MarchesPaused(), "recall: refused to a player who is not an administrator");

		reset_sent();
		say(c, "boss", "$recall", COMMAND_CHANNEL_MAIL);
		CHECK(c->recall.active && MarchesPaused(), "recall: an administrator starts it, and the pause with it");
		CHECK(MarchesPauseSecondsLeft() >= 299 && MarchesPauseSecondsLeft() <= 300, "recall: the pause is 5 minutes");
		CHECK(replied("5 min"), "recall: the answer says how long the marches are suspended");

		reset_sent();
		RecallTick(c);
		RecallTick(c); /* too soon: the next request is paced 1-2 s later */
		int returns = 0;
		for (int i = 0; i < sent_count; i++)
			if (sent_size[i] >= 4 && (uint16_t)(sent[i][2] | (sent[i][3] << 8)) == _MSG_REQUEST_TROOPRETURN)
				returns++;
		CHECK(returns == 1, "recall: requests go out one at a time, not all at once");

		for (int i = 0; i < 20 && c->recall.active; i++) {
			c->recall.next_at = 0;
			RecallTick(c);
		}
		int indices[16], n = 0;
		for (int i = 0; i < sent_count; i++)
			if (sent_size[i] >= 12 && (uint16_t)(sent[i][2] | (sent[i][3] << 8)) == _MSG_REQUEST_TROOPRETURN && n < 16)
				indices[n++] = (int)(sent[i][8] | (sent[i][9] << 8));
		CHECK(!c->recall.active && n == 6, "recall: one take-back request per march slot of the account (6), then it stops");
		CHECK(n == 6 && indices[0] == 0 && indices[5] == 5, "recall: march indices 0 to 5, in order");
		CHECK(replied("Rappel terminé"), "recall: the requester is told when it is done");

		reset_sent();
		say(c, "boss", "$recall", COMMAND_CHANNEL_MAIL);
		CHECK(c->recall.active, "recall: run again once finished, it starts again and renews the pause");
		say(c, "boss", "$recall", COMMAND_CHANNEL_MAIL);
		CHECK(replied("déjà en cours"), "recall: asked while one is running, it only renews the pause");
		free(c);

		/* nothing out: no request, but the pause holds */
		MarchesPauseEnd();
		c = fresh("boss");
		c->recall.pause_seconds = 300;
		c->player.max_marches = 6;
		c->player.current_marches = 0;
		say(c, "boss", "$recall", COMMAND_CHANNEL_MAIL);
		CHECK(!c->recall.active && MarchesPaused() && replied("rien à rappeler"), "recall: no march out, nothing to take back but still paused");
		free(c);

		/* march counts not received yet */
		MarchesPauseEnd();
		c = fresh("boss");
		c->recall.pause_seconds = 300;
		c->player.max_marches = 0;
		CHECK(StartRecall(c, "boss") == RECALL_NO_DATA && MarchesPaused(), "recall: march counts unknown, the pause holds anyway");

		/* the pause is not in Connection: a reconnection wipes it and must not lift the pause */
		memset(c, 0, sizeof(*c));
		CHECK(MarchesPaused(), "recall: the pause survives the reconnection's wipe of Connection");
		free(c);

		/* gathering waits, and goes on when the pause is over */
		MarchesPauseEnd();
		c = fresh("boss");
		c->recall.pause_seconds = 300;
		c->gather.enabled = true;
		c->gather.max_marches = 1;
		c->player.max_marches = 6;
		c->player.current_marches = 0;
		c->gather.tile_count = 1;
		c->gather.tiles[0] = (GatherTile){ .used = true, .zone_id = 1, .point_id = 2, .level = 3, .amount = 1000 };
		MarchesPauseStart(300);
		reset_sent();
		c->gather.next_march_at = 1;
		GatherTick(c);
		CHECK(find_packet(_MSG_REQUEST_TROOPMARCH_NOTATK) < 0, "recall: no gathering march during the pause");
		MarchesPauseEnd();
		c->gather.next_march_at = 1;
		GatherTick(c);
		CHECK(find_packet(_MSG_REQUEST_TROOPMARCH_NOTATK) >= 0, "recall: gathering goes on once the pause is over");

		/* the request builder refuses too */
		MarchesPauseStart(300);
		reset_sent();
		RequestGatherMarch(c, 1, 2, 0, 10);
		CHECK(find_packet(_MSG_REQUEST_TROOPMARCH_NOTATK) < 0, "recall: the gathering request builder sends nothing during the pause either");
		free(c);

		/* gather never asks for more troops than the account actually has, once c->troop is loaded */
		MarchesPauseEnd();
		c = fresh("boss");
		c->gather.enabled = true;
		c->gather.max_marches = 1;
		c->player.max_marches = 6;
		c->gather.tile_count = 1;
		// amount=1000 / 23.3 + 1 = 44 troops by the uncapped formula
		c->gather.tiles[0] = (GatherTile){ .used = true, .zone_id = 1, .point_id = 2, .level = 3, .amount = 1000 };
		c->troop.loaded = true;
		c->troop.infantry[0] = 5; // only 5 troops recorded as free, whatever tier/kind they are
		c->troop.total = 5;       // total is a separately maintained running sum, not derived - see TroopAdd
		reset_sent();
		c->gather.next_march_at = 1;
		GatherTick(c);
		int k = find_packet(_MSG_REQUEST_TROOPMARCH_NOTATK);
		uint32_t sent_count_field = k >= 0
			? (uint32_t)(sent[k][66] | sent[k][67] << 8 | sent[k][68] << 16 | sent[k][69] << 24) : 0;
		CHECK(k >= 0 && sent_count_field == 5,
			"gather: the troop count is capped to c->troop.total, not the tile-derived formula");

		/* no troops recorded at all: nothing is sent, and the tile is left untargeted */
		c->gather.tiles[0].targeted = false;
		c->troop.infantry[0] = 0;
		c->troop.total = 0;
		c->gather.next_march_at = 1;
		reset_sent();
		GatherTick(c);
		CHECK(find_packet(_MSG_REQUEST_TROOPMARCH_NOTATK) < 0 && !c->gather.tiles[0].targeted,
			"gather: with zero troops recorded, no march is sent and the tile stays available");
		free(c);

		/* gather also subtracts troops already committed to marches still out, not just c->troop.total */
		c = fresh("boss");
		c->gather.enabled = true;
		c->gather.max_marches = 2;
		c->player.max_marches = 6;
		c->gather.tile_count = 2;
		c->gather.tiles[0] = (GatherTile){ .used = true, .zone_id = 1, .point_id = 1, .level = 5, .amount = 5000 };
		c->gather.tiles[1] = (GatherTile){ .used = true, .zone_id = 1, .point_id = 2, .level = 4, .amount = 5000 };
		c->troop.loaded = true;
		c->troop.infantry[0] = 100;
		c->troop.total = 100;
		reset_sent();
		c->gather.next_march_at = 1;
		GatherTick(c); // highest level tile first (5 before 4): takes all 100 available troops
		k = find_packet(_MSG_REQUEST_TROOPMARCH_NOTATK);
		uint32_t amt1 = k >= 0 ? (uint32_t)(sent[k][66] | sent[k][67] << 8 | sent[k][68] << 16 | sent[k][69] << 24) : 0;
		CHECK(k >= 0 && amt1 == 100 && c->gather.troops_out == 100,
			"gather: first march takes all available troops and troops_out tracks it");

		reset_sent();
		c->gather.next_march_at = 1;
		GatherTick(c); // second tile: nothing left free to send
		CHECK(find_packet(_MSG_REQUEST_TROOPMARCH_NOTATK) < 0 && !c->gather.tiles[1].targeted,
			"gather: troops already out on the first march leave nothing free for a second one");

		// that first march is refused: troops_out rolls back too, not just active_marches
		uint8_t refuse[2] = { 2, 0 };
		RecvGatherMarchResp(c, refuse, sizeof(refuse));
		CHECK(c->gather.troops_out == 0, "gather: a refusal rolls troops_out back too");

		// resend now succeeds (troops free again), then the march comes home and credits them back
		c->gather.tiles[0].targeted = false;
		reset_sent();
		c->gather.next_march_at = 1;
		GatherTick(c);
		CHECK(c->gather.troops_out == 100, "gather: troops_out is tracked again after a fresh accepted send");
		RecvGatherTroopHome(c, NULL, 0);
		CHECK(c->gather.troops_out == 0, "gather: troops coming home are credited back to troops_out");
		free(c);

		/* a delivery already going on does not send its next march during the pause, and does after it */
		MarchesPauseEnd();
		c = fresh("boss");
		c->transfer.state = TRANSFER_SEND_MARCH;
		c->transfer.resource_type = RESOURCE_GOLD;
		c->transfer.amount = c->transfer.remaining = 1000;
		c->transfer.zone_id = 1;
		c->transfer.point_id = 2;
		snprintf(c->transfer.target_name, sizeof(c->transfer.target_name), "alice");
		MarchesPauseStart(300);
		reset_sent();
		ResourceTransferTick(c);
		CHECK(find_packet(_MSG_REQUEST_SEND_RESHELP) < 0 && c->transfer.remaining == 1000 && c->transfer.state == TRANSFER_SEND_MARCH,
			"recall: a delivery's next march waits during the pause, its state untouched");
		MarchesPauseEnd();
		ResourceTransferTick(c);
		CHECK(find_packet(_MSG_REQUEST_SEND_RESHELP) >= 0, "recall: the delivery's march goes out once the pause is over");
		free(c);

		/* deliveries: refused up front, and one in progress is cancelled */
		MarchesPauseEnd();
		c = fresh("boss");
		c->recall.pause_seconds = 300;
		c->player.max_marches = 6;
		c->player.current_marches = 0;
		c->transfer.state = TRANSFER_SEND_MARCH;
		snprintf(c->transfer.target_name, sizeof(c->transfer.target_name), "alice");
		c->transfer.remaining = 500000;
		reset_sent();
		say(c, "boss", "$recall", COMMAND_CHANNEL_MAIL);
		CHECK(c->transfer.state == TRANSFER_IDLE && replied("annulée"), "recall: a delivery in progress is cancelled, and its requester told");

		reset_sent();
		say(c, "boss", "$gold 1M", COMMAND_CHANNEL_MAIL);
		CHECK(c->transfer.state == TRANSFER_IDLE && replied("suspendues"), "recall: a delivery asked during the pause is refused, with the reason");

		MarchesPauseEnd();
		reset_sent();
		say(c, "boss", "$help", COMMAND_CHANNEL_MAIL);
		CHECK(replied("recall"), "recall: listed in the administrators' help");
		reset_sent();
		say(c, "eve", "$help", COMMAND_CHANNEL_MAIL);
		CHECK(!replied("recall"), "recall: not listed for everybody else");
		free(c);

		/* pause configured to 0: the recall is done, no pause */
		MarchesPauseEnd();
		c = fresh("boss");
		c->recall.pause_seconds = 0;
		c->player.max_marches = 6;
		c->player.current_marches = 2;
		say(c, "boss", "$recall", COMMAND_CHANNEL_MAIL);
		CHECK(c->recall.active && !MarchesPaused(), "recall: recall.pause_seconds = 0 recalls without any pause");
		free(c);

		char shown[32];
		FormatDurationFr(300, shown, sizeof(shown));
		CHECK(strcmp(shown, "5 min") == 0, "recall: 300 seconds read as 5 min");
		FormatDurationFr(270, shown, sizeof(shown));
		CHECK(strcmp(shown, "4 min 30 s") == 0, "recall: 270 seconds read as 4 min 30 s");
		FormatDurationFr(45, shown, sizeof(shown));
		CHECK(strcmp(shown, "45 s") == 0, "recall: 45 seconds read as 45 s");

		MarchesPauseEnd(); /* leave no pause behind for whatever runs after */
	}

	/* Guild bank: members deposit by sending resources to the bot, the bot keeps a balance per player, the
	 * resource commands take it back. The deposit below is a real delivery report received by an account
	 * (Zyco sent it 925,000 stone), flag byte 13 = 1. */
	{
		static const uint8_t deposit_report[] = {
			0xe3, 0x50, 0x00, 0x00, 0x00, 0xdd, 0x59, 0xb5, 0x6a, 0x00, 0x00, 0x00, 0x00, 0x01, 0x5a, 0x79,
			0x63, 0x6f, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x48,
			0x1d, 0x0e, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
		};
		char dir[] = "/tmp/lmbot_bank_XXXXXX";
		CHECK(mkdtemp(dir) != NULL, "guild bank: temporary folder");
		GuildBankReset();
		MarchesPauseEnd();

		c = fresh("boss");
		snprintf(c->bot.data_path, sizeof(c->bot.data_path), "%s/", dir);
		c->guildbank.enabled = true;
		c->server_time = 1000;   /* the bank starts counting at 1000; the report is dated far later */
		c->resources.rock = 100000000;
		c->bank.delivery_tax_percent = 5.0;
		const char *members[] = { "Zyco", "alice", "boss", "Bob", "Little Zyco" };
		for (int i = 0; i < 5; i++)
			snprintf(c->alliance_member.member[c->alliance_member.count++].name, 14, "%s", members[i]);

		/* a deposit */
		reset_sent();
		RecvResHelpReport(c, deposit_report, sizeof(deposit_report));
		CHECK(GuildBankBalance("Zyco", RESOURCE_ROCK) == 925000, "guild bank: a delivery received is credited to its sender, at the net amount");
		CHECK(GuildBankBalance("Zyco", RESOURCE_FOOD) == 0 && GuildBankTotal(RESOURCE_ROCK) == 925000, "guild bank: only the resource sent is credited");
		CHECK(replied("Dépôt reçu"), "guild bank: the sender is told");

		RecvResHelpReport(c, deposit_report, sizeof(deposit_report));
		CHECK(GuildBankBalance("Zyco", RESOURCE_ROCK) == 925000, "guild bank: the same report delivered again (next login) is counted once");

		/* older than the bank: not a deposit */
		uint8_t old_report[sizeof(deposit_report)];
		memcpy(old_report, deposit_report, sizeof(old_report));
		old_report[0] = 0xe9; old_report[5] = 0x10; old_report[6] = 0x00; old_report[7] = 0x00; old_report[8] = 0x00;   /* another report, dated 16 */
		RecvResHelpReport(c, old_report, sizeof(old_report));
		CHECK(GuildBankBalance("Zyco", RESOURCE_ROCK) == 925000, "guild bank: a delivery older than the bank is not credited");

		/* a delivery sent by the bot (flag 0) is not a deposit */
		uint8_t sent_report[sizeof(deposit_report)];
		memcpy(sent_report, deposit_report, sizeof(sent_report));
		sent_report[0] = 0xf1; sent_report[13] = 0;
		RecvResHelpReport(c, sent_report, sizeof(sent_report));
		CHECK(GuildBankBalance("Zyco", RESOURCE_ROCK) == 925000, "guild bank: a delivery the bot sent is not a deposit");

		/* the file keeps it: a restart (memory wiped) finds the balance and does not count the report again */
		GuildBankReset();
		RecvResHelpReport(c, deposit_report, sizeof(deposit_report));
		CHECK(GuildBankBalance("Zyco", RESOURCE_ROCK) == 925000, "guild bank: the balance survives a restart, and the report is still counted once");

		/* off: nothing is credited */
		Connection *off = fresh("boss");
		snprintf(off->bot.data_path, sizeof(off->bot.data_path), "%s/", dir);
		RecvResHelpReport(off, deposit_report, sizeof(deposit_report));
		free(off);
		CHECK(GuildBankBalance("Zyco", RESOURCE_ROCK) == 925000, "guild bank: disabled, a delivery received changes nothing");

		/* $bal */
		reset_sent();
		say(c, "Zyco", "$bal", COMMAND_CHANNEL_MAIL);
		CHECK(replied("925 000") && replied("pierre"), "guild bank: $bal shows the player's balance, digit by digit");
		reset_sent();
		say(c, "alice", "$bal", COMMAND_CHANNEL_MAIL);
		CHECK(replied("vide"), "guild bank: $bal of a player with nothing says so");
		reset_sent();
		say(c, "alice", "$bal Zyco", COMMAND_CHANNEL_MAIL);
		CHECK(replied("Seuls les administrateurs"), "guild bank: only an administrator reads somebody else's balance");
		reset_sent();
		say(c, "boss", "$bal Zyco", COMMAND_CHANNEL_MAIL);
		CHECK(replied("925 000"), "guild bank: an administrator reads another player's balance");

		/* withdrawals */
		reset_sent();
		say(c, "eve", "$stone 100k", COMMAND_CHANNEL_MAIL);
		CHECK(c->transfer_queue_count == 0 && replied("réservée aux membres"), "guild bank: somebody outside the guild is refused, with the reason");
		reset_sent();
		say(c, "alice", "$stone 100k", COMMAND_CHANNEL_MAIL);
		CHECK(c->transfer_queue_count == 0 && replied("vide"), "guild bank: a member with no balance is told it is empty");
		reset_sent();
		say(c, "Zyco", "$stone 1M", COMMAND_CHANNEL_MAIL);
		CHECK(c->transfer_queue_count == 0 && replied("insuffisant"), "guild bank: asking for more than the balance is refused");
		reset_sent();
		say(c, "boss", "$stone 1M", COMMAND_CHANNEL_MAIL);
		CHECK(c->transfer_queue_count == 0 && replied("vide"), "guild bank: an administrator takes back their own balance like everybody, not the stock");

		say(c, "Zyco", "$stone 500k", COMMAND_CHANNEL_MAIL);
		CHECK(c->transfer_queue_count == 1 && c->transfer_queue[0].amount == 526316 && c->transfer_queue[0].from_balance,
			"guild bank: 500k asked is 526,316 sent so that 500k arrives after the 5% tax");
		ResourceTransferTick(c);
		CHECK(c->transfer.state == TRANSFER_WAIT_TARGET && c->transfer.from_balance && strcmp(c->transfer.balance_owner, "Zyco") == 0,
			"guild bank: the request starts as soon as nothing else is going on (and looks the player up)");
		CHECK(GuildBankBalance("Zyco", RESOURCE_ROCK) == 925000, "guild bank: nothing is debited before a march leaves");

		/* the march leaves: the gross amount is debited at once */
		c->transfer.zone_id = 1; c->transfer.point_id = 2; c->transfer.state = TRANSFER_SEND_MARCH; c->transfer.not_before = 0;
		reset_sent();
		ResourceTransferTick(c);
		CHECK(find_packet(_MSG_REQUEST_SEND_RESHELP) >= 0, "guild bank: the delivery's march is sent");
		CHECK(GuildBankBalance("Zyco", RESOURCE_ROCK) == 925000 - 526316 && c->transfer.in_flight == 526316,
			"guild bank: the gross amount that leaves is debited from the balance");

		/* the server refuses it: the debit goes back */
		uint8_t refused[72] = { 14 };
		reset_sent();
		RecvSHelp(c, refused);
		CHECK(GuildBankBalance("Zyco", RESOURCE_ROCK) == 925000 && c->transfer.state == TRANSFER_FAILED,
			"guild bank: a march the server refuses gives its debit back");
		ResourceTransferTick(c);
		CHECK(c->transfer.state == TRANSFER_IDLE, "guild bank: the failed delivery is over");

		/* accepted: the debit stands, and the player is told when it is done */
		say(c, "Zyco", "$stone 500k", COMMAND_CHANNEL_MAIL);
		ResourceTransferTick(c);
		c->transfer.zone_id = 1; c->transfer.point_id = 2; c->transfer.state = TRANSFER_SEND_MARCH; c->transfer.not_before = 0;
		ResourceTransferTick(c);
		uint8_t accepted[72] = { 0 };
		reset_sent();
		RecvSHelp(c, accepted);
		CHECK(GuildBankBalance("Zyco", RESOURCE_ROCK) == 925000 - 526316 && c->transfer.in_flight == 0, "guild bank: an accepted march keeps its debit");
		c->transfer.not_before = 0;
		ResourceTransferTick(c);   /* nothing left to send: complete */
		ResourceTransferTick(c);
		CHECK(c->transfer.state == TRANSFER_IDLE && replied("Retrait terminé") && replied("Solde restant"),
			"guild bank: the player is told the withdrawal is done, with what is left");

		/* $stop before the server answered gives the debit back */
		say(c, "Zyco", "$stone 100k", COMMAND_CHANNEL_MAIL);
		ResourceTransferTick(c);
		c->transfer.zone_id = 1; c->transfer.point_id = 2; c->transfer.state = TRANSFER_SEND_MARCH; c->transfer.not_before = 0;
		ResourceTransferTick(c);
		uint64_t debited = GuildBankBalance("Zyco", RESOURCE_ROCK);
		say(c, "Zyco", "$stop", COMMAND_CHANNEL_MAIL);
		CHECK(c->transfer.state == TRANSFER_IDLE && GuildBankBalance("Zyco", RESOURCE_ROCK) == debited + 105263,
			"guild bank: $stop before the march was accepted gives its debit back");

		/* all: the whole balance, whatever the tax */
		say(c, "Zyco", "$stone all", COMMAND_CHANNEL_MAIL);
		CHECK(c->transfer_queue_count == 1 && c->transfer_queue[0].amount == GuildBankBalance("Zyco", RESOURCE_ROCK),
			"guild bank: 'all' takes the whole balance");
		TransferQueueRemove(c, "Zyco");

		/* the queue: one delivery at a time, in order, each told its place */
		GuildBankCredit(c, "alice", RESOURCE_ROCK, 1000000);
		AbortTransfer(c);
		say(c, "Zyco", "$stone 100k", COMMAND_CHANNEL_MAIL);
		ResourceTransferTick(c);            /* Zyco's runs */
		reset_sent();
		say(c, "alice", "$stone 100k", COMMAND_CHANNEL_MAIL);
		CHECK(TransferQueuePosition(c, "alice") == 1 && replied("position 1"), "guild bank: a second member waits in the queue and is told their place");
		reset_sent();
		say(c, "Zyco", "$stone 100k", COMMAND_CHANNEL_MAIL);
		CHECK(replied("précédente est encore en cours"), "guild bank: asking again while one's own delivery runs is refused");
		reset_sent();
		say(c, "alice", "$stop", COMMAND_CHANNEL_MAIL);
		CHECK(TransferQueuePosition(c, "alice") == 0 && replied("en attente est annulée"), "guild bank: $stop removes a request still waiting");
		say(c, "alice", "$stone 100k", COMMAND_CHANNEL_MAIL);
		AbortTransfer(c);
		ResourceTransferTick(c);
		CHECK(c->transfer.state == TRANSFER_WAIT_TARGET && strcmp(c->transfer.issued_name, "alice") == 0, "guild bank: when the first is over the next one starts");

		/* a request cannot outrun its balance: the amount of a march never exceeds what is held */
		AbortTransfer(c);
		c->transfer.from_balance = true;
		snprintf(c->transfer.balance_owner, sizeof(c->transfer.balance_owner), "alice");
		c->transfer.resource_type = RESOURCE_ROCK;
		c->transfer.remaining = 5000000;
		CHECK(CalculateTransferAmount(c) == GuildBankBalance("alice", RESOURCE_ROCK), "guild bank: a march never carries more than the balance holds");
		AbortTransfer(c);

		/* administrators give from the stock, never touching the members' deposits or the reserve */
		c->bank.reserve.rock = 50000000;
		reset_sent();
		say(c, "alice", "$adminstone Bob 1M", COMMAND_CHANNEL_MAIL);
		CHECK(replied("Non autorisé") && c->transfer_queue_count == 0, "admin send: refused to a player who is not an administrator");
		reset_sent();
		say(c, "boss", "$adminstone Bob 60M", COMMAND_CHANNEL_MAIL);
		CHECK(c->transfer_queue_count == 0 && replied("Ressources insuffisantes"), "admin send: more than the stock above the reserve and the deposits is refused");
		reset_sent();
		say(c, "boss", "$adminstone Nobody 1M", COMMAND_CHANNEL_MAIL);
		CHECK(c->transfer_queue_count == 0 && replied("n'est pas dans la guilde"), "admin send: a player outside the guild is refused");
		say(c, "boss", "$adminstone Little Zyco 1M", COMMAND_CHANNEL_MAIL);
		CHECK(c->transfer_queue_count == 1 && strcmp(c->transfer_queue[0].target, "Little Zyco") == 0 && strcmp(c->transfer_queue[0].requester, "boss") == 0
			&& !c->transfer_queue[0].from_balance, "admin send: a name with a space is read whole, the amount is the last word");
		ResourceTransferTick(c);
		CHECK(strcmp(TransferRequester(c), "boss") == 0 && strcmp(c->transfer.target_name, "Little Zyco") == 0,
			"admin send: the messages go to the administrator, the resources to the named player");
		AbortTransfer(c);

		/* $recall clears the queue too */
		say(c, "Zyco", "$stone 100k", COMMAND_CHANNEL_MAIL);
		ResourceTransferTick(c);
		say(c, "alice", "$stone 100k", COMMAND_CHANNEL_MAIL);
		c->recall.pause_seconds = 300;
		reset_sent();
		say(c, "boss", "$recall", COMMAND_CHANNEL_MAIL);
		CHECK(c->transfer.state == TRANSFER_IDLE && c->transfer_queue_count == 0 && replied("en attente est annulée"),
			"guild bank: $recall cancels the delivery in progress and the requests waiting, and tells them");
		MarchesPauseEnd();
		free(c);

		/* damaged names are refused, they could break the file */
		c = fresh("boss");
		snprintf(c->bot.data_path, sizeof(c->bot.data_path), "%s/", dir);
		CHECK(!GuildBankCredit(c, "bad\tname", RESOURCE_FOOD, 5) && !GuildBankCredit(c, "", RESOURCE_FOOD, 5), "guild bank: a name with a tab or an empty name is refused");
		CHECK(!GuildBankDebit(c, "Zyco", RESOURCE_ROCK, 900000000ULL) && GuildBankDebit(c, "Zyco", RESOURCE_ROCK, 1),
			"guild bank: a debit above the balance changes nothing, one within it works");
		free(c);

		GuildBankReset();
		char cleanup[64];
		snprintf(cleanup, sizeof(cleanup), "rm -rf %s", dir);
		if (system(cleanup) != 0) { /* best effort */ }
	}

	/* Guild bank, changes asked by the web console: it never writes guild_bank.txt while the bot runs, it queues
	 * requests in guild_bank_edits.txt and the bot applies them. */
	{
		char dir[] = "/tmp/lmbot_bankedit_XXXXXX";
		CHECK(mkdtemp(dir) != NULL, "bank edits: temporary folder");
		GuildBankReset();

		c = fresh("boss");
		snprintf(c->bot.data_path, sizeof(c->bot.data_path), "%s/", dir);
		c->guildbank.enabled = true;
		c->server_time = 5000;
		GuildBankCredit(c, "Bob", RESOURCE_FOOD, 100);
		GuildBankCredit(c, "alice", RESOURCE_ROCK, 200);

		char edits[400];
		snprintf(edits, sizeof(edits), "%s/guild_bank_edits.txt", dir);
		CHECK(GuildBankApplyEdits(c) == 0, "bank edits: nothing to apply when the console asked nothing");

		FILE *fp = fopen(edits, "w");
		fprintf(fp, "set\tBob\t0\t7500000\nset\tLittle Zyco\t1\t42\nset\talice\t1\t0\nnonsense\nset\tBob\t9\t1\nset\tBad\tx\t1\n");
		fclose(fp);
		CHECK(GuildBankApplyEdits(c) == 3, "bank edits: three requests understood, the malformed ones ignored");
		CHECK(GuildBankBalance("Bob", RESOURCE_FOOD) == 7500000, "bank edits: a balance is set to exactly the amount asked");
		CHECK(GuildBankBalance("Little Zyco", RESOURCE_ROCK) == 42, "bank edits: a player who had nothing is created, name with a space included");
		CHECK(GuildBankBalance("alice", RESOURCE_ROCK) == 0, "bank edits: 0 empties a balance");
		CHECK(access(edits, F_OK) != 0, "bank edits: the request file is consumed, the console can start a new one");
		CHECK(GuildBankApplyEdits(c) == 0, "bank edits: applied once");

		/* saved: a restart finds it */
		GuildBankReset();
		GuildBankLoad(c);
		CHECK(GuildBankBalance("Bob", RESOURCE_FOOD) == 7500000, "bank edits: the change is written to guild_bank.txt");

		/* left over by a stop in the middle of a batch: applied again on the next start */
		char working[400];
		snprintf(working, sizeof(working), "%s/guild_bank_edits.applying", dir);
		fp = fopen(working, "w");
		fprintf(fp, "set\tBob\t2\t11\n");
		fclose(fp);
		CHECK(GuildBankApplyEdits(c) == 1 && GuildBankBalance("Bob", RESOURCE_WOOD) == 11, "bank edits: a batch interrupted by a stop is applied on the next run");

		/* reset */
		fp = fopen(edits, "w");
		fprintf(fp, "reset\n");
		fclose(fp);
		c->server_time = 9000;
		CHECK(GuildBankApplyEdits(c) == 1 && GuildBankAccountCount() == 0 && GuildBankTotal(RESOURCE_FOOD) == 0, "bank edits: reset empties every balance");

		/* after a reset, deliveries dated before it do not come back, even with a number never seen */
		static const uint8_t old_delivery[] = {
			0x77, 0x77, 0x00, 0x00, 0x00, 0xdd, 0x0f, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x42, 0x6f,
			0x62, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10,
			0x27, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
		};
		RecvResHelpReport(c, old_delivery, sizeof(old_delivery));   /* dated 4061, before the reset at 9000 */
		CHECK(GuildBankAccountCount() == 0, "bank edits: a delivery older than the reset is not credited afterwards");

		fp = fopen(edits, "w");
		fprintf(fp, "set\tbad\tname\t0\t1\n");   /* a name with a tab in the request */
		fclose(fp);
		GuildBankApplyEdits(c);
		CHECK(GuildBankAccountCount() == 0, "bank edits: a request that cannot be read changes nothing");
		free(c);

		GuildBankReset();
		char cleanup[64];
		snprintf(cleanup, sizeof(cleanup), "rm -rf %s", dir);
		if (system(cleanup) != 0) { /* best effort */ }
	}

	/* Five deposits captured live on an account that was online, one for each resource (asked 1M food, 2M ore, 3M wood,
	 * 500k stone, 200k gold; the sender's tax was 7.2% every time). The game pushed each as a delivery report, flag "received",
	 * as the resources arrived: the message the bank reads. Report numbers go up by one. */
	{
		GuildBankReset();
		char dir[] = "/tmp/lmbot_live_XXXXXX";
		CHECK(mkdtemp(dir) != NULL, "live deposits: temporary folder");
static const uint8_t live_0[] = {
			0x3c, 0x21, 0x01, 0x00, 0x00, 0xd2, 0x9a, 0xb5, 0x6a, 0x00, 0x00, 0x00, 0x00, 0x01, 0x5a, 0x79,
			0x63, 0x6f, 0x20, 0x32, 0x70, 0x6f, 0x69, 0x6e, 0x74, 0x30, 0x00, 0x00, 0x29, 0x0e, 0x00, 0x00,
			0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
		};
static const uint8_t live_1[] = {
			0x3d, 0x21, 0x01, 0x00, 0x00, 0xe7, 0x9a, 0xb5, 0x6a, 0x00, 0x00, 0x00, 0x00, 0x01, 0x5a, 0x79,
			0x63, 0x6f, 0x20, 0x32, 0x70, 0x6f, 0x69, 0x6e, 0x74, 0x30, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
			0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x52, 0x1c, 0x00, 0x00, 0x00, 0x00, 0x00
		};
static const uint8_t live_2[] = {
			0x3e, 0x21, 0x01, 0x00, 0x00, 0xfe, 0x9a, 0xb5, 0x6a, 0x00, 0x00, 0x00, 0x00, 0x01, 0x5a, 0x79,
			0x63, 0x6f, 0x20, 0x32, 0x70, 0x6f, 0x69, 0x6e, 0x74, 0x30, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
			0x00, 0x00, 0x00, 0x00, 0x7b, 0x2a, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
		};
static const uint8_t live_3[] = {
			0x3f, 0x21, 0x01, 0x00, 0x00, 0x13, 0x9b, 0xb5, 0x6a, 0x00, 0x00, 0x00, 0x00, 0x01, 0x5a, 0x79,
			0x63, 0x6f, 0x20, 0x32, 0x70, 0x6f, 0x69, 0x6e, 0x74, 0x30, 0x00, 0x00, 0x00, 0x00, 0x00, 0x80,
			0x14, 0x07, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
		};
static const uint8_t live_4[] = {
			0x40, 0x21, 0x01, 0x00, 0x00, 0x2a, 0x9b, 0xb5, 0x6a, 0x00, 0x00, 0x00, 0x00, 0x01, 0x5a, 0x79,
			0x63, 0x6f, 0x20, 0x32, 0x70, 0x6f, 0x69, 0x6e, 0x74, 0x30, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
			0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xd5, 0x02, 0x00
		};
		const uint8_t *live[5] = { live_0, live_1, live_2, live_3, live_4 };
		c = fresh("boss");
		snprintf(c->bot.data_path, sizeof(c->bot.data_path), "%s/", dir);
		c->guildbank.enabled = true;
		c->server_time = 1000;
		for (int i = 0; i < 5; i++)
			RecvResHelpReport(c, live[i], 47);
		CHECK(GuildBankBalance("Zyco 2point0", RESOURCE_FOOD) == 928000, "live deposits: 1M of food sent, 928,000 credited");
		CHECK(GuildBankBalance("Zyco 2point0", RESOURCE_ORE) == 1856000, "live deposits: 2M of ore sent, 1,856,000 credited");
		CHECK(GuildBankBalance("Zyco 2point0", RESOURCE_WOOD) == 2784000, "live deposits: 3M of wood sent, 2,784,000 credited");
		CHECK(GuildBankBalance("Zyco 2point0", RESOURCE_ROCK) == 464000, "live deposits: 500k of stone sent, 464,000 credited");
		CHECK(GuildBankBalance("Zyco 2point0", RESOURCE_GOLD) == 185600, "live deposits: 200k of gold sent, 185,600 credited");
		CHECK(GuildBankAccountCount() == 1, "live deposits: one player, a 12-character name read whole");

		for (int i = 0; i < 5; i++)
			RecvResHelpReport(c, live[i], 47);   /* the game sends its reports again at the next login */
		CHECK(GuildBankBalance("Zyco 2point0", RESOURCE_FOOD) == 928000 && GuildBankBalance("Zyco 2point0", RESOURCE_GOLD) == 185600,
			"live deposits: the same five reports delivered again change nothing");
		free(c);
		GuildBankReset();
		char cleanup[64];
		snprintf(cleanup, sizeof(cleanup), "rm -rf %s", dir);
		if (system(cleanup) != 0) { /* best effort */ }
	}

	/* The first deposits made to the bot by a player, from its own journal (debug dump of the three delivery reports it
	 * received): Zyco sent 135,033 stone, then 926,000 ore twice. They were lost because the bank was not enabled in that
	 * account's configuration, and the bank was created lazily at the first deposit, dated the same second. */
	{
		GuildBankReset();
		char dir[] = "/tmp/lmbot_first_XXXXXX";
		CHECK(mkdtemp(dir) != NULL, "first deposits: temporary folder");
static const uint8_t first_0[] = {
			0xee, 0xed, 0x00, 0x00, 0x00, 0x44, 0x9d, 0xb5, 0x6a, 0x00, 0x00, 0x00, 0x00, 0x01, 0x5a, 0x79,
			0x63, 0x6f, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x79,
			0x0f, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
		};
static const uint8_t first_1[] = {
			0xef, 0xed, 0x00, 0x00, 0x00, 0x5c, 0x9d, 0xb5, 0x6a, 0x00, 0x00, 0x00, 0x00, 0x01, 0x5a, 0x79,
			0x63, 0x6f, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
			0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x30, 0x21, 0x0e, 0x00, 0x00, 0x00, 0x00, 0x00
		};
static const uint8_t first_2[] = {
			0xf0, 0xed, 0x00, 0x00, 0x00, 0x76, 0x9d, 0xb5, 0x6a, 0x00, 0x00, 0x00, 0x00, 0x01, 0x5a, 0x79,
			0x63, 0x6f, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
			0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x30, 0x21, 0x0e, 0x00, 0x00, 0x00, 0x00, 0x00
		};

		const uint8_t *first[3] = { first_0, first_1, first_2 };

		/* disabled, as in that configuration: nothing is credited, nothing is created */
		c = fresh("boss");
		snprintf(c->bot.data_path, sizeof(c->bot.data_path), "%s/", dir);
		c->server_time = 1790287100;
		for (int i = 0; i < 3; i++)
			RecvResHelpReport(c, first[i], 47);
		CHECK(GuildBankAccountCount() == 0, "first deposits: with the bank disabled a delivery received credits nothing");
		free(c);

		/* enabled: the bank starts counting when the bot comes up (first tick), so what arrives after is counted */
		GuildBankReset();
		c = fresh("boss");
		snprintf(c->bot.data_path, sizeof(c->bot.data_path), "%s/", dir);
		c->guildbank.enabled = true;
		c->server_time = 1790287172;   /* the same second as the first report */
		GuildBankTick(c);
		RecvResHelpReport(c, first[0], 47);
		RecvResHelpReport(c, first[1], 47);
		RecvResHelpReport(c, first[2], 47);
		CHECK(GuildBankBalance("Zyco", RESOURCE_ROCK) == 135033, "first deposits: 135,033 stone credited, even dated the second the bank started");
		CHECK(GuildBankBalance("Zyco", RESOURCE_ORE) == 1852000, "first deposits: two deliveries of 926,000 ore add up to 1,852,000");
		CHECK(GuildBankAccountCount() == 1, "first deposits: one player");

		/* the bot was started later: what was delivered before is not credited */
		GuildBankReset();
		c->server_time = 1790287500;
		GuildBankReset();
		{
			char file[400];
			snprintf(file, sizeof(file), "%s/guild_bank.txt", dir);
			remove(file);
		}
		GuildBankTick(c);
		RecvResHelpReport(c, first[0], 47);
		CHECK(GuildBankAccountCount() == 0, "first deposits: a delivery dated before the bot started with the bank is not credited");
		free(c);

		GuildBankReset();
		char cleanup[64];
		snprintf(cleanup, sizeof(cleanup), "rm -rf %s", dir);
		if (system(cleanup) != 0) { /* best effort */ }
	}

	/* ---- autotrain: priority order, per-kind independence, completion, refusal ---- */
	{
		c = fresh("boss");
		c->troop.loaded = true;
		c->troop.ranged[TIER_T2] = 1000;
		c->troop.infantry[TIER_T2] = 500;
		c->autotrain.enabled = true;
		c->autotrain.target_count = 2;
		c->autotrain.targets[0] = (AutoTrainTarget){ .kind = TROOP_RANGED, .tier = TIER_T2, .cap = 5000 };
		c->autotrain.targets[1] = (AutoTrainTarget){ .kind = TROOP_INFANTRY, .tier = TIER_T2, .cap = 2000 };
		reset_sent();

		AutoTrainTick(c);
		int k = find_packet(_MSG_REQUEST_TRAINING_);
		uint32_t amount = k >= 0 ? (uint32_t)(sent[k][10] | sent[k][11] << 8 | sent[k][12] << 16 | sent[k][13] << 24) : 0;
		CHECK(k >= 0 && sent[k][8] == TROOP_RANGED && sent[k][9] == TIER_T2 && amount == 4000,
			"autotrain: first tick trains the missing amount for the top-priority target");
		CHECK(c->autotrain.kind_busy[TROOP_RANGED], "autotrain: ranged marked busy after sending its order");

		reset_sent();
		AutoTrainTick(c); // ranged already busy, and the pacing delay has not elapsed yet
		CHECK(sent_count == 0, "autotrain: no new order while the pacing delay has not elapsed");

		c->autotrain.next_action_at = 0; // simulate the pacing delay having elapsed
		reset_sent();
		AutoTrainTick(c);
		k = find_packet(_MSG_REQUEST_TRAINING_);
		amount = k >= 0 ? (uint32_t)(sent[k][10] | sent[k][11] << 8 | sent[k][12] << 16 | sent[k][13] << 24) : 0;
		CHECK(k >= 0 && sent[k][8] == TROOP_INFANTRY && sent[k][9] == TIER_T2 && amount == 1500,
			"autotrain: ranged being busy does not block infantry's own target");

		// training completes for ranged: RecvAddSoldier reports it, frees the kind, credits the count
		uint8_t addsoldier[6] = { TROOP_RANGED, TIER_T2, 0xA0, 0x0F, 0x00, 0x00 }; // 4000 LE
		RecvAddSoldier(c, addsoldier, sizeof(addsoldier));
		CHECK(c->troop.ranged[TIER_T2] == 5000, "autotrain: RecvAddSoldier credits the trained amount");
		CHECK(!c->autotrain.kind_busy[TROOP_RANGED], "autotrain: RecvAddSoldier frees the kind's queue");

		c->autotrain.next_action_at = 0;
		reset_sent();
		AutoTrainTick(c);
		k = find_packet(_MSG_REQUEST_TRAINING_);
		CHECK(k < 0 || sent[k][8] != TROOP_RANGED, "autotrain: a target already at its cap is not retrained");

		// refusal: RecvTrainingStart with a non-zero status frees the kind and arms a backoff
		c->autotrain.kind_busy[TROOP_INFANTRY] = true;
		uint8_t refuse[2] = { 3, TROOP_INFANTRY };
		RecvTrainingStart(c, refuse, sizeof(refuse));
		uint64_t short_backoff = c->autotrain.kind_retry_at[TROOP_INFANTRY];
		CHECK(!c->autotrain.kind_busy[TROOP_INFANTRY] && short_backoff > 0,
			"autotrain: a refusal frees the kind and arms a backoff");
		CHECK(short_backoff < now_ms() + 5 * 60 * 1000,
			"autotrain: the first refusals use a short backoff (assumed transient)");

		// a target that is never going to succeed (e.g. the research for that tier is not
		// done) must not be hammered forever at the short backoff - after enough refusals
		// in a row the backoff grows a lot, without ever giving up on it for the whole run
		for (int i = 0; i < AUTOTRAIN_HARD_BLOCK_THRESHOLD - 1; i++)
			RecvTrainingStart(c, refuse, sizeof(refuse));
		uint64_t long_backoff = c->autotrain.kind_retry_at[TROOP_INFANTRY];
		CHECK(long_backoff > now_ms() + 5 * 60 * 1000,
			"autotrain: enough consecutive refusals switch to a long backoff instead of retrying every minute forever");

		// an accepted order resets the streak, so a later real transient refusal is not
		// immediately treated as another hard block
		uint8_t accept[2] = { 0, TROOP_INFANTRY };
		RecvTrainingStart(c, accept, sizeof(accept));
		CHECK(c->autotrain.kind_consecutive_refusals[TROOP_INFANTRY] == 0,
			"autotrain: an accepted order resets the refusal streak");

		free(c);
	}

	printf("%s\n", failures ? "SOME TESTS FAILED" : "ALL BOT LOGIC TESTS PASSED");
	return failures ? 1 : 0;
}
