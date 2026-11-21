#include "api_internal.h"
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

void api_hex_id(const lp_id *id, char out[33]) {
    lp_id_hex(id, out);
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
        pg = db_try_err(ci, tl_db_err, sizeof tl_db_err);
        if (!pg) {
            if (!tl_db_err[0]) snprintf(tl_db_err, sizeof tl_db_err, "database connection failed");
            pthread_mutex_lock(&db_pool_mu);
            db_pool[slot].in_use = 0;
            pthread_mutex_unlock(&db_pool_mu);
            return NULL;
        }
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

void api_http(int c, int code, const char *body) {
    api_db_release();
    lp_http_reply(c, code, body);
}

void api_capture_begin(int stdio_mode) {
    lp_http_capture_begin(stdio_mode);
}

const char *api_capture_end(int stdio_mode, int *code_out) {
    return lp_http_capture_end(stdio_mode, code_out);
}

/* JSON through the one reader and writer (laplace/json.h). */
static lp_json *api_jdoc(const char *body) {
    return body ? lp_json_parse_prefix(body, strlen(body), NULL) : NULL;
}

const char *api_jvalue(const char *body, const char *key) {
    lp_json *j = api_jdoc(body);
    const char *value = lp_json_text(j, lp_json_get(j, LP_JSON_ROOT, key), NULL);
    lp_json_free(j);
    return value;
}

int api_jstr(const char *body, const char *key, char *out, size_t cap) {
    lp_json *j = api_jdoc(body);
    size_t n = 0;
    const char *s = lp_json_string(j, lp_json_get(j, LP_JSON_ROOT, key), &n);
    int ok = s && n < cap && !memchr(s, 0, n);
    if (ok) { memcpy(out, s, n); out[n] = 0; }
    lp_json_free(j);
    return ok;
}

int api_jbool(const char *body, const char *key, int dflt) {
    lp_json *j = api_jdoc(body);
    uint32_t v = lp_json_get(j, LP_JSON_ROOT, key);
    bool b; const char *s = lp_json_string(j, v, NULL);
    int answer = lp_json_bool(j, v, &b) ? b : s && !strcmp(s, "true") ? 1 : s && !strcmp(s, "false") ? 0 : dflt;
    lp_json_free(j);
    return answer;
}

int api_jint(const char *body, const char *key, int *out) {
    lp_json *j = api_jdoc(body);
    int64_t v;
    int ok = lp_json_integer(j, lp_json_get(j, LP_JSON_ROOT, key), &v) && v >= INT_MIN && v <= INT_MAX;
    if (ok) *out = (int)v;
    lp_json_free(j);
    return ok;
}

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

void api_parts_json(const lp_ref *parts, size_t n, lp_buf *out) {
    for (size_t i = 0; i < n; i++) {
        char id[33];
        api_hex_id(&parts[i].id, id);
        lp_buf_fmt(out, "%s\"%s\"", i ? "," : "", id);
    }
}

int api_mask_claim(const char *m, int len) {
    if (len < 5) return 0;
    return ((const unsigned char *)m)[4] & 0x80;
}

void api_emit_mask_values(lp_buf *out, const char *wire, int len) {
    const lp_highway *highway = api_view_highway();
    lp_buf_byte(out, '[');
    if (highway && len == 12 && lp_be(wire, 4) == 64) {
        lp_mask mask = 0;
        const uint8_t *bytes = (const uint8_t *)wire + 4;
        for (unsigned bit = 0; bit < 64; bit++)
            if (bytes[bit >> 3] & (0x80u >> (bit & 7))) mask |= UINT64_C(1) << bit;
        int shown = 0;
        for (size_t i = 0; i < highway->nbanks; i++) {
            const lp_bank *bank = highway->bank + i;
            if (!(mask & lp_mask_field(bank))) continue;
            lp_buf_fmt(out, "%s{\"bank\":\"%s\",\"carrier\":\"%s\",\"encoding\":\"%s\",\"values\":[",
                       shown++ ? "," : "", bank->name, bank->carrier, bank->flags ? "flags" : "enum");
            unsigned values = 0;
            for (uint32_t slot = 0; slot < bank->list->count; slot++) {
                if (!lp_mask_has(mask, bank, slot)) continue;
                const lp_tier0_record *record = lp_highway_at(highway, bank->list, slot);
                if (!record) continue;
                char id[33];
                api_hex_id(&record->id, id);
                const char *label = lp_highway_identifier(highway, bank->list, slot);
                lp_buf_fmt(out, "%s{\"slot\":%u,\"id\":\"%s\",\"label\":", values++ ? "," : "", slot, id);
                lp_json_str(out, label ? label : "", strlen(label ? label : ""));
                lp_buf_byte(out, '}');
            }
            lp_buf_cstr(out, "]}");
        }
    }
    lp_buf_byte(out, ']');
}

void api_coord_json(const lp_ref *r, lp_buf *out) {
    lp_buf_fmt(out, "[%.17g,%.17g,%.17g,%.17g]",
               (double)r->c.m[0] / LP_FIXED_ONE, (double)r->c.m[1] / LP_FIXED_ONE,
               (double)r->c.m[2] / LP_FIXED_ONE, (double)r->c.m[3] / LP_FIXED_ONE);
}

void api_unit_coord_json(const lp_ref *r, lp_buf *out) {
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
    lp_buf_fmt(out, "[%.17g,%.17g,%.17g,%.17g]", v[0], v[1], v[2], v[3]);
}
