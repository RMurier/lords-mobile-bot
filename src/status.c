#include <stdio.h>
#include <string.h>
#include <time.h>
#ifdef _WIN32
  #include <windows.h>
#endif
#include "status.h"
#include "command.h"
#include "map_point.h"

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

	fprintf(f, "\"resources\":{\"food\":%u,\"rock\":%u,\"wood\":%u,\"ore\":%u,\"gold\":%u},",
		c->resources.food, c->resources.rock, c->resources.wood, c->resources.ore, c->resources.gold);
	fprintf(f, "\"bag\":{\"food\":%u,\"rock\":%u,\"wood\":%u,\"ore\":%u,\"gold\":%u},",
		c->bag_resources.food, c->bag_resources.rock, c->bag_resources.wood, c->bag_resources.ore,
		c->bag_resources.gold);
	fprintf(f, "\"production\":{\"food\":%lld,\"rock\":%lld,\"wood\":%lld,\"ore\":%lld,\"gold\":%lld},",
		(long long)c->production.food, (long long)c->production.rock, (long long)c->production.wood,
		(long long)c->production.ore, (long long)c->production.gold);

	fprintf(f, "\"troops\":{\"loaded\":%s,\"total\":%u,\"infantry\":%llu,\"cavalry\":%llu,\"ranged\":%llu,\"siege\":%llu},",
		c->troop.loaded ? "true" : "false", c->troop.total,
		(unsigned long long)TroopSum(c->troop.infantry), (unsigned long long)TroopSum(c->troop.cavalry),
		(unsigned long long)TroopSum(c->troop.ranged), (unsigned long long)TroopSum(c->troop.siege));
	fprintf(f, "\"wounded\":{\"loaded\":%s,\"total\":%u},",
		c->wounded.loaded ? "true" : "false", c->wounded.troop.total);

	fprintf(f, "\"alliance\":{\"rank\":%d,\"members\":%u},", (int)c->RoleAlliance.Rank, c->alliance_member.count);

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
