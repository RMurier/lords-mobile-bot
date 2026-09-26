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
 *       src/log.c src/protocol.c src/des.c src/map_point.c src/command.c src/config.c src/guildbank.c src/research_table.c src/building_table.c
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

/* enough of every resource that an autotrain order is never cut down by the stock */
static void give_stock(Connection *c) { c->resources.food = c->resources.rock = c->resources.wood = c->resources.ore = 2000000000u; c->resources.gold = 2000000000u; }

static void reset_sent(void) { sent_count = 0; useitem_count = 0; }

static int failures = 0;
#define CHECK(cond, msg) do { if (cond) printf("PASS %s\n", msg); else { printf("FAIL %s\n", msg); failures++; } } while (0)

static Connection *fresh(const char *admins)
{
	Connection *c = calloc(1, sizeof(*c));
	c->sock = -1;
	c->gather.pending_tile = GATHER_NO_PENDING_TILE;
	c->gather.kind_priority[0] = TROOP_INFANTRY; // matches LoadConfig's default fallback chain
	c->gather.kind_priority[1] = TROOP_RANGED;
	c->gather.kind_priority[2] = TROOP_CAVALRY;
	c->gather.kind_priority[3] = TROOP_SIEGE;
	c->gather.kind_priority_count = 4;
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

	/* ---- black market: use_bag_rss must not drain the whole bag ------
	 * A live bug: RecvUseItem never credited c->resources.* for a plain resource item, so
	 * CanAffordMarketItem never became true after a bag top-up - EvaluateBlackMarket kept
	 * retrying every 3s (BlackMarketTick) and BagApply kept "topping up" the same unmet need,
	 * spending a little more of the bag each time, until a 2B wood bag was fully emptied
	 * without ever completing the trade. BagApply now credits the resource itself. */
	{
		c = fresh("boss");
		c->market.loaded = true;
		c->market.settings.auto_trade = true;
		c->market.settings.use_bag_rss = true;
		c->market.settings.spend_wood = true;
		c->market.items[0].resource_kind = RESOURCE_WOOD;
		c->market.items[0].resource_count = 5000000;
		c->items[TIMBER_5M].quantity = 2; // 10M available, only 5M needed: must not use both

		reset_sent();
		EvaluateBlackMarket(c);
		CHECK(find_packet(_MSG_REQUEST_BLACKMARKET_BUY) < 0 && c->market_bag_wait > 0,
			"market: not affordable yet, tops up from the bag and waits instead of buying");
		CHECK(c->resources.wood == 5000000 && c->items[TIMBER_5M].quantity == 1,
			"market: BagApply credits the resource immediately, using only the one item needed");
		CHECK(c->market.bag_topup_attempts == 1, "market: one top-up attempt recorded");

		c->market_bag_wait = 0; // simulate BlackMarketTick's 3s wait having elapsed
		reset_sent();
		EvaluateBlackMarket(c);
		CHECK(find_packet(_MSG_REQUEST_BLACKMARKET_BUY) >= 0 && c->market.buy_pending,
			"market: now affordable, buys instead of topping up from the bag again");
		CHECK(c->items[TIMBER_5M].quantity == 1 && c->market.bag_topup_attempts == 1,
			"market: the trade completed without ever touching the second wood item");
		free(c);
	}

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

	/* ---- multi-march pacing: the 2nd+ march must wait too, not just the 1st ----------
	 * Also re-finds the target (RequestAllyPoint) before every march after the first, instead of
	 * reusing the cached zone/point - a live delivery was kicked by the server (no error packet)
	 * right after the 2nd march every time, regardless of the gap between marches, and this was
	 * the one structural difference left between the successful 1st march and every one after it. */
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
		CHECK(c->transfer.state == TRANSFER_FIND_TARGET && c->transfer.not_before > now_ms()
			&& find_packet(_MSG_REQUEST_ALLYPOINT) < 0,
			"a march accepted (RecvSHelp) waits, then re-finds the target before the next one (not sent yet: not_before has not elapsed)");

		c->transfer.state = TRANSFER_WAIT_MARCH;
		c->transfer.not_before = 0;
		c->player.current_marches = 1;
		c->transfer.remaining = 500000;

		reset_sent();
		/* home[0] = b = 0 (success), then food/rock/wood/ore/gold stocks (4 bytes each, unused here) */
		RecvHelp_Home(c, home);
		CHECK(c->transfer.state == TRANSFER_FIND_TARGET && c->transfer.not_before > now_ms(),
			"a march returning home (RecvHelp_Home) also waits, then re-finds the target");

		/* once the wait is over, the tick re-sends RequestAllyPoint exactly like the first march did */
		c->transfer.not_before = 0;
		reset_sent();
		ResourceTransferTick(c);
		CHECK(c->transfer.state == TRANSFER_WAIT_TARGET && find_packet(_MSG_REQUEST_ALLYPOINT) >= 0,
			"once the wait elapses, the next march looks the target up again, the same way the first one did");

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

		/* code 27 of a scroll: impossible during the RvR (KvK), as the game says (told by the account's owner) */
		say(c, "boss", "$migrate 796 301 491", COMMAND_CHANNEL_MAIL);
		reset_sent();
		{ uint8_t toc[21] = { 0, 0x2c, 0x2a, 0, 0, '1', 0 }; RecvKingdomServer(c, toc, sizeof(toc)); }
		reset_sent();
		{ uint8_t kvk[3] = { 27, 0xfb, 0x04 }; RecvUseItem(c, kvk, sizeof(kvk)); }
		CHECK(c->migration.state == MIGRATION_IDLE && replied("code 27) : impossible pendant le RvR (KvK)"), "a scroll refused with code 27 says it is impossible during the RvR (KvK)");

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
		/* the game's own tables (research_table.h): every id 1..403, all 16 categories */
		CHECK(RESEARCH_TECH_COUNT == 403 && RESEARCH_KIND_COUNT == 16, "research table: 403 researches in 16 categories");
		CHECK(ResearchKnownName(303) != NULL && strcmp(ResearchKnownName(303), "Gear: Bigger Bags III") == 0
			&& strcmp(ResearchKnownName(126), "Army Leadership: Tax Break") == 0 && ResearchKnownName(404) == NULL,
			"research table: #303 is Gear's Bigger Bags III, #126 is Army Leadership's Tax Break (was only inferred)");
		{
			static const struct { uint16_t id; const char *en; } KNOWN[] = {
				{6,"Construction Speed"},{8,"Gem Harvesting I"},{54,"Fire Trebuchet"},{74,"Energy Recovery I"},{123,"More Gatherers"},
				{125,"Bigger Bags I"},{143,"Gold Storage I"},{228,"Wonder March I"},{229,"Gem Harvesting II"},{234,"Bigger Bags II"},
				{299,"Barracks Expansion II"},{301,"Ration Run IV"},{302,"Forced March III"},{305,"Quick Maneuvers III"},
			};
			bool all = true;
			for (size_t i = 0; i < sizeof(KNOWN) / sizeof(KNOWN[0]); i++)
				if (strcmp(ResearchTech(KNOWN[i].id)->name_en, KNOWN[i].en) != 0)
					all = false;
			CHECK(all, "research table: the 14 ids confirmed in the game's UI carry the names it showed");
		}
		{
			const ResearchTechInfo *tb = ResearchTech(126);
			const ResearchLevelInfo *l1 = ResearchLevelRow(tb, 1);
			CHECK(tb->max_level == 10 && l1 && l1->academy == 22 && l1->req_id[0] == 142 && l1->req_level[0] == 3
				&& ResearchLevelRow(tb, 11) == NULL && ResearchKind(tb->kind)->order == 7,
				"research table: Tax Break has 10 levels, level 1 needs Academy 22 and #142 at level 3");
			uint32_t rows = 0;
			for (uint16_t i = 1; i <= RESEARCH_TECH_COUNT; i++)
				rows += ResearchTech(i)->max_level;
			CHECK(rows == 3425, "research table: one level row per level of every research");
		}

		/* the plain start, captured from the PC client: #221 (Furious Defense, Ranged) level 8 -> 9, no item used */
		{
			static const uint8_t req_start_plain_l9[] = { 0x1a, 0x00, 0x00, 0x00, 0xdd, 0x00, 0x09, 0x00, 0x00, 0x00 };
			static const uint8_t resp_start_plain_l9[] = {
				0x00, 0xdd, 0x00, 0x09, 0x90, 0xb0, 0xb7, 0x6a, 0x00, 0x00, 0x00, 0x00, 0x9c, 0x21, 0x0e, 0x00,
				0x8d, 0xba, 0x98, 0xc0, 0x1b, 0xea, 0xe4, 0xe3, 0xac, 0xdf, 0x24, 0xed, 0xbb, 0x12, 0x68, 0xe7,
				0x25, 0xde, 0x5f, 0x74, 0x9d, 0xc0, 0x4e, 0x00, 0x68, 0x87, 0x01, 0x00 };
			Connection *pc = fresh("boss");
			reset_sent();
			pc->protocol.seq_id = 0x19;
			RequestResearchStartPlain(pc, 221, 9);
			int at = find_packet(3202);
			CHECK(at >= 0 && sent_size[at] == 4 + sizeof(req_start_plain_l9)
				&& memcmp(sent[at] + 4, req_start_plain_l9, sizeof(req_start_plain_l9)) == 0,
				"research: the plain start (3202) is byte for byte the game's, no item needed");
			RecvResearchStart(pc, resp_start_plain_l9, sizeof(resp_start_plain_l9));
			CHECK(pc->research.in_progress == 221 && pc->research.total_time == 926108 && pc->research.start_time == 0x6ab7b090,
				"research: the answer to a plain start is read like the smart-use one (926108s, 10.7 days)");
			CHECK(strcmp(ResearchKnownName(221), "Sigils: Furious Defense (Ranged)") == 0, "research table: #221 is Furious Defense (Ranged)");
			free(pc);
		}

		/* choosing the next research to start */
		{
			Connection *pc = fresh("boss");
			ResearchPick pick;
			char why[160];
			pc->building_count = 1;
			pc->building[0].build_id = BUILDING_ACADEMY;
			pc->building[0].level = 30;
			pc->resources.food = pc->resources.rock = pc->resources.wood = pc->resources.ore = pc->resources.gold = 1000000000LL;
			uint16_t id = ResearchPickNext(pc, 1, 0, &pick, why, sizeof(why), false);
			const ResearchLevelInfo *row = id ? ResearchLevelRow(ResearchTech(id), pick.level) : NULL;
			bool req_ok = row != NULL;
			for (int r = 0; row && r < 4; r++)
				if (row->req_id[r] && ResearchLevel(&pc->research, row->req_id[r]) < row->req_level[r])
					req_ok = false;
			CHECK(id != 0 && pick.level == 1 && req_ok, "research pick: a first level whose prerequisites are met");

			pc->building[0].level = 0;
			CHECK(ResearchPickNext(pc, 1, 0, &pick, why, sizeof(why), false) == 0 && strstr(why, "Acad") != NULL,
				"research pick: Academy too low, and it says so");
			pc->building[0].level = 30;
			pc->resources.gold = 0;
			CHECK(ResearchPickNext(pc, 1, 0, &pick, why, sizeof(why), false) == 0 && strstr(why, "Ressources") != NULL,
				"research pick: not enough resources, and it says so");
			pc->resources.gold = 1000000000LL;

			/* a research whose prerequisite is missing: the prerequisite is what gets started, for that research */
			id = ResearchPickNext(pc, 0, 126, &pick, why, sizeof(why), false);
			row = id ? ResearchLevelRow(ResearchTech(id), pick.level) : NULL;
			req_ok = row != NULL;
			for (int r = 0; row && r < 4; r++)
				if (row->req_id[r] && ResearchLevel(&pc->research, row->req_id[r]) < row->req_level[r])
					req_ok = false;
			CHECK(id != 0 && id != 126 && pick.for_id == 126 && req_ok,
				"research pick: #126 needs #142 at level 3, so a prerequisite is started first, for #126");

			/* the whole game, category by category, from nothing: the chain always resolves (no dead end, no loop),
			 * and every step started has its prerequisites at their level when it is started */
			int kinds_done = 0, invalid = 0, stuck = 0;
			for (uint16_t k = 0; k < RESEARCH_KIND_COUNT; k++) {
				Connection *gc = fresh("boss");
				gc->building_count = 1;
				gc->building[0].build_id = BUILDING_ACADEMY;
				gc->building[0].level = BUILDING_MAX_LEVEL; /* the mana levels count: the last researches ask for Academy 55 */
				gc->resources.food = gc->resources.rock = gc->resources.wood = gc->resources.ore = gc->resources.gold = 4000000000LL;
				uint8_t kind = RESEARCH_KINDS[k].kind;
				int guard = 0;
				while (ResearchPickNext(gc, kind, 0, &pick, why, sizeof(why), false) && guard++ < 5000) {
					const ResearchLevelInfo *step_row = ResearchLevelRow(ResearchTech(pick.id), pick.level);
					for (int r = 0; r < 4; r++)
						if (step_row->req_id[r] && ResearchLevel(&gc->research, step_row->req_id[r]) < step_row->req_level[r])
							invalid++;
					if (ResearchLevel(&gc->research, pick.id) + 1 != pick.level)
						invalid++;
					ResearchSetLevel(&gc->research, pick.id, pick.level);
				}
				bool finished = strstr(why, "maximum") != NULL;
				for (uint16_t i = 1; i <= RESEARCH_TECH_COUNT; i++)
					if (ResearchTech(i)->kind == kind && !ResearchTech(i)->locked && ResearchLevel(&gc->research, i) < ResearchTech(i)->max_level)
						finished = false;
				if (finished) kinds_done++; else stuck++;
				free(gc);
			}
			CHECK(kinds_done == 16 && stuck == 0 && invalid == 0,
				"research pick: every category can be taken to its maximum, prerequisites first, each step valid when started");

			for (uint16_t i = 1; i <= RESEARCH_TECH_COUNT; i++)
				if (ResearchTech(i)->kind == 1)
					ResearchSetLevel(&pc->research, i, ResearchTech(i)->max_level);
			CHECK(ResearchPickNext(pc, 1, 0, &pick, why, sizeof(why), false) == 0 && strstr(why, "maximum") != NULL,
				"research pick: everything at its maximum, and it says so");

			/* the command itself */
			pc->research.loaded = true;
			for (uint16_t i = 1; i <= RESEARCH_TECH_COUNT; i++)
				ResearchSetLevel(&pc->research, i, 0);
			reset_sent();
			say(pc, "eve", "$research start economie", COMMAND_CHANNEL_MAIL);
			CHECK(find_packet(3202) < 0, "$research start: refused to a stranger");
			say(pc, "boss", "$research start economie", COMMAND_CHANNEL_MAIL);
			CHECK(find_packet(3202) >= 0 && replied("Lancement demand"), "$research start <category>: sends the plain start");
			pc->research.in_progress = 126;
			reset_sent();
			say(pc, "boss", "$research start economie", COMMAND_CHANNEL_MAIL);
			CHECK(find_packet(3202) < 0 && replied("d\xc3\xa9j\xc3\xa0 en cours"), "$research start: nothing sent while a research is running");
			pc->research.in_progress = 0;
			reset_sent();
			say(pc, "boss", "$research start zzz", COMMAND_CHANNEL_MAIL);
			CHECK(find_packet(3202) < 0, "$research start: unknown category sends nothing");
			free(pc);
		}

		/* every category name the console writes, and the French ones, designates exactly its own category */
		{
			static const char *EN[] = { "Economy", "Defense", "Military", "Monster Hunt", "Upgrade Defenses", "Upgrade Military",
				"Army Leadership", "Military Command", "Familiars", "Familiar Battles", "Sigils", "Wonder Battles", "Gear",
				"Advanced Wonder Battles", "Mana Awakening", "Guild Duel" };
			int exact = 0, exact_fr = 0;
			for (uint8_t o = 0; o < 16; o++) {
				const ResearchKindInfo *found[RESEARCH_KIND_COUNT_MAX], *found_fr[RESEARCH_KIND_COUNT_MAX];
				size_t n = ResearchFindKinds(EN[o], found, RESEARCH_KIND_COUNT_MAX);
				if (n == 1 && found[0]->order == o + 1) exact++;
				for (uint16_t i = 0; i < RESEARCH_KIND_COUNT; i++)
					if (RESEARCH_KINDS[i].order == o + 1) {
						size_t nf = ResearchFindKinds(RESEARCH_KINDS[i].name_fr, found_fr, RESEARCH_KIND_COUNT_MAX);
						if (nf == 1 && found_fr[0]->order == o + 1) exact_fr++;
					}
			}
			CHECK(exact == 16 && exact_fr == 16, "research categories: each full name (English as the console writes it, French) finds exactly its own category");
		}

		/* automatic research: configuration */
		{
			char dir[] = "/tmp/lmbot-rs-XXXXXX";
			mkdtemp(dir);
			char cfg[300];
			snprintf(cfg, sizeof(cfg), "%s/r.cfg", dir);
			FILE *fp = fopen(cfg, "w");
			fprintf(fp, "research.enabled = true\nresearch.categories = Sceaux, gear , 1, sigils\nresearch.reserve_gold = 5M\ndata.path = %s/data/\n", dir);
			fclose(fp);
			Connection *rc = calloc(1, sizeof(*rc));
			bool ok = LoadConfig(rc, cfg);
			CHECK(ok && rc->research_auto.enabled && rc->research_auto.kind_count == 3
				&& rc->research_auto.kinds[0] == 12 && rc->research_auto.kinds[1] == 15 && rc->research_auto.kinds[2] == 1
				&& rc->research_auto.reserve.gold == 5000000,
				"research config: categories by name/number in priority order, duplicates dropped, reserve read");
			free(rc);
			fp = fopen(cfg, "w"); fprintf(fp, "research.categories = Sceaux, zzz\n"); fclose(fp);
			rc = calloc(1, sizeof(*rc));
			CHECK(!LoadConfig(rc, cfg), "research config: an unknown category is a configuration error");
			free(rc);
			fp = fopen(cfg, "w"); fprintf(fp, "research.categories = batailles\n"); fclose(fp);
			rc = calloc(1, sizeof(*rc));
			CHECK(!LoadConfig(rc, cfg), "research config: a name matching several categories is a configuration error");
			free(rc);
		}

		/* automatic research: behaviour */
		{
			Connection *rc = fresh("boss");
			rc->research.loaded = true;
			rc->research_auto.enabled = true;
			rc->research_auto.kinds[0] = 1;   /* Economy */
			rc->research_auto.kind_count = 1;
			rc->building_count = 1;
			rc->building[0].build_id = BUILDING_ACADEMY;
			rc->building[0].level = 30;
			rc->resources.food = rc->resources.rock = rc->resources.wood = rc->resources.ore = rc->resources.gold = 1000000000LL;

			reset_sent();
			ResearchAutoTick(rc);
			CHECK(find_packet(3202) < 0 && rc->research_auto.next_check_at != 0, "research auto: first look only arms the pause, nothing sent");
			ResearchAutoTick(rc);
			CHECK(find_packet(3202) < 0, "research auto: nothing sent before the pause is over");
			rc->research_auto.next_check_at = 1;
			ResearchAutoTick(rc);
			int at = find_packet(3202);
			uint16_t first = at >= 0 ? (uint16_t)(sent[at][8] | sent[at][9] << 8) : 0;
			CHECK(at >= 0 && rc->research_auto.pending && ResearchTech(first) && ResearchTech(first)->kind == 1 && sent[at][10] == 1,
				"research auto: starts a research of the configured category, level 1");
			reset_sent();
			rc->research_auto.next_check_at = 1;
			ResearchAutoTick(rc);
			CHECK(find_packet(3202) < 0, "research auto: never a second start while the first one is unanswered");

			/* refused: that research is left alone, the next look picks another one */
			uint8_t refused[44] = { 5 };
			RecvResearchStart(rc, refused, sizeof(refused));
			CHECK(!rc->research_auto.pending && rc->research_auto.retry_at[first] > now_ms() && strstr(rc->research_auto.state, "refus") != NULL,
				"research auto: a refused start is remembered and the research left alone");
			rc->research_auto.next_check_at = 1;
			reset_sent();
			ResearchAutoTick(rc);
			at = find_packet(3202);
			uint16_t second = at >= 0 ? (uint16_t)(sent[at][8] | sent[at][9] << 8) : 0;
			CHECK(at >= 0 && second != first, "research auto: after a refusal it tries another research, not the same one");

			/* accepted: nothing more while it runs */
			uint8_t okresp[44] = { 0, (uint8_t)second, (uint8_t)(second >> 8), 1, 0x90, 0xb0, 0xb7, 0x6a, 0, 0, 0, 0, 0x10, 0x0e, 0, 0 };
			RecvResearchStart(rc, okresp, sizeof(okresp));
			reset_sent();
			rc->research_auto.next_check_at = 1;
			ResearchAutoTick(rc);
			CHECK(rc->research.in_progress == second && find_packet(3202) < 0 && strstr(rc->research_auto.state, "En cours") != NULL,
				"research auto: nothing sent while a research is running");

			/* it finishes: looks again after a pause */
			uint8_t done[3] = { (uint8_t)second, (uint8_t)(second >> 8), 1 };
			RecvResearchComplete(rc, done, sizeof(done));
			CHECK(!ResearchInProgress(&rc->research) && rc->research_auto.next_check_at == 0, "research auto: a completion re-arms the pause");

			/* the reserve is never spent */
			rc->research_auto.next_check_at = 1;
			rc->research_auto.reserve.gold = 1000000000u;
			for (int i = 0; i <= RESEARCH_ID_MAX; i++) rc->research_auto.retry_at[i] = 0;
			reset_sent();
			ResearchAutoTick(rc);
			rc->research_auto.next_check_at = 1;
			ResearchAutoTick(rc);
			CHECK(find_packet(3202) < 0 && strstr(rc->research_auto.state, "Ressources") != NULL,
				"research auto: nothing started when the reserve leaves too little, and the state says why");

			/* disabled, or no category */
			rc->research_auto.reserve.gold = 0;
			rc->research_auto.enabled = false;
			rc->research_auto.next_check_at = 1;
			reset_sent();
			ResearchAutoTick(rc);
			CHECK(find_packet(3202) < 0, "research auto: disabled sends nothing");
			free(rc);
		}

		/* automatic construction: names, configuration */
		{
			int unique = 0, selectable = 0, unique_fr = 0;
			for (uint16_t i = 0; i < BUILDING_TYPE_COUNT; i++) {
				const BuildingTypeInfo *t = &BUILDING_TYPES[i], *found[8];
				if (!BuildingSelectable(t)) continue;
				selectable++;
				if (BuildFindTypes(t->name_en, found, 8) == 1 && found[0] == t) unique++;
				if (BuildFindTypes(t->name_fr, found, 8) == 1 && found[0] == t) unique_fr++;
			}
			CHECK(selectable == 26 && unique == selectable && unique_fr == selectable,
				"build: the 26 buildings the automatic construction can work on each have a full name (English, French) that finds only themselves");
			const BuildingTypeInfo *f[8];
			CHECK(BuildFindTypes("Castle", f, 8) == 1 && f[0]->id == 8 && BuildFindTypes("wall", f, 8) == 1 && f[0]->id == 12
				&& BuildFindTypes("8", f, 8) == 1 && f[0]->id == 8 && BuildFindTypes("Residence", f, 8) == 0,
				"build: \"Castle\" is the Castle (not the Castle Wall), a part of a name or a build_id works, special buildings are refused");

			char dir[] = "/tmp/lmbot-bd-XXXXXX";
			mkdtemp(dir);
			char cfg[300];
			snprintf(cfg, sizeof(cfg), "%s/b.cfg", dir);
			FILE *fp = fopen(cfg, "w");
			fprintf(fp, "build.enabled = true\nbuild.buildings = Castle, barracks , Castle Wall, castle\nbuild.reserve_wood = 2M\ndata.path = %s/data/\n", dir);
			fclose(fp);
			Connection *bc = calloc(1, sizeof(*bc));
			bool ok = LoadConfig(bc, cfg);
			CHECK(ok && bc->build_auto.enabled && bc->build_auto.type_count == 3 && bc->build_auto.types[0] == 8
				&& bc->build_auto.types[1] == 6 && bc->build_auto.types[2] == 12 && bc->build_auto.reserve.wood == 2000000,
				"build config: buildings by name in priority order, duplicates dropped, reserve read");
			free(bc);
			fp = fopen(cfg, "w"); fprintf(fp, "build.buildings = Castle, Nonexistent\n"); fclose(fp);
			bc = calloc(1, sizeof(*bc));
			CHECK(!LoadConfig(bc, cfg), "build config: an unknown building is a configuration error");
			free(bc);
		}

		/* automatic construction: every building from its first level to its maximum, prerequisites first */
		{
			int reached = 0, stuck = 0, invalid = 0, total = 0;
			for (uint16_t i = 0; i < BUILDING_TYPE_COUNT; i++) {
				const BuildingTypeInfo *goal_type = &BUILDING_TYPES[i];
				if (!BuildingSelectable(goal_type)) continue;
				total++;

				Connection *bc = fresh("boss");
				bc->resources.food = bc->resources.rock = bc->resources.wood = bc->resources.ore = bc->resources.gold = 4000000000LL;
				for (uint16_t r = 1; r <= RESEARCH_TECH_COUNT; r++)
					ResearchSetLevel(&bc->research, r, ResearchTech(r)->max_level); /* the research prerequisites are met */
				bc->building_count = 0;
				for (uint16_t k = 0; k < BUILDING_TYPE_COUNT; k++) {
					const BuildingTypeInfo *t = &BUILDING_TYPES[k];
					if (!BuildingSelectable(t)) continue;
					bc->building[bc->building_count].position_id = (uint16_t)(1000 + k);
					bc->building[bc->building_count].build_id = t->id;
					bc->building[bc->building_count].level = t->min_level;
					bc->building_count++;
				}
				/* a second farm: the first one (lowest slot) is the one kept at a low level for $askhelp, never upgraded */
				bc->building[bc->building_count++] = (BuildingInfo){ .position_id = 2000, .build_id = 4, .level = 1 };

				BuildPick pick;
				char why[160];
				int guard = 0;
				while (BuildPickNext(bc, goal_type->id, &pick, why, sizeof(why)) && guard++ < 20000) {
					int at = -1;
					for (int b = 0; b < bc->building_count; b++)
						if (bc->building[b].position_id == pick.slot) at = b;
					const BuildingTypeInfo *pt = BuildingType(pick.build_id);
					const BuildingLevelInfo *row = BuildingLevelRow(pt, pick.level);
					if (at < 0 || bc->building[at].level + 1 != pick.level || !row) { invalid++; break; }
					for (int q = 0; q < BUILDING_REQ_MAX; q++) {
						if (!row->req_id[q]) continue;
						int best = 0;
						for (int b = 0; b < bc->building_count; b++)
							if (bc->building[b].build_id == row->req_id[q] && bc->building[b].level > best) best = bc->building[b].level;
						if (best < row->req_level[q]) invalid++;
					}
					bc->building[at].level = pick.level;
				}
				bool finished = strstr(why, "maximum") != NULL;
				for (int b2 = 0; b2 < bc->building_count; b2++)
					if (bc->building[b2].position_id == 1003 && bc->building[b2].level != 1) invalid++; /* the kept farm stays at level 1 */
				if (finished) reached++; else { stuck++; printf("  stuck: %s: %s\n", goal_type->name_en, why); }
				free(bc);
			}
			CHECK(reached == total && stuck == 0 && invalid == 0,
				"build plan: each of the 26 buildings can be taken to its maximum from its first level, prerequisites first, each step valid when started");
		}

		/* the plain start of a construction, captured from the PC client: a farm 23 -> 24 (slot 62162), then another (slot 62668) */
		{
			static const uint8_t req_build_1[] = { 0x14, 0x00, 0x00, 0x00, 0xd2, 0xf2, 0x04, 0x00, 0x02 };
			static const uint8_t req_build_2[] = { 0x1e, 0x00, 0x00, 0x00, 0xcc, 0xf4, 0x04, 0x00, 0x02 };
			static const uint8_t resp_begin_1[] = {
				0xd2, 0xf2, 0x04, 0x00, 0x18, 0xf8, 0xbd, 0xb7, 0x6a, 0x00, 0x00, 0x00, 0x00, 0xff, 0x55, 0x02, 0x00, 0x4d, 0xf2, 0x8e, 0xbd,
				0xe1, 0xcd, 0xcc, 0xe3, 0x72, 0xc3, 0x0c, 0xed, 0x81, 0xf6, 0x4f, 0xe7, 0x25, 0xde, 0x5f, 0x74, 0x00, 0x00, 0x00 };
			static const uint8_t resp_complete_1[] = { 0xd2, 0xf2, 0x04, 0x00, 0x18, 0x00 };
			Connection *bc = fresh("boss");
			reset_sent();
			bc->protocol.seq_id = 0x13;
			RequestBuildStart(bc, 62162, 4);
			int at = find_packet(2003);
			CHECK(at >= 0 && sent_size[at] == 4 + sizeof(req_build_1) && memcmp(sent[at] + 4, req_build_1, sizeof(req_build_1)) == 0,
				"build: the start request is byte for byte the game's (farm, slot 62162)");
			reset_sent();
			bc->protocol.seq_id = 0x1d;
			RequestBuildStart(bc, 62668, 4);
			at = find_packet(2003);
			CHECK(at >= 0 && memcmp(sent[at] + 4, req_build_2, sizeof(req_build_2)) == 0, "build: the second captured start is byte for byte the game's too");

			bc->building_count = 1;
			bc->building[0] = (BuildingInfo){ .position_id = 62162, .build_id = 4, .level = 23 };
			RecvBuildBegin(bc, resp_begin_1, sizeof(resp_begin_1));
			CHECK(bc->construction[0].used && bc->construction[0].slot == 62162 && bc->construction[0].build_id == 4
				&& bc->construction[0].level == 24 && bc->construction[0].start_time == 0x6ab7bdf8 && bc->construction[0].duration == 153087,
				"build: the answer to a start fills the queue (farm to level 24, 153087 s)");
			RecvBuildComplete(bc, resp_complete_1, sizeof(resp_complete_1));
			CHECK(bc->building[0].level == 24 && !bc->construction[0].used, "build: a completion raises the building's level and frees the queue");
			free(bc);
		}

		/* automatic construction: it starts what it plans, one start at a time, and follows the answers */
		{
			Connection *bc = fresh("boss");
			bc->build_auto.enabled = true;
			bc->build_auto.types[0] = 6; /* Barracks */
			bc->build_auto.type_count = 1;
			bc->resources.food = bc->resources.rock = bc->resources.wood = bc->resources.ore = bc->resources.gold = 1000000000LL;
			bc->building_count = 3;
			bc->building[0] = (BuildingInfo){ .position_id = 10, .build_id = 6, .level = 1 };
			bc->building[1] = (BuildingInfo){ .position_id = 11, .build_id = 8, .level = 3 };
			bc->building[2] = (BuildingInfo){ .position_id = 12, .build_id = 12, .level = 3 };
			reset_sent();
			BuildAutoTick(bc);
			CHECK(sent_count == 0, "build auto: nothing before the construction packet of the login has told which queues exist");
			bc->construction_loaded = true;
			bc->construction_extra_expires = INT64_MAX; /* a permanent second queue */
			BuildAutoTick(bc);
			int at = find_packet(2003);
			CHECK(at >= 0 && (uint16_t)(sent[at][8] | sent[at][9] << 8) == 10 && (uint16_t)(sent[at][10] | sent[at][11] << 8) == 6
				&& bc->build_auto.pending && strstr(bc->build_auto.state, "Lancement demand") != NULL,
				"build auto: starts the Barracks it planned (slot 10) and waits for the answer");
			reset_sent();
			bc->build_auto.next_check_at = 0;
			BuildAutoTick(bc);
			CHECK(sent_count == 0, "build auto: never a second start while the first one is unanswered");

			uint8_t begin[40] = { 10, 0, 6, 0, 2, 0xf8, 0xbd, 0xb7, 0x6a, 0, 0, 0, 0, 0x10, 0x0e, 0, 0 };
			RecvBuildBegin(bc, begin, sizeof(begin));
			CHECK(!bc->build_auto.pending && bc->construction[0].used && bc->construction[0].slot == 10, "build auto: the answer clears the wait and fills a queue");

			/* refused: the slot is left alone */
			bc->build_auto.next_check_at = 0;
			bc->construction[0].used = 0;
			bc->build_auto.pending = true;
			bc->build_auto.pending_slot = 10;
			uint8_t err[4] = { 1, 0, 0, 0 };
			RecvBuildingError(bc, err, sizeof(err));
			CHECK(!bc->build_auto.pending && strstr(bc->build_auto.state, "refus") != NULL, "build auto: a server error is logged and the slot left alone");
			bc->build_auto.next_check_at = 0;
			reset_sent();
			BuildAutoTick(bc);
			at = find_packet(2003);
			CHECK(at < 0 || (uint16_t)(sent[at][8] | sent[at][9] << 8) != 10, "build auto: the refused slot is not tried again right away");
			free(bc);

			/* the queues: one busy is enough without a second queue, not with one */
			bc = fresh("boss");
			bc->build_auto.enabled = true;
			bc->build_auto.types[0] = 6;
			bc->build_auto.type_count = 1;
			bc->resources.food = bc->resources.rock = bc->resources.wood = bc->resources.ore = bc->resources.gold = 1000000000LL;
			bc->building_count = 3;
			bc->building[0] = (BuildingInfo){ .position_id = 10, .build_id = 6, .level = 1 };
			bc->building[1] = (BuildingInfo){ .position_id = 11, .build_id = 8, .level = 3 };
			bc->building[2] = (BuildingInfo){ .position_id = 12, .build_id = 12, .level = 3 };
			bc->construction_loaded = true;
			bc->construction_extra_expires = INT64_MAX;
			bc->construction[0] = (BuildingConstruction){ .used = 1, .slot = 11, .build_id = 8, .level = 4, .start_time = 1, .duration = 100 };
			bc->construction[1] = (BuildingConstruction){ .used = 1, .slot = 12, .build_id = 12, .level = 4, .start_time = 1, .duration = 100 };
			reset_sent();
			BuildAutoTick(bc);
			CHECK(sent_count == 0 && strstr(bc->build_auto.state, "occup") != NULL, "build auto: nothing started while both construction queues are busy, and it says so");
			bc->construction[1].used = 0;
			bc->build_auto.next_check_at = 0;
			BuildAutoTick(bc);
			CHECK(find_packet(2003) >= 0, "build auto: with a permanent second queue, one busy queue still leaves a free one");
			bc->build_auto.pending = false;
			reset_sent();
			bc->construction_extra_expires = 0;   /* no second queue */
			bc->build_auto.next_check_at = 0;
			BuildAutoTick(bc);
			CHECK(sent_count == 0 && strstr(bc->build_auto.state, "une seule file") != NULL,
				"build auto: without a second queue, one construction running occupies the only queue");
			bc->construction_extra_expires = (int64_t)time(NULL) + 3600; /* rented, still running */
			bc->build_auto.next_check_at = 0;
			BuildAutoTick(bc);
			CHECK(find_packet(2003) >= 0, "build auto: a rented second queue counts while its end date is ahead");
			free(bc);
		}

		/* $askhelp: the requests are the game's, byte for byte, and the loop follows its own answers */
		{
			static const uint8_t req_help[] = { 0x15, 0x00, 0x00, 0x00, 0x01 };
			static const uint8_t req_cancel[] = { 0x1f, 0x00, 0x00, 0x00, 0x00 };
			Connection *ac = fresh("boss");
			reset_sent();
			ac->protocol.seq_id = 0x14;
			RequestBuildHelp(ac, 0);
			int at = find_packet(2852);   /* the number the game sends (not the enum constant of that name: it is 2854) */
			CHECK(at >= 0 && sent_size[at] == 4 + sizeof(req_help) && memcmp(sent[at] + 4, req_help, sizeof(req_help)) == 0,
				"askhelp: the alliance help request is byte for byte the game's (u32 seq, 1 for queue 0)");
			reset_sent();
			ac->protocol.seq_id = 0x14;
			RequestBuildHelp(ac, 1);
			at = find_packet(2852);
			CHECK(at >= 0 && sent[at][8] == 2, "askhelp: for a construction in queue 1 the byte is 2 (queue + 1: 1 was refused live for queue 1)");
			reset_sent();
			ac->protocol.seq_id = 0x1e;
			RequestBuildCancel(ac, 0);
			at = find_packet(2006);
			CHECK(at >= 0 && sent_size[at] == 4 + sizeof(req_cancel) && memcmp(sent[at] + 4, req_cancel, sizeof(req_cancel)) == 0,
				"askhelp: the cancel request is byte for byte the game's (u32 seq, queue 0)");
			free(ac);

			/* two cycles, the Mana Lode running in the second queue must never be touched */
			ac = fresh("boss");
			ac->resources.food = ac->resources.rock = ac->resources.wood = ac->resources.ore = ac->resources.gold = 1000000000LL;
			ac->building_count = 3;
			ac->building[0] = (BuildingInfo){ .position_id = 50, .build_id = 4, .level = 1 };
			ac->building[1] = (BuildingInfo){ .position_id = 51, .build_id = 8, .level = 5 };
			ac->building[2] = (BuildingInfo){ .position_id = 52, .build_id = 4, .level = 9 };
			ac->construction_loaded = true;
			ac->construction_extra_expires = INT64_MAX;
			ac->construction[1] = (BuildingConstruction){ .used = 1, .slot = 999, .build_id = 26, .level = 31, .start_time = 1, .duration = 100000 };
			char error[200];
			CHECK(AskHelpStart(ac, "boss", 2, error, sizeof(error)) && ac->askhelp.active, "askhelp: a series of 2 cycles starts");
			CHECK(!AskHelpStart(ac, "boss", 2, error, sizeof(error)), "askhelp: a second series is refused while one runs");

			uint8_t begin[40];
			for (int cycle = 1; cycle <= 2; cycle++) {
				reset_sent();
				ac->askhelp.next_at = 0;
				AskHelpTick(ac);
				at = find_packet(2003);
				uint16_t slot = at >= 0 ? (uint16_t)(sent[at][8] | sent[at][9] << 8) : 0;
				CHECK(at >= 0 && slot == 50 && ac->askhelp.phase == ASKHELP_WAIT_BEGIN, "askhelp: starts the upgrade of the low-level farm (slot 50, not the level 9 one)");
				reset_sent();
				memset(begin, 0, sizeof(begin));
				begin[0] = 50; begin[2] = 4; begin[4] = 2; begin[13] = 0x10; begin[14] = 0x0e;
				RecvBuildBegin(ac, begin, sizeof(begin));
				CHECK(find_packet(2852) < 0 && ac->askhelp.queue == 0 && ac->askhelp.phase == ASKHELP_WAIT_HELP,
					"askhelp: does not ask for help the instant the answer arrives, the start went to the free queue (0)");
				uint64_t pause = ac->askhelp.next_at - now_ms();
				CHECK(pause >= 900 && pause <= 2100, "askhelp: waits about 1 to 2 seconds before asking, like the game (1.2 s measured)");
				ac->askhelp.next_at = 0;
				AskHelpTick(ac);
				CHECK(find_packet(2852) >= 0 && ac->askhelp.phase == ASKHELP_WAIT_CANCEL_TIMER, "askhelp: then asks the alliance for help");
				uint64_t wait = ac->askhelp.next_at - now_ms();
				CHECK(wait >= 2900 && wait <= 4100, "askhelp: waits 3 to 4 seconds after the help request before cancelling");
				reset_sent();
				AskHelpTick(ac);
				CHECK(find_packet(2006) < 0, "askhelp: does not cancel before the wait is over");
				ac->askhelp.next_at = 0;
				AskHelpTick(ac);
				at = find_packet(2006);
				CHECK(at >= 0 && sent[at][8] == 0 && ac->construction[1].used && ac->construction[1].slot == 999,
					"askhelp: cancels queue 0 - the one it started - and never the other one");
				uint8_t cancel_answer[23] = { 0 };
				RecvBuildCancel(ac, cancel_answer, sizeof(cancel_answer));
				CHECK(ac->askhelp.done == (uint16_t)cycle && !ac->construction[0].used, "askhelp: the cancel's answer counts the cycle and frees the queue");
			}
			CHECK(!ac->askhelp.active, "askhelp: stops by itself after the last cycle");
			CHECK(ac->construction[1].used && ac->construction[1].slot == 999, "askhelp: the long construction in the other queue is still there");
			free(ac);

			/* what stops it */
			ac = fresh("boss");
			ac->resources.food = ac->resources.rock = ac->resources.wood = ac->resources.ore = ac->resources.gold = 1000000000LL;
			ac->building_count = 3;
			ac->building[0] = (BuildingInfo){ .position_id = 50, .build_id = 4, .level = 1 };
			ac->building[1] = (BuildingInfo){ .position_id = 51, .build_id = 8, .level = 5 };
			ac->building[2] = (BuildingInfo){ .position_id = 52, .build_id = 4, .level = 9 };
			ac->construction_loaded = true;
			ac->construction_extra_expires = INT64_MAX;
			AskHelpStart(ac, "boss", 5, error, sizeof(error));
			ac->askhelp.next_at = 0;
			AskHelpTick(ac);
			uint8_t err[4] = { 1, 0, 0, 0 };
			reset_sent();
			RecvBuildingError(ac, err, sizeof(err));
			CHECK(!ac->askhelp.active && find_packet(2006) < 0, "askhelp: a server error stops the series and cancels nothing");
			CHECK(replied("01 00 00 00") && replied("emplacement 50") && replied("files vues"),
				"askhelp: a refusal says the code, what was asked, the queues the bot holds and the stocks against the cost");

			AskHelpStart(ac, "boss", 5, error, sizeof(error));
			ac->askhelp.next_at = 0;
			AskHelpTick(ac);
			ac->askhelp.phase_since = 1;   /* the answer never came */
			reset_sent();
			AskHelpTick(ac);
			CHECK(!ac->askhelp.active && find_packet(2006) < 0, "askhelp: no answer to the start stops the series, nothing cancelled");

			ac->construction[0] = (BuildingConstruction){ .used = 1, .slot = 60, .build_id = 8, .level = 6, .start_time = 1, .duration = 100 };
			ac->construction[1] = (BuildingConstruction){ .used = 1, .slot = 61, .build_id = 8, .level = 6, .start_time = 1, .duration = 100 };
			CHECK(!AskHelpStart(ac, "boss", 3, error, sizeof(error)) && !ac->askhelp.active, "askhelp: refused when both construction queues are busy");
			free(ac);

			/* the command */
			ac = fresh("boss");
			ac->resources.food = ac->resources.rock = ac->resources.wood = ac->resources.ore = ac->resources.gold = 1000000000LL;
			ac->building_count = 3;
			ac->building[0] = (BuildingInfo){ .position_id = 50, .build_id = 4, .level = 1 };
			ac->building[1] = (BuildingInfo){ .position_id = 51, .build_id = 8, .level = 5 };
			ac->building[2] = (BuildingInfo){ .position_id = 52, .build_id = 4, .level = 9 };
			ac->construction_loaded = true;
			ac->construction_extra_expires = INT64_MAX;
			say(ac, "eve", "$askhelp 3", COMMAND_CHANNEL_MAIL);
			CHECK(!ac->askhelp.active, "$askhelp: refused to a stranger");
			say(ac, "boss", "$askhelp 500", COMMAND_CHANNEL_MAIL);
			CHECK(!ac->askhelp.active, "$askhelp: more than 100 cycles is refused");
			say(ac, "boss", "$askhelp 3 farm", COMMAND_CHANNEL_MAIL);
			CHECK(!ac->askhelp.active, "$askhelp: no building to name any more, it is always the low-level farm");
			say(ac, "boss", "$askhelp 3", COMMAND_CHANNEL_MAIL);
			CHECK(ac->askhelp.active && ac->askhelp.total == 3, "$askhelp <times>: starts a series on the low-level farm");
			say(ac, "boss", "$askhelp stop", COMMAND_CHANNEL_MAIL);
			CHECK(!ac->askhelp.active, "$askhelp stop: stops it");
			free(ac);
		}

		/* the server's answer to the help request: accepted, or refused (then this cycle's cancel goes through and the series stops) */
		{
			static const uint8_t ans_ok[] = { 0x00, 0x01, 0x04, 0x00, 0x18, 0x1e };          /* captured */
			static const uint8_t ans_refused[] = { 0x02, 0x01, 0x5d, 0x87, 0x02, 0x00 };      /* live, farm in queue 1 asked with byte 1 */
			Connection *hc = fresh("boss");
			hc->resources.food = hc->resources.rock = hc->resources.wood = hc->resources.ore = hc->resources.gold = 1000000000LL;
			hc->building_count = 2;
			hc->building[0] = (BuildingInfo){ .position_id = 57584, .build_id = 4, .level = 12 };
			hc->building[1] = (BuildingInfo){ .position_id = 51, .build_id = 8, .level = 13 };
			hc->construction_loaded = true;
			hc->construction_extra_expires = INT64_MAX;
			char error[200];
			uint8_t begin[40] = { 0xf0, 0xe0, 0x04, 0x00, 0x0d };
			begin[37] = 1;
			AskHelpStart(hc, "boss", 3, error, sizeof(error));
			hc->askhelp.next_at = 0;
			AskHelpTick(hc);
			RecvBuildBegin(hc, begin, sizeof(begin));
			hc->askhelp.next_at = 0;
			reset_sent();
			AskHelpTick(hc);
			int at = find_packet(2852);
			CHECK(at >= 0 && sent[at][8] == 2, "askhelp: the series asks for help with the queue's byte (queue 1 -> 2)");
			RecvBuildHelpAnswer(hc, ans_ok, sizeof(ans_ok));
			CHECK(!hc->askhelp.stop_after_cancel, "askhelp: an accepted help request changes nothing");
			RecvBuildHelpAnswer(hc, ans_refused, sizeof(ans_refused));
			CHECK(hc->askhelp.stop_after_cancel && hc->askhelp.active, "askhelp: a refused help request is noted, the series is not cut off in the middle of a cycle");
			hc->askhelp.next_at = 0;
			reset_sent();
			AskHelpTick(hc);
			CHECK(find_packet(2006) >= 0 && sent[find_packet(2006)][8] == 1, "askhelp: the construction is still cancelled after a refused help request");
			uint8_t cancel_answer[23] = { 0 };
			cancel_answer[20] = 1;   /* the queue that was cancelled */
			reset_sent();
			RecvBuildCancel(hc, cancel_answer, sizeof(cancel_answer));
			CHECK(!hc->askhelp.active && hc->askhelp.done == 1 && replied("refus"), "askhelp: then it stops, and says the server refused the help");
			free(hc);
		}

		/* a start refused after good cycles is retried after a long pause, twice, then the series stops; the cancel's queue byte is checked */
		{
			Connection *rc = fresh("boss");
			rc->resources.food = rc->resources.rock = rc->resources.wood = rc->resources.ore = rc->resources.gold = 1000000000LL;
			rc->building_count = 2;
			rc->building[0] = (BuildingInfo){ .position_id = 57584, .build_id = 4, .level = 12 };
			rc->building[1] = (BuildingInfo){ .position_id = 51, .build_id = 8, .level = 13 };
			rc->construction_loaded = true;
			rc->construction_extra_expires = INT64_MAX;
			char error[200];
			AskHelpStart(rc, "boss", 5, error, sizeof(error));
			rc->askhelp.done = 2;                     /* two cycles behind it */
			rc->askhelp.next_at = 0;
			AskHelpTick(rc);
			uint8_t err[2] = { 3, 1 };
			reset_sent();
			RecvBuildingError(rc, err, sizeof(err));
			CHECK(rc->askhelp.active && rc->askhelp.phase == ASKHELP_PAUSE && rc->askhelp.start_retries == 1,
				"askhelp: a start refused after good cycles is retried later instead of stopping the series");
			uint64_t wait = rc->askhelp.next_at - now_ms();
			CHECK(wait >= 19000 && wait <= 31000, "askhelp: the retry waits about 20 to 30 seconds");
			rc->askhelp.next_at = 0;
			AskHelpTick(rc);
			RecvBuildingError(rc, err, sizeof(err));
			rc->askhelp.next_at = 0;
			AskHelpTick(rc);
			RecvBuildingError(rc, err, sizeof(err));
			CHECK(!rc->askhelp.active && replied("03 01"), "askhelp: after two retries the refusal stops the series and is reported");

			/* the cancel's own answer says which queue it cancelled: it has to be the bot's */
			rc = fresh("boss");
			rc->resources.food = rc->resources.rock = rc->resources.wood = rc->resources.ore = rc->resources.gold = 1000000000LL;
			rc->building_count = 2;
			rc->building[0] = (BuildingInfo){ .position_id = 57584, .build_id = 4, .level = 12 };
			rc->building[1] = (BuildingInfo){ .position_id = 51, .build_id = 8, .level = 13 };
			rc->construction_loaded = true;
			rc->construction_extra_expires = INT64_MAX;
			AskHelpStart(rc, "boss", 3, error, sizeof(error));
			rc->askhelp.next_at = 0;
			AskHelpTick(rc);
			uint8_t begin[40] = { 0xf0, 0xe0, 0x04, 0x00, 0x0d };
			begin[37] = 1;
			RecvBuildBegin(rc, begin, sizeof(begin));
			rc->askhelp.next_at = 0; AskHelpTick(rc);
			rc->askhelp.next_at = 0; AskHelpTick(rc);       /* help, then cancel */
			uint8_t answer[23] = { 0 };
			answer[20] = 0;                                  /* it says queue 0 was cancelled, the bot asked for queue 1 */
			RecvBuildCancel(rc, answer, sizeof(answer));
			CHECK(!rc->askhelp.active && rc->askhelp.done == 0, "askhelp: a cancel answer naming another queue than the bot's stops the series, nothing counted");
			free(rc);
		}

		/* the queue a start went to is read from the answer (byte 37), not guessed: live, the farm went to queue 1 with queue 0 empty */
		{
			/* the bot's own answer at 13:30:21, farm slot 57584 to level 13: the byte after the gold is 01 */
			static const uint8_t live_begin_q1[] = {
				0xf0, 0xe0, 0x04, 0x00, 0x0d, 0xec, 0xc8, 0xb7, 0x6a, 0x00, 0x00, 0x00, 0x00, 0x1e, 0x08, 0x00, 0x00,
				0x30, 0x38, 0x05, 0x03, 0x96, 0x01, 0x00, 0x00, 0xe5, 0xc2, 0x8d, 0x77, 0x71, 0xa7, 0x01, 0x00,
				0x54, 0xc6, 0x43, 0x01, 0x01, 0x00, 0x00 };
			Connection *qc = fresh("boss");
			qc->building_count = 1;
			qc->building[0] = (BuildingInfo){ .position_id = 57584, .build_id = 4, .level = 12 };
			qc->construction_loaded = true;
			qc->construction_extra_expires = INT64_MAX;
			RecvBuildBegin(qc, live_begin_q1, sizeof(live_begin_q1));
			CHECK(qc->construction[1].used && qc->construction[1].slot == 57584 && !qc->construction[0].used,
				"build: the answer's queue byte (1) puts the farm in queue 1 although queue 0 is empty");
			free(qc);

			/* askhelp cancels that queue, never the guessed one; a cancel that gets no answer is tried once on the other queue */
			qc = fresh("boss");
			qc->resources.food = qc->resources.rock = qc->resources.wood = qc->resources.ore = qc->resources.gold = 1000000000LL;
			qc->building_count = 2;
			qc->building[0] = (BuildingInfo){ .position_id = 57584, .build_id = 4, .level = 12 };
			qc->building[1] = (BuildingInfo){ .position_id = 51, .build_id = 8, .level = 13 };
			qc->construction_loaded = true;
			qc->construction_extra_expires = INT64_MAX;
			char error[200];
			AskHelpStart(qc, "boss", 2, error, sizeof(error));
			qc->askhelp.next_at = 0;
			AskHelpTick(qc);
			reset_sent();
			RecvBuildBegin(qc, live_begin_q1, sizeof(live_begin_q1));
			CHECK(qc->askhelp.queue == 1, "askhelp: the series takes the queue from the answer (1)");
			qc->askhelp.next_at = 0;
			AskHelpTick(qc);     /* help */
			qc->askhelp.next_at = 0;
			reset_sent();
			AskHelpTick(qc);     /* cancel */
			int at = find_packet(2006);
			CHECK(at >= 0 && sent[at][8] == 1, "askhelp: cancels queue 1, where the answer said the farm is");

			/* no answer: the other queue, once */
			qc->askhelp.phase_since = 1;
			reset_sent();
			AskHelpTick(qc);
			at = find_packet(2006);
			CHECK(at >= 0 && sent[at][8] == 0 && qc->askhelp.retried_other && qc->askhelp.active, "askhelp: a cancel without answer is tried once on the other queue");
			qc->askhelp.phase_since = 1;
			reset_sent();
			AskHelpTick(qc);
			CHECK(!qc->askhelp.active && find_packet(2006) < 0, "askhelp: and then it stops, without a third try");

			/* an answer without the queue byte: the series stops and cancels nothing */
			qc->construction[0].used = qc->construction[1].used = 0;
			AskHelpStart(qc, "boss", 2, error, sizeof(error));
			qc->askhelp.next_at = 0;
			AskHelpTick(qc);
			reset_sent();
			RecvBuildBegin(qc, live_begin_q1, 17);
			CHECK(!qc->askhelp.active && find_packet(2006) < 0, "askhelp: an answer that does not say the queue stops the series, nothing cancelled");
			free(qc);
		}

		/* a farm left under construction by a series cut short is cancelled first, then the series goes on */
		{
			Connection *lc = fresh("boss");
			lc->resources.food = lc->resources.rock = lc->resources.wood = lc->resources.ore = lc->resources.gold = 1000000000LL;
			lc->building_count = 3;
			lc->building[0] = (BuildingInfo){ .position_id = 50, .build_id = 4, .level = 3 };
			lc->building[1] = (BuildingInfo){ .position_id = 51, .build_id = 8, .level = 12 };
			lc->building[2] = (BuildingInfo){ .position_id = 52, .build_id = 4, .level = 30 };
			lc->construction_loaded = true;
			lc->construction_extra_expires = INT64_MAX;
			lc->construction[1] = (BuildingConstruction){ .used = 1, .slot = 50, .build_id = 4, .level = 4, .start_time = 1, .duration = 2000 };
			lc->construction[0] = (BuildingConstruction){ .used = 1, .slot = 999, .build_id = 26, .level = 31, .start_time = 1, .duration = 100000 };
			char error[200];
			CHECK(AskHelpStart(lc, "boss", 2, error, sizeof(error)), "askhelp: a farm left under construction does not refuse the series");
			reset_sent();
			lc->askhelp.next_at = 0;
			AskHelpTick(lc);
			int at = find_packet(2006);
			CHECK(at >= 0 && sent[at][8] == 1 && find_packet(2003) < 0 && lc->askhelp.recovering,
				"askhelp: cancels the leftover farm, in the queue it is in (1), and starts nothing yet");
			uint8_t answer[23] = { 0 };
			answer[20] = 1;   /* the leftover farm was in queue 1 */
			RecvBuildCancel(lc, answer, sizeof(answer));
			CHECK(lc->askhelp.done == 0 && !lc->askhelp.recovering && !lc->construction[1].used && lc->construction[0].used && lc->construction[0].slot == 999,
				"askhelp: the recovery cancel is not a cycle, frees that queue and leaves the other one alone");
			reset_sent();
			lc->askhelp.next_at = 0;
			AskHelpTick(lc);
			CHECK(find_packet(2003) >= 0, "askhelp: then the series goes on with a start");
			free(lc);

			/* something else in the queue is never cancelled */
			lc = fresh("boss");
			lc->resources.food = lc->resources.rock = lc->resources.wood = lc->resources.ore = lc->resources.gold = 1000000000LL;
			lc->building_count = 2;
			lc->building[0] = (BuildingInfo){ .position_id = 50, .build_id = 4, .level = 3 };
			lc->building[1] = (BuildingInfo){ .position_id = 51, .build_id = 8, .level = 12 };
			lc->construction_loaded = true;
			lc->construction_extra_expires = INT64_MAX;
			lc->construction[0] = (BuildingConstruction){ .used = 1, .slot = 51, .build_id = 8, .level = 13, .start_time = 1, .duration = 2000 };
			AskHelpStart(lc, "boss", 2, error, sizeof(error));
			reset_sent();
			lc->askhelp.next_at = 0;
			AskHelpTick(lc);
			CHECK(find_packet(2006) < 0 && find_packet(2003) >= 0, "askhelp: another building under construction is never cancelled, the farm is started in the free queue");
			free(lc);
		}

		/* the farm's cost is covered from the bag when the stock lacks some, once, and the series goes on */
		{
			Connection *sc = fresh("boss");
			sc->resources.food = sc->resources.wood = sc->resources.ore = sc->resources.gold = 1000000000LL;
			sc->resources.rock = 4375;   /* the farm to level 13 asks 13,675 stone: 9,300 short */
			sc->building_count = 2;
			sc->building[0] = (BuildingInfo){ .position_id = 50, .build_id = 4, .level = 12 };
			sc->building[1] = (BuildingInfo){ .position_id = 51, .build_id = 8, .level = 13 };
			sc->construction_loaded = true;
			sc->construction_extra_expires = INT64_MAX;
			char error[200];
			CHECK(AskHelpStart(sc, "boss", 2, error, sizeof(error)), "askhelp: a stock that lacks some stone does not refuse the series at once");

			sc->items[STONE_10K].quantity = 0;
			reset_sent();
			sc->askhelp.next_at = 0;
			AskHelpTick(sc);
			CHECK(!sc->askhelp.active && find_packet(2003) < 0 && replied("pierre") && replied("sac 0"),
				"askhelp: with nothing in the bag it stops, and names the resource, what is missing, the stock and the bag");

			sc->resources.rock = 4375;
			sc->items[STONE_10K].quantity = 1;
			CHECK(AskHelpStart(sc, "boss", 2, error, sizeof(error)), "askhelp: a series can start again");
			reset_sent();
			sc->askhelp.next_at = 0;
			AskHelpTick(sc);
			CHECK(find_packet(1406) >= 0 && find_packet(2003) < 0 && sc->resources.rock >= 13675 && sc->items[STONE_10K].quantity == 0 && sc->askhelp.topups == 1,
				"askhelp: a 10K stone pack from the bag covers the missing stone, nothing is started yet");
			reset_sent();
			sc->askhelp.next_at = 0;
			AskHelpTick(sc);
			CHECK(find_packet(2003) >= 0, "askhelp: once the stone is credited the farm is started");
			free(sc);
		}

		/* the farm kept at a low level: never upgraded by the automatic construction */
		{
			Connection *fc = fresh("boss");
			fc->build_auto.enabled = true;
			fc->build_auto.types[0] = 4; /* Farm */
			fc->build_auto.type_count = 1;
			fc->resources.food = fc->resources.rock = fc->resources.wood = fc->resources.ore = fc->resources.gold = 1000000000LL;
			fc->building_count = 3;
			fc->building[0] = (BuildingInfo){ .position_id = 70, .build_id = 4, .level = 4 };
			fc->building[1] = (BuildingInfo){ .position_id = 71, .build_id = 4, .level = 2 };
			fc->building[2] = (BuildingInfo){ .position_id = 72, .build_id = 8, .level = 9 };
			CHECK(BuildingReservedFarm(fc) == 1, "reserved farm: the farm with the lowest level (slot 71)");
			BuildPick pick;
			char why[160];
			bool ok = BuildPickNext(fc, 4, &pick, why, sizeof(why));
			CHECK(ok && pick.slot == 70, "reserved farm: the automatic construction upgrades the other farm, never the kept one");
			fc->building[0].level = 55; /* the other farm is done */
			ok = BuildPickNext(fc, 4, &pick, why, sizeof(why));
			CHECK(!ok && strstr(why, "maximum") != NULL, "reserved farm: with the other farm done, nothing is left to do for the farms");
			fc->building_count = 2;
			fc->building[0] = (BuildingInfo){ .position_id = 71, .build_id = 4, .level = 2 };
			fc->building[1] = (BuildingInfo){ .position_id = 72, .build_id = 8, .level = 9 };
			ok = BuildPickNext(fc, 4, &pick, why, sizeof(why));
			CHECK(!ok && strstr(why, "gardée") != NULL, "reserved farm: a single farm is kept as it is, and the plan says why");
			char error[200];
			fc->building_count = 1;
			fc->building[0] = (BuildingInfo){ .position_id = 72, .build_id = 8, .level = 9 };
			fc->construction_loaded = true;
			fc->construction_extra_expires = INT64_MAX;
			CHECK(!AskHelpStart(fc, "boss", 3, error, sizeof(error)) && strstr(error, "aucune ferme") != NULL, "askhelp: no farm on the account, and it says so");
			free(fc);
		}

		/* the second queue's end date is read from the construction packet */
		{
			uint8_t pkt[BUILDING_QUEUE_INFO_SIZE];
			memset(pkt, 0, sizeof(pkt));
			for (int i = 0; i < 8; i++) pkt[BUILDING_QUEUE_SLOTS * BUILDING_QUEUE_ENTRY_SIZE + i] = i < 7 ? 0xff : 0x7f;
			Connection *qc = fresh("boss");
			RecvBuildingQueue(qc, pkt, sizeof(pkt));
			CHECK(qc->construction_loaded && qc->construction_extra_expires == INT64_MAX, "build queue: the trailing i64 of the packet is the second queue's end date (INT64_MAX = permanent)");
			free(qc);
		}

		/* $research: progress per category, remaining researches of one category */
		{
			Connection *rc = fresh("boss");
			rc->research = c->research;
			reset_sent();
			say(rc, "eve", "$research", COMMAND_CHANNEL_MAIL);
			CHECK(replied("Seuls les administrateurs peuvent consulter"), "$research: refused to a stranger");
			reset_sent();
			say(rc, "boss", "$research", COMMAND_CHANNEL_MAIL);
			CHECK(replied("En cours") && replied("Sceaux") && replied("Duel de guildes"), "$research: research in progress and every category");
			reset_sent();
			say(rc, "boss", "$research sceaux", COMMAND_CHANNEL_MAIL);
			CHECK(replied("Sceaux :") && replied("Reste"), "$research <category>: what is left in the category (by name)");
			reset_sent();
			say(rc, "boss", "$research 7", COMMAND_CHANNEL_MAIL);
			CHECK(replied("Direction Arm"), "$research <n>: category by its position in the game's tabs");
			reset_sent();
			say(rc, "boss", "$research economie", COMMAND_CHANNEL_MAIL);
			CHECK(replied("conomie"), "$research: accents do not have to be typed");
			reset_sent();
			say(rc, "boss", "$research batailles", COMMAND_CHANNEL_MAIL);
			CHECK(replied("Plusieurs cat"), "$research: an ambiguous name lists the matching categories");
			reset_sent();
			say(rc, "boss", "$research zzz", COMMAND_CHANNEL_MAIL);
			CHECK(replied("Cat") && replied("inconnue"), "$research: unknown category is said");
			free(rc);
		}

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
		c->gather.scan_done = true; // this test is about the march, not the zone scan
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
		CHECK(find_packet(_MSG_REQUEST_MAP_ADVANCE) >= 0 && c->gather.pending_tile != GATHER_NO_PENDING_TILE,
			"recall: gathering goes on once the pause is over (looks at the tile first)");
		c->gather.march_send_at = 1; // skip the human-pacing wait between the look and the march
		GatherTick(c);
		CHECK(find_packet(_MSG_REQUEST_TROOPMARCH_NOTATK) >= 0, "recall: the march itself follows once the pacing delay is over");

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
		c->gather.scan_done = true; // this test is about the march, not the zone scan
		c->player.max_marches = 6;
		c->gather.tile_count = 1;
		// amount=1000 / 23.3 + 1 = 44 troops by the uncapped formula
		c->gather.tiles[0] = (GatherTile){ .used = true, .zone_id = 1, .point_id = 2, .level = 3, .amount = 1000 };
		c->troop.loaded = true;
		c->troop.infantry[0] = 5; // only 5 troops recorded as free, whatever tier/kind they are
		c->troop.total = 5;       // total is a separately maintained running sum, not derived - see TroopAdd
		reset_sent();
		c->gather.next_march_at = 1;
		GatherTick(c); // looks at the tile first (RequestMapAdvance), march itself waits for march_send_at
		c->gather.march_send_at = 1; // skip the human-pacing wait between the look and the march
		GatherTick(c);
		int k = find_packet(_MSG_REQUEST_TROOPMARCH_NOTATK);
		// gather.kind defaults to TROOP_INFANTRY, whose count sits at offset 22 - see
		// RequestGatherMarch's comment for the real 4-slot layout confirmed from captures.
		uint32_t sent_count_field = k >= 0
			? (uint32_t)(sent[k][22] | sent[k][23] << 8 | sent[k][24] << 16 | sent[k][25] << 24) : 0;
		CHECK(k >= 0 && sent_count_field == 5,
			"gather: the troop count is capped to that kind's troops, not the tile-derived formula");

		free(c);

		/* no troops recorded at all: nothing is sent, and the tile/slot are freed again */
		c = fresh("boss");
		c->gather.enabled = true;
		c->gather.max_marches = 1;
		c->gather.scan_done = true;
		c->player.max_marches = 6;
		c->gather.tile_count = 1;
		c->gather.tiles[0] = (GatherTile){ .used = true, .zone_id = 1, .point_id = 2, .level = 3, .amount = 1000 };
		c->troop.loaded = true;
		c->troop.infantry[0] = 0;
		c->troop.total = 0;
		reset_sent();
		c->gather.next_march_at = 1;
		GatherTick(c); // sends the RequestMapAdvance, marks the tile targeted, reserves a slot
		c->gather.march_send_at = 1;
		GatherTick(c); // at send time: nothing free - backs out of the tile and the slot it reserved
		CHECK(find_packet(_MSG_REQUEST_TROOPMARCH_NOTATK) < 0 && !c->gather.tiles[0].targeted
			&& c->gather.active_marches == 0,
			"gather: with zero troops recorded, no march is sent and the tile/slot are freed again");
		CHECK(c->gather.next_march_at > now_ms() + 15000,
			"gather: a troop shortage backs off for a while instead of re-picking the same tile every tick");
		free(c);

		/* gather.kind is a priority list: falls back to the next kind if the first has nothing free */
		c = fresh("boss");
		c->gather.enabled = true;
		c->gather.max_marches = 1;
		c->gather.scan_done = true;
		c->player.max_marches = 6;
		c->gather.tile_count = 1;
		c->gather.tiles[0] = (GatherTile){ .used = true, .zone_id = 1, .point_id = 2, .level = 3, .amount = 1000 };
		c->troop.loaded = true;
		c->troop.infantry[0] = 0;   // top of the priority list: nothing free
		c->troop.ranged[0] = 4000;  // next in the list: plenty free
		reset_sent();
		c->gather.next_march_at = 1;
		GatherTick(c);
		c->gather.march_send_at = 1;
		GatherTick(c);
		k = find_packet(_MSG_REQUEST_TROOPMARCH_NOTATK);
		uint32_t ranged_amt = k >= 0 ? (uint32_t)(sent[k][38] | sent[k][39] << 8 | sent[k][40] << 16 | sent[k][41] << 24) : 0;
		uint32_t infantry_amt = k >= 0 ? (uint32_t)(sent[k][22] | sent[k][23] << 8 | sent[k][24] << 16 | sent[k][25] << 24) : 0;
		CHECK(k >= 0 && infantry_amt == 0 && ranged_amt > 0,
			"gather: falls back to the next kind in priority order when the first has nothing free");
		free(c);

		/* gather also subtracts troops already committed to marches still out, not just c->troop.total */
		c = fresh("boss");
		c->gather.enabled = true;
		c->gather.max_marches = 2;
		c->gather.scan_done = true;
		c->player.max_marches = 6;
		c->gather.tile_count = 2;
		c->gather.tiles[0] = (GatherTile){ .used = true, .zone_id = 1, .point_id = 1, .level = 5, .amount = 5000 };
		c->gather.tiles[1] = (GatherTile){ .used = true, .zone_id = 1, .point_id = 2, .level = 4, .amount = 5000 };
		c->troop.loaded = true;
		c->troop.infantry[0] = 100;
		c->troop.total = 100;
		reset_sent();
		c->gather.next_march_at = 1;
		GatherTick(c); // highest level tile first (5 before 4): looks at it
		c->gather.march_send_at = 1;
		GatherTick(c); // then takes all 100 available troops
		k = find_packet(_MSG_REQUEST_TROOPMARCH_NOTATK);
		uint32_t amt1 = k >= 0 ? (uint32_t)(sent[k][22] | sent[k][23] << 8 | sent[k][24] << 16 | sent[k][25] << 24) : 0;
		CHECK(k >= 0 && amt1 == 100 && c->gather.troops_out == 100,
			"gather: first march takes all available troops and troops_out tracks it");

		reset_sent();
		c->gather.next_march_at = 1;
		GatherTick(c); // second tile: nothing left free to send, even after looking at it
		c->gather.march_send_at = 1;
		GatherTick(c);
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
		c->gather.march_send_at = 1;
		GatherTick(c);
		CHECK(c->gather.troops_out == 100, "gather: troops_out is tracked again after a fresh accepted send");
		RecvGatherTroopHome(c, NULL, 0);
		CHECK(c->gather.troops_out == 0, "gather: troops coming home are credited back to troops_out");
		free(c);

		/* a march tops up with the next kind in priority when the first is not enough, and troops out
		 * are tracked per kind: what infantry has out never shrinks the cavalry stock */
		c = fresh("boss");
		c->gather.enabled = true;
		c->gather.max_marches = 2;
		c->gather.scan_done = true;
		c->player.max_marches = 6;
		c->gather.kind_priority[0] = TROOP_INFANTRY;
		c->gather.kind_priority[1] = TROOP_CAVALRY;
		c->gather.kind_priority_count = 2;
		c->gather.tile_count = 2;
		c->gather.tiles[0] = (GatherTile){ .used = true, .zone_id = 1, .point_id = 1, .level = 5, .amount = 5000 };
		c->gather.tiles[1] = (GatherTile){ .used = true, .zone_id = 1, .point_id = 2, .level = 4, .amount = 5000 };
		c->troop.loaded = true;
		c->troop.infantry[0] = 100;
		c->troop.cavalry[0] = 70;
		reset_sent();
		c->gather.next_march_at = 1;
		GatherTick(c);
		c->gather.march_send_at = 1;
		GatherTick(c); // wants 215: infantry gives its 100, cavalry tops up with its 70, same march
		k = find_packet(_MSG_REQUEST_TROOPMARCH_NOTATK);
		uint32_t inf_sent = k >= 0 ? (uint32_t)(sent[k][22 + 16*TROOP_INFANTRY] | sent[k][23 + 16*TROOP_INFANTRY] << 8) : 0;
		uint32_t cav_sent = k >= 0 ? (uint32_t)(sent[k][22 + 16*TROOP_CAVALRY] | sent[k][23 + 16*TROOP_CAVALRY] << 8) : 0;
		CHECK(k >= 0 && inf_sent == 100 && cav_sent == 70,
			"gather: one march mixes kinds in priority order when the first kind is not enough");
		CHECK(c->gather.troops_out_by_kind[TROOP_INFANTRY] == 100 && c->gather.troops_out_by_kind[TROOP_CAVALRY] == 70
			&& c->gather.troops_out == 170, "gather: each kind's troops out are tracked separately");
		RecvGatherTroopHome(c, NULL, 0);
		CHECK(c->gather.troops_out == 0 && c->gather.troops_out_by_kind[TROOP_INFANTRY] == 0
			&& c->gather.troops_out_by_kind[TROOP_CAVALRY] == 0,
			"gather: a mixed march coming home credits back every kind it carried");
		free(c);

		/* the first kind alone is enough: nothing is taken from the next one */
		c = fresh("boss");
		c->gather.enabled = true;
		c->gather.max_marches = 2;
		c->gather.scan_done = true;
		c->player.max_marches = 6;
		c->gather.kind_priority[0] = TROOP_INFANTRY;
		c->gather.kind_priority[1] = TROOP_CAVALRY;
		c->gather.kind_priority_count = 2;
		c->gather.tile_count = 1;
		c->gather.tiles[0] = (GatherTile){ .used = true, .zone_id = 1, .point_id = 1, .level = 5, .amount = 5000 };
		c->troop.loaded = true;
		c->troop.infantry[0] = 1000;
		c->troop.cavalry[0] = 70;
		reset_sent();
		c->gather.next_march_at = 1;
		GatherTick(c);
		c->gather.march_send_at = 1;
		GatherTick(c);
		CHECK(c->gather.troops_out_by_kind[TROOP_INFANTRY] == 215 && c->gather.troops_out_by_kind[TROOP_CAVALRY] == 0,
			"gather: the second kind is left alone when the first has enough");
		free(c);

		/* restart with marches already out: they count against gather.max_marches (2 out + 3 reserved = 1 more) */
		c = fresh("boss");
		c->gather.enabled = true;
		c->gather.max_marches = 3;
		c->gather.scan_done = true;
		c->player.max_marches = 5;
		{
			uint8_t snap[2] = { 5, 2 };
			RecvMarchData(c, snap);
		}
		c->gather.tile_count = 3;
		c->gather.tiles[0] = (GatherTile){ .used = true, .zone_id = 1, .point_id = 1, .level = 5, .amount = 5000 };
		c->gather.tiles[1] = (GatherTile){ .used = true, .zone_id = 1, .point_id = 2, .level = 5, .amount = 5000 };
		c->gather.tiles[2] = (GatherTile){ .used = true, .zone_id = 1, .point_id = 3, .level = 5, .amount = 5000 };
		for (int i = 0; i < 6; i++) {
			c->gather.next_march_at = 1;
			GatherTick(c);
			c->gather.march_send_at = 1;
			GatherTick(c);
			uint8_t ok[2] = { 0, 1 };
			if (c->gather.active_marches > 0 && c->gather.pending_count > 0 && (int)c->gather.pending_count > i)
				RecvGatherMarchResp(c, ok, sizeof(ok));
		}
		CHECK(c->gather.active_marches == 1, "gather: marches already out at login count against gather.max_marches");
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

	/* Automatic gathering: the active zone scan (RequestMapData), byte-for-byte matched against
	 * real captures - see GatherSettings' comment (connection.h). radius=1 around the castle's own
	 * zone (100,100 -> zone 99) covers exactly that one zone, so the scan is a single request. */
	{
		map_pos_t home = { 100, 100 };
		uint16_t hz; uint8_t hp;
		MapPosToPointCode(home, &hz, &hp);

		c = fresh("boss");
		c->gather.enabled = true;
		c->gather.radius = 1;
		c->player.zone_id = hz; c->player.point_id = hp;
		c->player.max_marches = 6;
		c->player.current_marches = 0;

		reset_sent();
		c->gather.next_scan_at = 0;
		GatherTick(c);
		int k = find_packet(_MSG_REQUEST_MAPDATA);
		CHECK(k >= 0 && find_packet(_MSG_REQUEST_OPEN_UI) < 0,
			"gather: scans with MAPDATA directly, no OPEN_UI needed first");
		CHECK(sent_size[k] == 49, "gather: MAPDATA is the captured 49-byte shape regardless of zone count");
		CHECK(sent[k][8] == 1, "gather: count is the true number of zones (1), never hardcoded to 4");
		uint16_t sent_zone = (uint16_t)(sent[k][9] | (sent[k][10] << 8));
		CHECK(sent_zone == 99, "gather: requests the castle's own zone (100,100 -> zone 99)");
		bool rest_zero = true;
		for (int i = 11; i < 49; i++) if (sent[k][i] != 0) rest_zero = false;
		CHECK(rest_zero, "gather: unused zone slots and padding are zero, never a repeat of the real zone");
		CHECK(c->gather.scan_cursor == 1 && !c->gather.scan_done, "gather: scan_cursor advances, scan not done yet");

		reset_sent();
		c->gather.next_scan_at = 0;
		GatherTick(c);
		CHECK(find_packet(_MSG_REQUEST_MAPDATA) < 0 && c->gather.scan_done,
			"gather: nothing left in a radius-1 rectangle, scan ends after the one zone");
		free(c);
	}

	/* Automatic gathering: sending a march. Live-tested, a level 13 tile with just over 1B in
	 * stock made the uncapped amount/GATHER_DEFAULT_TROOP_CAPACITY formula ask for 44,193,237
	 * troops - refused outright (nowhere near a real troop count). gather.max_troop_count caps
	 * that. Also covers the two-step march (RequestMapAdvance, paced, then the march itself) on
	 * a path that is not $recall's. */
	{
		c = fresh("boss");
		c->gather.enabled = true;
		c->gather.max_marches = 1;
		c->gather.scan_done = true;
		c->gather.max_troop_count = 500000; // admin's real, known troop count
		c->player.max_marches = 6;
		c->player.current_marches = 0;
		c->gather.tile_count = 1;
		c->gather.tiles[0] = (GatherTile){ .used = true, .zone_id = 1, .point_id = 2, .level = 13,
			.amount = 1029702400 }; // the exact live figure that asked for 44,193,237 troops uncapped

		reset_sent();
		c->gather.next_march_at = 1;
		GatherTick(c);
		int adv = find_packet(_MSG_REQUEST_MAP_ADVANCE);
		CHECK(adv >= 0 && find_packet(_MSG_REQUEST_TROOPMARCH_NOTATK) < 0 && c->gather.tiles[0].targeted,
			"gather: looks at the tile and reserves it before sending anything else");
		CHECK(c->gather.pending_tile == 0 && c->gather.march_send_at > now_ms(),
			"gather: the march itself waits out a human-like pause first");

		c->gather.march_send_at = 1;
		GatherTick(c);
		int m = find_packet(_MSG_REQUEST_TROOPMARCH_NOTATK);
		CHECK(m >= 0 && c->gather.pending_tile == GATHER_NO_PENDING_TILE, "gather: the march follows, pending tile cleared");
		// gather.kind defaults to TROOP_INFANTRY, whose count sits at offset 22 - see
		// RequestGatherMarch's comment for the real 4-slot layout confirmed from captures.
		uint32_t sent_troops = (uint32_t)sent[m][22] | ((uint32_t)sent[m][23] << 8)
			| ((uint32_t)sent[m][24] << 16) | ((uint32_t)sent[m][25] << 24);
		CHECK(sent_troops == 500000, "gather: troop count capped at gather.max_troop_count instead of the raw 44M+ formula result");
		free(c);
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
		CHECK(c->transfer_queue_count == 1 && c->transfer_queue[0].lines[0].amount == 526316 && c->transfer_queue[0].from_balance,
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
		CHECK(c->transfer_queue_count == 1 && c->transfer_queue[0].lines[0].amount == GuildBankBalance("Zyco", RESOURCE_ROCK),
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

	/* $rss / $adminrss: several resources in one command, sent in priority order (gold, ore, wood,
	 * stone, then food last) regardless of the order typed, 0 skips a resource. */
	{
		char dir[] = "/tmp/lmbot_rss_XXXXXX";
		CHECK(mkdtemp(dir) != NULL, "rss: temporary folder");
		GuildBankReset();
		MarchesPauseEnd();

		c = fresh("boss");
		snprintf(c->bot.data_path, sizeof(c->bot.data_path), "%s/", dir);
		c->guildbank.enabled = true;
		c->server_time = 1000;
		c->bank.delivery_tax_percent = 0.0;   /* keep the amounts round: tax math is covered elsewhere */
		c->resources.food = c->resources.rock = c->resources.wood = c->resources.ore = c->resources.gold = 100000000;
		const char *members[] = { "boss", "Bob" };
		for (int i = 0; i < 2; i++)
			snprintf(c->alliance_member.member[c->alliance_member.count++].name, 14, "%s", members[i]);
		GuildBankCredit(c, "boss", RESOURCE_ROCK, 100);
		GuildBankCredit(c, "boss", RESOURCE_ORE, 200);
		GuildBankCredit(c, "boss", RESOURCE_GOLD, 300);

		/* usage and "all zero" are both rejected without touching the queue */
		reset_sent();
		say(c, "boss", "$rss", COMMAND_CHANNEL_MAIL);
		CHECK(c->transfer_queue_count == 0 && replied("Usage"), "rss: no arguments at all is rejected with the usage");
		reset_sent();
		say(c, "boss", "$rss 0 0 0 0 0", COMMAND_CHANNEL_MAIL);
		CHECK(c->transfer_queue_count == 0 && replied("Rien à retirer"), "rss: every amount at 0 is rejected, nothing queued");

		/* stone, ore and gold requested (in that typed order): sent gold first, then ore, then stone last - food and wood skipped */
		reset_sent();
		say(c, "boss", "$rss 0 100 0 200 300", COMMAND_CHANNEL_MAIL);
		CHECK(c->transfer_queue_count == 1 && c->transfer_queue[0].line_count == 3
			&& c->transfer_queue[0].lines[0].type == RESOURCE_GOLD && c->transfer_queue[0].lines[0].amount == 300
			&& c->transfer_queue[0].lines[1].type == RESOURCE_ORE  && c->transfer_queue[0].lines[1].amount == 200
			&& c->transfer_queue[0].lines[2].type == RESOURCE_ROCK && c->transfer_queue[0].lines[2].amount == 100,
			"rss: queued in priority order (gold, ore, stone), not the order typed, food and wood skipped (0)");

		ResourceTransferTick(c);
		CHECK(c->transfer.line_count == 3 && c->transfer.line_index == 0
			&& c->transfer.resource_type == RESOURCE_GOLD && c->transfer.amount == 300,
			"rss: the delivery starts on the highest priority resource (gold)");

		/* gold's delivery completes: line_index advances to ore instead of going idle */
		c->transfer.zone_id = 1; c->transfer.point_id = 2; c->transfer.remaining = 0; c->transfer.state = TRANSFER_COMPLETE;
		reset_sent();
		ResourceTransferTick(c);
		CHECK(c->transfer.state == TRANSFER_FIND_TARGET && c->transfer.line_index == 1
			&& c->transfer.resource_type == RESOURCE_ORE && c->transfer.amount == 200 && c->transfer.not_before > now_ms(),
			"rss: gold done, moves on to ore (re-finding the target) instead of completing the whole request");

		/* ore's delivery completes too: advances to the last line (stone) */
		c->transfer.remaining = 0; c->transfer.state = TRANSFER_COMPLETE;
		ResourceTransferTick(c);
		CHECK(c->transfer.state == TRANSFER_FIND_TARGET && c->transfer.line_index == 2
			&& c->transfer.resource_type == RESOURCE_ROCK && c->transfer.amount == 100,
			"rss: ore done, moves on to stone (the last line)");

		/* stone's delivery completes: nothing left, goes idle this time */
		c->transfer.remaining = 0; c->transfer.state = TRANSFER_COMPLETE;
		ResourceTransferTick(c);
		CHECK(c->transfer.state == TRANSFER_IDLE, "rss: stone done, nothing left in the batch: the delivery is over");
		AbortTransfer(c);

		/* insufficient balance on any one resource cancels the whole request, not just that resource */
		reset_sent();
		say(c, "boss", "$rss 0 0 0 0 1000000", COMMAND_CHANNEL_MAIL);   /* boss only has 300 gold deposited */
		CHECK(c->transfer_queue_count == 0 && replied("insuffisant"), "rss: a single resource above the balance rejects the whole command");

		/* "all", like the single-resource commands' own $gold all: the whole balance, no need to know the exact number */
		reset_sent();
		say(c, "boss", "$rss 0 0 0 0 all", COMMAND_CHANNEL_MAIL);
		CHECK(c->transfer_queue_count == 1 && c->transfer_queue[0].line_count == 1
			&& c->transfer_queue[0].lines[0].type == RESOURCE_GOLD && c->transfer_queue[0].lines[0].amount == 300,
			"rss: \"all\" takes the whole balance of that resource (boss has 300 gold deposited)");
		AbortTransfer(c);
		c->transfer_queue_count = 0;

		/* $adminrss: same ordering, admin-only, from the bot's stock, name can hold spaces */
		reset_sent();
		say(c, "eve", "$adminrss 0 0 0 0 1M Bob", COMMAND_CHANNEL_MAIL);
		CHECK(c->transfer_queue_count == 0 && replied("Seuls les administrateurs"), "adminrss: a stranger cannot use it");

		reset_sent();
		say(c, "boss", "$adminrss 0 50 0 0 0 Bob", COMMAND_CHANNEL_MAIL);
		CHECK(c->transfer_queue_count == 1 && c->transfer_queue[0].line_count == 1
			&& c->transfer_queue[0].lines[0].type == RESOURCE_ROCK && c->transfer_queue[0].lines[0].amount == 50
			&& !c->transfer_queue[0].from_balance && strcmp(c->transfer_queue[0].target, "Bob") == 0,
			"adminrss: a single non-zero resource among five gives from the stock, never the balance");
		AbortTransfer(c);
		c->transfer_queue_count = 0;

		/* "all" here means everything the bot can give from its stock, not a balance - and unlike
		 * $admin<ressource>, $adminrss is allowed to dip into the members' deposits, so "all" is
		 * the whole stock, not the stock minus the 300 gold boss has deposited. */
		reset_sent();
		say(c, "boss", "$adminrss 0 0 0 0 all Bob", COMMAND_CHANNEL_MAIL);
		CHECK(c->transfer_queue_count == 1 && c->transfer_queue[0].line_count == 1
			&& c->transfer_queue[0].lines[0].type == RESOURCE_GOLD && c->transfer_queue[0].lines[0].amount == 100000000
			&& c->transfer_queue[0].ignore_deposits,
			"adminrss: \"all\" takes the whole stock, deposits included (100,000,000, not minus the 300 boss deposited)");
		AbortTransfer(c);
		c->transfer_queue_count = 0;

		/* asking for more than the stock minus deposits still goes through - only the reserve, not
		 * the deposits, can stop $adminrss */
		reset_sent();
		say(c, "boss", "$adminrss 0 0 0 0 200000000 Bob", COMMAND_CHANNEL_MAIL);   /* > the 100M in stock */
		CHECK(c->transfer_queue_count == 0 && replied("Ressources insuffisantes"),
			"adminrss: still rejected once the request exceeds the raw stock itself");
		reset_sent();
		say(c, "boss", "$adminrss 0 0 0 0 99999900 Bob", COMMAND_CHANNEL_MAIL);   /* > stock - 300 deposited, <= stock */
		CHECK(c->transfer_queue_count == 1 && c->transfer_queue[0].lines[0].amount == 99999900,
			"adminrss: dips into the 300 gold members have deposited without being blocked");
		AbortTransfer(c);
		c->transfer_queue_count = 0;

		/* the reserve still holds $adminrss back, unlike the deposits */
		c->bank.reserve.gold = 1000;
		reset_sent();
		say(c, "boss", "$adminrss 0 0 0 0 all Bob", COMMAND_CHANNEL_MAIL);
		CHECK(c->transfer_queue_count == 1 && c->transfer_queue[0].lines[0].amount == 100000000 - 1000,
			"adminrss: \"all\" still respects the configured reserve (1000 gold kept back)");
		AbortTransfer(c);
		c->transfer_queue_count = 0;
		c->bank.reserve.gold = 0;

		snprintf(c->alliance_member.member[c->alliance_member.count++].name, 14, "%s", "Little Zyco");
		reset_sent();
		say(c, "boss", "$adminrss 0 0 0 0 100 Little Zyco", COMMAND_CHANNEL_MAIL);
		CHECK(c->transfer_queue_count == 1 && strcmp(c->transfer_queue[0].target, "Little Zyco") == 0,
			"adminrss: the player name (after the five amounts) can hold spaces");
		AbortTransfer(c);
		c->transfer_queue_count = 0;

		/* $adminall: empties the stock (reserve included) into another player, deposits excepted -
		 * boss still has 100/200/300 deposited (rock/ore/gold) from earlier in this block. */
		c->bank.reserve.food = 5000000;
		c->bank.reserve.gold = 1000;
		reset_sent();
		say(c, "eve", "$adminall Bob", COMMAND_CHANNEL_MAIL);
		CHECK(c->transfer_queue_count == 0 && replied("Seuls les administrateurs"), "adminall: a stranger cannot use it");

		reset_sent();
		say(c, "boss", "$adminall Bob", COMMAND_CHANNEL_MAIL);
		CHECK(c->transfer_queue_count == 1 && c->transfer_queue[0].ignore_reserve && !c->transfer_queue[0].from_balance
			&& strcmp(c->transfer_queue[0].target, "Bob") == 0 && c->transfer_queue[0].line_count == 5,
			"adminall: queues every resource (all non-zero here), flagged to ignore the reserve");
		CHECK(c->transfer_queue[0].lines[0].type == RESOURCE_GOLD && c->transfer_queue[0].lines[0].amount == 100000000 - 300,
			"adminall: gold ignores its 1,000 reserve but still respects the 300 members deposited");
		CHECK(c->transfer_queue[0].lines[4].type == RESOURCE_FOOD && c->transfer_queue[0].lines[4].amount == 100000000,
			"adminall: food ignores its 5,000,000 reserve too, nobody deposited food so nothing else is subtracted");
		AbortTransfer(c);
		c->transfer_queue_count = 0;

		/* contrast: every other admin command still respects the reserve normally */
		reset_sent();
		say(c, "boss", "$adminfood Bob 100M", COMMAND_CHANNEL_MAIL);
		CHECK(c->transfer_queue_count == 0 && replied("Ressources insuffisantes"),
			"adminall vs adminfood: adminfood still refuses to dip into the reserve, unlike adminall");

		/* nothing left once the deposits are excepted */
		c->resources.food = c->resources.rock = c->resources.wood = c->resources.ore = c->resources.gold = 0;
		reset_sent();
		say(c, "boss", "$adminall Bob", COMMAND_CHANNEL_MAIL);
		CHECK(c->transfer_queue_count == 0 && replied("Rien à envoyer"), "adminall: nothing in stock, nothing queued");

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

	/* ---- autotrain: only one (kind, tier) trains account-wide at a time, round-robin across kinds ---- */
	{
		c = fresh("boss");
		c->troop.loaded = true;
		c->troop.ranged[TIER_T2] = 1000;
		c->troop.infantry[TIER_T2] = 500;
		c->autotrain.enabled = true;
		give_stock(c);
		c->autotrain.target[TROOP_INFANTRY][TIER_T2] = 2000;
		c->autotrain.target[TROOP_RANGED][TIER_T2]   = 5000;
		reset_sent();

		// Both kinds need action, but the game only ever trains one at a time account-wide (a
		// real capture showed two more kinds requested while a first was still training both got
		// refused outright) - infantry (the lower TroopKind index) goes first.
		AutoTrainTick(c);
		int k = find_packet(_MSG_REQUEST_TRAINING_);
		CHECK(k >= 0 && sent[k][8] == TROOP_INFANTRY && sent[k][9] == TIER_T2,
			"autotrain: first tick starts the lowest kind that needs training");
		CHECK(c->autotrain.busy, "autotrain: busy (account-wide) after sending an order");

		c->autotrain.next_action_at = 0; // simulate the pacing delay having elapsed
		reset_sent();
		AutoTrainTick(c); // infantry's order is still training - nothing else may start meanwhile
		CHECK(sent_count == 0, "autotrain: no other kind can start while one is still training account-wide");

		// infantry's batch finishes: RecvAddSoldier reports it, frees the account-wide slot, credits the count
		uint8_t addsoldier[6] = { TROOP_INFANTRY, TIER_T2, 0xDC, 0x05, 0x00, 0x00 }; // 1500 LE
		RecvAddSoldier(c, addsoldier, sizeof(addsoldier));
		CHECK(c->troop.infantry[TIER_T2] == 2000, "autotrain: RecvAddSoldier credits the trained amount");
		CHECK(!c->autotrain.busy, "autotrain: RecvAddSoldier frees the account-wide busy flag");

		c->autotrain.next_action_at = 0;
		reset_sent();
		AutoTrainTick(c);
		k = find_packet(_MSG_REQUEST_TRAINING_);
		CHECK(k >= 0 && sent[k][8] == TROOP_RANGED,
			"autotrain: round-robin moves on to the next kind once infantry reaches its target");

		uint8_t addsoldier2[6] = { TROOP_RANGED, TIER_T2, 0x70, 0x0F, 0x00, 0x00 }; // 4000 LE
		RecvAddSoldier(c, addsoldier2, sizeof(addsoldier2));
		c->autotrain.next_action_at = 0;
		reset_sent();
		AutoTrainTick(c);
		k = find_packet(_MSG_REQUEST_TRAINING_);
		CHECK(k < 0 || sent[k][8] != TROOP_INFANTRY,
			"autotrain: a kind already at its target is not retrained just because round-robin reaches it again");
		free(c);
	}

	/* ---- autotrain: a refusal pauses every kind (one training at a time) and halves the next amount ---- */
	{
		c = fresh("boss");
		c->troop.loaded = true;
		c->autotrain.enabled = true;
		give_stock(c);
		c->autotrain.target[TROOP_INFANTRY][TIER_T1] = 5000000;
		c->autotrain.target[TROOP_RANGED][TIER_T1]   = 5000000;
		reset_sent();

		AutoTrainTick(c);
		int k = find_packet(_MSG_REQUEST_TRAINING_);
		uint32_t first = k >= 0 ? (uint32_t)(sent[k][10] | sent[k][11] << 8 | sent[k][12] << 16 | sent[k][13] << 24) : 0;
		CHECK(first == AUTOTRAIN_INITIAL_BATCH_GUESS && first <= 5000, "autotrain: the first amount is no more than a level-25 barracks holds");

		uint8_t bareRefusal[1] = { 2 }; // code 2: not "already training" (code 1), so the amount is what gets cut
		RecvTrainingStart(c, bareRefusal, sizeof(bareRefusal));
		CHECK(c->autotrain.next_action_at >= now_ms() + AUTOTRAIN_REFUSAL_PAUSE_MS - 1000,
			"autotrain: a refusal pauses everything, not just that kind - the next kind is not tried a second later");
		reset_sent();
		AutoTrainTick(c);
		CHECK(sent_count == 0, "autotrain: nothing is sent during that pause");

		c->autotrain.next_action_at = 0; // pause over
		c->autotrain.retry_at[TROOP_INFANTRY][TIER_T1] = 0;
		reset_sent();
		AutoTrainTick(c);
		k = find_packet(_MSG_REQUEST_TRAINING_);
		uint32_t second = k >= 0 ? (uint32_t)(sent[k][10] | sent[k][11] << 8 | sent[k][12] << 16 | sent[k][13] << 24) : 0;
		CHECK(second > 0 && second <= first / 2 + 1, "autotrain: after a refusal the next order asks for half the amount");
		free(c);
	}

	/* ---- autotrain: a training already running at login (_MSG_RESP_TRAININGINFO_) blocks every order until it ends ---- */
	{
		c = fresh("boss");
		c->troop.loaded = true;
		c->autotrain.enabled = true;
		give_stock(c);
		c->autotrain.target[TROOP_INFANTRY][TIER_T1] = 5000000;
		c->autotrain.target[TROOP_RANGED][TIER_T1]   = 5000000;
		// the capture: infantry T1, 17500, begun 1790006... server clock 1790427614 - 69042 s, 76845 s long
		uint8_t info[18] = { 0, 0, 0x5c, 0x44, 0, 0, 0xde, 0xc1, 0xb6, 0x6a, 0, 0, 0, 0, 0x2d, 0x2c, 0x01, 0 };
		c->server_time = 0x6ab6c1deULL + 69042;
		RecvTrainingInfo(c, info, sizeof(info));
		CHECK(c->autotrain.busy && c->training[TROOP_INFANTRY].active && c->training[TROOP_INFANTRY].amount == 17500,
			"autotrain: a training in progress at login marks the slot busy");
		CHECK(c->autotrain.running_until == 0x6ab6c1deULL + 76845, "autotrain: its end is begin + duration");
		CHECK(c->autotrain.last_granted == 0, "autotrain: the login training is not taken as what one order may hold");
		reset_sent();
		AutoTrainTick(c);
		CHECK(sent_count == 0, "autotrain: nothing is sent while it runs");

		c->server_time = 0x6ab6c1deULL + 76845 + AUTOTRAIN_END_GRACE_S + 1; // past its end, no 'troops added' packet came
		c->autotrain.next_action_at = 0;
		AutoTrainTick(c);
		CHECK(!c->autotrain.busy && !c->training[TROOP_INFANTRY].active, "autotrain: the slot is freed once the end has passed");
		free(c);

		// already over at login: idle
		c = fresh("boss");
		c->troop.loaded = true;
		c->autotrain.enabled = true;
		give_stock(c);
		c->server_time = 0x6ab6c1deULL + 76845 + 100;
		RecvTrainingInfo(c, info, sizeof(info));
		CHECK(!c->autotrain.busy && !c->training[TROOP_INFANTRY].active, "autotrain: a training whose end has passed is idle");
		free(c);
		// nothing training: zeros
		c = fresh("boss");
		uint8_t none[18] = { 0 };
		c->server_time = 0x6ab6c1deULL;
		RecvTrainingInfo(c, none, sizeof(none));
		CHECK(!c->autotrain.busy, "autotrain: an empty TRAININGINFO is idle");
		free(c);
	}

	/* ---- the second capture: infantry T2 x 2472 running, from a real login (client clock 0x6ab7d365) ---- */
	{
		c = fresh("boss");
		c->troop.loaded = true;
		c->autotrain.enabled = true;
		give_stock(c);
		c->autotrain.target[TROOP_INFANTRY][TIER_T2] = 5000000;
		c->server_time = 0x6ab7d365ULL;
		uint8_t info[18] = { 0x00, 0x01, 0xa8, 0x09, 0x00, 0x00, 0x14, 0xc9, 0xb7, 0x6a, 0, 0, 0, 0, 0x91, 0x38, 0x00, 0x00 };
		RecvTrainingInfo(c, info, sizeof(info));
		CHECK(c->autotrain.busy && c->training[TROOP_INFANTRY].active && c->training[TROOP_INFANTRY].tier == TIER_T2
			&& c->training[TROOP_INFANTRY].amount == 2472, "TRAININGINFO (real capture): infantry T2 x 2472 is running");
		CHECK(c->autotrain.running_until == 0x6ab7c914ULL + 14481, "TRAININGINFO (real capture): it ends at begin + 14481 s");
		reset_sent();
		AutoTrainTick(c);
		CHECK(sent_count == 0, "TRAININGINFO (real capture): no order is sent while it runs");
		// a refusal with code 1 is that same 'already training': it must not shrink the next amount
		c->autotrain.busy = false; c->autotrain.running_until = 0; c->autotrain.next_action_at = 0;
		AutoTrainTick(c);
		uint8_t code1[1] = { 1 };
		RecvTrainingStart(c, code1, sizeof(code1));
		CHECK(c->autotrain.last_granted == 0, "autotrain: code 1 (already training) does not shrink the next amount");
		uint8_t code2[1] = { 2 };
		c->autotrain.next_action_at = 0; c->autotrain.retry_at[TROOP_INFANTRY][TIER_T2] = 0;
		AutoTrainTick(c);
		RecvTrainingStart(c, code2, sizeof(code2));
		CHECK(c->autotrain.last_granted > 0, "autotrain: another refusal code does shrink it");
		free(c);
	}

	/* ---- an order is never bigger than the stock pays for ---- */
	{
		c = fresh("boss");
		c->troop.loaded = true;
		c->autotrain.enabled = true;
		give_stock(c);
		c->autotrain.target[TROOP_INFANTRY][TIER_T2] = 5000000;
		// the login of the second capture: 0 food, 47370 stone, 28580 wood, 30560 ore, 5.96M gold
		c->resources.food = 0; c->resources.rock = 47370; c->resources.wood = 28580; c->resources.ore = 30560; c->resources.gold = 5960000;
		int scarce = -1;
		CHECK(TroopsAffordable(c, TROOP_INFANTRY, TIER_T2, false, &scarce) == 0 && scarce == 0, "affordable: no food = no infantry T2, food is what is short");
		reset_sent();
		AutoTrainTick(c);
		CHECK(find_packet(_MSG_REQUEST_TRAINING_) < 0, "autotrain: nothing is asked when not one troop can be paid");
		CHECK(c->autotrain.retry_at[TROOP_INFANTRY][TIER_T2] > now_ms() + 5 * 60 * 1000 && !c->autotrain.busy,
			"autotrain: that box waits, and the slot is not left busy");

		c->resources.food = 250000;                                       // 2500 troops' worth of food (100 each)
		CHECK(TroopsAffordable(c, TROOP_INFANTRY, TIER_T2, false, NULL) == 285, "affordable: the scarcest resource sets it (28580 wood / 100)");
		c->autotrain.retry_at[TROOP_INFANTRY][TIER_T2] = 0;
		c->autotrain.next_action_at = 0;
		reset_sent();
		AutoTrainTick(c);
		int k = find_packet(_MSG_REQUEST_TRAINING_);
		uint32_t amount = k >= 0 ? (uint32_t)(sent[k][10] | sent[k][11] << 8 | sent[k][12] << 16 | sent[k][13] << 24) : 0;
		CHECK(amount == 285, "autotrain: the order is cut down to what the stock pays for");
		CHECK(TroopsAffordable(c, TROOP_INFANTRY, TIER_T5, false, NULL) == UINT32_MAX, "affordable: T5 has no price in the table, not limited");
		free(c);
	}

	/* ---- the bag pays what the stock lacks, and no more than that ---- */
	{
		c = fresh("boss");
		c->troop.loaded = true;
		c->autotrain.enabled = true;
		give_stock(c);
		c->autotrain.target[TROOP_INFANTRY][TIER_T2] = 5000000;
		c->resources.food = 0;                                            // 5000 infantry T2 = 500000 food
		c->items[FOOD_500K].quantity = 3; c->items[FOOD_5K].quantity = 10;
		c->autotrain.smart_refused_until = now_ms() + 3600000;   // the item-by-item path: the game's own request was refused
		CHECK(TroopsAffordable(c, TROOP_INFANTRY, TIER_T2, false, NULL) == 0 && TroopsAffordable(c, TROOP_INFANTRY, TIER_T2, true, NULL) == 15500,
			"affordable: the bag counts when asked (1550000 food in it / 100)");
		reset_sent();
		AutoTrainTick(c);
		CHECK(find_packet(_MSG_REQUEST_TRAINING_) < 0 && useitem_count == 1 && c->items[FOOD_500K].quantity == 2 && c->items[FOOD_5K].quantity == 10,
			"bag: only what is missing is taken - one 500K item for the 500000 food, nothing more");
		CHECK(c->autotrain.bag_topups == 1 && c->resources.food == 500000, "bag: the food is credited, one top-up counted");
		CHECK(!c->autotrain.busy && c->autotrain.next_action_at > now_ms(), "bag: the order waits a few seconds for the server to credit it");

		// nothing else while the item's answer is awaited
		c->autotrain.next_action_at = 0;
		reset_sent();
		AutoTrainTick(c);
		CHECK(sent_count == 0 && c->autotrain.bag_item == FOOD_500K, "bag: nothing is sent while the item's answer is awaited");

		// the answer: status 0, item, what is left of it (2)
		uint8_t answer[6] = { 0, (uint8_t)(FOOD_500K & 0xff), (uint8_t)(FOOD_500K >> 8), 2, 0, 0 };
		RecvUseItem(c, answer, sizeof(answer));
		CHECK(c->autotrain.bag_item == 0 && c->resources.food == 500000, "bag: the answer confirms the credit and frees the step");
		c->autotrain.next_action_at = 0;
		reset_sent();
		AutoTrainTick(c);
		int k = find_packet(_MSG_REQUEST_TRAINING_);
		uint32_t amount = k >= 0 ? (uint32_t)(sent[k][10] | sent[k][11] << 8 | sent[k][12] << 16 | sent[k][13] << 24) : 0;
		CHECK(k >= 0 && amount == 5000 && useitem_count == 0 && c->autotrain.bag_topups == 0,
			"bag: then the order goes out for the full amount, with nothing more taken from the bag");
		free(c);

		// a refused item (a capture: status 0x44 for wood and ore items): its credit is taken back, the bag is left alone
		c = fresh("boss");
		c->troop.loaded = true;
		c->autotrain.enabled = true;
		give_stock(c);
		c->autotrain.target[TROOP_INFANTRY][TIER_T2] = 5000000;
		c->resources.wood = 2312;                                         // 23 troops' worth of wood
		c->items[TIMBER_500K].quantity = 3;
		c->autotrain.smart_refused_until = now_ms() + 3600000;   // the item-by-item path: the game's own request was refused
		reset_sent();
		AutoTrainTick(c);
		CHECK(c->autotrain.bag_item == TIMBER_500K && c->resources.wood == 2312 + 500000, "bag refused: the item is used and credited first");
		uint8_t refused[3] = { 0x44, 0x02, 0x04 };
		RecvUseItem(c, refused, sizeof(refused));
		CHECK(c->autotrain.bag_item == 0 && c->resources.wood == 2312 && c->autotrain.bag_refused_until > now_ms(),
			"bag refused: the credit is taken back and the bag is left alone");
		c->autotrain.next_action_at = 0;
		reset_sent();
		AutoTrainTick(c);
		k = find_packet(_MSG_REQUEST_TRAINING_);
		amount = k >= 0 ? (uint32_t)(sent[k][10] | sent[k][11] << 8 | sent[k][12] << 16 | sent[k][13] << 24) : 0;
		CHECK(k >= 0 && amount == 23 && useitem_count == 0, "bag refused: the order is cut to what the stock really pays (23), no second try");
		free(c);

		// the bag cannot cover it: the order shrinks to what the stock pays for, and the bag stays untouched
		c = fresh("boss");
		c->troop.loaded = true;
		c->autotrain.enabled = true;
		give_stock(c);
		c->autotrain.target[TROOP_INFANTRY][TIER_T2] = 5000000;
		c->resources.food = 100000;                                       // 1000 troops
		c->items[FOOD_5K].quantity = 2;                                   // +10000 food = 100 more troops, still short of 5000
		c->autotrain.smart_refused_until = now_ms() + 3600000;   // the item-by-item path: the game's own request was refused
		reset_sent();
		AutoTrainTick(c);
		CHECK(find_packet(_MSG_REQUEST_TRAINING_) < 0 && useitem_count == 1 && c->items[FOOD_5K].quantity == 0,
			"bag: the shortfall of the amount stock + bag can pay is taken from the bag");
		uint8_t answer2[6] = { 0, (uint8_t)(FOOD_5K & 0xff), (uint8_t)(FOOD_5K >> 8), 0, 0, 0 };
		RecvUseItem(c, answer2, sizeof(answer2));
		c->autotrain.next_action_at = 0;
		reset_sent();
		AutoTrainTick(c);
		int k2 = find_packet(_MSG_REQUEST_TRAINING_);
		uint32_t amount2 = k2 >= 0 ? (uint32_t)(sent[k2][10] | sent[k2][11] << 8 | sent[k2][12] << 16 | sent[k2][13] << 24) : 0;
		CHECK(amount2 == 1100, "bag: the order is cut to what stock + bag pay for (110000 food / 100)");
		free(c);
	}

	/* ---- the game's own "train and use the bag" request, from the capture: 7226 cavalry T2, only food short ---- */
	{
		c = fresh("boss");
		c->troop.loaded = true;
		c->autotrain.enabled = true;
		give_stock(c);
		c->autotrain.target[TROOP_CAVALRY][TIER_T2] = 7226;              // gap 7226: that is the amount asked
		c->autotrain.last_granted = 4818;                                 // 4818 x 3/2 = 7227, cut to the gap
		c->resources.food = 289040;                                       // 722600 needed: 433560 short
		// the bag as it was before the capture's use: 0x0587 x2, 150K x686, 30K x602, 5K x3
		c->items[FOOD_250K].quantity = 2; c->items[FOOD_150K].quantity = 686; c->items[FOOD_30K].quantity = 602; c->items[FOOD_5K].quantity = 3;
		reset_sent();
		AutoTrainTick(c);
		int k = find_packet(_MSG_REQUEST_SMARTUSE_FOR_TRAINING);
		CHECK(k >= 0 && find_packet(_MSG_REQUEST_TRAINING_) < 0 && useitem_count == 0,
			"smart use: one request, no separate order and no item used one by one");
		const uint8_t expected[20] = { TROOP_CAVALRY, TIER_T2, 0x3a, 0x1c, 0x00, 0x00, 0x04, 0x00,
			0x87, 0x05, 0x01, 0x00, 0xf6, 0x03, 0x01, 0x00, 0xf1, 0x03, 0x01, 0x00 };
		const uint8_t tail[4] = { 0x92, 0x04, 0x01, 0x00 };
		CHECK(k >= 0 && memcmp(&sent[k][8], expected, sizeof(expected)) == 0 && memcmp(&sent[k][28], tail, sizeof(tail)) == 0,
			"smart use: type, tier, amount and the four items are the ones of the capture (250K, 150K, 30K, 5K)");
		CHECK(c->autotrain.busy && c->autotrain.pending_amount == 7226, "smart use: the slot is busy with the amount asked");

		// the answer of the capture: result 0, then the items left
		const uint8_t answer[23] = { 0, 0, 0, 0, 0, 4, 0, 0x87, 0x05, 1, 0, 0xf6, 0x03, 0xad, 0x02, 0xf1, 0x03, 0x59, 0x02, 0x92, 0x04, 2, 0 };
		RecvSmartUseForWork(c, answer, sizeof(answer));
		CHECK(c->items[FOOD_250K].quantity == 1 && c->items[FOOD_150K].quantity == 685 && c->items[FOOD_30K].quantity == 601 && c->items[FOOD_5K].quantity == 2,
			"smart use: the answer's quantities become the bag's");
		CHECK(c->autotrain.busy, "smart use: an accepted request keeps the slot busy until the troops are added");
		free(c);

		// refused: item by item for a while, and the slot is free again
		c = fresh("boss");
		c->troop.loaded = true;
		c->autotrain.enabled = true;
		give_stock(c);
		c->autotrain.target[TROOP_CAVALRY][TIER_T2] = 7226;
		c->autotrain.last_granted = 4818;
		c->resources.food = 289040;
		c->items[FOOD_250K].quantity = 2; c->items[FOOD_150K].quantity = 686;
		reset_sent();
		AutoTrainTick(c);
		const uint8_t refusedAnswer[1] = { 1 };
		RecvSmartUseForWork(c, refusedAnswer, sizeof(refusedAnswer));
		CHECK(!c->autotrain.busy && c->autotrain.smart_refused_until > now_ms(), "smart use refused: the slot is free, the request is not tried again for a while");
		c->autotrain.next_action_at = 0;
		reset_sent();
		AutoTrainTick(c);
		CHECK(find_packet(_MSG_REQUEST_SMARTUSE_FOR_TRAINING) < 0 && useitem_count == 1, "smart use refused: the bag is used item by item instead");
		free(c);
	}

	/* ---- the guild shop: two purchases of the capture (the fruit, then a migration scroll) ---- */
	{
		c = fresh("boss");
		reset_sent();
		RequestBuyItem(c, SHOP_TYPE_GUILD, GUILD_SHOP_FRUIT_KEY, LORD_REVIVE_FRUIT_ITEM, 1);
		int k = find_packet(_MSG_REQUEST_BUYITEM);
		const uint8_t fruitReq[7] = { 0x02, 0x0c, 0x00, 0x5d, 0x04, 0x01, 0x00 };
		CHECK(k >= 0 && sent_size[k] == 15 && memcmp(&sent[k][8], fruitReq, sizeof(fruitReq)) == 0,
			"shop: the fruit request is the captured one (type 2, key 12, item 1117, quantity 1)");
		reset_sent();
		RequestBuyItem(c, SHOP_TYPE_GUILD, GUILD_SHOP_SCROLL_KEY, MIGRATION_SCROLL, 1);
		k = find_packet(_MSG_REQUEST_BUYITEM);
		const uint8_t scrollReq[7] = { 0x02, 0xd4, 0x00, 0xfb, 0x04, 0x01, 0x00 };
		CHECK(k >= 0 && memcmp(&sent[k][8], scrollReq, sizeof(scrollReq)) == 0, "shop: the scroll request is the captured one (key 212, item 1275)");
		const uint8_t fruitAns[12] = { 0x00, 0x02, 0x0c, 0x00, 0x5d, 0x04, 0x01, 0x00, 0x94, 0x28, 0xa1, 0x01 };
		RecvBuyItem(c, fruitAns, sizeof(fruitAns));
		CHECK(c->items[LORD_REVIVE_FRUIT_ITEM].quantity == 1 && c->RoleAlliance.Money == 27338900, "shop: the fruit answer gives the bag's quantity and the guild coins left");
		const uint8_t scrollAns[12] = { 0x00, 0x02, 0xd4, 0x00, 0xfb, 0x04, 0x0a, 0x00, 0x84, 0xcc, 0x94, 0x01 };
		RecvBuyItem(c, scrollAns, sizeof(scrollAns));
		CHECK(c->items[MIGRATION_SCROLL].quantity == 10 && c->RoleAlliance.Money == 26528900, "shop: the scroll answer: 10 in the bag, 26528900 coins left");
		free(c);

		// a migration that buys the scrolls it lacks, one at a time
		c = fresh("boss");
		c->items_loaded = true; c->migration_buy_scrolls = true; c->migration_scrolls_needed = 2;
		c->RoleAlliance.Money = 3000000; c->player.current_kingdom_id = 12;
		say(c, "boss", "$migrate 796 301 491", COMMAND_CHANNEL_MAIL);
		reset_sent();
		{ uint8_t toc[21] = { 0, 0x2c, 0x2a, 0, 0, '1', 0 }; RecvKingdomServer(c, toc, sizeof(toc)); }
		CHECK(c->migration.state == MIGRATION_BUYING && replied("Achat de 2 vélin(s) de migration"), "shop: scrolls missing and coins enough: the migration buys them first");
		c->migration.buy_at = 0;
		reset_sent();
		MigrationTick(c);
		k = find_packet(_MSG_REQUEST_BUYITEM);
		CHECK(k >= 0 && sent[k][9] == 0xd4 && sent[k][13] == 1 && c->migration.buy_in_flight, "shop: one scroll is bought (quantity 1)");
		reset_sent();
		MigrationTick(c);
		CHECK(sent_count == 0, "shop: nothing else is sent until the answer");
		{ uint8_t a1[12] = { 0x00, 0x02, 0xd4, 0x00, 0xfb, 0x04, 0x01, 0x00, 0x00, 0x00, 0x2d, 0x00 }; RecvBuyItem(c, a1, sizeof(a1)); }
		c->migration.buy_at = 0;
		reset_sent();
		MigrationTick(c);
		CHECK(find_packet(_MSG_REQUEST_BUYITEM) >= 0, "shop: the second scroll is bought after the first one's answer");
		{ uint8_t a2[12] = { 0x00, 0x02, 0xd4, 0x00, 0xfb, 0x04, 0x02, 0x00, 0x00, 0x00, 0x2c, 0x00 }; RecvBuyItem(c, a2, sizeof(a2)); }
		c->migration.buy_at = 0;
		reset_sent();
		MigrationTick(c);
		CHECK(find_packet(_MSG_REQUEST_USEITEM) >= 0 && c->migration.state == MIGRATION_WAIT_SCROLL_RESULT && find_packet(_MSG_REQUEST_BUYITEM) < 0,
			"shop: with enough scrolls the migration goes on with one");
		free(c);

		// not enough coins: the free offer is tried, and it says why
		c = fresh("boss");
		c->items_loaded = true; c->migration_buy_scrolls = true; c->migration_scrolls_needed = 2;
		c->RoleAlliance.Money = 900000; c->player.current_kingdom_id = 12;
		say(c, "boss", "$migrate 796 301 491", COMMAND_CHANNEL_MAIL);
		reset_sent();
		{ uint8_t toc[21] = { 0, 0x2c, 0x2a, 0, 0, '1', 0 }; RecvKingdomServer(c, toc, sizeof(toc)); }
		CHECK(replied("Pas assez de pièces de guilde pour acheter les 2 vélin(s) qui manquent") && c->migration.state == MIGRATION_WAIT_RESULT && find_packet(_MSG_REQUEST_BUYITEM) < 0,
			"shop: not enough guild coins: nothing is bought, the free offer is tried");
		free(c);

		// off by default: no purchase
		c = fresh("boss");
		c->items_loaded = true; c->migration_scrolls_needed = 2; c->RoleAlliance.Money = 3000000; c->player.current_kingdom_id = 12;
		say(c, "boss", "$migrate 796 301 491", COMMAND_CHANNEL_MAIL);
		reset_sent();
		{ uint8_t toc[21] = { 0, 0x2c, 0x2a, 0, 0, '1', 0 }; RecvKingdomServer(c, toc, sizeof(toc)); }
		CHECK(c->migration.state == MIGRATION_WAIT_RESULT && find_packet(_MSG_REQUEST_BUYITEM) < 0, "shop: without migration.buy_scrolls nothing is bought");
		free(c);
	}

	/* ---- the lord is dead: _MSG_RESP_LORD_BEINGEXECUTED from the capture ---- */
	{
		c = fresh("boss");
		c->server_time = 1790445866;
		snprintf(c->player.name, sizeof(c->player.name), "Zyco");
		const uint8_t executed[13] = { 0xd7, 0xda, 0xa5, 0x6a, 0x00, 0x00, 0x00, 0x00, 0x80, 0x3a, 0x09, 0x00, 0x01 };
		CHECK(!c->lord.dead, "lord: alive until the login says otherwise");
		RecvLordBeingExecuted(c, executed, sizeof(executed));
		CHECK(c->lord.dead && c->lord.since == 1789254359ULL && c->lord.wait == 604800 && c->lord.flag == 1,
			"lord: the packet of the capture: since 2026-09-12 23:05:59, an execution wait of 604800 s (7 days), flag 1");
		RecvLordBeingExecuted(c, executed, 5);
		CHECK(c->lord.dead, "lord: a short packet changes nothing");
		CHECK(c->lord.where == LORD_DEAD, "lord: 4408 = dead");
		RecvLordWhere(c, _MSG_RESP_LORD_HOME, NULL, 0);
		CHECK(c->lord.where == LORD_HOME && !c->lord.dead, "lord: 4409 = back in the castle");
		const uint8_t cage[4] = { 1, 2, 3, 4 };
		RecvLordWhere(c, _MSG_RESP_LORD_BEINGCAPTIVE, cage, sizeof(cage));
		CHECK(c->lord.where == LORD_CAPTIVE && !c->lord.dead, "lord: 4401 = in a prison");
		RecvLordWhere(c, _MSG_RESP_LORD_BEINGRELEASED, NULL, 0);
		CHECK(c->lord.where == LORD_HOME, "lord: 4407 = released");
		free(c);
	}

	/* ---- map monster hunt: from the two captures ---- */
	{
		c = fresh("boss");
		c->hunt.enabled = true; c->hunt.level = 3; c->hunt.chat_report = true;
		c->player.zone_id = 0x1d8; c->player.point_id = 0x5b; c->player.current_kingdom_id = 13;
		c->server_time = 1790438221;

		// the login: 872 stored 1009 s before, 1201 ms per point (the second capture) = 1712, and the owner read 1744 a few seconds later
		uint8_t login[705] = { 0 };
		uint32_t stored = 872; uint64_t when = c->server_time - 1009; uint16_t freq = 1201;
		memcpy(login + 366, &stored, 4); memcpy(login + 370, &when, 8); memcpy(login + 378, &freq, 2);
		HuntReadLogin(c, login, sizeof(login));
		CHECK(c->hunt.energy_known && HuntEnergyNow(c) == 1712, "hunt: the login's stored energy and time give 872 + 1009 s / 1.201 = 1712");
		c->hunt.energy_max = 1000;
		CHECK(HuntEnergyNow(c) == 1000, "hunt: the energy never counts above the maximum the owner gave");

		// the heroes: five mages (4, 5, 6, 16, 19) and two physical ones (1, 2)
		uint8_t save[10 + 7 * 20] = { 0 };
		const uint16_t ids[7] = { 1, 2, 4, 5, 6, 16, 19 };
		const uint8_t levels[7] = { 60, 60, 60, 59, 58, 57, 56 };
		save[8] = 7;
		for (int i = 0; i < 7; i++) { uint8_t *r = save + 10 + i * 20; memcpy(r, &ids[i], 2); r[2] = levels[i]; r[7] = 8; r[8] = 5; }
		RecvHeroSave(c, save, sizeof(save));
		CHECK(c->hunt.hero_count == 7, "hunt: HEROSAVE lists the heroes the account has");
		uint16_t team[HUNT_TEAM_SIZE];
		CHECK(HuntPickHeroes(c, 1, team) && team[0] == 4 && team[1] == 5 && team[2] == 6 && team[3] == 16 && team[4] == 19,
			"hunt: a monster weak to magic (Gorzilla) gets the five mages, best level first");
		CHECK(HuntPickHeroes(c, 2, team) && team[0] == 1 && team[1] == 2 && team[2] == 4 && team[3] == 5 && team[4] == 6,
			"hunt: a monster weak to physical gets the physical heroes first, the team completed with the best of the others");
		CHECK(HuntPickHeroes(c, 0, team) && team[0] == 1 && team[2] == 4, "hunt: no weakness: the highest levels");

		// the map: the two Gorzilla records of the first capture (level 1 at zone 0x1c8 point 0xce, level 3 at zone 0x1d8 point 0x0d), in a bulk snapshot
		uint8_t map[3 + 20 + 15 + 30 + 15 + 20] = { 0 };
		const uint8_t gorz1[15] = { 0xc8, 0x01, 0xce, 0x0a, 0x01, 0x27, 0x00, 0x0e, 0xa9, 0x10, 0x00, 0x00, 0x00, 0xc8, 0x42 };
		const uint8_t gorz3[15] = { 0xd8, 0x01, 0x0d, 0x0a, 0x03, 0x27, 0x00, 0xf8, 0xaa, 0x10, 0x00, 0x00, 0x00, 0xc8, 0x42 };
		memcpy(map + 3 + 20, gorz1, 15);
		memcpy(map + 3 + 20 + 15 + 30, gorz3, 15);
		RecvMapInfoPlus(c, map, sizeof(map));
		CHECK(c->hunt.monster_count == 2 && c->hunt.monsters[0].key == 0x27 && c->hunt.monsters[0].level == 1 && c->hunt.monsters[1].level == 3
			&& c->hunt.monsters[1].serial == 0x10aaf8 && c->hunt.monsters[1].zone_id == 0x1d8 && c->hunt.monsters[1].point_id == 0x0d,
			"hunt: monster records of a bulk snapshot are found by their kind byte, level, key and serial");

		// an Astra monster (key 241, level 4, seen in the captures) is an event one: not hunted
		const uint8_t astra[15] = { 0xc9, 0x01, 0xd0, 0x0a, 0x04, 0xf1, 0x00, 0x01, 0x02, 0x03, 0x04, 0x00, 0x00, 0xc8, 0x42 };
		uint8_t astraMap[3 + 15 + 20] = { 0 };
		memcpy(astraMap + 3, astra, 15);
		RecvMapInfoPlus(c, astraMap, sizeof(astraMap));
		CHECK(c->hunt.monster_count == 2, "hunt: an Astra (event) monster is not taken for a map monster");

		// the attack request is the captured one
		reset_sent();
		const uint16_t captured[HUNT_TEAM_SIZE] = { 16, 19, 6, 4, 5 };
		RequestSendMonster(c, 0x1d8, 0x0d, captured, 3, 0x27);
		int k = find_packet(_MSG_REQUEST_SENDMONSTER);
		const uint8_t expected[17] = { 0xd8, 0x01, 0x0d, 0x01, 0x10, 0x00, 0x13, 0x00, 0x06, 0x00, 0x04, 0x00, 0x05, 0x00, 0x03, 0x27, 0x00 };
		CHECK(k >= 0 && memcmp(&sent[k][8], expected, sizeof(expected)) == 0 && sent_size[k] == 25,
			"hunt: the attack request is the captured one (zone, point, 01, five heroes, level, key)");

		// a series: the energy is at the maximum (4804) -> attack the nearest level 3 monster
		c->hunt.energy_max = 4804;
		c->hunt.energy_stored = 4804; c->hunt.energy_time = c->server_time;
		c->hunt.scan_done = true; c->hunt.next_rescan_at = now_ms() + 3600000;
		reset_sent();
		HuntTick(c);
		k = find_packet(_MSG_REQUEST_SENDMONSTER);
		const uint8_t attack[17] = { 0xd8, 0x01, 0x0d, 0x01, 0x04, 0x00, 0x05, 0x00, 0x06, 0x00, 0x10, 0x00, 0x13, 0x00, 0x03, 0x27, 0x00 };
		CHECK(k >= 0 && memcmp(&sent[k][8], attack, sizeof(attack)) == 0 && c->hunt.phase == HUNT_WAIT_ANSWER && c->hunt.series,
			"hunt: at the maximum energy the nearest monster of the level is attacked with the mages");
		reset_sent();
		c->hunt.next_action_at = 0;
		HuntTick(c);
		CHECK(sent_count == 0, "hunt: nothing else is sent while the answer is awaited");

		// the answer of the capture: 564 left -> the cost of a level 3 attack is 4240
		const uint8_t answer[31] = { 0x00, 0x08, 0xd8, 0x01, 0x0d, 0xbd, 0xeb, 0xb7, 0x6a, 0x00, 0x00, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00, 0x34, 0x02, 0x00, 0x00,
			0x10, 0x00, 0x13, 0x00, 0x06, 0x00, 0x04, 0x00, 0x05, 0x00 };
		RecvSendMonster(c, answer, sizeof(answer));
		CHECK(c->hunt.energy_stored == 564 && c->hunt.cost[3] == 4240 && c->hunt.phase == HUNT_WAIT_HOME,
			"hunt: the answer gives the energy left (564) and the cost of a level 3 attack (4240)");

		// the report (not dead: killed byte 0, health 5904091 -> 4083727) and the heroes back
		uint8_t report[40] = { 0xfb, 0x37, 0x01, 0x00, 0x00, 0xc0, 0xeb, 0xb7, 0x6a, 0, 0, 0, 0, 0x0d, 0x00, 0xd8, 0x01, 0x0d, 0x00, 0x6c, 0x0c, 0x27, 0x00, 0x03,
			0xdb, 0x16, 0x5a, 0x00, 0x0f, 0x50, 0x3e, 0x00 };
		RecvMonsterReport(c, report, sizeof(report));
		CHECK(!c->hunt.killed, "hunt: a report with the killed byte at 0 and health left is not a kill");
		const uint8_t home[1] = { 8 };
		RecvMonsterHome(c, home, sizeof(home));
		CHECK(c->hunt.phase == HUNT_IDLE && c->hunt.series && c->hunt.monster_count == 2, "hunt: the heroes are back, the series goes on");

		// 564 energy left, an attack costs 4240: the monster goes to the guild chat, once
		c->hunt.next_action_at = 0;
		reset_sent();
		HuntTick(c);
		const map_pos_t pos = getTileMapPosbyPointCode(0x1d8, 0x0d);
		char expected_chat[96];
		snprintf(expected_chat, sizeof(expected_chat), "Gorzilla niveau 3 K:13 X:%u Y:%u", pos.x, pos.y);
		k = find_packet(_MSG_REQUEST_SENDCHAT);
		CHECK(k >= 0 && sent[k][8] == 1 && replied(expected_chat) && find_packet(_MSG_REQUEST_SENDMONSTER) < 0 && !c->hunt.series,
			"hunt: not enough energy for another attack: the monster's name, level and coordinates go to the guild chat");
		c->hunt.energy_stored = 4804; c->hunt.energy_time = c->server_time; c->hunt.next_action_at = 0;
		reset_sent();
		HuntTick(c);
		CHECK(find_packet(_MSG_REQUEST_SENDMONSTER) < 0 && c->hunt.monsters[1].reported_until > now_ms(),
			"hunt: a monster reported to the chat is left alone (no attack on it again)");
		free(c);

		// the kill: the third report of the capture has the killed byte at 1 and the health at 0
		c = fresh("boss");
		c->hunt.enabled = true; c->hunt.level = 3; c->player.zone_id = 0x1d8; c->player.point_id = 0x5b; c->player.current_kingdom_id = 13;
		c->server_time = 1790438221; c->hunt.energy_known = true; c->hunt.energy_freq_ms = 1201; c->hunt.energy_max = 4804;
		c->hunt.energy_stored = 4804; c->hunt.energy_time = c->server_time; c->hunt.scan_done = true; c->hunt.next_rescan_at = now_ms() + 3600000;
		RecvHeroSave(c, save, sizeof(save));
		RecvMapInfoPlus(c, map, sizeof(map));
		HuntTick(c);
		RecvSendMonster(c, answer, sizeof(answer));
		uint8_t lastReport[40] = { 0xfd, 0x37, 0x01, 0x00, 0x00, 0xe4, 0xeb, 0xb7, 0x6a, 0, 0, 0, 0, 0x0d, 0x00, 0xd8, 0x01, 0x0d, 0x01, 0x6c, 0x0c, 0x27, 0x00, 0x03,
			0x3a, 0x08, 0x1d, 0x00, 0x00, 0x00, 0x00, 0x00 };
		RecvMonsterReport(c, lastReport, sizeof(lastReport));
		CHECK(c->hunt.killed, "hunt: the report's killed byte says the monster is dead");
		RecvMonsterHome(c, home, sizeof(home));
		CHECK(c->hunt.kills == 1 && !c->hunt.series && c->hunt.monster_count == 1 && c->hunt.monsters[0].level == 1,
			"hunt: a kill ends the series and takes the monster off the list");
		free(c);

		// a refusal that is not the energy: the monster is left alone and three in a row pause the hunt
		c = fresh("boss");
		c->hunt.enabled = true; c->hunt.level = 3; c->player.zone_id = 0x1d8; c->player.point_id = 0x5b; c->player.current_kingdom_id = 13;
		c->server_time = 1790438221; c->hunt.energy_known = true; c->hunt.energy_freq_ms = 1201; c->hunt.energy_max = 4804;
		c->hunt.energy_stored = 4804; c->hunt.energy_time = c->server_time; c->hunt.scan_done = true; c->hunt.next_rescan_at = now_ms() + 3600000;
		RecvHeroSave(c, save, sizeof(save));
		RecvMapInfoPlus(c, map, sizeof(map));
		HuntTick(c);
		const uint8_t refused[1] = { 5 };
		RecvSendMonster(c, refused, sizeof(refused));
		CHECK(!c->hunt.series && c->hunt.phase == HUNT_IDLE && c->hunt.monsters[1].refused_until > now_ms() && c->hunt.refusals == 1 && c->hunt.cost[3] == 0,
			"hunt: a refusal (no cost known): the monster is left alone for a while, no chat message");
		free(c);

		// the map scan: the first one runs at once, the next ones only when a hunt is near
		c = fresh("boss");
		c->hunt.enabled = true; c->hunt.level = 3; c->player.zone_id = 0x1d8; c->player.point_id = 0x5b;
		c->server_time = 1790438221; c->hunt.energy_known = true; c->hunt.energy_freq_ms = 1201; c->hunt.energy_max = 64540;
		c->hunt.energy_stored = 100; c->hunt.energy_time = c->server_time;
		reset_sent();
		HuntTick(c);
		CHECK(find_packet(_MSG_REQUEST_MAPDATA) >= 0, "hunt: the first scan of the map starts at once");
		c->hunt.scan_done = true; c->hunt.next_rescan_at = 1; c->hunt.next_scan_at = 0;
		reset_sent();
		HuntTick(c);
		CHECK(find_packet(_MSG_REQUEST_MAPDATA) < 0, "hunt: no new scan while the energy is far from its maximum");
		c->hunt.energy_stored = 64540 - 100;
		HuntTick(c);
		CHECK(find_packet(_MSG_REQUEST_MAPDATA) >= 0, "hunt: a scan starts when the energy is within ten minutes of its maximum");
		c->hunt.energy_max = 0;
		c->hunt.scan_done = false; c->hunt.next_scan_at = 0;
		reset_sent();
		HuntTick(c);
		CHECK(sent_count == 0, "hunt: without the energy maximum the bot does nothing");
		free(c);

		// fewer than five heroes: no hunt
		c = fresh("boss");
		c->hunt.enabled = true; c->hunt.level = 3; c->player.zone_id = 0x1d8; c->player.point_id = 0x5b;
		c->server_time = 1790438221; c->hunt.energy_known = true; c->hunt.energy_freq_ms = 1201; c->hunt.energy_max = 4804;
		c->hunt.energy_stored = 4804; c->hunt.energy_time = c->server_time; c->hunt.scan_done = true; c->hunt.next_rescan_at = now_ms() + 3600000;
		RecvMapInfoPlus(c, map, sizeof(map));
		reset_sent();
		HuntTick(c);
		CHECK(find_packet(_MSG_REQUEST_SENDMONSTER) < 0, "hunt: an account with fewer than five heroes does not hunt");
		free(c);
	}

	/* ---- the bag completes food last ---- */
	{
		c = fresh("boss");
		c->troop.loaded = true;
		c->autotrain.enabled = true;
		give_stock(c);
		c->autotrain.target[TROOP_INFANTRY][TIER_T2] = 5000000;
		c->resources.food = 0; c->resources.wood = 0;                     // both short for 5000 infantry T2
		c->items[FOOD_500K].quantity = 3; c->items[TIMBER_500K].quantity = 3;
		c->autotrain.smart_refused_until = now_ms() + 3600000;   // the item-by-item path: the game's own request was refused
		reset_sent();
		AutoTrainTick(c);
		CHECK(c->autotrain.bag_item == TIMBER_500K && c->autotrain.bag_res == RESOURCE_WOOD, "bag: wood is completed before food");
		uint8_t woodAnswer[6] = { 0, (uint8_t)(TIMBER_500K & 0xff), (uint8_t)(TIMBER_500K >> 8), 2, 0, 0 };
		RecvUseItem(c, woodAnswer, sizeof(woodAnswer));
		c->autotrain.next_action_at = 0;
		reset_sent();
		AutoTrainTick(c);
		CHECK(c->autotrain.bag_item == FOOD_500K && c->autotrain.bag_res == RESOURCE_FOOD, "bag: food comes last, once nothing else is missing");
		free(c);
	}

	/* ---- the server's answer says what was really granted, and what is left of the stock ---- */
	{
		c = fresh("boss");
		c->troop.loaded = true;
		c->autotrain.enabled = true;
		give_stock(c);
		c->autotrain.target[TROOP_INFANTRY][TIER_T2] = 5000000;
		reset_sent();
		AutoTrainTick(c);
		// the capture: 5000 asked, 15 granted, then food 481274, stone 56301, wood 925, ore 73, gold 5971858
		uint8_t granted[39] = { 0x00, 0x00, 0x01, 0x0f, 0x00, 0x00, 0x00, 0xfa, 0x57, 0x07, 0x00, 0xed, 0xdb, 0x00, 0x00, 0x9d, 0x03, 0x00, 0x00,
			0x49, 0x00, 0x00, 0x00, 0x92, 0x1f, 0x5b, 0x00, 0x32, 0xd6, 0xb7, 0x6a, 0x00, 0x00, 0x00, 0x00, 0x58, 0x00, 0x00, 0x00 };
		RecvTrainingStart(c, granted, sizeof(granted));
		CHECK(c->training[TROOP_INFANTRY].amount == 15 && c->autotrain.last_granted == 15, "TRAINING answer (real capture): 15 granted, not the 5000 asked");
		CHECK(c->resources.food == 481274 && c->resources.rock == 56301 && c->resources.wood == 925 && c->resources.ore == 73 && c->resources.gold == 5971858,
			"TRAINING answer (real capture): the stock left replaces the client's count");
		free(c);
	}

	/* ---- the first order is what the account's barracks hold ---- */
	{
		c = fresh("boss");
		c->building_count = 3;
		c->building[0].build_id = BUILDING_BARRACKS; c->building[0].level = 25;
		c->building[1].build_id = BUILDING_BARRACKS; c->building[1].level = 1;
		c->building[2].build_id = BUILDING_ACADEMY;  c->building[2].level = 25;
		CHECK(BarracksCapacityFloor(c) == 5020, "barracks: the capacities of every Barracks add up (5000 + 20), other buildings ignored");
		c->building_count = 0;
		CHECK(BarracksCapacityFloor(c) == 0, "barracks: 0 until the building list has arrived");
		c->building_count = 1;
		c->building[0].build_id = BUILDING_BARRACKS; c->building[0].level = 1;
		c->troop.loaded = true;
		c->autotrain.enabled = true;
		give_stock(c);
		c->autotrain.target[TROOP_INFANTRY][TIER_T1] = 5000000;
		reset_sent();
		AutoTrainTick(c);
		int k = find_packet(_MSG_REQUEST_TRAINING_);
		uint32_t amount = k >= 0 ? (uint32_t)(sent[k][10] | sent[k][11] << 8 | sent[k][12] << 16 | sent[k][13] << 24) : 0;
		CHECK(amount == 20, "autotrain: a level-1 barracks: the first order is 20 troops, not a fixed 5000");
		free(c);
	}

	/* ---- autotrain: moves on to the next unmet tier (ascending) while the lowest one is backed off ---- */
	{
		c = fresh("boss");
		c->troop.loaded = true;
		c->autotrain.enabled = true;
		give_stock(c);
		c->autotrain.target[TROOP_INFANTRY][TIER_T2] = 2000;
		c->autotrain.target[TROOP_INFANTRY][TIER_T4] = 2000;
		reset_sent();

		AutoTrainTick(c);
		int k = find_packet(_MSG_REQUEST_TRAINING_);
		CHECK(k >= 0 && sent[k][9] == TIER_T2, "autotrain: tries the lowest unmet tier first (T2 before T4)");

		// T2 refused (e.g. a resource shortage) - reply carries kind/tier/amount now
		uint8_t refuseT2[7] = { 2, TROOP_INFANTRY, TIER_T2, 0, 0, 0, 0 };
		RecvTrainingStart(c, refuseT2, sizeof(refuseT2));
		CHECK(!c->autotrain.busy, "autotrain: a refusal frees the account-wide busy flag");
		uint64_t short_backoff = c->autotrain.retry_at[TROOP_INFANTRY][TIER_T2];
		CHECK(short_backoff > 0 && short_backoff < now_ms() + 5 * 60 * 1000,
			"autotrain: the first refusals on a tier use a short backoff (assumed transient)");

		c->autotrain.next_action_at = 0;
		reset_sent();
		AutoTrainTick(c); // T2 backed off for this kind - T4 (the next unmet tier) is not
		k = find_packet(_MSG_REQUEST_TRAINING_);
		CHECK(k >= 0 && sent[k][9] == TIER_T4,
			"autotrain: moves on to the next unmet tier while the lowest one is backed off");

		// a tier that is never going to succeed must not be hammered forever at the short
		// backoff - after enough refusals in a row on that SAME tier, the backoff grows a lot,
		// without ever giving up on it for the whole run (a shortage could resolve later)
		for (int i = 0; i < AUTOTRAIN_HARD_BLOCK_THRESHOLD - 1; i++)
			RecvTrainingStart(c, refuseT2, sizeof(refuseT2));
		uint64_t long_backoff = c->autotrain.retry_at[TROOP_INFANTRY][TIER_T2];
		CHECK(long_backoff > now_ms() + 5 * 60 * 1000,
			"autotrain: enough consecutive refusals on one tier switch to a long backoff for that tier");

		// an accepted order on T2 resets ONLY T2's streak, so a later real transient refusal
		// there is not immediately treated as another hard block
		uint8_t acceptT2[7] = { 0, TROOP_INFANTRY, TIER_T2, 0, 0, 0, 0 };
		RecvTrainingStart(c, acceptT2, sizeof(acceptT2));
		CHECK(c->autotrain.consecutive_refusals[TROOP_INFANTRY][TIER_T2] == 0,
			"autotrain: an accepted order resets that tier's refusal streak");
		free(c);
	}

	/* ---- autotrain: a refusal with no kind/tier in it (just the status byte) is still matched
	 * to the right (kind, tier) via what AutoTrainTick itself remembers sending, instead of
	 * leaving the account-wide slot stuck busy forever ---- */
	{
		c = fresh("boss");
		c->troop.loaded = true;
		c->autotrain.enabled = true;
		give_stock(c);
		c->autotrain.target[TROOP_INFANTRY][TIER_T2] = 5000000;
		c->autotrain.target[TROOP_RANGED][TIER_T2]   = 5000000;
		c->autotrain.target[TROOP_CAVALRY][TIER_T2]  = 5000000;

		// infantry T2: sent, then accepted for far less than asked (real capture: asked
		// 3,701,309, server granted 29) - a full-size response, decoded normally
		AutoTrainTick(c);
		uint8_t acceptInf[7] = { 0, TROOP_INFANTRY, TIER_T2, 29, 0, 0, 0 };
		RecvTrainingStart(c, acceptInf, sizeof(acceptInf));
		CHECK(c->training[TROOP_INFANTRY].amount == 29, "autotrain: a full response is decoded normally regardless of how small the granted amount is");
		CHECK(c->autotrain.busy, "autotrain: an accepted order keeps the account-wide slot busy until it actually finishes");

		// infantry's small batch finishes, freeing the slot for ranged: sent, then refused with
		// ONLY a status byte (no kind/tier/amount at all - confirmed live)
		uint8_t addsoldierInf[6] = { TROOP_INFANTRY, TIER_T2, 0x1D, 0x00, 0x00, 0x00 }; // 29 LE
		RecvAddSoldier(c, addsoldierInf, sizeof(addsoldierInf));
		c->autotrain.next_action_at = 0;
		reset_sent();
		AutoTrainTick(c);
		int k = find_packet(_MSG_REQUEST_TRAINING_);
		CHECK(k >= 0 && sent[k][8] == TROOP_RANGED, "autotrain: round-robin moves on to ranged next");
		CHECK(c->autotrain.busy, "autotrain: ranged's order marks the account-wide slot busy");
		uint8_t bareRefusal[1] = { 1 };
		RecvTrainingStart(c, bareRefusal, sizeof(bareRefusal));
		CHECK(!c->autotrain.busy,
			"autotrain: a bare status-only refusal still frees the slot instead of leaving it stuck forever");
		CHECK(c->autotrain.consecutive_refusals[TROOP_RANGED][TIER_T2] == 1,
			"autotrain: the bare refusal is counted against the correct (kind, tier) for backoff");

		// cavalry T2: same bare-refusal shape, proving this keeps working for a second (kind,
		// tier) in a row rather than only the first time
		c->autotrain.next_action_at = 0;
		reset_sent();
		AutoTrainTick(c);
		k = find_packet(_MSG_REQUEST_TRAINING_);
		CHECK(k >= 0 && sent[k][8] == TROOP_CAVALRY, "autotrain: round-robin moves on to cavalry next");
		CHECK(c->autotrain.busy, "autotrain: cavalry's order marks the account-wide slot busy");
		RecvTrainingStart(c, bareRefusal, sizeof(bareRefusal));
		CHECK(!c->autotrain.busy && c->autotrain.consecutive_refusals[TROOP_CAVALRY][TIER_T2] == 1,
			"autotrain: correctly attributes a second consecutive bare refusal to cavalry, not ranged again");
		free(c);
	}

	/* ---- autotrain: never requests the raw (possibly 7-digit) gap outright - starts from a
	 * modest guess and grows from what was actually granted, instead of looking non-human ---- */
	{
		c = fresh("boss");
		c->troop.loaded = true;
		c->autotrain.enabled = true;
		give_stock(c);
		c->autotrain.target[TROOP_INFANTRY][TIER_T2] = 5000000; // real capture: bot used to ask for 3,701,309 in one shot

		AutoTrainTick(c);
		int k = find_packet(_MSG_REQUEST_TRAINING_);
		uint32_t amount = k >= 0 ? (uint32_t)(sent[k][10] | sent[k][11] << 8 | sent[k][12] << 16 | sent[k][13] << 24) : 0;
		CHECK(amount == AUTOTRAIN_INITIAL_BATCH_GUESS,
			"autotrain: first-ever request for a (kind, tier) uses the modest initial guess, not the full multi-million gap");

		// granted far less than asked (real capture: 29) - the next request grows from THAT, not
		// back up to the full gap
		uint8_t accept[7] = { 0, TROOP_INFANTRY, TIER_T2, 29, 0, 0, 0 };
		RecvTrainingStart(c, accept, sizeof(accept));
		uint8_t addsoldier[6] = { TROOP_INFANTRY, TIER_T2, 29, 0, 0, 0 };
		RecvAddSoldier(c, addsoldier, sizeof(addsoldier));

		c->autotrain.next_action_at = 0;
		reset_sent();
		AutoTrainTick(c);
		k = find_packet(_MSG_REQUEST_TRAINING_);
		amount = k >= 0 ? (uint32_t)(sent[k][10] | sent[k][11] << 8 | sent[k][12] << 16 | sent[k][13] << 24) : 0;
		CHECK(amount == 43, // 29 * 3/2, integer division
			"autotrain: the next request grows ~50 percent from what was actually granted last time, not the raw gap");

		// resolve infantry's 2nd request (43 accepted in full) so the account-wide slot frees up
		uint8_t accept2[7] = { 0, TROOP_INFANTRY, TIER_T2, 43, 0, 0, 0 };
		RecvTrainingStart(c, accept2, sizeof(accept2));
		uint8_t addsoldier2[6] = { TROOP_INFANTRY, TIER_T2, 43, 0, 0, 0 };
		RecvAddSoldier(c, addsoldier2, sizeof(addsoldier2));

		// a totally different kind's FIRST-EVER request must already use what was learned from
		// infantry, not restart from the modest initial guess - confirmed live: the account's
		// own owner reports the same cap applies to every kind and tier
		c->autotrain.target[TROOP_INFANTRY][TIER_T2] = 0; // done needing infantry - isolate ranged's request
		c->autotrain.target[TROOP_RANGED][TIER_T2] = 5000000;
		c->autotrain.next_action_at = 0;
		reset_sent();
		AutoTrainTick(c);
		k = find_packet(_MSG_REQUEST_TRAINING_);
		amount = k >= 0 ? (uint32_t)(sent[k][10] | sent[k][11] << 8 | sent[k][12] << 16 | sent[k][13] << 24) : 0;
		CHECK(k >= 0 && sent[k][8] == TROOP_RANGED && amount == 64, // 43 * 3/2, integer division - grown from infantry's history
			"autotrain: the learned cap is account-wide - a different kind's first-ever request already uses it, not the initial guess");
		free(c);
	}

	/* ---- gather: a tile with a player's name/tag embedded is occupied, never targeted ---- */
	{
		c = fresh("boss");
		c->gather.enabled = true;
		c->gather.max_marches = 1;
		c->gather.scan_done = true;
		c->player.max_marches = 6;
		c->player.zone_id = 100; c->player.point_id = 0; // castle inside zone 100, same as the test tiles - keeps them in GatherZoneInRange's rectangle

		// Two ORE tiles in one _MSG_RESP_UPDATE_MAPINFO_PLUS push: point 5 (level 4, higher -
		// would normally win) has a name+tag embedded like a real capture showed for an
		// occupied tile; point 6 (level 3, lower) has an all-zero name field, i.e. free.
		uint8_t buf[3 + 51 * 2];
		memset(buf, 0, sizeof(buf));
		uint8_t *r0 = buf + 3;
		r0[0] = 100; r0[1] = 0; r0[2] = 5; r0[3] = 3;
		memcpy(r0 + 4, "Hentai Man", 10);
		memcpy(r0 + 17, "TR4", 3);
		r0[22] = 4;
		r0[23] = 0x68; r0[24] = 0x6b; r0[25] = 0x0e; r0[26] = 0x00; // 945000

		uint8_t *r1 = buf + 3 + 51;
		r1[0] = 100; r1[1] = 0; r1[2] = 6; r1[3] = 3;
		r1[22] = 3;
		r1[23] = 0x80; r1[24] = 0xfc; r1[25] = 0x0a; r1[26] = 0x00; // 720000

		RecvMapInfoPlus(c, buf, sizeof(buf));

		bool found_occupied = false, found_free = false;
		for (int i = 0; i < c->gather.tile_count; i++) {
			GatherTile *t = &c->gather.tiles[i];
			if (t->point_id == 5) {
				found_occupied = true;
				CHECK(t->occupied && strcmp(t->occupied_by, "Hentai Man") == 0,
					"gather: a tile with a name embedded is marked occupied, name decoded");
			}
			if (t->point_id == 6) {
				found_free = true;
				CHECK(!t->occupied, "gather: a tile with an all-zero name field is not occupied");
			}
		}
		CHECK(found_occupied && found_free, "gather: both tiles tracked from the map data push");

		reset_sent();
		c->gather.next_march_at = 1;
		GatherTick(c);
		int adv = find_packet(_MSG_REQUEST_MAP_ADVANCE);
		CHECK(adv >= 0 && c->gather.pending_tile != GATHER_NO_PENDING_TILE
			&& c->gather.tiles[c->gather.pending_tile].point_id == 6,
			"gather: picks the free, lower-level tile over the occupied, higher-level one");

		// the occupier leaves: a fresh push with the name zeroed clears the flag
		memset(r0 + 4, 0, 13);
		RecvMapInfoPlus(c, buf, sizeof(buf));
		for (int i = 0; i < c->gather.tile_count; i++) {
			if (c->gather.tiles[i].point_id == 5)
				CHECK(!c->gather.tiles[i].occupied, "gather: a tile is freed again once its name field clears");
		}
		free(c);
	}

	/* ---- gather: every known tile occupied backs off with a warning instead of retrying
	 * silently forever (real report: 34/34 tiles occupied, bot looked "stuck" with no log) ---- */
	{
		c = fresh("boss");
		c->gather.enabled = true;
		c->gather.max_marches = 1;
		c->gather.scan_done = true;
		c->player.max_marches = 6;
		c->player.zone_id = 100; c->player.point_id = 0; // castle inside zone 100, same as the test tile

		uint8_t buf[3 + 51];
		memset(buf, 0, sizeof(buf));
		uint8_t *r0 = buf + 3;
		r0[0] = 100; r0[1] = 0; r0[2] = 5; r0[3] = 3;
		memcpy(r0 + 4, "Someone", 7); // any non-zero name marks it occupied
		r0[22] = 4;
		r0[23] = 0x68; r0[24] = 0x6b; r0[25] = 0x0e; r0[26] = 0x00; // 945000
		RecvMapInfoPlus(c, buf, sizeof(buf));
		CHECK(c->gather.tile_count == 1 && c->gather.tiles[0].occupied,
			"gather: setup - the only known tile is occupied");

		reset_sent();
		c->gather.next_march_at = 1; // pacing already elapsed
		GatherTick(c);
		CHECK(find_packet(_MSG_REQUEST_MAP_ADVANCE) < 0 && find_packet(_MSG_REQUEST_TROOPMARCH_NOTATK) < 0,
			"gather: nothing sent when every known tile is occupied");
		CHECK(c->gather.next_march_at > now_ms() + 20000,
			"gather: backs off for a while instead of retrying every tick with no pacing at all");
		free(c);
	}

	/* ---- gather: the tile scan reruns periodically instead of trusting passive pushes forever
	 * (real report: a tile stayed marked occupied for 7+ minutes after the occupier presumably
	 * left, with no passive push ever correcting it) ---- */
	{
		c = fresh("boss");
		c->gather.enabled = true;
		c->gather.scan_done = true;
		c->player.max_marches = 6;

		reset_sent();
		GatherTick(c);
		CHECK(c->gather.scan_done, "gather: reaching scan_done for the first time only arms the rescan timer, does not rescan instantly");
		CHECK(find_packet(_MSG_REQUEST_MAPDATA) < 0, "gather: no rescan request sent while the interval has not elapsed yet");

		c->gather.next_rescan_at = 1; // simulate the interval having elapsed
		reset_sent();
		GatherTick(c);
		CHECK(!c->gather.scan_done && c->gather.scan_cursor == 0,
			"gather: once the interval elapses, scan_done resets and the scan restarts from the beginning");

		// a march mid-send (pending_tile) must never be interrupted by a rescan
		c = fresh("boss");
		c->gather.enabled = true;
		c->gather.scan_done = true;
		c->gather.next_rescan_at = 1;
		c->gather.pending_tile = 0;
		c->gather.tile_count = 1;
		c->gather.tiles[0].used = true;
		c->player.max_marches = 6;
		c->gather.march_send_at = now_ms() + 999999; // still waiting out the pacing delay
		GatherTick(c);
		CHECK(c->gather.scan_done, "gather: a rescan never interrupts a march that is still mid-send");
		free(c);
	}

	/* ---- gather: a resource tile outside the scan's own rectangle is not tracked, but its
	 * kingdom_id is irrelevant to that decision ---- */
	{
		// Confirmed live (real capture) that a genuinely free tile decodes kingdom_id=0 (no
		// occupier to report one for) while an occupied one decodes the occupier's real
		// kingdom - an earlier version of this code filtered resource tiles on kingdom_id
		// matching the player's own, which silently dropped every free tile it ever saw and
		// only ever kept occupied ones. This test locks in that a free tile (kingdom_id=0) is
		// tracked exactly like an occupied one would be, as long as it's within the scan's own
		// rectangle - kingdom_id plays no part in that decision anymore.
		c = fresh("boss");
		c->gather.enabled = true;
		c->player.zone_id = 100; c->player.point_id = 0; // castle inside zone 100

		uint8_t buf[3 + 51 * 2];
		memset(buf, 0, sizeof(buf));

		// point 5: inside the scan rectangle (same zone as the castle), free (kingdom_id=0,
		// no occupier) - must still be tracked
		uint8_t *r0 = buf + 3;
		r0[0] = 100; r0[1] = 0; r0[2] = 5; r0[3] = 3;
		r0[22] = 4;
		r0[23] = 0x68; r0[24] = 0x6b; r0[25] = 0x0e; r0[26] = 0x00; // 945000

		// point 6: a zone far outside any plausible scan radius of the castle - must be rejected
		uint8_t *r1 = buf + 3 + 51;
		r1[0] = 0xE8; r1[1] = 0x03; r1[2] = 6; r1[3] = 3; // zone_id = 1000
		r1[22] = 1;
		r1[23] = 0x60; r1[24] = 0x0e; r1[25] = 0x03; r1[26] = 0x00; // 200000

		RecvMapInfoPlus(c, buf, sizeof(buf));

		bool found5 = false, found6 = false;
		for (int i = 0; i < c->gather.tile_count; i++) {
			if (c->gather.tiles[i].point_id == 5) found5 = true;
			if (c->gather.tiles[i].point_id == 6) found6 = true;
		}
		CHECK(found5, "gather: a free tile (kingdom_id=0) inside the scan rectangle is tracked");
		CHECK(!found6, "gather: a tile far outside the scan rectangle is not tracked");
		free(c);
	}

	/* ---- $heal: heals every wounded troop at once ---- */
	{
		c = fresh("boss");
		c->wounded.loaded = true;
		c->wounded.troop.infantry[0] = 11558; c->wounded.troop.total += 11558;
		c->wounded.troop.ranged[0]   = 11557; c->wounded.troop.total += 11557;
		c->wounded.troop.cavalry[0]  = 11557; c->wounded.troop.total += 11557;
		// siege left at 0, matching the real capture

		reset_sent();
		say(c, "eve", "$heal", COMMAND_CHANNEL_MAIL);
		CHECK(find_packet(_MSG_REQUEST_HEALINGTROOP) < 0, "heal: a non-administrator cannot trigger it");

		reset_sent();
		say(c, "boss", "$heal", COMMAND_CHANNEL_MAIL);
		int idx = find_packet(_MSG_REQUEST_HEALINGTROOP);
		CHECK(idx >= 0, "heal: an administrator triggers _MSG_REQUEST_HEALINGTROOP");
		if (idx >= 0) {
			const uint8_t *p = (const uint8_t*)sent[idx] + 4; // payload start
			uint32_t infantry = (uint32_t)p[8]  | (uint32_t)p[9]  << 8 | (uint32_t)p[10] << 16 | (uint32_t)p[11] << 24;
			uint32_t ranged   = (uint32_t)p[24] | (uint32_t)p[25] << 8 | (uint32_t)p[26] << 16 | (uint32_t)p[27] << 24;
			uint32_t cavalry  = (uint32_t)p[40] | (uint32_t)p[41] << 8 | (uint32_t)p[42] << 16 | (uint32_t)p[43] << 24;
			uint32_t siege    = (uint32_t)p[56] | (uint32_t)p[57] << 8 | (uint32_t)p[58] << 16 | (uint32_t)p[59] << 24;
			CHECK(infantry == 11558 && ranged == 11557 && cavalry == 11557 && siege == 0,
				"heal: the 4 per-kind wounded totals land in the confirmed slot offsets (8/24/40/56)");
		}
		free(c);
	}

	/* ---- $revive: starts a free (divine) resurrection for every dead troop at once ---- */
	{
		c = fresh("boss");
		c->valhalla.loaded = true;
		c->valhalla.dead[TROOP_INFANTRY] = 259821;
		c->valhalla.dead[TROOP_RANGED]   = 388779;
		c->valhalla.dead[TROOP_CAVALRY]  = 601400;
		c->valhalla.dead[TROOP_SIEGE]    = 0;

		reset_sent();
		say(c, "eve", "$revive", COMMAND_CHANNEL_MAIL);
		CHECK(find_packet(_MSG_REQUEST_VALHALLA_DIVINE_REVIVE) < 0, "revive: a non-administrator cannot trigger it");

		reset_sent();
		say(c, "boss", "$revive", COMMAND_CHANNEL_MAIL);
		int idx = find_packet(_MSG_REQUEST_VALHALLA_DIVINE_REVIVE);
		CHECK(idx >= 0, "revive: an administrator triggers _MSG_REQUEST_VALHALLA_DIVINE_REVIVE");
		if (idx >= 0) {
			const uint8_t *p = (const uint8_t*)sent[idx] + 4;
			uint32_t infantry = (uint32_t)p[8]  | (uint32_t)p[9]  << 8 | (uint32_t)p[10] << 16 | (uint32_t)p[11] << 24;
			uint32_t ranged   = (uint32_t)p[24] | (uint32_t)p[25] << 8 | (uint32_t)p[26] << 16 | (uint32_t)p[27] << 24;
			uint32_t cavalry  = (uint32_t)p[40] | (uint32_t)p[41] << 8 | (uint32_t)p[42] << 16 | (uint32_t)p[43] << 24;
			CHECK(infantry == 259821 && ranged == 388779 && cavalry == 601400,
				"revive: the 4 per-kind dead totals land in the confirmed slot offsets, matching a real capture");
		}

		reset_sent();
		c->valhalla.dead[0] = c->valhalla.dead[1] = c->valhalla.dead[2] = c->valhalla.dead[3] = 0;
		say(c, "boss", "$revive", COMMAND_CHANNEL_MAIL);
		CHECK(find_packet(_MSG_REQUEST_VALHALLA_DIVINE_REVIVE) < 0, "revive: nothing sent when there is nobody to revive");
		free(c);
	}

	/* ---- RecvValhallaInfo: decodes the 4 per-kind dead-troop totals at their confirmed offsets ---- */
	{
		c = fresh("boss");
		uint8_t buf[200];
		memset(buf, 0, sizeof(buf));
		buf[88]  = 0xed; buf[89] = 0xf6; buf[90] = 0x03; // 259821, TROOP_INFANTRY
		buf[104] = 0xab; buf[105] = 0xee; buf[106] = 0x05; // 388779, TROOP_RANGED
		buf[120] = 0x38; buf[121] = 0x2d; buf[122] = 0x09; // 601400, TROOP_CAVALRY
		// TROOP_SIEGE slot (offset 136) left at 0, matching the real capture

		RecvValhallaInfo(c, buf, sizeof(buf));
		CHECK(c->valhalla.loaded, "RecvValhallaInfo: marks itself loaded");
		CHECK(c->valhalla.dead[TROOP_INFANTRY] == 259821 && c->valhalla.dead[TROOP_RANGED] == 388779
			&& c->valhalla.dead[TROOP_CAVALRY] == 601400 && c->valhalla.dead[TROOP_SIEGE] == 0,
			"RecvValhallaInfo: the 4 per-kind dead totals decode at the confirmed 16-byte stride from offset 88");
		free(c);
	}

	printf("%s\n", failures ? "SOME TESTS FAILED" : "ALL BOT LOGIC TESTS PASSED");
	return failures ? 1 : 0;
}
