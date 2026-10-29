#define _GNU_SOURCE
#include "lpm.h"
#include "lpm_config.h"

#include "laplace/laplace.h"
#include "os.h"

#include <math.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#ifndef _WIN32
#include <fcntl.h>
#include <unistd.h>
#endif

#include <tree_sitter/api.h>

typedef struct Ent {
    uint8_t id[16];
    int64_t m[4];
    uint64_t hilbert;
    uint32_t *docs;
    uint32_t nd, cd;
    uint32_t seen;
    uint8_t tier;
    uint8_t used;
    uint8_t *kids;
    uint32_t nkids;
} Ent;

typedef struct Doc {
    uint8_t id[16];
    int64_t m[4];
    uint64_t hilbert;
    uint8_t tier;
    uint32_t line0, line1, bytes;
    uint32_t path;
    uint32_t file;      /* index into Lpm.fl */
} Doc;

/* A file as it was admitted: what reindexing compares (size and time, then content), and its documents,
 * which are contiguous. A file that changed or went away is dead: its documents are no longer answers. */
typedef struct LFile {
    uint32_t path;
    uint32_t doc0, ndoc;
    uint64_t size, hash;
    int64_t mtime;      /* nanoseconds */
    uint8_t dead, seen;
} LFile;

struct Lpm {
    const lp_tier0_record *t0;
    lp_text *text;
    Ent *slot;
    size_t nslot, nent;
    Doc *docs;
    size_t ndocs, cdocs;
    char *paths;
    size_t npaths, cpaths;
    uint8_t *feat;
    size_t nfeat, cfeat;
    uint64_t files, skipped, bytes, compositions;
    int collecting;
    struct SEnt *sc;
    size_t sc_n, sc_cap;
    char *sc_pool;
    size_t sc_pn, sc_pcap;
    uint8_t *sc_feat;
    size_t sc_fn, sc_fcap;
    /* Declaration chunking while a grammar file is admitted: a named node becomes its own document
     * when no ancestor already is one and it spans LPM_CHUNK_MIN_LINES..LPM_CHUNK_MAX_LINES lines. */
    int chunk_on;
    uint32_t chunk_path, chunk_lines;
    LFile *fl;
    size_t nfl, cfl;
    uint32_t *fmap;     /* path -> file index + 1, open addressing; dead files stay in it and are skipped */
    size_t nfmap;
    size_t dead_docs;
    size_t live_n, live_at_ndocs, live_at_dead;   /* live_stats cache */
    double live_avg;
    uint32_t cur_file;
    int have_meta;      /* index_file and update_file say what they read; lpm_admit of a bare buffer does not */
    uint64_t meta_size, meta_hash;
    int64_t meta_mtime;
};

/* A document small enough to be the answer: a declaration or a block of text, never a whole large file. */
#define LPM_CHUNK_MIN_LINES 3
#define LPM_CHUNK_MAX_LINES 120
/* Text with no grammar is cut into blocks at blank lines and headings, LPM_TEXT_MIN..LPM_TEXT_MAX lines each. */
#define LPM_TEXT_MIN_LINES 8
#define LPM_TEXT_MAX_LINES 48

static void *grow(void *p, size_t n) {
    p = realloc(p, n ? n : 1);
    if (!p) abort();
    return p;
}

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

static void rehash(Lpm *ix, size_t nslot) {
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

static Ent *ent_put(Lpm *ix, const lp_ref *r, int *fresh) {
    if (ix->nent * 10 >= ix->nslot * 7) rehash(ix, ix->nslot ? ix->nslot * 2 : 1024);
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

static Ent *ent_find(const Lpm *ix, const uint8_t id[16]) {
    if (!ix->nslot) return NULL;
    size_t k = mix(id) & (ix->nslot - 1);
    while (ix->slot[k].used) {
        if (!memcmp(ix->slot[k].id, id, 16)) return &ix->slot[k];
        k = (k + 1) & (ix->nslot - 1);
    }
    return NULL;
}

static void add_feat(Lpm *ix, const uint8_t id[16]) {
    if (!ix->collecting) return;
    if (ix->nfeat == ix->cfeat) {
        ix->cfeat = ix->cfeat ? ix->cfeat * 2 : 256;
        ix->feat = grow(ix->feat, ix->cfeat * 16);
    }
    memcpy(ix->feat + ix->nfeat * 16, id, 16);
    ix->nfeat++;
}

static void ent_kids(Ent *e, const lp_ref *ch, uint32_t n) {
    if (e->kids || n == 0 || n > 200000u) return;
    e->kids = malloc((size_t)n * 16);
    if (!e->kids) return;
    for (uint32_t i = 0; i < n; i++) memcpy(e->kids + (size_t)i * 16, ch[i].id.b, 16);
    e->nkids = n;
}

static lp_ref on_compose(void *ud, const lp_ref *ch, uint32_t n, uint8_t tier) {
    Lpm *ix = ud;
    lp_ref r = lp_ref_compose(ch, n, tier);
    int fresh = 0;
    for (uint32_t i = 0; i < n; i++) ent_put(ix, &ch[i], &fresh);
    Ent *e = ent_put(ix, &r, &fresh);
    ent_kids(e, ch, n);
    ix->compositions++;
    int low = 1;
    for (uint32_t i = 0; i < n; i++) if (ch[i].tier > 1) { low = 0; break; }
    if (low && n > 1) add_feat(ix, r.id.b);
    return r;
}

static int cmp16(const void *a, const void *b) { return memcmp(a, b, 16); }

static void post_features_range(Lpm *ix, uint32_t doc, size_t f0, size_t f1) {
    if (f1 <= f0) return;
    size_t n = f1 - f0;
    uint8_t *tmp = malloc(n * 16);
    if (!tmp) abort();
    memcpy(tmp, ix->feat + f0 * 16, n * 16);
    qsort(tmp, n, 16, cmp16);
    const uint8_t *prev = NULL;
    for (size_t i = 0; i < n; i++) {
        const uint8_t *id = tmp + i * 16;
        if (prev && !memcmp(prev, id, 16)) continue;
        prev = id;
        Ent *e = ent_find(ix, id);
        if (!e) continue;
        if (e->nd && e->docs[e->nd - 1] == doc) continue;
        if (e->nd == e->cd) {
            e->cd = e->cd ? e->cd * 2 : 4;
            e->docs = grow(e->docs, e->cd * sizeof *e->docs);
        }
        e->docs[e->nd++] = doc;
    }
    free(tmp);
}

static void post_features(Lpm *ix, uint32_t doc) {
    post_features_range(ix, doc, 0, ix->nfeat);
}

static uint32_t add_path(Lpm *ix, const char *path) {
    size_t n = strlen(path) + 1;
    if (ix->npaths + n > ix->cpaths) {
        ix->cpaths = ix->cpaths ? ix->cpaths * 2 : 4096;
        if (ix->cpaths < ix->npaths + n) ix->cpaths = ix->npaths + n;
        ix->paths = grow(ix->paths, ix->cpaths);
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

static uint32_t count_lines(const uint8_t *s, size_t n) {
    uint32_t lines = 1;
    for (size_t i = 0; i < n; i++) if (s[i] == '\n') lines++;
    return lines;
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

typedef struct SEnt {
    uint64_t h;
    lp_ref ref;
    uint32_t off, len, feat_at, feat_n;
    uint8_t used;
} SEnt;

static uint64_t fnv(const uint8_t *s, size_t n) {
    uint64_t h = 1469598103934665603ull;
    for (size_t i = 0; i < n; i++) { h ^= s[i]; h *= 1099511628211ull; }
    return h ? h : 1;
}

static uint64_t str_hash(const char *s) { return fnv((const uint8_t *)s, strlen(s)); }

static void fmap_rehash(Lpm *ix, size_t cap) {
    uint32_t *m = calloc(cap, sizeof *m);
    if (!m) abort();
    for (size_t i = 0; i < ix->nfl; i++) {
        if (ix->fl[i].dead) continue;
        size_t k = str_hash(ix->paths + ix->fl[i].path) & (cap - 1);
        while (m[k]) k = (k + 1) & (cap - 1);
        m[k] = (uint32_t)i + 1;
    }
    free(ix->fmap);
    ix->fmap = m;
    ix->nfmap = cap;
}

/* The live file stored under this path (normalised as add_path stores it), or -1. */
static long file_find(const Lpm *ix, const char *p) {
    if (!ix->nfmap) return -1;
    size_t k = str_hash(p) & (ix->nfmap - 1);
    while (ix->fmap[k]) {
        uint32_t i = ix->fmap[k] - 1;
        if (!ix->fl[i].dead && !strcmp(ix->paths + ix->fl[i].path, p)) return (long)i;
        k = (k + 1) & (ix->nfmap - 1);
    }
    return -1;
}

/* Live documents and their mean length in bytes, recomputed when documents were added or killed. */
static size_t live_stats(Lpm *ix, double *avg) {
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

static void file_kill(Lpm *ix, uint32_t i) {
    if (ix->fl[i].dead) return;
    ix->fl[i].dead = 1;
    ix->dead_docs += ix->fl[i].ndoc;
}

static uint32_t file_add(Lpm *ix, uint32_t path_off, uint64_t size, int64_t mtime, uint64_t hash) {
    if (ix->nfl == ix->cfl) {
        ix->cfl = ix->cfl ? ix->cfl * 2 : 256;
        ix->fl = grow(ix->fl, ix->cfl * sizeof *ix->fl);
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
    if ((ix->nfl + 1) * 2 > ix->nfmap) fmap_rehash(ix, ix->nfmap ? ix->nfmap * 2 : 1024);
    else {
        size_t k = str_hash(ix->paths + path_off) & (ix->nfmap - 1);
        while (ix->fmap[k]) k = (k + 1) & (ix->nfmap - 1);
        ix->fmap[k] = i + 1;
    }
    return i;
}

static void norm_path(char *out, size_t cap, const char *in) {
    snprintf(out, cap, "%s", in);
#ifdef _WIN32
    for (char *q = out; *q; q++) if (*q == '\\') *q = '/';
#endif
    size_t n = strlen(out);
    while (n > 1 && out[n - 1] == '/') out[--n] = 0;
}

static lp_ref compose_put(Lpm *ix, const lp_ref *ch, uint32_t n, uint8_t tier) {
    lp_ref r = lp_ref_compose(ch, n, tier);
    int fresh = 0;
    Ent *e = ent_put(ix, &r, &fresh);
    ix->compositions++;
    ent_kids(e, ch, n);
    return r;
}

static int is_ascii_alnum(uint8_t c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
}

static int is_stop_word_bytes(const uint8_t *w, size_t n) {
    static const char *stops[] = {
        "a", "an", "the", "of", "in", "on", "at", "to", "for", "with", "by", "from",
        "as", "is", "are", "was", "were", "be", "been", "and", "or", "not", "that",
        "this", "it", "its", "into", "over", "than", "then", NULL
    };
    if (n == 0 || n > 6) return 0;
    for (int i = 0; stops[i]; i++) {
        if (strlen(stops[i]) == n && !memcmp(w, stops[i], n)) return 1;
    }
    return 0;
}

static lp_ref lower_token_ref(Lpm *ix, const uint8_t *w, size_t n, int register_ent) {
    lp_ref ch[64];
    if (n > 64) n = 64;
    for (size_t i = 0; i < n; i++) {
        uint8_t c = w[i];
        if (c >= 'A' && c <= 'Z') c = (uint8_t)(c - 'A' + 'a');
        ch[i] = lp_ref_atom(ix->t0, (uint32_t)c);
    }
    lp_ref r = (n == 1) ? ch[0] : lp_ref_compose(ch, n, 2);
    if (register_ent) {
        int fresh = 0;
        ent_put(ix, &r, &fresh);
        if (ix->collecting && n >= 2) add_feat(ix, r.id.b);
    }
    return r;
}

static void collect_subtoken_feats(Lpm *ix, const uint8_t *s, size_t n) {
    if (!ix->collecting || !n) return;
    size_t i = 0;
    while (i < n) {
        while (i < n && !is_ascii_alnum(s[i])) i++;
        if (i >= n) break;
        size_t w0 = i;
        while (i < n && is_ascii_alnum(s[i])) i++;
        size_t wlen = i - w0;
        if (wlen >= 2 && wlen <= 64) lower_token_ref(ix, s + w0, wlen, 1);
        /* Also split CamelCase / digit boundaries into sub-tokens */
        size_t sub0 = w0;
        for (size_t k = w0 + 1; k <= i; k++) {
            int split = (k == i);
            if (!split) {
                uint8_t prev = s[k - 1], cur = s[k];
                int prev_lo = (prev >= 'a' && prev <= 'z');
                int prev_up = (prev >= 'A' && prev <= 'Z');
                int prev_dg = (prev >= '0' && prev <= '9');
                int cur_up  = (cur >= 'A' && cur <= 'Z');
                int cur_dg  = (cur >= '0' && cur <= '9');
                int next_lo = (k + 1 < i) && (s[k + 1] >= 'a' && s[k + 1] <= 'z');
                if ((prev_lo && cur_up) || (prev_up && cur_up && next_lo) || (prev_dg != cur_dg)) split = 1;
            }
            if (split) {
                size_t slen = k - sub0;
                if (slen >= 2 && slen < wlen && slen <= 64) lower_token_ref(ix, s + sub0, slen, 1);
                sub0 = k;
            }
        }
    }
}

/* A span of text, decomposed once. The same bytes are the same entity on the next call. */
static lp_ref text_cached(Lpm *ix, const uint8_t *s, size_t n) {
    lp_ref zero; memset(&zero, 0, sizeof zero);
    if (!n) return zero;
    if ((ix->sc_n + 1) * 2 > ix->sc_cap) {
        size_t oc = ix->sc_cap;
        SEnt *old = ix->sc;
        ix->sc_cap = oc ? oc * 2 : 1024;
        ix->sc = calloc(ix->sc_cap, sizeof(SEnt));
        if (!ix->sc) abort();
        for (size_t i = 0; i < oc; i++) if (old[i].used) {
            size_t k = old[i].h & (ix->sc_cap - 1);
            while (ix->sc[k].used) k = (k + 1) & (ix->sc_cap - 1);
            ix->sc[k] = old[i];
        }
        free(old);
    }
    uint64_t h = fnv(s, n);
    size_t k = h & (ix->sc_cap - 1);
    while (ix->sc[k].used) {
        SEnt *e = &ix->sc[k];
        if (e->h == h && e->len == n && !memcmp(ix->sc_pool + e->off, s, n)) {
            for (uint32_t i = 0; i < e->feat_n; i++) add_feat(ix, ix->sc_feat + (size_t)(e->feat_at + i) * 16);
            return e->ref;
        }
        k = (k + 1) & (ix->sc_cap - 1);
    }
    size_t feat0 = ix->nfeat;
    lp_ref r = lp_text_decompose(ix->text, s, n, on_compose, ix);
    collect_subtoken_feats(ix, s, n);
    if (ix->sc_pn + n > ix->sc_pcap) {
        ix->sc_pcap = (ix->sc_pn + n) * 2 + 4096;
        ix->sc_pool = grow(ix->sc_pool, ix->sc_pcap);
    }
    memcpy(ix->sc_pool + ix->sc_pn, s, n);
    uint32_t fn = (uint32_t)(ix->nfeat - feat0);
    if (ix->sc_fn + fn > ix->sc_fcap) {
        ix->sc_fcap = ix->sc_fcap ? ix->sc_fcap * 2 : 1024;
        if (ix->sc_fcap < ix->sc_fn + fn) ix->sc_fcap = ix->sc_fn + fn;
        ix->sc_feat = grow(ix->sc_feat, ix->sc_fcap * 16);
    }
    if (fn) memcpy(ix->sc_feat + ix->sc_fn * 16, ix->feat + feat0 * 16, (size_t)fn * 16);
    ix->sc[k] = (SEnt){ h, r, (uint32_t)ix->sc_pn, (uint32_t)n, (uint32_t)ix->sc_fn, fn, 1 };
    ix->sc_pn += n;
    ix->sc_fn += fn;
    ix->sc_n++;
    return r;
}

/* Records of a delimited file. A row is the composition of its fields. The file is the composition of its rows.
 * A field is text, so UAX #29 applies to the field and not to the file. */
static lp_ref admit_delimited(Lpm *ix, const uint8_t *s, size_t n, int comma) {
    lp_ref *rows = NULL;
    size_t nr = 0, cr = 0;
    size_t i = 0;
    while (i < n) {
        size_t line = i;
        while (i < n && s[i] != '\n') i++;
        size_t end = i;
        if (i < n && s[i] == '\n') i++;
        if (end > line && s[end - 1] == '\r') end--;
        if (end == line) continue;
        lp_ref *fields = NULL;
        size_t nf = 0, cf = 0;
        size_t f = line;
        int inq = 0;
        for (size_t p = line; p <= end; p++) {
            int brk = p == end || (!inq && ((comma && s[p] == ',') || (!comma && s[p] == '\t')));
            if (!brk && comma && s[p] == '"') {
                if (inq && p + 1 < end && s[p + 1] == '"') { p++; continue; }
                inq = !inq;
                continue;
            }
            if (!brk) continue;
            const uint8_t *fs = s + f;
            size_t fl = p - f;
            if (comma && fl >= 2 && fs[0] == '"' && fs[fl - 1] == '"') { fs++; fl -= 2; }
            if (nf == cf) { cf = cf ? cf * 2 : 8; fields = grow(fields, cf * sizeof *fields); }
            fields[nf++] = text_cached(ix, fs, fl);
            f = p + 1;
        }
        if (nf) {
            uint8_t tier = 0;
            for (size_t k = 0; k < nf; k++) if (fields[k].tier > tier) tier = fields[k].tier;
            lp_ref row = compose_put(ix, fields, (uint32_t)nf, (uint8_t)(tier + 1));
            if (nr == cr) { cr = cr ? cr * 2 : 256; rows = grow(rows, cr * sizeof *rows); }
            rows[nr++] = row;
        }
        free(fields);
    }
    lp_ref trunk;
    memset(&trunk, 0, sizeof trunk);
    if (nr) {
        uint8_t tier = 0;
        for (size_t k = 0; k < nr; k++) if (rows[k].tier > tier) tier = rows[k].tier;
        trunk = compose_put(ix, rows, (uint32_t)nr, (uint8_t)(tier + 1));
    }
    free(rows);
    return trunk;
}

static const TSLanguage *load_lang(const char *name) {
    static char names[64][32];
    static const TSLanguage *langs[64];
    static int nlang;
    for (int i = 0; i < nlang; i++) if (!strcmp(names[i], name)) return langs[i];
    char so[512], sym[96];
    /* LAPLACE_GRAMMARS names the directory the compiled grammars are in; unset, the one this build was configured with. */
    const char *dir = getenv("LAPLACE_GRAMMARS");
    snprintf(so, sizeof so, "%s/libtree-sitter-%s" OS_DLL_SUFFIX, dir && *dir ? dir : LPM_GRAMMARS_DEFAULT, name);
    if (!os_exists(so)) return NULL;
    void *h = os_dl_open(so);
    if (!h) return NULL;
    snprintf(sym, sizeof sym, "tree_sitter_%s", name);
    TSLanguage *(*fn)(void) = (TSLanguage *(*)(void))os_dl_sym(h, sym);
    if (!fn) return NULL;
    if (nlang < 64) { snprintf(names[nlang], sizeof names[nlang], "%s", name); langs[nlang] = fn(); return langs[nlang++]; }
    return fn();
}

static const char *path_basename(const char *path) {
    if (!path) return "";
    const char *s1 = strrchr(path, '/');
    const char *s2 = strrchr(path, '\\');
    const char *b = s1 > s2 ? s1 : s2;
    return b ? b + 1 : path;
}

static const char *grammar_name(const char *path) {
    const char *base = path_basename(path);
    if (!strcmp(base, "CMakeLists.txt")) return "cmake";
    if (!strcmp(base, "Makefile") || !strcmp(base, "GNUmakefile")) return "make";
    if (!strcmp(base, "Dockerfile") || !strncmp(base, "Dockerfile.", 11)) return "dockerfile";
    const char *dot = base ? strrchr(base, '.') : NULL;
    if (!dot) return NULL;
    if (!strcmp(dot, ".txt") || !strcmp(dot, ".md") || !strcmp(dot, ".rst")) return NULL;
    if (!strcmp(dot, ".tsv") || !strcmp(dot, ".csv")) return NULL;
    if (!strcmp(dot, ".c") || !strcmp(dot, ".h")) return "c";
    if (!strcmp(dot, ".cc") || !strcmp(dot, ".cpp") || !strcmp(dot, ".cxx") || !strcmp(dot, ".hpp") || !strcmp(dot, ".hh")) return "cpp";
    if (!strcmp(dot, ".cu") || !strcmp(dot, ".cuh")) return "cuda";
    if (!strcmp(dot, ".cs")) return "c_sharp";
    if (!strcmp(dot, ".py")) return "python";
    if (!strcmp(dot, ".js") || !strcmp(dot, ".mjs") || !strcmp(dot, ".cjs")) return "javascript";
    if (!strcmp(dot, ".ts")) return "typescript";
    if (!strcmp(dot, ".tsx")) return "tsx";
    if (!strcmp(dot, ".rs")) return "rust";
    if (!strcmp(dot, ".go")) return "go";
    if (!strcmp(dot, ".rb")) return "ruby";
    if (!strcmp(dot, ".java")) return "java";
    if (!strcmp(dot, ".php")) return "php";
    if (!strcmp(dot, ".lua")) return "lua";
    if (!strcmp(dot, ".css")) return "css";
    if (!strcmp(dot, ".toml")) return "toml";
    if (!strcmp(dot, ".cmake")) return "cmake";
    if (!strcmp(dot, ".xml") || !strcmp(dot, ".csproj") || !strcmp(dot, ".props") || !strcmp(dot, ".targets")) return "xml";
    if (!strcmp(dot, ".json")) return "json";
    if (!strcmp(dot, ".sql")) return "sql";
    if (!strcmp(dot, ".html") || !strcmp(dot, ".htm")) return "html";
    if (!strcmp(dot, ".sh") || !strcmp(dot, ".bash")) return "bash";
    if (!strcmp(dot, ".yml") || !strcmp(dot, ".yaml")) return "yaml";
    return NULL;
}

/* A document: the entity r, lines l0..l1 of the file at path_off, holding the features collected since f0. */
static void push_doc(Lpm *ix, const lp_ref *r, uint32_t l0, uint32_t l1, uint32_t bytes, uint32_t path_off, size_t f0) {
    add_feat(ix, r->id.b);
    if (ix->ndocs == ix->cdocs) {
        ix->cdocs = ix->cdocs ? ix->cdocs * 2 : 256;
        ix->docs = grow(ix->docs, ix->cdocs * sizeof *ix->docs);
    }
    Doc *d = &ix->docs[ix->ndocs];
    memset(d, 0, sizeof *d);
    memcpy(d->id, r->id.b, 16);
    memcpy(d->m, r->c.m, sizeof d->m);
    d->hilbert = lp_hilbert4(&r->c);
    d->tier = r->tier;
    d->line0 = l0;
    d->line1 = l1;
    d->bytes = bytes;
    d->path = path_off;
    d->file = ix->cur_file;
    post_features_range(ix, (uint32_t)ix->ndocs, f0, ix->nfeat);
    ix->ndocs++;
}

/* The concrete syntax tree, composed the way the engine composes a grammar-only recipe:
 * each node is its children with the bytes between them kept as text; a leaf is text. */
static lp_ref ast_node(Lpm *ix, TSNode nd, const uint8_t *src, uint32_t lo, uint32_t hi, int depth) {
    uint32_t nc = ts_node_child_count(nd);
    if (nc == 0 || depth > 200) return text_cached(ix, src + lo, hi > lo ? hi - lo : 0);
    int emit = 0, outer = ix->chunk_on;
    uint32_t l0 = 0, l1 = 0;
    size_t f0 = ix->nfeat;
    if (outer && ts_node_is_named(nd)) {
        l0 = ts_node_start_point(nd).row + 1;
        l1 = ts_node_end_point(nd).row + 1;
        uint32_t span = l1 - l0 + 1;
        if (span >= LPM_CHUNK_MIN_LINES && span <= LPM_CHUNK_MAX_LINES && !(l0 == 1 && l1 >= ix->chunk_lines)) {
            emit = 1;
            ix->chunk_on = 0;           /* its descendants are inside this document */
        }
    }
    TSNode *cn = malloc(sizeof(TSNode) * nc);
    uint32_t *cs = malloc(sizeof(uint32_t) * nc * 2);
    uint32_t *ce = cs + nc;
    if (!cn || !cs) abort();
    TSTreeCursor cur = ts_tree_cursor_new(nd);
    uint32_t got = 0;
    if (ts_tree_cursor_goto_first_child(&cur)) do {
        cn[got] = ts_tree_cursor_current_node(&cur);
        cs[got] = ts_node_start_byte(cn[got]);
        ce[got] = ts_node_end_byte(cn[got]);
        got++;
    } while (got < nc && ts_tree_cursor_goto_next_sibling(&cur));
    ts_tree_cursor_delete(&cur);
    lp_ref *kids = malloc(sizeof(lp_ref) * (2 * (size_t)got + 1));
    uint8_t *has = calloc(2 * (size_t)got + 1, 1);
    if (!kids || !has) abort();
    for (uint32_t i = 0; i < got; i++) if (ce[i] > cs[i]) {
        kids[2 * i + 1] = ast_node(ix, cn[i], src, cs[i], ce[i], depth + 1);
        has[2 * i + 1] = 1;
    }
    uint32_t at = lo;
    for (uint32_t i = 0; i < got; i++) {
        if (cs[i] > at) { kids[2 * i] = text_cached(ix, src + at, cs[i] - at); has[2 * i] = 1; }
        if (ce[i] > at) at = ce[i];
    }
    if (hi > at) { kids[2 * got] = text_cached(ix, src + at, hi - at); has[2 * got] = 1; }
    uint32_t k = 0; uint8_t tier = 0;
    for (uint32_t i = 0; i <= 2 * got; i++) if (has[i]) {
        kids[k] = kids[i];
        if (kids[k].tier > tier) tier = kids[k].tier;
        k++;
    }
    lp_ref r = k ? compose_put(ix, kids, k, (uint8_t)(tier + 1)) : text_cached(ix, src + lo, hi > lo ? hi - lo : 0);
    free(kids); free(has); free(cs); free(cn);
    ix->chunk_on = outer;
    if (emit) push_doc(ix, &r, l0, l1, hi - lo, ix->chunk_path, f0);
    return r;
}

static lp_ref admit_grammar(Lpm *ix, const char *name, const uint8_t *s, size_t n, uint32_t path_off, uint32_t file_lines) {
    const TSLanguage *lang = load_lang(name);
    lp_ref none; memset(&none, 0, sizeof none);
    if (!lang || n > 0xffffffffu) return none;
    TSParser *ps = ts_parser_new();
    ts_parser_set_language(ps, lang);
    TSTree *t = ts_parser_parse_string(ps, NULL, (const char *)s, (uint32_t)n);
    TSNode root = ts_tree_root_node(t);
    uint32_t nc = ts_node_child_count(root);
    if (nc == 0) {
        lp_ref trunk = ast_node(ix, root, s, 0, (uint32_t)n, 0);
        ts_tree_delete(t);
        ts_parser_delete(ps);
        return trunk;
    }
    TSNode *cn = malloc(sizeof(TSNode) * nc);
    uint32_t *cs = malloc(sizeof(uint32_t) * nc * 2);
    uint32_t *ce = cs + nc;
    if (!cn || !cs) abort();
    TSTreeCursor cur = ts_tree_cursor_new(root);
    uint32_t got = 0;
    if (ts_tree_cursor_goto_first_child(&cur)) do {
        cn[got] = ts_tree_cursor_current_node(&cur);
        cs[got] = ts_node_start_byte(cn[got]);
        ce[got] = ts_node_end_byte(cn[got]);
        got++;
    } while (got < nc && ts_tree_cursor_goto_next_sibling(&cur));
    ts_tree_cursor_delete(&cur);
    lp_ref *kids = malloc(sizeof(lp_ref) * (2 * (size_t)got + 1));
    uint8_t *has = calloc(2 * (size_t)got + 1, 1);
    if (!kids || !has) abort();
    /* Each top-level node is a document if it is a declaration-sized span; a larger one (a namespace, a class)
     * is not, and its members are. */
    ix->chunk_path = path_off;
    ix->chunk_lines = file_lines;
    for (uint32_t i = 0; i < got; i++) if (ce[i] > cs[i]) {
        ix->chunk_on = 1;
        kids[2 * i + 1] = ast_node(ix, cn[i], s, cs[i], ce[i], 1);
        ix->chunk_on = 0;
        has[2 * i + 1] = 1;
    }
    uint32_t at = 0;
    for (uint32_t i = 0; i < got; i++) {
        if (cs[i] > at) { kids[2 * i] = text_cached(ix, s + at, cs[i] - at); has[2 * i] = 1; }
        if (ce[i] > at) at = ce[i];
    }
    if ((uint32_t)n > at) { kids[2 * got] = text_cached(ix, s + at, (uint32_t)n - at); has[2 * got] = 1; }
    uint32_t k = 0; uint8_t tier = 0;
    for (uint32_t i = 0; i <= 2 * got; i++) if (has[i]) {
        kids[k] = kids[i];
        if (kids[k].tier > tier) tier = kids[k].tier;
        k++;
    }
    lp_ref trunk = k ? compose_put(ix, kids, k, (uint8_t)(tier + 1)) : text_cached(ix, s, n);
    free(kids); free(has); free(cs); free(cn);
    ts_tree_delete(t);
    ts_parser_delete(ps);
    return trunk;
}

/* Text with no grammar: blocks of lines, cut at a blank line or before a heading once a block holds
 * LPM_TEXT_MIN_LINES, and always at LPM_TEXT_MAX_LINES. Each block is a document; the file stays one too. */
static void admit_text_blocks(Lpm *ix, const uint8_t *s, size_t n, uint32_t path_off, uint32_t file_lines) {
    if (file_lines <= LPM_TEXT_MAX_LINES) return;
    size_t b0 = 0, i = 0;
    uint32_t line = 1, l0 = 1, lines = 0;
    while (i < n) {
        size_t ls = i;
        while (i < n && s[i] != '\n') i++;
        size_t le = i;
        if (i < n) i++;
        int blank = 1;
        for (size_t k = ls; k < le; k++) if (s[k] != ' ' && s[k] != '\t' && s[k] != '\r') { blank = 0; break; }
        int heading = !blank && s[ls] == '#';
        if (heading && lines >= LPM_TEXT_MIN_LINES) {
            size_t f0 = ix->nfeat;
            lp_ref r = text_cached(ix, s + b0, ls - b0);
            push_doc(ix, &r, l0, line - 1, (uint32_t)(ls - b0), path_off, f0);
            b0 = ls; l0 = line; lines = 0;
        }
        lines++;
        if ((blank && lines >= LPM_TEXT_MIN_LINES) || lines >= LPM_TEXT_MAX_LINES || i >= n) {
            size_t f0 = ix->nfeat;
            lp_ref r = text_cached(ix, s + b0, i - b0);
            if (r.tier) push_doc(ix, &r, l0, line, (uint32_t)(i - b0), path_off, f0);
            b0 = i; l0 = line + 1; lines = 0;
        }
        line++;
    }
}

static int is_utf8(const uint8_t *s, size_t n) {
    size_t i = 0; uint32_t cp;
    while (i < n) if (!lp_utf8_next(s, n, &i, &cp)) return 0;
    return 1;
}

int lpm_admit(Lpm *ix, const char *path, const uint8_t *s, size_t n) {
    if (!n) return 0;
    lp_ref trunk;
    memset(&trunk, 0, sizeof trunk);
    ix->nfeat = 0;
    ix->collecting = 1;
    const char *base = path_basename(path);
    const char *dot = base ? strrchr(base, '.') : NULL;
    int utf8 = is_utf8(s, n);
    uint32_t path_off = add_path(ix, path ? path : "");
    /* The same path admitted again replaces what it held. */
    long was = file_find(ix, ix->paths + path_off);
    if (was >= 0) file_kill(ix, (uint32_t)was);
    if (ix->have_meta) ix->cur_file = file_add(ix, path_off, ix->meta_size, ix->meta_mtime, ix->meta_hash);
    else ix->cur_file = file_add(ix, path_off, n, 0, fnv(s, n));
    ix->have_meta = 0;
    uint32_t file_lines = utf8 ? count_lines(s, n) : 0;
    if (dot && (!strcmp(dot, ".tsv") || !strcmp(dot, ".csv")) && utf8) {
        trunk = admit_delimited(ix, s, n, dot[1] == 'c');
    } else {
        const char *g = utf8 ? grammar_name(path) : NULL;
        if (g && load_lang(g)) trunk = admit_grammar(ix, g, s, n, path_off, file_lines);
        else if (utf8) {
            admit_text_blocks(ix, s, n, path_off, file_lines);
            trunk = text_cached(ix, s, n);
        }
        else {
            lp_id *ids = malloc(n * sizeof *ids);
            __int128 sum[4] = {0, 0, 0, 0};
            if (!ids) return -1;
            for (size_t b = 0; b < n; b++) {
                ids[b] = ix->t0[s[b]].id;
                for (int d = 0; d < 4; d++) sum[d] += ix->t0[s[b]].m[d];
            }
            if (n == 1) trunk.id = ids[0];
            else lp_id_compose(ids, n, &trunk.id);
            for (int d = 0; d < 4; d++) trunk.c.m[d] = (int64_t)(sum[d] / (__int128)n);
            trunk.tier = 1;
            int fresh = 0;
            ent_put(ix, &trunk, &fresh);
            ix->compositions++;
            free(ids);
        }
    }
    add_feat(ix, trunk.id.b);
    if (ix->ndocs == ix->cdocs) {
        ix->cdocs = ix->cdocs ? ix->cdocs * 2 : 256;
        ix->docs = grow(ix->docs, ix->cdocs * sizeof *ix->docs);
    }
    Doc *doc = &ix->docs[ix->ndocs];
    memset(doc, 0, sizeof *doc);
    memcpy(doc->id, trunk.id.b, 16);
    memcpy(doc->m, trunk.c.m, sizeof doc->m);
    doc->hilbert = lp_hilbert4(&trunk.c);
    doc->tier = trunk.tier;
    doc->line0 = 1;
    doc->line1 = file_lines;
    doc->bytes = n > 0xffffffffu ? 0xffffffffu : (uint32_t)n;
    doc->path = path_off;
    doc->file = ix->cur_file;
    post_features(ix, (uint32_t)ix->ndocs);
    ix->ndocs++;
    ix->fl[ix->cur_file].ndoc = (uint32_t)(ix->ndocs - ix->fl[ix->cur_file].doc0);
    ix->collecting = 0;
    ix->files++;
    ix->bytes += n;
    return 0;
}

static int wanted(const char *name) {
    static const char *exts[] = {
        ".c", ".h", ".cc", ".cpp", ".hpp", ".cs", ".sql", ".in", ".toml", ".tsv",
        ".sh", ".ps1", ".py", ".ts", ".tsx", ".js", ".json", ".yml", ".yaml",
        ".recipe", ".cmake", ".control", ".env", ".lmf", ".ttl", ".md", ".txt", ".rst",
        NULL
    };
    static const char *names[] = {"CMakeLists.txt", "Makefile", "source", "order", "Dockerfile", NULL};
    for (int i = 0; names[i]; i++) if (!strcmp(name, names[i])) return 1;
    const char *dot = strrchr(name, '.');
    if (!dot) return 0;
    for (int i = 0; exts[i]; i++) if (!strcmp(dot, exts[i])) return 1;
    return 0;
}

static int skip_dir(const char *name) {
    static const char *s[] = {".git", "site", "build", "out", "node_modules", "bin", "obj",
                              ".venv", "__pycache__", "vcpkg_installed", "dist", NULL};
    for (int i = 0; s[i]; i++) if (!strcmp(name, s[i])) return 1;
    return 0;
}

static int64_t mtime_ns(const struct statx *x) {
    return (int64_t)x->stx_mtime.tv_sec * 1000000000LL + (int64_t)x->stx_mtime.tv_nsec;
}

/* 1 and the bytes in *buf when the file is one to admit, 0 when it is skipped, -1 when memory ran out. */
static int read_small(Lpm *ix, const char *path, uint8_t **buf, size_t *len) {
    *buf = NULL;
    *len = 0;
    if (!os_exists(path) || os_is_dir(path)) return 0;
    FILE *f = fopen(path, "rb");
    if (!f) { ix->skipped++; return 0; }
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); ix->skipped++; return 0; }
    long sz = ftell(f);
    if (sz <= 0 || sz > (1 << 20) || fseek(f, 0, SEEK_SET) != 0) { fclose(f); ix->skipped++; return 0; }
    uint8_t *b = malloc((size_t)sz);
    if (!b) { fclose(f); return -1; }
    size_t n = fread(b, 1, (size_t)sz, f);
    fclose(f);
    if (n != (size_t)sz) { free(b); ix->skipped++; return 0; }
    *buf = b;
    *len = n;
    return 1;
}

static int admit_read(Lpm *ix, const char *path, const uint8_t *buf, size_t n, int64_t mtime) {
    ix->have_meta = 1;
    ix->meta_size = n;
    ix->meta_mtime = mtime;
    ix->meta_hash = fnv(buf, n);
    return lpm_admit(ix, path, buf, n);
}

static int index_file(Lpm *ix, const char *path) {
    struct statx x;
    memset(&x, 0, sizeof x);
    if (statx(AT_FDCWD, path, 0, STATX_SIZE | STATX_MTIME, &x) != 0) return 0;
    uint8_t *buf;
    size_t n;
    int got = read_small(ix, path, &buf, &n);
    if (got <= 0) return got;
    int rc = admit_read(ix, path, buf, n, mtime_ns(&x));
    free(buf);
    return rc;
}

/* A file met again: unchanged when its size and time agree, or its content does; read and admitted again
 * otherwise, which leaves what it held before dead. */
static int update_file(Lpm *ix, const char *path, LpmUpdate *u) {
    char key[4096];
    norm_path(key, sizeof key, path);
    struct statx x;
    memset(&x, 0, sizeof x);
    if (statx(AT_FDCWD, path, 0, STATX_SIZE | STATX_MTIME, &x) != 0) return 0;
    int64_t mt = mtime_ns(&x);
    long f = file_find(ix, key);
    if (f >= 0 && ix->fl[f].size == x.stx_size && ix->fl[f].mtime == mt) {
        ix->fl[f].seen = 1;
        u->unchanged++;
        return 0;
    }
    uint8_t *buf;
    size_t n;
    int got = read_small(ix, path, &buf, &n);
    if (got <= 0) return got;
    if (f >= 0 && ix->fl[f].size == n && ix->fl[f].hash == fnv(buf, n)) {
        ix->fl[f].mtime = mt;
        ix->fl[f].seen = 1;
        u->unchanged++;
        free(buf);
        return 0;
    }
    int rc = admit_read(ix, path, buf, n, mt);
    free(buf);
    if (f >= 0) u->changed++;
    else u->added++;
    return rc;
}

static int walk_dir(Lpm *ix, const char *path, LpmUpdate *u) {
    os_list ls = {0};
    if (os_list_dir(path, &ls) != 0) return -1;
    for (size_t i = 0; i < ls.n; i++) {
        const char *name = ls.item[i];
        if (!strcmp(name, ".") || !strcmp(name, "..")) continue;
        char child[4096];
        int nw = snprintf(child, sizeof child, "%s/%s", path, name);
        if (nw >= 0 && (size_t)nw < sizeof child) {
#ifndef _WIN32
            struct stat lst;
            if (lstat(child, &lst) == 0 && S_ISLNK(lst.st_mode)) continue;
#endif
            if (os_is_dir(child)) {
                if (!skip_dir(name)) {
                    if (walk_dir(ix, child, u)) {
                        os_list_free(&ls);
                        return -1;
                    }
                }
            } else if (wanted(name)) {
                if (u ? update_file(ix, child, u) : index_file(ix, child)) {
                    os_list_free(&ls);
                    return -1;
                }
            }
        }
    }
    os_list_free(&ls);
    return 0;
}

int lpm_index_path(Lpm *ix, const char *path) {
    if (!path || !*path) return -1;
    if (os_is_dir(path)) return walk_dir(ix, path, NULL);
    if (!os_exists(path)) return -1;
    return index_file(ix, path);
}

static int under(const char *p, const char *root, size_t rl) {
    return !strncmp(p, root, rl) && (p[rl] == '/' || p[rl] == 0);
}

int lpm_update_path(Lpm *ix, const char *path, LpmUpdate *u) {
    if (!path || !*path || !u) return -1;
    char root[4096];
    norm_path(root, sizeof root, path);
    size_t rl = strlen(root);
    for (size_t i = 0; i < ix->nfl; i++)
        if (!ix->fl[i].dead && under(ix->paths + ix->fl[i].path, root, rl)) ix->fl[i].seen = 0;
    int rc = 0;
    if (os_is_dir(path)) rc = walk_dir(ix, path, u);
    else if (os_exists(path) && wanted(path_basename(path))) rc = update_file(ix, path, u);
    if (rc) return rc;
    /* What was under the root and was not met again is gone. */
    for (size_t i = 0; i < ix->nfl; i++) {
        LFile *f = &ix->fl[i];
        if (!f->dead && !f->seen && under(ix->paths + f->path, root, rl)) {
            file_kill(ix, (uint32_t)i);
            u->removed++;
        }
    }
    return 0;
}

size_t lpm_search(Lpm *ix, const char *q, size_t n, LpmHit *out, size_t cap) {
    if (!cap || !ix->ndocs) return 0;
    uint64_t saved_files = ix->files, saved_bytes = ix->bytes, saved_comp = ix->compositions;
    size_t saved_ndocs = ix->ndocs;
    ix->nfeat = 0;
    ix->collecting = 1;
    size_t i = 0;
    uint32_t cp;
    int utf8 = 1;
    const uint8_t *s = (const uint8_t *)q;
    while (i < n) {
        if (!lp_utf8_next(s, n, &i, &cp)) { utf8 = 0; break; }
    }
    lp_ref trunk;
    if (utf8 && n) {
        trunk = lp_text_decompose(ix->text, s, n, on_compose, ix);
        collect_subtoken_feats(ix, s, n);
    } else memset(&trunk, 0, sizeof trunk);
    add_feat(ix, trunk.id.b);
    ix->collecting = 0;
    /* A query is not a document. Roll the occurrence counters back. Entities stay:
     * the query's own constituents are real records. */
    ix->files = saved_files;
    ix->bytes = saved_bytes;
    ix->compositions = saved_comp;
    ix->ndocs = saved_ndocs;

    if (ix->nfeat > 1) {
        qsort(ix->feat, ix->nfeat, 16, cmp16);
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
    double *rank = calloc(ix->ndocs, sizeof *rank);
    if (!shared || !rank) abort();
    /* BM25 with presence only: each shared constituent adds its IDF, and a document's sum is scaled by its
     * length against the mean, so a declaration that holds the query outranks the whole file around it. */
    const double k1 = 1.2, b = 0.75;
    double avg;
    size_t live = live_stats(ix, &avg);
    if (!live) { free(shared); free(rank); return 0; }
    for (size_t f = 0; f < ix->nfeat; f++) {
        Ent *e = ent_find(ix, ix->feat + f * 16);
        if (!e || !e->nd) continue;
        double idf = log(1.0 + (double)live / (double)e->nd);
        for (uint32_t d = 0; d < e->nd; d++) {
            uint32_t di = e->docs[d];
            if (di >= ix->ndocs || ix->fl[ix->docs[di].file].dead) continue;
            shared[di]++;
            rank[di] += idf;
        }
    }
    for (size_t d = 0; d < ix->ndocs; d++) if (shared[d]) {
        double norm = 1.0 - b + b * (double)ix->docs[d].bytes / avg;
        rank[d] *= (k1 + 1.0) / (1.0 + k1 * norm);
    }
    uint8_t *taken = calloc(ix->ndocs, 1);
    if (!taken) abort();
    size_t wrote = 0;
    while (wrote < cap) {
        uint32_t best = 0;
        int any = 0;
        for (size_t d = 0; d < ix->ndocs; d++) {
            if (taken[d] || !shared[d]) continue;
            if (!any || rank[d] > rank[best] || (rank[d] == rank[best] && ix->docs[d].bytes < ix->docs[best].bytes)) {
                best = (uint32_t)d;
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
    free(shared);
    free(rank);
    free(taken);
    return wrote;
}

size_t lpm_containers(const Lpm *ix, const uint8_t id[16], LpmHit *out, size_t cap) {
    size_t n = 0;
    Ent *e = ent_find(ix, id);
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

static void unit_s3(const int64_t m[4], double p[4]) {
    double s2 = 0.0;
    for (int d = 0; d < 4; d++) {
        p[d] = ldexp((double)m[d], -53);
        s2 += p[d] * p[d];
    }
    if (s2 > 1e-30) {
        double inv = 1.0 / sqrt(s2);
        for (int d = 0; d < 4; d++) p[d] *= inv;
    } else {
        p[0] = 1.0; p[1] = 0.0; p[2] = 0.0; p[3] = 0.0;
    }
}

static double dist4(const double a[4], const double b[4]) {
    double dx = a[0] - b[0], dy = a[1] - b[1], dz = a[2] - b[2], dw = a[3] - b[3];
    return sqrt(dx * dx + dy * dy + dz * dz + dw * dw);
}

static double directed_hausdorff4(const double *a, size_t na, const double *b, size_t nb) {
    if (!na || !nb) return 2.0;
    double max_min = 0.0;
    for (size_t i = 0; i < na; i++) {
        double min_d = 1e9;
        const double *ai = a + i * 4;
        for (size_t j = 0; j < nb; j++) {
            double d = dist4(ai, b + j * 4);
            if (d < min_d) min_d = d;
        }
        if (min_d > max_min) max_min = min_d;
    }
    return max_min;
}

static double subtraj_frechet4(const double *q, size_t nq, const double *d, size_t nd) {
    if (!nq || !nd) return -1.0;
    if (nd <= nq) return lp_frechet4(q, nq, d, nd);

    const double match_thresh = 0.25;
    size_t first_match = nd, last_match = 0;
    int any_match = 0;
    for (size_t j = 0; j < nd; j++) {
        const double *dj = d + j * 4;
        for (size_t i = 0; i < nq; i++) {
            if (dist4(dj, q + i * 4) <= match_thresh) {
                if (!any_match) first_match = j;
                last_match = j;
                any_match = 1;
                break;
            }
        }
    }

    if (any_match) {
        size_t pad = nq <= 4 ? 1 : 2;
        size_t w_start = first_match > pad ? first_match - pad : 0;
        size_t w_end = last_match + pad < nd - 1 ? last_match + pad : nd - 1;
        size_t max_win = nq * 3 > 8 ? nq * 3 : 8;
        if (w_end - w_start + 1 > max_win) w_end = w_start + max_win - 1;

        size_t win_len = w_end - w_start + 1;
        double *core = malloc(win_len * 4 * sizeof(double));
        size_t ncore = 0;
        for (size_t j = w_start; j <= w_end; j++) {
            const double *dj = d + j * 4;
            for (size_t i = 0; i < nq; i++) {
                if (dist4(dj, q + i * 4) <= match_thresh) {
                    memcpy(core + ncore * 4, dj, 4 * sizeof(double));
                    ncore++;
                    break;
                }
            }
        }
        size_t matched_q = 0;
        for (size_t i = 0; i < nq; i++) {
            const double *qi = q + i * 4;
            for (size_t k = 0; k < ncore; k++) {
                if (dist4(qi, core + k * 4) <= match_thresh) {
                    matched_q++;
                    break;
                }
            }
        }
        double core_frechet = ncore ? lp_frechet4(q, nq, core, ncore) : lp_frechet4(q, nq, d + w_start * 4, win_len);
        double core_haus = ncore ? directed_hausdorff4(q, nq, core, ncore) : core_frechet;
        free(core);
        double core_shape = ncore >= 2 ? fmin(core_frechet, core_haus + 0.12) : core_frechet;
        double cov_pen = (1.0 - (double)matched_q / (double)nq) * 0.45;
        double win_frechet = lp_frechet4(q, nq, d + w_start * 4, win_len);
        return fmin(core_shape + cov_pen, win_frechet);
    }

    size_t w = nq + 2 < nd ? nq + 2 : nd;
    size_t stride = nq / 2 > 1 ? nq / 2 : 1;
    double best = lp_frechet4(q, nq, d, nd);
    for (size_t s = 0; s + w <= nd; s += stride) {
        double f = lp_frechet4(q, nq, d + s * 4, w);
        if (f < best) best = f;
    }
    return best;
}

static void traj_walk(const Lpm *ix, const uint8_t id[16], double **pts, size_t *n, size_t *cap, int depth) {
    Ent *e = ent_find(ix, id);
    if (!e || depth > 64) return;
    if (!e->nkids || e->tier <= 2) {
        if (*n == *cap) {
            *cap = *cap ? *cap * 2 : 64;
            *pts = grow(*pts, *cap * 4 * sizeof(double));
        }
        unit_s3(e->m, *pts + *n * 4);
        (*n)++;
        return;
    }
    for (uint32_t i = 0; i < e->nkids; i++) traj_walk(ix, e->kids + (size_t)i * 16, pts, n, cap, depth + 1);
}

double lpm_frechet(const Lpm *ix, const uint8_t a[16], const uint8_t b[16]) {
    if (!ent_find(ix, a) || !ent_find(ix, b)) return -1;
    if (!memcmp(a, b, 16)) return 0.0;
    double *pa = NULL, *pb = NULL;
    size_t na = 0, nb = 0, ca = 0, cb = 0;
    traj_walk(ix, a, &pa, &na, &ca, 0);
    traj_walk(ix, b, &pb, &nb, &cb, 0);
    double d = (na && nb) ? subtraj_frechet4(pa, na, pb, nb) : -1;
    free(pa);
    free(pb);
    return d;
}

static void extract_word_traj(Lpm *ix, const uint8_t *s, size_t n, double **pts_out, size_t *npts_out, double cent_out[4]) {
    double *all_pts = NULL, *content_pts = NULL;
    size_t n_all = 0, cap_all = 0, n_cont = 0, cap_cont = 0;
    double sum[4] = {0, 0, 0, 0};
    size_t i = 0;
    while (i < n) {
        while (i < n && !is_ascii_alnum(s[i])) i++;
        if (i >= n) break;
        size_t w0 = i;
        while (i < n && is_ascii_alnum(s[i])) i++;
        size_t sub0 = w0;
        for (size_t k = w0 + 1; k <= i; k++) {
            int split = (k == i);
            if (!split) {
                uint8_t prev = s[k - 1], cur = s[k];
                int prev_lo = (prev >= 'a' && prev <= 'z');
                int prev_up = (prev >= 'A' && prev <= 'Z');
                int prev_dg = (prev >= '0' && prev <= '9');
                int cur_up  = (cur >= 'A' && cur <= 'Z');
                int cur_dg  = (cur >= '0' && cur <= '9');
                int next_lo = (k + 1 < i) && (s[k + 1] >= 'a' && s[k + 1] <= 'z');
                if ((prev_lo && cur_up) || (prev_up && cur_up && next_lo) || (prev_dg != cur_dg)) split = 1;
            }
            if (split) {
                size_t slen = k - sub0;
                if (slen >= 1 && slen <= 64) {
                    uint8_t low[64];
                    for (size_t b = 0; b < slen; b++) {
                        uint8_t c = s[sub0 + b];
                        low[b] = (c >= 'A' && c <= 'Z') ? (uint8_t)(c - 'A' + 'a') : c;
                    }
                    lp_ref r = lower_token_ref(ix, low, slen, 0);
                    double pt[4];
                    unit_s3(r.c.m, pt);
                    if (n_all == cap_all) {
                        cap_all = cap_all ? cap_all * 2 : 32;
                        all_pts = grow(all_pts, cap_all * 4 * sizeof(double));
                    }
                    memcpy(all_pts + n_all * 4, pt, 4 * sizeof(double));
                    n_all++;
                    if (!is_stop_word_bytes(low, slen)) {
                        if (n_cont == cap_cont) {
                            cap_cont = cap_cont ? cap_cont * 2 : 32;
                            content_pts = grow(content_pts, cap_cont * 4 * sizeof(double));
                        }
                        memcpy(content_pts + n_cont * 4, pt, 4 * sizeof(double));
                        n_cont++;
                        for (int d = 0; d < 4; d++) sum[d] += pt[d];
                    }
                }
                sub0 = k;
            }
        }
    }
    if (n_cont > 0) {
        free(all_pts);
        *pts_out = content_pts;
        *npts_out = n_cont;
    } else {
        free(content_pts);
        *pts_out = all_pts;
        *npts_out = n_all;
        for (size_t k = 0; k < n_all; k++) {
            for (int d = 0; d < 4; d++) sum[d] += all_pts[k * 4 + d];
        }
    }
    double norm2 = sum[0] * sum[0] + sum[1] * sum[1] + sum[2] * sum[2] + sum[3] * sum[3];
    if (norm2 > 1e-30) {
        double inv = 1.0 / sqrt(norm2);
        for (int d = 0; d < 4; d++) cent_out[d] = sum[d] * inv;
    } else {
        cent_out[0] = 1.0; cent_out[1] = 0.0; cent_out[2] = 0.0; cent_out[3] = 0.0;
    }
}

static size_t dedup_feat(uint8_t *buf, size_t n) {
    if (n <= 1) return n;
    qsort(buf, n, 16, cmp16);
    size_t w = 1;
    for (size_t k = 1; k < n; k++) {
        if (memcmp(buf + k * 16, buf + (w - 1) * 16, 16)) {
            memcpy(buf + w * 16, buf + k * 16, 16);
            w++;
        }
    }
    return w;
}

void lpm_rerank_parts(Lpm *ix, const char *query, size_t qn, const char *doc, size_t dn, LpmRerank *out) {
    memset(out, 0, sizeof *out);
    out->frechet = -1.0;
    uint32_t *shared_out = &out->shared;
    double *frechet_out = &out->frechet;
    if (!ix || !qn || !dn) return;
    /* Corpus statistics before the query and the candidate are decomposed into the index's entities. */
    double avg;
    size_t live = live_stats(ix, &avg);
    size_t all_docs = ix->ndocs;

    uint64_t saved_files = ix->files, saved_bytes = ix->bytes, saved_comp = ix->compositions;
    size_t saved_ndocs = ix->ndocs;

    ix->nfeat = 0;
    ix->collecting = 1;
    lp_ref qr = {0};
    if (is_utf8((const uint8_t *)query, qn)) {
        qr = lp_text_decompose(ix->text, (const uint8_t *)query, qn, on_compose, ix);
        collect_subtoken_feats(ix, (const uint8_t *)query, qn);
    }
    if (qr.tier) add_feat(ix, qr.id.b);
    size_t qfn = dedup_feat(ix->feat, ix->nfeat);
    uint8_t *qfeat = qfn ? malloc(qfn * 16) : NULL;
    if (qfn && qfeat) memcpy(qfeat, ix->feat, qfn * 16);

    ix->nfeat = 0;
    lp_ref dr = {0};
    if (is_utf8((const uint8_t *)doc, dn)) {
        dr = lp_text_decompose(ix->text, (const uint8_t *)doc, dn, on_compose, ix);
        collect_subtoken_feats(ix, (const uint8_t *)doc, dn);
    }
    if (dr.tier) add_feat(ix, dr.id.b);
    size_t dfn = dedup_feat(ix->feat, ix->nfeat);
    ix->collecting = 0;

    ix->files = saved_files;
    ix->bytes = saved_bytes;
    ix->compositions = saved_comp;
    ix->ndocs = saved_ndocs;

    /* Lexical evidence. A query constituent the candidate holds counts with its IDF weight times the chance that
     * holding it is not an accident of length: a document of L bytes holds a constituent found in a fraction r
     * of the index's documents (mean length A bytes) with probability p = 1 - (1 - r)^(L/A), so the match is
     * worth w * (1 - p). A short span that holds the query keeps nearly all its weight; a long one that holds
     * the same constituents keeps less, and a constituent every document holds is worth nothing. */
    uint32_t shared = 0;
    double overlap_w = 0.0, total_w = 0.0;
    double len_ratio = live ? (double)dn / avg : 0.0;
    size_t i = 0, j = 0;
    while (i < qfn) {
        Ent *qe = ent_find(ix, qfeat + i * 16);
        int known = qe && qe->nd && all_docs;
        double w = known ? (1.0 + log((double)(all_docs + 1) / (double)qe->nd)) : 1.0;
        double rate = known ? (double)qe->nd / (double)all_docs : 0.0;
        if (rate > 1.0) rate = 1.0;
        double chance = rate > 0.0 && len_ratio > 0.0 ? 1.0 - pow(1.0 - rate, len_ratio) : 0.0;
        total_w += w;
        while (j < dfn && memcmp(ix->feat + j * 16, qfeat + i * 16, 16) < 0) j++;
        if (j < dfn && !memcmp(ix->feat + j * 16, qfeat + i * 16, 16)) {
            shared++;
            overlap_w += w * (1.0 - chance);
        }
        i++;
    }
    free(qfeat);

    double *q_pts = NULL, *d_pts = NULL;
    size_t nq_pts = 0, nd_pts = 0;
    double q_cent[4], d_cent[4];
    extract_word_traj(ix, (const uint8_t *)query, qn, &q_pts, &nq_pts, q_cent);
    extract_word_traj(ix, (const uint8_t *)doc, dn, &d_pts, &nd_pts, d_cent);

    double fr = (nq_pts && nd_pts)
        ? subtraj_frechet4(q_pts, nq_pts, d_pts, nd_pts)
        : ((qr.tier && dr.tier) ? lpm_frechet(ix, qr.id.b, dr.id.b) : -1.0);
    free(q_pts);
    free(d_pts);

    if (!memcmp(qr.id.b, dr.id.b, 16) && qr.tier) {
        if (shared_out) *shared_out = shared;
        if (frechet_out) *frechet_out = 0.0;
        out->score = out->lexical = out->geometric = out->centroid = 1.0;
        return;
    }

    if (shared_out) *shared_out = shared;
    if (frechet_out) *frechet_out = fr;

    double lex = total_w > 0.0 ? (overlap_w / total_w) : 0.0;
    double geom = fr >= 0.0 ? (1.0 / (1.0 + fr)) : 0.0;
    double cent_dist = dist4(q_cent, d_cent);
    double cent_sim = 1.0 / (1.0 + cent_dist);
    /* Shape and centroid say how well the evidence is arranged, so they scale it: with no lexical evidence the
     * score is 0 whatever the length, instead of the constant the shape terms give any text. */
    double score = lex * (0.55 + 0.35 * geom + 0.10 * cent_sim);
    out->lexical = lex;
    out->geometric = geom;
    out->centroid = cent_sim;
    out->score = score > 1.0 ? 1.0 : score;
}

double lpm_rerank(Lpm *ix, const char *query, size_t qn, const char *doc, size_t dn, uint32_t *shared_out, double *frechet_out) {
    LpmRerank r;
    lpm_rerank_parts(ix, query, qn, doc, dn, &r);
    if (shared_out) *shared_out = r.shared;
    if (frechet_out) *frechet_out = r.frechet;
    return r.score;
}

int lpm_identify(Lpm *ix, const char *s, size_t n, LpmRec *out) {
    memset(out, 0, sizeof *out);
    lp_id flat;
    if (!lp_id_codepoints_utf8(s, n, &flat)) return -1;
    ix->collecting = 0;
    lp_ref trunk = lp_text_decompose(ix->text, (const uint8_t *)s, n, on_compose, ix);
    memcpy(out->id, trunk.id.b, 16);
    memcpy(out->m, trunk.c.m, sizeof out->m);
    out->hilbert = lp_hilbert4(&trunk.c);
    out->tier = trunk.tier;
    out->found = 1;
    Ent *e = ent_find(ix, trunk.id.b);
    if (e) out->seen = e->seen;
    (void)flat;
    return memcmp(flat.b, trunk.id.b, 16) ? 1 : 0;
}

int lpm_lookup(const Lpm *ix, const uint8_t id[16], LpmRec *out) {
    memset(out, 0, sizeof *out);
    Ent *e = ent_find(ix, id);
    if (!e) return -1;
    memcpy(out->id, e->id, 16);
    memcpy(out->m, e->m, sizeof out->m);
    out->hilbert = e->hilbert;
    out->tier = e->tier;
    out->seen = e->seen;
    out->found = 1;
    return 0;
}

int lpm_fetch(const Lpm *ix, const uint8_t id[16], LpmRec *out, uint8_t *kids, uint32_t cap, uint32_t *nkids) {
    if (lpm_lookup(ix, id, out)) { if (nkids) *nkids = 0; return -1; }
    Ent *e = ent_find(ix, id);
    uint32_t n = e && e->kids ? e->nkids : 0;
    if (nkids) *nkids = n;
    if (kids && cap && n) memcpy(kids, e->kids, (n < cap ? n : cap) * 16);
    return 0;
}

static void emit_node(const Lpm *ix, const uint8_t id[16], FILE *o) {
    Ent *e = ent_find(ix, id);
    if (e && e->nkids > 1) {
        fputc('[', o);
        for (uint32_t i = 0; i < e->nkids; i++) {
            if (i) fputc(',', o);
            emit_node(ix, e->kids + (size_t)i * 16, o);
        }
        fputc(']', o);
        return;
    }
    int64_t cp = lp_tier0_codepoint(ix->t0, (const lp_id *)id);
    if (cp == ' ') { fputs("' '", o); return; }
    if (cp >= 32 && cp < 127) { fputc(cp, o); return; }
    if (cp >= 0) { fprintf(o, "U+%04X", (unsigned)cp); return; }
    if (e && e->nkids == 1) { emit_node(ix, e->kids, o); return; }
    fputs("?", o);
}

void lpm_show_text(Lpm *ix, const uint8_t *s, size_t n, void *file) {
    FILE *o = file;
    static const char H[] = "0123456789abcdef";
    ix->collecting = 0;
    lp_ref trunk = lp_text_decompose(ix->text, s, n, on_compose, ix);
    char id[33];
    for (int b = 0; b < 16; b++) { id[b * 2] = H[trunk.id.b[b] >> 4]; id[b * 2 + 1] = H[trunk.id.b[b] & 15]; }
    id[32] = 0;
    fprintf(o, "id        %s\n", id);
    fprintf(o, "tier      %u\n", trunk.tier);
    fprintf(o, "hilbert   %llu\n", (unsigned long long)lp_hilbert4(&trunk.c));
    fprintf(o, "m         %lld %lld %lld %lld\n",
            (long long)trunk.c.m[0], (long long)trunk.c.m[1], (long long)trunk.c.m[2], (long long)trunk.c.m[3]);
    emit_node(ix, trunk.id.b, o);
    fputc('\n', o);
}

int lpm_last(const Lpm *ix, LpmHit *out) {
    if (!ix->ndocs) return -1;
    Doc *d = &ix->docs[ix->ndocs - 1];
    memset(out, 0, sizeof *out);
    memcpy(out->id, d->id, 16);
    memcpy(out->m, d->m, sizeof out->m);
    out->hilbert = d->hilbert;
    out->line0 = d->line0;
    out->line1 = d->line1;
    out->bytes = d->bytes;
    snprintf(out->path, sizeof out->path, "%s", ix->paths + d->path);
    return 0;
}

void lpm_stats(const Lpm *ix, LpmStats *s) {
    memset(s, 0, sizeof *s);
    for (size_t i = 0; i < ix->nfl; i++) if (!ix->fl[i].dead) { s->files++; s->bytes += ix->fl[i].size; }
    s->skipped = ix->skipped;
    s->entities = ix->nent;
    s->documents = ix->ndocs - ix->dead_docs;
    s->stale = ix->dead_docs;
    s->compositions = ix->compositions;
    for (size_t i = 0; i < ix->nslot; i++) if (ix->slot[i].used && ix->slot[i].nd) s->words++;
}

static void wr(FILE *f, const void *p, size_t n) {
    if (fwrite(p, 1, n, f) != n) abort();
}

/* Records, version 2: entities with their constituents, files with what reindexing compares, documents,
 * postings. Everything an index held is there when it is loaded. */
int lpm_save(const Lpm *ix, const char *path) {
    char tmp[4200];
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *f = fopen(tmp, "wb");
    if (!f) return -1;
    wr(f, "LPM1", 4);
    uint32_t ver = 2;
    wr(f, &ver, 4);
    uint64_t nent = ix->nent, nd = ix->ndocs, nf = ix->nfl;
    wr(f, &nent, 8);
    for (size_t i = 0; i < ix->nslot; i++) {
        Ent *e = &ix->slot[i];
        if (!e->used) continue;
        wr(f, e->id, 16);
        wr(f, e->m, 32);
        wr(f, &e->hilbert, 8);
        wr(f, &e->seen, 4);
        wr(f, &e->tier, 1);
        uint8_t pad[3] = {0};
        wr(f, pad, 3);
        wr(f, &e->nkids, 4);
        if (e->nkids) wr(f, e->kids, (size_t)e->nkids * 16);
    }
    wr(f, &nf, 8);
    for (size_t i = 0; i < ix->nfl; i++) {
        const LFile *fl = &ix->fl[i];
        const char *p = ix->paths + fl->path;
        uint32_t pl = (uint32_t)strlen(p);
        wr(f, &pl, 4);
        wr(f, p, pl);
        wr(f, &fl->size, 8);
        wr(f, &fl->mtime, 8);
        wr(f, &fl->hash, 8);
        wr(f, &fl->doc0, 4);
        wr(f, &fl->ndoc, 4);
        wr(f, &fl->dead, 1);
    }
    wr(f, &nd, 8);
    for (size_t i = 0; i < ix->ndocs; i++) {
        Doc *d = &ix->docs[i];
        wr(f, d->id, 16);
        wr(f, d->m, 32);
        wr(f, &d->hilbert, 8);
        wr(f, &d->line0, 4);
        wr(f, &d->line1, 4);
        wr(f, &d->bytes, 4);
        wr(f, &d->tier, 1);
        wr(f, &d->file, 4);
    }
    uint64_t np = 0;
    for (size_t i = 0; i < ix->nslot; i++) if (ix->slot[i].used && ix->slot[i].nd) np++;
    wr(f, &np, 8);
    for (size_t i = 0; i < ix->nslot; i++) {
        Ent *e = &ix->slot[i];
        if (!e->used || !e->nd) continue;
        wr(f, e->id, 16);
        wr(f, &e->nd, 4);
        wr(f, e->docs, e->nd * 4);
    }
    if (fclose(f) != 0) return -1;
    remove(path);
    return rename(tmp, path) == 0 ? 0 : -1;
}

static int rd(FILE *f, void *p, size_t n) {
    return fread(p, 1, n, f) == n;
}

/* NULL when the file is missing, not records, another version, or cut short. */
Lpm *lpm_load(const char *path, const char *tier0) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    char mag[4];
    uint32_t ver = 0;
    if (!rd(f, mag, 4) || memcmp(mag, "LPM1", 4) || !rd(f, &ver, 4) || ver != 2) { fclose(f); return NULL; }
    Lpm *ix = lpm_new(tier0);
    if (!ix) { fclose(f); return NULL; }
    uint64_t nent;
    if (!rd(f, &nent, 8)) goto bad;
    for (uint64_t i = 0; i < nent; i++) {
        lp_ref r;
        memset(&r, 0, sizeof r);
        uint32_t seen, nk;
        uint8_t tier, pad[3];
        uint64_t hilbert;
        if (!rd(f, r.id.b, 16) || !rd(f, r.c.m, 32) || !rd(f, &hilbert, 8) || !rd(f, &seen, 4) || !rd(f, &tier, 1) ||
            !rd(f, pad, 3) || !rd(f, &nk, 4)) goto bad;
        r.tier = tier;
        int fresh = 0;
        Ent *e = ent_put(ix, &r, &fresh);
        e->seen = seen;
        e->hilbert = hilbert;
        if (nk) {
            uint8_t *k = malloc((size_t)nk * 16);
            if (!k) abort();
            if (!rd(f, k, (size_t)nk * 16)) { free(k); goto bad; }
            free(e->kids);
            e->kids = k;
            e->nkids = nk;
        }
    }
    uint64_t nf;
    if (!rd(f, &nf, 8)) goto bad;
    for (uint64_t i = 0; i < nf; i++) {
        uint32_t pl;
        if (!rd(f, &pl, 4) || pl > 65535) goto bad;
        char *p = malloc((size_t)pl + 1);
        if (!p) abort();
        if (!rd(f, p, pl)) { free(p); goto bad; }
        p[pl] = 0;
        uint64_t size, hash;
        int64_t mtime;
        uint32_t doc0, ndoc;
        uint8_t dead;
        if (!rd(f, &size, 8) || !rd(f, &mtime, 8) || !rd(f, &hash, 8) || !rd(f, &doc0, 4) || !rd(f, &ndoc, 4) ||
            !rd(f, &dead, 1)) { free(p); goto bad; }
        uint32_t off = add_path(ix, p);
        free(p);
        if (ix->nfl == ix->cfl) {
            ix->cfl = ix->cfl ? ix->cfl * 2 : 256;
            ix->fl = grow(ix->fl, ix->cfl * sizeof *ix->fl);
        }
        LFile *fl = &ix->fl[ix->nfl++];
        memset(fl, 0, sizeof *fl);
        fl->path = off;
        fl->size = size;
        fl->mtime = mtime;
        fl->hash = hash;
        fl->doc0 = doc0;
        fl->ndoc = ndoc;
        fl->dead = dead;
        fl->seen = 1;
        if (dead) ix->dead_docs += ndoc;
        else ix->files++;
    }
    size_t cap = 1024;
    while (cap < (ix->nfl + 1) * 2) cap *= 2;
    fmap_rehash(ix, cap);
    uint64_t nd;
    if (!rd(f, &nd, 8)) goto bad;
    for (uint64_t i = 0; i < nd; i++) {
        if (ix->ndocs == ix->cdocs) {
            ix->cdocs = ix->cdocs ? ix->cdocs * 2 : 256;
            ix->docs = grow(ix->docs, ix->cdocs * sizeof *ix->docs);
        }
        Doc *d = &ix->docs[ix->ndocs];
        memset(d, 0, sizeof *d);
        uint8_t tier;
        if (!rd(f, d->id, 16) || !rd(f, d->m, 32) || !rd(f, &d->hilbert, 8) || !rd(f, &d->line0, 4) ||
            !rd(f, &d->line1, 4) || !rd(f, &d->bytes, 4) || !rd(f, &tier, 1) || !rd(f, &d->file, 4)) goto bad;
        if (d->file >= ix->nfl) goto bad;
        d->tier = tier;
        d->path = ix->fl[d->file].path;
        ix->ndocs++;
    }
    uint64_t np;
    if (!rd(f, &np, 8)) goto bad;
    for (uint64_t i = 0; i < np; i++) {
        uint8_t id[16];
        uint32_t count;
        if (!rd(f, id, 16) || !rd(f, &count, 4)) goto bad;
        Ent *e = ent_find(ix, id);
        if (!e) goto bad;
        e->cd = count;
        e->docs = malloc((count ? count : 1) * sizeof *e->docs);
        if (!e->docs) abort();
        if (!rd(f, e->docs, (size_t)count * 4)) goto bad;
        e->nd = count;
    }
    fclose(f);
    return ix;
bad:
    fclose(f);
    lpm_free(ix);
    return NULL;
}
