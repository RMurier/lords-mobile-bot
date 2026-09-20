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
	CHECK(c->transfer.not_before > time(NULL), "the transfer waits for the resources to be credited");
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

		packet[o++] = 1;                               /* b2: alliance chat */
		packet[o++] = 1; packet[o++] = 0;              /* one message */
		o += 24;                                       /* three u64 */
		packet[o++] = 0; packet[o++] = 0;              /* alli_or_king, num8 = plain text */
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
	CHECK(replied("$relocate random|<x> <y> - déplacer le château") && replied("$confirm / $cancel - valider ou annuler") && replied("gérer les administrateurs")
		&& replied("solde de la banque, du sac et total"), "help for an administrator lists the new commands in French");
	reset_sent();
	say(c, "boss", "$aide", COMMAND_CHANNEL_MAIL);
	CHECK(sent_count == 0, "French command names do not exist: only the texts are French");
	say(c, "eve", "$stop", COMMAND_CHANNEL_MAIL);
	CHECK(replied("Aucune livraison en cours."), "replies are in French");
	free(c);

	/* ---- relocation: administrators only, always confirmed ---------------- */
	{
		map_pos_t home = { 200, 200 };
		uint16_t hz; uint8_t hp;
		MapPosToPointCode(home, &hz, &hp);

		c = fresh("boss,alice");
		c->items_loaded = true;
		c->player.zone_id = hz; c->player.point_id = hp; c->player.current_kingdom_id = 42;

		say(c, "eve", "$relocate random", COMMAND_CHANNEL_MAIL);
		CHECK(c->pending.kind == PENDING_NONE && replied("Seuls les administrateurs"), "relocation refused for a stranger");
		reset_sent();
		say(c, "boss", "$relocate random", COMMAND_CHANNEL_MAIL);
		CHECK(c->pending.kind == PENDING_NONE && replied("aucun relocalisateur aléatoire"), "no random relocator in the bag: refused");
		reset_sent();

		c->items[RANDOM_RELOCATOR].quantity = 1;
		say(c, "boss", "$relocate random", COMMAND_CHANNEL_MAIL);
		CHECK(c->pending.kind == PENDING_RELOCATE_RANDOM && replied("Confirmez avec $confirm dans les 60 secondes, ou $cancel") && useitem_count == 0,
			"random relocation asks for a confirmation and sends nothing yet");
		reset_sent();
		say(c, "eve", "$confirm", COMMAND_CHANNEL_MAIL);
		CHECK(c->pending.kind == PENDING_RELOCATE_RANDOM && useitem_count == 0, "a stranger cannot confirm");
		say(c, "alice", "$confirm", COMMAND_CHANNEL_MAIL);
		CHECK(c->pending.kind == PENDING_RELOCATE_RANDOM && useitem_count == 0 && replied("demandée par boss"), "another administrator cannot confirm someone else's action");
		reset_sent();
		say(c, "boss", "$confirm", COMMAND_CHANNEL_MAIL);
		CHECK(useitem_count == 1 && c->pending.kind == PENDING_NONE, "the requester confirms: the relocator is used");
		CHECK((uint16_t)(sent[0][8] | (sent[0][9] << 8)) == RANDOM_RELOCATOR, "the packet uses the random relocator item");
		reset_sent();
		say(c, "boss", "$confirm", COMMAND_CHANNEL_MAIL);
		CHECK(useitem_count == 0 && replied("Aucune action en attente"), "a confirmation cannot be replayed");

		/* the server answers: item 1004/1003, quantity, unknown, zone, point, kingdom */
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

		/* expiry and cancel (the server's answer above set the quantity of relocators to 0) */
		c->items[RANDOM_RELOCATOR].quantity = 1;
		say(c, "boss", "$relocate random", COMMAND_CHANNEL_MAIL);
		reset_sent();
		c->pending.expires = time(NULL) - 1;
		say(c, "boss", "$confirm", COMMAND_CHANNEL_MAIL);
		CHECK(useitem_count == 0 && c->pending.kind == PENDING_NONE && replied("60 secondes est dépassé"), "an expired confirmation is refused");
		c->items[RANDOM_RELOCATOR].quantity = 1;
		say(c, "boss", "$relocate random", COMMAND_CHANNEL_MAIL);
		reset_sent();
		say(c, "eve", "$cancel", COMMAND_CHANNEL_MAIL);
		CHECK(c->pending.kind == PENDING_RELOCATE_RANDOM, "a stranger cannot cancel");
		say(c, "alice", "$cancel", COMMAND_CHANNEL_MAIL);
		CHECK(c->pending.kind == PENDING_NONE && replied("Action annulée"), "an administrator can cancel");

		/* relocation to coordinates with the advanced relocator */
		reset_sent();
		say(c, "boss", "$relocate 100 100", COMMAND_CHANNEL_MAIL);
		CHECK(c->pending.kind == PENDING_NONE && replied("aucun relocalisateur avancé"), "no advanced relocator: refused");
		c->items[ADVANCE_RELOCATOR].quantity = 2;
		reset_sent();
		say(c, "boss", "$relocate 99999 5", COMMAND_CHANNEL_MAIL);
		CHECK(c->pending.kind == PENDING_NONE && replied("Coordonnées invalides"), "coordinates outside the map are refused");
		reset_sent();
		say(c, "boss", "$relocate abc", COMMAND_CHANNEL_MAIL);
		CHECK(replied("Usage : $relocate random | $relocate <x> <y>"), "wrong arguments show the usage");
		reset_sent();
		c->player.zone_id = hz; c->player.point_id = hp;
		say(c, "boss", "$relocate 200 200", COMMAND_CHANNEL_MAIL);
		CHECK(c->pending.kind == PENDING_NONE && replied("déjà en X:200 Y:200"), "already there: refused");
		reset_sent();
		say(c, "boss", "$relocate 100 100", COMMAND_CHANNEL_MAIL);
		CHECK(c->pending.kind == PENDING_RELOCATE_TO && replied("X:100 Y:100 (royaume 42)") && useitem_count == 0, "coordinates relocation asks for a confirmation");
		reset_sent();
		say(c, "boss", "$confirm", COMMAND_CHANNEL_MAIL);
		{
			map_pos_t target = { 100, 100 };
			uint16_t tz; uint8_t tp;
			MapPosToPointCode(target, &tz, &tp);
			const uint8_t *pk = sent[0];
			CHECK(useitem_count == 1 && (uint16_t)(pk[8] | (pk[9] << 8)) == ADVANCE_RELOCATOR, "the advanced relocator is used");
			CHECK((uint16_t)(pk[12] | (pk[13] << 8)) == 42 && (uint16_t)(pk[14] | (pk[15] << 8)) == tz && pk[16] == tp,
				"the packet carries the current kingdom and the target zone and point");
		}
		/* the server refuses */
		reset_sent();
		{
			uint8_t refused[2] = { 5, 0 };
			RecvUseItem(c, refused, sizeof(refused));
			CHECK(replied("échoué (code 5)"), "a refusal by the server is reported");
			c->pending.report_to[0] = '\0';
		}
		reset_sent();
		{
			uint8_t refused[2] = { 5, 0 };
			c->pending.report_until = time(NULL) - 1;
			snprintf(c->pending.report_to, sizeof(c->pending.report_to), "boss");
			RecvUseItem(c, refused, sizeof(refused));
			CHECK(sent_count == 0, "a late answer is not reported");
		}
		free(c);
	}

	printf("%s\n", failures ? "SOME TESTS FAILED" : "ALL BOT LOGIC TESTS PASSED");
	return failures ? 1 : 0;
}
