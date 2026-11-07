#define _GNU_SOURCE
#include "api_internal.h"

/* ---- code from the database (Laplace-MCP#9): what an ingested source's files hold, read off the containment.
 * A file is a trunk [metadata, content]; a recipe with a grammar records its content as the syntax tree, so the nodes
 * above a name are its enclosing expressions, statements and declarations, and above them the file. Nothing is read
 * from a working tree: the path is the file's own pathname record, the text and lines are the recorded content. */
typedef struct { lp_id id; uint8_t file; } CwNode;
typedef struct { CwNode *n; size_t nn, cn; lp_idmap *m; } CwDag;
static size_t cw_node(CwDag *d, const lp_id *id, int *fresh) {
    bool f;
    size_t i = lp_idmap_put(d->m, id, &f);
    if (f) {
        if (d->nn == d->cn) { d->cn = d->cn ? d->cn * 2 : 1024; CwNode *g = realloc(d->n, d->cn * sizeof *d->n); if (!g) abort(); d->n = g; }
        memset(&d->n[d->nn], 0, sizeof d->n[d->nn]);
        d->n[d->nn].id = *id;
        d->nn++;
    }
    if (fresh) *fresh = f;
    return i;
}
/* A file's path from the OS's record in its metadata tree: [pathname, its folders], or [filename, NAME]. */
char *api_cw_file_name(Reader *rd, const lp_id *file) {
    lp_id at = *file, kid[64];
    for (int depth = 0; depth < 8; depth++) {
        size_t n = reader_parts(rd, &at, kid, 64);
        if (!n) break;
        for (size_t i = 0; i < n && i < 64; i++) {
            lp_id two[4];
            size_t m = reader_parts(rd, &kid[i], two, 4);
            if (m != 2) continue;
            char *k = reader_text(rd, &two[0], 32);
            int path = !strcmp(k, "pathname"), name = !strcmp(k, "filename");
            free(k);
            if (name) return reader_text(rd, &two[1], 300);
            if (path) {
                lp_id seg[128];
                size_t ns = reader_parts(rd, &two[1], seg, 128);
                char *o = calloc(1, 4096);
                size_t ol = 0;
                for (size_t z = 0; z < ns && z < 128 && ol < 4000; z++) {
                    char *t = reader_text(rd, &seg[z], 300);
                    ol += (size_t)snprintf(o + ol, 4096 - ol, "/%s", t);
                    free(t);
                }
                return o;
            }
        }
        at = kid[0];
    }
    return strdup("");
}

/* Up from one name through every path that holds it, to the files: a holder whose first vertex is metadata, reached
 * through its content, is a file and the walk stops there. A name held only as metadata (a file called so) is not an
 * occurrence. A code recipe states [declaration, "declared identifier", name],
 * with declaration = [kind, complete source bytes]. A record of one claim is
 * that claim, said as a record (LP_SAID_RECORD) where it stands, so a holder of the name said so one level up is a claim
 * too, as the engine's walk up counts it (walk_up): a file that declares one thing defines it. The front is bounded, so a
 * name as common as a keyword answers what it reached and says so. */
static int cw_walk_many(PGconn *pg,const lp_id *names, size_t count, CwDag *d, size_t max_nodes, int *levels, int *cut) {
    d->m = lp_idmap_new();
    lp_idmap *front = lp_idmap_new();
    for (size_t i=0;i<count;i++) { cw_node(d,&names[i],NULL); lp_idmap_put(front,&names[i],NULL); }
    lp_vec(lp_vertex) vx = { 0 };
    int level = 0, files = 0;
    *cut = 0;
    while (lp_idmap_count(front) && level < 128) {
        int nh = 0;
        Hold *h = holds_any(pg,lp_idmap_keys(front), (int)lp_idmap_count(front), &nh);
        lp_idmap *next = lp_idmap_new();
        for (int j = 0; j < nh && !*cut; j++) {
            size_t nv = lp_path_vertices(h[j].path, (size_t)h[j].path_len, NULL, 0);
            if (!nv) continue;
            lp_vec_reserve(&vx, nv);
            lp_path_vertices(h[j].path, (size_t)h[j].path_len, vx.v, nv);
            int is_file = vx.v[0].said == LP_SAID_METADATA;
            for (size_t v = 0; v < nv; v++) {
                if (vx.v[v].said == LP_SAID_METADATA) continue;
                if (lp_idmap_find(front, &vx.v[v].id) < 0) continue;
                ptrdiff_t ci = lp_idmap_find(d->m, &vx.v[v].id);
                if (ci < 0) continue;
                if (d->nn >= max_nodes) { *cut = 1; break; }
                int fresh = 0;
                size_t pi = cw_node(d, &h[j].entity, &fresh);
                if (fresh && is_file) { d->n[pi].file = 1; files++; }
                else if (fresh) lp_idmap_put(next, &h[j].entity, NULL);
            }
        }
        holds_free(h, nh);
        lp_idmap_free(front);
        front = next;
        level++;
        if (*cut) break;
    }
    lp_vec_free(&vx);
    lp_idmap_free(front);
    *levels = level;
    return files;
}
static int cw_walk(PGconn *pg,const lp_id *name, CwDag *d, size_t max_nodes, int *levels, int *cut) {
    return cw_walk_many(pg,name,1,d,max_nodes,levels,cut);
}

static size_t cw_lines(const char *text, size_t upto) {
    size_t l = 1;
    for (size_t i = 0; i < upto && text[i]; i++) if (text[i] == '\n') l++;
    return l;
}

void api_cw_str(char **out, size_t *on, size_t *ocap, const char *s) {
    size_t l = strlen(s) * 6 + 8;
    char *e = malloc(l);
    api_json_esc(s, e, l);
    api_buf_add(out, on, ocap, e, strlen(e));
    free(e);
}

typedef struct { size_t file; int defines, occ; char *path; } CwFile;
static int cw_file_cmp(const void *a, const void *b) {
    const CwFile *x = a, *y = b;
    if (x->defines != y->defines) return y->defines - x->defines;
    if (x->occ != y->occ) return y->occ - x->occ;
    return strcmp(x->path, y->path);
}

/* ---- file_outline: what a file declares, from the database. The file is found by its name: up from the name through
 * the OS's record of it (its metadata) to the trunk whose first vertex is that record, and its pathname ends in the path
 * asked for (or the file is named by its ID, as code_where returns it). What a code recipe says of a file is a record
 * of typed declaration claims, one a declaration, in the file's order (or one claim, its own record). Each is at the first
 * line at or after the one before it where its name stands as a word in the file's recorded text. */
static int fo_hex(const char *s, lp_id *id) {
    if (strlen(s) != 32) return 0;
    for (int b = 0; b < 16; b++) {
        unsigned v;
        if (sscanf(s + b * 2, "%2x", &v) != 1) return 0;
        id->b[b] = (uint8_t)v;
    }
    return 1;
}
/* One entity's vertices, as laplace_paths gives its path. */
static size_t fo_vertices(PGconn *pg, const lp_id *id, lp_vertex **out, size_t *cap) {
    char h[33], arr[40];
    api_hex_id(id, h);
    snprintf(arr, sizeof arr, "{%s}", h);
    const char *v[1] = { arr };
    PGresult *r = db_exec_params(pg, "SELECT path::text FROM @extschema@.laplace_paths($1::@extschema@.blake3[])", 1, NULL, v, NULL, NULL, 0);
    size_t n = 0;
    if (PQresultStatus(r) == PGRES_TUPLES_OK && PQntuples(r) > 0) {
        const char *hx = PQgetvalue(r, 0, 0);
        size_t hl = strlen(hx) / 2;
        uint8_t *b = malloc(hl + 1);
        for (size_t i = 0; i < hl; i++) { unsigned x; sscanf(hx + i * 2, "%2x", &x); b[i] = (uint8_t)x; }
        n = lp_path_vertices(b, hl, NULL, 0);
        lp_reserve((void **)out, cap, n, sizeof **out);
        lp_path_vertices(b, hl, *out, n);
        free(b);
    }
    PQclear(r);
    return n;
}
static int fo_word(char c) { return isalnum((unsigned char)c) || c == '_' || (unsigned char)c >= 0x80; }
typedef struct { lp_id *v; size_t n, cap; } FoClaims;
/* Content trunks may contain arbitrarily nested blocks of records. Follow
 * their explicit roles, preserving source order; never descend text leaves. */
static void fo_collect_claims(PGconn *pg, const lp_vertex *roots, size_t count, FoClaims *claims) {
    typedef struct { lp_id id; uint32_t role; } Pending;
    lp_vec(Pending) pending = {0};
    lp_vertex *vertices=NULL; size_t capacity=0;
    for (size_t i=count; i-- > 0;) {
        Pending next = {roots[i].id, roots[i].said};
        if (next.role==LP_SAID_CLAIM || next.role==LP_SAID_RECORD || next.role==LP_SAID_HOLDS) lp_push(&pending,next);
    }
    while (pending.n) {
        Pending current = pending.v[--pending.n];
        if (current.role==LP_SAID_CLAIM) { lp_push(claims,current.id); continue; }
        size_t n=fo_vertices(pg,&current.id,&vertices,&capacity), marked=0;
        for (size_t i=n; i-- > 0;) {
            Pending next = {vertices[i].id,vertices[i].said};
            if (next.role!=LP_SAID_CLAIM && next.role!=LP_SAID_RECORD && next.role!=LP_SAID_HOLDS) continue;
            marked++;
            for (uint32_t run=vertices[i].run ? vertices[i].run : 1; run; run--) lp_push(&pending,next);
        }
        if (!marked && current.role==LP_SAID_RECORD) lp_push(claims,current.id);
    }
    free(vertices);
    lp_vec_free(&pending);
}
/* One file's outline: its recorded text (every part but the OS's record and what is said of the file) and the claims
 * [kind, name] said of it, in the file's order, each at the first line at or after the one before it where its name
 * stands as a word in that text (line 0: it does not). file_outline answers it; code_callers and code_callees read it. */
typedef struct { char *kind, *name, *source; size_t line, start, length, name_at; } FoDecl;
typedef struct { lp_id kind, name, source; } CodeDeclaration;
/* Read the recipe's typed declaration from the persisted semantic claim. */
static int code_declaration(Reader *rd, const lp_id *claim, CodeDeclaration *out) {
    lp_id parts[4], subject[3];
    if (reader_parts(rd, claim, parts, 4) != 3) return 0;
    char *relation = reader_text(rd, &parts[1], 80);
    int declaration = !strcmp(relation, "declared identifier");
    free(relation);
    if (!declaration || reader_parts(rd, &parts[0], subject, 3) != 2) return 0;
    *out = (CodeDeclaration){subject[0], parts[2], subject[1]};
    return 1;
}
static void fo_free(FoDecl *d, size_t n) {
    for (size_t k = 0; k < n; k++) { free(d[k].kind); free(d[k].name); free(d[k].source); }
    free(d);
}
static size_t fo_outline(PGconn *pg, Reader *rd, const lp_id *file, FoDecl **declp, char **textp, size_t *tnp) {
    lp_vertex *fv=NULL, *rv=NULL; size_t fcap=0, rcap=0;
    size_t nv = fo_vertices(pg, file, &fv, &fcap);
    /* the recorded text: every part but the OS's record and what is said of the file */
    size_t tn = 0, tcap = 1 << 16;
    char *text = malloc(tcap);
    text[0] = 0;
    FoClaims claims = { 0 };
    fo_collect_claims(pg, fv, nv, &claims);
    for (size_t v = 0; v < nv; v++) {
        uint32_t sd = fv[v].said;
        if (sd == LP_SAID_METADATA) continue;
        if (sd == LP_SAID_RECORD || sd == LP_SAID_CLAIM) {
            size_t nr = sd == LP_SAID_RECORD ? fo_vertices(pg, &fv[v].id, &rv, &rcap) : 0;
            int has = 0;
            for (size_t k = 0; k < nr; k++) if (rv[k].said == LP_SAID_CLAIM) has = 1;
            if (!has || sd == LP_SAID_CLAIM) continue;
        }
        for (uint32_t k = 0; k < (fv[v].run ? fv[v].run : 1); k++) {
            char *t = reader_text(rd, &fv[v].id, (size_t)8 << 20);
            size_t l = strlen(t);
            if (tn + l + 1 > tcap) { while (tn + l + 1 > tcap) tcap *= 2; text = realloc(text, tcap); }
            memcpy(text + tn, t, l + 1);
            tn += l;
            free(t);
        }
    }
    FoDecl *dl = NULL;
    size_t nd = 0, at = 0, line = 1;
    for (size_t k = 0; k < claims.n; k++) {
        CodeDeclaration declaration;
        if (!code_declaration(rd, &claims.v[k], &declaration)) continue;
        char *kind = reader_text(rd, &declaration.kind, 120), *name = reader_text(rd, &declaration.name, 400);
        size_t nl = strlen(name), found = 0, fl = 0;
        for (size_t p = at, ln = line; nl && p + nl <= tn; p++) {
            if (p > at && text[p - 1] == '\n') ln++;
            if (!memcmp(text + p, name, nl) && (p == 0 || !fo_word(text[p - 1])) && !fo_word(text[p + nl])) { found = p + 1; fl = ln; break; }
        }
        dl = realloc(dl, (nd + 1) * sizeof *dl);
        char *source = reader_text(rd, &declaration.source, (size_t)8 << 20);
        size_t length = strlen(source);
        const char *begin = length ? strstr(text + (at < tn ? at : tn), source) : NULL;
        if (!begin && length) begin = strstr(text, source);
        dl[nd++] = (FoDecl){kind, name, source, found ? fl : 0, begin ? (size_t)(begin-text) : SIZE_MAX, length, found ? found-1 : SIZE_MAX};
        if (found) { at = found - 1; line = fl; }
    }
    lp_vec_free(&claims);
    free(fv); free(rv);
    *declp = dl;
    *textp = text;
    *tnp = tn;
    return nd;
}
/* Declaration spans come from recorded grammar captures, not a second parser.
 * Select the smallest declaration that contains a recorded textual position. */
static long fo_holder(const FoDecl *declarations, size_t count, size_t position) {
    long best = -1;
    for (size_t i = 0; i < count; i++) {
        const FoDecl *d = &declarations[i];
        if (d->start == SIZE_MAX || position < d->start || position-d->start >= d->length) continue;
        if (best < 0 || d->length < declarations[best].length) best = (long)i;
    }
    return best;
}
void on_file_outline(int c, const char *body) {
    char want[2048] = "", fid[64] = "";
    api_jstr(body, "path", want, sizeof want);
    api_jstr(body, "file", fid, sizeof fid);
    if (!want[0] && !fid[0]) { api_http(c, 400, "{\"error\":\"path or file\"}"); return; }
    PGconn *pg = api_db();
    if (!pg) {
        char e[512], b[640];
        api_json_esc(PQerrorMessage(PG), e, sizeof e);
        snprintf(b, sizeof b, "{\"error\":\"database\",\"detail\":\"%s\"}", e);
        api_http(c, 503, b);
        return;
    }
    uint64_t t0 = api_nsec();
    Reader *rd = reader_new(pg);
    lp_vec(lp_id) files = { 0 };
    if (fid[0]) {
        lp_id f;
        if (!fo_hex(fid, &f)) { reader_free(rd); api_http(c, 400, "{\"error\":\"file is a 32-digit ID\"}"); return; }
        lp_push(&files, f);
    } else {
        const char *base = strrchr(want, '/');
        base = base ? base + 1 : want;
        lp_ref trunk, parts[256];
        size_t np = 0;
        if (!*base || !api_name_entity(base, &trunk, parts, 256, &np)) { reader_free(rd); api_http(c, 400, "{\"error\":\"path\"}"); return; }
        lp_idmap *seen = lp_idmap_new(), *front = lp_idmap_new();
        lp_idmap_put(front, &trunk.id, NULL);
        lp_vec(lp_vertex) vx = { 0 };
        for (int level = 0; level < 10 && lp_idmap_count(front) && lp_idmap_count(seen) < 20000; level++) {
            int nh = 0;
            Hold *h = holds_any(pg,lp_idmap_keys(front), (int)lp_idmap_count(front), &nh);
            lp_idmap *next = lp_idmap_new();
            for (int j = 0; j < nh; j++) {
                size_t nv = lp_path_vertices(h[j].path, (size_t)h[j].path_len, NULL, 0);
                if (!nv) continue;
                lp_vec_reserve(&vx, nv);
                lp_path_vertices(h[j].path, (size_t)h[j].path_len, vx.v, nv);
                bool fresh;
                lp_idmap_put(seen, &h[j].entity, &fresh);
                if (!fresh) continue;
                /* a trunk whose first vertex is the OS's record, reached through that record: a file */
                if (vx.v[0].said == LP_SAID_METADATA && lp_idmap_find(front, &vx.v[0].id) >= 0) {
                    char *path = api_cw_file_name(rd, &h[j].entity);
                    size_t pl = strlen(path), wl = strlen(want);
                    if (pl >= wl && !strcmp(path + pl - wl, want) && (pl == wl || want[0] == '/' || path[pl - wl - 1] == '/'))
                        lp_push(&files, h[j].entity);
                    free(path);
                } else if (vx.v[0].said != LP_SAID_METADATA) lp_idmap_put(next, &h[j].entity, NULL);
            }
            holds_free(h, nh);
            lp_idmap_free(front);
            front = next;
        }
        lp_vec_free(&vx);
        lp_idmap_free(front);
        lp_idmap_free(seen);
    }
    char *out = NULL;
    size_t on = 0, ocap = 0;
    api_buf_fmt(&out, &on, &ocap, "{\"database\":\"%s\",\"files\":[", PQdb(pg));
    for (size_t f = 0; f < files.n && f < 16; f++) {
        FoDecl *dl = NULL;
        char *text = NULL;
        size_t tn = 0, nd = fo_outline(pg, rd, &files.v[f], &dl, &text, &tn);
        char *path = api_cw_file_name(rd, &files.v[f]), pe[8192], fh[33];
        api_json_esc(path, pe, sizeof pe);
        api_hex_id(&files.v[f], fh);
        api_buf_fmt(&out, &on, &ocap, "%s{\"path\":\"%s\",\"file\":\"%s\",\"bytes\":%zu,\"declarations\":[", f ? "," : "", pe, fh, tn);
        free(path);
        for (size_t k = 0; k < nd; k++) {
            api_buf_add(&out, &on, &ocap, k ? ",{\"kind\":\"" : "{\"kind\":\"", k ? 10 : 9);
            api_cw_str(&out, &on, &ocap, dl[k].kind);
            api_buf_add(&out, &on, &ocap, "\",\"name\":\"", 10);
            api_cw_str(&out, &on, &ocap, dl[k].name);
            api_buf_add(&out, &on, &ocap, "\"", 1);
            if (dl[k].line) api_buf_fmt(&out, &on, &ocap, ",\"line\":%zu", dl[k].line);
            api_buf_add(&out, &on, &ocap, "}", 1);
        }
        api_buf_add(&out, &on, &ocap, "]", 1);
        if (api_jbool(body, "include_text", 0)) {
            api_buf_add(&out, &on, &ocap, ",\"text\":\"", 9);
            api_cw_str(&out, &on, &ocap, text);
            api_buf_add(&out, &on, &ocap, "\"", 1);
        }
        api_buf_add(&out, &on, &ocap, "}", 1);
        fo_free(dl, nd);
        free(text);
    }
    api_buf_fmt(&out, &on, &ocap, "],\"ms\":%.3f}", (double)(api_nsec() - t0) / 1e6);
    lp_vec_free(&files);
    reader_free(rd);
    api_http(c, 200, out);
    free(out);
}

/* recipe_plan: which recipe reads what, as laplace ingest --plan answers it; nothing is read into the database. */
void on_recipe_plan(int c, const char *body) {
    char what[4096] = "";
    api_jstr(body, "input", what, sizeof what);
    if (what[0] && !api_plain_arg(what)) { api_http(c, 400, "{\"error\":\"input is a source name or a path\"}"); return; }
    char *av[] = { (char *)ENGINE, "ingest", "--plan", what[0] ? what : NULL, NULL };
    api_on_engine(c, av);
}

void on_recipe_catalog(int c, const char *body) {
    (void)body;
    char *av[] = { (char *)ENGINE, "ingest", "--catalog", NULL };
    api_on_engine(c, av);
}

PGconn *api_cc_db(int c);
void on_code_where(int c, const char *body) {
    char name[2048] = "", under[512] = "";
    if ((!api_jstr(body, "query", name, sizeof name) || !name[0]) && (!api_jstr(body, "input", name, sizeof name) || !name[0])) {
        api_http(c, 400, "{\"error\":\"query\"}"); return;
    }
    api_jstr(body, "path", under, sizeof under);
    int limit=20, context=120, per=3, defs_only=api_jbool(body,"defines",0);
    api_jint(body,"limit",&limit); api_jint(body,"context",&context); api_jint(body,"occurrences",&per);
    if (limit<1) limit=1; if(limit>200) limit=200;
    if (context<20) context=20; if(context>4000) context=4000;
    if (per<0) per=0; if(per>16) per=16;
    lp_ref trunk, parts[256]; size_t np=0;
    if (!api_name_entity(name,&trunk,parts,256,&np)) { api_http(c,500,"{\"error\":\"tier0\"}"); return; }
    PGconn *pg=api_cc_db(c); if(!pg) return;
    uint64_t t0=api_nsec(); CwDag d={0}; int levels=0,cut=0;
    int nfiles=cw_walk(pg,&trunk.id,&d,400000,&levels,&cut);
    Reader *rd=reader_new(pg);
    typedef struct { CwFile rank; FoDecl *declarations; size_t count,length; char *text; } Found;
    Found *found=calloc((size_t)nfiles+1,sizeof *found); size_t nf=0;
    for(size_t i=0;i<d.nn;i++){
        if(!d.n[i].file) continue;
        char *path=api_cw_file_name(rd,&d.n[i].id);
        if(under[0]&&!strstr(path,under)){ free(path); continue; }
        Found f={0}; f.rank.file=i; f.rank.path=path;
        f.count=fo_outline(pg,rd,&d.n[i].id,&f.declarations,&f.text,&f.length);
        for(size_t k=0;k<f.count;k++) if(!strcmp(f.declarations[k].name,name)) f.rank.defines=1;
        const char *at=f.text;
        while((at=strstr(at,name))){ f.rank.occ++; at+=strlen(name); }
        if(!f.rank.occ||(defs_only&&!f.rank.defines)){fo_free(f.declarations,f.count);free(f.text);free(path);continue;}
        found[nf++]=f;
    }
    /* CwFile is the first member: preserve definition-first ordering. */
    qsort(found,nf,sizeof *found,cw_file_cmp);
    char idh[33]; api_hex_id(&trunk.id,idh);
    char *out=NULL; size_t on=0,ocap=0;
    api_buf_fmt(&out,&on,&ocap,"{\"query\":\""); api_cw_str(&out,&on,&ocap,name);
    api_buf_fmt(&out,&on,&ocap,"\",\"id\":\"%s\",\"database\":\"%s\",\"levels\":%d,\"nodes\":%zu,\"files\":%zu,\"cut\":%s,\"hits\":[",
            idh,PQdb(pg),levels,d.nn,nf,cut?"true":"false");
    size_t shown=0;
    for(size_t f=0;f<nf&&shown<(size_t)limit;f++){
        Found *file=&found[f]; char fh[33]; api_hex_id(&d.n[file->rank.file].id,fh);
        api_buf_fmt(&out,&on,&ocap,"%s{\"path\":\"",shown?",":""); api_cw_str(&out,&on,&ocap,file->rank.path);
        api_buf_fmt(&out,&on,&ocap,"\",\"file\":\"%s\",\"defines\":[",fh);
        int nk=0;
        for(size_t k=0;k<file->count;k++){
            FoDecl *decl=&file->declarations[k]; if(strcmp(decl->name,name)) continue;
            int duplicate=0;
            for(size_t j=0;j<k;j++) if(!strcmp(file->declarations[j].name,name)&&!strcmp(file->declarations[j].kind,decl->kind)) duplicate=1;
            if(duplicate) continue;
            api_buf_fmt(&out,&on,&ocap,"%s\"",nk++?",":""); api_cw_str(&out,&on,&ocap,decl->kind); api_buf_add(&out,&on,&ocap,"\"",1);
        }
        api_buf_fmt(&out,&on,&ocap,"],\"occurrences\":%d,\"at\":[",file->rank.occ);
        const char *at=file->text; int emitted=0;
        while(emitted<per&&(at=strstr(at,name))){
            size_t position=(size_t)(at-file->text),start=position,end=position+strlen(name);
            long holder=fo_holder(file->declarations,file->count,position);
            if(holder>=0){start=file->declarations[holder].start;end=start+file->declarations[holder].length;}
            else {while(start&&file->text[start-1]!='\n')start--;while(end<file->length&&file->text[end]!='\n')end++;}
            size_t line=cw_lines(file->text,position),line0=cw_lines(file->text,start),line1=cw_lines(file->text,end)-(end>start&&file->text[end-1]=='\n');
            api_buf_fmt(&out,&on,&ocap,"%s{\"line\":%zu,\"line0\":%zu,\"line1\":%zu,\"context\":\"",emitted?",":"",line,line0,line1);
            size_t bytes=end-start; if(bytes>(size_t)context) bytes=(size_t)context;
            while(bytes&&((uint8_t)file->text[start+bytes]&0xc0)==0x80)bytes--;
            char saved=file->text[start+bytes];file->text[start+bytes]=0;api_cw_str(&out,&on,&ocap,file->text+start);file->text[start+bytes]=saved;
            api_buf_fmt(&out,&on,&ocap,"\",\"enclosing\":\"");
            size_t head=strcspn(file->text+start,"\r\n");if(head>end-start)head=end-start;if(head>240)head=240;
            while(head&&((uint8_t)file->text[start+head]&0xc0)==0x80)head--;
            saved=file->text[start+head];file->text[start+head]=0;api_cw_str(&out,&on,&ocap,file->text+start);file->text[start+head]=saved;
            api_buf_add(&out,&on,&ocap,"\"}",2);emitted++;at+=strlen(name);
        }
        api_buf_add(&out,&on,&ocap,"]}",2);shown++;
    }
    api_buf_fmt(&out,&on,&ocap,"],\"shown\":%zu,\"ms\":%.3f}",shown,(double)(api_nsec()-t0)/1e6);
    for(size_t f=0;f<nf;f++){fo_free(found[f].declarations,found[f].count);free(found[f].text);free(found[f].rank.path);}
    free(found);reader_free(rd);lp_idmap_free(d.m);free(d.n);
    api_http(c,200,out?out:"{\"error\":\"memory\"}");free(out);
}

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

