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

/* Automatic resource-tile gathering. Tiles are known only from whatever
 * _MSG_RESP_UPDATE_MAPINFO(_PLUS) arrives on its own - live-tested twice, the
 * server never answers _MSG_REQUEST_MAPDATA sent by this bot (0 responses to 22,
 * then 23, correctly zoned and paced requests), unlike direct actions (marches,
 * resources...) which all work. Same wall the war feature hit; see its own header
 * comment. So, for now, this only gathers tiles a human happens to have shown the
 * account by opening the map - not a true hands-off scan.
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
    uint16_t zone_id;
    uint8_t  point_id;
    uint8_t  resource_kind; // GatherResourceKind
    uint8_t  level;         // 1-5
    uint32_t amount;
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

typedef struct {
    bool     enabled;
    uint8_t  max_marches;   // out of player.max_marches, how many to use for gathering

    bool     scan_done;     // just gates the one-time "no active scan" warning below

    uint8_t  active_marches; // gather marches this code has out right now (subset of player.current_marches)
    uint64_t next_march_at;  // now_ms() deadline: do not send another gather march before this

    GatherTile tiles[GATHER_MAX_TILES];
    uint16_t   tile_count;
    time_t     last_status_log; // throttles the periodic "N tiles known" log line
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

typedef struct {
	bool loaded;
	TroopData troop;
	TroopData healing;
	long num;
	uint total_time;
} WoundedTroopData;



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

    uint16_t zone_id;
    uint8_t point_id;
    
    uint64_t not_before; /* now_ms() deadline: do not start before this (bag credit wait, human pacing) */

    /* Guild bank (guildbank.h): the marches of a withdrawal are debited from balance_owner's balance as they leave. */
    bool     from_balance;
    char     balance_owner[13];
    uint32_t in_flight;      /* debited for a march sent but not accepted yet: given back if it is refused */

    TransferState state;
} ResourceTransfer;

/* A request waiting for its turn: with a guild bank several members ask at once, one delivery goes at a time. */
typedef struct {
    char         requester[13];  // who asked, told about the result
    char         target[13];     // who receives
    ResourceType type;
    uint32_t     amount;         // GROSS amount to send: the delivery tax is already added
    bool         from_balance;   // taken from the requester's guild balance, else from the stock
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
	ResourceStock bag_resources;  // consumable resource items
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
	
	WoundedTroopData wounded;
	
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
