/*
 * Configuration parser.
 *
 * Each configuration key is matched using a simple if/return chain.
 *
 * Although a lookup table or hash map could also be used, this function is
 * only executed once during application startup when the configuration file
 * is loaded. It is never called from the main game loop, network packet
 * processing, or any performance-critical code.
 *
 * Because the parser runs only once, the cost of multiple strcmp() calls is
 * negligible compared to the overall initialization time. This approach keeps
 * the code straightforward, easy to read, and simple to extend—adding a new
 * configuration option only requires adding another comparison.
 *
 * Prefer clarity and maintainability here over unnecessary optimization.
 */

#include "config.h"
#include "command.h"
#include "log.h"
#include <stdlib.h>

#include <ctype.h>

static CommandChannel ParseCommandChannel(const char *value)
{
    if (strcmp(value, "WORLD") == 0)
        return COMMAND_CHANNEL_WORLD;

    if (strcmp(value, "GUILD") == 0)
        return COMMAND_CHANNEL_GUILD;

    if (strcmp(value, "MAIL") == 0)
        return COMMAND_CHANNEL_MAIL;

    return COMMAND_CHANNEL_GUILD; // default
}


static uint16_t ParseShield(const char *value)
{
    if (strcmp(value, "SHIELD_4H") == 0)
        return SHIELD_4H;

    if (strcmp(value, "SHIELD_8H") == 0)
        return SHIELD_8H;

    if (strcmp(value, "SHIELD_12H") == 0)
        return SHIELD_12H;

    if (strcmp(value, "SHIELD_1D") == 0)
        return SHIELD_1D;

    if (strcmp(value, "SHIELD_3D") == 0)
        return SHIELD_3D;

    if (strcmp(value, "SHIELD_7D") == 0)
        return SHIELD_7D;

    if (strcmp(value, "SHIELD_14D") == 0)
        return SHIELD_14D;

    return 0;
}

static bool ParseShieldPriority(Connection *c, const char *value)
{
    char buffer[512];

    strncpy(buffer, value, sizeof(buffer));
    buffer[sizeof(buffer) - 1] = '\0';

    int count = 0;

    char *token = strtok(buffer, ",");
    
    while (token && count < 8) {
        while (*token == ' ')
            token++;

        uint16_t shield = ParseShield(token);

        if (shield == 0) {
            printf("Invalid shield priority value: %s\n", token);
            return false;
        }

        c->protection.shield_priority[count++] = shield;

        token = strtok(NULL, ",");
    }

    c->protection.shield_priority_count = count;
    return true;
}

static uint16_t ParseAntiScout(const char *value)
{
    if (strcmp(value, "ANTISCOUT_4H") == 0)
        return ANTISCOUT_4H;

    if (strcmp(value, "ANTISCOUT_8H") == 0)
        return ANTISCOUT_8H;

    if (strcmp(value, "ANTISCOUT_1D") == 0)
        return ANTISCOUT_1D;

    if (strcmp(value, "ANTISCOUT_3D") == 0)
        return ANTISCOUT_3D;

    if (strcmp(value, "ANTISCOUT_7D") == 0)
        return ANTISCOUT_7D;

    return 0;
}

static bool ParseAntiScoutPriority(Connection *c, const char *value)
{
    char buffer[512];

    strncpy(buffer, value, sizeof(buffer));
    buffer[sizeof(buffer) - 1] = '\0';

    int count = 0;

    char *token = strtok(buffer, ",");

    while (token && count < 8) {
        while (*token == ' ')
            token++;

        uint16_t item = ParseAntiScout(token);

        if (item == 0) {
            printf("Invalid anti-scout priority value: %s\n", token);
            return false;
        }

        c->protection.antiscout_priority[count++] = item;

        token = strtok(NULL, ",");
    }

    c->protection.antiscout_priority_count = count;
    return true;
}

/*
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
*/

static bool ParseBool(const char *value)
{
    return strcmp(value, "true") == 0 || strcmp(value, "1") == 0;
}

/* "WORLD, GUILD, MAIL" -> bit mask of 1 << CommandChannel. At least one valid channel is required. */
static bool ParseChannelMask(const char *value, uint8_t *mask)
{
    char buffer[128];
    uint8_t result = 0;

    strncpy(buffer, value, sizeof(buffer));
    buffer[sizeof(buffer) - 1] = '\0';

    for (char *token = strtok(buffer, ","); token; token = strtok(NULL, ",")) {
        while (*token == ' ')
            token++;

        char *end = token + strlen(token);

        while (end > token && end[-1] == ' ')
            *--end = '\0';

        if (strcmp(token, "WORLD") == 0)
            result |= 1u << COMMAND_CHANNEL_WORLD;
        else if (strcmp(token, "GUILD") == 0)
            result |= 1u << COMMAND_CHANNEL_GUILD;
        else if (strcmp(token, "MAIL") == 0)
            result |= 1u << COMMAND_CHANNEL_MAIL;
        else {
            printf("Invalid command channel: %s (use WORLD, GUILD or MAIL)\n", token);
            return false;
        }
    }

    if (result == 0)
        return false;

    *mask = result;
    return true;
}

/* "name1, name2" -> administrators (admin.name and admin.names may both be used and are merged). */
static bool ParseAdminList(Connection *c, const char *value)
{
    char buffer[256];

    strncpy(buffer, value, sizeof(buffer));
    buffer[sizeof(buffer) - 1] = '\0';

    for (char *token = strtok(buffer, ","); token; token = strtok(NULL, ",")) {
        while (*token == ' ')
            token++;

        char *end = token + strlen(token);

        while (end > token && end[-1] == ' ')
            *--end = '\0';

        if (*token == '\0')
            continue;

        if (!AdminAdd(c, token)) {
            printf("Invalid administrator name: %s (1 to 12 characters, at most %d administrators)\n", token, MAX_ADMINS);
            return false;
        }
    }

    return true;
}

static bool ParseTroopKind(const char *s, uint8_t *out)
{
    if (strcmp(s, "INFANTRY") == 0) { *out = TROOP_INFANTRY; return true; }
    if (strcmp(s, "RANGED") == 0 || strcmp(s, "SNIPER") == 0) { *out = TROOP_RANGED; return true; }
    if (strcmp(s, "CAVALRY") == 0) { *out = TROOP_CAVALRY; return true; }
    if (strcmp(s, "SIEGE") == 0) { *out = TROOP_SIEGE; return true; }
    return false;
}

/* "INFANTRY, RANGED, CAVALRY, SIEGE" - which kind's slot a gather march fills, tried in this
 * order at send time (see GatherSettings' kind_priority comment): falls back to the next kind
 * if the previous one has no troops free right now. Each kind at most once. */
static bool ParseGatherKindPriority(Connection *c, const char *value)
{
    char buffer[128];
    strncpy(buffer, value, sizeof(buffer));
    buffer[sizeof(buffer) - 1] = '\0';

    c->gather.kind_priority_count = 0;
    bool seen[4] = {0};

    for (char *token = strtok(buffer, ","); token; token = strtok(NULL, ",")) {
        while (*token == ' ')
            token++;
        char *end = token + strlen(token);
        while (end > token && end[-1] == ' ')
            *--end = '\0';
        if (*token == '\0')
            continue;

        uint8_t kind;
        if (!ParseTroopKind(token, &kind)) {
            printf("Invalid gather.kind entry: %s (expected INFANTRY, RANGED, CAVALRY or SIEGE)\n", token);
            return false;
        }
        if (seen[kind]) {
            printf("Duplicate gather.kind entry: %s\n", token);
            return false;
        }
        if (c->gather.kind_priority_count >= 4) {
            printf("Too many gather.kind entries (max 4)\n");
            return false;
        }
        seen[kind] = true;
        c->gather.kind_priority[c->gather.kind_priority_count++] = kind;
    }

    return true;
}

uint64_t parse_number_u64(const char *str); // command.c - accepts a plain number or one with a K/M/B suffix

/* One box of the 4x5 grid (see AutoTrainSettings' comment): autotrain.<kind>_t<N> = target,
 * accepts K/M/B suffixes like the bank/cargo_ship reserve settings (e.g. 10M). */
static void SetAutoTrainTarget(Connection *c, uint8_t kind, uint8_t tier, const char *value)
{
    c->autotrain.target[kind][tier] = (uint32_t)parse_number_u64(value);
}

static bool ParserConfig(Connection *c, const char *key, const char *value) {
	// gateway server 
	if (strcmp(key, "server.addr") == 0) {
		strncpy(c->gateway_server.addr, value, 16);
		return true;
	}
	
	if (strcmp(key, "server.port") == 0) {
		c->gateway_server.port = (uint16_t)strtoul(value, NULL, 10);
		return true;
	}
	
	// reconnect
	if (strcmp(key, "reconnect.enabled") == 0) {
		c->reconnect.enabled = (strcmp(value, "true") == 0 || strcmp(value, "1") == 0);
		return true;
	}
	
	if (strcmp(key, "reconnect.delay") == 0) {
		c->reconnect.delay = (uint32_t)strtoul(value, NULL, 10);
		return true;
	}
	
	if (strcmp(key, "reconnect.kicked_delay") == 0) {
		c->reconnect.kicked_delay = (uint32_t)strtoul(value, NULL, 10);
		return true;
	}
	
	if (strcmp(key, "migration.scrolls_needed") == 0) {
		uint32_t needed = (uint32_t)strtoul(value, NULL, 10);
		
		if (needed < 1 || needed > 9999)
			return false;
		
		c->migration_scrolls_needed = (uint16_t)needed;
		return true;
	}
	
	if (strcmp(key, "reconnect.max_attempts") == 0) {
		c->reconnect.max_attempts = (uint32_t)strtoul(value, NULL, 10);
		return true;
	}
	
	// logging
	if (strcmp(key, "log.debug") == 0) {
		if (!g_log_debug_forced)
			g_log_debug = (strcmp(value, "true") == 0 || strcmp(value, "1") == 0);
		return true;
	}
	
	if (strcmp(key, "log.packets") == 0) {
		g_log_packets = (strcmp(value, "true") == 0 || strcmp(value, "1") == 0);
		return true;
	}
	
	// client version 
	if (strcmp(key, "client.version_major") == 0) {
		c->app.version_major = (uint8_t)strtoul(value, NULL, 10);
		return true;
	}
	
	if (strcmp(key, "client.version_minor") == 0) {
		c->app.version_minor = (uint8_t)strtoul(value, NULL, 10);
		return true;
	}
	
	if (strcmp(key, "client.version_patch") == 0) {
		c->app.version_patch = (uint16_t)strtoul(value, NULL, 10);
		return true;
	}
	
	if (strcmp(key, "client.platform") == 0) {
		c->app.platform = (uint8_t)strtoul(value, NULL, 10);
		return true;
	}

	if (strcmp(key, "client.language_code") == 0) {
		c->app.language_code = (uint8_t)strtoul(value, NULL, 10);
		return true;
	}
	
	// login 
	if (strcmp(key, "account.igg_id") == 0) {
		c->auth.igg_id = (uint64_t)strtoull(value, NULL, 10);
		return true;
	}
	
	if (strcmp(key, "account.device_uuid") == 0) {
		strncpy(c->auth.device_uuid, value, sizeof(c->auth.device_uuid));
		return true;
	}
	
	if (strcmp(key, "account.access_key") == 0) {
		strcpy(c->auth.session, value);
		c->auth.session_len = (uint16_t)strlen(c->auth.session);
		return true;
	}
	
	// command 
	if (strcmp(key, "command.input") == 0) {
		return ParseChannelMask(value, &c->bot.command_input_mask);
	}
	
	if (strcmp(key, "command.output") == 0) {
		c->bot.command_output = ParseCommandChannel(value);
		return true;
	}
	
	if (strcmp(key, "command.prefix") == 0) {
		if (value[0] != '\0') {
			c->bot.command_prefix = value[0];
		}
		return true;
	}
	
	
	if (strcmp(key, "alliance.auto_help") == 0) {
		c->alliance.auto_help = (strcmp(value, "true") == 0);
		return true;
	}
	
	if (strcmp(key, "alliance.auto_open_gifts") == 0) {
		c->alliance.auto_open_gifts = (strcmp(value, "true") == 0);
		return true;
	}

	if (strcmp(key, "activity.auto_double_ticket") == 0) {
		c->activity.auto_double_ticket = (strcmp(value, "true") == 0);
		return true;
	}

	if (strcmp(key, "activity.auto_online_gift") == 0) {
		c->activity.auto_online_gift = (strcmp(value, "true") == 0);
		return true;
	}

	if (strcmp(key, "war.enabled") == 0) {
		c->war.enabled = (strcmp(value, "true") == 0);
		return true;
	}

	// Old key, kept as an alias: it used to be war-only, now every notify.on_* feature
	// shares the same webhook URL (notify.discord_webhook, below).
	if (strcmp(key, "war.discord_webhook") == 0) {
		strncpy(c->notify.discord_webhook, value, sizeof(c->notify.discord_webhook) - 1);
		c->notify.discord_webhook[sizeof(c->notify.discord_webhook) - 1] = '\0';
		return true;
	}

	if (strcmp(key, "notify.discord_webhook") == 0) {
		strncpy(c->notify.discord_webhook, value, sizeof(c->notify.discord_webhook) - 1);
		c->notify.discord_webhook[sizeof(c->notify.discord_webhook) - 1] = '\0';
		return true;
	}

	if (strcmp(key, "notify.on_war") == 0) {
		c->notify.on_war = (strcmp(value, "true") == 0);
		return true;
	}

	if (strcmp(key, "notify.on_antiscout_report") == 0) {
		c->notify.on_antiscout_report = (strcmp(value, "true") == 0);
		return true;
	}

	if (strcmp(key, "notify.on_shield_expiring") == 0) {
		c->notify.on_shield_expiring = (strcmp(value, "true") == 0);
		return true;
	}

	if (strcmp(key, "notify.on_antiscout_expiring") == 0) {
		c->notify.on_antiscout_expiring = (strcmp(value, "true") == 0);
		return true;
	}

	if (strcmp(key, "notify.on_transfer_done") == 0) {
		c->notify.on_transfer_done = (strcmp(value, "true") == 0);
		return true;
	}

	if (strcmp(key, "gather.enabled") == 0) {
		c->gather.enabled = (strcmp(value, "true") == 0);
		return true;
	}

	if (strcmp(key, "guildbank.enabled") == 0) {
		c->guildbank.enabled = (strcmp(value, "true") == 0);
		return true;
	}

	if (strcmp(key, "recall.pause_seconds") == 0) {
		c->recall.pause_seconds = (uint32_t)strtoul(value, NULL, 10);
		return true;
	}

	if (strcmp(key, "gather.max_marches") == 0) {
		c->gather.max_marches = (uint8_t)strtoul(value, NULL, 10);
		return true;
	}

	if (strcmp(key, "autotrain.enabled") == 0) {
		c->autotrain.enabled = (strcmp(value, "true") == 0);
		return true;
	}

	// autotrain.<kind>_t<1-5> = target - one box of the 4x5 grid (see AutoTrainSettings' comment).
	// A small table + loop instead of 20 near-identical strcmp blocks - still a single, obvious
	// place to look, just without the copy-paste risk of typing out every combination by hand.
	if (strncmp(key, "autotrain.", 10) == 0) {
		static const struct { const char *name; uint8_t kind; } kinds[] = {
			{ "infantry", TROOP_INFANTRY }, { "ranged", TROOP_RANGED },
			{ "cavalry", TROOP_CAVALRY },   { "siege", TROOP_SIEGE },
		};
		const char *suffix = key + 10;
		for (size_t i = 0; i < sizeof(kinds) / sizeof(kinds[0]); i++) {
			size_t len = strlen(kinds[i].name);
			if (strncmp(suffix, kinds[i].name, len) != 0) continue;
			const char *tier_part = suffix + len;
			if (tier_part[0] != '_' || tier_part[1] != 't' || tier_part[3] != '\0') continue;
			if (tier_part[2] < '1' || tier_part[2] > '5') continue;
			SetAutoTrainTarget(c, kinds[i].kind, (uint8_t)(tier_part[2] - '1'), value);
			return true;
		}
	}

	if (strcmp(key, "gather.radius") == 0) {
		c->gather.radius = (uint16_t)strtoul(value, NULL, 10);
		return true;
	}

	if (strcmp(key, "gather.kind") == 0) {
		return ParseGatherKindPriority(c, value);
	}

	if (strcmp(key, "gather.max_troop_count") == 0) {
		c->gather.max_troop_count = (uint32_t)strtoul(value, NULL, 10);
		return true;
	}

if (strcmp(key, "protection.enabled") == 0) {
		c->protection.enabled = (strcmp(value, "true") == 0);
		return true;
	}
	
	if (strcmp(key, "protection.shield_always_on") == 0) {
		c->protection.shield_always_on = (strcmp(value, "true") == 0);
		return true;
	}
	
	if (strcmp(key, "protection.shield_on_incoming_attack") == 0) {
		c->protection.shield_on_incoming_attack = (strcmp(value, "true") == 0);
		return true;
	}
	
	if (strcmp(key, "protection.shield_on_incoming_scout") == 0) {
		c->protection.shield_on_incoming_scout = (strcmp(value, "true") == 0);
		return true;
	}
	
	if (strcmp(key, "protection.shield_priority") == 0) {
		return ParseShieldPriority(c, value);
	}

	if (strcmp(key, "protection.antiscout_always_on") == 0) {
		c->protection.antiscout_always_on = (strcmp(value, "true") == 0);
		return true;
	}

	if (strcmp(key, "protection.antiscout_on_no_shield") == 0) {
		c->protection.antiscout_on_no_shield = (strcmp(value, "true") == 0);
		return true;
	}

	if (strcmp(key, "protection.antiscout_priority") == 0) {
		return ParseAntiScoutPriority(c, value);
	}

	// data directory
	if (strcmp(key, "data.path") == 0) {
		strncpy(c->bot.data_path, value, sizeof(c->bot.data_path) - 1);
		c->bot.data_path[sizeof(c->bot.data_path) - 1] = '\0';
		return true;
	}

	// bank
	if (strcmp(key, "bank.enabled") == 0) {
		c->bank.enabled = ParseBool(value);
		return true;
	}

	if (strcmp(key, "bank.send_food") == 0) {
		c->bank.send_food = ParseBool(value);
		return true;
	}

	if (strcmp(key, "bank.send_rock") == 0) {
		c->bank.send_rock = ParseBool(value);
		return true;
	}

	if (strcmp(key, "bank.send_wood") == 0) {
		c->bank.send_wood = ParseBool(value);
		return true;
	}

	if (strcmp(key, "bank.send_ore") == 0) {
		c->bank.send_ore = ParseBool(value);
		return true;
	}

	if (strcmp(key, "bank.send_gold") == 0) {
		c->bank.send_gold = ParseBool(value);
		return true;
	}

	if (strcmp(key, "bank.reserve_food") == 0) {
		c->bank.reserve.food = (uint32_t)parse_number_u64(value);
		return true;
	}

	if (strcmp(key, "bank.reserve_rock") == 0) {
		c->bank.reserve.rock = (uint32_t)parse_number_u64(value);
		return true;
	}

	if (strcmp(key, "bank.reserve_wood") == 0) {
		c->bank.reserve.wood = (uint32_t)parse_number_u64(value);
		return true;
	}

	if (strcmp(key, "bank.reserve_ore") == 0) {
		c->bank.reserve.ore = (uint32_t)parse_number_u64(value);
		return true;
	}

	if (strcmp(key, "bank.reserve_gold") == 0) {
		c->bank.reserve.gold = (uint32_t)parse_number_u64(value);
		return true;
	}

	if (strcmp(key, "bank.max_delivery_distance") == 0) {
		c->bank.max_delivery_distance = (uint32_t)strtoul(value, NULL, 10);
		return true;
	}

	if (strcmp(key, "bank.delivery_tax_percent") == 0) {
		c->bank.delivery_tax_percent = strtod(value, NULL);
		return true;
	}

	if (strcmp(key, "bank.use_bag_rss") == 0) {
		c->bank.use_bag_rss = ParseBool(value);
		return true;
	}

	if (strcmp(key, "bank.use_bag_food") == 0) {
		c->bank.use_bag_food = ParseBool(value);
		return true;
	}

	if (strcmp(key, "bank.use_bag_rock") == 0) {
		c->bank.use_bag_rock = ParseBool(value);
		return true;
	}

	if (strcmp(key, "bank.use_bag_wood") == 0) {
		c->bank.use_bag_wood = ParseBool(value);
		return true;
	}

	if (strcmp(key, "bank.use_bag_ore") == 0) {
		c->bank.use_bag_ore = ParseBool(value);
		return true;
	}

	if (strcmp(key, "bank.use_bag_gold") == 0) {
		c->bank.use_bag_gold = ParseBool(value);
		return true;
	}

	// troop recall
	if (strcmp(key, "protection.recall_on_incoming_attack") == 0) {
		c->protection.recall_on_incoming_attack = ParseBool(value);
		return true;
	}

	if (strcmp(key, "protection.recall_on_incoming_scout") == 0) {
		c->protection.recall_on_incoming_scout = ParseBool(value);
		return true;
	}

	if (strcmp(key, "protection.recall_on_incoming_conflict") == 0) {
		c->protection.recall_on_incoming_conflict = ParseBool(value);
		return true;
	}

	if (strcmp(key, "admin.name") == 0 || strcmp(key, "admin.names") == 0) {
		return ParseAdminList(c, value);
	}
	
	// Cargo ship setting 
	if (strcmp(key, "cargo_ship.auto_trade") == 0) {
		if (strcmp(value, "true") != 0 && strcmp(value, "false") != 0) {
			return false;
		}
		
		c->market.settings.auto_trade = (strcmp(value, "true") == 0);
		return true;
	}
	
	// Spend resources 
	if (strcmp(key, "cargo_ship.spend_food") == 0) {
		c->market.settings.spend_food = (strcmp(value, "true") == 0);
		return true;
	}
	
	if (strcmp(key, "cargo_ship.spend_rock") == 0) {
		c->market.settings.spend_rock = (strcmp(value, "true") == 0);
		return true;
	}
	
	if (strcmp(key, "cargo_ship.spend_wood") == 0) {
		c->market.settings.spend_wood = (strcmp(value, "true") == 0);
		return true;
	}
	
	if (strcmp(key, "cargo_ship.spend_ore") == 0) {
		c->market.settings.spend_ore  = (strcmp(value, "true") == 0);
		return true;
	}
	
	if (strcmp(key, "cargo_ship.spend_gold") == 0) {
		c->market.settings.spend_gold  = (strcmp(value, "true") == 0);
		return true;
	}
	
	// Use resources from bag
	if (strcmp(key, "cargo_ship.use_bag_rss") == 0) {
		c->market.settings.use_bag_rss  = (strcmp(value, "true") == 0);
		return true;
	}
	
	
	// Reserved resources 
	if (strcmp(key, "cargo_ship.reserve_food") == 0) {
		c->market.reserve.food  = (uint32_t)parse_number_u64(value);
		// printf("cargo_ship.reserve_food: %lu\n", c->market.reserve.food);
		return true;
	}
	
	if (strcmp(key, "cargo_ship.reserve_rock") == 0) {
		c->market.reserve.rock  = (uint32_t)parse_number_u64(value);
		return true;
	}
	
	if (strcmp(key, "cargo_ship.reserve_wood") == 0) {
		c->market.reserve.wood  = (uint32_t)parse_number_u64(value);
		return true;
	}
	
	if (strcmp(key, "cargo_ship.reserve_ore") == 0) {
		c->market.reserve.ore  = (uint32_t)parse_number_u64(value);
		return true;
	}
	
	if (strcmp(key, "cargo_ship.reserve_gold") == 0) {
		c->market.reserve.gold  = (uint32_t)parse_number_u64(value);
		return true;
	}
	
	return true;
}

bool LoadConfig(Connection *c, const char *filename)
{
	FILE *fp = fopen(filename, "r");
	
	if (!fp) {
		return false;
	}
	
	char line[512];
	char key[64], value[512];
	uint32_t line_num = 0;
	
	/* Defaults, overridden by the matching config keys */
	c->app.platform = 1;
	c->reconnect.enabled      = true;
	c->reconnect.delay        = 60;
	c->reconnect.kicked_delay = 60;
	c->reconnect.max_attempts = 0;
	c->migration_scrolls_needed = 1;
	c->gather.max_marches = 1;
	c->gather.radius      = 30;
	c->gather.kind_priority[0] = TROOP_INFANTRY;
	c->gather.kind_priority[1] = TROOP_RANGED;
	c->gather.kind_priority[2] = TROOP_CAVALRY;
	c->gather.kind_priority[3] = TROOP_SIEGE;
	c->gather.kind_priority_count = 4;
	c->gather.pending_tile = GATHER_NO_PENDING_TILE;
	c->recall.pause_seconds = 300; // $recall: no march for 5 minutes
	c->notify.on_war = true; // preserves the old always-on-when-webhook-set war alert behavior
	c->bot.command_input_mask = (1u << COMMAND_CHANNEL_GUILD) | (1u << COMMAND_CHANNEL_MAIL);
	c->bot.command_output     = COMMAND_CHANNEL_MAIL;
	snprintf(c->bot.data_path, sizeof(c->bot.data_path), "./data/");
	
	while (fgets(line, sizeof(line), fp)) {
		line_num++;
		
		/* Skip blank lines */
		if (line[0] == '\n' || line[0] == '\r') 
			continue;
		
		/* Skip comments */
		if (line[0] == '#' || line[0] == ';' || (line[0] == '/' && line[1] == '/')) 
			continue;
		
		if (sscanf(line, " %63[^=]= %511[^\n]", key, value) != 2) {
			line[strcspn(line, "\r\n")] = '\0';
			printf("%s:%u: error: unexpected syntax: %s\n", filename, line_num, line);
			fclose(fp);
			return false;
		}
		
		key[strcspn(key, " \t")] = '\0';
		value[strcspn(value, "\r\n")] = '\0';	
		
		if (ParserConfig(c, key, value) != true) {
			printf("%s:%u: error: `%s`\n", filename, line_num, value);
			fclose(fp);
			return false;
		}
    }

    fclose(fp);

    /* Administrators listed in the file cannot be removed by commands; the ones added in game are loaded now. */
    c->bot.admin_config_count = c->bot.admin_count;
    AdminLoadRuntime(c);

    return true;
}