#include <stdio.h>
#include <string.h>
#include <time.h>
#ifdef _WIN32
  #include <windows.h>
#endif
#include "status.h"
#include "command.h"
#include "map_point.h"
#include "protocol.h"

#define STATUS_INTERVAL 5

uint8_t GetVIPLevel(uint32_t vipPoints);

static bool StatusPath(const Connection *c, char *out, size_t size, const char *name)
{
	size_t length = strlen(c->bot.data_path);

	if (length == 0)
		return false;

	char last = c->bot.data_path[length - 1];
	int written = snprintf(out, size, "%s%s%s", c->bot.data_path, (last == '/' || last == '\\') ? "" : "/", name);

	return written > 0 && (size_t)written < size;
}

/* Player names are user text: keep the JSON valid whatever they contain. */
static void JsonString(FILE *f, const char *text)
{
	fputc('"', f);
	for (; *text; text++) {
		unsigned char ch = (unsigned char)*text;

		if (ch == '"' || ch == '\\')
			fprintf(f, "\\%c", ch);
		else if (ch < 0x20)
			fprintf(f, "\\u%04x", ch);
		else
			fputc(ch, f);
	}
	fputc('"', f);
}

static uint64_t TroopSum(const uint32_t *tiers)
{
	return (uint64_t)tiers[0] + tiers[1] + tiers[2] + tiers[3];
}

void StatusWrite(Connection *c, bool connected)
{
	char path[400], temp[420];

	if (!StatusPath(c, path, sizeof(path), "status.json"))
		return;

	MakeDirectories(path);
	snprintf(temp, sizeof(temp), "%s.tmp", path);

	FILE *f = fopen(temp, "w");
	if (!f)
		return;

	time_t now = time(NULL);
	const ShieldInfo *sh = &c->shield_info;
	uint64_t shield_end = sh->active ? sh->begin_time + sh->duration : 0;
	int64_t shield_left = (shield_end > c->server_time) ? (int64_t)(shield_end - c->server_time) : 0;
	const AntiScoutInfo *as = &c->antiscout_info;
	uint64_t antiscout_end = as->active ? as->begin_time + as->duration : 0;
	int64_t antiscout_left = (antiscout_end > c->server_time) ? (int64_t)(antiscout_end - c->server_time) : 0;
	map_pos_t pos = getTileMapPosbyPointCode(c->player.zone_id, c->player.point_id);

	fprintf(f, "{\"written_at\":%lld,\"connected\":%s,\"server_time\":%llu,",
		(long long)now, connected ? "true" : "false", (unsigned long long)c->server_time);

	fputs("\"name\":", f); JsonString(f, c->player.name);
	fprintf(f, ",\"power\":%llu,\"kills\":%llu,\"gems\":%u,\"vip_points\":%u,\"vip_level\":%u,",
		(unsigned long long)c->player.power, (unsigned long long)c->player.kills, c->player.gems,
		c->player.vip_point, GetVIPLevel(c->player.vip_point));
	fprintf(f, "\"kingdom\":%u,\"home_kingdom\":%u,\"x\":%u,\"y\":%u,\"marches\":%u,\"max_marches\":%u,",
		c->player.current_kingdom_id, c->player.home_kingdom_id, pos.x, pos.y,
		c->player.current_marches, c->player.max_marches);

	fprintf(f, "\"shield\":{\"loaded\":%s,\"active\":%s,\"item_id\":%u,\"end\":%llu,\"remaining\":%lld},",
		sh->loaded ? "true" : "false", sh->active ? "true" : "false", sh->item_id,
		(unsigned long long)shield_end, (long long)shield_left);

	fprintf(f, "\"antiscout\":{\"loaded\":%s,\"active\":%s,\"item_id\":%u,\"end\":%llu,\"remaining\":%lld},",
		as->loaded ? "true" : "false", as->active ? "true" : "false", as->item_id,
		(unsigned long long)antiscout_end, (long long)antiscout_left);

	fprintf(f, "\"resources\":{\"food\":%u,\"rock\":%u,\"wood\":%u,\"ore\":%u,\"gold\":%u},",
		c->resources.food, c->resources.rock, c->resources.wood, c->resources.ore, c->resources.gold);
	// The bag is the resource items (packs of 3K, 10K... 60M) the account holds: BagTotal adds up their values. This used to
	// print Connection.bag_resources, a field nothing ever filled, so the console's "Sac" was always 0.
	fprintf(f, "\"bag\":{\"food\":%llu,\"rock\":%llu,\"wood\":%llu,\"ore\":%llu,\"gold\":%llu},",
		(unsigned long long)BagTotal(c, RESOURCE_FOOD), (unsigned long long)BagTotal(c, RESOURCE_ROCK),
		(unsigned long long)BagTotal(c, RESOURCE_WOOD), (unsigned long long)BagTotal(c, RESOURCE_ORE),
		(unsigned long long)BagTotal(c, RESOURCE_GOLD));
	fprintf(f, "\"production\":{\"food\":%lld,\"rock\":%lld,\"wood\":%lld,\"ore\":%lld,\"gold\":%lld},",
		(long long)c->production.food, (long long)c->production.rock, (long long)c->production.wood,
		(long long)c->production.ore, (long long)c->production.gold);

	fprintf(f, "\"troops\":{\"loaded\":%s,\"total\":%u,\"infantry\":%llu,\"cavalry\":%llu,\"ranged\":%llu,\"siege\":%llu,\"t5\":%llu},",
		c->troop.loaded ? "true" : "false", c->troop.total,
		(unsigned long long)TroopSum(c->troop.infantry), (unsigned long long)TroopSum(c->troop.cavalry),
		(unsigned long long)TroopSum(c->troop.ranged), (unsigned long long)TroopSum(c->troop.siege),
		(unsigned long long)TroopSum(c->troop.t5_data));

	// Per (kind, tier) breakdown - "troops" above only has kind-level sums, not enough to tell
	// e.g. "plenty of troops overall but none of the tier gather/training actually needs".
	{
		static const char *kind_keys[4] = {"infantry", "ranged", "cavalry", "siege"};
		const uint32_t *kind_tiers[4] = {c->troop.infantry, c->troop.ranged, c->troop.cavalry, c->troop.siege};
		fprintf(f, "\"troops_by_tier\":{");
		for (int k = 0; k < 4; k++) {
			fprintf(f, "%s\"%s\":[%u,%u,%u,%u,%u]", k ? "," : "", kind_keys[k],
				kind_tiers[k][0], kind_tiers[k][1], kind_tiers[k][2], kind_tiers[k][3], c->troop.t5_data[k]);
		}
		fprintf(f, "},");

		// What each kind's building is training right now, regardless of whether autotrain or
		// a human in game started it (see TrainingSlot's comment) - no ETA, see its comment.
		fprintf(f, "\"training\":[");
		for (int k = 0; k < 4; k++) {
			TrainingSlot *t = &c->training[k];
			fprintf(f, "%s{\"kind\":\"%s\",\"active\":%s,\"tier\":%u,\"amount\":%u}",
				k ? "," : "", kind_keys[k], t->active ? "true" : "false", t->tier + 1, t->amount);
		}
		fprintf(f, "],");

		// Autotrain's own view: per kind, each of the 5 tiers with its configured target and
		// its current count - same numbers the per-type config grid is about. A tier with
		// target 0 is still listed (the UI decides whether to show it).
		fprintf(f, "\"autotrain\":{\"enabled\":%s,\"kinds\":[", c->autotrain.enabled ? "true" : "false");
		for (int k = 0; k < 4; k++) {
			fprintf(f, "%s{\"kind\":\"%s\",\"steps\":[", k ? "," : "", kind_keys[k]);
			for (int tier = 0; tier <= TIER_T5; tier++) {
				uint32_t current = tier <= TIER_T4 ? kind_tiers[k][tier] : c->troop.t5_data[k];
				fprintf(f, "%s{\"tier\":%u,\"cap\":%u,\"current\":%u}",
					tier ? "," : "", tier + 1, c->autotrain.target[k][tier], current);
			}
			fprintf(f, "]}");
		}
		fprintf(f, "]},");
	}

	// Research: the levels of every research (the console maps them with the game's table), the one
	// running, and what the automatic mode is doing.
	{
		int64_t now_s = c->server_time ? (int64_t)c->server_time : (int64_t)now;
		fprintf(f, "\"research\":{\"loaded\":%s,\"in_progress\":%u,\"start\":%lld,\"total\":%u,\"remaining\":%lld,\"academy\":%u,\"levels\":\"",
			c->research.loaded ? "true" : "false", ResearchInProgress(&c->research) ? c->research.in_progress : 0,
			(long long)c->research.start_time, c->research.total_time,
			(long long)ResearchSecondsLeft(&c->research, now_s), AcademyLevel(c));
		for (int i = 0; i < RESEARCH_LEVEL_BYTES; i++)
			fprintf(f, "%02x", c->research.levels[i]);
		fprintf(f, "\",\"auto\":{\"enabled\":%s,\"kinds\":[", c->research_auto.enabled ? "true" : "false");
		for (uint8_t i = 0; i < c->research_auto.kind_count; i++)
			fprintf(f, "%s%u", i ? "," : "", c->research_auto.kinds[i]);
		fputs("],\"state\":", f);
		JsonString(f, c->research_auto.state);
		fputs("}},", f);
	}

	// The lord: dead accounts get _MSG_RESP_LORD_BEINGEXECUTED at the login (RecvLordBeingExecuted).
	fprintf(f, "\"lord\":{\"where\":\"%s\",\"dead\":%s,\"since\":%llu,\"wait\":%u,\"fruits\":%u,\"guild_coins\":%u},",
		c->lord.where == LORD_DEAD ? "dead" : c->lord.where == LORD_CAPTIVE ? "captive" : "home", c->lord.dead ? "true" : "false",
		(unsigned long long)c->lord.since, c->lord.wait, (unsigned)c->items[LORD_REVIVE_FRUIT_ITEM].quantity, (unsigned)c->RoleAlliance.Money);

	// Map monster hunt: the energy the bot counts, what it knows, what it is doing.
	{
		const HuntSettings *h = &c->hunt;
		fprintf(f, "\"monster\":{\"enabled\":%s,\"level\":%u,\"energy_max\":%u,\"energy\":%u,\"energy_known\":%s,\"cost\":%u,\"heroes\":%u,\"monsters\":%u,\"series\":%s,\"attacks\":%u,\"kills\":%u},",
			h->enabled ? "true" : "false", h->level, h->energy_max, HuntEnergyNow(c), h->energy_known ? "true" : "false",
			h->level <= HUNT_MAX_LEVEL ? h->cost[h->level] : 0, h->hero_count, h->monster_count, h->series ? "true" : "false", h->attacks, h->kills);
	}

	// Construction: every building (where it stands, which, its level - the ones sent at login), what is being
	// built, and what the automatic construction would do next.
	{
		int64_t now_s = c->server_time ? (int64_t)c->server_time : (int64_t)now;
		fprintf(f, "\"build\":{\"reserved_farm\":%d,\"count\":%u,\"queues\":%d,\"permanent_queue\":%s,\"buildings\":[", BuildingReservedFarm(c) >= 0 ? c->building[BuildingReservedFarm(c)].position_id : 0, c->building_count,
			!c->construction_loaded ? 1 : (c->construction_extra_expires > now_s ? 2 : 1), c->construction_extra_expires == INT64_MAX ? "true" : "false");
		for (uint8_t i = 0; i < c->building_count; i++)
			fprintf(f, "%s[%u,%u,%u]", i ? "," : "", c->building[i].position_id, c->building[i].build_id, c->building[i].level);
		fputs("],\"queue\":[", f);
		bool first = true;
		for (int i = 0; i < BUILDING_QUEUE_SLOTS; i++) {
			const BuildingConstruction *q = &c->construction[i];
			if (!q->used) continue;
			fprintf(f, "%s{\"slot\":%u,\"id\":%u,\"level\":%u,\"remaining\":%lld,\"total\":%u}", first ? "" : ",", q->slot, q->build_id,
				q->level, (long long)BuildingConstructionSecondsLeft(q, now_s), q->duration);
			first = false;
		}
		fprintf(f, "],\"auto\":{\"enabled\":%s,\"types\":[", c->build_auto.enabled ? "true" : "false");
		for (uint8_t i = 0; i < c->build_auto.type_count; i++)
			fprintf(f, "%s%u", i ? "," : "", c->build_auto.types[i]);
		fprintf(f, "],\"plan\":{\"slot\":%u,\"id\":%u,\"level\":%u},\"state\":", c->build_auto.plan_slot, c->build_auto.plan_build_id,
			c->build_auto.plan_level);
		JsonString(f, c->build_auto.state);
		fputs("}},", f);
	}

	fprintf(f, "\"wounded\":{\"loaded\":%s,\"total\":%u},",
		c->wounded.loaded ? "true" : "false", c->wounded.troop.total);

	uint64_t dead_total = (uint64_t)c->valhalla.dead[0] + c->valhalla.dead[1] + c->valhalla.dead[2] + c->valhalla.dead[3];
	fprintf(f, "\"valhalla\":{\"loaded\":%s,\"dead_total\":%llu},",
		c->valhalla.loaded ? "true" : "false", (unsigned long long)dead_total);

	fprintf(f, "\"alliance\":{\"rank\":%d,\"members\":%u,\"member_names\":[", (int)c->RoleAlliance.Rank, c->alliance_member.count);
	for (uint32_t i = 0; i < c->alliance_member.count && i < MAX_ALLIANCE_MEMBER; i++) {
		if (i) fputc(',', f);
		JsonString(f, c->alliance_member.member[i].name);
	}
	fputs("]},", f);

	fputs("\"guild_chat\":[", f);
	{
		const GuildChatLog *log = &c->guild_chat_log;
		uint32_t start = (log->count < GUILD_CHAT_LOG_SIZE) ? 0 : log->next;

		for (uint32_t i = 0; i < log->count; i++) {
			const GuildChatEntry *e = &log->entries[(start + i) % GUILD_CHAT_LOG_SIZE];

			if (i) fputc(',', f);
			fprintf(f, "{\"time\":%lld,\"player\":", (long long)e->time);
			JsonString(f, e->player_name);
			fputs(",\"message\":", f);
			JsonString(f, e->message);
			fputc('}', f);
		}
	}
	fputs("]}\n", f);

	fclose(f);
#ifdef _WIN32
	MoveFileExA(temp, path, MOVEFILE_REPLACE_EXISTING);
#else
	rename(temp, path);
#endif
}

void StatusTick(Connection *c)
{
	static time_t last = 0;
	time_t now = time(NULL);

	if (now - last < STATUS_INTERVAL)
		return;

	last = now;
	StatusWrite(c, true);
}
