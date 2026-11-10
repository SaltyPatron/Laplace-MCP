#define _GNU_SOURCE
#include "api_internal.h"
#include "lpm_internal.h"

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

static const char *tail_path_segments(const char *path, int nseg) {
    if (!path || !*path) return "";
    const char *p = path + strlen(path);
    int seen = 0;
    while (p > path) {
        p--;
        if (*p == '/' || *p == '\\') {
            seen++;
            if (seen == nseg) return p + 1;
        }
    }
    return path;
}

static double basename_stem_boost(const char *path, const char *query) {
    const char *base = tail_path_segments(path, 1);
    if (!base || !*base || !query || !*query) return 1.0;
    size_t blen = strlen(base);
    const char *dot = strrchr(base, '.');
    if (dot && dot > base) blen = (size_t)(dot - base);

    uint8_t qstems[64][64];
    size_t qslen[64];
    size_t nq = 0;
    size_t i = 0, qn = strlen(query);
    while (i < qn && nq < 64) {
        while (i < qn && !lpm_is_ascii_alnum((uint8_t)query[i])) i++;
        if (i >= qn) break;
        size_t w0 = i;
        while (i < qn && lpm_is_ascii_alnum((uint8_t)query[i])) i++;
        size_t sub0 = w0;
        for (size_t k = w0 + 1; k <= i && nq < 64; k++) {
            int split = (k == i);
            if (!split) {
                uint8_t prev = (uint8_t)query[k - 1], cur = (uint8_t)query[k];
                int prev_lo = (prev >= 'a' && prev <= 'z');
                int prev_up = (prev >= 'A' && prev <= 'Z');
                int prev_dg = (prev >= '0' && prev <= '9');
                int cur_up  = (cur >= 'A' && cur <= 'Z');
                int cur_dg  = (cur >= '0' && cur <= '9');
                int next_lo = (k + 1 < i) && (query[k + 1] >= 'a' && query[k + 1] <= 'z');
                if ((prev_lo && cur_up) || (prev_up && cur_up && next_lo) || (prev_dg != cur_dg)) split = 1;
            }
            if (split) {
                size_t slen = k - sub0;
                if (slen >= 3 && slen <= 64) {
                    qslen[nq] = lpm_stem_ascii((const uint8_t *)query + sub0, slen, qstems[nq]);
                    if (qslen[nq] >= 3) nq++;
                }
                sub0 = k;
            }
        }
    }

    int matches = 0;
    i = 0;
    while (i < blen) {
        while (i < blen && !lpm_is_ascii_alnum((uint8_t)base[i])) i++;
        if (i >= blen) break;
        size_t w0 = i;
        while (i < blen && lpm_is_ascii_alnum((uint8_t)base[i])) i++;
        size_t sub0 = w0;
        for (size_t k = w0 + 1; k <= i; k++) {
            int split = (k == i);
            if (!split) {
                uint8_t prev = (uint8_t)base[k - 1], cur = (uint8_t)base[k];
                int prev_lo = (prev >= 'a' && prev <= 'z');
                int prev_up = (prev >= 'A' && prev <= 'Z');
                int prev_dg = (prev >= '0' && prev <= '9');
                int cur_up  = (cur >= 'A' && cur <= 'Z');
                int cur_dg  = (cur >= '0' && cur <= '9');
                int next_lo = (k + 1 < i) && (base[k + 1] >= 'a' && base[k + 1] <= 'z');
                if ((prev_lo && cur_up) || (prev_up && cur_up && next_lo) || (prev_dg != cur_dg)) split = 1;
            }
            if (split) {
                size_t slen = k - sub0;
                if (slen >= 3 && slen <= 64) {
                    uint8_t bstem[64];
                    size_t bslen = lpm_stem_ascii((const uint8_t *)base + sub0, slen, bstem);
                    if (bslen >= 3) {
                        for (size_t m = 0; m < nq; m++) {
                            if (qslen[m] == bslen && !memcmp(qstems[m], bstem, bslen)) {
                                matches++;
                                break;
                            }
                        }
                    }
                }
                sub0 = k;
            }
        }
    }
    if (!matches) return 1.0;
    if (matches == 1) return 1.03;
    double boost = 1.0 + 0.14 * (double)matches;
    return boost > 1.35 ? 1.35 : boost;
}

/* A code_search candidate from the multi-tier Laplace AST/text index, reranked by 4D Frechet trajectory distance. */
typedef struct {
    char path[4096];
    uint32_t line0, line1;
    int lexical;
    int lex_rank, rr_rank;          /* where the constituent retrieval and the 4D Frechet rerank put it, from 1 */
    double fused;
    uint8_t id[16];
    uint64_t hilbert;
    uint32_t shared;
    double rerank, frechet;
    char *snippet;
} CodeCand;

static int cand_rerank_cmp(const void *a, const void *b) {
    const CodeCand *x = *(CodeCand *const *)a, *y = *(CodeCand *const *)b;
    if (x->rerank != y->rerank) return x->rerank < y->rerank ? 1 : -1;
    if (x->shared != y->shared) return x->shared < y->shared ? 1 : -1;
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

void on_code_search(int c, const char *body) {
    char query[8192] = "", path[4096] = "";
    if (!api_jstr(body, "query", query, sizeof query) && !api_jstr(body, "input", query, sizeof query)) {
        api_http(c, 400, "{\"error\":\"query\"}");
        return;
    }
    if (!query[0]) {
        api_http(c, 400, "{\"error\":\"empty query\"}");
        return;
    }
    int limit = 15;
    api_jint(body, "limit", &limit);
    api_jint(body, "top_k", &limit);
    if (limit < 1) limit = 1;
    if (limit > 64) limit = 64;
    uint64_t t0 = api_nsec();
    Lpm *ix = ensure_code_ix();
    if (!ix) {
        api_http(c, 500, "{\"error\":\"code index\"}");
        return;
    }
    LpmStats st = {0};
    lpm_stats(ix, &st);
    if (api_jstr(body, "path", path, sizeof path) && path[0]) {
        if (!os_exists(path)) {
            api_http(c, 404, "{\"error\":\"code indexing path does not exist\"}");
            return;
        }
        LpmUpdate update = {0};
        if (lpm_update_path(ix, path, &update)) {
            api_http(c, 500, "{\"error\":\"code indexing path could not be refreshed\"}");
            return;
        }
    } else if (st.documents == 0 && os_exists(".")) {
        lpm_index_path(ix, ".");
    }
    lpm_stats(ix, &st);

    char scope_s[8] = "all";
    if (api_jstr(body, "scope", scope_s, sizeof scope_s) && scope_s[0]) {
        if (strcmp(scope_s, "all") && strcmp(scope_s, "code") && strcmp(scope_s, "docs")) {
            api_http(c, 400, "{\"error\":\"scope is all, code or docs\"}");
            return;
        }
    } else {
        snprintf(scope_s, sizeof scope_s, "all");
    }

    double w_rr = 1.0;
    if (!env_weight("LAPLACE_CODE_RERANK_WEIGHT", &w_rr)) {
        api_http(c, 400, "{\"error\":\"LAPLACE_CODE_RERANK_WEIGHT is a number from 0 to 1\"}");
        return;
    }

    /* bridge (LAPLACE_CODE_BRIDGE): the query's words joined by what the database's wordnets attest of them */
    const char *be = getenv("LAPLACE_CODE_BRIDGE");
    int bridge = api_jbool(body, "bridge", be && *be && strcmp(be, "0"));
    char binfo[8192] = "{}";
    double bridge_ms = 0;
    const char *bridge_error = NULL;
    char *bridged = bridge ? bridge_query(query, binfo, sizeof binfo, &bridge_ms, &bridge_error) : NULL;
    if (bridge_error) {
        api_http(c, 502, bridge_error);
        return;
    }
    const char *rq = bridged ? bridged : query;

    size_t want = (size_t)limit * 5;
    if (strcmp(scope_s, "all")) want = 192;
    if (want < 64) want = 64;
    if (want > 192) want = 192;
    LpmHit hits[192];
    size_t nh = lpm_search(ix, rq, strlen(rq), hits, want);
    CodeCand *cand = calloc(nh + 1, sizeof *cand);
    if (!cand) {
        free(bridged);
        api_http(c, 500, "{\"error\":\"out of memory\"}");
        return;
    }
    size_t nc = 0;
    for (size_t i = 0; i < nh; i++) {
        int is_doc = lpm_is_prose_or_data_path(hits[i].path);
        if (!strcmp(scope_s, "code") && is_doc) continue;
        if (!strcmp(scope_s, "docs") && !is_doc) continue;
        CodeCand *k = &cand[nc++];
        snprintf(k->path, sizeof k->path, "%s", hits[i].path);
        k->line0 = hits[i].line0; k->line1 = hits[i].line1;
        k->lexical = 1;
        k->lex_rank = (int)nc;
        memcpy(k->id, hits[i].id, 16);
        k->hilbert = hits[i].hilbert;
        k->shared = hits[i].shared;
    }

    /* Laplace orders the candidates: 4D S^3 Frechet trajectory + Hausdorff + length-chance-adjusted IDF rerank. */
    for (size_t i = 0; i < nc; i++) {
        CodeCand *k = &cand[i];
        char snip[8192] = "";
        read_lines_snippet(k->path, k->line0, k->line1, snip, sizeof snip);
        k->snippet = strdup(snip);
        k->frechet = -1.0;
        if (snip[0]) {
            char hdr[384] = "", hhdr[384] = "";
            if (k->line0 > 8 && !lpm_is_prose_or_data_path(k->path)) {
                read_lines_snippet(k->path, 1, 8, hdr, sizeof hdr);
            }
            const char *dot = strrchr(k->path, '.');
            if (dot && !strcmp(dot, ".c") && strlen(k->path) < 4090) {
                char hpath[4096];
                snprintf(hpath, sizeof hpath, "%s", k->path);
                char *hdot = strrchr(hpath, '.');
                if (hdot) {
                    hdot[1] = 'h'; hdot[2] = 0;
                    read_lines_snippet(hpath, 1, 5, hhdr, sizeof hhdr);
                }
            }
            char full_snip[10240];
            const char *rel = tail_path_segments(k->path, 3);
            int sn = snprintf(full_snip, sizeof full_snip, "%s\n%s%s%s", rel, hdr, hhdr, snip);
            if (sn < 0) sn = 0;
            if ((size_t)sn >= sizeof full_snip) sn = (int)(sizeof full_snip - 1);
            k->rerank = lpm_rerank(ix, rq, strlen(rq), full_snip, (size_t)sn, &k->shared, &k->frechet);
            k->rerank *= basename_stem_boost(k->path, rq);
            if (!strcmp(scope_s, "all")) {
                if (lpm_is_prose_or_data_path(k->path)) k->rerank *= 0.35;
                else if (lpm_is_test_path(k->path)) k->rerank *= 0.25;
                else if (dot && (!strcasecmp(dot, ".h") || !strcasecmp(dot, ".hpp"))) k->rerank *= 0.85;
            }
            if (k->rerank > 1.0) k->rerank = 1.0;
        } else {
            k->rerank = 0.0;
        }
    }

    char fusion[16] = "";
    if (!api_jstr(body, "fusion", fusion, sizeof fusion) || !fusion[0]) {
        const char *fe = getenv("LAPLACE_CODE_FUSION");
        snprintf(fusion, sizeof fusion, "%s", fe && *fe ? fe : "rerank");
    }
    int by_rerank = !strcmp(fusion, "rerank");
    if (strcmp(fusion, "rrf") && !by_rerank) {
        for (size_t i = 0; i < nc; i++) free(cand[i].snippet);
        free(cand); free(bridged);
        api_http(c, 400, "{\"error\":\"fusion is rrf or rerank\"}");
        return;
    }
    const double K = 60.0;
    CodeCand **byr = malloc(sizeof *byr * (nc + 1));
    for (size_t i = 0; i < nc; i++) byr[i] = &cand[i];
    qsort(byr, nc, sizeof *byr, cand_rerank_cmp);
    for (size_t i = 0; i < nc; i++) {
        CodeCand *k = byr[i];
        k->rr_rank = (int)i + 1;
        k->fused = w_rr / (K + k->rr_rank) + (k->lex_rank ? 0.5 / (K + k->lex_rank) : 0.0);
    }
    free(byr);
    qsort(cand, nc, sizeof *cand, by_rerank ? cand_rerank_cmp_v : cand_cmp);
    int explain = api_jbool(body, "explain", 0);
    size_t shown = explain ? nc : nc < (size_t)limit ? nc : (size_t)limit;
    double ms = (double)(api_nsec() - t0) / 1e6;
    char *out = NULL;
    size_t un = 0, ucap = 0;
    api_buf_fmt(&out, &un, &ucap,
            "{\"docs\":%llu,\"entities\":%llu,\"ms\":%.3f,\"model\":\"laplace-frechet4d\","
            "\"fusion\":\"%s\",\"scope\":\"%s\",\"weights\":{\"rerank\":%g}",
            (unsigned long long)st.documents, (unsigned long long)st.entities, ms,
            by_rerank ? "rerank" : "rrf", scope_s, w_rr);
    if (bridge) api_buf_fmt(&out, &un, &ucap, ",\"bridge\":{\"words\":%s,\"ms\":%.3f}", binfo, bridge_ms);
    free(bridged);
    api_buf_fmt(&out, &un, &ucap, ",\"candidates\":{\"lexical\":%zu,\"reranked\":%zu},\"hits\":[", nh, nc);
    for (size_t i = 0; i < shown; i++) {
        CodeCand *k = &cand[i];
        char id[33], pe[8192], se[24576];
        static const char H[] = "0123456789abcdef";
        for (int b = 0; b < 16; b++) {
            id[b * 2] = H[k->id[b] >> 4];
            id[b * 2 + 1] = H[k->id[b] & 15];
        }
        id[32] = 0;
        api_json_esc(k->path, pe, sizeof pe);
        api_json_esc(k->snippet ? k->snippet : "", se, sizeof se);
        api_buf_fmt(&out, &un, &ucap,
                "%s{\"rank\":%zu,\"path\":\"%s\",\"line0\":%u,\"line1\":%u,\"source\":\"laplace\","
                "\"id\":\"%s\",\"hilbert\":\"%016llx\",\"shared\":%u,\"frechet\":%.6f,\"rerank\":%.6f,"
                "\"fused\":%.6f,\"ranks\":{\"rerank\":%d,\"lexical\":%d},\"snippet\":\"%s\"}",
                i ? "," : "", i + 1, pe, k->line0, k->line1, id, (unsigned long long)k->hilbert, k->shared,
                k->frechet, k->rerank, k->fused, k->rr_rank, k->lex_rank, se);
    }
    api_buf_add(&out, &un, &ucap, "]}", 2);
    for (size_t i = 0; i < nc; i++) free(cand[i].snippet);
    free(cand);
    api_http(c, 200, out ? out : "{\"hits\":[]}");
    free(out);
}
void on_reindex(int c, const char *body) {
    char path[4096] = "";
    api_jstr(body, "path", path, sizeof path);
    uint64_t t0 = api_nsec();
    Lpm *ix = ensure_code_ix();
    if (!ix) {
        api_http(c, 500, "{\"error\":\"code index\"}");
        return;
    }
    LpmUpdate u = {0};
    int rebuilt = 0;
    if (path[0]) {
        if (!os_exists(path)) {
            api_http(c, 400, "{\"error\":\"path does not exist\"}");
            return;
        }
        lpm_update_path(CODE_IX, path, &u);
        const char *rec = getenv("LAPLACE_CODE_RECORDS");
        if (rec && *rec && (u.changed || u.added || u.removed) && lpm_save(CODE_IX, rec)) {
            api_http(c, 500, "{\"error\":\"records not saved\"}");
            return;
        }
    } else if (code_refresh(&u, &rebuilt)) {
        api_http(c, 500, "{\"error\":\"code index refresh\"}");
        return;
    }
    LpmStats st = {0};
    lpm_stats(CODE_IX, &st);
    double ms = (double)(api_nsec() - t0) / 1e6;
    char out[512];
    snprintf(out, sizeof out,
             "{\"status\":\"indexed\",\"unchanged\":%llu,\"changed\":%llu,\"added\":%llu,\"removed\":%llu,"
             "\"rebuilt\":%s,\"files\":%llu,\"docs\":%llu,\"stale\":%llu,\"entities\":%llu,\"ms\":%.3f}",
             (unsigned long long)u.unchanged, (unsigned long long)u.changed, (unsigned long long)u.added,
             (unsigned long long)u.removed, rebuilt ? "true" : "false", (unsigned long long)st.files,
             (unsigned long long)st.documents, (unsigned long long)st.stale, (unsigned long long)st.entities, ms);
    api_http(c, 200, out);
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

void on_rerank(int c, const char *body) {
    char query[8192] = "";
    if (!api_jstr(body, "query", query, sizeof query) && !api_jstr(body, "input", query, sizeof query)) {
        api_http(c, 400, "{\"error\":\"query\"}");
        return;
    }
    char *docs[256];
    size_t nd = api_parse_str_array(body, "documents", docs, 256);
    if (!nd) {
        api_http(c, 400, "{\"error\":\"documents\"}");
        return;
    }
    int top_n = (int)nd;
    if (api_jint(body, "top_n", &top_n) && (top_n < 1 || (size_t)top_n > nd)) top_n = (int)nd;
    uint64_t t0 = api_nsec();
    Lpm *ix = ensure_code_ix();
    if (!ix) {
        for (size_t i = 0; i < nd; i++) free(docs[i]);
        api_http(c, 500, "{\"error\":\"tier0\"}");
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
    double ms = (double)(api_nsec() - t0) / 1e6;
    char *out = NULL;
    size_t un = 0, ucap = 0;
    api_buf_fmt(&out, &un, &ucap, "{\"model\":\"laplace-frechet4d\",\"ms\":%.3f,\"results\":[", ms);
    for (size_t i = 0; i < nh; i++) {
        api_buf_fmt(&out, &un, &ucap,
                "%s{\"index\":%u,\"relevance_score\":%.6f,\"shared\":%u,\"frechet\":%.6f}",
                i ? "," : "", items[i].index, items[i].relevance_score, items[i].shared, items[i].frechet);
    }
    api_buf_add(&out, &un, &ucap, "]}", 2);
    api_http(c, 200, out ? out : "{\"results\":[]}");
    free(out);
}
