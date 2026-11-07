#define _GNU_SOURCE
#include "lpm_internal.h"

/* Text with no grammar: decomposed along natural heading sections and paragraph boundaries.
 * Never cuts inside a paragraph or section at an arbitrary line count. */
static void admit_text_blocks(Lpm *ix, const uint8_t *s, size_t n, uint32_t path_off, uint32_t file_lines) {
    if (file_lines <= 4) return;
    size_t b0 = 0, i = 0;
    uint32_t line = 1, l0 = 1, nonblank = 0;
    int in_fence = 0;
    while (i < n) {
        size_t ls = i;
        while (i < n && s[i] != '\n') i++;
        size_t le = i;
        if (i < n) i++;
        int blank = 1;
        size_t first = ls;
        for (size_t k = ls; k < le; k++) if (s[k] != ' ' && s[k] != '\t' && s[k] != '\r') { blank = 0; first = k; break; }
        if (!blank && le - first >= 3 && s[first] == '`' && s[first + 1] == '`' && s[first + 2] == '`') in_fence = !in_fence;
        int heading = !in_fence && !blank && s[ls] == '#';
        if (heading && nonblank >= 1 && ls > b0) {
            size_t f0 = ix->nfeat;
            lp_ref r = lpm_text_cached(ix, s + b0, ls - b0);
            if (r.tier && !(l0 == 1 && line - 1 >= file_lines))
                lpm_push_doc(ix, &r, l0, line - 1, (uint32_t)(ls - b0), path_off, f0);
            b0 = ls; l0 = line; nonblank = 0;
        }
        if (!blank) nonblank++;
        if ((!in_fence && blank && nonblank >= 2) || i >= n) {
            if (nonblank >= 1 && i > b0 && !(l0 == 1 && line >= file_lines)) {
                size_t f0 = ix->nfeat;
                lp_ref r = lpm_text_cached(ix, s + b0, i - b0);
                if (r.tier) lpm_push_doc(ix, &r, l0, line, (uint32_t)(i - b0), path_off, f0);
            }
            b0 = i; l0 = line + 1; nonblank = 0;
        }
        line++;
    }
}

int lpm_admit(Lpm *ix, const char *path, const uint8_t *s, size_t n) {
    if (!n) return 0;
    lp_ref trunk;
    memset(&trunk, 0, sizeof trunk);
    ix->nfeat = 0;
    ix->collecting = 1;
    const char *base = lpm_path_basename(path);
    const char *dot = base ? strrchr(base, '.') : NULL;
    int utf8 = lpm_is_utf8(s, n);
    uint32_t path_off = lpm_add_path(ix, path ? path : "");
    /* The same path admitted again replaces what it held. */
    long was = lpm_file_find(ix, ix->paths + path_off);
    if (was >= 0) lpm_file_kill(ix, (uint32_t)was);
    if (ix->have_meta) ix->cur_file = lpm_file_add(ix, path_off, ix->meta_size, ix->meta_mtime, ix->meta_hash);
    else ix->cur_file = lpm_file_add(ix, path_off, n, 0, lpm_fnv(s, n));
    ix->have_meta = 0;
    uint32_t file_lines = utf8 ? lpm_count_lines(s, n) : 0;
    int had_grammar = 0;
    if (dot && (!strcmp(dot, ".tsv") || !strcmp(dot, ".csv")) && utf8) {
        trunk = lpm_admit_delimited(ix, s, n, dot[1] == 'c');
    } else {
        const char *g = utf8 ? lpm_grammar_name(path) : NULL;
        if (g && lpm_load_lang(g)) {
            had_grammar = 1;
            trunk = lpm_admit_grammar(ix, g, s, n, path_off, file_lines);
        } else if (utf8) {
            admit_text_blocks(ix, s, n, path_off, file_lines);
            trunk = lpm_text_cached(ix, s, n);
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
            lpm_ent_put(ix, &trunk, &fresh);
            ix->compositions++;
            free(ids);
        }
    }
    /* Attach the file's path subtokens (last 3 segments) and, for .c files, its sibling .h opening header comment. */
    size_t pf0 = ix->nfeat;
    const char *rel = lpm_last_path_segments(ix->paths + path_off, 3);
    if (rel && *rel) lpm_collect_subtoken_feats(ix, (const uint8_t *)rel, strlen(rel));
    if (had_grammar && utf8 && dot && !strcmp(dot, ".c") && path && strlen(path) < 4090) {
        char hpath[4096];
        snprintf(hpath, sizeof hpath, "%s", path);
        char *hdot = strrchr(hpath, '.');
        if (hdot) {
            hdot[1] = 'h'; hdot[2] = 0;
            FILE *hf = fopen(hpath, "rb");
            if (hf) {
                uint8_t hbuf[512];
                size_t hn = fread(hbuf, 1, sizeof hbuf, hf);
                fclose(hf);
                size_t hc = lpm_header_comment_bytes(hbuf, hn);
                if (hc >= 8 && lpm_is_utf8(hbuf, hc)) lpm_text_cached(ix, hbuf, hc);
            }
        }
    }
    size_t pf1 = ix->nfeat;
    if (pf1 > pf0) {
        uint32_t doc0 = ix->fl[ix->cur_file].doc0;
        for (uint32_t di = doc0; di < (uint32_t)ix->ndocs; di++) {
            lpm_post_features_range(ix, di, pf0, pf1);
        }
    }

    lpm_add_feat(ix, trunk.id.b);
    if (ix->ndocs == ix->cdocs) {
        ix->cdocs = ix->cdocs ? ix->cdocs * 2 : 256;
        ix->docs = lpm_grow(ix->docs, ix->cdocs * sizeof *ix->docs);
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
    lpm_post_features(ix, (uint32_t)ix->ndocs);
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
    ix->meta_hash = lpm_fnv(buf, n);
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
    lpm_norm_path(key, sizeof key, path);
    struct statx x;
    memset(&x, 0, sizeof x);
    if (statx(AT_FDCWD, path, 0, STATX_SIZE | STATX_MTIME, &x) != 0) return 0;
    int64_t mt = mtime_ns(&x);
    long f = lpm_file_find(ix, key);
    if (f >= 0 && ix->fl[f].size == x.stx_size && ix->fl[f].mtime == mt) {
        ix->fl[f].seen = 1;
        u->unchanged++;
        return 0;
    }
    uint8_t *buf;
    size_t n;
    int got = read_small(ix, path, &buf, &n);
    if (got <= 0) return got;
    if (f >= 0 && ix->fl[f].size == n && ix->fl[f].hash == lpm_fnv(buf, n)) {
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
    lpm_norm_path(root, sizeof root, path);
    size_t rl = strlen(root);
    for (size_t i = 0; i < ix->nfl; i++)
        if (!ix->fl[i].dead && under(ix->paths + ix->fl[i].path, root, rl)) ix->fl[i].seen = 0;
    int rc = 0;
    if (os_is_dir(path)) rc = walk_dir(ix, path, u);
    else if (os_exists(path) && wanted(lpm_path_basename(path))) rc = update_file(ix, path, u);
    if (rc) return rc;
    /* What was under the root and was not met again is gone. */
    for (size_t i = 0; i < ix->nfl; i++) {
        LFile *f = &ix->fl[i];
        if (!f->dead && !f->seen && under(ix->paths + f->path, root, rl)) {
            lpm_file_kill(ix, (uint32_t)i);
            u->removed++;
        }
    }
    return 0;
}
