#define _GNU_SOURCE
#include "api_internal.h"

/* ---- code_search over the substrate: what an ingested source's files hold, read off the containment index. Nothing is
 * indexed here and nothing is read from a working tree.
 *
 * The query is decomposed as everything is (Native UAX #29): its word segments, and each run of segments with no space
 * between them (an identifier such as lp_ref_compose, a dotted or hyphenated name), are its terms, each the same entity
 * wherever that content was recorded. From the terms the Engine's reader walks up the container index to the files that
 * hold them (held_in: laplace_held_files, one call inside PostgreSQL), each file with the terms below it. A term held directly by more than LAPLACE_CODE_HUB
 * paths of a kind is a hub and is not walked. Files are those of the source's witness (LAPLACE_CODE_SOURCE, the witness's
 * name, default "Laplace Workspace"); each scores the inverse document frequency, over those files, of the terms it
 * holds. The retrieval unit of a file is the lowest node of its content that holds every term the file holds (their
 * common ancestor), else the lowest that holds the most of them; its lines are where its recorded text stands in the
 * file's recorded text. */

static int is_prose_or_data_path(const char *path) {
    if (!path) return 0;
    if (strstr(path, "/Laplace-Wiki/") || strstr(path, "\\Laplace-Wiki\\") ||
        strstr(path, "/docs/") || strstr(path, "\\docs\\")) return 1;
    const char *dot = strrchr(path, '.');
    if (!dot) return 0;
    static const char *const EXT[] = { ".md", ".txt", ".rst", ".adoc", ".json", ".tsv", ".csv", ".lmf", ".ttl", ".yml",
                                       ".yaml", ".recipe", ".format", ".env", ".in", ".cmake", ".control" };
    for (size_t i = 0; i < sizeof EXT / sizeof *EXT; i++) if (!strcasecmp(dot, EXT[i])) return 1;
    return 0;
}

static int is_test_path(const char *path) {
    if (!path) return 0;
    return strstr(path, "/tests/") != NULL || strstr(path, "/test/") != NULL ||
           strstr(path, ".Tests/") != NULL || strstr(path, "Tests.cs") != NULL ||
           strstr(path, "Test.cs") != NULL;
}

/* ---- the terms of a text: its word segments that hold a letter or digit, and its runs of segments with no space
 * between them, each once, at most 64, in the order they first stand. */
typedef struct { lp_id id[64]; char text[64][128]; int n; } Terms;
typedef struct { size_t at, len; } Seg;
typedef lp_vec(Seg) Segs;
static void seg_put(void *sink, size_t offset, size_t length, const lp_ref *value) {
    (void)value;
    lp_push((Segs *)sink, ((Seg){ offset, length }));
}
static int seg_word(const char *s, const Seg *g) {
    for (size_t i = 0; i < g->len; i++) {
        unsigned char ch = (unsigned char)s[g->at + i];
        if (ch >= 0x80 || isalnum(ch)) return 1;
    }
    return 0;
}
static int seg_space(const char *s, const Seg *g) {
    for (size_t i = 0; i < g->len; i++) if (!isspace((unsigned char)s[g->at + i])) return 0;
    return 1;
}
static void term_add(Terms *t, const char *s, size_t at, size_t len) {
    if (t->n >= 64 || !len) return;
    lp_id id;
    if (!lp_text_id(TX, s + at, len, &id)) return;
    for (int i = 0; i < t->n; i++) if (!memcmp(t->id[i].b, id.b, 16)) return;
    t->id[t->n] = id;
    size_t l = len < sizeof t->text[0] - 1 ? len : sizeof t->text[0] - 1;
    memcpy(t->text[t->n], s + at, l);
    t->text[t->n][l] = 0;
    t->n++;
}
static void code_terms(const char *s, Terms *t) {
    memset(t, 0, sizeof *t);
    size_t n = strlen(s);
    if (!TX || !n) return;
    Segs segs = { 0 };
    lp_text_segments(TX, (const uint8_t *)s, n, seg_put, &segs);
    for (size_t i = 0; i < segs.n;) {
        if (seg_space(s, &segs.v[i])) { i++; continue; }
        size_t j = i, words = 0;
        while (j < segs.n && !seg_space(s, &segs.v[j])) {
            if (seg_word(s, &segs.v[j])) { term_add(t, s, segs.v[j].at, segs.v[j].len); words++; }
            j++;
        }
        if (j - i > 1 && words) term_add(t, s, segs.v[i].at, segs.v[j - 1].at + segs.v[j - 1].len - segs.v[i].at);
        i = j;
    }
    lp_vec_free(&segs);
}

/* Inverse document frequency of a term held by df of n documents. */
static double term_idf(double df, double n) { return log(1.0 + (n - df + 0.5) / (df + 0.5)); }

/* ---- the files of a source: the witness or witnesses named so (laplace_witness_trust; the first part of each trunk is
 * its source record), and the files under each trunk, through whatever spine holds them. Kept until the witnesses change. */
static lp_idmap *SCOPE;
static char SCOPE_NAME[256];
static uint8_t SCOPE_SIG[16];
static int SCOPE_WITNESSES;
static lp_id SCOPE_TRUNK;             /* the witness, when exactly one is named so */
static lp_idmap *code_scope(PGconn *pg, Reader *rd, const char *name, int *witnesses) {
    PGresult *r = db_exec_params(pg, "SELECT witness FROM @extschema@.laplace_witness_trust() ORDER BY witness", 0, NULL, NULL, NULL, NULL, 1);
    if (PQresultStatus(r) != PGRES_TUPLES_OK) { PQclear(r); *witnesses = 0; return NULL; }
    int nw = PQntuples(r);
    lp_id *w = malloc(sizeof *w * (size_t)(nw ? nw : 1));
    uint8_t sig[16] = { 0 };
    for (int i = 0; i < nw; i++) {
        memcpy(w[i].b, PQgetvalue(r, i, 0), 16);
        for (int b = 0; b < 16; b++) sig[b] ^= w[i].b[b];
    }
    PQclear(r);
    if (SCOPE_NAME[0] && !strcmp(SCOPE_NAME, name) && !memcmp(SCOPE_SIG, sig, 16)) { free(w); *witnesses = SCOPE_WITNESSES; return SCOPE; }
    int nh = 0;
    Hold *h = paths_of(pg, w, nw, &nh);
    free(w);
    lp_vec(lp_id) records = { 0 };
    lp_vec(int) rows = { 0 };
    lp_vec(lp_vertex) vx = { 0 };
    for (int j = 0; j < nh; j++) {
        size_t nv = lp_path_vertices(h[j].path, (size_t)h[j].path_len, NULL, 0);
        if (!nv) continue;
        lp_vec_reserve(&vx, nv);
        lp_path_vertices(h[j].path, (size_t)h[j].path_len, vx.v, nv);
        lp_push(&records, vx.v[0].id);
        lp_push(&rows, j);
    }
    reader_prefetch(rd, records.v, records.n, sizeof SCOPE_NAME);
    lp_idmap *files = lp_idmap_new(), *front = lp_idmap_new();
    int matched = 0;
    for (size_t i = 0; i < records.n; i++) {
        char *t = reader_text(rd, &records.v[i], sizeof SCOPE_NAME);
        if (!strcmp(t, name)) {
            const Hold *x = &h[rows.v[i]];
            size_t nv = lp_path_vertices(x->path, (size_t)x->path_len, NULL, 0);
            lp_vec_reserve(&vx, nv);
            lp_path_vertices(x->path, (size_t)x->path_len, vx.v, nv);
            for (size_t k = 1; k < nv; k++) lp_idmap_put(front, &vx.v[k].id, NULL);
            SCOPE_TRUNK = x->entity;
            matched++;
        }
        free(t);
    }
    holds_free(h, nh);
    lp_vec_free(&records);
    lp_vec_free(&rows);
    /* down the spine to the files: a node whose first vertex is the OS's record is a file */
    for (int depth = 0; depth < 16 && lp_idmap_count(front); depth++) {
        Hold *p = paths_of(pg, lp_idmap_keys(front), (int)lp_idmap_count(front), &nh);
        lp_idmap *next = lp_idmap_new();
        for (int j = 0; j < nh; j++) {
            size_t nv = lp_path_vertices(p[j].path, (size_t)p[j].path_len, NULL, 0);
            if (!nv) continue;
            lp_vec_reserve(&vx, nv);
            lp_path_vertices(p[j].path, (size_t)p[j].path_len, vx.v, nv);
            if (vx.v[0].said == LP_SAID_METADATA) { lp_idmap_put(files, &p[j].entity, NULL); continue; }
            for (size_t k = 0; k < nv; k++) if (lp_idmap_find(files, &vx.v[k].id) < 0) lp_idmap_put(next, &vx.v[k].id, NULL);
        }
        holds_free(p, nh);
        lp_idmap_free(front);
        front = next;
    }
    lp_idmap_free(front);
    lp_vec_free(&vx);
    if (!matched) { lp_idmap_free(files); files = NULL; }
    lp_idmap_free(SCOPE);
    SCOPE = files;
    snprintf(SCOPE_NAME, sizeof SCOPE_NAME, "%s", name);
    memcpy(SCOPE_SIG, sig, 16);
    SCOPE_WITNESSES = matched;
    *witnesses = matched;
    return files;
}

static int env_weight(const char *name, double *w) {
    const char *v = getenv(name);
    if (!v || !*v) return 1;
    char *e = NULL;
    double x = strtod(v, &e);
    if (e == v || *e || !(x >= 0.0 && x <= 1.0)) return 0;
    *w = x;
    return 1;
}
static long env_long(const char *name, long dflt) {
    const char *v = getenv(name);
    if (!v || !*v) return dflt;
    char *e = NULL;
    long x = strtol(v, &e, 10);
    return e == v || *e ? dflt : x;
}

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

static char *bridge_query(const char *query, char *info, size_t icap, double *ms, const char **error) {
    uint64_t t0 = api_nsec();
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
        if (!api_engine_run(av, &log, &code, 20) || code != 0) {
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
                            api_json_esc(syn, se, sizeof se);
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
    *ms = (double)(api_nsec() - t0) / 1e6;
    if (!added) { free(out); return NULL; }
    return out;
}

/* ---- one hit: a file, its score, and the unit of its content that answers */
typedef struct {
    lp_id file, unit;
    double score, prior, seed;
    uint64_t keys;
    int shared, rank;
    char *path, *snippet;
    size_t line0, line1;
    int64_t hilbert;
} CodeHit;
static int hit_cmp(const void *a, const void *b) {
    const CodeHit *x = a, *y = b;
    double sx = x->score * x->prior, sy = y->score * y->prior;
    if (sx != sy) return sx < sy ? 1 : -1;
    if (x->shared != y->shared) return y->shared - x->shared;
    return memcmp(x->file.b, y->file.b, 16);
}
static int popcount64(uint64_t x) { int n = 0; while (x) { x &= x - 1; n++; } return n; }
static size_t lines_to(const char *text, size_t upto) {
    size_t l = 1;
    for (size_t i = 0; i < upto && text[i]; i++) if (text[i] == '\n') l++;
    return l;
}

void on_code_search(int c, const char *body) {
    char query[8192] = "", under[4096] = "", source[256] = "";
    if (!api_jstr(body, "query", query, sizeof query) && !api_jstr(body, "input", query, sizeof query)) {
        api_http(c, 400, "{\"error\":\"query\"}");
        return;
    }
    if (!query[0]) { api_http(c, 400, "{\"error\":\"empty query\"}"); return; }
    int limit = 15;
    api_jint(body, "limit", &limit);
    api_jint(body, "top_k", &limit);
    if (limit < 1) limit = 1;
    if (limit > 64) limit = 64;
    api_jstr(body, "path", under, sizeof under);
    const char *es = getenv("LAPLACE_CODE_SOURCE");
    if (!api_jstr(body, "source", source, sizeof source) || !source[0])
        snprintf(source, sizeof source, "%s", es && *es ? es : "Laplace Workspace");
    char scope_s[8] = "all";
    if (api_jstr(body, "scope", scope_s, sizeof scope_s) && scope_s[0]) {
        if (strcmp(scope_s, "all") && strcmp(scope_s, "code") && strcmp(scope_s, "docs")) {
            api_http(c, 400, "{\"error\":\"scope is all, code or docs\"}");
            return;
        }
    } else snprintf(scope_s, sizeof scope_s, "all");
    char fusion[16] = "";
    if (!api_jstr(body, "fusion", fusion, sizeof fusion) || !fusion[0]) {
        const char *fe = getenv("LAPLACE_CODE_FUSION");
        snprintf(fusion, sizeof fusion, "%s", fe && *fe ? fe : "rerank");
    }
    if (strcmp(fusion, "rrf") && strcmp(fusion, "rerank")) { api_http(c, 400, "{\"error\":\"fusion is rrf or rerank\"}"); return; }
    double w_rr = 1.0;
    if (!env_weight("LAPLACE_CODE_RERANK_WEIGHT", &w_rr)) {
        api_http(c, 400, "{\"error\":\"LAPLACE_CODE_RERANK_WEIGHT is a number from 0 to 1\"}");
        return;
    }
    PGconn *pg = api_cc_db(c);
    if (!pg) return;
    uint64_t t0 = api_nsec();

    /* bridge (LAPLACE_CODE_BRIDGE): the query's words joined by what the database's wordnets attest of them */
    const char *be = getenv("LAPLACE_CODE_BRIDGE");
    int bridge = api_jbool(body, "bridge", be && *be && strcmp(be, "0"));
    char binfo[8192] = "{}";
    double bridge_ms = 0;
    const char *bridge_error = NULL;
    char *bridged = bridge ? bridge_query(query, binfo, sizeof binfo, &bridge_ms, &bridge_error) : NULL;
    if (bridge_error) { api_http(c, 502, bridge_error); return; }
    Terms terms;
    code_terms(bridged ? bridged : query, &terms);
    free(bridged);
    if (!terms.n) { api_http(c, 400, "{\"error\":\"the query holds no word\"}"); return; }

    Reader *rd = reader_new(pg);
    int witnesses = 0;
    uint64_t t_scope = api_nsec();
    lp_idmap *scope = code_scope(pg, rd, source, &witnesses);

    /* how common each term is: the paths that hold it directly, read from the container index at once; a term with
     * more than LAPLACE_CODE_HUB of a kind is a hub */
    uint64_t t_terms = api_nsec();
    long cap = env_long("LAPLACE_CODE_HUB", 4096);
    long most = env_long("LAPLACE_CODE_NODES", 400000);
    double holders[64] = { 0 }, idf[64] = { 0 }, total = 0;
    uint64_t hubs = 0;
    {
        int *hub = calloc((size_t)terms.n * 2, sizeof *hub), nh = 0;
        Hold *h = holds_capped(pg, terms.id, terms.n, (int)cap, 0, 3, hub, &nh);
        for (int j = 0; j < nh; j++) if (h[j].src >= 0 && h[j].src < terms.n) holders[h[j].src]++;
        holds_free(h, nh);
        for (int b = 0; b < terms.n; b++) if (hub[2 * b] || hub[2 * b + 1]) { hubs |= 1ull << b; holders[b] = (double)cap + 1; }
        free(hub);
    }
    /* a term's weight: its inverse frequency among 64 times the hub cap of paths. Hubs are not walked. */
    double universe = 64.0 * ((double)cap + 1);
    for (int b = 0; b < terms.n; b++) if (holders[b] > 0 && !(hubs >> b & 1)) { idf[b] = term_idf(holders[b], universe); total += idf[b]; }

    /* every term that is held and no hub, walked up the container index to the source's files in one call inside
     * PostgreSQL (held_in), each file with its unit, the unit's length and each term's occurrences below it */
    lp_id walked[64];
    int walked_term[64], nwalked = 0;
    for (int b = 0; b < terms.n; b++)
        if (holders[b] > 0 && !(hubs >> b & 1)) { walked[nwalked] = terms.id[b]; walked_term[nwalked++] = b; }
    uint64_t t_walk = api_nsec();
    uint64_t walk_hubs = 0;
    int levels = 0, cut = 0;
    size_t nfound = 0;
    HeldFile *found = nwalked ? held_in(pg, witnesses == 1 ? &SCOPE_TRUNK : NULL, walked, nwalked, -1,
                                        most > 0 ? most : 400000, &walk_hubs, &levels, &cut, &nfound) : NULL;

    /* each file scores its unit by BM25 (k1 1.2, b 0.75) over the terms' occurrences; the unit's size is its tier (a
     * whole file's path is only its record and its content), against the mean of the units' */
    uint64_t t_rank = api_nsec();
    double mean = 0;
    size_t counted = 0;
    for (size_t i = 0; i < nfound; i++) if (!scope || lp_idmap_find(scope, &found[i].file) >= 0) { mean += (double)found[i].tier; counted++; }
    mean = counted && mean > 0 ? mean / (double)counted : 1;
    lp_vec(CodeHit) hits = { 0 };
    for (size_t i = 0; i < nfound; i++) {
        const HeldFile *f = &found[i];
        if (!f->keys || (scope && lp_idmap_find(scope, &f->file) < 0)) continue;
        CodeHit h = { .file = f->file, .unit = f->unit, .prior = 1.0 };
        double norm = 1.2 * (0.25 + 0.75 * (double)f->tier / mean);
        for (int w = 0; w < nwalked && w < 64; w++) {
            if (!(f->keys >> w & 1)) continue;
            double tf = f->count[w] ? (double)f->count[w] : 1;
            h.score += idf[walked_term[w]] * tf * 2.2 / (tf + norm);
            h.keys |= 1ull << walked_term[w];
        }
        h.shared = popcount64(h.keys);
        lp_push(&hits, h);
    }
    size_t reached = hits.n;
    qsort(hits.v, hits.n, sizeof *hits.v, hit_cmp);

    /* the leading files' paths: the scope and path filters and, over all scopes, prose and tests weigh less */
    size_t want = (size_t)limit * 3;
    if (want < 32) want = 32;
    if (want > hits.n) want = hits.n;
    lp_vec(lp_id) lead = { 0 };
    for (size_t i = 0; i < want; i++) lp_push(&lead, hits.v[i].file);
    reader_prefetch(rd, lead.v, lead.n, 1024);
    lp_vec_free(&lead);
    size_t kept = 0;
    for (size_t i = 0; i < want; i++) {
        CodeHit h = hits.v[i];
        h.path = api_cw_file_name(rd, &h.file);
        int doc = is_prose_or_data_path(h.path);
        if ((under[0] && !strstr(h.path, under)) || (!strcmp(scope_s, "code") && doc) || (!strcmp(scope_s, "docs") && !doc)) {
            free(h.path);
            continue;
        }
        if (!strcmp(scope_s, "all")) {
            const char *dot = strrchr(h.path, '.');
            if (doc) h.prior = 0.35;
            else if (is_test_path(h.path)) h.prior = 0.25;
            else if (dot && (!strcasecmp(dot, ".h") || !strcasecmp(dot, ".hpp"))) h.prior = 0.85;
        }
        hits.v[kept++] = h;
    }
    hits.n = kept;
    qsort(hits.v, hits.n, sizeof *hits.v, hit_cmp);
    int explain = api_jbool(body, "explain", 0);
    size_t shown = explain ? hits.n : hits.n < (size_t)limit ? hits.n : (size_t)limit;

    /* text only for what is returned: each shown unit's lines in its file's recorded text, and its first 60 lines */
    uint64_t t_units = api_nsec();
    lp_vec(lp_id) units = { 0 };
    for (size_t i = 0; i < shown; i++) {
        CodeHit *h = &hits.v[i];
        h->rank = (int)i + 1;
        lp_push(&units, h->unit);
        size_t tn = 0;
        char *text = fo_text(pg, rd, &h->file, &tn);
        int whole = !memcmp(h->unit.b, h->file.b, 16);
        char *unit = whole ? strdup("") : reader_text(rd, &h->unit, (size_t)1 << 20);
        const char *at = unit[0] ? strstr(text, unit) : NULL;
        size_t start = at ? (size_t)(at - text) : 0, length = at ? strlen(unit) : tn;
        h->line0 = lines_to(text, start);
        h->line1 = lines_to(text, start + length) - (length && text[start + length - 1] == '\n');
        size_t end = start, nl = 0;
        while (end < start + length && nl < 60) { if (text[end] == '\n') nl++; end++; }
        if (end - start > 8192) { end = start + 8192; while (end > start && ((uint8_t)text[end] & 0xc0) == 0x80) end--; }
        h->snippet = malloc(end - start + 1);
        memcpy(h->snippet, text + start, end - start);
        h->snippet[end - start] = 0;
        free(unit);
        free(text);
    }
    if (units.n) {
        uint8_t *param = malloc(20 + 20 * units.n);
        size_t pl = ids_param(pg, param, units.v, (uint32_t)units.n);
        const char *pv[1] = { (const char *)param };
        int plen[1] = { (int)pl }, pf[1] = { 1 };
        PGresult *r = db_exec_params(pg, "SELECT ordinal, hilbert FROM @extschema@.laplace_entities($1::@extschema@.blake3[])", 1, NULL, pv, plen, pf, 1);
        if (PQresultStatus(r) == PGRES_TUPLES_OK)
            for (int row = 0; row < PQntuples(r); row++) {
                size_t i = (size_t)lp_be(PQgetvalue(r, row, 0), 8) - 1;
                if (i < shown) hits.v[i].hilbert = (int64_t)lp_be(PQgetvalue(r, row, 1), 8);
            }
        PQclear(r);
        free(param);
    }
    lp_vec_free(&units);

    double ms = (double)(api_nsec() - t0) / 1e6;
    char *out = NULL;
    size_t un = 0, ucap = 0;
    api_buf_fmt(&out, &un, &ucap,
            "{\"docs\":%zu,\"entities\":%zu,\"ms\":%.3f,\"model\":\"laplace-containment\",\"fusion\":\"%s\",\"scope\":\"%s\","
            "\"source\":\"", scope ? lp_idmap_count(scope) : 0, nfound, ms, fusion, scope_s);
    api_cw_str(&out, &un, &ucap, source);
    api_buf_fmt(&out, &un, &ucap, "\",\"witnesses\":%d,\"weights\":{\"rerank\":%g},\"levels\":%d,\"cut\":%s,"
            "\"phases_ms\":{\"scope\":%.1f,\"terms\":%.1f,\"walk\":%.1f,\"rank\":%.1f,\"text\":%.1f},\"terms\":[",
            witnesses, w_rr, levels, cut ? "true" : "false", (double)(t_terms - t_scope) / 1e6,
            (double)(t_walk - t_terms) / 1e6, (double)(t_rank - t_walk) / 1e6, (double)(t_units - t_rank) / 1e6,
            (double)(api_nsec() - t_units) / 1e6);
    for (int b = 0; b < terms.n; b++) {
        char id[33];
        int seed = 0;
        for (int w = 0; w < nwalked; w++) if (walked_term[w] == b) seed = 1;
        api_hex_id(&terms.id[b], id);
        api_buf_fmt(&out, &un, &ucap, "%s{\"text\":\"", b ? "," : "");
        api_cw_str(&out, &un, &ucap, terms.text[b]);
        api_buf_fmt(&out, &un, &ucap, "\",\"id\":\"%s\",\"holders\":%.0f,\"idf\":%.6f,\"hub\":%s,\"walked\":%s}", id, holders[b],
                idf[b], hubs >> b & 1 ? "true" : "false", seed ? "true" : "false");
    }
    if (bridge) api_buf_fmt(&out, &un, &ucap, "],\"bridge\":{\"words\":%s,\"ms\":%.3f}", binfo, bridge_ms);
    else api_buf_add(&out, &un, &ucap, "]", 1);
    api_buf_fmt(&out, &un, &ucap, ",\"candidates\":{\"lexical\":%zu,\"reranked\":%zu},\"hits\":[", reached, hits.n);
    for (size_t i = 0; i < shown; i++) {
        CodeHit *h = &hits.v[i];
        char id[33];
        api_hex_id(&h->unit, id);
        double score = total > 0 ? h->score * h->prior / (total * 2.2 / 1.3) : 0;
        api_buf_fmt(&out, &un, &ucap, "%s{\"rank\":%zu,\"path\":\"", i ? "," : "", i + 1);
        api_cw_str(&out, &un, &ucap, h->path);
        api_buf_fmt(&out, &un, &ucap,
                "\",\"line0\":%zu,\"line1\":%zu,\"source\":\"laplace\",\"id\":\"%s\",\"hilbert\":\"%016llx\",\"shared\":%d,"
                "\"frechet\":-1,\"rerank\":%.6f,\"fused\":%.6f,\"ranks\":{\"rerank\":%d,\"lexical\":%d},\"snippet\":\"",
                h->line0, h->line1, id, (unsigned long long)h->hilbert, h->shared, score, h->score, h->rank, h->rank);
        api_cw_str(&out, &un, &ucap, h->snippet ? h->snippet : "");
        api_buf_add(&out, &un, &ucap, "\"}", 2);
    }
    api_buf_add(&out, &un, &ucap, "]}", 2);
    for (size_t i = 0; i < hits.n; i++) { free(hits.v[i].path); free(hits.v[i].snippet); }
    lp_vec_free(&hits);
    free(found);
    reader_free(rd);
    api_http(c, 200, out ? out : "{\"hits\":[]}");
    free(out);
}

/* reindex: the code is the database's, so bringing it up to date is ingesting its source again (LAPLACE_CODE_INGEST,
 * default workspace-code, whose recipe supersedes what changed), or one file or directory given as path. */
void on_reindex(int c, const char *body) {
    char path[4096] = "";
    api_jstr(body, "path", path, sizeof path);
    const char *src = getenv("LAPLACE_CODE_INGEST");
    if (path[0]) {
        if (!(path[0] == '/' || (path[0] && path[1] == ':'))) { api_http(c, 400, "{\"error\":\"path is absolute\"}"); return; }
        if (!os_exists(path)) { api_http(c, 400, "{\"error\":\"path does not exist\"}"); return; }
    }
    char *av[] = { (char *)ENGINE, "ingest", path[0] ? path : (char *)(src && *src ? src : "workspace-code"), NULL };
    api_on_engine(c, av);
}

/* ---- rerank: documents scored against a query by the inverse frequency, over the database's containment, of the
 * query's terms each holds (the terms as code_search reads them), as a share of all the query's terms. A term's
 * frequency is how many paths hold it directly; LAPLACE_CODE_HUB and more are alike. */
typedef struct { uint32_t index, shared; double score; size_t bytes; } RerankItem;
static int cmp_rerank_item(const void *a, const void *b) {
    const RerankItem *x = a, *y = b;
    if (x->score != y->score) return x->score < y->score ? 1 : -1;
    if (x->bytes != y->bytes) return x->bytes < y->bytes ? -1 : 1;
    return (x->index > y->index) - (x->index < y->index);
}

void on_rerank(int c, const char *body) {
    char query[8192] = "";
    if (!api_jstr(body, "query", query, sizeof query) && !api_jstr(body, "input", query, sizeof query)) {
        api_http(c, 400, "{\"error\":\"query\"}");
        return;
    }
    char *docs[256];
    size_t nd = api_parse_str_array(body, "documents", docs, 256);
    if (!nd) { api_http(c, 400, "{\"error\":\"documents\"}"); return; }
    int top_n = (int)nd;
    if (api_jint(body, "top_n", &top_n) && (top_n < 1 || (size_t)top_n > nd)) top_n = (int)nd;
    PGconn *pg = api_cc_db(c);
    if (!pg) { for (size_t i = 0; i < nd; i++) free(docs[i]); return; }
    uint64_t t0 = api_nsec();
    Terms q;
    code_terms(query, &q);
    long cap = env_long("LAPLACE_CODE_HUB", 4096);
    double df[64] = { 0 }, idf[64] = { 0 }, total = 0;
    int *hub = calloc((size_t)(q.n ? q.n : 1) * 2, sizeof *hub), nh = 0;
    Hold *h = q.n ? holds_capped(pg, q.id, q.n, (int)cap, 0, 3, hub, &nh) : NULL;
    for (int j = 0; j < nh; j++) if (h[j].src >= 0 && h[j].src < q.n) df[h[j].src]++;
    holds_free(h, nh);
    for (int b = 0; b < q.n; b++) {
        if (hub[2 * b] || hub[2 * b + 1]) df[b] = (double)cap + 1;
        idf[b] = term_idf(df[b], (double)cap + 1);
        total += idf[b];
    }
    free(hub);
    RerankItem *items = calloc(nd, sizeof *items);
    Terms *d = malloc(sizeof *d);
    for (size_t i = 0; i < nd; i++) {
        code_terms(docs[i], d);
        items[i] = (RerankItem){ (uint32_t)i, 0, 0, strlen(docs[i]) };
        for (int b = 0; b < q.n; b++)
            for (int k = 0; k < d->n; k++)
                if (!memcmp(q.id[b].b, d->id[k].b, 16)) { items[i].shared++; items[i].score += idf[b]; break; }
        if (total > 0) items[i].score /= total;
        free(docs[i]);
    }
    free(d);
    qsort(items, nd, sizeof *items, cmp_rerank_item);
    size_t shown = (size_t)top_n < nd ? (size_t)top_n : nd;
    double ms = (double)(api_nsec() - t0) / 1e6;
    char *out = NULL;
    size_t un = 0, ucap = 0;
    api_buf_fmt(&out, &un, &ucap, "{\"model\":\"laplace-containment\",\"ms\":%.3f,\"results\":[", ms);
    for (size_t i = 0; i < shown; i++)
        api_buf_fmt(&out, &un, &ucap, "%s{\"index\":%u,\"relevance_score\":%.6f,\"shared\":%u,\"frechet\":-1}",
                i ? "," : "", items[i].index, items[i].score, items[i].shared);
    api_buf_add(&out, &un, &ucap, "]}", 2);
    free(items);
    api_http(c, 200, out ? out : "{\"results\":[]}");
    free(out);
}
