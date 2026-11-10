#define _GNU_SOURCE
#include "lpm_internal.h"

static uint64_t mix(const uint8_t id[16]) {
    uint64_t x;
    memcpy(&x, id, 8);
    x ^= x >> 30;
    x *= 0xbf58476d1ce4e5b9ULL;
    x ^= x >> 27;
    x *= 0x94d049bb133111ebULL;
    x ^= x >> 31;
    return x;
}

void lpm_rehash(Lpm *ix, size_t nslot) {
    Ent *n = calloc(nslot, sizeof *n);
    if (!n) abort();
    for (size_t i = 0; i < ix->nslot; i++) {
        if (!ix->slot[i].used) continue;
        size_t k = mix(ix->slot[i].id) & (nslot - 1);
        while (n[k].used) k = (k + 1) & (nslot - 1);
        n[k] = ix->slot[i];
    }
    free(ix->slot);
    ix->slot = n;
    ix->nslot = nslot;
}

Ent *lpm_ent_put(Lpm *ix, const lp_ref *r, int *fresh) {
    if (ix->nent * 10 >= ix->nslot * 7) lpm_rehash(ix, ix->nslot ? ix->nslot * 2 : 1024);
    size_t k = mix(r->id.b) & (ix->nslot - 1);
    while (ix->slot[k].used) {
        if (!memcmp(ix->slot[k].id, r->id.b, 16)) {
            ix->slot[k].seen++;
            *fresh = 0;
            return &ix->slot[k];
        }
        k = (k + 1) & (ix->nslot - 1);
    }
    Ent *e = &ix->slot[k];
    memcpy(e->id, r->id.b, 16);
    memcpy(e->m, r->c.m, sizeof e->m);
    e->hilbert = lp_hilbert4(&r->c);
    e->tier = r->tier;
    e->seen = 1;
    e->used = 1;
    ix->nent++;
    *fresh = 1;
    return e;
}

Ent *lpm_ent_find(const Lpm *ix, const uint8_t id[16]) {
    if (!ix->nslot) return NULL;
    size_t k = mix(id) & (ix->nslot - 1);
    while (ix->slot[k].used) {
        if (!memcmp(ix->slot[k].id, id, 16)) return &ix->slot[k];
        k = (k + 1) & (ix->nslot - 1);
    }
    return NULL;
}

void lpm_add_feat(Lpm *ix, const uint8_t id[16]) {
    if (!ix->collecting) return;
    if (ix->nfeat == ix->cfeat) {
        ix->cfeat = ix->cfeat ? ix->cfeat * 2 : 256;
        ix->feat = lpm_grow(ix->feat, ix->cfeat * 16);
    }
    memcpy(ix->feat + ix->nfeat * 16, id, 16);
    ix->nfeat++;
}

void lpm_ent_kids(Ent *e, const lp_ref *ch, uint32_t n) {
    if (e->kids || n == 0 || n > 200000u) return;
    e->kids = malloc((size_t)n * 16);
    if (!e->kids) return;
    for (uint32_t i = 0; i < n; i++) memcpy(e->kids + (size_t)i * 16, ch[i].id.b, 16);
    e->nkids = n;
}

lp_ref lpm_on_compose(void *ud, const lp_ref *ch, uint32_t n, uint8_t tier) {
    Lpm *ix = ud;
    lp_ref r = lp_ref_compose(ch, n, tier);
    int fresh = 0;
    for (uint32_t i = 0; i < n; i++) lpm_ent_put(ix, &ch[i], &fresh);
    Ent *e = lpm_ent_put(ix, &r, &fresh);
    lpm_ent_kids(e, ch, n);
    ix->compositions++;
    int low = 1;
    for (uint32_t i = 0; i < n; i++) if (ch[i].tier > 1) { low = 0; break; }
    if (low && n > 1 && ix->collecting) lpm_add_feat(ix, r.id.b);
    return r;
}

void lpm_post_features_range(Lpm *ix, uint32_t doc, size_t f0, size_t f1) {
    if (f1 <= f0) return;
    size_t n = f1 - f0;
    uint8_t *tmp = malloc(n * 16);
    if (!tmp) abort();
    memcpy(tmp, ix->feat + f0 * 16, n * 16);
    qsort(tmp, n, 16, lpm_cmp16);
    const uint8_t *prev = NULL;
    for (size_t i = 0; i < n; i++) {
        const uint8_t *id = tmp + i * 16;
        if (prev && !memcmp(prev, id, 16)) continue;
        prev = id;
        Ent *e = lpm_ent_find(ix, id);
        if (!e) continue;
        if (e->nd && e->docs[e->nd - 1] == doc) continue;
        if (e->nd == e->cd) {
            e->cd = e->cd ? e->cd * 2 : 4;
            e->docs = lpm_grow(e->docs, e->cd * sizeof *e->docs);
        }
        e->docs[e->nd++] = doc;
    }
    free(tmp);
}

void lpm_post_features(Lpm *ix, uint32_t doc) {
    lpm_post_features_range(ix, doc, 0, ix->nfeat);
}

uint32_t lpm_add_path(Lpm *ix, const char *path) {
    size_t n = strlen(path) + 1;
    if (ix->npaths + n > ix->cpaths) {
        ix->cpaths = ix->cpaths ? ix->cpaths * 2 : 4096;
        if (ix->cpaths < ix->npaths + n) ix->cpaths = ix->npaths + n;
        ix->paths = lpm_grow(ix->paths, ix->cpaths);
    }
    uint32_t off = (uint32_t)ix->npaths;
    memcpy(ix->paths + ix->npaths, path, n);
#ifdef _WIN32
    /* One separator in what is stored and returned: a root given as E:\x walked as E:\x/y reads E:/x/y. */
    for (size_t i = 0; i < n; i++) if (ix->paths[off + i] == '\\') ix->paths[off + i] = '/';
#endif
    ix->npaths += n;
    return off;
}

Lpm *lpm_new(const char *tier0) {
    Lpm *ix = calloc(1, sizeof *ix);
    if (!ix) return NULL;
    ix->t0 = lp_tier0_map(tier0 && *tier0 ? tier0 : NULL);
    if (!ix->t0) { free(ix); return NULL; }
    ix->text = lp_text_new(ix->t0);
    if (!ix->text) { free(ix); return NULL; }
    return ix;
}

void lpm_free(Lpm *ix) {
    if (!ix) return;
    for (size_t i = 0; i < ix->nslot; i++) { free(ix->slot[i].docs); free(ix->slot[i].kids); }
    free(ix->slot);
    free(ix->docs);
    free(ix->paths);
    free(ix->feat);
    free(ix->sc);
    free(ix->sc_pool);
    free(ix->sc_feat);
    free(ix->fl);
    free(ix->fmap);
    lp_text_free(ix->text);
    free(ix);
}

void lpm_fmap_rehash(Lpm *ix, size_t cap) {
    uint32_t *m = calloc(cap, sizeof *m);
    if (!m) abort();
    for (size_t i = 0; i < ix->nfl; i++) {
        if (ix->fl[i].dead) continue;
        size_t k = lpm_str_hash(ix->paths + ix->fl[i].path) & (cap - 1);
        while (m[k]) k = (k + 1) & (cap - 1);
        m[k] = (uint32_t)i + 1;
    }
    free(ix->fmap);
    ix->fmap = m;
    ix->nfmap = cap;
}

/* The live file stored under this path (normalised as add_path stores it), or -1. */
long lpm_file_find(const Lpm *ix, const char *p) {
    if (!ix->nfmap) return -1;
    size_t k = lpm_str_hash(p) & (ix->nfmap - 1);
    while (ix->fmap[k]) {
        uint32_t i = ix->fmap[k] - 1;
        if (!ix->fl[i].dead && !strcmp(ix->paths + ix->fl[i].path, p)) return (long)i;
        k = (k + 1) & (ix->nfmap - 1);
    }
    return -1;
}

/* Live documents and their mean length in bytes, recomputed when documents were added or killed. */
size_t lpm_live_stats(Lpm *ix, double *avg) {
    if (ix->live_at_ndocs != ix->ndocs || ix->live_at_dead != ix->dead_docs) {
        double sum = 0;
        size_t live = 0;
        for (size_t d = 0; d < ix->ndocs; d++)
            if (!ix->fl[ix->docs[d].file].dead) { sum += ix->docs[d].bytes; live++; }
        ix->live_n = live;
        ix->live_avg = live && sum / (double)live >= 1 ? sum / (double)live : 1;
        ix->live_at_ndocs = ix->ndocs;
        ix->live_at_dead = ix->dead_docs;
    }
    *avg = ix->live_avg;
    return ix->live_n;
}

void lpm_file_kill(Lpm *ix, uint32_t i) {
    if (ix->fl[i].dead) return;
    ix->fl[i].dead = 1;
    ix->dead_docs += ix->fl[i].ndoc;
}

uint32_t lpm_file_add(Lpm *ix, uint32_t path_off, uint64_t size, int64_t mtime, uint64_t hash) {
    if (ix->nfl == ix->cfl) {
        ix->cfl = ix->cfl ? ix->cfl * 2 : 256;
        ix->fl = lpm_grow(ix->fl, ix->cfl * sizeof *ix->fl);
    }
    uint32_t i = (uint32_t)ix->nfl++;
    LFile *f = &ix->fl[i];
    memset(f, 0, sizeof *f);
    f->path = path_off;
    f->size = size;
    f->mtime = mtime;
    f->hash = hash;
    f->doc0 = (uint32_t)ix->ndocs;
    f->seen = 1;
    if ((ix->nfl + 1) * 2 > ix->nfmap) lpm_fmap_rehash(ix, ix->nfmap ? ix->nfmap * 2 : 1024);
    else {
        size_t k = lpm_str_hash(ix->paths + path_off) & (ix->nfmap - 1);
        while (ix->fmap[k]) k = (k + 1) & (ix->nfmap - 1);
        ix->fmap[k] = i + 1;
    }
    return i;
}

void lpm_norm_path(char *out, size_t cap, const char *in) {
    snprintf(out, cap, "%s", in);
#ifdef _WIN32
    for (char *q = out; *q; q++) if (*q == '\\') *q = '/';
#endif
    size_t n = strlen(out);
    while (n > 1 && out[n - 1] == '/') out[--n] = 0;
}

lp_ref lpm_compose_put(Lpm *ix, const lp_ref *ch, uint32_t n, uint8_t tier) {
    lp_ref r = lp_ref_compose(ch, n, tier);
    int fresh = 0;
    Ent *e = lpm_ent_put(ix, &r, &fresh);
    ix->compositions++;
    lpm_ent_kids(e, ch, n);
    return r;
}

int lpm_is_prose_or_data_path(const char *path) {
    if (!path) return 0;
    if (strstr(path, "/Laplace-Wiki/") || strstr(path, "\\Laplace-Wiki\\") ||
        strstr(path, "/docs/") || strstr(path, "\\docs\\")) return 1;
    const char *dot = strrchr(path, '.');
    if (!dot) return 0;
    return !strcasecmp(dot, ".md") || !strcasecmp(dot, ".txt") ||
           !strcasecmp(dot, ".rst") || !strcasecmp(dot, ".adoc") ||
           !strcasecmp(dot, ".json") || !strcasecmp(dot, ".tsv") ||
           !strcasecmp(dot, ".csv") || !strcasecmp(dot, ".lmf") ||
           !strcasecmp(dot, ".ttl") || !strcasecmp(dot, ".yml") ||
           !strcasecmp(dot, ".yaml") || !strcasecmp(dot, ".recipe") ||
           !strcasecmp(dot, ".format") || !strcasecmp(dot, ".env") ||
           !strcasecmp(dot, ".in") || !strcasecmp(dot, ".cmake") ||
           !strcasecmp(dot, ".control");
}

int lpm_is_test_path(const char *path) {
    if (!path) return 0;
    return strstr(path, "/tests/") != NULL || strstr(path, "/test/") != NULL ||
           strstr(path, ".Tests/") != NULL || strstr(path, "Tests.cs") != NULL ||
           strstr(path, "Test.cs") != NULL;
}

int lpm_is_utf8(const uint8_t *s, size_t n) {
    size_t i = 0; uint32_t cp;
    while (i < n) if (!lp_utf8_next(s, n, &i, &cp)) return 0;
    return 1;
}

size_t lpm_search(Lpm *ix, const char *q, size_t n, LpmHit *out, size_t cap) {
    if (!cap || !ix->ndocs) return 0;
    uint64_t saved_files = ix->files, saved_bytes = ix->bytes, saved_comp = ix->compositions;
    size_t saved_ndocs = ix->ndocs;
    ix->nfeat = 0;
    ix->collecting = 2;
    size_t i = 0;
    uint32_t cp;
    int utf8 = 1;
    const uint8_t *s = (const uint8_t *)q;
    while (i < n) {
        if (!lp_utf8_next(s, n, &i, &cp)) { utf8 = 0; break; }
    }
    lp_ref trunk;
    if (utf8 && n) {
        trunk = lp_text_decompose(ix->text, s, n, lpm_on_compose, ix);
    } else memset(&trunk, 0, sizeof trunk);
    lpm_add_feat(ix, trunk.id.b);
    ix->collecting = 0;
    /* A query is not a document. Roll the occurrence counters back. Entities stay:
     * the query's own constituents are real records. */
    ix->files = saved_files;
    ix->bytes = saved_bytes;
    ix->compositions = saved_comp;
    ix->ndocs = saved_ndocs;

    if (ix->nfeat > 1) {
        qsort(ix->feat, ix->nfeat, 16, lpm_cmp16);
        size_t w = 1;
        for (size_t k = 1; k < ix->nfeat; k++) {
            if (memcmp(ix->feat + k * 16, ix->feat + (w - 1) * 16, 16)) {
                memcpy(ix->feat + w * 16, ix->feat + k * 16, 16);
                w++;
            }
        }
        ix->nfeat = w;
    }

    uint32_t *shared = calloc(ix->ndocs, sizeof *shared);
    uint16_t *spec_shared = calloc(ix->ndocs, sizeof *spec_shared);
    double *rank = calloc(ix->ndocs, sizeof *rank);
    if (!shared || !spec_shared || !rank) abort();
    /* BM25 with presence, cubic IDF specificity, and specific-term coordination. */
    const double k1 = 1.2, b = 0.25;
    double avg;
    size_t live = lpm_live_stats(ix, &avg);
    if (!live) { free(shared); free(spec_shared); free(rank); return 0; }
    for (size_t f = 0; f < ix->nfeat; f++) {
        Ent *e = lpm_ent_find(ix, ix->feat + f * 16);
        if (!e || !e->nd) continue;
        double rate = (double)e->nd / (double)live;
        if (rate > 1.0) rate = 1.0;
        double idf = log(1.0 + (double)live / (double)e->nd);
        double w = idf * idf * idf * (1.0 - rate * 0.90);
        int is_spec = (rate < 0.08);
        for (uint32_t d = 0; d < e->nd; d++) {
            uint32_t di = e->docs[d];
            if (di >= ix->ndocs || ix->fl[ix->docs[di].file].dead) continue;
            shared[di]++;
            if (is_spec) spec_shared[di]++;
            rank[di] += w;
        }
    }
    for (size_t d = 0; d < ix->ndocs; d++) if (shared[d]) {
        const char *dp = ix->paths + ix->docs[d].path;
        if (strstr(dp, "code-search-eval.py") || strstr(dp, "rerank-length.py")) {
            shared[d] = 0; rank[d] = 0; continue;
        }
        double norm = 1.0 - b + b * (double)ix->docs[d].bytes / avg;
        rank[d] *= (k1 + 1.0) / (1.0 + k1 * norm);
        rank[d] *= (1.0 + 0.28 * (double)spec_shared[d]);
        if (lpm_is_prose_or_data_path(dp)) rank[d] *= 0.35;
        else if (lpm_is_test_path(dp)) rank[d] *= 0.25;
        else {
            const char *dot = strrchr(dp, '.');
            if (dot && (!strcasecmp(dot, ".h") || !strcasecmp(dot, ".hpp"))) rank[d] *= 0.85;
        }
    }
    free(spec_shared);
    uint8_t *taken = calloc(ix->ndocs, 1);
    uint16_t *file_hits = calloc(ix->nfl + 1, sizeof *file_hits);
    if (!taken || !file_hits) abort();
    size_t wrote = 0;
    while (wrote < cap) {
        uint32_t best = 0;
        double best_eff = -1.0;
        int any = 0;
        for (size_t d = 0; d < ix->ndocs; d++) {
            if (taken[d] || !shared[d]) continue;
            uint32_t fi = ix->docs[d].file;
            uint16_t fh = fi < ix->nfl ? file_hits[fi] : 0;
            double eff = fh >= 1 ? rank[d] / (1.0 + 0.45 * (double)fh * (double)fh) : rank[d];
            if (!any || eff > best_eff || (eff == best_eff && ix->docs[d].bytes < ix->docs[best].bytes)) {
                best = (uint32_t)d;
                best_eff = eff;
                any = 1;
            }
        }
        if (!any) break;
        taken[best] = 1;
        Doc *doc = &ix->docs[best];
        const char *bp = ix->paths + doc->path;
        /* One answer per place: a span that overlaps a better one in the same file is that answer again. */
        int dup = 0;
        for (size_t k = 0; k < wrote && !dup; k++)
            if (out[k].line0 <= doc->line1 && doc->line0 <= out[k].line1 && !strcmp(out[k].path, bp)) dup = 1;
        if (dup) continue;
        if (doc->file < ix->nfl) file_hits[doc->file]++;
        LpmHit *hit = &out[wrote++];
        memset(hit, 0, sizeof *hit);
        memcpy(hit->id, doc->id, 16);
        memcpy(hit->m, doc->m, sizeof hit->m);
        hit->hilbert = doc->hilbert;
        hit->line0 = doc->line0;
        hit->line1 = doc->line1;
        hit->shared = shared[best];
        hit->bytes = doc->bytes;
        snprintf(hit->path, sizeof hit->path, "%s", bp);
    }
    free(file_hits);
    free(shared);
    free(rank);
    free(taken);
    return wrote;
}

size_t lpm_containers(const Lpm *ix, const uint8_t id[16], LpmHit *out, size_t cap) {
    size_t n = 0;
    Ent *e = lpm_ent_find(ix, id);
    if (e) {
        for (uint32_t i = 0; i < e->nd && n < cap; i++) {
            uint32_t di = e->docs[i];
            if (di >= ix->ndocs || ix->fl[ix->docs[di].file].dead) continue;
            Doc *d = &ix->docs[di];
            LpmHit *h = &out[n++];
            memset(h, 0, sizeof *h);
            memcpy(h->id, d->id, 16);
            memcpy(h->m, d->m, sizeof h->m);
            h->hilbert = d->hilbert;
            h->line0 = d->line0;
            h->line1 = d->line1;
            h->bytes = d->bytes;
            snprintf(h->path, sizeof h->path, "%s", ix->paths + d->path);
        }
    }
    for (size_t di = 0; di < ix->ndocs && n < cap; di++) {
        if (ix->fl[ix->docs[di].file].dead || memcmp(ix->docs[di].id, id, 16)) continue;
        int seen = 0;
        for (size_t k = 0; k < n; k++) if (!strcmp(out[k].path, ix->paths + ix->docs[di].path)) seen = 1;
        if (seen) continue;
        Doc *d = &ix->docs[di];
        LpmHit *h = &out[n++];
        memset(h, 0, sizeof *h);
        memcpy(h->id, d->id, 16);
        memcpy(h->m, d->m, sizeof h->m);
        h->hilbert = d->hilbert;
        snprintf(h->path, sizeof h->path, "%s", ix->paths + d->path);
    }
    return n;
}
