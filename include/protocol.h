#ifndef PROTOCOL_H
#define PROTOCOL_H

#include <stdint.h>
#include "connection.h"

void RequestGuestLogIn(Connection *conn);
void RequestLogIn(Connection *conn);
void RequestClientInitOver(Connection *conn);
void RequestHeartBeat(Connection *conn);

void RequestTroopTraining(Connection *c, uint8_t kind, uint8_t tier, uint32_t amount);

void RequestTroopRecall(Connection *c, uint8_t Index);
void RequestViewChat(Connection *c, uint8_t channel, uint8_t prev, int8_t kind, int64_t DataID, int64_t DataTime);

void RequestSendChat(Connection *c, uint8_t channel, const char *message);

void RequestRallyList(Connection *c);
void RequestRallyDetail(Connection *c, uint8_t arg1, uint32_t arg2);

void RequestJoinRally(Connection *c, const char *ally_name, const uint32_t troop_array[16]);


void Send_Mall_TestBuy(Connection *c, uint16_t type);


void RequestBlackMarketData(Connection *c);
void RequestBlackMarketBuy(Connection *c, uint8_t mIdx);
void RequestSmartUseBlackMarketBuy(Connection *c, SmartUseList smart_use, uint8_t mIdx);
void SendBlackMarketBuy(Connection *c, uint8_t mIdx);

void RequestMissionInfo(Connection *c, uint8_t missionType);
void RequestAllyPoint(Connection *c, const char *name);
void RequestMapAdvance(Connection *c, uint16_t zone_id, uint8_t point_id);
uint64_t now_ms(void);

void RequestWatchTowerLineDetail(Connection *c, uint32_t);
void RequestTroopTakeBack(Connection*, uint8_t);

void RequestSendHelp(Connection *c, uint16_t record_sn_count, const uint32_t *record_sn);
void SendStartBuilding(Connection *c, uint16_t position_id, uint16_t build_id, uint8_t operation_type);
void ServerNewbieTeleport(Connection *c, uint16_t kingdom_id, uint16_t zone_id, uint8_t point_id);
void ServerRelocate(Connection *c, uint16_t kingdom_id, uint16_t zone_id, uint8_t point_id);
void RequestAllianceGiftInfo(Connection*);
void RequestOpenAllianceGift(Connection*, uint32_t);

void RequestDeleteAllianceGiftBox(Connection*, uint32_t);

/* VIP 12+ bulk gift actions: open every ready box, then clear every expired one, in two clicks
 * instead of one per box. See the comment above RequestOpenAllAllianceGiftBox() in protocol.c. */
void RequestOpenAllAllianceGiftBox(Connection *c);
void RequestAllianceGiftCheckExpired(Connection *c, uint8_t kind);
void RecvAllianceGiftOpenAll(Connection *c, const uint8_t *data, uint16_t size);
void RecvAllianceGiftCheckExpired(Connection *c, const uint8_t *data, uint16_t size);

void ServerRename(Connection *c, bool bought, uint16_t num, const char *name);
void RequestBuyItem(Connection *c, uint8_t Type, uint16_t Key, uint16_t ItemID, uint16_t Qty);
void RequestBuyGiftItem(Connection *c, uint8_t Type, uint16_t Key, uint16_t ItemID, uint16_t Qty, const char Name[13]);


void RequestSimpleUseItem(Connection *c, uint32_t item_id, uint16_t quantity);
void RequestUseAdvancedRelocator(Connection *c, uint16_t kingdom_id, uint16_t zone_id, uint8_t point_id);
void RecvUseItem(Connection *c, const uint8_t *data, uint16_t);

void ServerMagicGateDoEvent(Connection *c, uint16_t n, uint8_t x);


void RecvLoginError(Connection *c, const uint8_t *data);
void HandleLoginValidate(Connection *c, const uint8_t *data, uint16_t size);
void RecvChatMessage(Connection *c, const uint8_t *data);

void RecvAllBuildData(Connection *c, const uint8_t *data);

void RecvItemInfo(Connection *c, const uint8_t *data, uint16_t size);
void RecvIBuffInfo(Connection *c, const uint8_t *data);
void RecvMarchData(Connection*, const uint8_t*);
void RecvLoginRoleInfo(Connection *c, const uint8_t *data, uint16_t size);

void RecvResources(Connection *c, const uint8_t *data);

void RecvBlackMarket_Data(Connection *c, const uint8_t *data);
void RecvBlackMarket_Buy(Connection *c, const uint8_t *data);
void RecvMailInfo(Connection *c, const uint8_t *data);


void RecvAllyPoint(Connection *c, const uint8_t *data);

void RecvAllianceHelp(Connection *c, const uint8_t *data);
void RecvAllianceMemberNeedsHelp(Connection *c, const uint8_t *data);
void RecvPendingAllianceMembersNeedHelp(Connection *c, const uint8_t *data);
void RecvAllianceGiftInfo(Connection *c, const uint8_t *data);
void RecvRoleUpdateInfo(Connection *, const uint8_t*);

void RecvAllianceGiftOpen(Connection *c, const uint8_t *data);
void RecvDeleteAllianceGiftBox(Connection*, const uint8_t*);

void RecvBuyItem(Connection *c, const uint8_t *data, uint16_t size);
void RecvArmyGroupInfo(Connection *c, const uint8_t *data, uint16_t size);
void RecvTroopTrainingImmediate(Connection *c, const uint8_t *data, uint16_t size);
void RecvAddSoldier(Connection *c, const uint8_t *data, uint16_t size);
void RecvTrainingStart(Connection *c, const uint8_t *data, uint16_t size);
void RecvTrainingInfo(Connection *c, const uint8_t *data, uint16_t size);
uint32_t BarracksCapacityFloor(const Connection *c);
uint32_t TroopsAffordable(const Connection *c, uint8_t kind, uint8_t tier, bool with_bag, int *short_resource);
void AutoTrainTick(Connection *c);
void RecvWoundedTroopData(Connection *c, const uint8_t *data);
void RecvValhallaInfo(Connection *c, const uint8_t *data, uint16_t size);
void RequestHealingTroop(Connection *c);
void RequestValhallaDivineRevive(Connection *c);

void RecvRefreshResources(Connection *c, const uint8_t *data);

void HeartbeatTick(Connection *c);
void BlackMarketTick(Connection *c);
const char *GetShieldName(uint16_t item_id);
bool HasAnyShieldItem(Connection *c);
bool HasAnyAntiScoutItem(Connection *c);
void UsePriorityShield(Connection *c);
void UsePriorityAntiScout(Connection *c);
void ShieldTick(Connection *c);
void AntiScoutTick(Connection *c);
void AllianceGiftTick(Connection*);
uint8_t GetVIPLevel(uint32_t vipPoints);
void RecvAllianceInfo(Connection*, const uint8_t*);

void RequestTreasureGetDoubleTicket(Connection *c);
void RecvTreasureGetDoubleTicket(Connection *c, const uint8_t *data, uint16_t size);
void RequestOnlineGift(Connection *c);
void RecvOnlineGift(Connection *c, const uint8_t *data, uint16_t size);
void ActivityTick(Connection *c);

void RecvMapInfoPlus(Connection *c, const uint8_t *data, uint16_t size);
void WarTick(Connection *c);
void NotifyDiscord(Connection *c, const char *message);

void RequestMapData(Connection *c, uint16_t zone_id);
void RequestGatherMarch(Connection *c, uint16_t zone_id, uint8_t point_id, uint8_t kind, uint32_t troop_count);
void RequestGatherMarchMixed(Connection *c, uint16_t zone_id, uint8_t point_id, const uint32_t counts[4]);
void RequestGatherRecall(Connection *c, uint32_t march_id);

/* $recall: take every march back, then send none for recall.pause_seconds (5 minutes by default). The pause
 * lives outside Connection so it holds across a reconnection. */
typedef enum {
	RECALL_STARTED,      // requests are going out, one every 1-2 s
	RECALL_RUNNING,      // a recall is already under way: only the pause was renewed
	RECALL_NO_MARCHES,   // no march out: nothing to take back
	RECALL_NO_DATA       // the march counts have not been received yet
} RecallResult;

bool     MarchesPaused(void);
uint32_t MarchesPauseSecondsLeft(void);
void     MarchesPauseStart(uint32_t seconds);
void     MarchesPauseEnd(void);
void     FormatDurationFr(uint32_t seconds, char *out, size_t size);
RecallResult StartRecall(Connection *c, const char *requester);
void     RecallTick(Connection *c);

/* Deliveries (resource commands and the guild bank). */
const char *TransferRequester(const Connection *c);
void     AbortTransfer(Connection *c);
bool     TransferQueuePush(Connection *c, const TransferRequest *request);
int      TransferQueuePosition(const Connection *c, const char *requester);
bool     TransferQueueRemove(Connection *c, const char *requester);
uint32_t StockAvailable(const Connection *c, ResourceType type, bool from_balance, bool ignore_reserve, bool ignore_deposits);
void RecvGatherMarchResp(Connection *c, const uint8_t *data, uint16_t size);
void RecvGatheringEvent(Connection *c, const uint8_t *data, uint16_t size);
void RecvGatherReturnResp(Connection *c, const uint8_t *data, uint16_t size);
void RecvGatherTroopHome(Connection *c, const uint8_t *data, uint16_t size);
void RecvGatherReportInfo(Connection *c, const uint8_t *data, uint16_t size);
void GatherTick(Connection *c);
void RecvAntiScoutReportInfo(Connection *c, const uint8_t *data, uint16_t size);

void RequestAllianceQuit(Connection *c);
void RequestAllianceSearchByTag(Connection *c, const char *tag);
void RequestAllianceApplyById(Connection *c, uint32_t alliance_id);
void RecvAllianceQuitResp(Connection *c, const uint8_t *data, uint16_t size);
void RecvAllianceSearchResult(Connection *c, const uint8_t *data, uint16_t size);
void RecvAllianceApplyResp(Connection *c, const uint8_t *data, uint16_t size);
void AllianceOpTick(Connection *c);

void RecvBuildingQueue(Connection*, const uint8_t*, uint16_t);

void RecvUpdateWatchTowerAddLineInfo(Connection*, const uint8_t*);
void RecvWatchTowerLineDetail(Connection *c, const uint8_t *data);
const char *FormatTime(uint32_t totalSecs);


void RecvDarknestBroadcast(Connection *c, const uint8_t *data);

void RecvRallyCountData(Connection*, const uint8_t*);

void DarknestRallyTick(Connection *);

void RecvWarBegin(Connection *c, const uint8_t *data);

void RecvNPCWallHallData(Connection *c, const uint8_t *data);
void RecvWallHallTroop(Connection*, const uint8_t*);
void RecvNPCWallHallDetail(Connection*, const uint8_t*);
void RecvWallHallDel(Connection *c, const uint8_t *data);
void RecvWallHallData(Connection *c, const uint8_t *data);
void RecvWallHallDetail(Connection *c, const uint8_t *data);
void RecvTechnologyInfo(Connection*, const uint8_t*, uint16_t);


void RecvAddConflictLine(Connection *c, const uint8_t *data);
void RecvMapInfoPlus(Connection *c, const uint8_t *data, uint16_t size);

void RecvMagicGateDoEvent(Connection *c, const uint8_t *data);

void RequestUnknown(Connection *c);
void RecvWallHallDetailClose(Connection *c, const uint8_t *data);


void RecvJoinedRallyData(Connection *c, const uint8_t *data);

void ResourceTransferTick(Connection *c);


void RequestSendMail(Connection *c, const char *player_name, const char *subject, const char *message);
void RequestSendMailFmt(Connection *c, const char *player_name, const char *subject, const char *fmt, ...);

void RecvSHelp(Connection *c, const uint8_t *data);
void RecvHelp_Home(Connection *c, const uint8_t *data);
void RequestResearchStart(Connection *c, uint16_t tech_id, uint8_t level, const ResearchItemUse *items, uint16_t item_count);
void RequestResearchStartPlain(Connection *c, uint16_t tech_id, uint8_t level);
uint8_t AcademyLevel(const Connection *c);
typedef struct {
    uint16_t id;        // the research to start
    uint8_t  level;     // the level to request (its current level + 1)
    uint16_t for_id;    // the research of the goal it serves (differs from id when id is a prerequisite)
    uint16_t unlocks;   // how many goal researches wait for this very step
} ResearchPick;
uint16_t ResearchPickNext(const Connection *c, uint8_t kind, uint16_t only_id, ResearchPick *pick, char *why, size_t why_size, bool auto_mode);
size_t ResearchFindKinds(const char *query, const ResearchKindInfo **found, size_t max);
void ResearchAutoTick(Connection *c);
typedef struct {
    uint16_t slot;        // where the building stands
    uint16_t build_id;
    uint8_t  level;       // the level to build
    uint16_t for_slot;    // the goal building it serves (differs from slot when this one is a prerequisite)
    uint16_t for_id;
    uint16_t unlocks;     // how many goals wait for this very step
} BuildPick;
bool BuildPickNext(const Connection *c, uint16_t build_id, BuildPick *pick, char *why, size_t why_size);
size_t BuildFindTypes(const char *query, const BuildingTypeInfo **found, size_t max);
void BuildAutoTick(Connection *c);
void RequestBuildStart(Connection *c, uint16_t slot, uint16_t build_id);
void RecvBuildBegin(Connection *c, const uint8_t *data, uint16_t size);
void RecvBuildComplete(Connection *c, const uint8_t *data, uint16_t size);
void RecvBuildingError(Connection *c, const uint8_t *data, uint16_t size);
void RecvBuildCancel(Connection *c, const uint8_t *data, uint16_t size);
void RecvBuildHelpAnswer(Connection *c, const uint8_t *data, uint16_t size);
void RequestBuildHelp(Connection *c, uint8_t queue);
void RequestBuildCancel(Connection *c, uint8_t queue);
bool AskHelpStart(Connection *c, const char *requester, uint16_t total, char *error, size_t error_size);
int BuildingReservedFarm(const Connection *c);
void AskHelpStop(Connection *c, const char *why);
void AskHelpTick(Connection *c);
void RequestResearchCancel(Connection *c, uint16_t tech_id, uint8_t level);
uint16_t ResearchSnapshotDiff(Connection *c);
void RecomputeSupplyCapacity(Connection *c);
uint16_t BuildingSnapshotDiff(Connection *c);
void RecvResearchStart(Connection *c, const uint8_t *data, uint16_t size);
void RecvResearchCancel(Connection *c, const uint8_t *data, uint16_t size);
void RecvResearchComplete(Connection *c, const uint8_t *data, uint16_t size);
void RecvResHelpReport(Connection *c, const uint8_t *data, uint16_t size);
double DeliveryTaxPercent(const Connection *c);


void format_number2(uint64_t num, char *out, size_t size);

void EvaluateBlackMarket(Connection *c);

/* Resource items in the bag (bank.use_bag_*, cargo_ship.use_bag_rss) */
#define BAG_PLAN_MAX 8

typedef struct {
	uint16_t item_id;
	uint16_t quantity;
} BagUse;

uint64_t BagTotal(const Connection *c, ResourceType type);
int  BagPlan(const Connection *c, ResourceType type, uint64_t need, BagUse out[BAG_PLAN_MAX]);
void BagApply(Connection *c, const BagUse *plan, int count, ResourceType type);

/* Answers a player through the configured command.output channel. Defined in command.c. */
void BotReply(Connection *c, const char *player_name, const char *subject, const char *fmt, ...);
void BotReplyTo(Connection *c, const char *player_name, const char *subject, CommandChannel channel, const char *fmt, ...);

/* Tells whoever asked for a relocation how it went. Defined in command.c. */
void ReportRelocation(Connection *c, bool ok, uint8_t status);

/* Kingdom migration. The packet layouts come from a capture of the official client. */
void RequestKingdomServer(Connection *c, uint16_t kingdom_id);
void RequestFreeCrossTeleport(Connection *c, uint16_t kingdom_id, uint16_t zone_id, uint8_t point_id);
void RequestMigrationScroll(Connection *c, uint16_t kingdom_id, uint16_t zone_id, uint8_t point_id);
void RecvMigrationScrollResult(Connection *c);
bool MigrationStart(Connection *c, const char *requester, uint16_t kingdom_id, uint16_t x, uint16_t y, uint16_t zone_id, uint8_t point_id);
void MigrationScrollStatus(const Connection *c, char *out, size_t size);
void RecvKingdomServer(Connection *c, const uint8_t *data, uint16_t size);
void RecvFreeCrossTeleport(Connection *c, const uint8_t *data);
void RecvCrossKingdomClose(Connection *c, const uint8_t *data, uint16_t size);
void MigrationTick(Connection *c);

void RecvAllianceMemberInfo(Connection *c, const uint8_t *data);
void RequestAllianceMemberInfo(Connection *c);

#endif