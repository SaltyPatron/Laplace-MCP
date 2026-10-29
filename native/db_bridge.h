#ifndef LAPLACE_MCP_DB_BRIDGE_H
#define LAPLACE_MCP_DB_BRIDGE_H
#include "engine.h"
void lpm_read_scope(PGconn *,const char *connection);
void lpm_read_end(void);
#endif
