/* The service's read scope over the engine's reader (reader.h: Laplace::engine_reader). Connections themselves are the
 * reader's (laplace_db, db_connect, db_try in its pg.c); the service only names the connection a request reads on. */
#include "db_bridge.h"

static PGconn *read_master;

void lpm_read_scope(PGconn *pg,const char *connection){
    read_master=pg;db_note(connection);
}
void lpm_read_end(void){
    if(read_master && !db_read_end(read_master))PQreset(read_master);
    read_master=NULL;
}
