#ifndef COMMAND_PARSER
#define COMMAND_PARSER

#include <stdint.h>
#include "connection.h"

/* Handles a message that may be a bot command. `source` is where it came from (world chat, alliance chat or mail). */
void command_handler(Connection *c, const char *player_name, const char *message, CommandChannel source);

/* Administrators (admin.names in the config file, plus the ones added in game). */
bool IsAdmin(const Connection *c, const char *name);
bool AdminAdd(Connection *c, const char *name);
bool AdminRemove(Connection *c, const char *name);
bool AdminSaveRuntime(const Connection *c);
void MakeDirectories(const char *path);
void AdminLoadRuntime(Connection *c);

#endif
