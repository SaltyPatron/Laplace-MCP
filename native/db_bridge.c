/* The connection the engine's reader uses. holds_above lives in Laplace-Engine/src/read.c;
 * the service links that file and these symbols, and does not fork the laplace binary. */
#include "engine.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

uint32_t id_oid;
static char noted_conn[8192];

const char *laplace_db(void) {
    const char *v = getenv("LAPLACE_CONNINFO");
    return v && *v ? v : "host=/tmp port=5432 user=laplace dbname=laplace";
}
const char *db_noted(void) { return noted_conn; }

void *xrealloc(void *p, size_t n) {
    p = realloc(p, n ? n : 1);
    if (!p) { perror("realloc"); exit(1); }
    return p;
}

PGconn *db_connect(const char *conninfo) {
    if (!conninfo) conninfo = "";
    if (conninfo != noted_conn) snprintf(noted_conn, sizeof noted_conn, "%s", conninfo);
    PGconn *pg = PQconnectdb(conninfo);
    if (PQstatus(pg) != CONNECTION_OK) {
        fprintf(stderr, "%s (LAPLACE_CONNINFO: %s)\n", PQerrorMessage(pg), conninfo);
        exit(1);
    }
    PQclear(PQexec(pg, "SET client_min_messages = warning"));
    PQclear(PQexec(pg, "SET enable_parallel_append = on"));
    if (!id_oid) {
        PGresult *r = PQexec(pg, "SELECT 'blake3'::regtype::oid");
        if (PQresultStatus(r) == PGRES_TUPLES_OK && PQntuples(r))
            id_oid = (uint32_t)strtoul(PQgetvalue(r, 0, 0), NULL, 10);
        PQclear(r);
    }
    return pg;
}

PGresult *db_ask(PGconn *pg, const char *sql, int n, const char *const *v, const int *l, const int *f) {
    static struct { PGconn *pg; const char *sql; } known[64];
    static int nknown;
    int k = 0;
    #pragma omp critical(db_ask)
    {
        while (k < nknown && !(known[k].pg == pg && known[k].sql == sql)) k++;
        if (k == nknown && nknown < 64) {
            char name[16];
            snprintf(name, sizeof name, "q%d", k);
            if (!nknown || known[nknown - 1].pg != pg) {
                int had = 0;
                for (int i = 0; i < nknown; i++) had |= known[i].pg == pg;
                if (!had) PQclear(PQexec(pg, "SET plan_cache_mode = force_generic_plan"));
            }
            PGresult *r = PQprepare(pg, name, sql, n, NULL);
            if (PQresultStatus(r) != PGRES_COMMAND_OK) {
                fprintf(stderr, "prepare: %s", PQerrorMessage(pg));
                exit(1);
            }
            PQclear(r);
            known[nknown].pg = pg;
            known[nknown++].sql = sql;
        }
    }
    if (k >= 64) return PQexecParams(pg, sql, n, NULL, v, l, f, 1);
    char name[16];
    snprintf(name, sizeof name, "q%d", k);
    return PQexecPrepared(pg, name, n, v, l, f, 1);
}
