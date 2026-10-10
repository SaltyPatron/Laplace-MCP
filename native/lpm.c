#define _GNU_SOURCE
#include "lpm.h"
#include "lpm_config.h"

#include "laplace/laplace.h"

#include <math.h>
#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

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
} Doc;

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
};

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

static void post_features(Lpm *ix, uint32_t doc) {
    if (!ix->nfeat) return;
    qsort(ix->feat, ix->nfeat, 16, cmp16);
    const uint8_t *prev = NULL;
    for (size_t i = 0; i < ix->nfeat; i++) {
        const uint8_t *id = ix->feat + i * 16;
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

static lp_ref compose_put(Lpm *ix, const lp_ref *ch, uint32_t n, uint8_t tier) {
    lp_ref r = lp_ref_compose(ch, n, tier);
    int fresh = 0;
    Ent *e = ent_put(ix, &r, &fresh);
    ix->compositions++;
    ent_kids(e, ch, n);
    return r;
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
    snprintf(so, sizeof so, "%s/libtree-sitter-%s.so", dir && *dir ? dir : LPM_GRAMMARS_DEFAULT, name);
    if (access(so, R_OK) != 0) return NULL;
    void *h = dlopen(so, RTLD_NOW | RTLD_LOCAL);
    if (!h) return NULL;
    snprintf(sym, sizeof sym, "tree_sitter_%s", name);
    TSLanguage *(*fn)(void) = dlsym(h, sym);
    if (!fn) return NULL;
    if (nlang < 64) { snprintf(names[nlang], sizeof names[nlang], "%s", name); langs[nlang] = fn(); return langs[nlang++]; }
    return fn();
}

static const char *grammar_name(const char *path) {
    const char *base = path ? strrchr(path, '/') : NULL;
    base = base ? base + 1 : path;
    const char *dot = base ? strrchr(base, '.') : NULL;
    if (!dot) return NULL;
    if (!strcmp(dot, ".txt") || !strcmp(dot, ".md") || !strcmp(dot, ".rst")) return NULL;
    if (!strcmp(dot, ".tsv") || !strcmp(dot, ".csv")) return NULL;
    if (!strcmp(dot, ".c") || !strcmp(dot, ".h")) return "c";
    if (!strcmp(dot, ".cc") || !strcmp(dot, ".cpp") || !strcmp(dot, ".hpp")) return "cpp";
    if (!strcmp(dot, ".py")) return "python";
    if (!strcmp(dot, ".js")) return "javascript";
    if (!strcmp(dot, ".ts")) return "typescript";
    if (!strcmp(dot, ".tsx")) return "tsx";
    if (!strcmp(dot, ".xml")) return "xml";
    if (!strcmp(dot, ".json")) return "json";
    if (!strcmp(dot, ".sql")) return "sql";
    if (!strcmp(dot, ".html")) return "html";
    if (!strcmp(dot, ".sh")) return "bash";
    if (!strcmp(dot, ".yml") || !strcmp(dot, ".yaml")) return "yaml";
    return NULL;
}

/* The concrete syntax tree, composed the way the engine composes a grammar-only recipe:
 * each node is its children with the bytes between them kept as text; a leaf is text. */
static lp_ref ast_node(Lpm *ix, TSNode nd, const uint8_t *src, uint32_t lo, uint32_t hi, int depth) {
    uint32_t nc = ts_node_child_count(nd);
    if (nc == 0 || depth > 200) return text_cached(ix, src + lo, hi > lo ? hi - lo : 0);
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
    return r;
}

static lp_ref admit_grammar(Lpm *ix, const char *name, const uint8_t *s, size_t n) {
    const TSLanguage *lang = load_lang(name);
    lp_ref none; memset(&none, 0, sizeof none);
    if (!lang || n > 0xffffffffu) return none;
    TSParser *ps = ts_parser_new();
    ts_parser_set_language(ps, lang);
    TSTree *t = ts_parser_parse_string(ps, NULL, (const char *)s, (uint32_t)n);
    lp_ref trunk = ast_node(ix, ts_tree_root_node(t), s, 0, (uint32_t)n, 0);
    ts_tree_delete(t);
    ts_parser_delete(ps);
    return trunk;
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
    const char *base = path ? strrchr(path, '/') : NULL;
    base = base ? base + 1 : path;
    const char *dot = base ? strrchr(base, '.') : NULL;
    int utf8 = is_utf8(s, n);
    if (dot && (!strcmp(dot, ".tsv") || !strcmp(dot, ".csv")) && utf8) {
        trunk = admit_delimited(ix, s, n, dot[1] == 'c');
    } else {
        const char *g = utf8 ? grammar_name(path) : NULL;
        if (g && load_lang(g)) trunk = admit_grammar(ix, g, s, n);
        else if (utf8) trunk = text_cached(ix, s, n);
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
    doc->line1 = utf8 ? count_lines(s, n) : 0;
    doc->bytes = n > 0xffffffffu ? 0xffffffffu : (uint32_t)n;
    doc->path = add_path(ix, path ? path : "");
    post_features(ix, (uint32_t)ix->ndocs);
    ix->ndocs++;
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

static int index_file(Lpm *ix, const char *path) {
    struct stat st;
    if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) return 0;
    if (st.st_size <= 0 || st.st_size > (1 << 20)) { ix->skipped++; return 0; }
    FILE *f = fopen(path, "rb");
    if (!f) { ix->skipped++; return 0; }
    uint8_t *buf = malloc((size_t)st.st_size);
    if (!buf) { fclose(f); return -1; }
    size_t n = fread(buf, 1, (size_t)st.st_size, f);
    fclose(f);
    if (n != (size_t)st.st_size) { free(buf); ix->skipped++; return 0; }
    int rc = lpm_admit(ix, path, buf, n);
    free(buf);
    return rc;
}

static int index_dir(Lpm *ix, const char *path) {
    DIR *d = opendir(path);
    if (!d) return -1;
    struct dirent *de;
    while ((de = readdir(d))) {
        if (de->d_name[0] == '.' && (de->d_name[1] == 0 || (de->d_name[1] == '.' && de->d_name[2] == 0))) continue;
        char child[4096];
        int nw = snprintf(child, sizeof child, "%s/%s", path, de->d_name);
        if (nw < 0 || (size_t)nw >= sizeof child) continue;
        struct stat st;
        if (lstat(child, &st) != 0) continue;
        if (S_ISLNK(st.st_mode)) continue;
        if (S_ISDIR(st.st_mode)) {
            if (!skip_dir(de->d_name)) {
                if (index_dir(ix, child)) { closedir(d); return -1; }
            }
        } else if (S_ISREG(st.st_mode) && wanted(de->d_name)) {
            if (index_file(ix, child)) { closedir(d); return -1; }
        }
    }
    closedir(d);
    return 0;
}

int lpm_index_path(Lpm *ix, const char *path) {
    struct stat st;
    if (stat(path, &st) != 0) return -1;
    if (S_ISDIR(st.st_mode)) return index_dir(ix, path);
    return index_file(ix, path);
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
    if (utf8 && n) trunk = lp_text_decompose(ix->text, s, n, on_compose, ix);
    else memset(&trunk, 0, sizeof trunk);
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
    for (size_t f = 0; f < ix->nfeat; f++) {
        Ent *e = ent_find(ix, ix->feat + f * 16);
        if (!e || !e->nd) continue;
        double wgt = (double)ix->ndocs / (double)e->nd;
        for (uint32_t d = 0; d < e->nd; d++) {
            uint32_t di = e->docs[d];
            if (di >= ix->ndocs) continue;
            shared[di]++;
            rank[di] += wgt;
        }
    }
    size_t nout = cap < ix->ndocs ? cap : ix->ndocs;
    uint8_t *taken = calloc(ix->ndocs, 1);
    if (!taken) abort();
    size_t wrote = 0;
    for (size_t h = 0; h < nout; h++) {
        uint32_t best = 0;
        int any = 0;
        for (size_t d = 0; d < ix->ndocs; d++) {
            if (taken[d] || !shared[d]) continue;
            if (!any || rank[d] > rank[best] || (rank[d] == rank[best] && shared[d] > shared[best])) {
                best = (uint32_t)d;
                any = 1;
            }
        }
        if (!any) break;
        taken[best] = 1;
        Doc *doc = &ix->docs[best];
        LpmHit *hit = &out[wrote++];
        memset(hit, 0, sizeof *hit);
        memcpy(hit->id, doc->id, 16);
        memcpy(hit->m, doc->m, sizeof hit->m);
        hit->hilbert = doc->hilbert;
        hit->line0 = doc->line0;
        hit->line1 = doc->line1;
        hit->shared = shared[best];
        hit->bytes = doc->bytes;
        snprintf(hit->path, sizeof hit->path, "%s", ix->paths + doc->path);
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
            if (di >= ix->ndocs) continue;
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
        if (memcmp(ix->docs[di].id, id, 16)) continue;
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

static void traj_walk(const Lpm *ix, const uint8_t id[16], double **pts, size_t *n, size_t *cap, int depth) {
    Ent *e = ent_find(ix, id);
    if (!e || depth > 64) return;
    if (!e->nkids) {
        if (*n == *cap) {
            *cap = *cap ? *cap * 2 : 64;
            *pts = grow(*pts, *cap * 4 * sizeof(double));
        }
        double *p = *pts + *n * 4;
        for (int d = 0; d < 4; d++) p[d] = ldexp((double)e->m[d], -53);
        (*n)++;
        return;
    }
    for (uint32_t i = 0; i < e->nkids; i++) traj_walk(ix, e->kids + (size_t)i * 16, pts, n, cap, depth + 1);
}

double lpm_frechet(const Lpm *ix, const uint8_t a[16], const uint8_t b[16]) {
    if (!ent_find(ix, a) || !ent_find(ix, b)) return -1;
    double *pa = NULL, *pb = NULL;
    size_t na = 0, nb = 0, ca = 0, cb = 0;
    traj_walk(ix, a, &pa, &na, &ca, 0);
    traj_walk(ix, b, &pb, &nb, &cb, 0);
    double d = (na && nb) ? lp_frechet4(pa, na, pb, nb) : -1;
    free(pa);
    free(pb);
    return d;
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

static int cp_of(const Lpm *ix, const uint8_t id[16], const uint32_t *cps, uint32_t nc) {
    for (uint32_t i = 0; i < nc; i++)
        if (!memcmp(ix->t0[cps[i]].id.b, id, 16)) return (int)cps[i];
    return -1;
}

static void emit_node(const Lpm *ix, const uint8_t id[16], const uint32_t *cps, uint32_t nc, FILE *o) {
    Ent *e = ent_find(ix, id);
    if (e && e->nkids > 1) {
        fputc('[', o);
        for (uint32_t i = 0; i < e->nkids; i++) {
            if (i) fputc(',', o);
            emit_node(ix, e->kids + (size_t)i * 16, cps, nc, o);
        }
        fputc(']', o);
        return;
    }
    int cp = cp_of(ix, id, cps, nc);
    if (cp == ' ') { fputs("' '", o); return; }
    if (cp >= 32 && cp < 127) { fputc(cp, o); return; }
    if (cp > 0) { fprintf(o, "U+%04X", cp); return; }
    if (e && e->nkids == 1) { emit_node(ix, e->kids, cps, nc, o); return; }
    fputs("?", o);
}

void lpm_show_text(Lpm *ix, const uint8_t *s, size_t n, void *file) {
    FILE *o = file;
    static const char H[] = "0123456789abcdef";
    ix->collecting = 0;
    lp_ref trunk = lp_text_decompose(ix->text, s, n, on_compose, ix);
    uint32_t cps[256];
    uint32_t nc = 0;
    size_t i = 0;
    uint32_t cp;
    while (i < n && nc < 256 && lp_utf8_next(s, n, &i, &cp)) {
        int seen = 0;
        for (uint32_t k = 0; k < nc; k++) if (cps[k] == cp) seen = 1;
        if (!seen) cps[nc++] = cp;
    }
    char id[33];
    for (int b = 0; b < 16; b++) { id[b * 2] = H[trunk.id.b[b] >> 4]; id[b * 2 + 1] = H[trunk.id.b[b] & 15]; }
    id[32] = 0;
    fprintf(o, "id        %s\n", id);
    fprintf(o, "tier      %u\n", trunk.tier);
    fprintf(o, "hilbert   %llu\n", (unsigned long long)lp_hilbert4(&trunk.c));
    fprintf(o, "m         %lld %lld %lld %lld\n",
            (long long)trunk.c.m[0], (long long)trunk.c.m[1], (long long)trunk.c.m[2], (long long)trunk.c.m[3]);
    emit_node(ix, trunk.id.b, cps, nc, o);
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
    s->files = ix->files;
    s->skipped = ix->skipped;
    s->bytes = ix->bytes;
    s->entities = ix->nent;
    s->documents = ix->ndocs;
    s->compositions = ix->compositions;
    for (size_t i = 0; i < ix->nslot; i++) if (ix->slot[i].used && ix->slot[i].nd) s->words++;
}

static void wr(FILE *f, const void *p, size_t n) {
    if (fwrite(p, 1, n, f) != n) abort();
}

int lpm_save(const Lpm *ix, const char *path) {
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    wr(f, "LPM1", 4);
    uint32_t ver = 1;
    wr(f, &ver, 4);
    uint64_t nent = ix->nent, nd = ix->ndocs;
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
        const char *p = ix->paths + d->path;
        uint32_t pl = (uint32_t)strlen(p);
        wr(f, &pl, 4);
        wr(f, p, pl);
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
    fclose(f);
    return 0;
}

static void rd(FILE *f, void *p, size_t n) {
    if (fread(p, 1, n, f) != n) abort();
}

Lpm *lpm_load(const char *path, const char *tier0) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    char mag[4];
    rd(f, mag, 4);
    if (memcmp(mag, "LPM1", 4)) { fclose(f); return NULL; }
    uint32_t ver;
    rd(f, &ver, 4);
    if (ver != 1) { fclose(f); return NULL; }
    Lpm *ix = lpm_new(tier0);
    if (!ix) { fclose(f); return NULL; }
    uint64_t nent;
    rd(f, &nent, 8);
    for (uint64_t i = 0; i < nent; i++) {
        lp_ref r;
        memset(&r, 0, sizeof r);
        uint32_t seen;
        uint8_t tier, pad[3];
        rd(f, r.id.b, 16);
        rd(f, r.c.m, 32);
        uint64_t hilbert;
        rd(f, &hilbert, 8);
        rd(f, &seen, 4);
        rd(f, &tier, 1);
        rd(f, pad, 3);
        r.tier = tier;
        int fresh = 0;
        Ent *e = ent_put(ix, &r, &fresh);
        e->seen = seen;
        e->hilbert = hilbert;
    }
    uint64_t nd;
    rd(f, &nd, 8);
    for (uint64_t i = 0; i < nd; i++) {
        if (ix->ndocs == ix->cdocs) {
            ix->cdocs = ix->cdocs ? ix->cdocs * 2 : 256;
            ix->docs = grow(ix->docs, ix->cdocs * sizeof *ix->docs);
        }
        Doc *d = &ix->docs[ix->ndocs];
        memset(d, 0, sizeof *d);
        uint32_t pl;
        uint8_t tier;
        rd(f, d->id, 16);
        rd(f, d->m, 32);
        rd(f, &d->hilbert, 8);
        rd(f, &d->line0, 4);
        rd(f, &d->line1, 4);
        rd(f, &d->bytes, 4);
        rd(f, &tier, 1);
        rd(f, &pl, 4);
        char *p = malloc(pl + 1);
        if (!p) abort();
        rd(f, p, pl);
        p[pl] = 0;
        d->tier = tier;
        d->path = add_path(ix, p);
        free(p);
        ix->ndocs++;
        ix->files++;
        ix->bytes += d->bytes;
    }
    uint64_t np;
    rd(f, &np, 8);
    for (uint64_t i = 0; i < np; i++) {
        uint8_t id[16];
        uint32_t count;
        rd(f, id, 16);
        rd(f, &count, 4);
        Ent *e = ent_find(ix, id);
        if (!e) abort();
        e->cd = count;
        e->docs = malloc(count * sizeof *e->docs);
        if (!e->docs) abort();
        rd(f, e->docs, count * 4);
        e->nd = count;
    }
    fclose(f);
    return ix;
}
