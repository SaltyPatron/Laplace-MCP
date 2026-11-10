#define _GNU_SOURCE
#include "lpm_internal.h"
#include <tree_sitter/api.h>

uint32_t lpm_count_lines(const uint8_t *s, size_t n) {
    uint32_t lines = 1;
    for (size_t i = 0; i < n; i++) if (s[i] == '\n') lines++;
    return lines;
}

/* A span of text, decomposed once. The same bytes are the same entity on the next call. */
lp_ref lpm_text_cached(Lpm *ix, const uint8_t *s, size_t n) {
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
lp_ref lpm_admit_delimited(Lpm *ix, const uint8_t *s, size_t n, int comma) {
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
            fields[nf++] = lpm_text_cached(ix, fs, fl);
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

const void *lpm_load_lang(const char *name) {
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

const char *lpm_path_basename(const char *path) {
    if (!path) return "";
    const char *s1 = strrchr(path, '/');
    const char *s2 = strrchr(path, '\\');
    const char *b = s1 > s2 ? s1 : s2;
    return b ? b + 1 : path;
}

const char *lpm_last_path_segments(const char *path, int nseg) {
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

size_t lpm_header_comment_bytes(const uint8_t *s, size_t n) {
    size_t max_b = n < 420 ? n : 420;
    for (size_t i = 0; i + 1 < max_b; i++) {
        if (s[i] == '#' && (s[i + 1] == 'i' || s[i + 1] == 'p' || s[i + 1] == 'd')) return i;
        if (s[i] == '*' && s[i + 1] == '/') return i + 2;
    }
    return max_b;
}

const char *lpm_grammar_name(const char *path) {
    const char *base = lpm_path_basename(path);
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

void lpm_push_doc(Lpm *ix, const lp_ref *r, uint32_t l0, uint32_t l1, uint32_t bytes, uint32_t path_off, size_t f0) {
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
    if (nc == 0 || depth > 200) return lpm_text_cached(ix, src + lo, hi > lo ? hi - lo : 0);
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
            kids[2 * i] = lpm_text_cached(ix, src + at, cs[i] - at);
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
        kids[2 * got] = lpm_text_cached(ix, src + at, hi - at);
        has[2 * got] = 1;
    }
    uint32_t k = 0;
    uint8_t tier = 0;
    for (uint32_t i = 0; i <= 2 * got; i++) if (has[i]) {
        kids[k] = kids[i];
        if (kids[k].tier > tier) tier = kids[k].tier;
        k++;
    }
    lp_ref r = k ? lpm_compose_put(ix, kids, k, (uint8_t)(tier + 1)) : lpm_text_cached(ix, src + lo, hi > lo ? hi - lo : 0);
    free(kids); free(has); free(cs); free(cn);
    ix->chunk_on = outer;
    if (container && depth > 0 && span >= 2 && span <= 60 && !(l0 == 1 && l1 >= ix->chunk_lines)) {
        push_doc_range(ix, &r, l0, l1, hi - lo, ix->chunk_path, f0, ix->nfeat);
    }
    return r;
}

lp_ref lpm_admit_grammar(Lpm *ix, const char *name, const uint8_t *s, size_t n, uint32_t path_off, uint32_t file_lines) {
    const TSLanguage *lang = (const TSLanguage *)lpm_load_lang(name);
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
