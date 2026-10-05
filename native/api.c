#define _GNU_SOURCE
#include "api.h"

#include "laplace/laplace.h"

#include <arpa/inet.h>
#include <libpq-fe.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

const lp_tier0_record *T0;
static lp_text *TX;
static PGconn *PG;

static const char *CONN_DEFAULT = "host=/tmp port=5432 user=laplace dbname=laplace";
static const char *ENGINE = "/repos/build/Laplace-Engine/icx-release/laplace";

static uint64_t nsec(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static void hex_id(const lp_id *id, char out[33]) {
    static const char H[] = "0123456789abcdef";
    for (int i = 0; i < 16; i++) {
        out[i * 2] = H[id->b[i] >> 4];
        out[i * 2 + 1] = H[id->b[i] & 15];
    }
    out[32] = 0;
}

static int valid_id(const char *s) {
    if (!s || strlen(s) != 32) return 0;
    for (int i = 0; i < 32; i++) {
        char c = s[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) return 0;
    }
    return 1;
}

static PGconn *db(void) {
    if (PG && PQstatus(PG) == CONNECTION_OK) return PG;
    if (PG) PQfinish(PG);
    const char *ci = getenv("LAPLACE_CONNINFO");
    if (!ci || !*ci) ci = CONN_DEFAULT;
    PG = PQconnectdb(ci);
    if (PQstatus(PG) != CONNECTION_OK) return NULL;
    PQexec(PG, "SET client_min_messages = warning");
    /* Parent scans (containers, claims, forward) run as a Gather over the leaves. A named leaf stays one process. */
    PQexec(PG, "SET enable_parallel_append = on");
    return PG;
}

static int capture;
static int capture_code;
static char *capture_body;

static void http(int c, int code, const char *body) {
    if (capture) {
        capture_code = code;
        free(capture_body);
        capture_body = strdup(body ? body : "");
        return;
    }
    const char *why = code == 200 ? "OK" : code == 202 ? "Accepted" : code == 400 ? "Bad Request" : code == 404 ? "Not Found" : "Error";
    char hdr[256];
    int n = snprintf(hdr, sizeof hdr,
                     "HTTP/1.1 %d %s\r\nContent-Type: application/json\r\nContent-Length: %zu\r\nConnection: close\r\n\r\n",
                     code, why, strlen(body));
    send(c, hdr, (size_t)n, MSG_NOSIGNAL);
    send(c, body, strlen(body), MSG_NOSIGNAL);
}

static char *json_esc(const char *s, char *out, size_t cap) {
    size_t n = 0;
    if (!s) s = "";
    for (const unsigned char *p = (const unsigned char *)s; *p && n + 6 < cap; p++) {
        if (*p == '"' || *p == '\\') {
            out[n++] = '\\';
            out[n++] = (char)*p;
        } else if (*p == '\n') {
            out[n++] = '\\';
            out[n++] = 'n';
        } else if (*p < 0x20) {
            n += (size_t)snprintf(out + n, cap - n, "\\u%04x", *p);
        } else out[n++] = (char)*p;
    }
    out[n] = 0;
    return out;
}

/* Value of a JSON string field. Handles a single level of backslash escapes. */
static int jstr(const char *body, const char *key, char *out, size_t cap) {
    char pat[80];
    snprintf(pat, sizeof pat, "\"%s\"", key);
    const char *p = strstr(body, pat);
    if (!p) return 0;
    p += strlen(pat);
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
    if (*p != ':') return 0;
    p++;
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
    if (*p != '"') return 0;
    p++;
    size_t n = 0;
    while (*p && *p != '"' && n + 1 < cap) {
        if (*p == '\\' && p[1]) {
            p++;
            if (*p == 'n') out[n++] = '\n';
            else out[n++] = *p;
            p++;
        } else out[n++] = *p++;
    }
    out[n] = 0;
    return 1;
}

static int name_entity(const char *text, lp_ref *trunk, lp_ref *parts, size_t cap, size_t *nparts) {
    if (!TX) return 0;
    *trunk = lp_text_parts(TX, (const uint8_t *)text, strlen(text), parts, cap, nparts);
    return 1;
}

static void parts_json(const lp_ref *parts, size_t n, char *out, size_t cap) {
    size_t u = 0;
    out[0] = 0;
    for (size_t i = 0; i < n && u + 40 < cap; i++) {
        char id[33];
        hex_id(&parts[i].id, id);
        u += (size_t)snprintf(out + u, cap - u, "%s\"%s\"", i ? "," : "", id);
    }
}

static int q1(PGconn *pg, const char *sql, const char *a, char **out, char *err, size_t errcap) {
    const char *v[1] = { a };
    PGresult *r = PQexecParams(pg, sql, 1, NULL, v, NULL, NULL, 0);
    if (PQresultStatus(r) != PGRES_TUPLES_OK || PQntuples(r) < 1) {
        snprintf(err, errcap, "%s", PQerrorMessage(pg));
        PQclear(r);
        PQclear(PQexec(pg, "ROLLBACK"));
        return 0;
    }
    *out = strdup(PQgetvalue(r, 0, 0));
    PQclear(r);
    return *out != NULL;
}

static int q2(PGconn *pg, const char *sql, const char *a, const char *b, char **out, char *err, size_t errcap) {
    const char *v[2] = { a, b };
    PGresult *r = PQexecParams(pg, sql, 2, NULL, v, NULL, NULL, 0);
    if (PQresultStatus(r) != PGRES_TUPLES_OK || PQntuples(r) < 1) {
        snprintf(err, errcap, "%s", PQerrorMessage(pg));
        PQclear(r);
        PQclear(PQexec(pg, "ROLLBACK"));
        return 0;
    }
    *out = strdup(PQgetvalue(r, 0, 0));
    PQclear(r);
    return *out != NULL;
}

static int jint(const char *body, const char *key, int *out) {
    char pat[80];
    snprintf(pat, sizeof pat, "\"%s\"", key);
    const char *p = strstr(body, pat);
    if (!p) return 0;
    p += strlen(pat);
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
    if (*p != ':') return 0;
    p++;
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
    if (*p == '"') return 0;
    char *end = NULL;
    long v = strtol(p, &end, 10);
    if (end == p) return 0;
    *out = (int)v;
    return 1;
}

static int mask_claim(const char *m, int len) {
    if (len < 5) return 0;
    return ((const unsigned char *)m)[4] & 0x80;
}

static const char *FORWARD_SQL =
    "SELECT COALESCE((SELECT json_agg(x) FROM ("
    "  SELECT json_build_object('i', i, 'j', j, 'paths', paths, 'runs', runs,"
    "    'next', next::text, 'times', times) AS x"
    "  FROM laplace_forward($1::blake3[], 64) LIMIT 128) s), '[]'::json)::text";

static const char *SHAPE_SQL =
    "SELECT laplace_frechet4d("
    " (SELECT ST_MakeLine(e.coord ORDER BY u.ord) FROM unnest(string_to_array($1, ',')::blake3[])"
    "    WITH ORDINALITY AS u(id, ord) JOIN entity e ON e.id = u.id),"
    " (SELECT ST_MakeLine(e.coord ORDER BY u.ord) FROM unnest(string_to_array($2, ',')::blake3[])"
    "    WITH ORDINALITY AS u(id, ord) JOIN entity e ON e.id = u.id)"
    ")::text";

static int part_list(const lp_ref *parts, size_t n, char *out, size_t cap) {
    size_t u = 0;
    if (!n) return 0;
    for (size_t i = 0; i < n; i++) {
        char id[33];
        hex_id(&parts[i].id, id);
        int w = snprintf(out + u, cap - u, "%s%s", i ? "," : "", id);
        if (w < 0 || u + (size_t)w >= cap) return 0;
        u += (size_t)w;
    }
    return 1;
}

static void coord_json(const lp_ref *r, char *out, size_t cap) {
    snprintf(out, cap, "[%.17g,%.17g,%.17g,%.17g]",
             (double)r->c.m[0] / LP_FIXED_ONE, (double)r->c.m[1] / LP_FIXED_ONE,
             (double)r->c.m[2] / LP_FIXED_ONE, (double)r->c.m[3] / LP_FIXED_ONE);
}

static void on_embeddings(int c, const char *body) {
    char text[8192];
    if (!jstr(body, "input", text, sizeof text) || !text[0]) {
        http(c, 400, "{\"error\":\"input\"}");
        return;
    }
    lp_ref trunk, parts[64];
    size_t np = 0;
    uint64_t t0 = nsec();
    if (!name_entity(text, &trunk, parts, 64, &np)) {
        http(c, 500, "{\"error\":\"tier0\"}");
        return;
    }
    double ms = (double)(nsec() - t0) / 1e6;
    char id[33], pj[64 * 40], cj[160];
    hex_id(&trunk.id, id);
    parts_json(parts, np, pj, sizeof pj);
    coord_json(&trunk, cj, sizeof cj);
    char out[8192];
    snprintf(out, sizeof out,
             "{\"object\":\"list\",\"model\":\"laplace\",\"data\":[{\"object\":\"record\",\"index\":0,"
             "\"id\":\"%s\",\"tier\":%u,\"coord\":%s,\"hilbert\":\"%016llx\",\"parts\":[%s]}],"
             "\"ms\":{\"identity\":%.3f}}",
             id, trunk.tier, cj, (unsigned long long)lp_hilbert4(&trunk.c), pj, ms);
    http(c, 200, out);
}

/* holds_above, tier_max, ids_param: Laplace-Engine/src/read.c, linked into this process. */
typedef struct {
    lp_id entity; int src; int16_t tier; uint8_t claim, stood;
    uint8_t *path; int path_len; lp_rating r; int matches;
} Hold;
Hold *holds_above(const lp_id *keys, int nkeys, int floor, int each, int standing, int *nout);
void holds_free(Hold *h, int n);
int tier_max(const lp_id *ids, int n);
size_t ids_param(uint8_t *out, const lp_id *ids, uint32_t n);

static void buf_add(char **b, size_t *n, size_t *cap, const char *s, size_t m) {
    if (*n + m + 1 > *cap) {
        size_t c = *cap ? *cap : 4096;
        while (*n + m + 1 > c) c *= 2;
        char *p = realloc(*b, c);
        if (!p) return;
        *b = p;
        *cap = c;
    }
    memcpy(*b + *n, s, m);
    *n += m;
    (*b)[*n] = 0;
}

static void buf_fmt(char **b, size_t *n, size_t *cap, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    char tmp[1200];
    int w = vsnprintf(tmp, sizeof tmp, fmt, ap);
    va_end(ap);
    if (w < 0) return;
    if ((size_t)w < sizeof tmp) { buf_add(b, n, cap, tmp, (size_t)w); return; }
    char *p = malloc((size_t)w + 1);
    if (!p) return;
    va_start(ap, fmt);
    vsnprintf(p, (size_t)w + 1, fmt, ap);
    va_end(ap);
    buf_add(b, n, cap, p, (size_t)w);
    free(p);
}

/* One constituent of a mixed-tier path. Tier 0 is a codepoint, tier 1 a grapheme.
 * A higher tier is read the same way as the trunk: the paths that hold it, and their attestations.
 * Its own constituents stay on it, so a word's codepoints are not dropped and are not searched as the trunk. */
typedef struct {
    lp_id id;
    int tier;
    int cp;
    int have_c;
    lp_coord c;
    Hold *h;
    int nh;
    PGresult *att;
    lp_id *kids;
    int *ktier;
    int *kcp;
    int nkids;
} Down;

static void downs_free(Down *d, int n) {
    if (!d) return;
    for (int i = 0; i < n; i++) {
        holds_free(d[i].h, d[i].nh);
        if (d[i].att) PQclear(d[i].att);
        free(d[i].kids);
        free(d[i].ktier);
        free(d[i].kcp);
    }
    free(d);
}

static void phy_leaf_name(char *out, size_t cap, int tier, const lp_id *id) {
    int big = tier == 0 || tier == 2 || tier == 3 || tier == 4 || tier == 5 || tier == 6;
    if (tier >= 16) snprintf(out, cap, "physicality_tx");
    else if (tier >= 0 && big) snprintf(out, cap, "physicality_t%d_%x", tier, id->b[0] >> 4);
    else if (tier >= 0) snprintf(out, cap, "physicality_t%d", tier);
    else snprintf(out, cap, "physicality");
}

static void emit_ids(char **b, size_t *n, size_t *cap, const uint8_t *path, int plen, int *nids) {
    size_t m = path && plen > 0 ? lp_path_ids(path, (size_t)plen, NULL, 0) : 0;
    *nids = (int)m;
    lp_id *ids = malloc(sizeof(lp_id) * (m ? m : 1));
    if (m) lp_path_ids(path, (size_t)plen, ids, m);
    buf_add(b, n, cap, "[", 1);
    for (size_t i = 0; i < m; i++) {
        char hex[33];
        hex_id(&ids[i], hex);
        buf_fmt(b, n, cap, "%s\"%s\"", i ? "," : "", hex);
    }
    buf_add(b, n, cap, "]", 1);
    free(ids);
}

static void emit_att_rows(char **b, size_t *n, size_t *cap, PGresult *att) {
    int rows = att ? PQntuples(att) : 0;
    for (int i = 0; i < rows; i++) {
        char claim[33], wit[33];
        lp_id id;
        memcpy(id.b, PQgetvalue(att, i, 0), 16);
        hex_id(&id, claim);
        memcpy(id.b, PQgetvalue(att, i, 1), 16);
        hex_id(&id, wit);
        int pos_null = PQgetisnull(att, i, 2);
        int pos = pos_null ? 0 : (int)lp_be(PQgetvalue(att, i, 2), 4);
        uint32_t sb = (uint32_t)lp_be(PQgetvalue(att, i, 3), 4);
        float score;
        memcpy(&score, &sb, 4);
        int games = (int)lp_be(PQgetvalue(att, i, 4), 4);
        double trust = lp_be_f64(PQgetvalue(att, i, 5));
        if (pos_null) buf_fmt(b, n, cap,
            "%s{\"claim\":\"%s\",\"witness\":\"%s\",\"position\":null,\"score\":%.6g,\"games\":%d,\"trust\":%.6g}",
            i ? "," : "", claim, wit, score, games, trust);
        else buf_fmt(b, n, cap,
            "%s{\"claim\":\"%s\",\"witness\":\"%s\",\"position\":%d,\"score\":%.6g,\"games\":%d,\"trust\":%.6g}",
            i ? "," : "", claim, wit, pos, score, games, trust);
    }
}

/* One constituent. Tier 0 carries its codepoint. A tier at or above 2 carries the paths that hold it. */
static void emit_down(char **b, size_t *n, size_t *cap, const Down *d, int fan) {
    char eid[33], cj[160];
    hex_id(&d->id, eid);
    buf_fmt(b, n, cap, "{\"id\":\"%s\",\"tier\":%d", eid, d->tier);
    if (d->cp >= 0) buf_fmt(b, n, cap, ",\"codepoint\":%d", d->cp);
    if (d->have_c) {
        lp_ref r;
        memset(&r, 0, sizeof r);
        r.c = d->c;
        coord_json(&r, cj, sizeof cj);
        buf_fmt(b, n, cap, ",\"coord\":%s", cj);
    }
    if (d->nkids > 0) {
        buf_add(b, n, cap, ",\"constituents\":[", strlen(",\"constituents\":["));
        for (int k = 0; k < d->nkids; k++) {
            char kid[33];
            hex_id(&d->kids[k], kid);
            buf_fmt(b, n, cap, "%s{\"id\":\"%s\",\"tier\":%d", k ? "," : "", kid, d->ktier[k]);
            if (d->kcp[k] >= 0) buf_fmt(b, n, cap, ",\"codepoint\":%d", d->kcp[k]);
            buf_add(b, n, cap, "}", 1);
        }
        buf_add(b, n, cap, "]", 1);
    }
    if (d->tier >= 2) {
        int nh = d->nh;
        Hold *h = d->h;
        int *order = malloc(sizeof(int) * (size_t)(nh ? nh : 1));
        double *conf = calloc((size_t)(nh ? nh : 1), sizeof(double));
        int nco = 0, noo = 0;
        for (int i = 0; i < nh; i++) {
            if (h[i].claim && h[i].stood) conf[i] = lp_confidence(&h[i].r, 2.0);
            if (h[i].claim) order[nco++] = i;
            else noo++;
        }
        for (int a = 1; a < nco; a++) {
            int k = order[a], p = a;
            while (p > 0 && conf[order[p - 1]] < conf[k]) { order[p] = order[p - 1]; p--; }
            order[p] = k;
        }
        int ncret = nco, noret = noo;
        if (fan > 0) {
            if (ncret > fan) ncret = fan;
            if (noret > fan) noret = fan;
        }
        buf_fmt(b, n, cap, ",\"holds_read\":%d,\"claims_read\":%d,\"occurrences_read\":%d,\"claims\":[", nh, nco, noo);
        for (int i = 0; i < ncret; i++) {
            Hold *s = &h[order[i]];
            char id[33];
            int nids = 0;
            hex_id(&s->entity, id);
            buf_fmt(b, n, cap,
                    "%s{\"entity\":\"%s\",\"tier\":%d,\"stood\":%s,\"confidence\":%.6f,"
                    "\"rating\":%.3f,\"deviation\":%.3f,\"volatility\":%.6f,\"matches\":%d,\"constituents\":",
                    i ? "," : "", id, (int)s->tier, s->stood ? "true" : "false", conf[order[i]],
                    s->r.rating, s->r.deviation, s->r.volatility, s->matches);
            emit_ids(b, n, cap, s->path, s->path_len, &nids);
            buf_fmt(b, n, cap, ",\"n\":%d}", nids);
        }
        buf_add(b, n, cap, "],\"occurrences\":[", strlen("],\"occurrences\":["));
        for (int i = 0, seen = 0; i < nh && seen < noret; i++) {
            if (h[i].claim) continue;
            char id[33];
            int nids = 0;
            hex_id(&h[i].entity, id);
            buf_fmt(b, n, cap, "%s{\"entity\":\"%s\",\"tier\":%d,\"constituents\":", seen ? "," : "", id, (int)h[i].tier);
            emit_ids(b, n, cap, h[i].path, h[i].path_len, &nids);
            buf_fmt(b, n, cap, ",\"n\":%d}", nids);
            seen++;
        }
        buf_add(b, n, cap, "],\"attestations\":[", strlen("],\"attestations\":["));
        emit_att_rows(b, n, cap, d->att);
        buf_add(b, n, cap, "]", 1);
        free(order);
        free(conf);
    }
    buf_add(b, n, cap, "}", 1);
}

static void on_search(int c, const char *body) {
    char text[8192], idbuf[64];
    lp_ref trunk, parts[64];
    size_t np = 0;
    uint64_t t0 = nsec();
    if (jstr(body, "input", text, sizeof text) && text[0]) {
        if (!name_entity(text, &trunk, parts, 64, &np)) {
            http(c, 500, "{\"error\":\"tier0\"}");
            return;
        }
        hex_id(&trunk.id, idbuf);
    } else if (jstr(body, "id", idbuf, sizeof idbuf) && valid_id(idbuf)) {
        memset(&trunk, 0, sizeof trunk);
        np = 0;
    } else {
        http(c, 400, "{\"error\":\"input or id\"}");
        return;
    }
    double id_ms = (double)(nsec() - t0) / 1e6;
    int fan = 0;
    jint(body, "fan", &fan);
    int known = np > 0;
    if (!np) {
        for (int i = 0; i < 16; i++) {
            unsigned v = 0;
            sscanf(idbuf + 2 * i, "%2x", &v);
            trunk.id.b[i] = (uint8_t)v;
        }
        int recorded = tier_max(&trunk.id, 1);
        if (recorded >= 0) { trunk.tier = (uint32_t)recorded; known = 1; }
    }
    t0 = nsec();
    int nh = 0;
    int floor = known ? (int)trunk.tier : -1;
    Hold *h = holds_above(&trunk.id, 1, floor, 0, 1, &nh);
    PGconn *pg = db();
    if (!pg) {
        holds_free(h, nh);
        char e[512], b[640];
        json_esc(PQerrorMessage(PG), e, sizeof e);
        snprintf(b, sizeof b, "{\"error\":\"database\",\"detail\":\"%s\"}", e);
        http(c, 503, b);
        return;
    }
    uint8_t ab[40];
    size_t al = ids_param(ab, &trunk.id, 1);
    const char *pv[1] = { (const char *)ab };
    int pl[1] = { (int)al }, pf[1] = { 1 };
    char leaf[64], psql[192];
    int big = floor == 0 || floor == 2 || floor == 3 || floor == 4 || floor == 5 || floor == 6;
    if (floor >= 16) snprintf(leaf, sizeof leaf, "physicality_tx");
    else if (floor >= 0 && big) snprintf(leaf, sizeof leaf, "physicality_t%d_%x", floor, trunk.id.b[0] >> 4);
    else if (floor >= 0) snprintf(leaf, sizeof leaf, "physicality_t%d", floor);
    else snprintf(leaf, sizeof leaf, "physicality");
    snprintf(psql, sizeof psql, "SELECT path, tier, mask FROM %s WHERE entity = ANY($1::blake3[])", leaf);
    PGresult *self = PQexecParams(pg, psql, 1, NULL, pv, pl, pf, 1);
    if (PQresultStatus(self) != PGRES_TUPLES_OK) {
        char e[512], b[700];
        json_esc(PQerrorMessage(pg), e, sizeof e);
        snprintf(b, sizeof b, "{\"error\":\"physicality\",\"detail\":\"%s\"}", e);
        PQclear(self);
        holds_free(h, nh);
        http(c, 500, b);
        return;
    }
    /* Attestations of this ID and of every claim that holds it. One set, the claim index. */
    int nclaim = 0;
    for (int i = 0; i < nh; i++) nclaim += h[i].claim;
    uint8_t *cb = malloc(20 + 20 * (size_t)(nclaim + 1));
    lp_id *cids = malloc(sizeof(lp_id) * (size_t)(nclaim + 1));
    int ncids = 0;
    cids[ncids++] = trunk.id;
    for (int i = 0; i < nh; i++) if (h[i].claim) cids[ncids++] = h[i].entity;
    size_t cl = ids_param(cb, cids, (uint32_t)ncids);
    const char *cv[1] = { (const char *)cb };
    int cln[1] = { (int)cl }, cf[1] = { 1 };
    PGresult *att = PQexecParams(pg,
        "SELECT a.claim, a.witness, a.position, a.score, a.games, w.trust "
        "FROM attestation a JOIN witness w ON w.id = a.witness WHERE a.claim = ANY($1::blake3[])",
        1, NULL, cv, cln, cf, 1);
    if (PQresultStatus(att) != PGRES_TUPLES_OK) {
        char e[512], b[700];
        json_esc(PQerrorMessage(pg), e, sizeof e);
        snprintf(b, sizeof b, "{\"error\":\"attestation\",\"detail\":\"%s\"}", e);
        PQclear(att); PQclear(self); free(cb); free(cids); holds_free(h, nh);
        http(c, 500, b);
        return;
    }
    /* The trunk's path mixes tiers. Each constituent keeps its own tier. Tier 0 and tier 1
     * stay in the path. A constituent above them is read for what holds it. */
    Down *down = NULL;
    int ndown = 0;
    if (np) {
        ndown = (int)np;
        down = calloc((size_t)ndown, sizeof *down);
        for (int i = 0; i < ndown; i++) {
            down[i].id = parts[i].id;
            down[i].tier = parts[i].tier;
            down[i].cp = (int)lp_tier0_codepoint(T0, &parts[i].id);
            down[i].have_c = 1;
            down[i].c = parts[i].c;
        }
    } else if (PQntuples(self) > 0) {
        const uint8_t *pw = (const uint8_t *)PQgetvalue(self, 0, 0);
        int plen = PQgetlength(self, 0, 0);
        size_t m = lp_path_ids(pw, (size_t)plen, NULL, 0);
        ndown = (int)m;
        down = calloc((size_t)(ndown ? ndown : 1), sizeof *down);
        lp_id *ids = malloc(sizeof(lp_id) * (m ? m : 1));
        if (m) lp_path_ids(pw, (size_t)plen, ids, m);
        for (int i = 0; i < ndown; i++) {
            down[i].id = ids[i];
            down[i].cp = (int)lp_tier0_codepoint(T0, &ids[i]);
            if (down[i].cp >= 0) down[i].tier = 0;
            else {
                int t = tier_max(&ids[i], 1);
                down[i].tier = t < 0 ? -1 : t;
            }
        }
        free(ids);
    }
    for (int i = 0; i < ndown; i++) {
        if (down[i].tier < 2) continue;
        down[i].h = holds_above(&down[i].id, 1, down[i].tier, 0, 1, &down[i].nh);
        int nc = 0;
        for (int k = 0; k < down[i].nh; k++) nc += down[i].h[k].claim;
        lp_id *cids2 = malloc(sizeof(lp_id) * (size_t)(nc + 1));
        int n2 = 0;
        cids2[n2++] = down[i].id;
        for (int k = 0; k < down[i].nh; k++) if (down[i].h[k].claim) cids2[n2++] = down[i].h[k].entity;
        uint8_t *bb = malloc(20 + 20 * (size_t)n2);
        size_t bl = ids_param(bb, cids2, (uint32_t)n2);
        const char *bv[1] = { (const char *)bb };
        int bln[1] = { (int)bl }, bf[1] = { 1 };
        down[i].att = PQexecParams(pg,
            "SELECT a.claim, a.witness, a.position, a.score, a.games, w.trust "
            "FROM attestation a JOIN witness w ON w.id = a.witness WHERE a.claim = ANY($1::blake3[])",
            1, NULL, bv, bln, bf, 1);
        free(bb);
        free(cids2);
        if (PQresultStatus(down[i].att) != PGRES_TUPLES_OK) {
            char e[512], b[700];
            json_esc(PQerrorMessage(pg), e, sizeof e);
            snprintf(b, sizeof b, "{\"error\":\"attestation\",\"detail\":\"%s\"}", e);
            downs_free(down, ndown);
            PQclear(att); PQclear(self); free(cb); free(cids); holds_free(h, nh);
            http(c, 500, b);
            return;
        }
        uint8_t oneb[40];
        size_t onel = ids_param(oneb, &down[i].id, 1);
        const char *ov[1] = { (const char *)oneb };
        int ol[1] = { (int)onel }, of[1] = { 1 };
        char leaf[64], psql2[192];
        phy_leaf_name(leaf, sizeof leaf, down[i].tier, &down[i].id);
        snprintf(psql2, sizeof psql2, "SELECT path FROM %s WHERE entity = ANY($1::blake3[])", leaf);
        PGresult *pr = PQexecParams(pg, psql2, 1, NULL, ov, ol, of, 1);
        if (PQresultStatus(pr) == PGRES_TUPLES_OK && PQntuples(pr) > 0) {
            const uint8_t *kw = (const uint8_t *)PQgetvalue(pr, 0, 0);
            int klen = PQgetlength(pr, 0, 0);
            size_t km = lp_path_ids(kw, (size_t)klen, NULL, 0);
            down[i].nkids = (int)km;
            down[i].kids = malloc(sizeof(lp_id) * (km ? km : 1));
            down[i].ktier = malloc(sizeof(int) * (km ? km : 1));
            down[i].kcp = malloc(sizeof(int) * (km ? km : 1));
            if (km) lp_path_ids(kw, (size_t)klen, down[i].kids, km);
            for (int k = 0; k < down[i].nkids; k++) {
                down[i].kcp[k] = (int)lp_tier0_codepoint(T0, &down[i].kids[k]);
                if (down[i].kcp[k] >= 0) down[i].ktier[k] = 0;
                else {
                    int t = tier_max(&down[i].kids[k], 1);
                    down[i].ktier[k] = t < 0 ? -1 : t;
                }
            }
        }
        PQclear(pr);
    }
    double db_ms = (double)(nsec() - t0) / 1e6;
    int *order = malloc(sizeof(int) * (size_t)(nh ? nh : 1));
    double *conf = calloc((size_t)(nh ? nh : 1), sizeof(double));
    int nco = 0, noo = 0;
    for (int i = 0; i < nh; i++) {
        if (h[i].claim && h[i].stood) conf[i] = lp_confidence(&h[i].r, 2.0);
        if (h[i].claim) order[nco++] = i;
        else noo++;
    }
    for (int a = 1; a < nco; a++) {
        int k = order[a];
        int b = a;
        while (b > 0 && conf[order[b - 1]] < conf[k]) { order[b] = order[b - 1]; b--; }
        order[b] = k;
    }
    int ncret = nco, noret = noo;
    if (fan > 0) {
        if (ncret > fan) ncret = fan;
        if (noret > fan) noret = fan;
    }
    char pj[64 * 40] = "", cj[160] = "null";
    if (np) {
        parts_json(parts, np, pj, sizeof pj);
        coord_json(&trunk, cj, sizeof cj);
    }
    char *out = NULL;
    size_t un = 0, ucap = 0;
    buf_fmt(&out, &un, &ucap,
            "{\"id\":\"%s\",\"tier\":%u,\"fan\":%d,\"coord\":%s,\"hilbert\":\"%016llx\","
            "\"parts\":[%s],\"ms\":{\"identity\":%.3f,\"database\":%.3f},"
            "\"holds_read\":%d,\"claims_read\":%d,\"occurrences_read\":%d,\"physicality\":[",
            idbuf, trunk.tier, fan, np ? cj : "null",
            np ? (unsigned long long)lp_hilbert4(&trunk.c) : 0ull, pj, id_ms, db_ms, nh, nco, noo);
    for (int i = 0; i < PQntuples(self); i++) {
        char one[32];
        int nids = 0;
        int tier = (int)lp_be(PQgetvalue(self, i, 1), 2);
        int claim = mask_claim(PQgetvalue(self, i, 2), PQgetlength(self, i, 2));
        buf_fmt(&out, &un, &ucap, "%s{\"tier\":%d,\"claim\":%s,\"constituents\":", i ? "," : "", tier, claim ? "true" : "false");
        emit_ids(&out, &un, &ucap, (const uint8_t *)PQgetvalue(self, i, 0), PQgetlength(self, i, 0), &nids);
        snprintf(one, sizeof one, ",\"n\":%d}", nids);
        buf_add(&out, &un, &ucap, one, strlen(one));
    }
    buf_add(&out, &un, &ucap, "],\"claims\":[", strlen("],\"claims\":["));
    int shown = 0;
    for (int i = 0; i < ncret; i++) {
        Hold *s = &h[order[i]];
        char eid[33];
        hex_id(&s->entity, eid);
        int nids = 0;
        buf_fmt(&out, &un, &ucap,
                "%s{\"entity\":\"%s\",\"tier\":%d,\"stood\":%s,\"confidence\":%.6f,"
                "\"rating\":%.3f,\"deviation\":%.3f,\"volatility\":%.6f,\"matches\":%d,\"constituents\":",
                shown ? "," : "", eid, (int)s->tier, s->stood ? "true" : "false", conf[order[i]],
                s->r.rating, s->r.deviation, s->r.volatility, s->matches);
        emit_ids(&out, &un, &ucap, s->path, s->path_len, &nids);
        buf_fmt(&out, &un, &ucap, ",\"n\":%d}", nids);
        shown++;
    }
    buf_add(&out, &un, &ucap, "],\"occurrences\":[", strlen("],\"occurrences\":["));
    shown = 0;
    for (int i = 0, seen = 0; i < nh && seen < noret; i++) {
        if (h[i].claim) continue;
        char eid[33];
        hex_id(&h[i].entity, eid);
        int nids = 0;
        buf_fmt(&out, &un, &ucap, "%s{\"entity\":\"%s\",\"tier\":%d,\"constituents\":", shown ? "," : "", eid, (int)h[i].tier);
        emit_ids(&out, &un, &ucap, h[i].path, h[i].path_len, &nids);
        buf_fmt(&out, &un, &ucap, ",\"n\":%d}", nids);
        shown++;
        seen++;
    }
    buf_add(&out, &un, &ucap, "],\"attestations\":[", strlen("],\"attestations\":["));
    for (int i = 0; i < PQntuples(att); i++) {
        char claim[33], wit[33];
        lp_id id;
        memcpy(id.b, PQgetvalue(att, i, 0), 16);
        hex_id(&id, claim);
        memcpy(id.b, PQgetvalue(att, i, 1), 16);
        hex_id(&id, wit);
        int pos_null = PQgetisnull(att, i, 2);
        int pos = pos_null ? 0 : (int)lp_be(PQgetvalue(att, i, 2), 4);
        uint32_t sb = (uint32_t)lp_be(PQgetvalue(att, i, 3), 4);
        float score;
        memcpy(&score, &sb, 4);
        int games = (int)lp_be(PQgetvalue(att, i, 4), 4);
        double trust = lp_be_f64(PQgetvalue(att, i, 5));
        if (pos_null) buf_fmt(&out, &un, &ucap,
            "%s{\"claim\":\"%s\",\"witness\":\"%s\",\"position\":null,\"score\":%.6g,\"games\":%d,\"trust\":%.6g}",
            i ? "," : "", claim, wit, score, games, trust);
        else buf_fmt(&out, &un, &ucap,
            "%s{\"claim\":\"%s\",\"witness\":\"%s\",\"position\":%d,\"score\":%.6g,\"games\":%d,\"trust\":%.6g}",
            i ? "," : "", claim, wit, pos, score, games, trust);
    }
    buf_add(&out, &un, &ucap, "],\"constituents\":[", strlen("],\"constituents\":["));
    for (int i = 0; i < ndown; i++) {
        if (i) buf_add(&out, &un, &ucap, ",", 1);
        emit_down(&out, &un, &ucap, &down[i], fan);
    }
    buf_add(&out, &un, &ucap, "]}", 2);
    PQclear(self);
    PQclear(att);
    free(cb);
    free(cids);
    free(order);
    free(conf);
    holds_free(h, nh);
    downs_free(down, ndown);
    if (!out) {
        http(c, 500, "{\"error\":\"memory\"}");
        return;
    }
    http(c, 200, out);
    free(out);
}
static void on_by_parts(int c, const char *body, const char *sql, const char *kind) {
    char text[8192];
    if (!jstr(body, "input", text, sizeof text) || !text[0]) {
        http(c, 400, "{\"error\":\"input\"}");
        return;
    }
    lp_ref trunk, parts[256];
    size_t np = 0;
    if (!name_entity(text, &trunk, parts, 256, &np) || !np) {
        http(c, 500, "{\"error\":\"tier0\"}");
        return;
    }
    char list[256 * 34];
    if (!part_list(parts, np, list, sizeof list)) {
        http(c, 400, "{\"error\":\"phrase too long\"}");
        return;
    }
    PGconn *pg = db();
    if (!pg) {
        http(c, 503, "{\"error\":\"database\"}");
        return;
    }
    char id[33], *rows = NULL, err[512], braced[256 * 34 + 2];
    hex_id(&trunk.id, id);
    snprintf(braced, sizeof braced, "{%s}", list);
    uint64_t t0 = nsec();
    if (!q1(pg, sql, braced, &rows, err, sizeof err)) {
        char e[512], b[700];
        json_esc(err, e, sizeof e);
        snprintf(b, sizeof b, "{\"error\":\"%s\",\"detail\":\"%s\"}", kind, e);
        http(c, 500, b);
        return;
    }
    double ms = (double)(nsec() - t0) / 1e6;
    size_t cap = strlen(rows) + 128;
    char *out = malloc(cap);
    snprintf(out, cap, "{\"id\":\"%s\",\"ms\":%.3f,\"%s\":%s}", id, ms, kind, rows);
    free(rows);
    http(c, 200, out);
    free(out);
}

static void on_shape(int c, const char *body) {
    char a[8192], b[8192];
    if (!jstr(body, "input", a, sizeof a) || !jstr(body, "other", b, sizeof b) || !a[0] || !b[0]) {
        http(c, 400, "{\"error\":\"input and other\"}");
        return;
    }
    lp_ref ta, tb, pa[256], pb[256];
    size_t na = 0, nb = 0;
    if (!name_entity(a, &ta, pa, 256, &na) || !name_entity(b, &tb, pb, 256, &nb)) {
        http(c, 500, "{\"error\":\"tier0\"}");
        return;
    }
    char la[256 * 34], lb[256 * 34], ida[33], idb[33];
    if (!part_list(pa, na, la, sizeof la) || !part_list(pb, nb, lb, sizeof lb)) {
        http(c, 400, "{\"error\":\"phrase too long\"}");
        return;
    }
    hex_id(&ta.id, ida);
    hex_id(&tb.id, idb);
    PGconn *pg = db();
    if (!pg) {
        http(c, 503, "{\"error\":\"database\"}");
        return;
    }
    char *dist = NULL, err[512];
    uint64_t t0 = nsec();
    if (!q2(pg, SHAPE_SQL, la, lb, &dist, err, sizeof err)) {
        char e[512], buf[700];
        json_esc(err, e, sizeof e);
        snprintf(buf, sizeof buf, "{\"error\":\"shape\",\"detail\":\"%s\"}", e);
        http(c, 500, buf);
        return;
    }
    double ms = (double)(nsec() - t0) / 1e6;
    char out[512];
    snprintf(out, sizeof out,
             "{\"id\":\"%s\",\"other\":\"%s\",\"frechet\":%s,\"ms\":%.3f}",
             ida, idb, dist, ms);
    free(dist);
    http(c, 200, out);
}

static void on_ingest(int c, const char *body) {
    char path[4096];
    if (!jstr(body, "path", path, sizeof path) || path[0] != '/') {
        http(c, 400, "{\"error\":\"path\"}");
        return;
    }
    struct stat st;
    if (stat(path, &st) != 0) {
        http(c, 400, "{\"error\":\"path not found\"}");
        return;
    }
    int fds[2];
    if (pipe(fds) != 0) {
        http(c, 500, "{\"error\":\"pipe\"}");
        return;
    }
    pid_t pid = fork();
    if (pid < 0) {
        close(fds[0]);
        close(fds[1]);
        http(c, 500, "{\"error\":\"fork\"}");
        return;
    }
    if (pid == 0) {
        dup2(fds[1], 1);
        dup2(fds[1], 2);
        close(fds[0]);
        close(fds[1]);
        char *av[] = { (char *)ENGINE, "ingest", path, NULL };
        execv(ENGINE, av);
        _exit(127);
    }
    close(fds[1]);
    char log[65536];
    size_t n = 0;
    while (n + 1 < sizeof log) {
        ssize_t r = read(fds[0], log + n, sizeof log - 1 - n);
        if (r <= 0) break;
        n += (size_t)r;
    }
    close(fds[0]);
    log[n] = 0;
    int status = 0;
    waitpid(pid, &status, 0);
    int code = WIFEXITED(status) ? WEXITSTATUS(status) : 128;
    char esc[65536];
    json_esc(log, esc, sizeof esc);
    char *out = malloc(strlen(esc) + 128);
    snprintf(out, strlen(esc) + 128, "{\"exit\":%d,\"engine\":\"laplace ingest\",\"log\":\"%s\"}", code, esc);
    http(c, code == 0 ? 200 : 500, out);
    free(out);
}

static const char *SURFACE =
    "{\"service\":\"Laplace-MCP\","
    "\"caller\":\"sends content or an ID; this process holds the database connection\","
    "\"endpoints\":["
    "{\"method\":\"POST\",\"path\":\"/v1/embeddings\",\"body\":\"{\\\"input\\\":\\\"text\\\"}\",\"does\":\"names the entity from tier 0\"},"
    "{\"method\":\"POST\",\"path\":\"/v1/search\",\"body\":\"{\\\"input\\\":\\\"text\\\"} or {\\\"id\\\":\\\"32 hex\\\"}; \\\"text\\\":true renders laplace_text\",\"does\":\"one ID into laplace_containers and laplace_claims\"},"
    "{\"method\":\"POST\",\"path\":\"/v1/forward\",\"body\":\"{\\\"input\\\":\\\"prompt\\\"}\",\"does\":\"laplace_forward on the prompt constituents\"},"
    "{\"method\":\"POST\",\"path\":\"/v1/shape\",\"body\":\"{\\\"input\\\":\\\"text\\\",\\\"other\\\":\\\"text\\\"}\",\"does\":\"laplace_frechet4d on stored constituent coordinates\"},"
    "{\"method\":\"POST\",\"path\":\"/v1/ingest\",\"body\":\"{\\\"path\\\":\\\"/absolute/path\\\"}\",\"does\":\"laplace ingest: decompose, deduplicate, record, attest\"},"
    "{\"method\":\"GET\",\"path\":\"/v1/surface\"},"
    "{\"method\":\"POST\",\"path\":\"/mcp\",\"body\":\"same bodies, with \\\"op\\\": embeddings|search|forward|shape|ingest|surface\"}"
    "],"
    "\"database_functions\":[\"laplace_containers\",\"laplace_claims\",\"laplace_claims_each\",\"laplace_text\",\"laplace_fills\",\"laplace_forward\",\"laplace_frechet4d\",\"laplace_dtw4d\",\"laplace_edr4d\",\"laplace_distance4d\",\"laplace_couple\",\"laplace_follows\",\"laplace_paths\",\"laplace_attested\",\"laplace_compose\",\"laplace_parts\"],"
    "\"engine_commands\":[\"tier0\",\"flags\",\"highway\",\"deploy\",\"sources\",\"ingest\",\"forget\",\"sweep\",\"index\",\"structure\",\"tree\",\"text\",\"pull\",\"turn\",\"hop\",\"translate\",\"degrees\",\"fills\",\"status\",\"bench\",\"model\"],"
    "\"specified_beyond_these_endpoints\":[\"translate\",\"degrees\",\"pull\",\"turn\",\"gaps\",\"file metadata\",\"DTW\",\"EDR\",\"outlier Fréchet\",\"the forward stages RESOLVE COUPLE ORIENT ROUTE SCAN PROPOSE STEER SELECT REALIZE WITNESS\"]}";

static int engine_run(char **av, char **outp, int *code, int timeout_s) {
    int fds[2];
    if (pipe(fds) != 0) return 0;
    pid_t pid = fork();
    if (pid < 0) {
        close(fds[0]);
        close(fds[1]);
        return 0;
    }
    if (pid == 0) {
        setpgid(0, 0);
        dup2(fds[1], 1);
        dup2(fds[1], 2);
        close(fds[0]);
        close(fds[1]);
        execv(ENGINE, av);
        _exit(127);
    }
    setpgid(pid, pid);
    close(fds[1]);
    size_t cap = 1 << 22, n = 0;
    char *buf = malloc(cap);
    uint64_t deadline = timeout_s > 0 ? nsec() + (uint64_t)timeout_s * 1000000000ull : 0;
    int timed = 0;
    while (n + 1 < cap) {
        int left = -1;
        if (timeout_s > 0) {
            int64_t ms = ((int64_t)deadline - (int64_t)nsec()) / 1000000ll;
            if (ms <= 0) { timed = 1; break; }
            left = (int)ms;
        }
        struct pollfd p = { .fd = fds[0], .events = POLLIN };
        int w = poll(&p, 1, left);
        if (w == 0) { timed = 1; break; }
        if (w < 0) break;
        ssize_t r = read(fds[0], buf + n, cap - 1 - n);
        if (r <= 0) break;
        n += (size_t)r;
    }
    close(fds[0]);
    if (timed) {
        kill(-pid, SIGTERM);
        snprintf(buf + n, cap - n, "\n[stopped after %d s]\n", timeout_s);
    }
    int st = 0;
    waitpid(pid, &st, 0);
    *code = timed ? 124 : (WIFEXITED(st) ? WEXITSTATUS(st) : 128);
    *outp = buf;
    return 1;
}

static int plain_arg(const char *s) {
    return s && s[0] && s[0] != '-' && strlen(s) < 4096;
}

static void on_engine(int c, char **av) {
    char *log = NULL;
    int code = 127;
    if (!engine_run(av, &log, &code, 0)) {
        http(c, 500, "{\"error\":\"engine\"}");
        return;
    }
    char *esc = malloc(strlen(log) * 6 + 64);
    json_esc(log, esc, strlen(log) * 6 + 64);
    char *out = malloc(strlen(esc) + 128);
    snprintf(out, strlen(esc) + 128, "{\"exit\":%d,\"log\":\"%s\"}", code, esc);
    free(esc);
    free(log);
    http(c, code == 0 ? 200 : 500, out);
    free(out);
}

static void on_hop(int c, const char *body) {
    char text[8192];
    if (!jstr(body, "input", text, sizeof text) || !plain_arg(text)) {
        http(c, 400, "{\"error\":\"input\"}");
        return;
    }
    char *av[] = { (char *)ENGINE, "hop", text, NULL };
    on_engine(c, av);
}

static void on_text(int c, const char *body) {
    char text[8192];
    if (!jstr(body, "input", text, sizeof text) || !plain_arg(text)) {
        http(c, 400, "{\"error\":\"input\"}");
        return;
    }
    char *av[] = { (char *)ENGINE, "text", text, NULL };
    on_engine(c, av);
}

static void on_translate(int c, const char *body) {
    char word[512], from[32], to[32];
    if (!jstr(body, "input", word, sizeof word) || !jstr(body, "from", from, sizeof from) ||
        !jstr(body, "to", to, sizeof to) || !plain_arg(word) || !plain_arg(from) || !plain_arg(to)) {
        http(c, 400, "{\"error\":\"input, from, to\"}");
        return;
    }
    char *av[] = { (char *)ENGINE, "translate", word, from, to, NULL };
    on_engine(c, av);
}

static void on_pull(int c, const char *body) {
    char text[8192];
    if (!jstr(body, "input", text, sizeof text) || !plain_arg(text)) {
        http(c, 400, "{\"error\":\"input\"}");
        return;
    }
    char *av[] = { (char *)ENGINE, "pull", text, NULL };
    on_engine(c, av);
}

static void on_degrees(int c, const char *body) {
    char a[8192], b[8192];
    if (!jstr(body, "input", a, sizeof a) || !jstr(body, "other", b, sizeof b) || !plain_arg(a) || !plain_arg(b)) {
        http(c, 400, "{\"error\":\"input and other\"}");
        return;
    }
    char *av[] = { (char *)ENGINE, "degrees", a, b, NULL };
    on_engine(c, av);
}

static void rpc_id(const char *body, char *out, size_t cap) {
    const char *p = strstr(body, "\"id\"");
    out[0] = 'n'; out[1] = 'u'; out[2] = 'l'; out[3] = 'l'; out[4] = 0;
    if (!p) return;
    p += 4;
    while (*p == ' ' || *p == '\t' || *p == ':') p++;
    size_t n = 0;
    if (*p == '"') {
        out[n++] = '"';
        p++;
        while (*p && *p != '"' && n + 2 < cap) out[n++] = *p++;
        if (n + 1 < cap) out[n++] = '"';
        out[n] = 0;
        return;
    }
    while (*p && *p != ',' && *p != '}' && *p != ' ' && n + 1 < cap) out[n++] = *p++;
    out[n] = 0;
}

static const char *TOOLS =
    "{\"tools\":["
    "{\"name\":\"search\",\"description\":\"Name text on the client from tier 0, or take an ID the caller already holds. The trunk is read with holds_above, and so is every constituent whose tier is above a codepoint or a grapheme. A tier-0 constituent stays in the path, in its place, with its codepoint. fan, when sent, caps how many claims and occurrences are returned; omitted, all of them are.\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"input\":{\"type\":\"string\"},\"id\":{\"type\":\"string\"},\"fan\":{\"type\":\"integer\"}},\"required\":[]}},"
    "{\"name\":\"embeddings\",\"description\":\"Same call shape as an embedding endpoint. Returns the Laplace entity ID, tier, coordinate, Hilbert value, and constituent IDs. Does not touch the database.\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"input\":{\"type\":\"string\"}},\"required\":[\"input\"]}},"
    "{\"name\":\"shape\",\"description\":\"Discrete Frechet distance between two texts, laplace_frechet4d on the stored coordinates of their constituents.\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"input\":{\"type\":\"string\"},\"other\":{\"type\":\"string\"}},\"required\":[\"input\",\"other\"]}},"
    "{\"name\":\"forward\",\"description\":\"laplace_forward on the constituent IDs of a prompt: which spans are observed and what follows them.\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"input\":{\"type\":\"string\"}},\"required\":[\"input\"]}},"
    "{\"name\":\"ingest\",\"description\":\"laplace ingest of an absolute filesystem path: decompose, deduplicate, record, attest into the Laplace database.\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"}},\"required\":[\"path\"]}},"
    "{\"name\":\"hop\",\"description\":\"laplace hop: claims that hold the named entity, and the tiers of the paths that contain it.\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"input\":{\"type\":\"string\"}},\"required\":[\"input\"]}},"
    "{\"name\":\"text\",\"description\":\"laplace text: the entity ID, coordinate, and constituents, computed on the client. No database.\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"input\":{\"type\":\"string\"}},\"required\":[\"input\"]}},"
    "{\"name\":\"translate\",\"description\":\"laplace translate: a word up through its concepts and down into another language. Languages as the resources write them, such as en, de, fr, ja.\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"input\":{\"type\":\"string\"},\"from\":{\"type\":\"string\"},\"to\":{\"type\":\"string\"}},\"required\":[\"input\",\"from\",\"to\"]}},"
    "{\"name\":\"degrees\",\"description\":\"laplace degrees: how far one entity is from another over rated claims.\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"input\":{\"type\":\"string\"},\"other\":{\"type\":\"string\"}},\"required\":[\"input\",\"other\"]}},"
    "{\"name\":\"pull\",\"description\":\"laplace pull: the forward pass. The prompt is decomposed, trajectories give precedes, contains and co-occurrence, claims tug back by their Glicko standing, and the firmware's take steps choose the segment. No fan cap is passed; the firmware's own fan and hops apply.\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"input\":{\"type\":\"string\"}},\"required\":[\"input\"]}}"
    "]}";

static void mcp_rpc(int c, const char *body) {
    char method[64], id[64];
    if (!jstr(body, "method", method, sizeof method)) {
        http(c, 400, "{\"jsonrpc\":\"2.0\",\"id\":null,\"error\":{\"code\":-32600,\"message\":\"method\"}}");
        return;
    }
    rpc_id(body, id, sizeof id);
    if (!strcmp(method, "notifications/initialized") || !strncmp(method, "notifications/", 14)) {
        http(c, 202, "");
        return;
    }
    if (!strcmp(method, "initialize")) {
        char ver[40] = "2025-03-26";
        jstr(body, "protocolVersion", ver, sizeof ver);
        char out[512];
        snprintf(out, sizeof out,
                 "{\"jsonrpc\":\"2.0\",\"id\":%s,\"result\":{\"protocolVersion\":\"%s\","
                 "\"capabilities\":{\"tools\":{}},\"serverInfo\":{\"name\":\"laplace\",\"version\":\"0.1\"}}}",
                 id, ver);
        http(c, 200, out);
        return;
    }
    if (!strcmp(method, "ping")) {
        char out[128];
        snprintf(out, sizeof out, "{\"jsonrpc\":\"2.0\",\"id\":%s,\"result\":{}}", id);
        http(c, 200, out);
        return;
    }
    if (!strcmp(method, "tools/list")) {
        char *out = malloc(strlen(TOOLS) + 80);
        snprintf(out, strlen(TOOLS) + 80, "{\"jsonrpc\":\"2.0\",\"id\":%s,\"result\":%s}", id, TOOLS);
        http(c, 200, out);
        free(out);
        return;
    }
    if (!strcmp(method, "tools/call")) {
        char name[32];
        if (!jstr(body, "name", name, sizeof name)) {
            char out[160];
            snprintf(out, sizeof out, "{\"jsonrpc\":\"2.0\",\"id\":%s,\"error\":{\"code\":-32602,\"message\":\"name\"}}", id);
            http(c, 200, out);
            return;
        }
        capture = 1;
        capture_code = 500;
        free(capture_body);
        capture_body = NULL;
        if (!strcmp(name, "search")) on_search(c, body);
        else if (!strcmp(name, "embeddings")) on_embeddings(c, body);
        else if (!strcmp(name, "shape")) on_shape(c, body);
        else if (!strcmp(name, "forward")) on_by_parts(c, body, FORWARD_SQL, "forward");
        else if (!strcmp(name, "ingest")) on_ingest(c, body);
        else if (!strcmp(name, "hop")) on_hop(c, body);
        else if (!strcmp(name, "text")) on_text(c, body);
        else if (!strcmp(name, "translate")) on_translate(c, body);
        else if (!strcmp(name, "degrees")) on_degrees(c, body);
        else if (!strcmp(name, "pull")) on_pull(c, body);
        else {
            capture = 0;
            char out[160];
            snprintf(out, sizeof out, "{\"jsonrpc\":\"2.0\",\"id\":%s,\"error\":{\"code\":-32602,\"message\":\"unknown tool\"}}", id);
            http(c, 200, out);
            return;
        }
        capture = 0;
        size_t elen = (capture_body ? strlen(capture_body) : 0) * 6 + 64;
        char *esc = malloc(elen);
        json_esc(capture_body ? capture_body : "", esc, elen);
        int err = capture_code != 200;
        char *out = malloc(strlen(esc) + 160);
        snprintf(out, strlen(esc) + 160,
                 "{\"jsonrpc\":\"2.0\",\"id\":%s,\"result\":{\"content\":[{\"type\":\"text\",\"text\":\"%s\"}],\"isError\":%s}}",
                 id, esc, err ? "true" : "false");
        free(esc);
        http(c, 200, out);
        free(out);
        return;
    }
    char out[200];
    snprintf(out, sizeof out, "{\"jsonrpc\":\"2.0\",\"id\":%s,\"error\":{\"code\":-32601,\"message\":\"method not found\"}}", id);
    http(c, 200, out);
}

static void dispatch(int c, const char *method, const char *path, const char *body) {
    char op[32] = "";
    if (!strcmp(path, "/mcp") && strstr(body, "\"jsonrpc\"")) {
        mcp_rpc(c, body);
        return;
    }
    if (!strcmp(path, "/mcp")) {
        if (!jstr(body, "op", op, sizeof op)) {
            http(c, 400, "{\"error\":\"op\"}");
            return;
        }
        if (!strcmp(op, "embeddings") || !strcmp(op, "embed")) path = "/v1/embeddings";
        else if (!strcmp(op, "search")) path = "/v1/search";
        else if (!strcmp(op, "forward")) path = "/v1/forward";
        else if (!strcmp(op, "shape") || !strcmp(op, "frechet")) path = "/v1/shape";
        else if (!strcmp(op, "ingest")) path = "/v1/ingest";
        else if (!strcmp(op, "hop")) path = "/v1/hop";
        else if (!strcmp(op, "text")) path = "/v1/text";
        else if (!strcmp(op, "translate")) path = "/v1/translate";
        else if (!strcmp(op, "degrees")) path = "/v1/degrees";
        else if (!strcmp(op, "pull")) path = "/v1/pull";
        else if (!strcmp(op, "surface")) path = "/v1/surface";
        else {
            http(c, 400, "{\"error\":\"op\"}");
            return;
        }
        method = !strcmp(path, "/v1/surface") ? "GET" : "POST";
    }
    if (!strcmp(method, "GET") && !strcmp(path, "/health")) {
        http(c, 200, "{\"ok\":true}");
        return;
    }
    if (!strcmp(method, "GET") && !strcmp(path, "/v1/surface")) {
        http(c, 200, SURFACE);
        return;
    }
    if (strcmp(method, "POST")) {
        http(c, 404, "{\"error\":\"not found\"}");
        return;
    }
    if (!strcmp(path, "/v1/embeddings")) on_embeddings(c, body);
    else if (!strcmp(path, "/v1/search")) on_search(c, body);
    else if (!strcmp(path, "/v1/forward")) on_by_parts(c, body, FORWARD_SQL, "forward");
    else if (!strcmp(path, "/v1/shape")) on_shape(c, body);
    else if (!strcmp(path, "/v1/ingest")) on_ingest(c, body);
    else if (!strcmp(path, "/v1/hop")) on_hop(c, body);
    else if (!strcmp(path, "/v1/text")) on_text(c, body);
    else if (!strcmp(path, "/v1/translate")) on_translate(c, body);
    else if (!strcmp(path, "/v1/degrees")) on_degrees(c, body);
    else if (!strcmp(path, "/v1/pull")) on_pull(c, body);
    else http(c, 404, "{\"error\":\"not found\"}");
}

int api_serve(const char *host, int port) {
    T0 = lp_tier0_map(NULL);
    if (!T0) {
        fprintf(stderr, "tier 0 did not map\n");
        return 1;
    }
    TX = lp_text_new(T0);
    if (!TX) return 1;
    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) return 1;
    int one = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_port = htons((uint16_t)port);
    if (inet_pton(AF_INET, host, &a.sin_addr) != 1) return 1;
    if (bind(s, (struct sockaddr *)&a, sizeof a) != 0) {
        perror("bind");
        return 1;
    }
    if (listen(s, 64) != 0) return 1;
    fprintf(stderr, "Laplace-MCP %s:%d\n", host, port);
    for (;;) {
        int c = accept(s, NULL, NULL);
        if (c < 0) continue;
        char req[1 << 20];
        size_t got = 0;
        while (got + 1 < sizeof req) {
            ssize_t r = recv(c, req + got, sizeof req - 1 - got, 0);
            if (r <= 0) break;
            got += (size_t)r;
            req[got] = 0;
            char *hb = strstr(req, "\r\n\r\n");
            if (!hb) continue;
            long need = 0;
            const char *cl = strstr(req, "Content-Length:");
            if (!cl) cl = strstr(req, "content-length:");
            if (cl) need = strtol(cl + 15, NULL, 10);
            if ((long)(got - (size_t)(hb + 4 - req)) >= need) break;
        }
        req[got] = 0;
        char method[16] = "GET", path[256] = "/";
        sscanf(req, "%15s %255s", method, path);
        char *q = strchr(path, '?');
        if (q) *q = 0;
        char *body = strstr(req, "\r\n\r\n");
        body = body ? body + 4 : "";
        dispatch(c, method, path, body);
        close(c);
    }
}
