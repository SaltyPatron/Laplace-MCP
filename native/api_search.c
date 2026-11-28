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
    /* a compressed file is what it holds: its extension is the one before .gz, as the Engine selects its recipe */
    char base[1024]; snprintf(base, sizeof base, "%s", path);
    size_t bl = strlen(base); if (bl > 3 && !strcasecmp(base + bl - 3, ".gz")) base[bl - 3] = 0;
    const char *dot = strrchr(base, '.'), *slash = strrchr(base, '/');
    if (!dot || (slash && dot < slash)) return 0;
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

/* ---- the query as its own Merkle DAG: every composition the text decomposer makes of it (lp_text_decompose), each
 * with its children, its text and whether a letter or digit is in it. The query is looked up by its IDs, top down:
 * the highest nodes the database holds are what the query shares with it, and below a held node nothing more is
 * asked (Query: the query is decomposed on the client and looked up by ID). */
typedef struct { lp_id id; uint32_t first, n; char *text; int word; } QNode;
typedef struct { lp_vec(QNode) node; lp_vec(lp_id) kid; } QDag;
static const QNode *qdag_node(const QDag *d, const lp_id *id) {
    for (size_t i = d->node.n; i-- > 0;) if (!memcmp(d->node.v[i].id.b, id->b, 16)) return &d->node.v[i];
    return NULL;
}
static lp_ref qdag_compose(void *sink, const lp_ref *ch, uint32_t n, uint8_t tier) {
    QDag *d = sink;
    lp_ref r = lp_ref_compose(ch, n, tier);
    if (qdag_node(d, &r.id)) return r;
    QNode x = { .id = r.id, .first = (uint32_t)d->kid.n, .n = n };
    lp_vec(char) text = { 0 };
    for (uint32_t i = 0; i < n; i++) {
        lp_push(&d->kid, ch[i].id);
        const QNode *k = qdag_node(d, &ch[i].id);
        if (k) { for (const char *c = k->text; *c; c++) lp_push(&text, *c); x.word |= k->word; continue; }
        int64_t cp = lp_tier0_codepoint(T0, &ch[i].id);                    /* a leaf: its codepoint */
        if (cp < 0) continue;
        uint8_t u[4]; size_t l = lp_utf8_put((uint32_t)cp, u);
        for (size_t j = 0; j < l; j++) lp_push(&text, (char)u[j]);
        x.word |= cp >= 0x80 || isalnum((int)cp);
    }
    lp_push(&text, 0);
    x.text = text.v;
    lp_push(&d->node, x);
    return r;
}
static void qdag_free(QDag *d) { for (size_t i = 0; i < d->node.n; i++) free(d->node.v[i].text); lp_vec_free(&d->node); lp_vec_free(&d->kid); }
/* Which of these IDs the database holds: one round trip (laplace_entities answers the held ones by ordinal). */
static uint8_t *held_ids(PGconn *pg, const lp_id *ids, size_t n) {
    uint8_t *held = calloc(n ? n : 1, 1);
    if (!n) return held;
    Args a = { 0 };
    arg_ids(&a, ids, n);
    char err[256];
    PGresult *r = db_call_try(pg, LP_SQL_ENTITIES_ORDINAL, &a, err, sizeof err);
    args_free(&a);
    if (r) {
        for (int row = 0; row < PQntuples(r); row++) {
            int64_t ord = col_i64(r, row, 0);
            if (ord > 0 && (size_t)(ord - 1) < n) held[ord - 1] = 1;
        }
        PQclear(r);
    }
    return held;
}
/* The query's terms: walked from its root down, each held node that holds a letter or digit is a term and nothing
 * under it is; a node the database does not hold is passed through to its children. The runs of segments with no
 * space between them (an identifier written in the query) the database holds are terms too, and a word inside such a
 * run is then the run's, not a term of its own. qinfo: how many nodes the query's DAG has and how many are held. */
static void query_terms(PGconn *pg, const char *s, Terms *t, size_t *qnodes, size_t *qheld) {
    memset(t, 0, sizeof *t);
    *qnodes = *qheld = 0;
    size_t n = strlen(s);
    if (!TX || !n) return;
    QDag d = { 0 };
    lp_ref root = lp_text_decompose(TX, (const uint8_t *)s, n, qdag_compose, &d);
    Terms runs;
    code_terms(s, &runs);                                                     /* words and the runs of segments */
    size_t nask = d.node.n + (size_t)runs.n;
    lp_id *ask = malloc(sizeof *ask * (nask ? nask : 1));
    for (size_t i = 0; i < d.node.n; i++) ask[i] = d.node.v[i].id;
    for (int i = 0; i < runs.n; i++) ask[d.node.n + (size_t)i] = runs.id[i];
    uint8_t *held = held_ids(pg, ask, nask);
    *qnodes = d.node.n;
    for (size_t i = 0; i < d.node.n; i++) *qheld += held[i];
    /* top down from the root, breadth first, each node once */
    lp_vec(lp_id) open = { 0 };
    lp_idmap *seen = lp_idmap_new();
    lp_push(&open, root.id);
    for (size_t at = 0; at < open.n && t->n < 64; at++) {
        lp_id id = open.v[at];
        bool fresh;
        lp_idmap_put(seen, &id, &fresh);
        if (!fresh) continue;
        const QNode *x = qdag_node(&d, &id);
        if (!x) {                                                             /* a codepoint: a term when held and a letter or digit */
            int64_t cp = lp_tier0_codepoint(T0, &id);
            if (cp >= 0 && (cp >= 0x80 || isalnum((int)cp))) { char u[8] = { 0 }; lp_utf8_put((uint32_t)cp, (uint8_t *)u); term_add(t, u, 0, strlen(u)); }
            continue;
        }
        if (held[x - d.node.v]) { if (x->word) term_add(t, x->text, 0, strlen(x->text)); continue; }
        for (uint32_t k = 0; k < x->n; k++) lp_push(&open, d.kid.v[x->first + k]);
    }
    /* the runs held: each a term, and the words within it its own no more */
    for (int i = 0; i < runs.n && t->n < 64; i++) {
        if (!held[d.node.n + (size_t)i]) continue;
        int words = 0;
        for (int w = 0; w < t->n; w++) if (strstr(runs.text[i], t->text[w])) words++;
        if (!words) continue;                                                 /* a run of no term held: nothing it shares */
        Terms kept = { 0 };
        for (int w = 0; w < t->n; w++) if (!strstr(runs.text[i], t->text[w]) || !strcmp(runs.text[i], t->text[w])) { kept.id[kept.n] = t->id[w]; memcpy(kept.text[kept.n], t->text[w], sizeof t->text[w]); kept.n++; }
        *t = kept;
        term_add(t, runs.text[i], 0, strlen(runs.text[i]));
    }
    lp_idmap_free(seen);
    lp_vec_free(&open);
    free(held);
    free(ask);
    qdag_free(&d);
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
    char err[256];
    PGresult *r = db_call_try(pg, LP_SQL_WITNESS_TRUST_ID_ORDERED, NULL, err, sizeof err);
    if (!r) { *witnesses = 0; return NULL; }
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

/* a positive count from the environment, dflt when it is not set; a value that is not one is refused (500), never
 * read as the default */
static int env_count(int c, const char *name, long dflt, long *out) {
    const char *v = getenv(name);
    *out = dflt;
    if (!v || !*v) return 1;
    char *e = NULL;
    long x = strtol(v, &e, 10);
    if (e == v || *e || x < 1) {
        char msg[192];
        snprintf(msg, sizeof msg, "{\"error\":\"%s is not a positive whole number\"}", name);
        api_http(c, 500, msg);
        return 0;
    }
    *out = x;
    return 1;
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

static char *bridge_query(const char *query, lp_buf *info, double *ms, const char **error) {
    uint64_t t0 = os_monotonic_ns();
    *error = NULL;
    const char *lang = getenv("LAPLACE_CODE_BRIDGE_LANGUAGE"), *nc = getenv("LAPLACE_CODE_BRIDGE_CONCEPTS");
    if (!lang || !*lang) lang = "en";
    char concepts[8] = "2", *end = NULL;
    if (nc && *nc) {
        long k = strtol(nc, &end, 10);
        if (end == nc || *end || k < 1 || k > 99) {
            *ms = 0;
            *error = "{\"error\":\"LAPLACE_CODE_BRIDGE_CONCEPTS is a whole number from 1 to 99\"}";
            return NULL;
        }
        snprintf(concepts, sizeof concepts, "%ld", k);
    }
    lp_buf out = { 0 }, seen = { 0 };
    lp_buf_cstr(&out, query);
    lp_buf_byte(&seen, '\n');
    lp_buf_byte(info, '{');
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
        lp_buf_byte(&seen, 0);
        if (bridge_has((const char *)seen.b, w)) { seen.n--; continue; }
        seen.n--;
        lp_buf_fmt(&seen, "%s\n", w);
        words++;
        char *av[] = { (char *)ENGINE, "translate", "-n", concepts, w, (char *)lang, (char *)lang, NULL };
        char *log = NULL;
        int code = 0;
        if (!os_run_captured(av, 20, (size_t)1 << 22, &log, &code) || code != 0) {
            free(log); lp_buf_free(&out); lp_buf_free(&seen);
            *error = "{\"error\":\"vocabulary bridge engine execution failed\"}";
            return NULL;
        }
        int listed = 0;
        lp_buf wj = { 0 };
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
                        lp_buf_byte(&seen, 0);
                        int dup = bridge_has((const char *)seen.b, syn);
                        seen.n--;
                        if (bridge_word_ok(syn) && !dup && listed < 8) {
                            lp_buf_fmt(&seen, "%s\n", syn);
                            lp_buf_fmt(&out, " %s", syn);
                            if (listed) lp_buf_byte(&wj, ',');
                            lp_json_str(&wj, syn, strlen(syn));
                            listed++;
                            added++;
                        }
                    }
                    q = *e ? e + 1 : e;
                }
            }
            line = nl ? nl + 1 : NULL;
        }
        if (listed) {
            if (!first) lp_buf_byte(info, ',');
            lp_json_str(info, w, strlen(w));
            lp_buf_cstr(info, ":[");
            lp_buf_put(info, wj.b, wj.n);
            lp_buf_byte(info, ']');
            first = 0;
        }
        lp_buf_free(&wj);
        free(log);
    }
    lp_buf_byte(info, '}');
    lp_buf_free(&seen);
    *ms = (double)(os_monotonic_ns() - t0) / 1e6;
    if (!added) { lp_buf_free(&out); return NULL; }
    lp_buf_byte(&out, 0);
    return (char *)out.b;
}

/* ---- one hit: a file, its score, and the unit of its content that answers */
typedef struct {
    lp_id file, unit;
    double score, prior, seed;
    uint64_t keys;
    int shared;
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
    long cap, most;
    if (!env_count(c, "LAPLACE_CODE_HUB", 4096, &cap) || !env_count(c, "LAPLACE_CODE_NODES", 400000, &most)) return;
    PGconn *pg = api_cc_db(c);
    if (!pg) return;
    uint64_t t0 = os_monotonic_ns();

    /* bridge (LAPLACE_CODE_BRIDGE): the query's words joined by what the database's wordnets attest of them */
    const char *be = getenv("LAPLACE_CODE_BRIDGE");
    int bridge = api_jbool(body, "bridge", be && *be && strcmp(be, "0"));
    lp_buf binfo = { 0 };
    double bridge_ms = 0;
    const char *bridge_error = NULL;
    char *bridged = bridge ? bridge_query(query, &binfo, &bridge_ms, &bridge_error) : NULL;
    if (bridge_error) { lp_buf_free(&binfo); api_http(c, 502, bridge_error); return; }
    Terms terms;
    size_t qnodes = 0, qheld = 0;
    query_terms(pg, bridged ? bridged : query, &terms, &qnodes, &qheld);
    free(bridged);
    if (!terms.n) { lp_buf_free(&binfo); api_http(c, 400, "{\"error\":\"the query holds no word\"}"); return; }

    Reader *rd = reader_new(pg);
    int witnesses = 0;
    uint64_t t_scope = os_monotonic_ns();
    lp_idmap *scope = code_scope(pg, rd, source, &witnesses);
    if (!witnesses || !scope) {
        double ms = (double)(os_monotonic_ns() - t0) / 1e6;
        lp_buf out = { 0 };
        lp_buf_fmt(&out,
                "{\"docs\":0,\"entities\":0,\"ms\":%.3f,\"model\":\"laplace-containment\",\"scope\":\"%s\","
                "\"source\":", ms, scope_s);
        lp_json_str(&out, source, strlen(source));
        lp_buf_fmt(&out,
                ",\"query\":{\"nodes\":%zu,\"held\":%zu},\"witnesses\":0,\"levels\":0,\"cut\":false,"
                "\"error\":\"requested source witness is not ingested\",\"candidates\":{\"reached\":0,\"kept\":0},\"hits\":[]}",
                qnodes, qheld);
        lp_buf_free(&binfo);
        reader_free(rd);
        api_reply_buf(c, 200, &out);
        return;
    }

    /* how common each term is: the paths that hold it directly, read from the container index at once; a term with
     * more than LAPLACE_CODE_HUB of a kind is a hub */
    uint64_t t_terms = os_monotonic_ns();
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
    uint64_t t_walk = os_monotonic_ns();
    uint64_t walk_hubs = 0;
    int levels = 0, cut = 0;
    size_t nfound = 0;
    HeldFile *found = nwalked ? held_in(pg, witnesses == 1 ? &SCOPE_TRUNK : NULL, walked, nwalked, -1,
                                        most, &walk_hubs, &levels, &cut, &nfound) : NULL;

    /* each file scores its unit by BM25 (k1 1.2, b 0.75) over the terms' occurrences; the unit's size is its tier (a
     * whole file's path is only its record and its content), against the mean of the units' */
    uint64_t t_rank = os_monotonic_ns();
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
    uint64_t t_units = os_monotonic_ns();
    lp_vec(lp_id) units = { 0 };
    /* every shown file's tree read in one walk, a level at a time for all of them */
    lp_id *shown_files = malloc(sizeof *shown_files * (shown ? shown : 1));
    char **texts = calloc(shown ? shown : 1, sizeof *texts);
    size_t *lens = calloc(shown ? shown : 1, sizeof *lens);
    for (size_t i = 0; i < shown; i++) shown_files[i] = hits.v[i].file;
    if (shown) fo_texts(pg, rd, shown_files, shown, texts, lens);
    for (size_t i = 0; i < shown; i++) {
        CodeHit *h = &hits.v[i];
        lp_push(&units, h->unit);
        size_t tn = lens[i];
        char *text = texts[i];
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
    free(shown_files);
    free(texts);
    free(lens);
    if (units.n) {
        Args a = { 0 };
        arg_ids(&a, units.v, units.n);
        char err[256];
        PGresult *r = db_call_try(pg, LP_SQL_ENTITIES_ORDINAL_HILBERT, &a, err, sizeof err);
        args_free(&a);
        if (r) {
            for (int row = 0; row < PQntuples(r); row++) {
                int64_t ord = col_i64(r, row, 0);
                if (ord > 0 && (size_t)(ord - 1) < shown) hits.v[ord - 1].hilbert = col_i64(r, row, 1);
            }
            PQclear(r);
        }
    }
    lp_vec_free(&units);

    double ms = (double)(os_monotonic_ns() - t0) / 1e6;
    lp_buf out = { 0 };
    lp_buf_fmt(&out,
            "{\"docs\":%zu,\"entities\":%zu,\"ms\":%.3f,\"model\":\"laplace-containment\",\"scope\":\"%s\","
            "\"source\":", scope ? lp_idmap_count(scope) : 0, nfound, ms, scope_s);
    lp_json_str(&out, source, strlen(source));
    lp_buf_fmt(&out, ",\"query\":{\"nodes\":%zu,\"held\":%zu},\"witnesses\":%d,\"levels\":%d,\"cut\":%s,"
            "\"phases_ms\":{\"scope\":%.1f,\"terms\":%.1f,\"walk\":%.1f,\"rank\":%.1f,\"text\":%.1f},\"terms\":[",
            qnodes, qheld, witnesses, levels, cut ? "true" : "false", (double)(t_terms - t_scope) / 1e6,
            (double)(t_walk - t_terms) / 1e6, (double)(t_rank - t_walk) / 1e6, (double)(t_units - t_rank) / 1e6,
            (double)(os_monotonic_ns() - t_units) / 1e6);
    for (int b = 0; b < terms.n; b++) {
        char id[33];
        int seed = 0;
        for (int w = 0; w < nwalked; w++) if (walked_term[w] == b) seed = 1;
        api_hex_id(&terms.id[b], id);
        lp_buf_fmt(&out, "%s{\"text\":", b ? "," : "");
        lp_json_str(&out, terms.text[b], strlen(terms.text[b]));
        lp_buf_fmt(&out, ",\"id\":\"%s\",\"holders\":%.0f,\"idf\":%.6f,\"hub\":%s,\"walked\":%s}", id, holders[b],
                idf[b], hubs >> b & 1 ? "true" : "false", seed ? "true" : "false");
    }
    if (bridge) {
        lp_buf_cstr(&out, "],\"bridge\":{\"words\":");
        lp_buf_put(&out, binfo.b, binfo.n);
        lp_buf_fmt(&out, ",\"ms\":%.3f}", bridge_ms);
    } else lp_buf_byte(&out, ']');
    lp_buf_free(&binfo);
    lp_buf_fmt(&out, ",\"candidates\":{\"reached\":%zu,\"kept\":%zu},\"hits\":[", reached, hits.n);
    for (size_t i = 0; i < shown; i++) {
        CodeHit *h = &hits.v[i];
        char id[33];
        api_hex_id(&h->unit, id);
        double score = total > 0 ? h->score * h->prior / (total * 2.2 / 1.3) : 0;
        lp_buf_fmt(&out, "%s{\"rank\":%zu,\"path\":", i ? "," : "", i + 1);
        lp_json_str(&out, h->path, strlen(h->path));
        lp_buf_fmt(&out,
                ",\"line0\":%zu,\"line1\":%zu,\"id\":\"%s\",\"hilbert\":\"%016llx\",\"shared\":%d,"
                "\"rerank\":%.6f,\"bm25\":%.6f,\"snippet\":",
                h->line0, h->line1, id, (unsigned long long)h->hilbert, h->shared, score, h->score);
        lp_json_str(&out, h->snippet ? h->snippet : "", strlen(h->snippet ? h->snippet : ""));
        lp_buf_byte(&out, '}');
    }
    lp_buf_cstr(&out, "]}");
    for (size_t i = 0; i < hits.n; i++) { free(hits.v[i].path); free(hits.v[i].snippet); }
    lp_vec_free(&hits);
    free(found);
    reader_free(rd);
    api_reply_buf(c, 200, &out);
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

/* ---- rerank: documents scored against a query by continuous substrate salience IDF + real unpacked 4D entity.coord
 * Fréchet curve distance (lp_frechet4), returning both typed channels cleanly with zero binary hub cliffs. */
typedef struct { uint32_t index, shared; double score, frechet4d; size_t bytes; } RerankItem;
static int cmp_rerank_item(const void *a, const void *b) {
    const RerankItem *x = a, *y = b;
    if (x->score != y->score) return x->score < y->score ? 1 : -1;
    if (x->frechet4d != y->frechet4d) return x->frechet4d < y->frechet4d ? -1 : 1;
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
    long cap;
    if (api_jint(body, "top_n", &top_n) && (top_n < 1 || (size_t)top_n > nd)) {
        for (size_t i = 0; i < nd; i++) free(docs[i]);
        api_http(c, 400, "{\"error\":\"top_n is from 1 to the number of documents\"}");
        return;
    }
    if (!env_count(c, "LAPLACE_CODE_HUB", 4096, &cap)) { for (size_t i = 0; i < nd; i++) free(docs[i]); return; }
    PGconn *pg = api_cc_db(c);
    if (!pg) { for (size_t i = 0; i < nd; i++) free(docs[i]); return; }
    uint64_t t0 = os_monotonic_ns();
    Terms q;
    code_terms(query, &q);
    double idf[64] = { 0 }, total = 0;
    if (q.n > 0) {
        int nsal = 0;
        LpSalienceRow *sal = salience_of(pg, q.id, q.n, NULL, 0, &nsal);
        for (int b = 0; b < q.n; b++) idf[b] = 1.0;
        for (int r = 0; r < nsal; r++) {
            if (sal[r].ord >= 1 && sal[r].ord <= q.n && sal[r].info_bits > 0.0)
                idf[sal[r].ord - 1] = sal[r].info_bits;
        }
        free(sal);
        for (int b = 0; b < q.n; b++) total += idf[b];
    }

    /* Collect all unique term IDs across query + documents for one batched real 4D entity.coord fetch. */
    Terms *doc_terms = calloc(nd, sizeof *doc_terms);
    lp_idmap *umap = lp_idmap_new();
    for (int b = 0; b < q.n; b++) lp_idmap_put(umap, &q.id[b], NULL);
    for (size_t i = 0; i < nd; i++) {
        code_terms(docs[i], &doc_terms[i]);
        for (int k = 0; k < doc_terms[i].n; k++) lp_idmap_put(umap, &doc_terms[i].id[k], NULL);
    }
    size_t nu = lp_idmap_count(umap);
    double *ucoords = calloc(nu ? nu * 4u : 4u, sizeof(double));
    uint8_t *uhas = calloc(nu ? nu : 1u, 1);
    if (nu > 0) coords_of(pg, lp_idmap_keys(umap), (int)nu, ucoords, uhas);

    double qcurve[64 * 4];
    size_t qclen = 0;
    for (int b = 0; b < q.n; b++) {
        int64_t u = lp_idmap_find(umap, &q.id[b]);
        if (u >= 0 && uhas[u]) { memcpy(qcurve + qclen * 4u, ucoords + (size_t)u * 4u, 32); qclen++; }
    }

    RerankItem *items = calloc(nd, sizeof *items);
    double dcurve[64 * 4];
    for (size_t i = 0; i < nd; i++) {
        const Terms *d = &doc_terms[i];
        items[i] = (RerankItem){ (uint32_t)i, 0, 0.0, INFINITY, strlen(docs[i]) };
        for (int b = 0; b < q.n; b++)
            for (int k = 0; k < d->n; k++)
                if (!memcmp(q.id[b].b, d->id[k].b, 16)) { items[i].shared++; items[i].score += idf[b]; break; }
        if (total > 0.0) items[i].score /= total;

        size_t dclen = 0;
        for (int k = 0; k < d->n; k++) {
            int64_t u = lp_idmap_find(umap, &d->id[k]);
            if (u >= 0 && uhas[u]) { memcpy(dcurve + dclen * 4u, ucoords + (size_t)u * 4u, 32); dclen++; }
        }
        if (qclen >= 1u && dclen >= 1u) {
            double fd = lp_frechet4(qcurve, qclen, dcurve, dclen);
            if (isfinite(fd) && fd >= 0.0) items[i].frechet4d = fd;
        }
        free(docs[i]);
    }
    free(doc_terms);
    lp_idmap_free(umap);
    free(ucoords);
    free(uhas);

    qsort(items, nd, sizeof *items, cmp_rerank_item);
    size_t shown = (size_t)top_n < nd ? (size_t)top_n : nd;
    double ms = (double)(os_monotonic_ns() - t0) / 1e6;
    lp_buf out = { 0 };
    lp_buf_fmt(&out, "{\"model\":\"laplace-salience-frechet4d\",\"ms\":%.3f,\"results\":[", ms);
    for (size_t i = 0; i < shown; i++) {
        lp_buf_fmt(&out, "%s{\"index\":%u,\"relevance_score\":%.6f,\"shared\":%u,\"frechet_4d\":",
                i ? "," : "", items[i].index, items[i].score, items[i].shared);
        if (isfinite(items[i].frechet4d)) lp_buf_fmt(&out, "%.6f}", items[i].frechet4d);
        else lp_buf_cstr(&out, "null}");
    }
    lp_buf_cstr(&out, "]}");
    free(items);
    api_reply_buf(c, 200, &out);
}
