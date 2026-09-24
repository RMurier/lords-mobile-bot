#ifndef BUILDINGS_H
#define BUILDINGS_H

/*
 * Buildings: how the game numbers their levels, and what the Trading Post's level gives.
 * What is confirmed and what is not: docs/buildings.md. Data of every building: gamedata/buildings.json.
 *
 * _MSG_RESP_BUILDINGINFO (2001) lists every building as { u16 slot, u16 build_id, u8 level }.
 * That ONE level number goes on after 25, the normal maximum, because the Mana upgrade comes next: 6 mana
 * levels, each reached in 5 steps (the wiki: "you need to build it 5 times at the current stage").
 * Confirmed by the player who owns the accounts, and by a capacity read in the game:
 *
 *    25      normal maximum, mana level 0
 *    26..29  still mana level 0, with 1 to 4 steps of mana 1 done
 *    30      mana 1                      (34 is still mana 1 with 4 steps of mana 2, 35 is mana 2)
 *    32      mana 1, 2 steps of 5 toward mana 2
 *    35      mana 2
 *    55      mana 6, the maximum (25 + 6 x 5)
 *
 * so the mana level is (level - 25) / 5 and the steps toward the next one (level - 25) % 5. A building
 * that has no Mana upgrade (Watchtower, Treasure Trove) stays at 25 (9 for the Treasure Trove).
 */

#include <stdint.h>

#define BUILDING_NORMAL_MAX_LEVEL 25
#define BUILDING_MANA_STEPS       5
#define BUILDING_MANA_MAX         6
#define BUILDING_MAX_LEVEL        (BUILDING_NORMAL_MAX_LEVEL + BUILDING_MANA_MAX * BUILDING_MANA_STEPS)

/* build_id of BUILDINGINFO. The ones the bot has always known, the Mana Lode (confirmed: the player's building
 * under construction, mana 1 -> 1 + 1/5, is build_id 26 in the construction packet), and the Treasure Trove
 * (inferred: the only building with 9 levels, and the accounts have level 9 and 4 on it). The other ids are not
 * identified. */
typedef enum {
    BUILDING_LUMBER_MILL    = 1,
    BUILDING_QUARRY         = 2,
    BUILDING_MINE           = 3,
    BUILDING_FARM           = 4,
    BUILDING_MANOR          = 5,
    BUILDING_BARRACKS       = 6,
    BUILDING_INFIRMARY      = 7,
    BUILDING_CASTLE         = 8,
    BUILDING_VAULT          = 9,
    BUILDING_ACADEMY        = 10,
    BUILDING_CASTLE_WALL    = 12,
    BUILDING_WATCHTOWER     = 13,
    BUILDING_EMBASSY        = 14,
    BUILDING_WORKSHOP       = 15,
    BUILDING_TREASURE_TROVE = 16,
    BUILDING_TRADING_POST   = 17,
    BUILDING_MANA_LODE      = 26
} BuildingId;

/* _MSG_RESP_BUILDINGEVENT (2002), at login: what is being built, 44 bytes = 2 entries of 18 bytes, then 8 bytes
 * (an i64 that is INT64_MAX in the four captures, meaning unknown). An entry:
 *
 *    0   u8    1 when the entry is used (the queue's kind, only 1 seen), 0 when empty
 *    1   u16   slot of the building (the same number BUILDINGINFO gives)
 *    3   u16   build_id
 *    5   u8    level being built: the TARGET level, current + 1
 *    6   u64   start time, server clock, seconds
 *    14  u32   duration in seconds
 *
 * Confirmed: the account building the Mana Lode from level 30 (mana 1) to 31 (mana 1 + 1/5) sends
 * { 1, slot 62967, build_id 26, level 31, ... }, the other account, with nothing under construction,
 * sends two empty entries. Only seen at login: whether an update uses the same layout is not known. */
#define BUILDING_QUEUE_SLOTS      2
#define BUILDING_QUEUE_ENTRY_SIZE 18
#define BUILDING_QUEUE_INFO_SIZE  (BUILDING_QUEUE_SLOTS * BUILDING_QUEUE_ENTRY_SIZE + 8)

typedef struct {
    uint8_t  used;        /* 0 = empty entry */
    uint16_t slot;
    uint16_t build_id;
    uint8_t  level;       /* target level */
    int64_t  start_time;
    uint32_t duration;
} BuildingConstruction;

/* Seconds left on a building under construction at server time `now`, 0 when empty or over. */
static inline int64_t BuildingConstructionSecondsLeft(const BuildingConstruction *b, int64_t now)
{
    if (!b->used)
        return 0;
    int64_t end = b->start_time + (int64_t)b->duration;
    return end > now ? end - now : 0;
}

/* Level 0..25 of the building without its mana part. */
static inline uint8_t BuildingNormalLevel(uint8_t level)
{
    return level > BUILDING_NORMAL_MAX_LEVEL ? BUILDING_NORMAL_MAX_LEVEL : level;
}

/* Mana level reached, 0..6. */
static inline uint8_t BuildingManaLevel(uint8_t level)
{
    if (level <= BUILDING_NORMAL_MAX_LEVEL)
        return 0;
    if (level >= BUILDING_MAX_LEVEL)
        return BUILDING_MANA_MAX;
    return (uint8_t)((level - BUILDING_NORMAL_MAX_LEVEL) / BUILDING_MANA_STEPS);
}

/* Steps done (0..4) toward the next mana level; 0 at the top. A level 32 is mana 1 with 2 steps. */
static inline uint8_t BuildingManaSteps(uint8_t level)
{
    if (level <= BUILDING_NORMAL_MAX_LEVEL || level >= BUILDING_MAX_LEVEL)
        return 0;
    return (uint8_t)((level - BUILDING_NORMAL_MAX_LEVEL) % BUILDING_MANA_STEPS);
}

/* --- Trading Post (wiki page "Trading Post", cross-checked against the accounts) ---------------------- */

/* Supply capacity of the building alone, in resource, for a normal level 1..25 (0 = not built). Read off the
 * wiki; the table the bot already had is the same, level for level. */
static inline uint32_t TradingPostBaseCapacity(uint8_t level)
{
    static const uint32_t capacity[BUILDING_NORMAL_MAX_LEVEL + 1] = {
        0, 5000, 15000, 30000, 50000, 75000, 105000, 140000, 180000, 225000, 275000, 330000, 400000, 490000,
        600000, 730000, 880000, 1050000, 1250000, 1450000, 1650000, 1850000, 2050000, 2250000, 2500000, 3000000
    };
    return capacity[BuildingNormalLevel(level)];
}

/* Each mana step adds 6,000: level 30 = 3,000,000 + 5 x 6,000 = 3,030,000, which is exactly what the game
 * showed for the account whose Trading Post is at 30 once the 1,350,000 of its "Bigger Bags" researches were
 * added (4,380,000). Above the maximum level the value stops growing. */
#define TRADING_POST_CAPACITY_PER_MANA_STEP 6000

static inline uint32_t TradingPostCapacity(uint8_t level)
{
    if (level == 0)
        return 0;
    if (level > BUILDING_MAX_LEVEL)
        level = BUILDING_MAX_LEVEL;
    uint32_t mana_steps = level > BUILDING_NORMAL_MAX_LEVEL ? (uint32_t)(level - BUILDING_NORMAL_MAX_LEVEL) : 0;
    return TradingPostBaseCapacity(level) + mana_steps * TRADING_POST_CAPACITY_PER_MANA_STEP;
}

/* Supply tax the building sets before the "Tax Break" research takes off its part, in percent. The wiki gives
 * whole numbers, which look rounded for the low levels (30, 29, 29, 28...): trust the level 25 value, 8%, which the
 * two accounts confirm (7.5% and 7.6% once Tax Break's 0.5% and 0.4% are subtracted), and treat the others as
 * approximate. The mana levels do not change it. */
static inline double TradingPostSupplyTaxPercent(uint8_t level)
{
    static const uint8_t tax[BUILDING_NORMAL_MAX_LEVEL + 1] = {
        0, 30, 29, 29, 28, 28, 27, 27, 26, 26, 25, 24, 23, 22, 21, 20, 19, 18, 17, 16, 15, 14, 13, 12, 11, 8
    };
    return tax[BuildingNormalLevel(level)];
}

#endif
