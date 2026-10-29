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
#include "engine.h"
#include "db_bridge.h"
#include <inttypes.h>
#include "semantic.h"

#include "laplace/laplace.h"

#include <ctype.h>
#include <libpq-fe.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

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
    const char *ci = getenv("LAPLACE_CONNINFO");
    if (!ci || !*ci) ci = CONN_DEFAULT;
    if(!PG || PQstatus(PG)!=CONNECTION_OK){
        if(PG){lpm_read_end();PQfinish(PG);}
        PG=PQconnectdb(ci);db_notices(PG);
    }
    if (PQstatus(PG) != CONNECTION_OK) return NULL;
    if(!db_read_snapshot(PG) && !db_read_begin(PG,NULL))return NULL;
    lpm_read_scope(PG,ci);
    return PG;
}

static int capture;
static int capture_code;
static char *capture_body;
static int stdio_capture;
static int stdio_code;
static char *stdio_body;

static void http(int c, int code, const char *body) {
    lpm_read_end();
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
    const char *why = code == 200 ? "OK" : code == 202 ? "Accepted" : code == 400 ? "Bad Request" : code == 401 ? "Unauthorized" : code == 404 ? "Not Found" : "Error";
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
    PGresult *r = db_exec_params(pg, sql, 1, NULL, v, NULL, NULL, 0);
    if (PQresultStatus(r) != PGRES_TUPLES_OK || PQntuples(r) < 1) {
        snprintf(err, errcap, "%s", PQerrorMessage(pg));
        PQclear(r);
        PQclear(db_exec(pg, "ROLLBACK"));
        return 0;
    }
    *out = strdup(PQgetvalue(r, 0, 0));
    PQclear(r);
    return *out != NULL;
}

static int q2(PGconn *pg, const char *sql, const char *a, const char *b, char **out, char *err, size_t errcap) {
    const char *v[2] = { a, b };
    PGresult *r = db_exec_params(pg, sql, 2, NULL, v, NULL, NULL, 0);
    if (PQresultStatus(r) != PGRES_TUPLES_OK || PQntuples(r) < 1) {
        snprintf(err, errcap, "%s", PQerrorMessage(pg));
        PQclear(r);
        PQclear(db_exec(pg, "ROLLBACK"));
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
    "  FROM @extschema@.laplace_forward($1::@extschema@.blake3[], 64) LIMIT 128) s), '[]'::json)::text";

static const char *SHAPE_SQL =
    "SELECT @extschema@.laplace_frechet4d("
    " (SELECT @postgis@.ST_MakeLine(e.coord ORDER BY u.ord) FROM unnest(string_to_array($1, ',')::@extschema@.blake3[])"
    "    WITH ORDINALITY AS u(id, ord) JOIN @extschema@.entity e ON e.id OPERATOR(@extschema@.=) u.id),"
    " (SELECT @postgis@.ST_MakeLine(e.coord ORDER BY u.ord) FROM unnest(string_to_array($2, ',')::@extschema@.blake3[])"
    "    WITH ORDINALITY AS u(id, ord) JOIN @extschema@.entity e ON e.id OPERATOR(@extschema@.=) u.id)"
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
                    "\"rating\":%.3f,\"deviation\":%.3f,\"volatility\":%.6f,\"matches\":%" PRId64 ",\"constituents\":",
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
    PGconn *pg=db();
    if(!pg){
        char escaped[512],error[640];json_esc(PQerrorMessage(PG),escaped,sizeof escaped);
        snprintf(error,sizeof error,"{\"error\":\"database\",\"detail\":\"%s\"}",escaped);http(c,503,error);return;
    }
    int fan = 0;
    jint(body, "fan", &fan);
    int known = np > 0;
    if (!np) {
        for (int i = 0; i < 16; i++) {
            unsigned v = 0;
            sscanf(idbuf + 2 * i, "%2x", &v);
            trunk.id.b[i] = (uint8_t)v;
        }
        int recorded = tier_max(pg, &trunk.id, 1);
        if (recorded >= 0) { trunk.tier = (uint32_t)recorded; known = 1; }
    }
    t0 = nsec();
    int nh = 0;
    int floor = known ? (int)trunk.tier : -1;
    Hold *h = holds_above(pg,&trunk.id, 1, floor, 0, 1, &nh);
    uint8_t ab[40];
    size_t al = ids_param(pg,ab, &trunk.id, 1);
    const char *pv[1] = { (const char *)ab };
    int pl[1] = { (int)al }, pf[1] = { 1 };
    PGresult *self = db_exec_params(pg,
        "SELECT path,tier,mask FROM @extschema@.laplace_physicalities($1::@extschema@.blake3[])",
        1,NULL,pv,pl,pf,1);
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
        int *tiers=malloc(sizeof(int)*(m?m:1));tiers_of(pg,ids,ndown,tiers);
        for (int i = 0; i < ndown; i++) {
            down[i].id=ids[i];down[i].cp=(int)lp_tier0_codepoint(T0,ids+i);
            down[i].tier=down[i].cp>=0?0:tiers[i];
        }
        free(tiers);
        free(ids);
    }
    for (int i = 0; i < ndown; i++) {
        if (down[i].tier < 2) continue;
        down[i].h = holds_above(pg,&down[i].id, 1, down[i].tier, 0, 1, &down[i].nh);
        int nc = 0;
        for (int k = 0; k < down[i].nh; k++) nc += down[i].h[k].claim;
        lp_id *cids2 = malloc(sizeof(lp_id) * (size_t)(nc + 1));
        int n2 = 0;
        cids2[n2++] = down[i].id;
        for (int k = 0; k < down[i].nh; k++) if (down[i].h[k].claim) cids2[n2++] = down[i].h[k].entity;
        down[i].att = attested(pg, cids2, n2, &down[i].natt, NULL);
        free(cids2);
    }
    /* One stored-path frontier and one tier frontier for the entire layer.
     * Duplicate constituent occurrences retain their own Down slots. */
    lp_vec(lp_id) roots={0};lp_vec(int) slots={0};
    for(int i=0;i<ndown;i++)if(down[i].tier>=2){lp_push(&roots,down[i].id);lp_push(&slots,i);}
    int npaths=0;Hold *paths=paths_of(pg,roots.v,(int)roots.n,&npaths);
    lp_vec(lp_id) children={0};
    for(int row=0;row<npaths;row++){
        Down *node=down+slots.v[paths[row].src];
        size_t count=lp_path_ids(paths[row].path,(size_t)paths[row].path_len,NULL,0);
        node->nkids=(int)count;node->kids=malloc(sizeof(lp_id)*(count?count:1));
        node->ktier=malloc(sizeof(int)*(count?count:1));node->kcp=malloc(sizeof(int)*(count?count:1));
        if(count)lp_path_ids(paths[row].path,(size_t)paths[row].path_len,node->kids,count);
        for(size_t k=0;k<count;k++)lp_push(&children,node->kids[k]);
    }
    int *tiers=malloc(sizeof(int)*(children.n?children.n:1));tiers_of(pg,children.v,(int)children.n,tiers);
    size_t offset=0;
    /* paths are ID ordered, so distribute in that same order. */
    for(int row=0;row<npaths;row++){
        Down *node=down+slots.v[paths[row].src];
        for(int k=0;k<node->nkids;k++){
            node->kcp[k]=(int)lp_tier0_codepoint(T0,node->kids+k);
            node->ktier[k]=node->kcp[k]>=0?0:tiers[offset];offset++;
        }
    }
    free(tiers);holds_free(paths,npaths);lp_vec_free(&children);lp_vec_free(&slots);lp_vec_free(&roots);
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
                "\"rating\":%.3f,\"deviation\":%.3f,\"volatility\":%.6f,\"matches\":%" PRId64 ",\"constituents\":",
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
    int lex_rank, rr_rank;          /* where the lexical retrieval and the Laplace rerank put it, from 1; 0: not there */
    int sem_rank[8];                /* where each semantic collection put it, from 1; 0: not there */
    double fused;
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

static int cand_rerank_cmp(const void *a, const void *b) {
    const CodeCand *x = *(CodeCand *const *)a, *y = *(CodeCand *const *)b;
    if (x->rerank != y->rerank) return x->rerank < y->rerank ? 1 : -1;
    if (x->cosine != y->cosine) return x->cosine < y->cosine ? 1 : -1;
    return (int)(x->line1 - x->line0) - (int)(y->line1 - y->line0);
}
static int cand_rerank_cmp_v(const void *a, const void *b) {
    const CodeCand *x = a, *y = b;
    return cand_rerank_cmp(&x, &y);
}
static int cand_cmp(const void *a, const void *b) {
    const CodeCand *x = a, *y = b;
    if (x->fused != y->fused) return x->fused < y->fused ? 1 : -1;
    if (x->rerank != y->rerank) return x->rerank < y->rerank ? 1 : -1;
    if (x->cosine != y->cosine) return x->cosine < y->cosine ? 1 : -1;
    return (int)(x->line1 - x->line0) - (int)(y->line1 - y->line0);
}

/* A weight from the environment: unset keeps *w; a number in [0, 1] replaces it; anything else is 0 (refused). */
static int env_weight(const char *name, double *w) {
    const char *v = getenv(name);
    if (!v || !*v) return 1;
    char *e = NULL;
    double x = strtod(v, &e);
    if (e == v || *e || !(x >= 0.0 && x <= 1.0)) return 0;
    *w = x;
    return 1;
}

/* Query-side vocabulary bridging through the substrate (#19). Each word of the query goes up to its concepts and back
 * down in the same language, as `laplace translate WORD en en` reads it: along the relations the engine's firmware
 * names for translate (a word's sense, the sense's synset, the synset's interlingual concept), its first concepts in
 * the witness's order (`-n`, LAPLACE_CODE_BRIDGE_CONCEPTS, default 2). The words reached are added to the text the
 * lexical retrieval and the Laplace rerank read; the embeddings are asked the query as written. Nothing here names a
 * word: what bridges is what the database's wordnets attest. A word the database has no concept of brings nothing. */
typedef struct { char *text; size_t n, cap; } Bridge;
static int bridge_word_ok(const char *w) {
    size_t l = strlen(w);
    if (l < 2 || l > 48) return 0;
    for (const char *p = w; *p; p++)
        if (!isalpha((unsigned char)*p) && *p != ' ' && *p != '-') return 0;
    return 1;
}
static int bridge_has(const char *list, const char *w) {     /* list: "\na\nb\n" */
    char k[64];
    snprintf(k, sizeof k, "\n%s\n", w);
    return strstr(list, k) != NULL;
}
/* Returns the bridged text (query, then the words reached), or NULL when nothing was reached; info gets a JSON object
 * of each query word to the words it reached. */
static char *bridge_query(const char *query, char *info, size_t icap, double *ms, const char **error) {
    uint64_t t0 = nsec();
    *error = NULL;
    const char *lang = getenv("LAPLACE_CODE_BRIDGE_LANGUAGE"), *nc = getenv("LAPLACE_CODE_BRIDGE_CONCEPTS");
    if (!lang || !*lang) lang = "en";
    char concepts[8];
    snprintf(concepts, sizeof concepts, "%d", nc && atoi(nc) > 0 && atoi(nc) < 100 ? atoi(nc) : 2);
    size_t ql = strlen(query);
    char *out = malloc(ql + 8192), *seen = malloc(ql + 8192);
    if (!out || !seen) { free(out); free(seen); *ms = 0; *error = "{\"error\":\"vocabulary bridge allocation failed\"}"; return NULL; }
    size_t on = (size_t)snprintf(out, ql + 8192, "%s", query), ocap = ql + 8192, in = 0;
    strcpy(seen, "\n");
    in += (size_t)snprintf(info + in, icap - in, "{");
    int words = 0, added = 0, first = 1;
    for (const char *p = query; *p && words < 16;) {
        while (*p && !isalpha((unsigned char)*p)) p++;
        const char *s = p;
        while (*p && isalpha((unsigned char)*p)) p++;
        size_t l = (size_t)(p - s);
        if (l < 3 || l > 40) continue;
        char w[48];
        for (size_t i = 0; i < l; i++) w[i] = (char)tolower((unsigned char)s[i]);
        w[l] = 0;
        if (bridge_has(seen, w)) continue;
        size_t sl = strlen(seen);
        snprintf(seen + sl, ql + 8192 - sl, "%s\n", w);
        words++;
        char *av[] = { (char *)ENGINE, "translate", "-n", concepts, w, (char *)lang, (char *)lang, NULL };
        char *log = NULL;
        int code = 0;
        if (!engine_run(av, &log, &code, 20) || code != 0) {
            free(log); free(out); free(seen);
            *error = "{\"error\":\"vocabulary bridge engine execution failed\"}";
            return NULL;
        }
        int listed = 0;
        char wj[2048];
        size_t wn = 0;
        wj[0] = 0;
        for (char *line = log; line && *line;) {
            char *nl = strchr(line, '\n');
            if (nl) *nl = 0;
            /* a concept's words in a language: four spaces, the language, then "a, b, c" */
            size_t ll = strlen(lang);
            if (!strncmp(line, "    ", 4) && !strncmp(line + 4, lang, ll) && line[4 + ll] == ' ' && !strstr(line, "(nothing attested)")) {
                char *q = line + 4 + ll;
                while (*q) {
                    while (*q == ' ' || *q == ',') q++;
                    char *e = q;
                    while (*e && *e != ',') e++;
                    char syn[64];
                    size_t yl = (size_t)(e - q);
                    while (yl && q[yl - 1] == ' ') yl--;
                    if (yl && yl < sizeof syn) {
                        for (size_t i = 0; i < yl; i++) syn[i] = (char)tolower((unsigned char)q[i]);
                        syn[yl] = 0;
                        if (bridge_word_ok(syn) && !bridge_has(seen, syn) && listed < 8) {
                            size_t sl2 = strlen(seen);
                            if (sl2 + yl + 2 < ql + 8192) snprintf(seen + sl2, ql + 8192 - sl2, "%s\n", syn);
                            if (on + yl + 2 < ocap) on += (size_t)snprintf(out + on, ocap - on, " %s", syn);
                            char se[160];
                            json_esc(syn, se, sizeof se);
                            if (wn + strlen(se) + 4 < sizeof wj) wn += (size_t)snprintf(wj + wn, sizeof wj - wn, "%s\"%s\"", listed ? "," : "", se);
                            listed++;
                            added++;
                        }
                    }
                    q = *e ? e + 1 : e;
                }
            }
            line = nl ? nl + 1 : NULL;
        }
        if (listed && in + wn + 64 < icap) { in += (size_t)snprintf(info + in, icap - in, "%s\"%s\":[%s]", first ? "" : ",", w, wj); first = 0; }
        free(log);
    }
    snprintf(info + in, icap - in, "}");
    free(seen);
    *ms = (double)(nsec() - t0) / 1e6;
    if (!added) { free(out); return NULL; }
    return out;
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

    /* scope: which embedding groups are asked. code: the code collections; docs: the prose ones; all: both. The
     * default is code, prose opt-in: this is a code search, and on the eval (#19) the prose collection never held an
     * answer while taking half the first ten places. Documentation stays reachable through the lexical retrieval and
     * scope docs or all. Without code collections configured the default is all, the prose ones being all there is. */
    char scope_s[8] = "";
    if (!jstr(body, "scope", scope_s, sizeof scope_s) || !scope_s[0]) {
        const char *se = getenv("LAPLACE_CODE_SCOPE"), *cc = getenv("LAPLACE_CODE_COLLECTIONS");
        snprintf(scope_s, sizeof scope_s, "%s", se && *se ? se : cc && *cc ? "code" : "all");
    }
    int scope = !strcmp(scope_s, "code") ? SEM_CODE : !strcmp(scope_s, "docs") ? SEM_PROSE
              : !strcmp(scope_s, "all") ? SEM_CODE | SEM_PROSE : 0;
    if (!scope) {
        http(c, 400, "{\"error\":\"scope is all, code or docs\"}");
        return;
    }
    /* The weight of a ranking in the fusion. A code collection's counts 1. The Laplace rerank, an overlap of the
     * query's constituents with the candidate's, and a prose collection, whose model reads documentation rather than
     * code, count half each by default (LAPLACE_CODE_RERANK_WEIGHT, LAPLACE_CODE_PROSE_WEIGHT; 0 drops the ranking). */
    double w_rr = 0.5, w_prose = 0.5;
    if (!env_weight("LAPLACE_CODE_RERANK_WEIGHT", &w_rr) || !env_weight("LAPLACE_CODE_PROSE_WEIGHT", &w_prose)) {
        http(c, 400, "{\"error\":\"LAPLACE_CODE_RERANK_WEIGHT and LAPLACE_CODE_PROSE_WEIGHT are numbers from 0 to 1\"}");
        return;
    }

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
        if (sem_search(query, per, scope, &sh, &ns, err, sizeof err)) {
            json_esc(err, ee, sizeof ee);
            char out[2400];
            snprintf(out, sizeof out, "{\"error\":\"semantic retrieval failed: %s\"}", ee);
            http(c, 502, out);
            return;
        }
        sem_ms = (double)(nsec() - s0) / 1e6;
    }

    /* bridge (LAPLACE_CODE_BRIDGE): the query's words joined by what the database's wordnets attest of them */
    const char *be = getenv("LAPLACE_CODE_BRIDGE");
    int bridge = jbool(body, "bridge", be && *be && strcmp(be, "0"));
    char binfo[8192] = "{}";
    double bridge_ms = 0;
    const char *bridge_error = NULL;
    char *bridged = bridge ? bridge_query(query, binfo, sizeof binfo, &bridge_ms, &bridge_error) : NULL;
    if (bridge_error) {
        sem_free(sh, ns);
        http(c, 502, bridge_error);
        return;
    }
    const char *rq = bridged ? bridged : query;

    size_t want = semantic ? (size_t)limit * 3 : (size_t)limit;
    LpmHit hits[192];
    size_t nh = lpm_search(ix, rq, strlen(rq), hits, want);
    CodeCand *cand = calloc(nh + ns + 1, sizeof *cand);
    if (!cand) {
        free(bridged);
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
        k->lex_rank = (int)i + 1;
        memcpy(k->id, hits[i].id, 16);
        k->hilbert = hits[i].hilbert;
        k->shared = hits[i].shared;
        k->cosine = -1;
    }
    /* Each collection is a ranking of its own (its model's cosines are not another's): a hit's rank within its collection. */
    const char *coll[8] = { 0 };
    int coll_prose[8] = { 0 };
    int ncoll = 0, *in_coll = calloc(ns + 1, sizeof *in_coll), *crank = calloc(ns + 1, sizeof *crank), seen_in[8] = { 0 };
    for (size_t i = 0; i < ns; i++) {
        int ci = 0;
        while (ci < ncoll && strcmp(coll[ci], sh[i].collection)) ci++;
        if (ci == ncoll && ncoll < 8) { coll_prose[ncoll] = sh[i].prose; coll[ncoll++] = sh[i].collection; }
        if (ci == 8) ci = 7;
        in_coll[i] = ci;
        crank[i] = ++seen_in[ci];
    }
    for (size_t i = 0; i < ns; i++) {
        char abs[4096];
        resolve_label(sh[i].path, abs, sizeof abs);
        CodeCand *same = NULL;
        for (size_t j = 0; j < nc && !same; j++)
            if (!strcmp(cand[j].path, abs) && cand[j].line0 <= sh[i].line1 && sh[i].line0 <= cand[j].line1)
                same = &cand[j];
        if (same) {    /* the same lines: keep the smaller span, remember the best cosine and each collection's best rank */
            int *r = &same->sem_rank[in_coll[i]];
            if (!*r || crank[i] < *r) *r = crank[i];
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
        k->sem_rank[in_coll[i]] = crank[i];
    }
    free(in_coll);
    free(crank);

    /* Laplace orders the union: rerank of each candidate's text against the query. */
    for (size_t i = 0; i < nc; i++) {
        CodeCand *k = &cand[i];
        char snip[8192] = "";
        if (k->lexical) read_lines_snippet(k->path, k->line0, k->line1, snip, sizeof snip);
        if (!snip[0] && k->semantic && os_exists(k->path)) read_lines_snippet(k->path, k->line0, k->line1, snip, sizeof snip);
        if (!snip[0] && k->semantic) snprintf(snip, sizeof snip, "%s", sh[k->semantic - 1].text);
        k->snippet = strdup(snip);
        k->frechet = -1.0;
        k->rerank = snip[0] ? lpm_rerank(ix, rq, strlen(rq), snip, strlen(snip), &k->shared, &k->frechet) : 0.0;

        if (!k->lexical) {
            LpmRec r = {0};
            lpm_identify(ix, snip, strlen(snip), &r);
            memcpy(k->id, r.id, 16);
            k->hilbert = r.hilbert;
        }
    }
    /* The order: reciprocal rank fusion (Cormack, Clarke and Buettcher 2009) of every collection's ranking and the Laplace
     * rerank's, sum 1/(K + rank) over the rankings that hold a candidate. The embeddings' order and Laplace's each count,
     * neither replaces the other, and no score is compared across models. LAPLACE_CODE_FUSION=rerank orders by the
     * Laplace rerank alone, as before. The lexical retrieval brings candidates; the rerank already weighs what they share. */
    char fusion[16] = "";
    if (!jstr(body, "fusion", fusion, sizeof fusion) || !fusion[0]) {
        const char *fe = getenv("LAPLACE_CODE_FUSION");
        snprintf(fusion, sizeof fusion, "%s", fe && *fe ? fe : "rrf");
    }
    int by_rerank = !strcmp(fusion, "rerank");
    if (strcmp(fusion, "rrf") && !by_rerank) {
        for (size_t i = 0; i < nc; i++) free(cand[i].snippet);
        free(cand); sem_free(sh, ns); free(bridged);
        http(c, 400, "{\"error\":\"fusion is rrf or rerank\"}");
        return;
    }
    const double K = 60.0;
    CodeCand **byr = malloc(sizeof *byr * (nc + 1));
    for (size_t i = 0; i < nc; i++) byr[i] = &cand[i];
    qsort(byr, nc, sizeof *byr, cand_rerank_cmp);
    for (size_t i = 0; i < nc; i++) {
        CodeCand *k = byr[i];
        k->rr_rank = (int)i + 1;
        k->fused = w_rr / (K + k->rr_rank);
        for (int ci = 0; ci < ncoll; ci++) if (k->sem_rank[ci]) k->fused += (coll_prose[ci] ? w_prose : 1.0) / (K + k->sem_rank[ci]);
    }
    free(byr);
    if (semantic) qsort(cand, nc, sizeof *cand, by_rerank ? cand_rerank_cmp_v : cand_cmp);   /* lexical alone keeps its retrieval's order */
    int explain = 0;
    explain = jbool(body, "explain", 0);
    size_t shown = explain ? nc : nc < (size_t)limit ? nc : (size_t)limit;
    double ms = (double)(nsec() - t0) / 1e6;
    char *out = NULL;
    size_t un = 0, ucap = 0;
    buf_fmt(&out, &un, &ucap,
            "{\"docs\":%llu,\"entities\":%llu,\"ms\":%.3f,\"semantic\":%s,\"semantic_ms\":%.3f,"
            "\"fusion\":\"%s\",\"scope\":\"%s\",\"weights\":{\"rerank\":%g",
            (unsigned long long)st.documents, (unsigned long long)st.entities, ms, semantic ? "true" : "false",
            sem_ms, semantic && !by_rerank ? "rrf" : "rerank", scope_s, w_rr);
    for (int ci = 0; ci < ncoll; ci++) {
        char cn[300];
        json_esc(coll[ci], cn, sizeof cn);
        buf_fmt(&out, &un, &ucap, ",\"%s\":%g", cn, coll_prose[ci] ? w_prose : 1.0);
    }
    buf_add(&out, &un, &ucap, "}", 1);
    if (bridge) buf_fmt(&out, &un, &ucap, ",\"bridge\":{\"words\":%s,\"ms\":%.3f}", binfo, bridge_ms);
    free(bridged);
    buf_fmt(&out, &un, &ucap, ",\"candidates\":{\"lexical\":%zu,\"semantic\":%zu,\"union\":%zu},\"hits\":[", nh, ns, nc);
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
        buf_fmt(&out, &un, &ucap, ",\"fused\":%.6f,\"ranks\":{\"rerank\":%d", k->fused, k->rr_rank);
        if (k->lex_rank) buf_fmt(&out, &un, &ucap, ",\"lexical\":%d", k->lex_rank);
        for (int ci = 0; ci < ncoll; ci++) if (k->sem_rank[ci]) {
            char cn[300];
            json_esc(coll[ci], cn, sizeof cn);
            buf_fmt(&out, &un, &ucap, ",\"%s\":%d", cn, k->sem_rank[ci]);
        }
        buf_add(&out, &un, &ucap, "}", 1);
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
    "{\"method\":\"POST\",\"path\":\"/v1/code_where\",\"body\":\"{\\\"query\\\":\\\"name or span\\\",\\\"path\\\":\\\"optional\\\",\\\"defines\\\":true}\",\"does\":\"where a name or span of code stands in the code the database holds, read off the containment above it\"},"
    "{\"method\":\"POST\",\"path\":\"/v1/file_outline\",\"body\":\"{\\\"path\\\":\\\"end of a pathname\\\"} or {\\\"file\\\":\\\"32 hex\\\"}\",\"does\":\"the typed declaration claims a code recipe said of a file, in order, with lines\"},"
    "{\"method\":\"POST\",\"path\":\"/v1/code_callers\",\"body\":\"{\\\"query\\\":\\\"declared name\\\",\\\"path\\\":\\\"optional\\\"}\",\"does\":\"the declarations whose code uses a name, with the lines\"},"
    "{\"method\":\"POST\",\"path\":\"/v1/code_callees\",\"body\":\"{\\\"query\\\":\\\"declared name\\\",\\\"path\\\":\\\"optional\\\"}\",\"does\":\"what a declaration calls that a code recipe declared, and where\"},"
    "{\"method\":\"POST\",\"path\":\"/v1/sources\",\"does\":\"the witnesses the database holds\"},"
    "{\"method\":\"POST\",\"path\":\"/v1/ingest_status\",\"body\":\"{\\\"source\\\":\\\"optional\\\"}\",\"does\":\"each witness with its parts and readings, the database's busy sessions, the ingest logs\"},"
    "{\"method\":\"POST\",\"path\":\"/v1/recipe_plan\",\"body\":\"{\\\"input\\\":\\\"optional source or path\\\"}\",\"does\":\"laplace ingest --plan\"},"
    "{\"method\":\"POST\",\"path\":\"/v1/recipe_catalog\",\"body\":\"{}\",\"does\":\"all available sources, aliases, content locations, decomposers and prerequisites; no database or corpus required\"},"
    "{\"method\":\"POST\",\"path\":\"/v1/hop\",\"body\":\"{\\\"input\\\":\\\"text\\\"}\",\"does\":\"laplace hop\"},"
    "{\"method\":\"POST\",\"path\":\"/v1/text\",\"body\":\"{\\\"input\\\":\\\"32 hex\\\"}\",\"does\":\"laplace text\"},"
    "{\"method\":\"POST\",\"path\":\"/v1/translate\",\"body\":\"{\\\"input\\\":\\\"word\\\",\\\"from\\\":\\\"en\\\",\\\"to\\\":\\\"fr\\\"}\",\"does\":\"laplace translate\"},"
    "{\"method\":\"POST\",\"path\":\"/v1/pull\",\"body\":\"{\\\"input\\\":\\\"text\\\"}\",\"does\":\"laplace pull\"},"
    "{\"method\":\"POST\",\"path\":\"/v1/degrees\",\"body\":\"{\\\"input\\\":\\\"text\\\",\\\"other\\\":\\\"text\\\"}\",\"does\":\"laplace degrees\"},"
    "{\"method\":\"GET\",\"path\":\"/v1/surface\"},"
    "{\"method\":\"GET\",\"path\":\"/health\"},"
    "{\"method\":\"POST\",\"path\":\"/mcp\",\"body\":\"JSON-RPC 2.0 MCP or op: embeddings|rerank|code_search|reindex|code_where|code_callers|code_callees|sources|ingest_status|file_outline|recipe_plan|recipe_catalog|search|forward|shape|frechet|ingest|hop|text|translate|degrees|pull|surface\"}"
    "],"
    "\"database_functions\":[\"laplace_containers\",\"laplace_claims\",\"laplace_claims_each\",\"laplace_text\",\"laplace_fills\",\"laplace_forward\",\"laplace_frechet4d\",\"laplace_dtw4d\",\"laplace_edr4d\",\"laplace_distance4d\",\"laplace_couple\",\"laplace_follows\",\"laplace_paths\",\"laplace_attested\",\"laplace_compose\",\"laplace_parts\"],"
    "\"engine_commands\":[\"tier0\",\"flags\",\"highway\",\"deploy\",\"sources\",\"ingest\",\"forget\",\"sweep\",\"index\",\"structure\",\"tree\",\"text\",\"pull\",\"turn\",\"hop\",\"translate\",\"degrees\",\"fills\",\"status\",\"bench\",\"model\"]}";

/* ---- code from the database (Laplace-MCP#9): what an ingested source's files hold, read off the containment.
 * A file is a trunk [metadata, content]; a recipe with a grammar records its content as the syntax tree, so the nodes
 * above a name are its enclosing expressions, statements and declarations, and above them the file. Nothing is read
 * from a working tree: the path is the file's own pathname record, the text and lines are the recorded content. */
typedef struct { lp_id id; uint8_t file; } CwNode;
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
 * occurrence. A code recipe states [declaration, "declared identifier", name],
 * with declaration = [kind, complete source bytes]. A record of one claim is
 * that claim, said as a record (LP_SAID_RECORD) where it stands, so a holder of the name said so one level up is a claim
 * too, as the engine's walk up counts it (walk_up): a file that declares one thing defines it. The front is bounded, so a
 * name as common as a keyword answers what it reached and says so. */
static int cw_walk_many(PGconn *pg,const lp_id *names, size_t count, CwDag *d, size_t max_nodes, int *levels, int *cut) {
    d->m = lp_idmap_new();
    lp_idmap *front = lp_idmap_new();
    for (size_t i=0;i<count;i++) { cw_node(d,&names[i],NULL); lp_idmap_put(front,&names[i],NULL); }
    lp_vec(lp_vertex) vx = { 0 };
    int level = 0, files = 0;
    *cut = 0;
    while (lp_idmap_count(front) && level < 128) {
        int nh = 0;
        Hold *h = holds_any(pg,lp_idmap_keys(front), (int)lp_idmap_count(front), &nh);
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
static int cw_walk(PGconn *pg,const lp_id *name, CwDag *d, size_t max_nodes, int *levels, int *cut) {
    return cw_walk_many(pg,name,1,d,max_nodes,levels,cut);
}

static size_t cw_lines(const char *text, size_t upto) {
    size_t l = 1;
    for (size_t i = 0; i < upto && text[i]; i++) if (text[i] == '\n') l++;
    return l;
}

static void cw_str(char **out, size_t *on, size_t *ocap, const char *s) {
    size_t l = strlen(s) * 6 + 8;
    char *e = malloc(l);
    json_esc(s, e, l);
    buf_add(out, on, ocap, e, strlen(e));
    free(e);
}

typedef struct { size_t file; int defines, occ; char *path; } CwFile;
static int cw_file_cmp(const void *a, const void *b) {
    const CwFile *x = a, *y = b;
    if (x->defines != y->defines) return y->defines - x->defines;
    if (x->occ != y->occ) return y->occ - x->occ;
    return strcmp(x->path, y->path);
}

/* ---- file_outline: what a file declares, from the database. The file is found by its name: up from the name through
 * the OS's record of it (its metadata) to the trunk whose first vertex is that record, and its pathname ends in the path
 * asked for (or the file is named by its ID, as code_where returns it). What a code recipe says of a file is a record
 * of typed declaration claims, one a declaration, in the file's order (or one claim, its own record). Each is at the first
 * line at or after the one before it where its name stands as a word in the file's recorded text. */
static int fo_hex(const char *s, lp_id *id) {
    if (strlen(s) != 32) return 0;
    for (int b = 0; b < 16; b++) {
        unsigned v;
        if (sscanf(s + b * 2, "%2x", &v) != 1) return 0;
        id->b[b] = (uint8_t)v;
    }
    return 1;
}
/* One entity's vertices, as laplace_paths gives its path. */
static size_t fo_vertices(PGconn *pg, const lp_id *id, lp_vertex **out, size_t *cap) {
    char h[33], arr[40];
    hex_id(id, h);
    snprintf(arr, sizeof arr, "{%s}", h);
    const char *v[1] = { arr };
    PGresult *r = db_exec_params(pg, "SELECT path::text FROM @extschema@.laplace_paths($1::@extschema@.blake3[])", 1, NULL, v, NULL, NULL, 0);
    size_t n = 0;
    if (PQresultStatus(r) == PGRES_TUPLES_OK && PQntuples(r) > 0) {
        const char *hx = PQgetvalue(r, 0, 0);
        size_t hl = strlen(hx) / 2;
        uint8_t *b = malloc(hl + 1);
        for (size_t i = 0; i < hl; i++) { unsigned x; sscanf(hx + i * 2, "%2x", &x); b[i] = (uint8_t)x; }
        n = lp_path_vertices(b, hl, NULL, 0);
        lp_reserve((void **)out, cap, n, sizeof **out);
        lp_path_vertices(b, hl, *out, n);
        free(b);
    }
    PQclear(r);
    return n;
}
static int fo_word(char c) { return isalnum((unsigned char)c) || c == '_' || (unsigned char)c >= 0x80; }
typedef struct { lp_id *v; size_t n, cap; } FoClaims;
/* Content trunks may contain arbitrarily nested blocks of records. Follow
 * their explicit roles, preserving source order; never descend text leaves. */
static void fo_collect_claims(PGconn *pg, const lp_vertex *roots, size_t count, FoClaims *claims) {
    typedef struct { lp_id id; uint32_t role; } Pending;
    lp_vec(Pending) pending = {0};
    lp_vertex *vertices=NULL; size_t capacity=0;
    for (size_t i=count; i-- > 0;) {
        Pending next = {roots[i].id, roots[i].said};
        if (next.role==LP_SAID_CLAIM || next.role==LP_SAID_RECORD || next.role==LP_SAID_HOLDS) lp_push(&pending,next);
    }
    while (pending.n) {
        Pending current = pending.v[--pending.n];
        if (current.role==LP_SAID_CLAIM) { lp_push(claims,current.id); continue; }
        size_t n=fo_vertices(pg,&current.id,&vertices,&capacity), marked=0;
        for (size_t i=n; i-- > 0;) {
            Pending next = {vertices[i].id,vertices[i].said};
            if (next.role!=LP_SAID_CLAIM && next.role!=LP_SAID_RECORD && next.role!=LP_SAID_HOLDS) continue;
            marked++;
            for (uint32_t run=vertices[i].run ? vertices[i].run : 1; run; run--) lp_push(&pending,next);
        }
        if (!marked && current.role==LP_SAID_RECORD) lp_push(claims,current.id);
    }
    free(vertices);
    lp_vec_free(&pending);
}
/* One file's outline: its recorded text (every part but the OS's record and what is said of the file) and the claims
 * [kind, name] said of it, in the file's order, each at the first line at or after the one before it where its name
 * stands as a word in that text (line 0: it does not). file_outline answers it; code_callers and code_callees read it. */
typedef struct { char *kind, *name, *source; size_t line, start, length, name_at; } FoDecl;
typedef struct { lp_id kind, name, source; } CodeDeclaration;
/* Read the recipe's typed declaration from the persisted semantic claim. */
static int code_declaration(Reader *rd, const lp_id *claim, CodeDeclaration *out) {
    lp_id parts[4], subject[3];
    if (reader_parts(rd, claim, parts, 4) != 3) return 0;
    char *relation = reader_text(rd, &parts[1], 80);
    int declaration = !strcmp(relation, "declared identifier");
    free(relation);
    if (!declaration || reader_parts(rd, &parts[0], subject, 3) != 2) return 0;
    *out = (CodeDeclaration){subject[0], parts[2], subject[1]};
    return 1;
}
static void fo_free(FoDecl *d, size_t n) {
    for (size_t k = 0; k < n; k++) { free(d[k].kind); free(d[k].name); free(d[k].source); }
    free(d);
}
static size_t fo_outline(PGconn *pg, Reader *rd, const lp_id *file, FoDecl **declp, char **textp, size_t *tnp) {
    lp_vertex *fv=NULL, *rv=NULL; size_t fcap=0, rcap=0;
    size_t nv = fo_vertices(pg, file, &fv, &fcap);
    /* the recorded text: every part but the OS's record and what is said of the file */
    size_t tn = 0, tcap = 1 << 16;
    char *text = malloc(tcap);
    text[0] = 0;
    FoClaims claims = { 0 };
    fo_collect_claims(pg, fv, nv, &claims);
    for (size_t v = 0; v < nv; v++) {
        uint32_t sd = fv[v].said;
        if (sd == LP_SAID_METADATA) continue;
        if (sd == LP_SAID_RECORD || sd == LP_SAID_CLAIM) {
            size_t nr = sd == LP_SAID_RECORD ? fo_vertices(pg, &fv[v].id, &rv, &rcap) : 0;
            int has = 0;
            for (size_t k = 0; k < nr; k++) if (rv[k].said == LP_SAID_CLAIM) has = 1;
            if (!has || sd == LP_SAID_CLAIM) continue;
        }
        for (uint32_t k = 0; k < (fv[v].run ? fv[v].run : 1); k++) {
            char *t = reader_text(rd, &fv[v].id, (size_t)8 << 20);
            size_t l = strlen(t);
            if (tn + l + 1 > tcap) { while (tn + l + 1 > tcap) tcap *= 2; text = realloc(text, tcap); }
            memcpy(text + tn, t, l + 1);
            tn += l;
            free(t);
        }
    }
    FoDecl *dl = NULL;
    size_t nd = 0, at = 0, line = 1;
    for (size_t k = 0; k < claims.n; k++) {
        CodeDeclaration declaration;
        if (!code_declaration(rd, &claims.v[k], &declaration)) continue;
        char *kind = reader_text(rd, &declaration.kind, 120), *name = reader_text(rd, &declaration.name, 400);
        size_t nl = strlen(name), found = 0, fl = 0;
        for (size_t p = at, ln = line; nl && p + nl <= tn; p++) {
            if (p > at && text[p - 1] == '\n') ln++;
            if (!memcmp(text + p, name, nl) && (p == 0 || !fo_word(text[p - 1])) && !fo_word(text[p + nl])) { found = p + 1; fl = ln; break; }
        }
        dl = realloc(dl, (nd + 1) * sizeof *dl);
        char *source = reader_text(rd, &declaration.source, (size_t)8 << 20);
        size_t length = strlen(source);
        const char *begin = length ? strstr(text + (at < tn ? at : tn), source) : NULL;
        if (!begin && length) begin = strstr(text, source);
        dl[nd++] = (FoDecl){kind, name, source, found ? fl : 0, begin ? (size_t)(begin-text) : SIZE_MAX, length, found ? found-1 : SIZE_MAX};
        if (found) { at = found - 1; line = fl; }
    }
    lp_vec_free(&claims);
    free(fv); free(rv);
    *declp = dl;
    *textp = text;
    *tnp = tn;
    return nd;
}
/* Declaration spans come from recorded grammar captures, not a second parser.
 * Select the smallest declaration that contains a recorded textual position. */
static long fo_holder(const FoDecl *declarations, size_t count, size_t position) {
    long best = -1;
    for (size_t i = 0; i < count; i++) {
        const FoDecl *d = &declarations[i];
        if (d->start == SIZE_MAX || position < d->start || position-d->start >= d->length) continue;
        if (best < 0 || d->length < declarations[best].length) best = (long)i;
    }
    return best;
}
static void on_file_outline(int c, const char *body) {
    char want[2048] = "", fid[64] = "";
    jstr(body, "path", want, sizeof want);
    jstr(body, "file", fid, sizeof fid);
    if (!want[0] && !fid[0]) { http(c, 400, "{\"error\":\"path or file\"}"); return; }
    PGconn *pg = db();
    if (!pg) {
        char e[512], b[640];
        json_esc(PQerrorMessage(PG), e, sizeof e);
        snprintf(b, sizeof b, "{\"error\":\"database\",\"detail\":\"%s\"}", e);
        http(c, 503, b);
        return;
    }
    uint64_t t0 = nsec();
    Reader *rd = reader_new(pg);
    lp_vec(lp_id) files = { 0 };
    if (fid[0]) {
        lp_id f;
        if (!fo_hex(fid, &f)) { reader_free(rd); http(c, 400, "{\"error\":\"file is a 32-digit ID\"}"); return; }
        lp_push(&files, f);
    } else {
        const char *base = strrchr(want, '/');
        base = base ? base + 1 : want;
        lp_ref trunk, parts[256];
        size_t np = 0;
        if (!*base || !name_entity(base, &trunk, parts, 256, &np)) { reader_free(rd); http(c, 400, "{\"error\":\"path\"}"); return; }
        lp_idmap *seen = lp_idmap_new(), *front = lp_idmap_new();
        lp_idmap_put(front, &trunk.id, NULL);
        lp_vec(lp_vertex) vx = { 0 };
        for (int level = 0; level < 10 && lp_idmap_count(front) && lp_idmap_count(seen) < 20000; level++) {
            int nh = 0;
            Hold *h = holds_any(pg,lp_idmap_keys(front), (int)lp_idmap_count(front), &nh);
            lp_idmap *next = lp_idmap_new();
            for (int j = 0; j < nh; j++) {
                size_t nv = lp_path_vertices(h[j].path, (size_t)h[j].path_len, NULL, 0);
                if (!nv) continue;
                lp_vec_reserve(&vx, nv);
                lp_path_vertices(h[j].path, (size_t)h[j].path_len, vx.v, nv);
                bool fresh;
                lp_idmap_put(seen, &h[j].entity, &fresh);
                if (!fresh) continue;
                /* a trunk whose first vertex is the OS's record, reached through that record: a file */
                if (vx.v[0].said == LP_SAID_METADATA && lp_idmap_find(front, &vx.v[0].id) >= 0) {
                    char *path = cw_file_name(rd, &h[j].entity);
                    size_t pl = strlen(path), wl = strlen(want);
                    if (pl >= wl && !strcmp(path + pl - wl, want) && (pl == wl || want[0] == '/' || path[pl - wl - 1] == '/'))
                        lp_push(&files, h[j].entity);
                    free(path);
                } else if (vx.v[0].said != LP_SAID_METADATA) lp_idmap_put(next, &h[j].entity, NULL);
            }
            holds_free(h, nh);
            lp_idmap_free(front);
            front = next;
        }
        lp_vec_free(&vx);
        lp_idmap_free(front);
        lp_idmap_free(seen);
    }
    char *out = NULL;
    size_t on = 0, ocap = 0;
    buf_fmt(&out, &on, &ocap, "{\"database\":\"%s\",\"files\":[", PQdb(pg));
    for (size_t f = 0; f < files.n && f < 16; f++) {
        FoDecl *dl = NULL;
        char *text = NULL;
        size_t tn = 0, nd = fo_outline(pg, rd, &files.v[f], &dl, &text, &tn);
        char *path = cw_file_name(rd, &files.v[f]), pe[8192], fh[33];
        json_esc(path, pe, sizeof pe);
        hex_id(&files.v[f], fh);
        buf_fmt(&out, &on, &ocap, "%s{\"path\":\"%s\",\"file\":\"%s\",\"bytes\":%zu,\"declarations\":[", f ? "," : "", pe, fh, tn);
        free(path);
        for (size_t k = 0; k < nd; k++) {
            buf_add(&out, &on, &ocap, k ? ",{\"kind\":\"" : "{\"kind\":\"", k ? 10 : 9);
            cw_str(&out, &on, &ocap, dl[k].kind);
            buf_add(&out, &on, &ocap, "\",\"name\":\"", 10);
            cw_str(&out, &on, &ocap, dl[k].name);
            buf_add(&out, &on, &ocap, "\"", 1);
            if (dl[k].line) buf_fmt(&out, &on, &ocap, ",\"line\":%zu", dl[k].line);
            buf_add(&out, &on, &ocap, "}", 1);
        }
        buf_add(&out, &on, &ocap, "]", 1);
        if (jbool(body, "include_text", 0)) {
            buf_add(&out, &on, &ocap, ",\"text\":\"", 9);
            cw_str(&out, &on, &ocap, text);
            buf_add(&out, &on, &ocap, "\"", 1);
        }
        buf_add(&out, &on, &ocap, "}", 1);
        fo_free(dl, nd);
        free(text);
    }
    buf_fmt(&out, &on, &ocap, "],\"ms\":%.3f}", (double)(nsec() - t0) / 1e6);
    lp_vec_free(&files);
    reader_free(rd);
    http(c, 200, out);
    free(out);
}

/* recipe_plan: which recipe reads what, as laplace ingest --plan answers it; nothing is read into the database. */
static void on_recipe_plan(int c, const char *body) {
    char what[4096] = "";
    jstr(body, "input", what, sizeof what);
    if (what[0] && !plain_arg(what)) { http(c, 400, "{\"error\":\"input is a source name or a path\"}"); return; }
    char *av[] = { (char *)ENGINE, "ingest", "--plan", what[0] ? what : NULL, NULL };
    on_engine(c, av);
}

static void on_recipe_catalog(int c, const char *body) {
    (void)body;
    char *av[] = { (char *)ENGINE, "ingest", "--catalog", NULL };
    on_engine(c, av);
}

static PGconn *cc_db(int c);
static void on_code_where(int c, const char *body) {
    char name[2048] = "", under[512] = "";
    if ((!jstr(body, "query", name, sizeof name) || !name[0]) && (!jstr(body, "input", name, sizeof name) || !name[0])) {
        http(c, 400, "{\"error\":\"query\"}"); return;
    }
    jstr(body, "path", under, sizeof under);
    int limit=20, context=120, per=3, defs_only=jbool(body,"defines",0);
    jint(body,"limit",&limit); jint(body,"context",&context); jint(body,"occurrences",&per);
    if (limit<1) limit=1; if(limit>200) limit=200;
    if (context<20) context=20; if(context>4000) context=4000;
    if (per<0) per=0; if(per>16) per=16;
    lp_ref trunk, parts[256]; size_t np=0;
    if (!name_entity(name,&trunk,parts,256,&np)) { http(c,500,"{\"error\":\"tier0\"}"); return; }
    PGconn *pg=cc_db(c); if(!pg) return;
    uint64_t t0=nsec(); CwDag d={0}; int levels=0,cut=0;
    int nfiles=cw_walk(pg,&trunk.id,&d,400000,&levels,&cut);
    Reader *rd=reader_new(pg);
    typedef struct { CwFile rank; FoDecl *declarations; size_t count,length; char *text; } Found;
    Found *found=calloc((size_t)nfiles+1,sizeof *found); size_t nf=0;
    for(size_t i=0;i<d.nn;i++){
        if(!d.n[i].file) continue;
        char *path=cw_file_name(rd,&d.n[i].id);
        if(under[0]&&!strstr(path,under)){ free(path); continue; }
        Found f={0}; f.rank.file=i; f.rank.path=path;
        f.count=fo_outline(pg,rd,&d.n[i].id,&f.declarations,&f.text,&f.length);
        for(size_t k=0;k<f.count;k++) if(!strcmp(f.declarations[k].name,name)) f.rank.defines=1;
        const char *at=f.text;
        while((at=strstr(at,name))){ f.rank.occ++; at+=strlen(name); }
        if(!f.rank.occ||(defs_only&&!f.rank.defines)){fo_free(f.declarations,f.count);free(f.text);free(path);continue;}
        found[nf++]=f;
    }
    /* CwFile is the first member: preserve definition-first ordering. */
    qsort(found,nf,sizeof *found,cw_file_cmp);
    char idh[33]; hex_id(&trunk.id,idh);
    char *out=NULL; size_t on=0,ocap=0;
    buf_fmt(&out,&on,&ocap,"{\"query\":\""); cw_str(&out,&on,&ocap,name);
    buf_fmt(&out,&on,&ocap,"\",\"id\":\"%s\",\"database\":\"%s\",\"levels\":%d,\"nodes\":%zu,\"files\":%zu,\"cut\":%s,\"hits\":[",
            idh,PQdb(pg),levels,d.nn,nf,cut?"true":"false");
    size_t shown=0;
    for(size_t f=0;f<nf&&shown<(size_t)limit;f++){
        Found *file=&found[f]; char fh[33]; hex_id(&d.n[file->rank.file].id,fh);
        buf_fmt(&out,&on,&ocap,"%s{\"path\":\"",shown?",":""); cw_str(&out,&on,&ocap,file->rank.path);
        buf_fmt(&out,&on,&ocap,"\",\"file\":\"%s\",\"defines\":[",fh);
        int nk=0;
        for(size_t k=0;k<file->count;k++){
            FoDecl *decl=&file->declarations[k]; if(strcmp(decl->name,name)) continue;
            int duplicate=0;
            for(size_t j=0;j<k;j++) if(!strcmp(file->declarations[j].name,name)&&!strcmp(file->declarations[j].kind,decl->kind)) duplicate=1;
            if(duplicate) continue;
            buf_fmt(&out,&on,&ocap,"%s\"",nk++?",":""); cw_str(&out,&on,&ocap,decl->kind); buf_add(&out,&on,&ocap,"\"",1);
        }
        buf_fmt(&out,&on,&ocap,"],\"occurrences\":%d,\"at\":[",file->rank.occ);
        const char *at=file->text; int emitted=0;
        while(emitted<per&&(at=strstr(at,name))){
            size_t position=(size_t)(at-file->text),start=position,end=position+strlen(name);
            long holder=fo_holder(file->declarations,file->count,position);
            if(holder>=0){start=file->declarations[holder].start;end=start+file->declarations[holder].length;}
            else {while(start&&file->text[start-1]!='\n')start--;while(end<file->length&&file->text[end]!='\n')end++;}
            size_t line=cw_lines(file->text,position),line0=cw_lines(file->text,start),line1=cw_lines(file->text,end)-(end>start&&file->text[end-1]=='\n');
            buf_fmt(&out,&on,&ocap,"%s{\"line\":%zu,\"line0\":%zu,\"line1\":%zu,\"context\":\"",emitted?",":"",line,line0,line1);
            size_t bytes=end-start; if(bytes>(size_t)context) bytes=(size_t)context;
            while(bytes&&((uint8_t)file->text[start+bytes]&0xc0)==0x80)bytes--;
            char saved=file->text[start+bytes];file->text[start+bytes]=0;cw_str(&out,&on,&ocap,file->text+start);file->text[start+bytes]=saved;
            buf_fmt(&out,&on,&ocap,"\",\"enclosing\":\"");
            size_t head=strcspn(file->text+start,"\r\n");if(head>end-start)head=end-start;if(head>240)head=240;
            while(head&&((uint8_t)file->text[start+head]&0xc0)==0x80)head--;
            saved=file->text[start+head];file->text[start+head]=0;cw_str(&out,&on,&ocap,file->text+start);file->text[start+head]=saved;
            buf_add(&out,&on,&ocap,"\"}",2);emitted++;at+=strlen(name);
        }
        buf_add(&out,&on,&ocap,"]}",2);shown++;
    }
    buf_fmt(&out,&on,&ocap,"],\"shown\":%zu,\"ms\":%.3f}",shown,(double)(nsec()-t0)/1e6);
    for(size_t f=0;f<nf;f++){fo_free(found[f].declarations,found[f].count);free(found[f].text);free(found[f].rank.path);}
    free(found);reader_free(rd);lp_idmap_free(d.m);free(d.n);
    http(c,200,out?out:"{\"error\":\"memory\"}");free(out);
}

/* ---- code_callers and code_callees (Laplace-MCP#27), read off the same containment code_where walks. A place the
 * name stands (a path up from it that is not a claim) is a chain of nodes below its file, each where it stands in the
 * file's recorded text. A node that starts on a declaration's line of the file's outline and holds that declaration's
 * name in its first line, before the place, is that declaration's node: the innermost one is what holds the place. */
static const char *cc_word(const char *base, const char *from, const char *end, const char *name) {
    size_t nl = strlen(name);
    for (const char *p = from; nl && p + nl <= end; p++)
        if (!memcmp(p, name, nl) && (p == base || !fo_word(p[-1])) && !fo_word(p[nl])) return p;
    return NULL;
}
static PGconn *cc_db(int c) {
    PGconn *pg = db();
    if (!pg) {
        char e[512], b[640];
        json_esc(PQerrorMessage(PG), e, sizeof e);
        snprintf(b, sizeof b, "{\"error\":\"database\",\"detail\":\"%s\"}", e);
        http(c, 503, b);
    }
    return pg;
}
typedef struct { int n; long l[16]; } CcLines;
static void cc_line(CcLines *cl, long line) {
    for (int z = 0; z < cl->n; z++) if (cl->l[z] == line) return;
    if (cl->n < 16) cl->l[cl->n++] = line;
}
static void cc_lines_json(char **out, size_t *on, size_t *ocap, const CcLines *cl) {
    buf_fmt(out, on, ocap, "\"at\":[");
    for (int z = 0; z < cl->n; z++) buf_fmt(out, on, ocap, "%s%ld", z ? "," : "", cl->l[z]);
    buf_fmt(out, on, ocap, "]");
}

/* code_callers: the declarations whose code uses the name, each with the lines it does at; a use outside every
 * declaration is its file's top level. A declaration's own name (its head) is not a use of it. */
static void on_code_callers(int c, const char *body) {
    char name[2048] = "", under[512] = "";
    jstr(body, "query", name, sizeof name);         /* "query", as code_where's: an MCP call's body names its tool "name" */
    if (!name[0]) { http(c, 400, "{\"error\":\"query\"}"); return; }
    jstr(body, "path", under, sizeof under);
    int limit = 50;
    jint(body, "limit", &limit);
    if (limit < 1) limit = 1;
    if (limit > 500) limit = 500;
    lp_ref trunk, parts[256];
    size_t np = 0;
    if (!name_entity(name, &trunk, parts, 256, &np)) { http(c, 500, "{\"error\":\"tier0\"}"); return; }
    PGconn *pg = cc_db(c);
    if (!pg) return;
    uint64_t t0 = nsec();
    CwDag d = { 0 };
    int levels = 0, cut = 0;
    cw_walk(pg,&trunk.id, &d, 400000, &levels, &cut);
    Reader *rd = reader_new(pg);
    char *out = NULL, *top = NULL;
    size_t on = 0, ocap = 0, tn = 0, tcap = 0, files = 0;
    buf_fmt(&out, &on, &ocap, "{\"name\":\"");
    cw_str(&out, &on, &ocap, name);
    buf_fmt(&out, &on, &ocap, "\",\"database\":\"%s\",\"callers\":[", PQdb(pg));
    int shown = 0, ntop = 0;
    for (size_t i = 0; i < d.nn && shown < limit; i++) {
        if (!d.n[i].file) continue;
        char *path = cw_file_name(rd, &d.n[i].id);
        if (under[0] && !strstr(path, under)) { free(path); continue; }
        FoDecl *dl = NULL;
        char *text = NULL;
        size_t tl = 0, nd = fo_outline(pg, rd, &d.n[i].id, &dl, &text, &tl);
        CcLines *by = calloc(nd + 1, sizeof *by), alone = {0};
        const char *nm = text;
        while ((nm = cc_word(text, nm, text + tl, name))) {
            size_t position = (size_t)(nm - text);
            long q = fo_holder(dl, nd, position);
            if (q < 0 || dl[q].name_at != position)
                cc_line(q >= 0 ? &by[q] : &alone, (long)cw_lines(text, position));
            nm += strlen(name);
        }
        char fh[33];
        hex_id(&d.n[i].id, fh);
        int any = 0;
        for (size_t q = 0; q < nd && shown < limit; q++) {
            if (!by[q].n) continue;
            buf_fmt(&out, &on, &ocap, "%s{\"name\":\"", shown ? "," : "");
            cw_str(&out, &on, &ocap, dl[q].name);
            buf_fmt(&out, &on, &ocap, "\",\"kind\":\"");
            cw_str(&out, &on, &ocap, dl[q].kind);
            buf_fmt(&out, &on, &ocap, "\",\"path\":\"");
            cw_str(&out, &on, &ocap, path);
            buf_fmt(&out, &on, &ocap, "\",\"file\":\"%s\",\"line\":%zu,", fh, dl[q].line);
            cc_lines_json(&out, &on, &ocap, &by[q]);
            buf_fmt(&out, &on, &ocap, "}");
            shown++;
            any = 1;
        }
        if (alone.n) {
            buf_fmt(&top, &tn, &tcap, "%s{\"path\":\"", ntop ? "," : "");
            cw_str(&top, &tn, &tcap, path);
            buf_fmt(&top, &tn, &tcap, "\",\"file\":\"%s\",", fh);
            cc_lines_json(&top, &tn, &tcap, &alone);
            buf_fmt(&top, &tn, &tcap, "}");
            ntop++;
        }
        if (any || alone.n) files++;
        free(by);
        fo_free(dl, nd);
        free(text);
        free(path);
    }
    buf_fmt(&out, &on, &ocap, "],\"top_level\":[");
    if (top) buf_add(&out, &on, &ocap, top, tn);
    buf_fmt(&out, &on, &ocap, "],\"files\":%zu,\"shown\":%d,\"cut\":%s,\"ms\":%.3f}", files, shown, cut ? "true" : "false", (double)(nsec() - t0) / 1e6);
    free(top);
    reader_free(rd);
    lp_idmap_free(d.m);
    free(d.n);
    http(c, 200, out ? out : "{\"error\":\"memory\"}");
    free(out);
}

/* code_callees: what a declaration calls. Its code is its node (the outermost node starting on the line of a
 * declaration of the name in a file's outline that holds the name there). A callee is a name in that code followed by
 * "(" that a code recipe declared in a file the database holds: a typed declaration claim said by that file's record of
 * claims, or as its one record; each with its kind and up to 4 of those files. A name called that nothing declares
 * (a library's, a language's) is listed apart, in outside. */
typedef struct { char *name, *kind; lp_id file[4]; int nfile, total; } CeCand;
static void ce_file(CeCand *x, const char *kind, const lp_id *file) {
    if (!x->kind) x->kind = strdup(kind);
    for (int k = 0; k < x->nfile; k++) if (!memcmp(x->file[k].b, file->b, 16)) return;
    x->total++;
    if (x->nfile < 4) x->file[x->nfile++] = *file;
}
static void on_code_callees(int c, const char *body) {
    char name[2048] = "", under[512] = "";
    jstr(body, "query", name, sizeof name);         /* "query", as code_where's: an MCP call's body names its tool "name" */
    if (!name[0]) { http(c, 400, "{\"error\":\"query\"}"); return; }
    jstr(body, "path", under, sizeof under);
    lp_ref trunk, parts[256];
    size_t np = 0;
    if (!name_entity(name, &trunk, parts, 256, &np)) { http(c, 500, "{\"error\":\"tier0\"}"); return; }
    PGconn *pg = cc_db(c);
    if (!pg) return;
    uint64_t t0 = nsec();
    CwDag d = { 0 };
    int levels = 0, cut = 0;
    cw_walk(pg,&trunk.id, &d, 400000, &levels, &cut);
    Reader *rd = reader_new(pg);
    /* the definitions: each its file, its outline entry and its code; the names each calls, by candidate index */
    typedef struct { size_t node; char *path, *kind; size_t line; long line1; lp_vec(size_t) calls; } CeDef;
    CeDef defs[16];
    size_t ndef = 0;
    lp_idmap *cand = lp_idmap_new();
    lp_vec(CeCand) cs = { 0 };
    for (size_t i = 0; i < d.nn && ndef < 16; i++) {
        if (!d.n[i].file) continue;
        char *path = cw_file_name(rd, &d.n[i].id);
        if (under[0] && !strstr(path, under)) { free(path); continue; }
        FoDecl *dl = NULL;
        char *text = NULL, *whole = NULL;
        size_t tl = 0, nd = fo_outline(pg, rd, &d.n[i].id, &dl, &text, &tl);
        whole = text; text = NULL;
        for (size_t q = 0; q < nd && ndef < 16; q++) {
            if (strcmp(dl[q].name, name) || dl[q].start == SIZE_MAX) continue;
            const char *s = whole + dl[q].start, *e = s + dl[q].length;
            CeDef *df = &defs[ndef++];
            memset(df, 0, sizeof *df);
            df->node = i;
            df->path = strdup(path);
            df->kind = strdup(dl[q].kind);
            df->line = dl[q].line;
            df->line1 = (long)cw_lines(whole, (size_t)(e-whole)) - (e>s && e[-1]=='\n');
            for (const char *p = s; p < e; p++) {          /* each name followed by "(" */
                if (!(isalpha((unsigned char)*p) || *p == '_' || (unsigned char)*p >= 0x80) || (p > whole && fo_word(p[-1]))) continue;
                const char *w = p;
                while (p < e && fo_word(*p)) p++;
                const char *a = p;
                while (a < e && (*a == ' ' || *a == '\t' || *a == '\r' || *a == '\n')) a++;
                if (a >= e || *a != '(') { p--; continue; }
                size_t wl = (size_t)(p - w);
                p--;
                if (wl >= 256 || (wl == strlen(name) && !memcmp(w, name, wl))) continue;
                char tok[256];
                memcpy(tok, w, wl);
                tok[wl] = 0;
                lp_ref tr, tp[256];
                size_t tnp = 0;
                if (!name_entity(tok, &tr, tp, 256, &tnp)) continue;
                bool fresh;
                size_t ci = lp_idmap_put(cand, &tr.id, &fresh);
                if (fresh) { CeCand x = { 0 }; x.name = strdup(tok); lp_push(&cs, x); }
                int have = 0;
                for (size_t z = 0; z < df->calls.n; z++) if (df->calls.v[z] == ci) have = 1;
                if (!have && df->calls.n < 256) lp_push(&df->calls, ci);
            }
        }
        fo_free(dl, nd);
        free(text);
        free(whole);
        free(path);
    }
    /* Resolve all candidate names together through the real containment tree.
     * A file may sit above any number of content/record block levels. */
    size_t ncand = lp_idmap_count(cand);
    if (ncand) {
        CwDag holders = {0}; int candidate_levels=0,candidate_cut=0;
        cw_walk_many(pg,lp_idmap_keys(cand),ncand,&holders,400000,&candidate_levels,&candidate_cut);
        cut |= candidate_cut;
        for (size_t i=0;i<holders.nn;i++) {
            if (!holders.n[i].file) continue;
            FoDecl *declarations=NULL; char *text=NULL; size_t length=0;
            size_t count=fo_outline(pg,rd,&holders.n[i].id,&declarations,&text,&length);
            for (size_t k=0;k<count;k++) {
                lp_ref identifier, constituents[256]; size_t nconstituents=0;
                if (!name_entity(declarations[k].name,&identifier,constituents,256,&nconstituents)) continue;
                int64_t candidate=lp_idmap_find(cand,&identifier.id);
                if (candidate>=0) ce_file(&cs.v[candidate],declarations[k].kind,&holders.n[i].id);
            }
            fo_free(declarations,count); free(text);
        }
        lp_idmap_free(holders.m); free(holders.n);
    }
    char *out = NULL;
    size_t on = 0, ocap = 0;
    buf_fmt(&out, &on, &ocap, "{\"name\":\"");
    cw_str(&out, &on, &ocap, name);
    buf_fmt(&out, &on, &ocap, "\",\"database\":\"%s\",\"definitions\":[", PQdb(pg));
    for (size_t f = 0; f < ndef; f++) {
        CeDef *df = &defs[f];
        char fh[33];
        hex_id(&d.n[df->node].id, fh);
        buf_fmt(&out, &on, &ocap, "%s{\"path\":\"", f ? "," : "");
        cw_str(&out, &on, &ocap, df->path);
        buf_fmt(&out, &on, &ocap, "\",\"file\":\"%s\",\"kind\":\"", fh);
        cw_str(&out, &on, &ocap, df->kind);
        buf_fmt(&out, &on, &ocap, "\",\"line0\":%zu,\"line1\":%ld,\"callees\":[", df->line, df->line1);
        int n = 0;
        for (size_t z = 0; z < df->calls.n; z++) {
            CeCand *x = &cs.v[df->calls.v[z]];
            if (!x->nfile) continue;
            buf_fmt(&out, &on, &ocap, "%s{\"name\":\"", n++ ? "," : "");
            cw_str(&out, &on, &ocap, x->name);
            buf_fmt(&out, &on, &ocap, "\",\"kind\":\"");
            cw_str(&out, &on, &ocap, x->kind ? x->kind : "");
            buf_fmt(&out, &on, &ocap, "\",\"declared_in\":%d,\"files\":[", x->total);
            for (int k = 0; k < x->nfile; k++) {
                char *pth = cw_file_name(rd, &x->file[k]), xh[33];
                hex_id(&x->file[k], xh);
                buf_fmt(&out, &on, &ocap, "%s{\"path\":\"", k ? "," : "");
                cw_str(&out, &on, &ocap, pth);
                buf_fmt(&out, &on, &ocap, "\",\"file\":\"%s\"}", xh);
                free(pth);
            }
            buf_fmt(&out, &on, &ocap, "]}");
        }
        buf_fmt(&out, &on, &ocap, "],\"outside\":[");
        n = 0;
        for (size_t z = 0; z < df->calls.n; z++) {
            CeCand *x = &cs.v[df->calls.v[z]];
            if (x->nfile) continue;
            buf_fmt(&out, &on, &ocap, "%s\"", n++ ? "," : "");
            cw_str(&out, &on, &ocap, x->name);
            buf_fmt(&out, &on, &ocap, "\"");
        }
        buf_fmt(&out, &on, &ocap, "]}");
        free(df->path);
        free(df->kind);
        lp_vec_free(&df->calls);
    }
    buf_fmt(&out, &on, &ocap, "],\"cut\":%s,\"ms\":%.3f}", cut ? "true" : "false", (double)(nsec() - t0) / 1e6);
    for (size_t z = 0; z < cs.n; z++) { free(cs.v[z].name); free(cs.v[z].kind); }
    lp_vec_free(&cs);
    lp_idmap_free(cand);
    reader_free(rd);
    lp_idmap_free(d.m);
    free(d.n);
    http(c, 200, out ? out : "{\"error\":\"memory\"}");
    free(out);
}

/* ---- ingest_status (Laplace-MCP#28): what the database holds and how current it is, read only. Each witness: its
 * name, trust, how many parts its trunk holds after its record, and readings, how many witnesses begin with the same record (a
 * source read again without superseding its earlier reading has more than one). Then the database's sessions that
 * are not idle, as pg_stat_activity shows them to this role, and the engine's per-source ingest logs
 * (LAPLACE_INGEST_LOGS, else $LAPLACE_WORK/logs/ingest), newest first, each with its last line. */
typedef struct { char name[256]; time_t mtime; long long bytes; } IsLog;
static int is_log_cmp(const void *a, const void *b) {
    const IsLog *x = a, *y = b;
    return x->mtime < y->mtime ? 1 : x->mtime > y->mtime ? -1 : strcmp(x->name, y->name);
}
static void on_ingest_status(int c, const char *body) {
    char want[512] = "";
    jstr(body, "source", want, sizeof want);
    PGconn *pg = cc_db(c);
    if (!pg) return;
    uint64_t t0 = nsec();
    PGresult *r = db_exec_params(pg, "SELECT id, trust FROM @extschema@.witness", 0, NULL, NULL, NULL, NULL, 1);
    if (PQresultStatus(r) != PGRES_TUPLES_OK) {
        char e[512], b[640];
        json_esc(PQerrorMessage(pg), e, sizeof e);
        snprintf(b, sizeof b, "{\"error\":\"witness\",\"detail\":\"%s\"}", e);
        PQclear(r);
        http(c, 500, b);
        return;
    }
    Reader *rd = reader_new(pg);
    int nw = PQntuples(r);
    size_t cap = (size_t)1 << 20;
    lp_id *kid = malloc(sizeof(lp_id) * cap), *recs = calloc((size_t)nw + 1, sizeof(lp_id));
    size_t *nfiles = calloc((size_t)nw + 1, sizeof(size_t));
    for (int i = 0; i < nw; i++) {
        lp_id id;
        memcpy(id.b, PQgetvalue(r, i, 0), 16);
        size_t m = reader_parts(rd, &id, kid, cap);
        if (m) recs[i] = kid[0];
        nfiles[i] = m;
    }
    char *out = NULL;
    size_t on = 0, ocap = 0;
    buf_fmt(&out, &on, &ocap, "{\"database\":\"%s\",\"sources\":[", PQdb(pg));
    int shown = 0;
    for (int i = 0; i < nw; i++) {
        lp_id id;
        memcpy(id.b, PQgetvalue(r, i, 0), 16);
        double trust;
        uint64_t tb = lp_be(PQgetvalue(r, i, 1), 8);
        memcpy(&trust, &tb, 8);
        if (want[0]) {                                  /* the whole name, not the shown 200 bytes of it */
            char *whole = nfiles[i] ? reader_text(rd, &recs[i], (size_t)1 << 16) : strdup("");
            int same = !strcmp(whole, want);
            free(whole);
            if (!same) continue;
        }
        char *nm = nfiles[i] ? reader_text(rd, &recs[i], 200) : strdup("");
        int readings = 0;
        for (int k = 0; k < nw; k++) if (nfiles[k] && !memcmp(recs[k].b, recs[i].b, 16)) readings++;
        char idh[33];
        hex_id(&id, idh);
        buf_fmt(&out, &on, &ocap, "%s{\"id\":\"%s\",\"name\":\"", shown++ ? "," : "", idh);
        cw_str(&out, &on, &ocap, nm);
        buf_fmt(&out, &on, &ocap, "\",\"trust\":%.3f,\"parts\":%zu%s,\"readings\":%d}", trust, nfiles[i] ? nfiles[i] - 1 : 0, nfiles[i] >= cap ? ",\"parts_cut\":true" : "", readings);
        free(nm);
    }
    PQclear(r);
    free(kid);
    free(recs);
    free(nfiles);
    reader_free(rd);
    buf_fmt(&out, &on, &ocap, "],\"activity\":[");
    r = db_exec(pg, "SELECT pid, coalesce(usename::text, ''), coalesce(state, ''), coalesce(extract(epoch FROM now() - query_start)::bigint, 0), "
                   "left(regexp_replace(coalesce(query, ''), '\\s+', ' ', 'g'), 200) FROM pg_stat_activity "
                   "WHERE datname = current_database() AND pid <> pg_backend_pid() AND state IS DISTINCT FROM 'idle' ORDER BY query_start NULLS LAST LIMIT 32");
    if (PQresultStatus(r) == PGRES_TUPLES_OK)
        for (int i = 0; i < PQntuples(r); i++) {
            buf_fmt(&out, &on, &ocap, "%s{\"pid\":%s,\"user\":\"", i ? "," : "", PQgetvalue(r, i, 0));
            cw_str(&out, &on, &ocap, PQgetvalue(r, i, 1));
            buf_fmt(&out, &on, &ocap, "\",\"state\":\"");
            cw_str(&out, &on, &ocap, PQgetvalue(r, i, 2));
            buf_fmt(&out, &on, &ocap, "\",\"seconds\":%s,\"query\":\"", PQgetvalue(r, i, 3));
            cw_str(&out, &on, &ocap, PQgetvalue(r, i, 4));
            buf_fmt(&out, &on, &ocap, "\"}");
        }
    PQclear(r);
    buf_fmt(&out, &on, &ocap, "],\"logs\":");
    char dir[4096] = "";
    const char *ld = getenv("LAPLACE_INGEST_LOGS"), *work = getenv("LAPLACE_WORK");
    if (ld && *ld) snprintf(dir, sizeof dir, "%s", ld);
    else if (work && *work) snprintf(dir, sizeof dir, "%s/logs/ingest", work);
    int listed = 0;
    os_list names = { 0 };
    if (dir[0] && !os_list_dir(dir, &names)) {
        lp_vec(IsLog) logs = { 0 };
        for (size_t entry = 0; entry < names.n; entry++) {
            const char *name = names.item[entry];
            size_t l = strlen(name);
            if (l < 5 || l >= 256 || strcmp(name + l - 4, ".log")) continue;
            char fp[4400];
            struct stat st;
            snprintf(fp, sizeof fp, "%s/%s", dir, name);
            if (stat(fp, &st) || !S_ISREG(st.st_mode)) continue;
            IsLog x = { "", st.st_mtime, (long long)st.st_size };
            memcpy(x.name, name, l - 4);
            x.name[l - 4] = 0;
            if (want[0] && strcmp(x.name, want)) continue;
            lp_push(&logs, x);
        }
        qsort(logs.v, logs.n, sizeof *logs.v, is_log_cmp);
        buf_fmt(&out, &on, &ocap, "{\"dir\":\"");
        cw_str(&out, &on, &ocap, dir);
        buf_fmt(&out, &on, &ocap, "\",\"runs\":[");
        for (size_t k = 0; k < logs.n && k < 20; k++) {
            char fp[4400], tail[4097] = "", when[32];
            snprintf(fp, sizeof fp, "%s/%s.log", dir, logs.v[k].name);
            FILE *f = fopen(fp, "rb");
            if (f) {
                long sz = logs.v[k].bytes > 4096 ? 4096 : (long)logs.v[k].bytes;
                fseek(f, -sz, SEEK_END);
                size_t got = fread(tail, 1, (size_t)sz, f);
                tail[got] = 0;
                fclose(f);
            }
            size_t tl = strlen(tail);
            while (tl && (tail[tl - 1] == '\n' || tail[tl - 1] == '\r' || tail[tl - 1] == ' ')) tail[--tl] = 0;
            char *last = strrchr(tail, '\n');
            last = last ? last + 1 : tail;
            if (strlen(last) > 240) last[240] = 0;
            for (char *p = last; *p; p++) if ((unsigned char)*p < 0x20 && *p != '\t') *p = ' ';
            struct tm tmv;
#ifdef _WIN32
            gmtime_s(&tmv, &logs.v[k].mtime);
#else
            gmtime_r(&logs.v[k].mtime, &tmv);
#endif
            strftime(when, sizeof when, "%Y-%m-%dT%H:%M:%SZ", &tmv);
            buf_fmt(&out, &on, &ocap, "%s{\"source\":\"", k ? "," : "");
            cw_str(&out, &on, &ocap, logs.v[k].name);
            buf_fmt(&out, &on, &ocap, "\",\"modified\":\"%s\",\"bytes\":%lld,\"last\":\"", when, logs.v[k].bytes);
            cw_str(&out, &on, &ocap, last);
            buf_fmt(&out, &on, &ocap, "\"}");
        }
        buf_fmt(&out, &on, &ocap, "]}");
        lp_vec_free(&logs);
        listed = 1;
    }
    os_list_free(&names);
    if (!listed) buf_fmt(&out, &on, &ocap, "null");
    buf_fmt(&out, &on, &ocap, ",\"ms\":%.3f}", (double)(nsec() - t0) / 1e6);
    http(c, 200, out);
    free(out);
}/* The sources the database holds: each witness, its name (the first part of its trunk, the record its source file
 * names) and its trust. */
static void on_sources(int c, const char *body) {
    (void)body;
    PGconn *pg = db();
    if (!pg) { http(c, 503, "{\"error\":\"database\"}"); return; }
    PGresult *r = db_exec_params(pg, "SELECT id, trust FROM @extschema@.witness", 0, NULL, NULL, NULL, NULL, 1);
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
    "{\"name\":\"code_search\",\"description\":\"Search code and documentation across indexed repositories using Laplace Tree-sitter AST declaration chunking, UAX #29 segmentation, IDF constituent postings, and 4D Frechet trajectory distance. With LAPLACE_CODE_SEMANTIC set, nearest chunks from the configured embedding endpoint and Qdrant collections join the candidates, and the union is ordered by reciprocal rank fusion of each collection's ranking and the Laplace rerank's (fusion rerank: the rerank alone); a configured service that fails is an error. Returns matching files, exact line ranges (line0..line1), source (lexical, semantic or both), Laplace entity ID, Hilbert index, shared constituent count, Frechet distance, rerank score, cosine for semantic hits, the fused score and each ranking's rank of the hit, and code snippet.\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"query\":{\"type\":\"string\"},\"path\":{\"type\":\"string\"},\"limit\":{\"type\":\"integer\"},\"fusion\":{\"type\":\"string\",\"enum\":[\"rrf\",\"rerank\"]},\"bridge\":{\"type\":\"boolean\",\"description\":\"add to the query the words the database's wordnets reach from its words (laplace translate), for the lexical retrieval and the rerank\"},\"scope\":{\"type\":\"string\",\"enum\":[\"all\",\"code\",\"docs\"],\"description\":\"which embedding collections are asked: code (the default), prose (docs), or both (all)\"},\"explain\":{\"type\":\"boolean\",\"description\":\"every candidate of the union, not only the first limit\"}},\"required\":[\"query\"]}},"
    "{\"name\":\"code_where\",\"description\":\"Where a name or any exact span of code (query) stands in the code the Laplace database holds (an ingested source such as workspace-code), read off the containment above it, nothing from a working tree. Each file that holds it, by the pathname its record gives: defines, the declaration kinds its recipe said of the name there (function_definition, class_declaration...), files that define it first; occurrences, how many places in its syntax tree hold it; at, up to occurrences (default 3) of them: the line, the innermost enclosing node of at least context bytes (default 120), and the enclosing declaration's first line with its span line0..line1. defines true: only files that define it. path: a substring the file path must contain.\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"query\":{\"type\":\"string\"},\"path\":{\"type\":\"string\"},\"defines\":{\"type\":\"boolean\"},\"limit\":{\"type\":\"integer\"},\"occurrences\":{\"type\":\"integer\"},\"context\":{\"type\":\"integer\"}},\"required\":[\"query\"]}},"
    "{\"name\":\"code_callers\",\"description\":\"Who uses a name (query) in the code the Laplace database holds: each declaration (name, kind, path, its line) whose code holds a place the name stands, with the lines it does at (at). A place outside every declaration is its file's top level (top_level); a declaration's own name is not a use of it. Read off the containment code_where walks and the file's outline; same-named declarations in different files are not told apart. path: a substring the file path must contain.\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"query\":{\"type\":\"string\",\"description\":\"a declared name\"},\"path\":{\"type\":\"string\"},\"limit\":{\"type\":\"integer\"}},\"required\":[\"query\"]}},"
    "{\"name\":\"code_callees\",\"description\":\"What a declaration calls, in the code the Laplace database holds: for each definition of the name (query; path, kind, line0..line1), each name in its code followed by ( that a code recipe declared, with its kind and the files that declare it (callees), and the names it calls that nothing there declares (outside: a library's, a language's). path: a substring the defining file's path must contain.\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"query\":{\"type\":\"string\",\"description\":\"a declared name\"},\"path\":{\"type\":\"string\"}},\"required\":[\"query\"]}},"
    "{\"name\":\"ingest_status\",\"description\":\"What the Laplace database holds and how current it is, read only: each source's witness (name, trust, parts: what its trunk holds after its record, and readings: how many witnesses begin with its record; more than one is a source read again without superseding its earlier reading), the database's sessions that are not idle (a running ingest), and the engine's per-source ingest logs, newest first, with each one's last line. source: one source's name.\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"source\":{\"type\":\"string\"}},\"required\":[]}},"
    "{\"name\":\"sources\",\"description\":\"The sources the Laplace database holds: each witness ID, its name and trust.\",\"inputSchema\":{\"type\":\"object\",\"properties\":{},\"required\":[]}},"
    "{\"name\":\"file_outline\",\"description\":\"What a file in the Laplace database declares: the typed declaration claims its code recipe said of it, in the file's order, each with the line where its name first stands after the one before. The file by the end of its pathname (path) or its ID (file, as code_where returns it).\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"},\"file\":{\"type\":\"string\"},\"include_text\":{\"type\":\"boolean\",\"description\":\"Include the source text reconstructed from persisted records.\"}},\"required\":[]}},"
    "{\"name\":\"recipe_plan\",\"description\":\"Which recipe reads what, as laplace ingest --plan answers it, without reading anything into the database: a source name or a path (input), else every source in order.\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"input\":{\"type\":\"string\"}},\"required\":[]}},"
    "{\"name\":\"recipe_catalog\",\"description\":\"Complete available ingestion sources, short aliases, content locations, recipes, file formats and prerequisites from the Engine; no database or corpus required.\",\"inputSchema\":{\"type\":\"object\",\"properties\":{},\"required\":[]}},"
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
        else if (!strcmp(name, "file_outline")) on_file_outline(c, body);
        else if (!strcmp(name, "code_callers")) on_code_callers(c, body);
        else if (!strcmp(name, "code_callees")) on_code_callees(c, body);
        else if (!strcmp(name, "ingest_status")) on_ingest_status(c, body);
        else if (!strcmp(name, "recipe_plan")) on_recipe_plan(c, body);
        else if (!strcmp(name, "recipe_catalog")) on_recipe_catalog(c, body);
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
        else if (!strcmp(op, "file_outline")) path = "/v1/file_outline";
        else if (!strcmp(op, "code_callers")) path = "/v1/code_callers";
        else if (!strcmp(op, "code_callees")) path = "/v1/code_callees";
        else if (!strcmp(op, "ingest_status")) path = "/v1/ingest_status";
        else if (!strcmp(op, "recipe_plan")) path = "/v1/recipe_plan";
        else if (!strcmp(op, "recipe_catalog")) path = "/v1/recipe_catalog";
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
    else if (!strcmp(path, "/v1/file_outline")) on_file_outline(c, body);
    else if (!strcmp(path, "/v1/code_callers")) on_code_callers(c, body);
    else if (!strcmp(path, "/v1/code_callees")) on_code_callees(c, body);
    else if (!strcmp(path, "/v1/ingest_status")) on_ingest_status(c, body);
    else if (!strcmp(path, "/v1/recipe_plan")) on_recipe_plan(c, body);
    else if (!strcmp(path, "/v1/recipe_catalog")) on_recipe_catalog(c, body);
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

/* ---- who may call the listener. Every request but GET /health carries Authorization: Bearer TOKEN, the one the
 * installation made for this service (Laplace-Operations: /etc/laplace/secrets/modular-mcp.env, made once, as the
 * monorepo MCP's mcp.env is). The token is LAPLACE_MCP_TOKEN, else the LAPLACE_MCP_TOKEN line of the file
 * LAPLACE_MCP_TOKEN_FILE names (or its first line). With no token of at least 32 characters and no whitespace the
 * listener does not start: it fails closed. stdio (lpm mcp) is the calling user's own process and takes none. */
static char SERVE_TOKEN[513];
static size_t SERVE_TOKEN_N;
static int token_take(const char *v, size_t n) {
    while (n && (v[n - 1] == '\n' || v[n - 1] == '\r')) n--;
    if (n >= 2 && (v[0] == '"' || v[0] == '\'') && v[n - 1] == v[0]) { v++; n -= 2; }
    if (n < 32 || n >= sizeof SERVE_TOKEN) return 0;
    for (size_t i = 0; i < n; i++) if ((unsigned char)v[i] <= ' ' || v[i] == 0x7f) return 0;
    memcpy(SERVE_TOKEN, v, n);
    SERVE_TOKEN[n] = 0;
    SERVE_TOKEN_N = n;
    return 1;
}
static int serve_token_load(void) {
    const char *e = getenv("LAPLACE_MCP_TOKEN");
    if (e && *e) return token_take(e, strlen(e));
    const char *f = getenv("LAPLACE_MCP_TOKEN_FILE");
    if (!f || !*f) return 0;
    FILE *fp = fopen(f, "rb");
    if (!fp) { perror(f); return 0; }
    char line[1024], first[1024] = "";
    int ok = 0, lines = 0;
    while (fgets(line, sizeof line, fp)) {
        if (!lines++) snprintf(first, sizeof first, "%s", line);
        if (!strncmp(line, "LAPLACE_MCP_TOKEN=", 18)) { ok = token_take(line + 18, strlen(line + 18)); break; }
    }
    fclose(fp);
    if (!ok && lines && !strchr(first, '=')) ok = token_take(first, strlen(first));
    memset(line, 0, sizeof line);
    memset(first, 0, sizeof first);
    return ok;
}
/* The request's bearer token against the service's, in time that does not depend on where they differ. */
static int request_authorized(const char *req, const char *head_end) {
    const char *p = req;
    while ((p = strstr(p, "\r\n")) && p < head_end) {
        p += 2;
        if (strncasecmp(p, "Authorization:", 14)) continue;
        const char *v = p + 14;
        while (*v == ' ' || *v == '\t') v++;
        if (strncasecmp(v, "Bearer ", 7)) return 0;
        v += 7;
        while (*v == ' ') v++;
        size_t n = strcspn(v, "\r\n");
        while (n && (v[n - 1] == ' ' || v[n - 1] == '\t')) n--;
        unsigned d = n != SERVE_TOKEN_N;
        for (size_t i = 0; i < SERVE_TOKEN_N; i++) d |= (unsigned)(unsigned char)SERVE_TOKEN[i] ^ (unsigned)(unsigned char)(i < n ? v[i] : 0);
        return d == 0 && SERVE_TOKEN_N > 0;
    }
    return 0;
}

int api_serve(const char *host, int port) {
    if (!serve_token_load()) {
        fprintf(stderr, "lpm serve: no token: set LAPLACE_MCP_TOKEN, or LAPLACE_MCP_TOKEN_FILE to a file with its "
                        "LAPLACE_MCP_TOKEN line (at least 32 characters, no whitespace); refusing to listen unauthenticated\n");
        return 2;
    }
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
        int open_route = !strcmp(method, "GET") && !strcmp(path, "/health");
        if (!open_route && (!body || !request_authorized(req, body))) {
            static const char deny[] = "HTTP/1.1 401 Unauthorized\r\nContent-Type: application/json\r\nWWW-Authenticate: Bearer\r\n"
                                       "Content-Length: 24\r\nConnection: close\r\n\r\n{\"error\":\"unauthorized\"}";
            send(c, deny, (int)strlen(deny), MSG_NOSIGNAL);
            free(req);
            close_sock(c);
            continue;
        }
        body = body ? body + 4 : "";
        dispatch(c, method, path, body);
        free(req);
        close_sock(c);
    }
}
