/* The connection the engine's reader uses. holds_above lives in Laplace-Engine/src/read.c;
 * the service links that file and these symbols, and does not fork the laplace binary. */
#include "db_bridge.h"
#include "lpm_config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static char noted_conn[8192];
static PGconn *read_master;

const char *laplace_db(void) {
    const char *v = getenv("LAPLACE_CONNINFO");
    return v && *v ? v : LPM_CONNINFO_DEFAULT;
}
const char *db_noted(void) { return noted_conn; }

void lpm_read_scope(PGconn *pg,const char *connection){
    read_master=pg;snprintf(noted_conn,sizeof noted_conn,"%s",connection);
}
void lpm_read_end(void){
    if(read_master && !db_read_end(read_master))PQreset(read_master);
    read_master=NULL;
}

void *xrealloc(void *p, size_t n) {
    p = realloc(p, n ? n : 1);
    if (!p) { perror("realloc"); exit(1); }
    return p;
}

/* The Engine's shared reader uses these runtime allocation/time adapters. */
double now(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec * 1e-9;
}

PGconn *db_connect(const char *conninfo) {
    if (!conninfo) conninfo = "";
    if (conninfo != noted_conn) snprintf(noted_conn, sizeof noted_conn, "%s", conninfo);
    PGconn *pg = PQconnectdb(conninfo);
    if (PQstatus(pg) != CONNECTION_OK) {
        fprintf(stderr, "Database connection failed: %s", PQerrorMessage(pg));
        exit(1);
    }
    db_notices(pg);
    return pg;
}
