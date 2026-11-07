#define _GNU_SOURCE
#include "api_internal.h"

/* ---- code_callers and code_callees (Laplace-MCP#27), read off the same containment code_where walks. A place the
 * name stands (a path up from it that is not a claim) is a chain of nodes below its file, each where it stands in the
 * file's recorded text. A node that starts on a declaration's line of the file's outline and holds that declaration's
 * name in its first line, before the place, is that declaration's node: the innermost one is what holds the place. */
static const char *cc_word(const char *base, const char *from, const char *end, const char *name) {
    size_t nl = strlen(name);
    for (const char *p = from; nl && p + nl <= end; p++)
        if (!memcmp(p, name, nl) && (p == base || !fo_word(p[-1])) && !fo_word(p[nl])) return p;
    return NULL;
}
PGconn *api_cc_db(int c) {
    PGconn *pg = api_db();
    if (!pg) {
        char e[512], b[640];
        api_json_esc(PQerrorMessage(PG), e, sizeof e);
        snprintf(b, sizeof b, "{\"error\":\"database\",\"detail\":\"%s\"}", e);
        api_http(c, 503, b);
    }
    return pg;
}
typedef struct { int n; long l[16]; } CcLines;
static void cc_line(CcLines *cl, long line) {
    for (int z = 0; z < cl->n; z++) if (cl->l[z] == line) return;
    if (cl->n < 16) cl->l[cl->n++] = line;
}
static void cc_lines_json(char **out, size_t *on, size_t *ocap, const CcLines *cl) {
    api_buf_fmt(out, on, ocap, "\"at\":[");
    for (int z = 0; z < cl->n; z++) api_buf_fmt(out, on, ocap, "%s%ld", z ? "," : "", cl->l[z]);
    api_buf_fmt(out, on, ocap, "]");
}

/* code_callers: the declarations whose code uses the name, each with the lines it does at; a use outside every
 * declaration is its file's top level. A declaration's own name (its head) is not a use of it. */
void on_code_callers(int c, const char *body) {
    char name[2048] = "", under[512] = "";
    api_jstr(body, "query", name, sizeof name);         /* "query", as code_where's: an MCP call's body names its tool "name" */
    if (!name[0]) { api_http(c, 400, "{\"error\":\"query\"}"); return; }
    api_jstr(body, "path", under, sizeof under);
    int limit = 50;
    api_jint(body, "limit", &limit);
    if (limit < 1) limit = 1;
    if (limit > 500) limit = 500;
    lp_ref trunk, parts[256];
    size_t np = 0;
    if (!api_name_entity(name, &trunk, parts, 256, &np)) { api_http(c, 500, "{\"error\":\"tier0\"}"); return; }
    PGconn *pg = api_cc_db(c);
    if (!pg) return;
    uint64_t t0 = api_nsec();
    CwDag d = { 0 };
    int levels = 0, cut = 0;
    cw_walk(pg,&trunk.id, &d, 400000, &levels, &cut);
    Reader *rd = reader_new(pg);
    char *out = NULL, *top = NULL;
    size_t on = 0, ocap = 0, tn = 0, tcap = 0, files = 0;
    api_buf_fmt(&out, &on, &ocap, "{\"name\":\"");
    api_cw_str(&out, &on, &ocap, name);
    api_buf_fmt(&out, &on, &ocap, "\",\"database\":\"%s\",\"callers\":[", PQdb(pg));
    int shown = 0, ntop = 0;
    for (size_t i = 0; i < d.nn && shown < limit; i++) {
        if (!d.n[i].file) continue;
        char *path = api_cw_file_name(rd, &d.n[i].id);
        if (under[0] && !strstr(path, under)) { free(path); continue; }
        FoDecl *dl = NULL;
        char *text = NULL;
        size_t tl = 0, nd = fo_outline(pg, rd, &d.n[i].id, &dl, &text, &tl);
        CcLines *by = calloc(nd + 1, sizeof *by), alone = {0};
        const char *nm = text;
        while ((nm = cc_word(text, nm, text + tl, name))) {
            size_t position = (size_t)(nm - text);
            long q = fo_holder(dl, nd, position);
            if (q < 0 || dl[q].name_at != position)
                cc_line(q >= 0 ? &by[q] : &alone, (long)cw_lines(text, position));
            nm += strlen(name);
        }
        char fh[33];
        api_hex_id(&d.n[i].id, fh);
        int any = 0;
        for (size_t q = 0; q < nd && shown < limit; q++) {
            if (!by[q].n) continue;
            api_buf_fmt(&out, &on, &ocap, "%s{\"name\":\"", shown ? "," : "");
            api_cw_str(&out, &on, &ocap, dl[q].name);
            api_buf_fmt(&out, &on, &ocap, "\",\"kind\":\"");
            api_cw_str(&out, &on, &ocap, dl[q].kind);
            api_buf_fmt(&out, &on, &ocap, "\",\"path\":\"");
            api_cw_str(&out, &on, &ocap, path);
            api_buf_fmt(&out, &on, &ocap, "\",\"file\":\"%s\",\"line\":%zu,", fh, dl[q].line);
            cc_lines_json(&out, &on, &ocap, &by[q]);
            api_buf_fmt(&out, &on, &ocap, "}");
            shown++;
            any = 1;
        }
        if (alone.n) {
            api_buf_fmt(&top, &tn, &tcap, "%s{\"path\":\"", ntop ? "," : "");
            api_cw_str(&top, &tn, &tcap, path);
            api_buf_fmt(&top, &tn, &tcap, "\",\"file\":\"%s\",", fh);
            cc_lines_json(&top, &tn, &tcap, &alone);
            api_buf_fmt(&top, &tn, &tcap, "}");
            ntop++;
        }
        if (any || alone.n) files++;
        free(by);
        fo_free(dl, nd);
        free(text);
        free(path);
    }
    api_buf_fmt(&out, &on, &ocap, "],\"top_level\":[");
    if (top) api_buf_add(&out, &on, &ocap, top, tn);
    api_buf_fmt(&out, &on, &ocap, "],\"files\":%zu,\"shown\":%d,\"cut\":%s,\"ms\":%.3f}", files, shown, cut ? "true" : "false", (double)(api_nsec() - t0) / 1e6);
    free(top);
    reader_free(rd);
    lp_idmap_free(d.m);
    free(d.n);
    api_http(c, 200, out ? out : "{\"error\":\"memory\"}");
    free(out);
}

/* code_callees: what a declaration calls. Its code is its node (the outermost node starting on the line of a
 * declaration of the name in a file's outline that holds the name there). A callee is a name in that code followed by
 * "(" that a code recipe declared in a file the database holds: a typed declaration claim said by that file's record of
 * claims, or as its one record; each with its kind and up to 4 of those files. A name called that nothing declares
 * (a library's, a language's) is listed apart, in outside. */
typedef struct { char *name, *kind; lp_id file[4]; int nfile, total; } CeCand;
static void ce_file(CeCand *x, const char *kind, const lp_id *file) {
    if (!x->kind) x->kind = strdup(kind);
    for (int k = 0; k < x->nfile; k++) if (!memcmp(x->file[k].b, file->b, 16)) return;
    x->total++;
    if (x->nfile < 4) x->file[x->nfile++] = *file;
}
void on_code_callees(int c, const char *body) {
    char name[2048] = "", under[512] = "";
    api_jstr(body, "query", name, sizeof name);         /* "query", as code_where's: an MCP call's body names its tool "name" */
    if (!name[0]) { api_http(c, 400, "{\"error\":\"query\"}"); return; }
    api_jstr(body, "path", under, sizeof under);
    lp_ref trunk, parts[256];
    size_t np = 0;
    if (!api_name_entity(name, &trunk, parts, 256, &np)) { api_http(c, 500, "{\"error\":\"tier0\"}"); return; }
    PGconn *pg = api_cc_db(c);
    if (!pg) return;
    uint64_t t0 = api_nsec();
    CwDag d = { 0 };
    int levels = 0, cut = 0;
    cw_walk(pg,&trunk.id, &d, 400000, &levels, &cut);
    Reader *rd = reader_new(pg);
    /* the definitions: each its file, its outline entry and its code; the names each calls, by candidate index */
    typedef struct { size_t node; char *path, *kind; size_t line; long line1; lp_vec(size_t) calls; } CeDef;
    CeDef defs[16];
    size_t ndef = 0;
    lp_idmap *cand = lp_idmap_new();
    lp_vec(CeCand) cs = { 0 };
    for (size_t i = 0; i < d.nn && ndef < 16; i++) {
        if (!d.n[i].file) continue;
        char *path = api_cw_file_name(rd, &d.n[i].id);
        if (under[0] && !strstr(path, under)) { free(path); continue; }
        FoDecl *dl = NULL;
        char *text = NULL, *whole = NULL;
        size_t tl = 0, nd = fo_outline(pg, rd, &d.n[i].id, &dl, &text, &tl);
        whole = text; text = NULL;
        for (size_t q = 0; q < nd && ndef < 16; q++) {
            if (strcmp(dl[q].name, name) || dl[q].start == SIZE_MAX) continue;
            const char *s = whole + dl[q].start, *e = s + dl[q].length;
            CeDef *df = &defs[ndef++];
            memset(df, 0, sizeof *df);
            df->node = i;
            df->path = strdup(path);
            df->kind = strdup(dl[q].kind);
            df->line = dl[q].line;
            df->line1 = (long)cw_lines(whole, (size_t)(e-whole)) - (e>s && e[-1]=='\n');
            for (const char *p = s; p < e; p++) {          /* each name followed by "(" */
                if (!(isalpha((unsigned char)*p) || *p == '_' || (unsigned char)*p >= 0x80) || (p > whole && fo_word(p[-1]))) continue;
                const char *w = p;
                while (p < e && fo_word(*p)) p++;
                const char *a = p;
                while (a < e && (*a == ' ' || *a == '\t' || *a == '\r' || *a == '\n')) a++;
                if (a >= e || *a != '(') { p--; continue; }
                size_t wl = (size_t)(p - w);
                p--;
                if (wl >= 256 || (wl == strlen(name) && !memcmp(w, name, wl))) continue;
                char tok[256];
                memcpy(tok, w, wl);
                tok[wl] = 0;
                lp_ref tr, tp[256];
                size_t tnp = 0;
                if (!api_name_entity(tok, &tr, tp, 256, &tnp)) continue;
                bool fresh;
                size_t ci = lp_idmap_put(cand, &tr.id, &fresh);
                if (fresh) { CeCand x = { 0 }; x.name = strdup(tok); lp_push(&cs, x); }
                int have = 0;
                for (size_t z = 0; z < df->calls.n; z++) if (df->calls.v[z] == ci) have = 1;
                if (!have && df->calls.n < 256) lp_push(&df->calls, ci);
            }
        }
        fo_free(dl, nd);
        free(text);
        free(whole);
        free(path);
    }
    /* Resolve all candidate names together through the real containment tree.
     * A file may sit above any number of content/record block levels. */
    size_t ncand = lp_idmap_count(cand);
    if (ncand) {
        CwDag holders = {0}; int candidate_levels=0,candidate_cut=0;
        cw_walk_many(pg,lp_idmap_keys(cand),ncand,&holders,400000,&candidate_levels,&candidate_cut);
        cut |= candidate_cut;
        for (size_t i=0;i<holders.nn;i++) {
            if (!holders.n[i].file) continue;
            FoDecl *declarations=NULL; char *text=NULL; size_t length=0;
            size_t count=fo_outline(pg,rd,&holders.n[i].id,&declarations,&text,&length);
            for (size_t k=0;k<count;k++) {
                lp_ref identifier, constituents[256]; size_t nconstituents=0;
                if (!api_name_entity(declarations[k].name,&identifier,constituents,256,&nconstituents)) continue;
                int64_t candidate=lp_idmap_find(cand,&identifier.id);
                if (candidate>=0) ce_file(&cs.v[candidate],declarations[k].kind,&holders.n[i].id);
            }
            fo_free(declarations,count); free(text);
        }
        lp_idmap_free(holders.m); free(holders.n);
    }
    char *out = NULL;
    size_t on = 0, ocap = 0;
    api_buf_fmt(&out, &on, &ocap, "{\"name\":\"");
    api_cw_str(&out, &on, &ocap, name);
    api_buf_fmt(&out, &on, &ocap, "\",\"database\":\"%s\",\"definitions\":[", PQdb(pg));
    for (size_t f = 0; f < ndef; f++) {
        CeDef *df = &defs[f];
        char fh[33];
        api_hex_id(&d.n[df->node].id, fh);
        api_buf_fmt(&out, &on, &ocap, "%s{\"path\":\"", f ? "," : "");
        api_cw_str(&out, &on, &ocap, df->path);
        api_buf_fmt(&out, &on, &ocap, "\",\"file\":\"%s\",\"kind\":\"", fh);
        api_cw_str(&out, &on, &ocap, df->kind);
        api_buf_fmt(&out, &on, &ocap, "\",\"line0\":%zu,\"line1\":%ld,\"callees\":[", df->line, df->line1);
        int n = 0;
        for (size_t z = 0; z < df->calls.n; z++) {
            CeCand *x = &cs.v[df->calls.v[z]];
            if (!x->nfile) continue;
            api_buf_fmt(&out, &on, &ocap, "%s{\"name\":\"", n++ ? "," : "");
            api_cw_str(&out, &on, &ocap, x->name);
            api_buf_fmt(&out, &on, &ocap, "\",\"kind\":\"");
            api_cw_str(&out, &on, &ocap, x->kind ? x->kind : "");
            api_buf_fmt(&out, &on, &ocap, "\",\"declared_in\":%d,\"files\":[", x->total);
            for (int k = 0; k < x->nfile; k++) {
                char *pth = api_cw_file_name(rd, &x->file[k]), xh[33];
                api_hex_id(&x->file[k], xh);
                api_buf_fmt(&out, &on, &ocap, "%s{\"path\":\"", k ? "," : "");
                api_cw_str(&out, &on, &ocap, pth);
                api_buf_fmt(&out, &on, &ocap, "\",\"file\":\"%s\"}", xh);
                free(pth);
            }
            api_buf_fmt(&out, &on, &ocap, "]}");
        }
        api_buf_fmt(&out, &on, &ocap, "],\"outside\":[");
        n = 0;
        for (size_t z = 0; z < df->calls.n; z++) {
            CeCand *x = &cs.v[df->calls.v[z]];
            if (x->nfile) continue;
            api_buf_fmt(&out, &on, &ocap, "%s\"", n++ ? "," : "");
            api_cw_str(&out, &on, &ocap, x->name);
            api_buf_fmt(&out, &on, &ocap, "\"");
        }
        api_buf_fmt(&out, &on, &ocap, "]}");
        free(df->path);
        free(df->kind);
        lp_vec_free(&df->calls);
    }
    api_buf_fmt(&out, &on, &ocap, "],\"cut\":%s,\"ms\":%.3f}", cut ? "true" : "false", (double)(api_nsec() - t0) / 1e6);
    for (size_t z = 0; z < cs.n; z++) { free(cs.v[z].name); free(cs.v[z].kind); }
    lp_vec_free(&cs);
    lp_idmap_free(cand);
    reader_free(rd);
    lp_idmap_free(d.m);
    free(d.n);
    api_http(c, 200, out ? out : "{\"error\":\"memory\"}");
    free(out);
}
