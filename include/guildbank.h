#ifndef GUILDBANK_H
#define GUILDBANK_H

/*
 * Guild bank: a vault per player. A member sends resources to the bot (a deposit), the bot writes down what it
 * really received in that player's balance, and the player takes it back later with the resource commands
 * ($food 1M...). Nothing is shared: a player can only take what they put in.
 *
 *   - A deposit is read from the delivery report the game sends the receiver (_MSG_RESP_RESHELPREPORTINFO, flag
 *     "received", docs/research.md and RecvResHelpReport): the sender's name and the NET amount, after the
 *     sender's own tax. That net amount is what is credited (1M sent, 950k arrived: 950k).
 *   - A withdrawal debits the GROSS amount that leaves the bot, march by march, and the player receives it minus
 *     the bot's own tax. The player pays that tax, once: not a round trip at one rate, the two taxes are the
 *     sender's on the way in and the bot's on the way out.
 *   - The balances live in <data.path>/guild_bank.txt, written after every change, so a restart or a reconnection
 *     never costs a player their deposit. They are kept here, not in Connection, which is wiped at every
 *     reconnection.
 *   - Every report has a number and is counted once, whatever the number of logins that deliver it again; only
 *     reports dated after the day the file was created count, so old deliveries are not credited by surprise.
 *
 * The web console (webui/) shows the balances and can change them. It reads guild_bank.txt (and, with SQL Server,
 * keeps a copy of it in the database, from which it gives the file back before the bot starts). To change a balance
 * while the bot runs it does not touch that file, which the bot owns: it drops requests in guild_bank_edits.txt,
 * which the bot applies within a second (GuildBankTick), one per line:
 *
 *     set<TAB>name<TAB>resource<TAB>amount     the balance becomes exactly that (0..4 = food, stone, wood, ore, gold)
 *     reset                                    every balance back to 0; only deliveries after that moment count again
 *
 * Off by default (guildbank.enabled). Amounts are in game units, one balance per resource in ResourceType order.
 */

#include "connection.h"

#define GUILDBANK_MAX_ACCOUNTS  512
#define GUILDBANK_MAX_SEEN      1024
#define GUILDBANK_RESOURCE_COUNT 5

/* Forget everything held in memory (tests, or before loading another folder). The file is left alone. */
void GuildBankReset(void);

/* Loads <data.path>/guild_bank.txt once per folder; a missing file is created, with today as the day counting starts. */
bool GuildBankLoad(Connection *c);

uint64_t GuildBankBalance(const char *name, ResourceType type);

/* Sum of every balance of a resource: what the bot must keep to be able to give it all back. */
uint64_t GuildBankTotal(ResourceType type);

uint32_t GuildBankAccountCount(void);

bool GuildBankCredit(Connection *c, const char *name, ResourceType type, uint64_t amount);

/* false, and nothing changed, when the balance is smaller than the amount. */
bool GuildBankDebit(Connection *c, const char *name, ResourceType type, uint64_t amount);

/* A delivery this bot received: credits the sender with the net amounts, once per report. Returns true when
 * something was credited (false for a report already counted, older than the bank, or empty). */
bool GuildBankDeposit(Connection *c, uint32_t report_id, uint32_t report_time, const char *sender,
                      const uint32_t net[GUILDBANK_RESOURCE_COUNT]);


/* The balance becomes exactly `amount` (the player is created if needed, 0 keeps an empty account out of the file). */
bool GuildBankSet(Connection *c, const char *name, ResourceType type, uint64_t amount);

/* Every balance back to 0. Deliveries dated before now stay ignored, whatever their number. */
bool GuildBankResetAll(Connection *c);

/* Applies the requests the console left in guild_bank_edits.txt (all of them, right now). Returns how many. */
uint32_t GuildBankApplyEdits(Connection *c);

/* Called by the main loop: looks for console requests once a second. */
void GuildBankTick(Connection *c);

#endif
