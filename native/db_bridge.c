/* The connection the engine's reader uses. holds_above lives in Laplace-Engine/src/read.c;
 * the service links that file and these symbols, and does not fork the laplace binary. */
#include "engine.h"
#include "lpm_config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static char noted_conn[8192];

const char *laplace_db(void) {
    const char *v = getenv("LAPLACE_CONNINFO");
    return v && *v ? v : LPM_CONNINFO_DEFAULT;
}
const char *db_noted(void) { return noted_conn; }

/* The engine's commands read under one exported snapshot (its main.c: db_read), which its pool takes when it opens.
 * This process outlives any one read and its pool opens once, so it names none: each statement sees the database as it
 * is when that statement begins, as this service has read since it was written. One world for a whole request needs
 * the engine's pool to take a snapshot per read; it has no call for that yet. */
const char *db_snapshot(void) { return NULL; }

void *xrealloc(void *p, size_t n) {
    p = realloc(p, n ? n : 1);
    if (!p) { perror("realloc"); exit(1); }
    return p;
}

/* read.c times its pool with the engine's now(). The engine defines that in table.c, which this
 * service does not link; xrealloc above is the same kind of symbol. */
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
