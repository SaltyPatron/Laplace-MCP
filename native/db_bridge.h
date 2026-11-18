#ifndef LAPLACE_MCP_DB_BRIDGE_H
#define LAPLACE_MCP_DB_BRIDGE_H
/* The service's read scope (db_bridge.c), over the Engine's reader (reader.h, walk.h: Laplace::engine_reader), whose
 * connections (laplace_db, db_connect, db_try) the service uses as they are. */
#include "walk.h"
void lpm_read_scope(PGconn *,const char *connection);
void lpm_read_end(void);
#endif
