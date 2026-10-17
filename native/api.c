#define _GNU_SOURCE
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <io.h>
#include <fcntl.h>
#define LAPLACE_WIN_ARPA_INET_H
#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif
typedef intptr_t ssize_t;
#define close_sock(s) closesocket(s)
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>
#define close_sock(s) close(s)
#endif

#include "api.h"
#include "lpm.h"
#include "lpm_config.h"
#include "os.h"
#include "semantic.h"

#include "laplace/laplace.h"

#include <libpq-fe.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

const lp_tier0_record *T0;
static lp_text *TX;
static PGconn *PG;
static Lpm *CODE_IX;

/* Where things are when the environment does not say: what this service was built with (lpm_config.h). */
static const char *CONN_DEFAULT = LPM_CONNINFO_DEFAULT;
/* The engine the routes run: LAPLACE_ENGINE, else the one this build was configured beside. */
static const char *ENGINE = LPM_ENGINE_DEFAULT;

static void engine_resolve(void) {
    const char *e = getenv("LAPLACE_ENGINE");
    if (e && *e) ENGINE = e;
}

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
static int stdio_capture;
static int stdio_code;
static char *stdio_body;

static void http(int c, int code, const char *body) {
    if (capture) {
        capture_code = code;
        free(capture_body);
        capture_body = strdup(body ? body : "");
        return;
    }
    if (stdio_capture) {
        stdio_code = code;
        free(stdio_body);
        stdio_body = strdup(body ? body : "");
        return;
    }
    const char *why = code == 200 ? "OK" : code == 202 ? "Accepted" : code == 400 ? "Bad Request" : code == 404 ? "Not Found" : "Error";
    char hdr[256];
    int n = snprintf(hdr, sizeof hdr,
                     "HTTP/1.1 %d %s\r\nContent-Type: application/json\r\nContent-Length: %zu\r\nConnection: close\r\n\r\n",
                     code, why, strlen(body));
    send(c, hdr, n, MSG_NOSIGNAL);
    send(c, body, (int)strlen(body), MSG_NOSIGNAL);
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

/* A flag: JSON true or false, or the strings "true" and "false". Absent or anything else: dflt. */
static int jbool(const char *body, const char *key, int dflt) {
    char pat[80];
    snprintf(pat, sizeof pat, "\"%s\"", key);
    const char *p = strstr(body, pat);
    if (!p) return dflt;
    p += strlen(pat);
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
    if (*p != ':') return dflt;
    p++;
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
    if (!strncmp(p, "true", 4) || !strncmp(p, "\"true\"", 6)) return 1;
    if (!strncmp(p, "false", 5) || !strncmp(p, "\"false\"", 7)) return 0;
    return dflt;
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

static void unit_coord_json(const lp_ref *r, char *out, size_t cap) {
    double v[4] = {
        (double)r->c.m[0] / LP_FIXED_ONE,
        (double)r->c.m[1] / LP_FIXED_ONE,
        (double)r->c.m[2] / LP_FIXED_ONE,
        (double)r->c.m[3] / LP_FIXED_ONE,
    };
    double s2 = v[0] * v[0] + v[1] * v[1] + v[2] * v[2] + v[3] * v[3];
    if (s2 > 1e-30) {
        double inv = 1.0 / sqrt(s2);
        for (int d = 0; d < 4; d++) v[d] *= inv;
    } else {
        v[0] = 1.0; v[1] = 0.0; v[2] = 0.0; v[3] = 0.0;
    }
    snprintf(out, cap, "[%.17g,%.17g,%.17g,%.17g]", v[0], v[1], v[2], v[3]);
}

static size_t parse_str_array(const char *body, const char *key, char **items, size_t max_items) {
    char pat[80];
    snprintf(pat, sizeof pat, "\"%s\"", key);
    const char *p = strstr(body, pat);
    if (!p) return 0;
    p += strlen(pat);
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
    if (*p != ':') return 0;
    p++;
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
    if (*p != '[') return 0;
    p++;
    size_t count = 0;
    while (*p && count < max_items) {
        while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r' || *p == ',') p++;
        if (*p == ']' || !*p) break;
        if (*p == '{') {
            const char *end = strchr(p, '}');
            if (!end) break;
            size_t len = (size_t)(end - p + 1);
            char *sub = malloc(len + 1);
            memcpy(sub, p, len);
            sub[len] = 0;
            char txt[16384] = "";
            if (jstr(sub, "text", txt, sizeof txt)) items[count++] = strdup(txt);
            free(sub);
            p = end + 1;
            continue;
        }
        if (*p != '"') break;
        p++;
        size_t cap = 4096, u = 0;
        char *s = malloc(cap);
        while (*p && *p != '"') {
            if (u + 4 >= cap) { cap *= 2; s = realloc(s, cap); }
            if (*p == '\\' && p[1]) {
                p++;
                if (*p == 'n') s[u++] = '\n';
                else if (*p == 'r') s[u++] = '\r';
                else if (*p == 't') s[u++] = '\t';
                else s[u++] = *p;
                p++;
            } else s[u++] = *p++;
        }
        s[u] = 0;
        if (*p == '"') p++;
        items[count++] = s;
    }
    return count;
}

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

static void on_embeddings(int c, const char *body) {
    char text[8192];
    uint64_t t0 = nsec();
    char *inputs[256];
    size_t ni = parse_str_array(body, "input", inputs, 256);
    if (!ni) ni = parse_str_array(body, "inputs", inputs, 256);
    if (!ni) {
        if (!jstr(body, "input", text, sizeof text) || !text[0]) {
            http(c, 400, "{\"error\":\"input\"}");
            return;
        }
        inputs[0] = strdup(text);
        ni = 1;
    }
    char *out = NULL;
    size_t un = 0, ucap = 0;
    buf_add(&out, &un, &ucap, "{\"object\":\"list\",\"model\":\"laplace\",\"data\":[", strlen("{\"object\":\"list\",\"model\":\"laplace\",\"data\":["));
    for (size_t i = 0; i < ni; i++) {
        lp_ref trunk, parts[64];
        size_t np = 0;
        if (!name_entity(inputs[i], &trunk, parts, 64, &np)) {
            for (size_t k = i; k < ni; k++) free(inputs[k]);
            free(out);
            http(c, 500, "{\"error\":\"tier0\"}");
            return;
        }
        char id[33], pj[64 * 40], cj[160], uj[160];
        hex_id(&trunk.id, id);
        parts_json(parts, np, pj, sizeof pj);
        coord_json(&trunk, cj, sizeof cj);
        unit_coord_json(&trunk, uj, sizeof uj);
        buf_fmt(&out, &un, &ucap,
                "%s{\"object\":\"embedding\",\"index\":%zu,\"embedding\":%s,"
                "\"id\":\"%s\",\"tier\":%u,\"coord\":%s,\"hilbert\":\"%016llx\",\"parts\":[%s]}",
                i ? "," : "", i, uj, id, trunk.tier, cj, (unsigned long long)lp_hilbert4(&trunk.c), pj);
        free(inputs[i]);
    }
    double ms = (double)(nsec() - t0) / 1e6;
    buf_fmt(&out, &un, &ucap, "],\"usage\":{\"prompt_tokens\":%zu,\"total_tokens\":%zu},\"ms\":{\"identity\":%.3f}}", ni, ni, ms);
    http(c, 200, out ? out : "{\"object\":\"list\",\"data\":[]}");
    free(out);
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
/* attested: Laplace-Engine/src/walk.c. Who said a claim is not a row: it is read off the containment above the claim,
 * one row a claim and the trunk that holds it (a trunk in the witness table is a witness, with its trust). */
typedef struct { lp_id claim, trunk; double games, tokens, score; uint32_t position; double trust; int witness; } Attested;
Attested *attested(PGconn *pg, const lp_id *claims, int n, int *nout, uint64_t *rows);

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
    Attested *att;
    int natt;
    lp_id *kids;
    int *ktier;
    int *kcp;
    int nkids;
} Down;

static void downs_free(Down *d, int n) {
    if (!d) return;
    for (int i = 0; i < n; i++) {
        holds_free(d[i].h, d[i].nh);
        free(d[i].att);
        free(d[i].kids);
        free(d[i].ktier);
        free(d[i].kcp);
    }
    free(d);
}

/* An entity's physicality is in the leaf of its ID's first byte (physicality_00 .. physicality_ff); a leaf holds every tier. */
static void phy_leaf_name(char *out, size_t cap, const lp_id *id) {
    snprintf(out, cap, "physicality_%02x", id->b[0]);
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

/* What the records under one trunk said of one claim: games (the records that say it), tokens (with the times each says
 * it), score (the mean a game), position (the least any gave it; null when none did). */
static void emit_att_rows(char **b, size_t *n, size_t *cap, const Attested *att, int rows) {
    for (int i = 0; i < rows; i++) {
        char claim[33], trunk[33], pos[16] = "null";
        hex_id(&att[i].claim, claim);
        hex_id(&att[i].trunk, trunk);
        if (att[i].position) snprintf(pos, sizeof pos, "%u", att[i].position);
        buf_fmt(b, n, cap,
            "%s{\"claim\":\"%s\",\"trunk\":\"%s\",\"witness\":%s,\"trust\":%.6g,"
            "\"games\":%.0f,\"tokens\":%.0f,\"score\":%.6g,\"position\":%s}",
            i ? "," : "", claim, trunk, att[i].witness ? "true" : "false", att[i].trust,
            att[i].games, att[i].tokens, att[i].games > 0 ? att[i].score / att[i].games : 0.0, pos);
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
        emit_att_rows(b, n, cap, d->att, d->natt);
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
    phy_leaf_name(leaf, sizeof leaf, &trunk.id);
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
    /* Who said this ID and every claim that holds it: a walk up the containment from them (the engine's attested). */
    int nclaim = 0;
    for (int i = 0; i < nh; i++) nclaim += h[i].claim;
    lp_id *cids = malloc(sizeof(lp_id) * (size_t)(nclaim + 1));
    int ncids = 0;
    cids[ncids++] = trunk.id;
    for (int i = 0; i < nh; i++) if (h[i].claim) cids[ncids++] = h[i].entity;
    int natt = 0;
    Attested *att = attested(pg, cids, ncids, &natt, NULL);
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
        down[i].att = attested(pg, cids2, n2, &down[i].natt, NULL);
        free(cids2);
        uint8_t oneb[40];
        size_t onel = ids_param(oneb, &down[i].id, 1);
        const char *ov[1] = { (const char *)oneb };
        int ol[1] = { (int)onel }, of[1] = { 1 };
        char leaf[64], psql2[192];
        phy_leaf_name(leaf, sizeof leaf, &down[i].id);
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
    emit_att_rows(&out, &un, &ucap, att, natt);
    buf_add(&out, &un, &ucap, "],\"constituents\":[", strlen("],\"constituents\":["));
    for (int i = 0; i < ndown; i++) {
        if (i) buf_add(&out, &un, &ucap, ",", 1);
        emit_down(&out, &un, &ucap, &down[i], fan);
    }
    buf_add(&out, &un, &ucap, "]}", 2);
    PQclear(self);
    free(att);
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

static int engine_run(char **av, char **outp, int *code, int timeout_s) {
#ifdef _WIN32
    (void)timeout_s;
    const char *tmpdir = getenv("TEMP");
    if (!tmpdir || !*tmpdir) tmpdir = getenv("TMP");
    if (!tmpdir || !*tmpdir) tmpdir = ".";
    char logpath[1024];
    snprintf(logpath, sizeof logpath, "%s/lpm-engine-%llu.log", tmpdir, (unsigned long long)nsec());
    int rc = os_run_logged(av[0], av, logpath, NULL, NULL);
    if (rc < 0) return 0;
    *code = rc;
    FILE *f = fopen(logpath, "rb");
    size_t cap = 1 << 22, n = 0;
    char *buf = malloc(cap);
    if (f && buf) {
        n = fread(buf, 1, cap - 1, f);
        fclose(f);
    }
    remove(logpath);
    if (!buf) return 0;
    buf[n] = '\0';
    *outp = buf;
    return 1;
#else
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
    buf[n] = '\0';
    if (timed) {
        kill(-pid, SIGTERM);
        snprintf(buf + n, cap - n, "\n[stopped after %d s]\n", timeout_s);
    }
    int st = 0;
    waitpid(pid, &st, 0);
    *code = timed ? 124 : (WIFEXITED(st) ? WEXITSTATUS(st) : 128);
    *outp = buf;
    return 1;
#endif
}

static void on_ingest(int c, const char *body) {
    char path[4096];
    if (!jstr(body, "path", path, sizeof path) || !(path[0] == '/' || (path[0] && path[1] == ':'))) {
        http(c, 400, "{\"error\":\"path\"}");
        return;
    }
    if (!os_exists(path)) {
        http(c, 400, "{\"error\":\"path not found\"}");
        return;
    }
    char *av[] = { (char *)ENGINE, "ingest", path, NULL };
    char *log = NULL;
    int code = 127;
    if (!engine_run(av, &log, &code, 0)) {
        http(c, 500, "{\"error\":\"engine\"}");
        return;
    }
    size_t elen = strlen(log) * 6 + 64;
    char *esc = malloc(elen);
    json_esc(log, esc, elen);
    char *out = malloc(strlen(esc) + 128);
    snprintf(out, strlen(esc) + 128, "{\"exit\":%d,\"engine\":\"laplace ingest\",\"log\":\"%s\"}", code, esc);
    free(esc);
    free(log);
    http(c, code == 0 ? 200 : 500, out);
    free(out);
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

/* LAPLACE_CODE_ROOTS, each existing root in turn. */
static void code_roots(void (*fn)(const char *root, void *ud), void *ud) {
    const char *roots = getenv("LAPLACE_CODE_ROOTS");
    if (!roots || !*roots) return;
    char *copy = strdup(roots);
    char *p = copy;
    while (p && *p) {
#ifdef _WIN32
        char *sep = strchr(p, ';');
#else
        char *sep = strchr(p, ':');
        if (!sep) sep = strchr(p, ';');
#endif
        if (sep) *sep = 0;
        if (*p && os_exists(p)) fn(p, ud);
        p = sep ? sep + 1 : NULL;
    }
    free(copy);
}

static void root_index(const char *root, void *ud) { lpm_index_path((Lpm *)ud, root); }

typedef struct { Lpm *ix; LpmUpdate u; } RootUpdate;
static void root_update(const char *root, void *ud) {
    RootUpdate *r = ud;
    lpm_update_path(r->ix, root, &r->u);
}

static Lpm *code_build(void) {
    Lpm *ix = lpm_new(NULL);
    if (ix) code_roots(root_index, ix);
    return ix;
}

static time_t CODE_CHECKED;

/* Bring the index up to date with its roots. Unchanged files are not read. When the documents of changed and
 * removed files pass 30% of all of them, the index is built again from the roots. Saved when anything moved. */
static int code_refresh(LpmUpdate *u, int *rebuilt) {
    RootUpdate r = { CODE_IX, {0} };
    code_roots(root_update, &r);
    *u = r.u;
    *rebuilt = 0;
    LpmStats st;
    lpm_stats(CODE_IX, &st);
    if (st.stale * 10 > (st.documents + st.stale) * 3) {
        Lpm *fresh = code_build();
        if (!fresh) return -1;
        lpm_free(CODE_IX);
        CODE_IX = fresh;
        *rebuilt = 1;
    }
    const char *rec = getenv("LAPLACE_CODE_RECORDS");
    if (rec && *rec && (u->changed || u->added || u->removed || *rebuilt) && lpm_save(CODE_IX, rec)) return -1;
    CODE_CHECKED = time(NULL);
    return 0;
}

/* The code index: the saved records when they load, else built from the roots (and saved). It is brought up to
 * date with its roots when it loads and then at most every LAPLACE_CODE_REFRESH_SECONDS (default 30; 0 never). */
static Lpm *ensure_code_ix(void) {
    const char *rec = getenv("LAPLACE_CODE_RECORDS");
    if (!CODE_IX) {
        if (rec && *rec && os_exists(rec)) CODE_IX = lpm_load(rec, NULL);
        if (!CODE_IX) {
            CODE_IX = code_build();
            if (!CODE_IX) return NULL;
            if (rec && *rec) lpm_save(CODE_IX, rec);
            CODE_CHECKED = time(NULL);
            return CODE_IX;
        }
        CODE_CHECKED = 0;
    }
    const char *ev = getenv("LAPLACE_CODE_REFRESH_SECONDS");
    long every = ev && *ev ? strtol(ev, NULL, 10) : 30;
    if (every > 0 && time(NULL) - CODE_CHECKED >= every) {
        LpmUpdate u;
        int rebuilt;
        if (code_refresh(&u, &rebuilt)) return NULL;
    }
    return CODE_IX;
}

static void read_lines_snippet(const char *path, uint32_t line0, uint32_t line1, char *out, size_t cap) {
    out[0] = 0;
    if (!path || !*path || cap < 2) return;
    FILE *f = fopen(path, "rb");
    if (!f) return;
    if (line0 == 0) line0 = 1;
    if (line1 < line0 || line1 - line0 > 60) line1 = line0 + 60;
    uint32_t cur = 1;
    size_t u = 0;
    int ch;
    while ((ch = fgetc(f)) != EOF) {
        if (cur >= line0 && cur <= line1) {
            if (ch != '\r' && u + 1 < cap) out[u++] = (char)ch;
        }
        if (ch == '\n') {
            cur++;
            if (cur > line1) break;
        }
    }
    out[u] = 0;
    fclose(f);
}

/* A code_search candidate, from the lexical index, from a semantic collection, or both. */
typedef struct {
    char path[4096];
    uint32_t line0, line1;
    int lexical, semantic;          /* semantic: index of the best semantic hit + 1 */
    uint8_t id[16];
    uint64_t hilbert;
    uint32_t shared;
    double cosine, rerank, frechet;
    char *snippet;
} CodeCand;

typedef struct { const char *label; char *out; size_t cap; int found; } RootMatch;

static void root_match(const char *root, void *ud) {
    RootMatch *m = ud;
    if (m->found) return;
    size_t rl = strlen(root);
    while (rl && (root[rl - 1] == '/' || root[rl - 1] == '\\')) rl--;
    size_t b = rl;
    while (b && root[b - 1] != '/' && root[b - 1] != '\\') b--;
    size_t nl = rl - b;
    if (nl && !strncmp(m->label, root + b, nl) && (m->label[nl] == '/' || m->label[nl] == '\\')) {
        snprintf(m->out, m->cap, "%.*s%s", (int)rl, root, m->label + nl);
        for (char *q = m->out; *q; q++) if (*q == '\\') *q = '/';
        m->found = 1;
    }
}

/* A collection's path is the repository label, then the path inside it: the root of that name supplies the rest. */
static int resolve_label(const char *label, char *out, size_t cap) {
    RootMatch m = { label, out, cap, 0 };
    code_roots(root_match, &m);
    if (!m.found) snprintf(out, cap, "%s", label);
    return m.found;
}

static int cand_cmp(const void *a, const void *b) {
    const CodeCand *x = a, *y = b;
    if (x->rerank != y->rerank) return x->rerank < y->rerank ? 1 : -1;
    if (x->cosine != y->cosine) return x->cosine < y->cosine ? 1 : -1;
    return (int)(x->line1 - x->line0) - (int)(y->line1 - y->line0);
}

static void on_code_search(int c, const char *body) {
    char query[8192] = "", path[4096] = "";
    if (!jstr(body, "query", query, sizeof query) && !jstr(body, "input", query, sizeof query)) {
        http(c, 400, "{\"error\":\"query\"}");
        return;
    }
    if (!query[0]) {
        http(c, 400, "{\"error\":\"empty query\"}");
        return;
    }
    int limit = 15;
    jint(body, "limit", &limit);
    jint(body, "top_k", &limit);
    if (limit < 1) limit = 1;
    if (limit > 64) limit = 64;
    uint64_t t0 = nsec();
    Lpm *ix = ensure_code_ix();
    if (!ix) {
        http(c, 500, "{\"error\":\"code index\"}");
        return;
    }
    LpmStats st = {0};
    lpm_stats(ix, &st);
    if (jstr(body, "path", path, sizeof path) && path[0] && os_exists(path)) {
        lpm_index_path(ix, path);
    } else if (st.documents == 0 && os_exists(".")) {
        lpm_index_path(ix, ".");
    }
    lpm_stats(ix, &st);

    /* Semantic: the configured collections' nearest chunks join the lexical candidates; failure is an error. */
    int semantic = sem_enabled();
    SemHit *sh = NULL;
    size_t ns = 0;
    double sem_ms = 0;
    if (semantic) {
        char err[1024] = "", ee[2200];
        uint64_t s0 = nsec();
        const char *pc = getenv("LAPLACE_CODE_SEMANTIC_CANDIDATES");
        size_t per = pc && atoi(pc) > 0 ? (size_t)atoi(pc) : (size_t)limit * 3;
        if (sem_search(query, per, &sh, &ns, err, sizeof err)) {
            json_esc(err, ee, sizeof ee);
            char out[2400];
            snprintf(out, sizeof out, "{\"error\":\"semantic retrieval failed: %s\"}", ee);
            http(c, 502, out);
            return;
        }
        sem_ms = (double)(nsec() - s0) / 1e6;
    }

    size_t want = semantic ? (size_t)limit * 3 : (size_t)limit;
    LpmHit hits[192];
    size_t nh = lpm_search(ix, query, strlen(query), hits, want);
    CodeCand *cand = calloc(nh + ns + 1, sizeof *cand);
    if (!cand) {
        sem_free(sh, ns);
        http(c, 500, "{\"error\":\"out of memory\"}");
        return;
    }
    size_t nc = 0;
    for (size_t i = 0; i < nh; i++) {
        CodeCand *k = &cand[nc++];
        snprintf(k->path, sizeof k->path, "%s", hits[i].path);
        k->line0 = hits[i].line0; k->line1 = hits[i].line1;
        k->lexical = 1;
        memcpy(k->id, hits[i].id, 16);
        k->hilbert = hits[i].hilbert;
        k->shared = hits[i].shared;
        k->cosine = -1;
    }
    for (size_t i = 0; i < ns; i++) {
        char abs[4096];
        resolve_label(sh[i].path, abs, sizeof abs);
        CodeCand *same = NULL;
        for (size_t j = 0; j < nc && !same; j++)
            if (!strcmp(cand[j].path, abs) && cand[j].line0 <= sh[i].line1 && sh[i].line0 <= cand[j].line1)
                same = &cand[j];
        if (same) {    /* the same lines: keep the smaller span, remember the best cosine */
            if (sh[i].cosine > same->cosine) { same->cosine = sh[i].cosine; same->semantic = (int)i + 1; }
            if (!same->lexical && sh[i].line1 - sh[i].line0 < same->line1 - same->line0) {
                same->line0 = sh[i].line0; same->line1 = sh[i].line1;
            }
            continue;
        }
        CodeCand *k = &cand[nc++];
        snprintf(k->path, sizeof k->path, "%s", abs);
        k->line0 = sh[i].line0; k->line1 = sh[i].line1;
        k->semantic = (int)i + 1;
        k->cosine = sh[i].cosine;
    }

    /* Laplace orders the union: rerank of each candidate's text against the query. */
    for (size_t i = 0; i < nc; i++) {
        CodeCand *k = &cand[i];
        char snip[8192] = "";
        if (k->lexical) read_lines_snippet(k->path, k->line0, k->line1, snip, sizeof snip);
        if (!snip[0] && k->semantic && os_exists(k->path)) read_lines_snippet(k->path, k->line0, k->line1, snip, sizeof snip);
        if (!snip[0] && k->semantic) snprintf(snip, sizeof snip, "%s", sh[k->semantic - 1].text);
        k->snippet = strdup(snip);
        k->frechet = -1.0;
        k->rerank = snip[0] ? lpm_rerank(ix, query, strlen(query), snip, strlen(snip), &k->shared, &k->frechet) : 0.0;
        if (!k->lexical) {
            LpmRec r = {0};
            lpm_identify(ix, snip, strlen(snip), &r);
            memcpy(k->id, r.id, 16);
            k->hilbert = r.hilbert;
        }
    }
    if (semantic) qsort(cand, nc, sizeof *cand, cand_cmp);
    size_t shown = nc < (size_t)limit ? nc : (size_t)limit;
    double ms = (double)(nsec() - t0) / 1e6;
    char *out = NULL;
    size_t un = 0, ucap = 0;
    buf_fmt(&out, &un, &ucap,
            "{\"docs\":%llu,\"entities\":%llu,\"ms\":%.3f,\"semantic\":%s,\"semantic_ms\":%.3f,"
            "\"candidates\":{\"lexical\":%zu,\"semantic\":%zu,\"union\":%zu},\"hits\":[",
            (unsigned long long)st.documents, (unsigned long long)st.entities, ms, semantic ? "true" : "false",
            sem_ms, nh, ns, nc);
    for (size_t i = 0; i < shown; i++) {
        CodeCand *k = &cand[i];
        char id[33], pe[8192], se[24576], ce[300] = "";
        static const char H[] = "0123456789abcdef";
        for (int b = 0; b < 16; b++) {
            id[b * 2] = H[k->id[b] >> 4];
            id[b * 2 + 1] = H[k->id[b] & 15];
        }
        id[32] = 0;
        json_esc(k->path, pe, sizeof pe);
        json_esc(k->snippet ? k->snippet : "", se, sizeof se);
        if (k->semantic) json_esc(sh[k->semantic - 1].collection, ce, sizeof ce);
        const char *src = k->lexical && k->semantic ? "both" : k->lexical ? "lexical" : "semantic";
        buf_fmt(&out, &un, &ucap,
                "%s{\"rank\":%zu,\"path\":\"%s\",\"line0\":%u,\"line1\":%u,\"source\":\"%s\","
                "\"id\":\"%s\",\"hilbert\":\"%016llx\",\"shared\":%u,\"frechet\":%.6f,\"rerank\":%.6f",
                i ? "," : "", i + 1, pe, k->line0, k->line1, src, id, (unsigned long long)k->hilbert, k->shared,
                k->frechet, k->rerank);
        if (k->semantic) buf_fmt(&out, &un, &ucap, ",\"cosine\":%.6f,\"collection\":\"%s\"", k->cosine, ce);
        buf_fmt(&out, &un, &ucap, ",\"snippet\":\"%s\"}", se);
    }
    buf_add(&out, &un, &ucap, "]}", 2);
    for (size_t i = 0; i < nc; i++) free(cand[i].snippet);
    free(cand);
    sem_free(sh, ns);
    http(c, 200, out ? out : "{\"hits\":[]}");
    free(out);
}
static void on_reindex(int c, const char *body) {
    char path[4096] = "";
    jstr(body, "path", path, sizeof path);
    uint64_t t0 = nsec();
    Lpm *ix = ensure_code_ix();
    if (!ix) {
        http(c, 500, "{\"error\":\"code index\"}");
        return;
    }
    LpmUpdate u = {0};
    int rebuilt = 0;
    if (path[0]) {
        if (!os_exists(path)) {
            http(c, 400, "{\"error\":\"path does not exist\"}");
            return;
        }
        lpm_update_path(CODE_IX, path, &u);
        const char *rec = getenv("LAPLACE_CODE_RECORDS");
        if (rec && *rec && (u.changed || u.added || u.removed) && lpm_save(CODE_IX, rec)) {
            http(c, 500, "{\"error\":\"records not saved\"}");
            return;
        }
    } else if (code_refresh(&u, &rebuilt)) {
        http(c, 500, "{\"error\":\"code index refresh\"}");
        return;
    }
    LpmStats st = {0};
    lpm_stats(CODE_IX, &st);
    double ms = (double)(nsec() - t0) / 1e6;
    char out[512];
    snprintf(out, sizeof out,
             "{\"status\":\"indexed\",\"unchanged\":%llu,\"changed\":%llu,\"added\":%llu,\"removed\":%llu,"
             "\"rebuilt\":%s,\"files\":%llu,\"docs\":%llu,\"stale\":%llu,\"entities\":%llu,\"ms\":%.3f}",
             (unsigned long long)u.unchanged, (unsigned long long)u.changed, (unsigned long long)u.added,
             (unsigned long long)u.removed, rebuilt ? "true" : "false", (unsigned long long)st.files,
             (unsigned long long)st.documents, (unsigned long long)st.stale, (unsigned long long)st.entities, ms);
    http(c, 200, out);
}

typedef struct {
    uint32_t index;
    uint32_t shared;
    double frechet;
    double relevance_score;
} RerankItem;

static int cmp_rerank_item(const void *a, const void *b) {
    const RerankItem *ra = (const RerankItem *)a;
    const RerankItem *rb = (const RerankItem *)b;
    if (ra->relevance_score > rb->relevance_score) return -1;
    if (ra->relevance_score < rb->relevance_score) return 1;
    return (ra->index > rb->index) - (ra->index < rb->index);
}

static void on_rerank(int c, const char *body) {
    char query[8192] = "";
    if (!jstr(body, "query", query, sizeof query) && !jstr(body, "input", query, sizeof query)) {
        http(c, 400, "{\"error\":\"query\"}");
        return;
    }
    char *docs[256];
    size_t nd = parse_str_array(body, "documents", docs, 256);
    if (!nd) {
        http(c, 400, "{\"error\":\"documents\"}");
        return;
    }
    int top_n = (int)nd;
    if (jint(body, "top_n", &top_n) && (top_n < 1 || (size_t)top_n > nd)) top_n = (int)nd;
    uint64_t t0 = nsec();
    Lpm *ix = ensure_code_ix();
    if (!ix) {
        for (size_t i = 0; i < nd; i++) free(docs[i]);
        http(c, 500, "{\"error\":\"tier0\"}");
        return;
    }
    RerankItem items[256];
    size_t qlen = strlen(query);
    for (size_t i = 0; i < nd; i++) {
        items[i].index = (uint32_t)i;
        items[i].shared = 0;
        items[i].frechet = -1.0;
        items[i].relevance_score = lpm_rerank(ix, query, qlen, docs[i], strlen(docs[i]), &items[i].shared, &items[i].frechet);
        free(docs[i]);
    }
    qsort(items, nd, sizeof(RerankItem), cmp_rerank_item);
    size_t nh = (size_t)top_n < nd ? (size_t)top_n : nd;
    double ms = (double)(nsec() - t0) / 1e6;
    char *out = NULL;
    size_t un = 0, ucap = 0;
    buf_fmt(&out, &un, &ucap, "{\"model\":\"laplace-frechet4d\",\"ms\":%.3f,\"results\":[", ms);
    for (size_t i = 0; i < nh; i++) {
        buf_fmt(&out, &un, &ucap,
                "%s{\"index\":%u,\"relevance_score\":%.6f,\"shared\":%u,\"frechet\":%.6f}",
                i ? "," : "", items[i].index, items[i].relevance_score, items[i].shared, items[i].frechet);
    }
    buf_add(&out, &un, &ucap, "]}", 2);
    http(c, 200, out ? out : "{\"results\":[]}");
    free(out);
}

static const char *SURFACE =
    "{\"service\":\"Laplace-MCP\","
    "\"caller\":\"sends content or an ID; this process holds the database connection and AST/constituent index\","
    "\"endpoints\":["
    "{\"method\":\"POST\",\"path\":\"/v1/embeddings\",\"body\":\"{\\\"input\\\":\\\"text\\\"} or {\\\"input\\\":[\\\"a\\\",\\\"b\\\"]}\",\"does\":\"names the entity from tier 0 and returns OpenAI-compatible 4D coordinate embedding + Laplace ID/Hilbert/parts\"},"
    "{\"method\":\"POST\",\"path\":\"/v1/rerank\",\"body\":\"{\\\"query\\\":\\\"text\\\",\\\"documents\\\":[\\\"a\\\",\\\"b\\\"],\\\"top_n\\\":10}\",\"does\":\"deterministic IDF constituent overlap + 4D Frechet trajectory reranker\"},"
    "{\"method\":\"POST\",\"path\":\"/v1/code_search\",\"body\":\"{\\\"query\\\":\\\"text\\\",\\\"path\\\":\\\"optional\\\",\\\"limit\\\":15}\",\"does\":\"Tree-sitter AST declaration + UAX #29 code search with 4D Frechet trajectory ranking\"},"
    "{\"method\":\"POST\",\"path\":\"/v1/reindex\",\"body\":\"{\\\"path\\\":\\\"optional\\\"}\",\"does\":\"rebuilds the AST/constituent code index\"},"
    "{\"method\":\"POST\",\"path\":\"/v1/search\",\"body\":\"{\\\"input\\\":\\\"text\\\"} or {\\\"id\\\":\\\"32 hex\\\"}; \\\"text\\\":true renders laplace_text\",\"does\":\"one ID into laplace_containers and laplace_claims\"},"
    "{\"method\":\"POST\",\"path\":\"/v1/forward\",\"body\":\"{\\\"input\\\":\\\"prompt\\\"}\",\"does\":\"laplace_forward on the prompt constituents\"},"
    "{\"method\":\"POST\",\"path\":\"/v1/shape\",\"body\":\"{\\\"input\\\":\\\"text\\\",\\\"other\\\":\\\"text\\\"}\",\"does\":\"laplace_frechet4d on stored constituent coordinates\"},"
    "{\"method\":\"POST\",\"path\":\"/v1/ingest\",\"body\":\"{\\\"path\\\":\\\"/absolute/path\\\"}\",\"does\":\"laplace ingest: decompose, deduplicate, record, attest\"},"
    "{\"method\":\"GET\",\"path\":\"/v1/surface\"},"
    "{\"method\":\"POST\",\"path\":\"/mcp\",\"body\":\"JSON-RPC 2.0 MCP or op: embeddings|rerank|code_search|reindex|search|forward|shape|ingest|surface\"}"
    "],"
    "\"database_functions\":[\"laplace_containers\",\"laplace_claims\",\"laplace_claims_each\",\"laplace_text\",\"laplace_fills\",\"laplace_forward\",\"laplace_frechet4d\",\"laplace_dtw4d\",\"laplace_edr4d\",\"laplace_distance4d\",\"laplace_couple\",\"laplace_follows\",\"laplace_paths\",\"laplace_attested\",\"laplace_compose\",\"laplace_parts\"],"
    "\"engine_commands\":[\"tier0\",\"flags\",\"highway\",\"deploy\",\"sources\",\"ingest\",\"forget\",\"sweep\",\"index\",\"structure\",\"tree\",\"text\",\"pull\",\"turn\",\"hop\",\"translate\",\"degrees\",\"fills\",\"status\",\"bench\",\"model\"]}";

/* ---- code from the database (Laplace-MCP#9): what an ingested source's files hold, read off the containment.
 * A file is a trunk [metadata, content]; a recipe with a grammar records its content as the syntax tree, so the nodes
 * above a name are its enclosing expressions, statements and declarations, and above them the file. Nothing is read
 * from a working tree: the path is the file's own pathname record, the text and lines are the recorded content. */
typedef struct Reader Reader;
Reader *reader_new(PGconn *pg);
void reader_free(Reader *);
char *reader_text(Reader *, const lp_id *id, size_t limit);
size_t reader_parts(Reader *, const lp_id *id, lp_id *out, size_t cap);
Hold *holds_any(const lp_id *keys, int nkeys, int *nout);

#define CW_DOWN 8
typedef struct { lp_id id; size_t down[CW_DOWN]; uint8_t nd, file, claim; } CwNode;   /* down: what it was reached from, toward the name */
typedef struct { CwNode *n; size_t nn, cn; lp_idmap *m; } CwDag;
static size_t cw_node(CwDag *d, const lp_id *id, int *fresh) {
    bool f;
    size_t i = lp_idmap_put(d->m, id, &f);
    if (f) {
        if (d->nn == d->cn) { d->cn = d->cn ? d->cn * 2 : 1024; CwNode *g = realloc(d->n, d->cn * sizeof *d->n); if (!g) abort(); d->n = g; }
        memset(&d->n[d->nn], 0, sizeof d->n[d->nn]);
        d->n[d->nn].id = *id;
        d->nn++;
    }
    if (fresh) *fresh = f;
    return i;
}
static void cw_link(CwDag *d, size_t up, size_t down) {
    CwNode *x = &d->n[up];
    for (int k = 0; k < x->nd; k++) if (x->down[k] == down) return;
    if (x->nd < CW_DOWN) x->down[x->nd++] = down;
}
/* A file's path from the OS's record in its metadata tree: [pathname, its folders], or [filename, NAME]. */
static char *cw_file_name(Reader *rd, const lp_id *file) {
    lp_id at = *file, kid[64];
    for (int depth = 0; depth < 8; depth++) {
        size_t n = reader_parts(rd, &at, kid, 64);
        if (!n) break;
        for (size_t i = 0; i < n && i < 64; i++) {
            lp_id two[4];
            size_t m = reader_parts(rd, &kid[i], two, 4);
            if (m != 2) continue;
            char *k = reader_text(rd, &two[0], 32);
            int path = !strcmp(k, "pathname"), name = !strcmp(k, "filename");
            free(k);
            if (name) return reader_text(rd, &two[1], 300);
            if (path) {
                lp_id seg[128];
                size_t ns = reader_parts(rd, &two[1], seg, 128);
                char *o = calloc(1, 4096);
                size_t ol = 0;
                for (size_t z = 0; z < ns && z < 128 && ol < 4000; z++) {
                    char *t = reader_text(rd, &seg[z], 300);
                    ol += (size_t)snprintf(o + ol, 4096 - ol, "/%s", t);
                    free(t);
                }
                return o;
            }
        }
        at = kid[0];
    }
    return strdup("");
}

/* Up from one name through every path that holds it, to the files: a holder whose first vertex is metadata, reached
 * through its content, is a file and the walk stops there. A name held only as metadata (a file called so) is not an
 * occurrence. What holds the name as a claim is what a recipe said of it: a code recipe says [kind, name] of each
 * declaration (Laplace-Engine attest_code_node), so a file reached through one defines the name. A record of one claim is
 * that claim, said as a record (LP_SAID_RECORD) where it stands, so a holder of the name said so one level up is a claim
 * too, as the engine's walk up counts it (walk_up): a file that declares one thing defines it. The front is bounded, so a
 * name as common as a keyword answers what it reached and says so. */
static int cw_walk(const lp_id *name, CwDag *d, size_t max_nodes, int *levels, int *cut) {
    d->m = lp_idmap_new();
    cw_node(d, name, NULL);
    lp_idmap *front = lp_idmap_new();
    lp_idmap_put(front, name, NULL);
    lp_vec(lp_vertex) vx = { 0 };
    int level = 0, files = 0;
    *cut = 0;
    while (lp_idmap_count(front) && level < 128) {
        int nh = 0;
        Hold *h = holds_any(lp_idmap_keys(front), (int)lp_idmap_count(front), &nh);
        lp_idmap *next = lp_idmap_new();
        for (int j = 0; j < nh && !*cut; j++) {
            size_t nv = lp_path_vertices(h[j].path, (size_t)h[j].path_len, NULL, 0);
            if (!nv) continue;
            lp_vec_reserve(&vx, nv);
            lp_path_vertices(h[j].path, (size_t)h[j].path_len, vx.v, nv);
            int is_file = vx.v[0].said == LP_SAID_METADATA;
            for (size_t v = 0; v < nv; v++) {
                if (vx.v[v].said == LP_SAID_METADATA) continue;
                if (lp_idmap_find(front, &vx.v[v].id) < 0) continue;
                ptrdiff_t ci = lp_idmap_find(d->m, &vx.v[v].id);
                if (ci < 0) continue;
                if (d->nn >= max_nodes) { *cut = 1; break; }
                int fresh = 0;
                size_t pi = cw_node(d, &h[j].entity, &fresh);
                cw_link(d, pi, (size_t)ci);
                if (vx.v[v].said == LP_SAID_CLAIM || (level == 1 && vx.v[v].said == LP_SAID_RECORD)) d->n[ci].claim = 1;   /* said as a claim, or as a claim's own record, by what holds it */
                if (fresh && is_file) { d->n[pi].file = 1; files++; }
                else if (fresh) lp_idmap_put(next, &h[j].entity, NULL);
            }
        }
        holds_free(h, nh);
        lp_idmap_free(front);
        front = next;
        level++;
        if (*cut) break;
    }
    lp_vec_free(&vx);
    lp_idmap_free(front);
    *levels = level;
    return files;
}

static size_t cw_lines(const char *text, size_t upto) {
    size_t l = 1;
    for (size_t i = 0; i < upto && text[i]; i++) if (text[i] == '\n') l++;
    return l;
}

/* The paths down from a file to the name (index 0), each its nodes file-first; at most cap of them. */
typedef struct { size_t n[160]; size_t len; } CwPath;
static void cw_paths(const CwDag *d, size_t at, CwPath *cur, CwPath *out, size_t *nout, size_t cap, size_t *steps) {
    if (*nout >= cap || cur->len >= 160 || ++*steps > 20000) return;
    cur->n[cur->len++] = at;
    if (at == 0) { out[(*nout)++] = *cur; cur->len--; return; }
    for (int k = 0; k < d->n[at].nd; k++) cw_paths(d, d->n[at].down[k], cur, out, nout, cap, steps);
    cur->len--;
}

static void cw_str(char **out, size_t *on, size_t *ocap, const char *s) {
    size_t l = strlen(s) * 6 + 8;
    char *e = malloc(l);
    buf_add(out, on, ocap, json_esc(s, e, l), strlen(e));
    free(e);
}

typedef struct { size_t file; int defines, occ; char *path; } CwFile;
static int cw_file_cmp(const void *a, const void *b) {
    const CwFile *x = a, *y = b;
    if (x->defines != y->defines) return y->defines - x->defines;
    if (x->occ != y->occ) return y->occ - x->occ;
    return strcmp(x->path, y->path);
}

static void on_code_where(int c, const char *body) {
    char name[2048];
    /* query, as code_search takes it (a tools/call's own "name" comes first in its body, so the key is not "name") */
    if ((!jstr(body, "query", name, sizeof name) || !name[0]) && (!jstr(body, "input", name, sizeof name) || !name[0])) {
        http(c, 400, "{\"error\":\"query\"}");
        return;
    }
    int limit = 20, context = 120, per = 3;
    jint(body, "limit", &limit);
    jint(body, "context", &context);
    jint(body, "occurrences", &per);
    if (limit < 1) limit = 1;
    if (limit > 200) limit = 200;
    if (context < 20) context = 20;
    if (context > 4000) context = 4000;
    if (per < 0) per = 0;
    if (per > 16) per = 16;
    char under[512] = "";
    jstr(body, "path", under, sizeof under);
    int defs_only = jbool(body, "defines", 0);
    if (!defs_only && strstr(body, "\"defines\":true")) defs_only = 1;
    lp_ref trunk, parts[256];
    size_t np = 0;
    if (!name_entity(name, &trunk, parts, 256, &np)) { http(c, 500, "{\"error\":\"tier0\"}"); return; }
    PGconn *pg = db();
    if (!pg) {
        char e[512], b[640];
        json_esc(PQerrorMessage(PG), e, sizeof e);
        snprintf(b, sizeof b, "{\"error\":\"database\",\"detail\":\"%s\"}", e);
        http(c, 503, b);
        return;
    }
    uint64_t t0 = nsec();
    CwDag d = { 0 };
    int levels = 0, cut = 0;
    int nfiles = cw_walk(&trunk.id, &d, 400000, &levels, &cut);
    double walk_ms = (double)(nsec() - t0) / 1e6;
    Reader *rd = reader_new(pg);
    /* each file: whether a claim path reaches it (it defines the name), and how many text paths */
    CwFile *fs = calloc((size_t)nfiles + 1, sizeof *fs);
    size_t nf = 0;
    CwPath *paths = malloc(sizeof(CwPath) * 32), cur;
    for (size_t i = 0; i < d.nn; i++) {
        if (!d.n[i].file) continue;
        char *path = cw_file_name(rd, &d.n[i].id);
        if (under[0] && !strstr(path, under)) { free(path); continue; }
        size_t npth = 0, steps = 0;
        cur.len = 0;
        cw_paths(&d, i, &cur, paths, &npth, 32, &steps);
        int def = 0, occ = 0;
        for (size_t k = 0; k < npth; k++) {
            if (paths[k].len >= 2 && d.n[paths[k].n[paths[k].len - 2]].claim) def = 1; else occ++;
        }
        if (defs_only && !def) { free(path); continue; }
        fs[nf++] = (CwFile){ i, def, occ, path };
    }
    qsort(fs, nf, sizeof *fs, cw_file_cmp);
    char idh[33];
    hex_id(&trunk.id, idh);
    char *out = NULL;
    size_t on = 0, ocap = 0;
    buf_add(&out, &on, &ocap, "{\"query\":\"", 10);
    cw_str(&out, &on, &ocap, name);
    buf_fmt(&out, &on, &ocap, "\",\"id\":\"%s\",\"database\":\"%s\",\"levels\":%d,\"nodes\":%zu,\"files\":%zu,\"cut\":%s,\"walk_ms\":%.3f,\"hits\":[",
            idh, PQdb(pg), levels, d.nn, nf, cut ? "true" : "false", walk_ms);
    int shown = 0;
    for (size_t f = 0; f < nf && shown < limit; f++) {
        size_t i = fs[f].file;
        size_t npth = 0, steps = 0;
        cur.len = 0;
        cw_paths(&d, i, &cur, paths, &npth, 32, &steps);
        char fh[33];
        hex_id(&d.n[i].id, fh);
        buf_fmt(&out, &on, &ocap, "%s{\"path\":\"", shown ? "," : "");
        cw_str(&out, &on, &ocap, fs[f].path);
        buf_fmt(&out, &on, &ocap, "\",\"file\":\"%s\",\"defines\":[", fh);
        int nk = 0;
        char *whole = NULL;
        for (size_t k = 0; k < npth; k++) {                 /* the kinds said of it: [kind, name], the node above the name */
            size_t z = paths[k].len >= 2 ? paths[k].len - 2 : 0;
            if (!d.n[paths[k].n[z]].claim) continue;
            lp_id two[4];
            if (reader_parts(rd, &d.n[paths[k].n[z]].id, two, 4) != 2) continue;
            char *kind = reader_text(rd, &two[0], 80);
            buf_add(&out, &on, &ocap, nk ? ",\"" : "\"", nk ? 2 : 1);
            cw_str(&out, &on, &ocap, kind);
            buf_add(&out, &on, &ocap, "\"", 1);
            free(kind);
            nk++;
        }
        buf_fmt(&out, &on, &ocap, "],\"occurrences\":%d,\"at\":[", fs[f].occ);
        int no = 0;
        long seen[16];
        for (size_t k = 0; k < npth && no < per; k++) {
            if (paths[k].len < 3 || d.n[paths[k].n[paths[k].len - 2]].claim) continue;
            const CwPath *pp = &paths[k];
            /* the innermost node of at least `context` bytes, and the outermost under 8 KiB below the file */
            char *ctx = NULL, *encl = NULL, *span = NULL, *inner = NULL;
            size_t ctx_at = 0;
            for (size_t z = pp->len - 1; z-- > 1;) {
                char *tx = reader_text(rd, &d.n[pp->n[z]].id, (size_t)context * 4);
                if (strlen(tx) >= (size_t)context || z == 1) { ctx = tx; ctx_at = z; break; }
                free(tx);
            }
            for (size_t z = 2; z < pp->len - 1; z++) {
                char *tx = reader_text(rd, &d.n[pp->n[z]].id, 8193);
                if (strlen(tx) <= 8192) { encl = tx; break; }
                free(tx);
            }
            /* lines: the enclosing declaration (else the context), whole, where it stands in the file's recorded
             * content; the name where the context stands in it */
            long line = -1, line0 = -1, line1 = -1;
            if (ctx_at) inner = reader_text(rd, &d.n[pp->n[ctx_at]].id, (size_t)1 << 20);
            span = encl ? strdup(encl) : inner ? strdup(inner) : NULL;
            if (span && span[0]) {
                if (!whole) whole = reader_text(rd, &d.n[pp->n[1]].id, (size_t)8 << 20);
                const char *at = strstr(whole, span);
                if (at) {
                    size_t len = strlen(span);
                    line0 = (long)cw_lines(whole, (size_t)(at - whole));
                    line1 = line0 + (long)cw_lines(at, len) - 1;
                    const char *in = inner && inner[0] ? strstr(at, inner) : NULL;
                    if (!in || (size_t)(in - at) >= len) in = at;
                    const char *nm = strstr(in, name);
                    if (nm && (size_t)(nm - at) < len) line = line0 + (long)cw_lines(at, (size_t)(nm - at)) - 1;
                }
            }
            free(span);
            free(inner);
            int dup = 0;
            for (int q = 0; q < no; q++) if (seen[q] == line && line > 0) dup = 1;
            if (!dup) {
                seen[no] = line;
                buf_add(&out, &on, &ocap, no ? ",{" : "{", no ? 2 : 1);
                if (line > 0) buf_fmt(&out, &on, &ocap, "\"line\":%ld", line); else buf_add(&out, &on, &ocap, "\"line\":null", 11);
                if (line0 > 0) buf_fmt(&out, &on, &ocap, ",\"line0\":%ld,\"line1\":%ld", line0, line1);
                buf_fmt(&out, &on, &ocap, ",\"depth\":%zu", pp->len - 2);
                if (getenv("LPM_CODE_WHERE_TRACE"))
                    for (size_t z = 1; z < pp->len; z++) { char *tx = reader_text(rd, &d.n[pp->n[z]].id, 60); fprintf(stderr, "  %zu %s%s\n", z, d.n[pp->n[z]].claim ? "(claim) " : "", tx); free(tx); }
                buf_add(&out, &on, &ocap, ",\"context\":\"", 12);
                if (ctx) cw_str(&out, &on, &ocap, ctx);
                buf_add(&out, &on, &ocap, "\",\"enclosing\":\"", 15);
                if (encl) {                                 /* its head: its first line that is not blank, at most 240 bytes */
                    char *hd = encl + strspn(encl, " \t\r\n");
                    size_t hl = strcspn(hd, "\r\n");
                    if (hl > 240) { hl = 240; while (hl && ((uint8_t)hd[hl] & 0xC0) == 0x80) hl--; }
                    hd[hl] = 0;
                    cw_str(&out, &on, &ocap, hd);
                }
                buf_add(&out, &on, &ocap, "\"}", 2);
                no++;
            }
            free(ctx);
            free(encl);
        }
        free(whole);
        buf_add(&out, &on, &ocap, "]}", 2);
        shown++;
    }
    buf_fmt(&out, &on, &ocap, "],\"shown\":%d,\"ms\":%.3f}", shown, (double)(nsec() - t0) / 1e6);
    for (size_t f = 0; f < nf; f++) free(fs[f].path);
    free(fs);
    free(paths);
    reader_free(rd);
    lp_idmap_free(d.m);
    free(d.n);
    http(c, 200, out ? out : "{\"error\":\"memory\"}");
    free(out);
}
/* The sources the database holds: each witness, its name (the first part of its trunk, the record its source file
 * names) and its trust. */
static void on_sources(int c, const char *body) {
    (void)body;
    PGconn *pg = db();
    if (!pg) { http(c, 503, "{\"error\":\"database\"}"); return; }
    PGresult *r = PQexecParams(pg, "SELECT id, trust FROM witness", 0, NULL, NULL, NULL, NULL, 1);
    if (PQresultStatus(r) != PGRES_TUPLES_OK) {
        char e[512], b[640];
        json_esc(PQerrorMessage(pg), e, sizeof e);
        snprintf(b, sizeof b, "{\"error\":\"witness\",\"detail\":\"%s\"}", e);
        PQclear(r);
        http(c, 500, b);
        return;
    }
    Reader *rd = reader_new(pg);
    char *out = NULL;
    size_t on = 0, ocap = 0;
    buf_fmt(&out, &on, &ocap, "{\"database\":\"%s\",\"sources\":[", PQdb(pg));
    for (int i = 0; i < PQntuples(r); i++) {
        lp_id id;
        memcpy(id.b, PQgetvalue(r, i, 0), 16);
        double trust;
        uint64_t tb = lp_be(PQgetvalue(r, i, 1), 8);
        memcpy(&trust, &tb, 8);
        lp_id first[2];
        size_t m = reader_parts(rd, &id, first, 2);
        char *nm = m ? reader_text(rd, &first[0], 200) : strdup("");
        char idh[33], e[1300];
        hex_id(&id, idh);
        buf_fmt(&out, &on, &ocap, "%s{\"id\":\"%s\",\"name\":\"%s\",\"trust\":%.3f}", i ? "," : "", idh, json_esc(nm, e, sizeof e), trust);
        free(nm);
    }
    buf_add(&out, &on, &ocap, "]}", 2);
    PQclear(r);
    reader_free(rd);
    http(c, 200, out);
    free(out);
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
    "{\"name\":\"code_search\",\"description\":\"Search code and documentation across indexed repositories using Laplace Tree-sitter AST declaration chunking, UAX #29 segmentation, IDF constituent postings, and 4D Frechet trajectory distance. With LAPLACE_CODE_SEMANTIC set, nearest chunks from the configured embedding endpoint and Qdrant collections join the candidates and Laplace rerank orders the union; a configured service that fails is an error. Returns matching files, exact line ranges (line0..line1), source (lexical, semantic or both), Laplace entity ID, Hilbert index, shared constituent count, Frechet distance, rerank score, cosine for semantic hits, and code snippet.\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"query\":{\"type\":\"string\"},\"path\":{\"type\":\"string\"},\"limit\":{\"type\":\"integer\"}},\"required\":[\"query\"]}},"
    "{\"name\":\"code_where\",\"description\":\"Where a name or any exact span of code (query) stands in the code the Laplace database holds (an ingested source such as workspace-code), read off the containment above it, nothing from a working tree. Each file that holds it, by the pathname its record gives: defines, the declaration kinds its recipe said of the name there (function_definition, class_declaration...), files that define it first; occurrences, how many places in its syntax tree hold it; at, up to occurrences (default 3) of them: the line, the innermost enclosing node of at least context bytes (default 120), and the enclosing declaration's first line with its span line0..line1. defines true: only files that define it. path: a substring the file path must contain.\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"query\":{\"type\":\"string\"},\"path\":{\"type\":\"string\"},\"defines\":{\"type\":\"boolean\"},\"limit\":{\"type\":\"integer\"},\"occurrences\":{\"type\":\"integer\"},\"context\":{\"type\":\"integer\"}},\"required\":[\"query\"]}},"
    "{\"name\":\"sources\",\"description\":\"The sources the Laplace database holds: each witness ID, its name and trust.\",\"inputSchema\":{\"type\":\"object\",\"properties\":{},\"required\":[]}},"
    "{\"name\":\"reindex\",\"description\":\"Bring the code index up to date with LAPLACE_CODE_ROOTS, or with one file or directory given as path: unchanged files (size and time, else content) are not read, changed and new files are admitted, files no longer there are removed. Returns the counts. The index also refreshes itself at most every LAPLACE_CODE_REFRESH_SECONDS.\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"}},\"required\":[]}},"
    "{\"name\":\"rerank\",\"description\":\"Deterministic Laplace reranker (drop-in replacement for neural cross-encoders). Scores candidate documents against a query using IDF-weighted constituent overlap and 4D discrete Frechet trajectory distance.\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"query\":{\"type\":\"string\"},\"documents\":{\"type\":\"array\",\"items\":{\"type\":\"string\"}},\"top_n\":{\"type\":\"integer\"}},\"required\":[\"query\",\"documents\"]}},"
    "{\"name\":\"search\",\"description\":\"Name text on the client from tier 0, or take an ID the caller already holds. The trunk is read with holds_above, and so is every constituent whose tier is above a codepoint or a grapheme. A tier-0 constituent stays in the path, in its place, with its codepoint. fan, when sent, caps how many claims and occurrences are returned; omitted, all of them are.\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"input\":{\"type\":\"string\"},\"id\":{\"type\":\"string\"},\"fan\":{\"type\":\"integer\"}},\"required\":[]}},"
    "{\"name\":\"embeddings\",\"description\":\"Same call shape as an embedding endpoint. Returns the Laplace entity ID, tier, 4D coordinate (also exposed as OpenAI-compatible embedding array), Hilbert value, and constituent IDs. Does not touch the database.\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"input\":{\"type\":\"string\"},\"inputs\":{\"type\":\"array\",\"items\":{\"type\":\"string\"}}},\"required\":[]}},"
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
        if (!strcmp(name, "code_search")) on_code_search(c, body);
        else if (!strcmp(name, "reindex")) on_reindex(c, body);
        else if (!strcmp(name, "code_where")) on_code_where(c, body);
        else if (!strcmp(name, "sources")) on_sources(c, body);
        else if (!strcmp(name, "rerank")) on_rerank(c, body);
        else if (!strcmp(name, "search")) on_search(c, body);
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
        else if (!strcmp(op, "rerank")) path = "/v1/rerank";
        else if (!strcmp(op, "code_search")) path = "/v1/code_search";
        else if (!strcmp(op, "reindex")) path = "/v1/reindex";
        else if (!strcmp(op, "code_where")) path = "/v1/code_where";
        else if (!strcmp(op, "sources")) path = "/v1/sources";
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
    if (!strcmp(path, "/v1/embeddings") || !strcmp(path, "/embeddings")) on_embeddings(c, body);
    else if (!strcmp(path, "/v1/rerank") || !strcmp(path, "/rerank")) on_rerank(c, body);
    else if (!strcmp(path, "/v1/code_search")) on_code_search(c, body);
    else if (!strcmp(path, "/v1/reindex")) on_reindex(c, body);
    else if (!strcmp(path, "/v1/code_where")) on_code_where(c, body);
    else if (!strcmp(path, "/v1/sources")) on_sources(c, body);
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

int api_mcp_stdio(void) {
#ifdef _WIN32
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
#endif
    engine_resolve();
    T0 = lp_tier0_map(NULL);
#ifdef _WIN32
    if (!T0) T0 = lp_tier0_map("E:/Data/Laplace/work/modular-client/prepared/tier0.bin");
#endif
    if (!T0) {
        fprintf(stderr, "tier 0 did not map\n");
        return 1;
    }
    TX = lp_text_new(T0);
    if (!TX) return 1;
    size_t cap = 1 << 20;
    char *line = malloc(cap);
    if (!line) return 1;
    while (fgets(line, (int)cap, stdin)) {
        size_t len = strlen(line);
        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) line[--len] = 0;
        if (!len) continue;
        stdio_capture = 1;
        stdio_code = 500;
        free(stdio_body);
        stdio_body = NULL;
        mcp_rpc(-1, line);
        stdio_capture = 0;
        if (stdio_code != 202 && stdio_body && stdio_body[0]) {
            fputs(stdio_body, stdout);
            fputc('\n', stdout);
            fflush(stdout);
        }
    }
    free(stdio_body);
    free(line);
    return 0;
}

int api_serve(const char *host, int port) {
#ifdef _WIN32
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return 1;
#endif
    engine_resolve();
    T0 = lp_tier0_map(NULL);
#ifdef _WIN32
    if (!T0) T0 = lp_tier0_map("E:/Data/Laplace/work/modular-client/prepared/tier0.bin");
#endif
    if (!T0) {
        fprintf(stderr, "tier 0 did not map\n");
        return 1;
    }
    TX = lp_text_new(T0);
    if (!TX) return 1;
    int s = (int)socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) return 1;
    int one = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (const char *)&one, sizeof one);
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
        int c = (int)accept(s, NULL, NULL);
        if (c < 0) continue;
        char *req = malloc(1 << 20);
        if (!req) { close_sock(c); continue; }
        size_t got = 0, rcap = 1 << 20;
        while (got + 1 < rcap) {
            ssize_t r = recv(c, req + got, (int)(rcap - 1 - got), 0);
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
        free(req);
        close_sock(c);
    }
}
