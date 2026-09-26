#ifndef CONNECTION_H
#define CONNECTION_H

#include <stdint.h>
#include <stdbool.h>

#ifdef _WIN32
  #ifndef _WIN32_WINNT
  #define _WIN32_WINNT 0x0601
  #endif
  #define WIN32_LEAN_AND_MEAN
  #define NOMINMAX
  #include <winsock2.h>
  #include <ws2tcpip.h>
  #include <windows.h>
  typedef unsigned int uint;
  #define close_socket closesocket
#else
  #include <unistd.h>
  #include <arpa/inet.h>
  #include <sys/socket.h>
  #include <netinet/in.h>
  #include <fcntl.h>
  #define close_socket close
#endif

#include <string.h>
#include <stdio.h>

#include "des.h"
#include "research.h"
#include "buildings.h"
#include "building_table.h"

typedef enum {
	EMS_Null,
	EMS_Begin,
	EMS_End,
	EMS_BeginAndEnd
} eMsgState;

typedef struct {
	uint16_t packet_size;
	uint16_t packet_type;
    uint8_t buffer[4096];
    size_t read_pos;
    size_t parse_pos;
} PacketStream;

typedef struct {
	uint8_t data[4096];
	uint16_t size;
	uint16_t offset;
} PacketBuffer;

/*
typedef struct {
	uint8_t data[4096];
	uint16_t size;
	uint16_t offset;
} Stream;*/


#define MAX_ITEM_COUNT 65536

typedef struct {
    uint32_t food;
    uint32_t rock;
    uint32_t wood;
    uint32_t ore;
    uint32_t gold;
} ResourceStock;

typedef struct {
    int64_t food;
    int64_t rock;
    int64_t wood;
    int64_t ore;
    int64_t gold;
} ResourceProduction;

typedef enum {
    RESOURCE_FOOD,
    RESOURCE_ROCK,
    RESOURCE_WOOD,
    RESOURCE_ORE,
    RESOURCE_GOLD
} ResourceType;

typedef struct {
	uint16_t quantity;
} Item;

typedef struct {
    uint16_t item_id;
    uint16_t item_count;
    uint8_t resource_kind;
    uint32_t resource_count;
    uint8_t rare;
} MarketItem;

typedef struct {
	bool speed_up;
    bool speed_up_research;
    bool speed_up_merging;
    bool speed_up_training;
    
    bool bright_talent_orb;
} BlackMarketBuy;

typedef struct {
	bool auto_trade;
	bool use_bag_rss;
    bool spend_food;
    bool spend_rock;
    bool spend_wood;
    bool spend_ore;
    bool spend_gold;
} MarketSettings;

typedef struct {
	bool loaded;
	bool buy_pending;
	uint8_t trade_locks;
	uint8_t trade_status;
	uint64_t refresh_time;
	MarketItem items[4];
	ResourceStock reserve;
	MarketSettings settings;
	uint8_t bag_topup_attempts; // safety net: stop retrying use_bag_rss top-ups after too many in a row
} BlackMarket;

typedef struct {
	char name[13];
	uint64_t power;
	uint64_t kills;
	uint32_t gems;
	uint32_t vip_point;
	uint16_t current_kingdom_id;
	uint16_t home_kingdom_id;
	uint16_t zone_id;
	uint8_t point_id;
	uint8_t max_marches;
	uint8_t current_marches;
} PlayerInfo;

// optional 
typedef struct {
    ResourceStock current;
    ResourceStock bag;

    bool loaded;
} ResourceInfo;

typedef struct {
    char addr[16];
    uint32_t port;
} ServerInfo;

typedef struct {
    uint8_t  version_major;
    uint8_t  version_minor;
    uint16_t version_patch;
    uint8_t  language_code;
    uint8_t  platform; /* byte sent after the version in the gateway login (1 = mobile, 9 = official PC client) */
} AppInfo;

typedef struct {
    int64_t igg_id;
    char device_uuid[50];
    uint16_t session_len;
    char session[512];
} AuthInfo;

typedef struct {
    uint32_t seq_id;
    uint32_t guest_seq_id;
} ProtocolState;

typedef struct {
    char player_name[13];
    char message[1024];
    uint8_t channel; /* channel the last message came from: 0 = world, 1 = alliance */
    bool pending;
} ChatState;

/* Recent alliance chat, kept for the web console (status.json -> "guild_chat"); a ring
 * buffer so a very chatty guild only ever costs a fixed, small amount of memory. */
#define GUILD_CHAT_LOG_SIZE 20
typedef struct {
    time_t time;
    char   player_name[13];
    char   message[241]; // web console chat lines are capped at 240 characters
} GuildChatEntry;

typedef struct {
    GuildChatEntry entries[GUILD_CHAT_LOG_SIZE];
    uint32_t next;  // ring buffer write cursor
    uint32_t count; // valid entries, caps at GUILD_CHAT_LOG_SIZE
} GuildChatLog;

/*
typedef enum {
    TRANSFER_IDLE,
    TRANSFER_FIND_TARGET,
    TRANSFER_WAIT_TARGET,
    TRANSFER_SEND_MARCH,
    TRANSFER_WAIT_MARCH,
    TRANSFER_COMPLETE,
    TRANSFER_FAILED
} TransferState;


typedef struct {
	bool active;
	TransferState state;
	char request_name[13];
	char target_name[13];
	
	ResourceType resource;
	
	uint32_t total_amount;
	uint32_t remaining_amount;
	
	uint16_t zone_id;
	uint8_t point_id;
	
	uint32_t current_chunk;
} ResourceTransfer;
*/

typedef struct {
    uint32_t serial_id;

    uint64_t send_time;

    uint8_t mail_type;
    uint32_t reply_id;

    uint16_t sender_head;
    uint16_t sender_kingdom;

    char sender_tag[4];
    char sender_name[14];

    uint8_t extra_flag;

    char title[256];
    char content[4096];

    uint8_t attachment_count;

} MailInfo;

typedef enum {
    Research = 0,
    Building,
    Max,
} HelpKind;

typedef struct {
    uint32_t record_sn;
    uint16_t head;
    uint8_t rank;
    char player_name[14];

    HelpKind help_kind;

    uint16_t event_id;
    uint8_t event_data_lv;

    uint8_t already_helped;
    uint8_t help_max;
    
    uint32_t record_sn_arr[255];
} AllianceHelp;

typedef struct {
	uint16_t position_id;
    uint16_t build_id;
    uint8_t level;
} BuildingInfo;

/*
typedef struct {
    uint32_t serial_id;

    uint8_t status;
    uint64_t receive_time;

    uint16_t box_item_id;
    uint16_t item_id;
    uint16_t quantity;

    uint8_t item_rank;

    char sender_name[13];
} AllianceGift;
*/


typedef struct {
    uint32_t sn;
    uint8_t status;
    int64_t rcv_time;
    uint16_t box_item_id;
    uint16_t item_id;
    uint16_t num;
    uint8_t item_rank;
    // Option
    uint32_t diamond;
    uint32_t money;
    char player[13];
} AllianceGift;


#define GIFT_TABLE_SIZE 8192

typedef enum {
    GIFT_EMPTY   = 0,
    GIFT_USED    = 1,
    GIFT_DELETED = 2
} GiftSlotState;

typedef struct {
	bool loaded;
    uint16_t count;
    uint16_t index;
    bool opening;
    AllianceGift gifts[GIFT_TABLE_SIZE];
} AllianceGiftList;


typedef enum {
    GIFT_STATE_IDLE,          // No gift data loaded yet.
    GIFT_STATE_LOADING,       // Waiting for RequestAllianceGiftInfo() response.
    GIFT_STATE_READY,         // Ready to process gifts.
    GIFT_STATE_OPENING,       // Waiting for open gift response.
    GIFT_STATE_DELETING,      // Waiting for delete gift response.
    GIFT_STATE_BULK_OPENING,  // VIP 12+: waiting for RequestOpenAllAllianceGiftBox()'s response.
    GIFT_STATE_BULK_CHECKING  // VIP 12+: waiting for RequestAllianceGiftCheckExpired()'s response.
} GiftState;

typedef struct {
    bool auto_help;
    bool auto_open_gifts;
    uint16_t gift_count;
    uint16_t gift_offset;

    uint16_t unopened_gift_count;

    GiftState gift_state;
    uint8_t bulk_check_kind; // GIFT_STATE_BULK_CHECKING: which "kind" argument is in flight (2, then 1)
    uint16_t recv_index;
    AllianceGift gifts[300];
} AllianceSettings;


#define MAX_SMART_USE_ITEMS 50

typedef struct {
    uint16_t id;
    uint16_t qty;
} SmartUseItem;

typedef struct {
    uint16_t count;
    SmartUseItem items[MAX_SMART_USE_ITEMS];
} SmartUseList;

typedef struct {
    bool auto_double_ticket;   // claim the "buy 1 get 1" treasure coupon automatically
    bool auto_online_gift;     // claim the periodic online/mystery gift automatically
    time_t last_double_ticket_try;
    time_t last_online_gift_try;
} ActivitySettings;

/* Automatic resource-tile gathering: scan the zones around the castle with
 * _MSG_REQUEST_MAPDATA, one zone at a time. The first attempt at this (see git
 * history) sent no active scan at all live: it always requested 4 zone slots,
 * padding whichever it didn't have with a repeat of the first one - a shape
 * that never appears in a real client's traffic (real captures always declare
 * the true count - 1, 2 or 4 - and pad unused slots with zero, never a repeat).
 * It also sent _MSG_REQUEST_OPEN_UI before scanning, on the assumption (from a
 * single capture where the two happened to be close together) that it was a
 * required precondition; a later capture shows the very first MAPDATA of a
 * session going out - and answered - before any OPEN_UI at all, so it is not
 * sent here. One zone per request is simpler to keep byte-identical to a real
 * client than trying to reproduce its multi-zone batches.
 *
 * Sending a march to the best untargeted tile found (while a slot is free) uses
 * the game's own "low level first" auto troop selection (troop_type_id = 0 lets
 * the server pick, matching 6 of 7 captured marches; only a level 4 ore tile got
 * a different, unconfirmed non-zero type - most likely the account running out of
 * the default troop, since the wiki does not document any hard tier requirement
 * per tile level). Troop count is computed from a single capacity constant
 * (~23.3 resource/troop) derived from those same 6 marches: experimental, not
 * from an official source, and does not account for running low on troops or
 * escalating tiers - worth calibrating against real results. */
#define GATHER_MAX_TILES 256
#define GATHER_DEFAULT_TROOP_CAPACITY 23.3
// Was assumed to point at the march itself (troops) rather than "tile taken", on the theory
// that many different tiles being simultaneously taken was unlikely - confirmed wrong live for
// high-level tiles specifically (manually checked: contested most of the time). Still worth a
// warning past this many consecutive refusals, just without assuming which cause it is - see
// the warning text in RecvGatherMarchResp.
#define GATHER_REFUSAL_WARN_THRESHOLD 5
// Upper bound on gather marches ever in flight at once (player.max_marches is a uint8_t in
// practice well under this with any realistic VIP/buff level) - sizes the FIFO queue below.
#define GATHER_MAX_ACTIVE_MARCHES 16
// After a refused march, leave that tile alone this long: without it the same (nearest) tile was
// re-picked every ~2s with the exact same troop count, refused every time, forever.
#define GATHER_TILE_REFUSAL_COOLDOWN_MS (60 * 1000)
// This many refusals in a row (necessarily on different tiles by then, given the cooldown) point at
// the troop count rather than tile occupancy: the learned cap below is then cut in half.
#define GATHER_REFUSALS_BEFORE_SHRINK 3
// How often to redo the full tile scan - see gather.next_rescan_at's comment. 5 minutes: frequent
// enough that a freed-up tile is not missed for long, rare enough not to spam RequestMapData.
#define GATHER_RESCAN_INTERVAL_MS (5 * 60 * 1000)

typedef enum {
    RESOURCE_KIND_FOOD  = 1,
    RESOURCE_KIND_STONE = 2,
    RESOURCE_KIND_ORE   = 3,
    RESOURCE_KIND_WOOD  = 4,
    RESOURCE_KIND_GOLD  = 5
} GatherResourceKind;

typedef struct {
    bool     used;
    bool     targeted;     // a march is already out (or was) for this tile
    uint64_t refused_until; // now_ms() deadline: skip this tile after the server refused a march to it
    uint16_t zone_id;
    uint8_t  point_id;
    uint8_t  resource_kind; // GatherResourceKind
    uint8_t  level;         // 1-5
    uint32_t amount;

    /* Confirmed live: a tile currently occupied (someone gathering it) embeds that player's
     * name (13 bytes) and alliance tag (3 bytes) at the exact offsets a WAR_RECORD_TAG
     * player-point record uses (offset+4 and +17 - see RecvMapInfoPlus) instead of zeros. Not
     * a separate flag - inferred from the name field being non-empty. Refreshed on every
     * update for this tile, so it clears again once the occupier leaves. */
    bool     occupied;
    char     occupied_by[13];

    /* Same record carries a value(2) right after alliance_tag, at offset+20 - confirmed on the
     * sibling WAR_RECORD_TAG record (tag=8) at that exact offset (there, the occupier's real
     * kingdom), and it exactly fills the gap between alliance_tag(+17,3) and level(+22) for this
     * record too. NOT "which kingdom this tile is in", despite the field's name and an earlier
     * version of this code filtering on it as if it were: confirmed live that a genuinely free
     * tile decodes this as 0 (no occupier to report a kingdom for), while an occupied one
     * decodes the occupier's real kingdom - filtering resource tiles by kingdom_id ==
     * current_kingdom_id silently dropped every free tile and kept only occupied ones, which is
     * the opposite of useful. Cross-kingdom garbage tiles (zone_id/point_id collide with the
     * local scan's numbering, decoding to a bogus local X/Y) are filtered by GatherZoneInRange
     * instead (protocol.c) - kingdom_id is kept here only for reference/logging, not as a
     * filter. */
    uint16_t kingdom_id;
} GatherTile;

/* $recall (command.c): every march is taken back, then the bot sends none for pause_seconds. The pause deadline
 * itself is NOT here: Connection is wiped at every reconnection and the pause must hold across one, so it is a
 * global in protocol.c (MarchesPaused()). */
typedef struct {
    uint32_t pause_seconds;   // recall.pause_seconds, default 300, 0 = no pause
    bool     active;          // still sending the recall requests, one every 1-2 s
    uint8_t  next_index;      // march index of the next request
    uint8_t  count;           // how many march indices are tried
    uint8_t  sent;            // requests sent so far
    uint64_t next_at;         // now_ms() deadline for the next request
    char     requester[64];   // who is told when it is done
} RecallState;

#define GATHER_NO_PENDING_TILE 0xFFFF

typedef struct {
    bool     enabled;
    uint8_t  max_marches;   // out of player.max_marches, how many to use for gathering
    uint16_t radius;        // tiles around the castle to scan
    uint32_t max_troop_count; // never send more troops than this in one gather march (0 = no cap)

    /* Which of the 4 troop-count slots RequestGatherMarch fills, tried in this order: the
     * first kind is used up first and the next ones top the same march up when it is not
     * enough (e.g. infantry first, ranged for the remainder) - see GatherTick's comment. Tier choice
     * within whichever kind is picked is left to the server's own auto-pick (troop_type_id/
     * the per-slot tag - always 0 in every capture so far, and it picked this account's
     * lowest available tier on its own, e.g. T2 when T1 was empty - so this is not
     * reimplemented here; only which KIND to use is ours to decide). */
    uint8_t  kind_priority[4];
    uint8_t  kind_priority_count;

    bool     scan_done;
    uint16_t scan_cursor;   // index into the zone rectangle being swept
    uint64_t next_scan_at;  // now_ms() deadline: do not send the next RequestMapData before this

    /* The first scan only happens once - after that, tile state (level/amount/occupied) is only
     * ever refreshed by whatever the server pushes passively. Confirmed live that this can go
     * stale: every known tile stayed marked occupied for 7+ minutes of real time (a real, briefly
     * occupied tile does eventually free up - other players finish gathering and leave), with no
     * passive push ever correcting it. So a full rescan (same mechanism as the first one - just
     * scan_done/scan_cursor reset) reruns periodically regardless of passive pushes, both to
     * refresh occupied tiles that quietly aren't anymore and to pick up new tiles. GatherTrackTile
     * updates existing entries in place by (zone_id, point_id), so rescanning never duplicates. */
    uint64_t next_rescan_at;

    uint8_t  active_marches; // gather marches this code has out right now (subset of player.current_marches)
    /* Marches already out when the bot connected (restart/reconnect): it never sent them, so
     * active_marches starts at 0 and, counting only its own, let it fill the whole march budget on
     * top of them - with 2 out and 3 reserved it went to 5/5. The login snapshot only gives a count,
     * not which are gather marches, so they are all charged against gather.max_marches (the safe
     * side: never more than reserved in total). See GatherInheritedMarches. */
    uint8_t  inherited_marches;
    bool     inherited_set;  // taken once, from the first march snapshot
    uint64_t next_march_at;  // now_ms() deadline: do not send another gather march before this

    /* Committed to a tile (targeted, slot reserved) but the march itself still waits: a
     * RequestMapAdvance was just sent for it and, like a resource delivery, needs a human-like
     * pause before the march - sending it in the same tick got resource deliveries refused
     * (code 14) for a target never "looked at" this way; gather marches were refused outright
     * (codes 2/6/12, live) without this step at all. */
    uint16_t pending_tile;   // index into tiles[], or GATHER_NO_PENDING_TILE
    uint64_t march_send_at;  // now_ms() deadline: send pending_tile's march no sooner than this

    /* c->troop.total is never adjusted when a march leaves or comes home, so capping to it
     * alone still lets the bot ask for troops that are already out on an earlier gather march.
     * The server never tells us which march came home or how many troops it carried (its
     * payload is undecoded - see RecvGatherTroopHome's comment), so this tracks it ourselves:
     * a FIFO of the amount sent with each march still out. Best-effort, not exact, if returns
     * do not arrive in the same order they were sent in - but a wrong guess here only means a
     * request that gets refused (same failure mode as today), never anything destructive, and
     * it is still strictly closer to reality than not tracking this at all. */
    uint32_t troops_out;
    /* Same ledger split per troop kind (TroopKind index, 0-3): what is still out, per kind. The
     * free stock of a kind is its own total minus ITS OWN out amount - subtracting the all-kinds
     * troops_out from one kind's total (what this used to do) hid troops that were really free
     * whenever a different kind was the one out. Tracks what was actually sent, never a guess. */
    uint32_t troops_out_by_kind[4];
    uint32_t pending_amounts[GATHER_MAX_ACTIVE_MARCHES][4]; // per march, per troop kind: one march can mix kinds
    // Same FIFO slots as pending_amounts (index into tiles[]) - lets a march coming home (or
    // getting refused) clear that specific tile's `targeted` flag. Without this, `targeted` was
    // only ever set to true and never back to false on the success path, so every tile gathered
    // even once stayed permanently excluded from GatherBestUntargeted - in a crowded/small radius
    // this starves out all the close tiles first, leaving only ever-farther untried ones to pick
    // from, which live looked like "only gathers far-away tiles" despite free tiles sitting right
    // next to the castle the whole time.
    uint16_t pending_tiles[GATHER_MAX_ACTIVE_MARCHES];
    uint8_t  pending_head;
    uint8_t  pending_count;

    GatherTile tiles[GATHER_MAX_TILES];
    uint16_t   tile_count;
    time_t     last_status_log; // throttles the periodic "N tiles known" log line

    /* Per-march troop cap learned from refusals (0 = none learned yet). The server refuses (code 2)
     * a march bigger than the commander's march capacity, and the bot has no way to read that
     * capacity, so it converges on it instead: halved after GATHER_REFUSALS_BEFORE_SHRINK refusals
     * in a row, raised 25% after each accepted march to keep probing upward. */
    uint32_t learned_max_troops;
    uint32_t last_sent_count;      // troop count of the most recent march request, for the above

    uint16_t consecutive_refusals; // resets on any accepted march; see GATHER_REFUSAL_WARN_THRESHOLD
    bool     refusal_warned;       // one warning per streak, not one per refusal
} GatherSettings;

/* $join <tag> / $leave: at most one alliance operation in flight at a time, its
 * outcome reported back to whoever asked for it. Reverse-engineered from a capture
 * of leaving a guild, searching by tag text, and applying (which is either an
 * instant join or a pending application depending on the target's settings - the
 * bot does not distinguish the two, both are reported as "candidature envoyée"). */
typedef enum {
    ALLIANCE_OP_NONE,
    ALLIANCE_OP_JOIN_SEARCHING,   // sent ALLIANCE_SEARCH, waiting for SRARCHRESULT
    ALLIANCE_OP_JOIN_APPLYING,    // sent ALLIANCE_APPLY, waiting for its response
    ALLIANCE_OP_LEAVING,          // sent ALLIANCE_QUIT, waiting for its response ($leave)
    ALLIANCE_OP_LEAVING_TO_JOIN,  // sent ALLIANCE_QUIT, waiting for its response, then a cooldown before $join's search
    ALLIANCE_OP_LEAVE_COOLDOWN    // left the previous alliance, waiting out not_before before searching for the new one
} AllianceOpState;

typedef struct {
    AllianceOpState state;
    char tag[4];         // 3-character alliance tag, case-sensitive
    char requester[13];
    time_t not_before;   // ALLIANCE_OP_LEAVE_COOLDOWN: do not search before this time
} AllianceOp;

/* Passive war watcher: tracks every player point seen in whatever _MSG_RESP_UPDATE_MAPINFO(_PLUS)
 * the server sends unprompted (no confirmed client request elicits it - see the comment above
 * WarTick in protocol.c), and reports (via Discord webhook) when a point we know sends the
 * compact single-point update that was observed, in a packet capture, at the exact moment a
 * shield bubble disappeared on screen. Reverse-engineered from one confirmed sample: not proven
 * to be exclusively a shield event, so it is logged as such but may need recalibration. */
#define WAR_MAX_POINTS 8192
#define WAR_ZONE_COUNT 1024   /* zoneId is 10 bits: (x>>5) + ((y>>4)<<4), x<512, y<1024 */

typedef struct {
    bool     used;
    uint16_t zone_id;
    uint8_t  point_id;
    uint16_t kingdom_id;
    char     name[13];
    char     tag[4];
} WarPoint;

typedef struct {
    bool     enabled;
    time_t   last_status_log; // throttles the periodic "N points known" log line
    uint16_t point_count;
    WarPoint points[WAR_MAX_POINTS];
} WarSettings;

/* Central Discord webhook config, shared by every feature that raises an alert (war watcher,
 * anti-scout reports, shield/anti-scout about to run out with nothing to renew with, resource
 * transfer completed). One URL, one on/off switch per event kind - see NotifyDiscord() in
 * protocol.c. Used to be war-only (war.discord_webhook); config.c still accepts that old key
 * as an alias into discord_webhook below so existing config files keep working. */
typedef struct {
    char discord_webhook[256];
    bool on_war;                 // point we track loses its shield bubble
    bool on_antiscout_report;    // someone tried to scout us (_MSG_RESP_ANTISCOUTREPORTINFO)
    bool on_shield_expiring;     // shield about to run out and no item left to renew it
    bool on_antiscout_expiring;  // same, for anti-scout
    bool on_transfer_done;       // a $bank resource delivery finished
} NotifySettings;

typedef enum
{
	// Token: 0x04000893 RID: 2195
	NONE,
	// Token: 0x04000894 RID: 2196
	RANK1,
	// Token: 0x04000895 RID: 2197
	RANK2,
	// Token: 0x04000896 RID: 2198
	RANK3,
	// Token: 0x04000897 RID: 2199
	RANK4,
	// Token: 0x04000898 RID: 2200
	RANK5,
	// Token: 0x04000899 RID: 2201
	RANKMAX = 5
} AllianceRank;


typedef struct {
	uint32_t Channel;
    AllianceRank Rank;
    uint8_t Apply;
    uint32_t Money;
} AllianceInfo;

typedef enum {
    HELP_SPAM_IDLE,
    HELP_SPAM_START_BUILD,
    HELP_SPAM_WAIT_BUILD,
    HELP_SPAM_WAIT_HELP,
    HELP_SPAM_CANCEL_BUILD
} HelpSpamState;

typedef enum {
    BUILD_TIMBER       = 1,
    BUILD_STONE        = 2,
    BUILD_ORE          = 3,
    BUILD_FOOD         = 4,
    BUILD_MANOR        = 5,
    BUILD_BARRACKS     = 6,
    BUILD_INFIRMARY    = 7,
    BUILD_CASTLE       = 8,
    BUILD_VAULT        = 9,
    BUILD_ACADEMY      = 10,
    BUILD_WALL         = 12,
    BUILD_WATCHTOWER   = 13,
    BUILD_EMBASSY      = 14,
    BUILD_WORKSHOP     = 15,
    BUILD_TRADING_POST = 17
} BUILDING_ID;


typedef struct {
    bool active;

    HelpSpamState state;

    uint16_t remaining_count;
    uint8_t speed;

    time_t last_action;
    
    uint16_t building_id;
    uint16_t building_pos;
    uint8_t building_level;
} HelpSpam;

typedef struct {
	bool enabled;
	
	bool send_food;
	bool send_rock;
	bool send_wood;
	bool send_ore;
	bool send_gold;
	
	ResourceStock reserve;
	uint32_t max_delivery_distance;
	double delivery_tax_percent; // the game deducts this % on arrival; requests are grossed up to compensate
	
	bool use_bag_rss;
	bool use_bag_food;
	bool use_bag_rock;
	bool use_bag_wood;
	bool use_bag_ore;
	bool use_bag_gold;
} BankSettings;

typedef enum {
    COMMAND_CHANNEL_WORLD,
    COMMAND_CHANNEL_GUILD,
    COMMAND_CHANNEL_MAIL
} CommandChannel;

#define MAX_ADMINS 16

typedef struct {
    /* Administrators: the first admin_config_count come from the config file
     * and cannot be removed by commands, the others were added in game and are
     * stored in <data.path>/admins.txt. */
    char admin_names[MAX_ADMINS][13];
    uint8_t admin_count;
    uint8_t admin_config_count;
    
    char command_prefix;
    bool admin_only;
    char data_path[256];
    
    uint8_t command_input_mask;   /* bit mask of 1 << CommandChannel: where commands are read */
    CommandChannel command_output; /* where replies are sent */
} BotSettings;

/* Automatic reconnection after a dropped connection. */
typedef struct {
    bool enabled;
    uint32_t delay;        /* seconds to wait before reconnecting after a dropped connection */
    uint32_t kicked_delay; /* seconds to wait after the account was logged in from another device, 0 = do not reconnect */
    uint32_t max_attempts; /* consecutive failed attempts before giving up, 0 = unlimited */
} ReconnectSettings;

typedef struct {
	char player_name[13];
	ResourceStock resource;
} PlayerBank;

/*
typedef struct {
	bool enabled;
	bool always_on;
	bool on_attack;
	bool on_scout;
	
	uint16_t priority[8];
	uint8_t priority_count;
} ShieldSettings;
*/

/*
typedef enum
{
	EWATCHTOWER_LINE_TARGET_CAPITAL,
	EWATCHTOWER_LINE_TARGET_CAMP,
	EWATCHTOWER_LINE_TARGET_AMBUSH,
	EWATCHTOWER_ADDLINE_WONDER1,
	EWATCHTOWER_ADDLINE_WONDER2,
	EWATCHTOWER_ADDLINE_WONDER3,
	EWATCHTOWER_ADDLINE_WONDER4,
	EWATCHTOWER_ADDLINE_WONDER5,
	EWATCHTOWER_ADDLINE_WONDER6,
	EWATCHTOWER_ADDLINE_WONDER7
} EWATCHTOWER_LINE_TARGET;

typedef struct {
    uint32_t line_id;
    uint8_t line_type;
    uint64_t begin_time;
    uint32_t require_time;
    EWATCHTOWER_LINE_TARGET target;
} WatchTowerEvent;
*/

typedef struct {
    bool active;
    bool loaded; // Have we received the buff list yet?
    bool pending;
    bool expiring_notified; // "about to run out, nothing to renew with" already sent - reset once active again with time to spare
    time_t no_item_logged_at; // throttles "no item available" (UsePriorityShield/UsePriorityAntiScout): checked every BotTick, would spam otherwise
    uint16_t item_id;
    uint16_t quantity;
    uint64_t begin_time;
    uint32_t duration;
} ShieldInfo;

/* "Anti-espionnage" (see items.h): same shape as ShieldInfo, its RESP_USEITEM
 * has the exact same extra fields. */
typedef ShieldInfo AntiScoutInfo;

typedef enum {
    HYPER_STATE_IDLE = 0,          // Nothing to do.
    HYPER_STATE_FIND_TARGET,       // Waiting for ally location.
    HYPER_STATE_READY,             // Target found, ready to transfer.
    HYPER_STATE_SENDING,           // Sending one or more marches.
    HYPER_STATE_WAIT_RETURN,       // Waiting for marches to return.
} HyperState;

typedef struct {
	bool enabled;
	char target_name[13];
	uint16_t max_transfer_distance;
	
	HyperState state;
    bool pending;              // Waiting for server response.

	struct {
		uint32_t send_at;
		uint32_t stop_at;
	} food, rock, wood, ore, gold;
	
	uint16_t zone_id;
	uint8_t point_id;
	
} HyperSettings;

typedef struct {
	bool loaded;
	uint32_t total;
	uint32_t infantry[4];
	uint32_t cavalry[4];
	uint32_t ranged[4];
	uint32_t siege[4];
	uint32_t t5_data[4];
} TroopData;

/* One per TroopKind: what _MSG_RESP_TRAINING_ (2408) last confirmed as accepted for that
 * kind's building, regardless of who asked for it (this bot's autotrain or a human in game -
 * RecvTrainingStart fires either way). No end time: the response's trailing bytes are not
 * decoded (see RecvTrainingStart's comment), so this can say a training is running and what
 * it is, not when it finishes. Cleared by TroopAdd, the same "a batch just completed" event
 * used to credit c->troop and free AutoTrainSettings' busy flag. */
typedef struct {
	bool     active;
	uint8_t  tier;
	uint32_t amount;
} TrainingSlot;

#define AUTOTRAIN_REFUSAL_BACKOFF_MS       (60 * 1000)       // normal case: likely transient (resources, timing)
#define AUTOTRAIN_HARD_BLOCK_THRESHOLD     5                 // this many refusals in a row -> stop assuming "transient"
// e.g. the tier's research/building requirement is not met yet: retrying every minute would
// never succeed and just spams logs, but the tier can't be dropped outright either since the
// requirement could be met later in the same run (research finishing, a building upgrade
// landing) - so keep retrying, just rarely.
#define AUTOTRAIN_HARD_BLOCK_BACKOFF_MS    (30 * 60 * 1000)

// First-ever request for a (kind, tier) with no history yet: a real player has no way to know
// the true affordable max either (see AutoTrainSettings' comment on last_granted), so this is a
// deliberately unremarkable guess, not the (possibly 7-digit) full gap to the target.
#define AUTOTRAIN_INITIAL_BATCH_GUESS      10000
// Once we know what was actually granted last time, grow toward the target by this factor per
// attempt (50%) instead of jumping straight back to the full gap.
#define AUTOTRAIN_BATCH_GROWTH_NUM         3
#define AUTOTRAIN_BATCH_GROWTH_DEN         2

/* Modeled directly on the game's own barracks/range/stable/workshop screen: one box per (kind,
 * tier) pair - a fixed 4x5 grid (infantry/ranged/cavalry/siege x T1-T5), each an independent
 * target; 0 = do not train that exact pair. For a given kind, AutoTrainTick always tries the
 * lowest tier that is not yet at its own target and not backed off, in T1..T5 order - e.g.
 * infantry T2=10M, T4=5M fills T2 to 10M first, then - once T2 is there - moves on to T4.
 *
 * Confirmed live (user report + capture) that the account can only ever have ONE (kind, tier)
 * actually training at a time, not one per kind/building as an earlier version of this code
 * assumed: two more kinds requested seconds after a first one (still training) were both
 * refused outright. So there is a single account-wide busy flag, not one per kind, and it stays
 * true from the moment a request is sent until the batch actually finishes (RecvAddSoldier /
 * RecvTroopTrainingImmediate) - an ACCEPTED order still blocks every other kind/tier until then,
 * exactly like "already training" in the game's own UI. AutoTrainTick round-robins across kinds
 * (next_kind) so one kind that keeps needing a lot cannot starve the other three of a turn.
 *
 * The game can silently grant far less than requested even on an accepted order (confirmed
 * live: 3.7M requested, 29 granted) - some real, un-decoded limit caps it regardless of what is
 * asked (checked: not present as a separate field anywhere in _MSG_RESP_TRAINING_ itself, a
 * capture asking for less than a known max showed nothing but the granted amount echoed back).
 * Confirmed live by the account's own owner that this limit is the SAME for every kind and every
 * tier at any given moment - only the price and duration differ per (kind, tier), not the cap -
 * so last_granted is a single account-wide value, not one per (kind, tier): learning it from an
 * attempt on ANY kind/tier immediately benefits every other one too, instead of rediscovering it
 * separately four times over.
 *
 * Since asking for more than that limit never gets more accepted, but DOES mean sending an
 * obviously non-human amount (no player types a 7-digit number into the training field),
 * AutoTrainTick never requests the raw gap to the target outright: each new attempt asks for at
 * most last_granted plus 50% - an ordinary "try a bit more than last time" progression -
 * starting from a modest, unremarkable guess (AUTOTRAIN_INITIAL_BATCH_GUESS) before anything has
 * been granted yet this run.
 *
 * Backoff after a refusal is indexed [kind][tier]: a refusal on one tier of a kind (e.g. T4
 * locked by research) must not block a different, already-unlocked tier of the same kind (e.g.
 * T2) - AutoTrainTick's loop naturally moves past a backed-off tier to the kind's next one, and
 * naturally retries the earlier one again once its own backoff expires. */
typedef struct {
	bool enabled;

	uint32_t target[4][5];                       // [kind][tier]; 0 = do not train that pair

	bool     busy;                               // true while the one account-wide training order is in flight or still running
	uint8_t  pending_kind;                       // kind of that order, valid while busy
	uint8_t  pending_tier;                       // tier of that order, valid while busy
	uint8_t  next_kind;                          // round-robin cursor - see this struct's comment

	uint32_t last_granted;                       // account-wide, shared by every (kind, tier) - see this struct's comment; 0 = no history yet
	uint64_t retry_at[4][5];                     // [kind][tier] now_ms() backoff after a refusal
	uint16_t consecutive_refusals[4][5];         // [kind][tier], resets on any accepted order for that pair
	uint64_t next_action_at;        // now_ms(): at most one new training order per tick, paced like gather
} AutoTrainSettings;

/* Automatic research: `research.categories` lists, in priority order, the categories (the game's own category
 * numbers, research_table.h) the bot researches by itself. Whenever no research is running it starts the next
 * one it can (ResearchPickNext, auto mode: prerequisites first, in any category; Academy; cost against what is left
 * after research.reserve_*). The guild bank's deposits are NOT held back. The game runs one research at a time. */
typedef struct {
	bool         enabled;
	uint8_t      kinds[RESEARCH_KIND_COUNT_MAX];  // category numbers, priority order
	uint8_t      kind_count;
	ResourceStock reserve;                         // never spent by research

	bool         pending;                          // a start was sent, its answer (3203) not received yet
	uint16_t     pending_tech;
	uint64_t     pending_since;                    // now_ms()
	uint64_t     next_check_at;                    // now_ms(): 0 = arm on the first look
	uint64_t     retry_at[RESEARCH_ID_MAX + 1];    // per research: left alone until then after a refusal
	char         state[192];                       // last decision in words, shown by the console
} ResearchAutoSettings;

/* $askhelp: start a construction, ask the alliance for help, wait a few seconds, cancel it, again and again. One
 * cycle = RequestBuildStart -> (answer 2004) -> RequestBuildHelp -> 3 to 4 s -> RequestBuildCancel -> (answer 2007). */
typedef enum { ASKHELP_IDLE, ASKHELP_WAIT_BEGIN, ASKHELP_WAIT_HELP, ASKHELP_WAIT_CANCEL_TIMER, ASKHELP_WAIT_CANCEL_ANSWER, ASKHELP_PAUSE } AskHelpPhase;

typedef struct {
	bool           active;
	AskHelpPhase  phase;
	char           requester[64];   // who is told how it went
	uint16_t       total;           // cycles asked for
	uint16_t       done;            // cycles completed (cancel answered)
	uint16_t       slot;            // the building being started / cancelled
	uint16_t       build_id;
	int8_t         queue;           // the queue entry the start went to, -1 = not known yet
	uint8_t        topups;          // times the bag was used to cover the farm's cost during this series
	bool           retried_other;   // the cancel got no answer and was tried on the other queue once
	bool           recovering;      // the cancel in flight is of the farm a previous series left under construction: not a cycle
	uint64_t       phase_since;     // now_ms()
	uint64_t       next_at;         // now_ms(): when the current wait ends
	ResourceStock  stock_before;    // to tell what the whole thing cost in the end
	int64_t        stock_before_raw[5];
} AskHelpState;

/* Automatic construction: `build.buildings` lists, in priority order, the building types (the game's build_id) whose
 * buildings the bot takes to their maximum level. The planner (BuildPickNext) follows the prerequisites from building to
 * building; the tick starts what it picks with the plain start (RequestBuildStart, captured from the PC client) whenever a
 * construction queue is free. */
typedef struct {
	bool         enabled;
	uint16_t     types[32];                       // build_id, priority order
	uint8_t      type_count;
	ResourceStock reserve;                         // never spent by construction

	uint64_t     next_check_at;                    // now_ms()
	bool         pending;                          // a start was sent, its answer (2004) not received yet
	uint16_t     pending_slot;
	uint64_t     pending_since;                    // now_ms()
	uint16_t     blocked_slot[16];                 // slots left alone for a while after a refusal or a missing answer
	uint64_t     blocked_until[16];                // now_ms()
	uint8_t      blocked_next;                     // round-robin cursor in the two arrays above
	uint16_t     plan_slot;                        // the step the planner picked last (0 = none)
	uint16_t     plan_build_id;
	uint8_t      plan_level;
	char         state[224];                       // last decision in words, shown by the console
} BuildAutoSettings;

typedef struct {
	bool loaded;
	TroopData troop;
	TroopData healing;
	long num;
	uint total_time;
} WoundedTroopData;

/* Dead troops waiting in the sanctuary, one total per TroopKind (not broken down by tier -
 * the game doesn't either for this screen). Confirmed live: _MSG_RESP_VALHALLA_INFO (9402)
 * carries these 4 counts as 4-byte values at a fixed 16-byte stride (offset 88, 104, 120, 136
 * in TroopKind order) - the rest of that 200-byte packet (points balance, an unrelated pending-
 * job timestamp, etc.) is not decoded. These exact same numbers are what a real
 * _MSG_REQUEST_VALHALLA_DIVINE_REVIVE capture sent back verbatim to revive everyone at once. */
typedef struct {
	bool     loaded;
	uint32_t dead[4]; // indexed by TroopKind
} ValhallaInfo;



typedef struct {
    bool pending;
    int64_t execute_time;
    char leader[13];
    uint8_t level;
} PendingRally;


typedef struct {
    // Master switch for all protection features.
    bool enabled;

    /* Shield */

    // Keep a shield active continuously (24/7).
    bool shield_always_on;

    // Use a shield when an incoming attack is detected.
    bool shield_on_incoming_attack;

    // Use a shield when an incoming scout is detected.
    bool shield_on_incoming_scout;

    // Shield item preference.
    uint8_t shield_priority_count;
    uint16_t shield_priority[8];

    /* Anti-scout ("anti-espionnage"): hides from scout reports without blocking
     * attacks. Either kept active all the time (antiscout_always_on, regardless
     * of the shield), or only as a fallback whenever no shield is active
     * (antiscout_on_no_shield: shield always wins if it can be kept up, anti-
     * scout only kicks in when it can't). always_on wins if both are set. */
    bool antiscout_always_on;
    bool antiscout_on_no_shield;
    uint8_t antiscout_priority_count;
    uint16_t antiscout_priority[8];

    /* Troop recall */

    // Recall troops when an incoming attack is detected.
    bool recall_on_incoming_attack;

    // Recall troops when an incoming scout is detected.
    bool recall_on_incoming_scout;
    
    bool recall_on_incoming_conflict;
} ProtectionSettings;

/*
typedef enum {
    T1 = 0,
    T2,
    T3,
    T4,
    T5
} TroopTier;
*/

typedef enum {
    TROOP_INFANTRY = 0,
    TROOP_RANGED   = 1,
    TROOP_CAVALRY  = 2,
    TROOP_SIEGE    = 3
} TroopKind;

typedef enum {
    TIER_T1 = 0,
    TIER_T2 = 1,
    TIER_T3 = 2,
    TIER_T4 = 3,
    TIER_T5 = 4
} TroopTier;

typedef enum {
    DARKNEST_FORMATION_FIXED,
    DARKNEST_FORMATION_LEADER
} DarknestFormationMode;

typedef struct {
    // Enable automatic Darknest rally joining.
    bool enabled;
    bool auto_join;
    
    // Join only Darknest levels within this range.
    uint8_t min_level;
    uint8_t max_level;
    uint8_t max_march;
    
    uint16_t max_distance;
    bool auto_transmute;
    uint8_t essence_level;

    // Total troops to send.
    uint32_t troop_count;
    uint32_t min_join_troops;
    
    // Formation (e.g. 8480 = 80% Infantry, 40% Ranged, 80% Cavalry).
    uint16_t formation;
    // Formation selection mode.
    DarknestFormationMode formation_mode;
    
    uint8_t tier_priority_count;
    // Tier consumption order.
    TroopTier tier_order[5];

    // Random join delay (seconds).
    uint16_t min_join_delay;
    uint16_t max_join_delay;
    
    // Reserve marches for other activities.
    uint8_t max_marches_to_use;

    // Don't join if troop count falls below this percentage.
    uint8_t min_troop_percent;
} DarknestSettings;

typedef struct {
    uint32_t active_rally_count;
    uint32_t being_rally_count;
} RallyState;


typedef struct {
	int64_t data_index;
	char leader[13];
	uint8_t level;
} DarknestRally;

typedef struct {
	double current;              // Current resource amount.
	uint32_t capacity;           // Maximum storage capacity.

	int64_t production_hour;     // Production per hour (negative = consumption).
	double production_second;    // Production per second.

	double update_timer;         // 1-second update accumulator.
} ResourceTracker;


typedef struct {
    char     name[13];
    uint8_t  vip;
    uint8_t  rank;
    int64_t  begin_time;
    uint32_t require_time;
    uint32_t troop_total;
    uint32_t troops[20];
} RallyMember;

typedef struct {
    uint32_t index;             // Rally index/id.
    uint8_t  kind;              // Rally type.

    int64_t  begin_time;        // Rally start time.
    uint32_t require_time;      // Time until march starts.

    /* Rally leader */
    uint16_t ally_zone_id;
    uint8_t  ally_point_id;
    uint16_t ally_head;
    char     ally_name[13];
    uint8_t  ally_vip;
    uint8_t  ally_rank;

    uint32_t ally_curr_troop;
    uint32_t ally_max_troop;

    uint16_t ally_home_kingdom;

    /* Target (Darknest) */
    uint16_t enemy_head;        // Always UINT16_MAX for NPC rallies.
    uint16_t enemy_zone_id;
    uint8_t  enemy_point_id;
    uint8_t  enemy_vip;
    uint16_t enemy_npc_id;

} NPCRally;

typedef struct {
	uint8_t  type;
	uint32_t index;
	uint8_t  kind;
	
	int64_t  begin_time;
	uint32_t require_time;
	
	/* Rally leader */
	uint16_t ally_zone_id;
	uint8_t  ally_point_id;
	uint16_t ally_head;
	char     ally_name[13];
	uint8_t  ally_vip;
	uint8_t  ally_rank;
	
	uint32_t ally_curr_troop;
	uint32_t ally_max_troop;
	
	/* Rally target */
	uint16_t enemy_zone_id;
	uint8_t  enemy_point_id;
	uint16_t enemy_head;
	char     enemy_name[13];
	uint8_t  enemy_vip;
	uint8_t  enemy_rank;
	
	char     enemy_alliance_tag[4]; // +1 for '\0'
	uint16_t enemy_home_kingdom;
} Rally;


typedef enum {
    TRANSFER_IDLE,
    TRANSFER_FIND_TARGET,
    TRANSFER_WAIT_TARGET,
    TRANSFER_SEND_MARCH,
    TRANSFER_WAIT_MARCH,
    TRANSFER_COMPLETE,
    TRANSFER_FAILED
} TransferState;


/* Who asked for a relocation, so the server's answer can be reported back to them. */
typedef struct {
    char   report_to[13];
    time_t report_until;
} RelocationReport;

/* Kingdom migration in progress: ask the target kingdom's server, then send the teleport request. */
typedef enum {
    MIGRATION_IDLE,
    MIGRATION_WAIT_SERVER,
    MIGRATION_WAIT_RESULT,
    MIGRATION_WAIT_SCROLL_RESULT
} MigrationState;

typedef struct {
    MigrationState state;
    char     requester[13];
    uint16_t kingdom_id;
    uint16_t x, y;
    uint16_t zone_id;
    uint8_t  point_id;
    time_t   deadline;
} Migration;

/* One resource of a $adminrss/$rss batch: up to TRANSFER_BATCH_MAX resources sent one after another
 * to the same target, in a fixed priority order (see BuildPriorityLines() in command.c) - gold first,
 * food last, since food is the one the recipient is least likely to be short on. */
#define TRANSFER_BATCH_MAX 5

typedef struct {
    ResourceType type;
    uint32_t     amount;   // GROSS amount: the delivery tax is already added
} TransferLine;

typedef struct {
	char issued_name[13]; // Who initiated resource command?
    char target_name[13]; // Who will receive resource?

    ResourceType resource_type;
    ResourceStock resource;

    time_t timeout;

    uint8_t max_marches;
    uint8_t cur_marches;

    uint32_t amount;
    uint32_t remaining;

    /* $adminrss/$rss: the resources still to come after this one. resource_type/amount/remaining
     * above always describe lines[line_index], the one currently being delivered - single-resource
     * commands ($food, $adminfood...) just set line_count = 1 and never touch line_index. */
    TransferLine lines[TRANSFER_BATCH_MAX];
    uint8_t      line_count;
    uint8_t      line_index;

    uint16_t zone_id;
    uint8_t point_id;

    uint64_t not_before; /* now_ms() deadline: do not start before this (bag credit wait, human pacing) */

    /* Guild bank (guildbank.h): the marches of a withdrawal are debited from balance_owner's balance as they leave. */
    bool     from_balance;
    char     balance_owner[13];
    uint32_t in_flight;      /* debited for a march sent but not accepted yet: given back if it is refused */

    bool     ignore_reserve; /* $adminall: send everything, the configured reserve included - never set for from_balance (a
                               * member's own deposit is never the bot's reserve to respect) or for anything else */

    bool     ignore_deposits; /* $adminrss: the admin can dip into what members have deposited - only an empty stock
                                * blocks it. Never set for from_balance or for $admin<resource>/$adminall. */

    TransferState state;
} ResourceTransfer;

/* A request waiting for its turn: with a guild bank several members ask at once, one delivery goes at a time. */
typedef struct {
    char         requester[13];  // who asked, told about the result
    char         target[13];     // who receives
    TransferLine lines[TRANSFER_BATCH_MAX];
    uint8_t      line_count;
    bool         from_balance;   // taken from the requester's guild balance, else from the stock
    bool         ignore_reserve; // $adminall: see ResourceTransfer's own field
    bool         ignore_deposits; // $adminrss: see ResourceTransfer's own field
} TransferRequest;

#define TRANSFER_QUEUE_MAX 16

typedef struct {
    bool enabled;   // guildbank.enabled: deposits are kept, the resource commands take them back
} GuildBankSettings;



#define MAX_ALLIANCE_MEMBER 100

typedef struct {
    int64_t user_id;
    uint16_t head;
    char name[14];
    uint8_t rank;
    uint64_t power;
    uint64_t troop_kill_num;
    int64_t logout_time;
    uint8_t white_list_flag;
} AllianceMember;

typedef struct {
    AllianceMember member[MAX_ALLIANCE_MEMBER];
    uint16_t recv_index;
    uint8_t data_finished;
    uint8_t count;
} AllianceMemberList;

typedef struct {
	// network
	int sock;
	PacketStream stream;
	// authentication
	AuthInfo auth;
	// game items
	Item items[MAX_ITEM_COUNT];
	bool items_loaded;
	// protocol state
	ProtocolState protocol;
	// outgoing packet buffer 
	// PacketBuffer reader;
	
	/*
	Stream sin;
	Stream sout;
	*/
	PacketBuffer sin;
	PacketBuffer sout;
	
	uint8_t data[4096];
	uint16_t size;
	uint16_t offset;
	// server state
	uint64_t server_time;
	time_t last_heartbeat;
	// login state
	bool lobby_login;
	// set once the game server accepted the login
	bool game_logged_in;
	// number of migration scrolls the game asks for this account (migration.scrolls_needed)
	uint16_t migration_scrolls_needed;
	// cargo ship: wait until bag items used for a trade are credited
	time_t market_bag_wait;
	// game server 
	ServerInfo game_server;
	ServerInfo gateway_server;
	
	// client configuration
	AppInfo app;
	// resources 
	ResourceStock resources;      // current resources
	ResourceStock bag_resources;  // UNUSED: nothing fills it - the bag is read with BagTotal() (protocol.h)
	ResourceProduction production; // Per-hour production
	uint64_t resources_last_update;
	
	ChatState chat;
	
	bool resource_loaded;
	// player information 
	PlayerInfo player;
	// game system 
	BlackMarket market;
	BlackMarketBuy buy;
	
	// resources sending
	// ResourceTransfer transfer;
	
	MailInfo mail;
	
	AllianceHelp help;
	
	uint8_t building_count;
	BuildingInfo building[256];
	int64_t  construction_extra_expires;  // the 8 bytes after the entries of _MSG_RESP_BUILDINGEVENT: when the second queue stops (INT64_MAX = permanent)
	bool     construction_loaded;         // that packet was received
	BuildingConstruction construction[BUILDING_QUEUE_SLOTS]; // what is being built, from _MSG_RESP_BUILDINGEVENT
	
	AllianceGiftList alliance_gifts;
	AllianceSettings alliance;
	SmartUseList smart_use;
	ActivitySettings activity;
	WarSettings war;
	NotifySettings notify;
	AllianceOp alliance_op;
	GatherSettings gather;
	RecallState recall;
	GuildChatLog guild_chat_log;

	HelpSpam help_spam;
	
	BankSettings bank;
	
	BotSettings bot;
	ReconnectSettings reconnect;
	
	AllianceInfo RoleAlliance;
	
	PlayerBank player_bank[1000];
	
	// ShieldSettings shield;
	
	ShieldInfo shield_info;
	AntiScoutInfo antiscout_info;
	
	HyperSettings hyper;
	
	TroopData troop;
	AutoTrainSettings autotrain;
	ResearchAutoSettings research_auto;
	BuildAutoSettings build_auto;
	AskHelpState askhelp;
	TrainingSlot training[4]; // indexed by TroopKind - see TrainingSlot's comment

	WoundedTroopData wounded;
	ValhallaInfo valhalla;
	
	ProtectionSettings protection;
	
	PendingRally rally;
	
	DarknestSettings darknest;
	
	RallyState rally_status;
	
	// Research information (layout and what is known: research.h, docs/research.md)
	ResearchState research;
	
	uint32_t supply_capacity;
	uint8_t  trading_post_level; // BUILDINGINFO level of the Trading Post (mana part included, buildings.h), 0 = not received

	/* Delivery tax really applied by the game, read from its delivery report
	 * (RecvResHelpReport). The rate is never sent as such: the report only carries the net
	 * amount, so it is rebuilt from it. Overrides bank.delivery_tax_percent once known. */
	double   delivery_tax_seen;
	bool     delivery_tax_seen_valid;
	uint32_t last_gross;   /* amount the server counted for the last march sent, until its report arrives */
	
	ResourceTracker tracker;
	
	NPCRally npc_rallies[30]; // maximum 30 npc rallies hold
	
	Rally ally_rallies[30];     // Rallies opened by our alliance.
	Rally enemy_rallies[30];    // Enemy rallies targeting us.
	
	RallyMember rally_members[30];
	
	ResourceTransfer transfer;
	TransferRequest transfer_queue[TRANSFER_QUEUE_MAX];
	uint8_t transfer_queue_count;
	GuildBankSettings guildbank;
	RelocationReport relocation;
	Migration migration;
	
	AllianceMemberList alliance_member;
} Connection;

/* API */
int  connect_server(const char *ip, unsigned short port);
void disconnect(Connection *conn);
bool send_packet(Connection *conn, bool enc);
int set_nonblocking(Connection *conn);
void reset_connection(Connection *c);

#endif
