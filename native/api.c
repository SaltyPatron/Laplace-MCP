#define _GNU_SOURCE
#include "api_internal.h"
#include "laplace/json.h"
#include <limits.h>

const lp_tier0_record *T0;
lp_text *TX;
PGconn *PG;
const char *ENGINE = LPM_ENGINE_DEFAULT;
static const lp_highway *VIEW_HIGHWAY;
const lp_highway *api_view_highway(void) { if (!VIEW_HIGHWAY) VIEW_HIGHWAY = lp_highway_map(NULL); return VIEW_HIGHWAY; }

void api_engine_resolve(void) {
    const char *e = getenv("LAPLACE_ENGINE");
    if (e && *e) ENGINE = e;
}

uint64_t api_nsec(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

void api_hex_id(const lp_id *id, char out[33]) {
    static const char H[] = "0123456789abcdef";
    for (int i = 0; i < 16; i++) {
        out[i * 2] = H[id->b[i] >> 4];
        out[i * 2 + 1] = H[id->b[i] & 15];
    }
    out[32] = 0;
}

int api_valid_id(const char *s) {
    if (!s || strlen(s) != 32) return 0;
    for (int i = 0; i < 32; i++) {
        char c = s[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) return 0;
    }
    return 1;
}

PGconn *api_db(void) {
    const char *ci = laplace_db();
    if(!PG || PQstatus(PG)!=CONNECTION_OK){
        if(PG){lpm_read_end();PQfinish(PG);}
        PG=db_try(ci);
    }
    if (!PG) return NULL;
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

void api_http(int c, int code, const char *body) {
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

void api_capture_begin(int stdio_mode) {
    if (stdio_mode) {
        stdio_capture = 1;
        stdio_code = 500;
        free(stdio_body);
        stdio_body = NULL;
    } else {
        capture = 1;
        capture_code = 500;
        free(capture_body);
        capture_body = NULL;
    }
}

const char *api_capture_end(int stdio_mode, int *code_out) {
    if (stdio_mode) {
        stdio_capture = 0;
        if (code_out) *code_out = stdio_code;
        return stdio_body ? stdio_body : "";
    } else {
        capture = 0;
        if (code_out) *code_out = capture_code;
        return capture_body ? capture_body : "";
    }
}

/* JSON through the one reader and writer (laplace/json.h). A request's field is a member of the object its text begins
 * with: a key inside a string or a nested object is none, and text that is no JSON object answers nothing. */
static lp_json *api_jdoc(const char *body) {
    return body ? lp_json_parse_prefix(body, strlen(body), NULL) : NULL;
}

/* s escaped for the inside of a JSON string, cut at cap without splitting an escape. */
char *api_json_esc(const char *s, char *out, size_t cap) {
    size_t n = 0;
    lp_buf one = { 0 };
    if (!s) s = "";
    for (const char *p = s; *p; p++) {
        one.n = 0;
        lp_json_str(&one, p, 1);
        size_t k = one.n - 2;
        if (n + k >= cap) break;
        memcpy(out + n, one.b + 1, k);
        n += k;
    }
    out[n] = 0;
    lp_buf_free(&one);
    return out;
}

/* The member's own text in body (so a nested object is handed on as it came), or NULL. */
const char *api_jvalue(const char *body, const char *key) {
    lp_json *j = api_jdoc(body);
    const char *value = lp_json_text(j, lp_json_get(j, LP_JSON_ROOT, key), NULL);
    lp_json_free(j);
    return value;
}

/* A string member, decoded (escapes, surrogate pairs) into out; 0 when absent, no string, holding NUL, or too long. */
int api_jstr(const char *body, const char *key, char *out, size_t cap) {
    lp_json *j = api_jdoc(body);
    size_t n = 0;
    const char *s = lp_json_string(j, lp_json_get(j, LP_JSON_ROOT, key), &n);
    int ok = s && n < cap && !memchr(s, 0, n);
    if (ok) { memcpy(out, s, n); out[n] = 0; }
    lp_json_free(j);
    return ok;
}

/* A flag: JSON true or false, or the strings "true" and "false". Absent or anything else: dflt. */
int api_jbool(const char *body, const char *key, int dflt) {
    lp_json *j = api_jdoc(body);
    uint32_t v = lp_json_get(j, LP_JSON_ROOT, key);
    bool b; const char *s = lp_json_string(j, v, NULL);
    int answer = lp_json_bool(j, v, &b) ? b : s && !strcmp(s, "true") ? 1 : s && !strcmp(s, "false") ? 0 : dflt;
    lp_json_free(j);
    return answer;
}

/* An integer member that int holds. */
int api_jint(const char *body, const char *key, int *out) {
    lp_json *j = api_jdoc(body);
    int64_t v;
    int ok = lp_json_integer(j, lp_json_get(j, LP_JSON_ROOT, key), &v) && v >= INT_MIN && v <= INT_MAX;
    if (ok) *out = (int)v;
    lp_json_free(j);
    return ok;
}

/* An array member's strings (an element that is an object gives its "text"), malloc'd into items, at most max_items. */
size_t api_parse_str_array(const char *body, const char *key, char **items, size_t max_items) {
    lp_json *j = api_jdoc(body);
    uint32_t list = lp_json_get(j, LP_JSON_ROOT, key);
    size_t count = 0;
    if (lp_json_kind(j, list) == LP_JSON_ARRAY)
        for (uint32_t i = 0; i < lp_json_count(j, list) && count < max_items; i++) {
            uint32_t item = lp_json_item(j, list, i);
            if (lp_json_kind(j, item) == LP_JSON_OBJECT) item = lp_json_get(j, item, "text");
            size_t n = 0; const char *s = lp_json_string(j, item, &n);
            if (s && !memchr(s, 0, n)) items[count++] = strdup(s);
        }
    lp_json_free(j);
    return count;
}

int api_name_entity(const char *text, lp_ref *trunk, lp_ref *parts, size_t cap, size_t *nparts) {
    if (!TX) return 0;
    *trunk = lp_text_parts(TX, (const uint8_t *)text, strlen(text), parts, cap, nparts);
    return 1;
}

void api_parts_json(const lp_ref *parts, size_t n, char *out, size_t cap) {
    size_t u = 0;
    out[0] = 0;
    for (size_t i = 0; i < n && u + 40 < cap; i++) {
        char id[33];
        api_hex_id(&parts[i].id, id);
        u += (size_t)snprintf(out + u, cap - u, "%s\"%s\"", i ? "," : "", id);
    }
}

int api_q1(PGconn *pg, const char *sql, const char *a, char **out, char *err, size_t errcap) {
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

int api_q2(PGconn *pg, const char *sql, const char *a, const char *b, char **out, char *err, size_t errcap) {
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


int api_mask_claim(const char *m, int len) {
    if (len < 5) return 0;
    return ((const unsigned char *)m)[4] & 0x80;
}
/* PostgreSQL bit(64)'s binary carrier has a network-order length and MSB-first
 * bit positions. Decode transport bytes here; Native's manifest APIs own every
 * field's enum/flags semantics and canonical value identities. */
void api_emit_mask_values(char **out,size_t *n,size_t *cap,const char *wire,int len){
    const lp_highway *highway=api_view_highway();api_buf_fmt(out,n,cap,"[");
    if(highway&&len==12&&lp_be(wire,4)==64){
        lp_mask mask=0;const uint8_t *bytes=(const uint8_t *)wire+4;
        for(unsigned bit=0;bit<64;bit++)if(bytes[bit>>3]&(0x80u>>(bit&7)))mask|=UINT64_C(1)<<bit;
        int shown=0;
        for(size_t i=0;i<highway->nbanks;i++){
            const lp_bank *bank=highway->bank+i;if(!(mask&lp_mask_field(bank)))continue;
            api_buf_fmt(out,n,cap,"%s{\"bank\":\"%s\",\"carrier\":\"%s\",\"encoding\":\"%s\",\"values\":[",shown++?",":"",bank->name,bank->carrier,bank->flags?"flags":"enum");
            unsigned values=0;for(uint32_t slot=0;slot<bank->list->count;slot++)if(lp_mask_has(mask,bank,slot)){
                const lp_tier0_record *record=lp_highway_at(highway,bank->list,slot);if(!record)continue;
                char id[33];api_hex_id(&record->id,id);const char *label=lp_highway_identifier(highway,bank->list,slot);
                api_buf_fmt(out,n,cap,"%s{\"slot\":%u,\"id\":\"%s\",\"label\":\"",values++?",":"",slot,id);api_cw_str(out,n,cap,label?label:"");api_buf_fmt(out,n,cap,"\"}");
            }
            api_buf_fmt(out,n,cap,"]}");
        }
    }
    api_buf_fmt(out,n,cap,"]");
}

const char *API_FORWARD_SQL =
    "SELECT COALESCE((SELECT json_agg(x) FROM ("
    "  SELECT json_build_object('i', i, 'j', j, 'paths', paths, 'runs', runs,"
    "    'next', next::text, 'times', times) AS x"
    "  FROM @extschema@.laplace_forward($1::@extschema@.blake3[], 64) LIMIT 128) s), '[]'::json)::text";

const char *API_SHAPE_SQL =
    "SELECT @extschema@.laplace_frechet4d("
    " (SELECT @postgis@.ST_MakeLine(e.coord ORDER BY u.ord) FROM unnest(string_to_array($1, ',')::@extschema@.blake3[])"
    "    WITH ORDINALITY AS u(id, ord) JOIN @extschema@.entity e ON e.id OPERATOR(@extschema@.=) u.id),"
    " (SELECT @postgis@.ST_MakeLine(e.coord ORDER BY u.ord) FROM unnest(string_to_array($2, ',')::@extschema@.blake3[])"
    "    WITH ORDINALITY AS u(id, ord) JOIN @extschema@.entity e ON e.id OPERATOR(@extschema@.=) u.id)"
    ")::text";

int api_part_list(const lp_ref *parts, size_t n, char *out, size_t cap) {
    size_t u = 0;
    if (!n) return 0;
    for (size_t i = 0; i < n; i++) {
        char id[33];
        api_hex_id(&parts[i].id, id);
        int w = snprintf(out + u, cap - u, "%s%s", i ? "," : "", id);
        if (w < 0 || u + (size_t)w >= cap) return 0;
        u += (size_t)w;
    }
    return 1;
}

void api_coord_json(const lp_ref *r, char *out, size_t cap) {
    snprintf(out, cap, "[%.17g,%.17g,%.17g,%.17g]",
             (double)r->c.m[0] / LP_FIXED_ONE, (double)r->c.m[1] / LP_FIXED_ONE,
             (double)r->c.m[2] / LP_FIXED_ONE, (double)r->c.m[3] / LP_FIXED_ONE);
}

void api_unit_coord_json(const lp_ref *r, char *out, size_t cap) {
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


/* An answer grows the one way every Laplace array grows (lp_reserve: doubled, and an allocation that fails ends the
 * process rather than dropping part of an answer). lpm never replaces the allocator, so free() releases it. */
void api_buf_add(char **b, size_t *n, size_t *cap, const char *s, size_t m) {
    lp_reserve((void **)b, cap, *n + m + 1, 1);
    memcpy(*b + *n, s, m);
    *n += m;
    (*b)[*n] = 0;
}

void api_buf_fmt(char **b, size_t *n, size_t *cap, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    char tmp[1200];
    int w = vsnprintf(tmp, sizeof tmp, fmt, ap);
    va_end(ap);
    if (w < 0) return;
    if ((size_t)w < sizeof tmp) { api_buf_add(b, n, cap, tmp, (size_t)w); return; }
    /* Longer: formatted straight into the answer's own room. */
    lp_reserve((void **)b, cap, *n + (size_t)w + 1, 1);
    va_start(ap, fmt);
    vsnprintf(*b + *n, (size_t)w + 1, fmt, ap);
    va_end(ap);
    *n += (size_t)w;
}
