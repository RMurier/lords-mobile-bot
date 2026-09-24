#ifndef RESEARCH_H
#define RESEARCH_H

/*
 * Research (technologies): what the server tells the bot, and how to read it.
 * Protocol notes, what is confirmed and what is not: docs/research.md.
 *
 * Confirmed from packet captures of the official PC client on two accounts:
 *
 *   _MSG_RESP_RESEARCHINFO (3201), sent once at login, 265 bytes:
 *
 *     0    u16   tech id of the research in progress (0 = none)
 *     2    u8    unknown (6 in the only capture with a research running)
 *     3    i64   start time of that research (server clock, seconds)
 *     11   u32   its total duration in seconds
 *     15   250 bytes: the current level of every research, 4 bits each
 *                  id odd  -> low nibble of byte (id - 1) / 2
 *                  id even -> high nibble of that byte
 *                ids 1..500. A level is 0..10 in every capture (15 is the most a nibble
 *                can hold, the game's own maximum per research is NOT in this packet).
 *
 * NOT sent by the server, so not available to the bot from the network: research names,
 * per-research maximum level, effects, costs, prerequisites, categories. Opening the
 * research window and all 16 categories sent no request at all (only keepalives): the client
 * reads them from its own data. See docs/research.md for what has to be filled in by hand.
 *
 * Starting, cancelling and finishing (confirmed on one account, research #57, levels 3 to 5;
 * every payload below is what follows the 4-byte packet header, request payloads as they
 * are before encryption):
 *
 *   START   request  1433 _MSG_REQUEST_SMARTUSE_FOR_RESEARCH
 *             u32 seq, u16 tech id, u8 TARGET level (current level + 1), u16 n,
 *             n x { u16 item id, u16 quantity }   resource items to use to cover the cost
 *           response 3203 _MSG_RESP_RESEARCH_EVENT_START, 44 bytes
 *             u8 status (0 = ok), u16 tech, u8 target level, i64 start time,
 *             u32 duration in seconds (0 when the game finished it at once for free),
 *             u32 unknown, 6 x u32 that look like the resource stocks afterwards
 *           response 1431 _MSG_RESP_SMARTUSE_FOR_WORK: what is left in the bag of each item used
 *   CANCEL  request  3206 u32 seq, u16 tech, u8 target level, 3 zero bytes
 *           response 3207, 32 bytes: u8 status, u16 tech, u8 level, u32 unknown, 6 x u32 stocks
 *   DONE    push     3208 _MSG_RESP_RESEARCH_EVENT_COMPLETE: u16 tech, u8 level reached
 *   SPEED   request  1427 _MSG_REQUEST_SMARTUSE_SPEEDUP: u32 seq, u16 kind (5 = research),
 *             u16 n, n x { u16 item id, u16 quantity }; or 1406 _MSG_REQUEST_USEITEM one by one
 *
 * Not seen: a plain start with no item to use (the 4 captured starts all used resource items),
 * the "free" (3204) and "instant" (3209) requests, and the meaning of the unknown fields.
 */

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#define RESEARCH_ID_MAX        500
#define RESEARCH_LEVEL_BYTES   (RESEARCH_ID_MAX / 2)
#define RESEARCH_HEADER_SIZE   15
#define RESEARCH_INFO_SIZE     (RESEARCH_HEADER_SIZE + RESEARCH_LEVEL_BYTES)

/* One resource item to use when starting a research (a pack of food, stone... from the bag). */
typedef struct {
    uint16_t item_id;
    uint16_t quantity;
} ResearchItemUse;

#define RESEARCH_START_MAX_ITEMS 16

typedef struct {
    bool     loaded;                       // _MSG_RESP_RESEARCHINFO received since the connection was made
    uint16_t in_progress;                  // tech id being researched, 0 = none
    uint8_t  unk;                          // header byte 2, meaning unknown
    int64_t  start_time;                   // server clock, seconds (in the captures: 74h before "now" for a 93h research)
    uint32_t total_time;                   // total duration of the research in progress, seconds
    uint8_t  levels[RESEARCH_LEVEL_BYTES]; // two 4-bit levels per byte, see the layout above
} ResearchState;

/* Current level of a research, 0 for an id outside 1..RESEARCH_ID_MAX. */
static inline uint8_t ResearchLevel(const ResearchState *r, uint16_t id)
{
    if (id == 0 || id > RESEARCH_ID_MAX)
        return 0;

    uint8_t b = r->levels[(id - 1) >> 1];
    return (id & 1) ? (b & 0x0F) : ((b >> 4) & 0x0F);
}

/* Sets a research's level (0..15), for when the server reports one reached. */
static inline void ResearchSetLevel(ResearchState *r, uint16_t id, uint8_t level)
{
    if (id == 0 || id > RESEARCH_ID_MAX)
        return;

    uint8_t *b = &r->levels[(id - 1) >> 1];
    level &= 0x0F;
    *b = (id & 1) ? (uint8_t)((*b & 0xF0) | level) : (uint8_t)((*b & 0x0F) | (level << 4));
}

static inline bool ResearchInProgress(const ResearchState *r)
{
    return r->loaded && r->in_progress != 0;
}

/* Server-clock second the research in progress ends at (0 when none). */
static inline int64_t ResearchEndTime(const ResearchState *r)
{
    return ResearchInProgress(r) ? r->start_time + (int64_t)r->total_time : 0;
}

/* Seconds left on the research in progress at server time `now`, 0 when none or already over. */
static inline int64_t ResearchSecondsLeft(const ResearchState *r, int64_t now)
{
    int64_t end = ResearchEndTime(r);
    return end > now ? end - now : 0;
}

/* How many researches have a level above 0. */
static inline uint16_t ResearchStartedCount(const ResearchState *r)
{
    uint16_t n = 0;
    for (uint16_t id = 1; id <= RESEARCH_ID_MAX; id++)
        if (ResearchLevel(r, id) > 0)
            n++;
    return n;
}

/* Supply (resource aid) tax. The game never sends the rate: the client computes it, and the delivery report
 * only carries the net amount. It is what the Trading Post's level sets (8% from level 25, buildings.h) minus
 * 0.1% per level of the "Tax Break" research (Army Leadership, effect "Supply Tax Reduction", up to 1% at
 * level 10 on the wiki): the two accounts read 7.5% and 7.6%, Trading Post 30 and 25 (both 8%), Tax Break
 * at 5 and 4.
 *
 * Tax Break is research #126: INFERRED. It is the only research one level higher on the 7.5% account
 * besides #123 and #125, which are already identified, and it sits next to Bigger Bags I in the tree. The
 * 8% base is now confirmed by the wiki's Trading Post page; the number 126 is not confirmed with a third
 * account. The delivery report overrides it (RecvResHelpReport) and a disagreement is logged. */
#define RESEARCH_TAX_BREAK_ID          126
#define RESEARCH_TAX_PER_LEVEL_PERCENT 0.1

/* The tax rate implied by the levels the server sent at login, in percent, from the rate the Trading Post
 * sets. Only meaningful once the research packet was received (r->loaded). */
static inline double ResearchDeliveryTaxPercent(const ResearchState *r, double trading_post_percent)
{
    double tax = trading_post_percent - RESEARCH_TAX_PER_LEVEL_PERCENT * ResearchLevel(r, RESEARCH_TAX_BREAK_ID);
    return tax > 0.0 ? tax : 0.0;
}

/* Names known so far (from the game's UI, English). Every other id is unknown: NULL. */
static inline const char *ResearchKnownName(uint16_t id)
{
    switch (id) {
        case 6:   return "Economy: Construction Speed";
        case 8:   return "Economy: Gem Harvesting I";
        case 54:  return "Military: Fire Trebuchet";
        case 74:  return "Monster hunt: Energy Recovery I";
        case 123: return "Army Leadership: More gatherers";
        case 125: return "Army Leadership: Bigger Bags I";
        case 143: return "Army Leadership: Gold Storage I";
        case 228: return "Wonder Battles: Wonder March I";
        case 229: return "Wonder Battles: Gem Harvesting II";
        case 234: return "Wonder Battles: Bigger Bags II";
        case 299: return "Gear: Barrack Expansion II";
        case 301: return "Gear: Ration Run IV";
        case 302: return "Gear: Forced March III";
        case 303: return "Gear: Bigger Bags III";
        case 305: return "Gear: Quick Maneuvers III";
        default:  return NULL;
    }
}

#endif
