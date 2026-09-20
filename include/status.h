#ifndef _STATUS_H_
#define _STATUS_H_

#include "connection.h"

/* Writes <data.path>/status.json every few seconds for the web console. */
void StatusTick(Connection *c);

/* Writes it now; connected = false marks the account as offline. */
void StatusWrite(Connection *c, bool connected);

#endif
