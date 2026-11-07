#define _GNU_SOURCE
#include "lpm_internal.h"
#include <tree_sitter/api.h>

static uint32_t count_lines(const uint8_t *s, size_t n) {
    uint32_t lines = 1;
    for (size_t i = 0; i < n; i++) if (s[i] == '\n') lines++;
    return lines;
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
    uint64_t h = lpm_fnv(s, n);
    size_t k = h & (ix->sc_cap - 1);
    while (ix->sc[k].used) {
        SEnt *e = &ix->sc[k];
        if (e->h == h && e->len == n && !memcmp(ix->sc_pool + e->off, s, n)) {
            for (uint32_t i = 0; i < e->feat_n; i++) lpm_add_feat(ix, ix->sc_feat + (size_t)(e->feat_at + i) * 16);
            return e->ref;
        }
        k = (k + 1) & (ix->sc_cap - 1);
    }
    size_t feat0 = ix->nfeat;
    lp_ref r = lp_text_decompose(ix->text, s, n, lpm_on_compose, ix);
    lpm_collect_subtoken_feats(ix, s, n);
    if (ix->sc_pn + n > ix->sc_pcap) {
        ix->sc_pcap = (ix->sc_pn + n) * 2 + 4096;
        ix->sc_pool = lpm_grow(ix->sc_pool, ix->sc_pcap);
    }
    memcpy(ix->sc_pool + ix->sc_pn, s, n);
    uint32_t fn = (uint32_t)(ix->nfeat - feat0);
    if (ix->sc_fn + fn > ix->sc_fcap) {
        ix->sc_fcap = ix->sc_fcap ? ix->sc_fcap * 2 : 1024;
        if (ix->sc_fcap < ix->sc_fn + fn) ix->sc_fcap = ix->sc_fn + fn;
        ix->sc_feat = lpm_grow(ix->sc_feat, ix->sc_fcap * 16);
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
            if (nf == cf) { cf = cf ? cf * 2 : 8; fields = lpm_grow(fields, cf * sizeof *fields); }
            fields[nf++] = text_cached(ix, fs, fl);
            f = p + 1;
        }
        if (nf) {
            uint8_t tier = 0;
            for (size_t k = 0; k < nf; k++) if (fields[k].tier > tier) tier = fields[k].tier;
            lp_ref row = lpm_compose_put(ix, fields, (uint32_t)nf, (uint8_t)(tier + 1));
            if (nr == cr) { cr = cr ? cr * 2 : 256; rows = lpm_grow(rows, cr * sizeof *rows); }
            rows[nr++] = row;
        }
        free(fields);
    }
    lp_ref trunk;
    memset(&trunk, 0, sizeof trunk);
    if (nr) {
        uint8_t tier = 0;
        for (size_t k = 0; k < nr; k++) if (rows[k].tier > tier) tier = rows[k].tier;
        trunk = lpm_compose_put(ix, rows, (uint32_t)nr, (uint8_t)(tier + 1));
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

static const char *last_path_segments(const char *path, int nseg) {
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

static size_t header_comment_bytes(const uint8_t *s, size_t n) {
    size_t max_b = n < 420 ? n : 420;
    for (size_t i = 0; i + 1 < max_b; i++) {
        if (s[i] == '#' && (s[i + 1] == 'i' || s[i + 1] == 'p' || s[i + 1] == 'd')) return i;
        if (s[i] == '*' && s[i + 1] == '/') return i + 2;
    }
    return max_b;
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

/* A document: the entity r, lines l0..l1 of the file at path_off, holding the features in [f0, f1). */
static void push_doc_range(Lpm *ix, const lp_ref *r, uint32_t l0, uint32_t l1, uint32_t bytes, uint32_t path_off, size_t f0, size_t f1) {
    lpm_add_feat(ix, r->id.b);
    if (f1 < ix->nfeat) f1 = ix->nfeat;
    if (ix->ndocs == ix->cdocs) {
        ix->cdocs = ix->cdocs ? ix->cdocs * 2 : 256;
        ix->docs = lpm_grow(ix->docs, ix->cdocs * sizeof *ix->docs);
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
    lpm_post_features_range(ix, (uint32_t)ix->ndocs, f0, f1);
    ix->ndocs++;
}

static void push_doc(Lpm *ix, const lp_ref *r, uint32_t l0, uint32_t l1, uint32_t bytes, uint32_t path_off, size_t f0) {
    push_doc_range(ix, r, l0, l1, bytes, path_off, f0, ix->nfeat);
}

static int is_ast_container(TSNode nd, int depth) {
    if (depth == 0) return 1;
    const char *tp = ts_node_type(nd);
    if (!tp) return 0;
    if (strstr(tp, "namespace") != NULL ||
        strstr(tp, "translation_unit") != NULL ||
        strstr(tp, "compilation_unit") != NULL ||
        !strcmp(tp, "program") ||
        !strcmp(tp, "module") ||
        !strcmp(tp, "source_file")) return 1;
    uint32_t l0 = ts_node_start_point(nd).row + 1;
    uint32_t l1 = ts_node_end_point(nd).row + 1;
    if (l1 >= l0 && (l1 - l0 + 1) <= 14) return 0;
    return strstr(tp, "class_decl") != NULL ||
           strstr(tp, "class_spec") != NULL ||
           strstr(tp, "record_decl") != NULL ||
           strstr(tp, "struct_decl") != NULL ||
           strstr(tp, "struct_spec") != NULL ||
           strstr(tp, "interface_decl") != NULL ||
           strstr(tp, "impl_item") != NULL ||
           strstr(tp, "mod_item") != NULL ||
           strstr(tp, "declaration_list") != NULL ||
           strstr(tp, "field_declaration_list") != NULL;
}

static int is_comment_or_prefix(TSNode nd, uint32_t span, int *is_meaningful) {
    const char *tp = ts_node_type(nd);
    if (!tp) { *is_meaningful = 0; return 0; }
    if (strstr(tp, "comment") != NULL) { *is_meaningful = 1; return 1; }
    if (strstr(tp, "attribute") != NULL || strstr(tp, "decorator") != NULL) { *is_meaningful = 0; return 1; }
    if (strstr(tp, "preproc_include") != NULL || strstr(tp, "using_directive") != NULL) { *is_meaningful = 0; return 1; }
    if (span == 1 && (strstr(tp, "type_definition") != NULL || strstr(tp, "declaration") != NULL)) {
        *is_meaningful = 1;
        return 1;
    }
    *is_meaningful = 1;
    return 0;
}

/* The concrete syntax tree, composed the way the engine composes a grammar-only recipe:
 * each node is its children with the bytes between them kept as text; a leaf is text.
 * Inside a container node (file root, namespace, class, record, struct, interface, impl, declaration_list),
 * contiguous non-container siblings with no blank line between them (e.g. a leading doc-comment block,
 * attributes, 1-line helper typedefs, and the declaration they belong to) are grouped into a single
 * searchable declaration span. */
static lp_ref ast_node(Lpm *ix, TSNode nd, const uint8_t *src, uint32_t lo, uint32_t hi, int depth) {
    uint32_t nc = ts_node_child_count(nd);
    if (nc == 0 || depth > 200) return text_cached(ix, src + lo, hi > lo ? hi - lo : 0);
    int outer = ix->chunk_on;
    int container = outer && is_ast_container(nd, depth);
    uint32_t l0 = ts_node_start_point(nd).row + 1;
    uint32_t l1 = ts_node_end_point(nd).row + 1;
    uint32_t span = l1 >= l0 ? l1 - l0 + 1 : 1;
    size_t f0 = ix->nfeat;

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

    int grp_active = 0, grp_only_prefix = 0, grp_meaningful = 0;
    size_t grp_f0 = 0, grp_f1 = 0;
    uint32_t grp_l0 = 0, grp_l1 = 0, grp_b0 = 0, grp_b1 = 0;
    lp_ref grp_ref;
    memset(&grp_ref, 0, sizeof grp_ref);

    uint32_t at = lo;
    for (uint32_t i = 0; i < got; i++) {
        if (cs[i] > at) {
            kids[2 * i] = text_cached(ix, src + at, cs[i] - at);
            has[2 * i] = 1;
        }
        if (ce[i] > at) at = ce[i];
        if (ce[i] <= cs[i]) continue;

        if (!container || !ts_node_is_named(cn[i])) {
            ix->chunk_on = 0;
            kids[2 * i + 1] = ast_node(ix, cn[i], src, cs[i], ce[i], depth + 1);
            has[2 * i + 1] = 1;
            continue;
        }

        uint32_t cl0 = ts_node_start_point(cn[i]).row + 1;
        uint32_t cl1 = ts_node_end_point(cn[i]).row + 1;
        uint32_t cspan = cl1 >= cl0 ? cl1 - cl0 + 1 : 1;

        if (is_ast_container(cn[i], depth + 1)) {
            if (grp_active) {
                if (grp_meaningful && grp_l1 >= grp_l0 && (grp_l1 - grp_l0 + 1 >= 4) &&
                    !(grp_l0 == 1 && grp_l1 >= ix->chunk_lines)) {
                    push_doc_range(ix, &grp_ref, grp_l0, grp_l1, grp_b1 - grp_b0, ix->chunk_path, grp_f0, grp_f1);
                }
                grp_active = 0;
            }
            ix->chunk_on = 1;
            kids[2 * i + 1] = ast_node(ix, cn[i], src, cs[i], ce[i], depth + 1);
            has[2 * i + 1] = 1;
            continue;
        }

        int can_join = grp_active && (cl0 <= grp_l1 + 1) && (grp_only_prefix || (cl1 - grp_l0 + 1 <= 80));
        if (grp_active && !can_join) {
            if (grp_meaningful && grp_l1 >= grp_l0 && (grp_l1 - grp_l0 + 1 >= 2) &&
                !(grp_l0 == 1 && grp_l1 >= ix->chunk_lines)) {
                push_doc_range(ix, &grp_ref, grp_l0, grp_l1, grp_b1 - grp_b0, ix->chunk_path, grp_f0, grp_f1);
            }
            grp_active = 0;
        }

        size_t f_before = ix->nfeat;
        ix->chunk_on = 0;
        kids[2 * i + 1] = ast_node(ix, cn[i], src, cs[i], ce[i], depth + 1);
        has[2 * i + 1] = 1;

        int meaningful = 0;
        int is_pref = is_comment_or_prefix(cn[i], cspan, &meaningful);
        if (!grp_active) {
            grp_active = 1;
            grp_only_prefix = is_pref;
            grp_meaningful = meaningful;
            grp_f0 = f_before;
            grp_f1 = ix->nfeat;
            grp_l0 = cl0;
            grp_l1 = cl1;
            grp_b0 = cs[i];
            grp_b1 = ce[i];
            grp_ref = kids[2 * i + 1];
        } else {
            if (!is_pref) {
                grp_only_prefix = 0;
                grp_ref = kids[2 * i + 1];
            }
            if (meaningful) grp_meaningful = 1;
            grp_f1 = ix->nfeat;
            grp_l1 = cl1;
            grp_b1 = ce[i];
        }
    }

    if (grp_active) {
        if (grp_meaningful && grp_l1 >= grp_l0 && (grp_l1 - grp_l0 + 1 >= 2) &&
            !(grp_l0 == 1 && grp_l1 >= ix->chunk_lines)) {
            push_doc_range(ix, &grp_ref, grp_l0, grp_l1, grp_b1 - grp_b0, ix->chunk_path, grp_f0, grp_f1);
        }
        grp_active = 0;
    }

    if (hi > at) {
        kids[2 * got] = text_cached(ix, src + at, hi - at);
        has[2 * got] = 1;
    }
    uint32_t k = 0;
    uint8_t tier = 0;
    for (uint32_t i = 0; i <= 2 * got; i++) if (has[i]) {
        kids[k] = kids[i];
        if (kids[k].tier > tier) tier = kids[k].tier;
        k++;
    }
    lp_ref r = k ? lpm_compose_put(ix, kids, k, (uint8_t)(tier + 1)) : text_cached(ix, src + lo, hi > lo ? hi - lo : 0);
    free(kids); free(has); free(cs); free(cn);
    ix->chunk_on = outer;
    if (container && depth > 0 && span >= 2 && span <= 60 && !(l0 == 1 && l1 >= ix->chunk_lines)) {
        push_doc_range(ix, &r, l0, l1, hi - lo, ix->chunk_path, f0, ix->nfeat);
    }
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
    ix->chunk_path = path_off;
    ix->chunk_lines = file_lines;
    ix->chunk_on = 1;
    lp_ref trunk = ast_node(ix, root, s, 0, (uint32_t)n, 0);
    ix->chunk_on = 0;
    ts_tree_delete(t);
    ts_parser_delete(ps);
    return trunk;
}

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
            lp_ref r = text_cached(ix, s + b0, ls - b0);
            if (r.tier && !(l0 == 1 && line - 1 >= file_lines))
                push_doc(ix, &r, l0, line - 1, (uint32_t)(ls - b0), path_off, f0);
            b0 = ls; l0 = line; nonblank = 0;
        }
        if (!blank) nonblank++;
        if ((!in_fence && blank && nonblank >= 2) || i >= n) {
            if (nonblank >= 1 && i > b0 && !(l0 == 1 && line >= file_lines)) {
                size_t f0 = ix->nfeat;
                lp_ref r = text_cached(ix, s + b0, i - b0);
                if (r.tier) push_doc(ix, &r, l0, line, (uint32_t)(i - b0), path_off, f0);
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
    const char *base = path_basename(path);
    const char *dot = base ? strrchr(base, '.') : NULL;
    int utf8 = lpm_is_utf8(s, n);
    uint32_t path_off = lpm_add_path(ix, path ? path : "");
    /* The same path admitted again replaces what it held. */
    long was = lpm_file_find(ix, ix->paths + path_off);
    if (was >= 0) lpm_file_kill(ix, (uint32_t)was);
    if (ix->have_meta) ix->cur_file = lpm_file_add(ix, path_off, ix->meta_size, ix->meta_mtime, ix->meta_hash);
    else ix->cur_file = lpm_file_add(ix, path_off, n, 0, lpm_fnv(s, n));
    ix->have_meta = 0;
    uint32_t file_lines = utf8 ? count_lines(s, n) : 0;
    int had_grammar = 0;
    if (dot && (!strcmp(dot, ".tsv") || !strcmp(dot, ".csv")) && utf8) {
        trunk = admit_delimited(ix, s, n, dot[1] == 'c');
    } else {
        const char *g = utf8 ? grammar_name(path) : NULL;
        if (g && load_lang(g)) {
            had_grammar = 1;
            trunk = admit_grammar(ix, g, s, n, path_off, file_lines);
        } else if (utf8) {
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
            lpm_ent_put(ix, &trunk, &fresh);
            ix->compositions++;
            free(ids);
        }
    }
    /* Attach the file's path subtokens (last 3 segments) and, for .c files, its sibling .h opening header comment. */
    size_t pf0 = ix->nfeat;
    const char *rel = last_path_segments(ix->paths + path_off, 3);
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
                size_t hc = header_comment_bytes(hbuf, hn);
                if (hc >= 8 && lpm_is_utf8(hbuf, hc)) text_cached(ix, hbuf, hc);
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
    else if (os_exists(path) && wanted(path_basename(path))) rc = update_file(ix, path, u);
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
