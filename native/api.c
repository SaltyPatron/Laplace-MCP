#define _GNU_SOURCE
#include "api_internal.h"
#include "laplace/json.h"
#include "os.h"
#include <limits.h>

const lp_tier0_record *T0;
lp_text *TX;
const char *ENGINE = LPM_ENGINE_DEFAULT;
static const lp_highway *VIEW_HIGHWAY;
const lp_highway *api_view_highway(void) { if (!VIEW_HIGHWAY) VIEW_HIGHWAY = lp_highway_map(NULL); return VIEW_HIGHWAY; }

enum { DB_POOL_MAX = 16, DB_POOL_DEFAULT = 4 };
typedef struct {
    PGconn *pg;
    char *conninfo;
    int in_use;
} DbPoolSlot;

static pthread_mutex_t db_pool_mu = PTHREAD_MUTEX_INITIALIZER;
static DbPoolSlot db_pool[DB_POOL_MAX];
static _Thread_local int tl_pool_slot = -1;
static _Thread_local PGconn *tl_pool_pg = NULL;
static _Thread_local char tl_db_err[512];

static int db_pool_limit(void) {
    const char *env = getenv("LAPLACE_DB_POOL_SIZE");
    if (!env || !*env) return DB_POOL_DEFAULT;
    char *end = NULL;
    long v = strtol(env, &end, 10);
    if (end == env || v < 1) return 1;
    if (v > DB_POOL_MAX) return DB_POOL_MAX;
    return (int)v;
}

const char *api_db_error(void) {
    return tl_db_err[0] ? tl_db_err : "database connection failed";
}

void api_db_release(void) {
    lpm_read_end();
    if (tl_pool_slot >= 0) {
        int slot = tl_pool_slot;
        tl_pool_slot = -1;
        tl_pool_pg = NULL;
        pthread_mutex_lock(&db_pool_mu);
        if (slot < DB_POOL_MAX) db_pool[slot].in_use = 0;
        pthread_mutex_unlock(&db_pool_mu);
    }
}

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
    tl_db_err[0] = 0;
    if (tl_pool_pg) {
        if (PQstatus(tl_pool_pg) == CONNECTION_OK &&
            (db_read_snapshot(tl_pool_pg) || db_read_begin(tl_pool_pg, NULL))) {
            lpm_read_scope(tl_pool_pg, ci);
            return tl_pool_pg;
        }
        api_db_release();
    }

    int cap = db_pool_limit();
    int slot = -1;
    PGconn *pg = NULL;
    int need_connect = 0;

    pthread_mutex_lock(&db_pool_mu);
    for (int i = 0; i < cap; i++) {
        if (!db_pool[i].in_use && db_pool[i].pg && db_pool[i].conninfo && !strcmp(db_pool[i].conninfo, ci)) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        for (int i = 0; i < cap; i++) {
            if (!db_pool[i].in_use) {
                slot = i;
                break;
            }
        }
    }
    if (slot >= 0) {
        db_pool[slot].in_use = 1;
        pg = db_pool[slot].pg;
        need_connect = !pg || PQstatus(pg) != CONNECTION_OK || !db_pool[slot].conninfo || strcmp(db_pool[slot].conninfo, ci) != 0;
        if (need_connect) {
            db_pool[slot].pg = NULL;
            free(db_pool[slot].conninfo);
            db_pool[slot].conninfo = NULL;
        }
    }
    pthread_mutex_unlock(&db_pool_mu);

    if (slot < 0) {
        snprintf(tl_db_err, sizeof tl_db_err, "database connection pool busy");
        return NULL;
    }

    if (need_connect) {
        if (pg) PQfinish(pg);
        pg = PQconnectdb(ci);
        if (!pg || PQstatus(pg) != CONNECTION_OK) {
            snprintf(tl_db_err, sizeof tl_db_err, "%s", pg ? PQerrorMessage(pg) : "out of memory");
            if (pg) PQfinish(pg);
            pthread_mutex_lock(&db_pool_mu);
            db_pool[slot].in_use = 0;
            pthread_mutex_unlock(&db_pool_mu);
            return NULL;
        }
        db_notices(pg);
        char *ci_copy = strdup(ci);
        pthread_mutex_lock(&db_pool_mu);
        db_pool[slot].pg = pg;
        db_pool[slot].conninfo = ci_copy;
        pthread_mutex_unlock(&db_pool_mu);
    }

    if (!db_read_snapshot(pg) && !db_read_begin(pg, NULL)) {
        snprintf(tl_db_err, sizeof tl_db_err, "%s", PQerrorMessage(pg));
        PQfinish(pg);
        pthread_mutex_lock(&db_pool_mu);
        db_pool[slot].pg = NULL;
        free(db_pool[slot].conninfo);
        db_pool[slot].conninfo = NULL;
        db_pool[slot].in_use = 0;
        pthread_mutex_unlock(&db_pool_mu);
        return NULL;
    }

    tl_pool_slot = slot;
    tl_pool_pg = pg;
    lpm_read_scope(pg, ci);
    return pg;
}

static int capture;
static int capture_code;
static char *capture_body;
static int stdio_capture;
static int stdio_code;
static char *stdio_body;

void api_http(int c, int code, const char *body) {
    api_db_release();
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
