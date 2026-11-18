#ifndef LAPLACE_MCP_DB_BRIDGE_H
#define LAPLACE_MCP_DB_BRIDGE_H
/* The service's connections (db_bridge.c), over the Engine's reader (reader.h, Laplace::engine_reader). */
#include "walk.h"                                   /* reader.h and the witness walk */
const char *laplace_db(void);                        /* LAPLACE_CONNINFO, else the compiled default */
const char *db_noted(void);                          /* the conninfo of the last db_connect */
PGconn *db_connect(const char *conninfo);            /* exits with the server's message if it cannot */
void lpm_read_scope(PGconn *,const char *connection);
void lpm_read_end(void);
#endif
