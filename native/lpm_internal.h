#ifndef LPM_INTERNAL_H
#define LPM_INTERNAL_H

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

typedef struct LFile {
    uint32_t path;
    uint32_t doc0, ndoc;
    uint64_t size, hash;
    int64_t mtime;      /* nanoseconds */
    uint8_t dead, seen;
} LFile;

typedef struct SEnt {
    uint64_t h;
    lp_ref ref;
    uint32_t off, len, feat_at, feat_n;
    uint8_t used;
} SEnt;

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
    int chunk_on;
    uint32_t chunk_path, chunk_lines;
    LFile *fl;
    size_t nfl, cfl;
    uint32_t *fmap;
    size_t nfmap;
    size_t dead_docs;
    size_t live_n, live_at_ndocs, live_at_dead;
    double live_avg;
    uint32_t cur_file;
    int have_meta;
    uint64_t meta_size, meta_hash;
    int64_t meta_mtime;
};

static inline void *lpm_grow(void *p, size_t n) {
    p = realloc(p, n ? n : 1);
    if (!p) abort();
    return p;
}

static inline uint64_t lpm_fnv(const uint8_t *s, size_t n) {
    uint64_t h = 1469598103934665603ull;
    for (size_t i = 0; i < n; i++) { h ^= s[i]; h *= 1099511628211ull; }
    return h ? h : 1;
}

static inline uint64_t lpm_str_hash(const char *s) { return lpm_fnv((const uint8_t *)s, strlen(s)); }
static inline int lpm_cmp16(const void *a, const void *b) { return memcmp(a, b, 16); }
static inline int lpm_is_ascii_alnum(uint8_t c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
}

void lpm_rehash(Lpm *ix, size_t nslot);
Ent *lpm_ent_put(Lpm *ix, const lp_ref *r, int *fresh);
Ent *lpm_ent_find(const Lpm *ix, const uint8_t id[16]);
void lpm_add_feat(Lpm *ix, const uint8_t id[16]);
void lpm_ent_kids(Ent *e, const lp_ref *ch, uint32_t n);
lp_ref lpm_on_compose(void *ud, const lp_ref *ch, uint32_t n, uint8_t tier);
lp_ref lpm_compose_put(Lpm *ix, const lp_ref *ch, uint32_t n, uint8_t tier);
void lpm_post_features_range(Lpm *ix, uint32_t doc, size_t f0, size_t f1);
void lpm_post_features(Lpm *ix, uint32_t doc);
uint32_t lpm_add_path(Lpm *ix, const char *path);
void lpm_fmap_rehash(Lpm *ix, size_t cap);
long lpm_file_find(const Lpm *ix, const char *p);
size_t lpm_live_stats(Lpm *ix, double *avg);
void lpm_file_kill(Lpm *ix, uint32_t i);
uint32_t lpm_file_add(Lpm *ix, uint32_t path_off, uint64_t size, int64_t mtime, uint64_t hash);
void lpm_norm_path(char *out, size_t cap, const char *in);
int lpm_is_utf8(const uint8_t *s, size_t n);
size_t lpm_stem_ascii(const uint8_t *w, size_t n, uint8_t out[64]);
int lpm_is_prose_or_data_path(const char *path);
int lpm_is_test_path(const char *path);
lp_ref lpm_lower_token_ref(Lpm *ix, const uint8_t *w, size_t n, int register_ent);
void lpm_collect_subtoken_feats(Lpm *ix, const uint8_t *s, size_t n);

#endif
