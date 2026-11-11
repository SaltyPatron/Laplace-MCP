#define _GNU_SOURCE
#include "api_internal.h"

/* ---- code from the database (Laplace-MCP#9): what an ingested source's files hold, read off the containment.
 * A file is a trunk [metadata, content]; a recipe with a grammar records its content as the syntax tree, so the nodes
 * above a name are its enclosing expressions, statements and declarations, and above them the file. Nothing is read
 * from a working tree: the path is the file's own pathname record, the text and lines are the recorded content. */
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

size_t cw_lines(const char *text, size_t upto) {
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
/* What a file says, read a level at a time: the vertices of the file and of every record and holds node below it, each
 * level one paths_of read for all of its nodes, never a statement a node. Text leaves are not descended. */
typedef struct { lp_idmap *at; lp_vec(lp_vertex) v; lp_vec(size_t) first, count; } FoTree;
static int fo_role(uint32_t said) { return said == LP_SAID_CLAIM || said == LP_SAID_RECORD || said == LP_SAID_HOLDS; }
static const lp_vertex *fo_tree_get(const FoTree *t, const lp_id *id, size_t *n) {
    int64_t i = lp_idmap_find(t->at, id);
    if (i < 0) { *n = 0; return NULL; }
    *n = t->count.v[i];
    return t->v.v + t->first.v[i];
}
static void fo_tree(PGconn *pg, const lp_id *file, FoTree *t) {
    memset(t, 0, sizeof *t);
    t->at = lp_idmap_new();
    lp_idmap *front = lp_idmap_new();
    lp_idmap_put(front, file, NULL);
    lp_vec(lp_vertex) vx = { 0 };
    while (lp_idmap_count(front)) {
        int nh = 0;
        Hold *h = paths_of(pg, lp_idmap_keys(front), (int)lp_idmap_count(front), &nh);
        lp_idmap *next = lp_idmap_new();
        for (int j = 0; j < nh; j++) {
            bool fresh;
            lp_idmap_put(t->at, &h[j].entity, &fresh);
            if (!fresh) continue;
            size_t nv = lp_path_vertices(h[j].path, (size_t)h[j].path_len, NULL, 0);
            lp_vec_reserve(&vx, nv);
            lp_path_vertices(h[j].path, (size_t)h[j].path_len, vx.v, nv);
            lp_push(&t->first, t->v.n);
            lp_push(&t->count, nv);
            for (size_t k = 0; k < nv; k++) {
                lp_push(&t->v, vx.v[k]);
                if ((vx.v[k].said == LP_SAID_RECORD || vx.v[k].said == LP_SAID_HOLDS) && lp_idmap_find(t->at, &vx.v[k].id) < 0)
                    lp_idmap_put(next, &vx.v[k].id, NULL);
            }
        }
        holds_free(h, nh);
        lp_idmap_free(front);
        front = next;
    }
    lp_idmap_free(front);
    lp_vec_free(&vx);
}
static void fo_tree_free(FoTree *t) { lp_idmap_free(t->at); lp_vec_free(&t->v); lp_vec_free(&t->first); lp_vec_free(&t->count); }
int fo_word(char c) { return isalnum((unsigned char)c) || c == '_' || (unsigned char)c >= 0x80; }
typedef struct { lp_id *v; size_t n, cap; } FoClaims;
/* Content trunks may contain arbitrarily nested blocks of records. Follow
 * their explicit roles, preserving source order; never descend text leaves. */
static void fo_collect_claims(const FoTree *tree, const lp_vertex *roots, size_t count, FoClaims *claims) {
    typedef struct { lp_id id; uint32_t role; } Pending;
    lp_vec(Pending) pending = {0};
    for (size_t i=count; i-- > 0;) {
        Pending next = {roots[i].id, roots[i].said};
        if (fo_role(next.role)) lp_push(&pending,next);
    }
    while (pending.n) {
        Pending current = pending.v[--pending.n];
        if (current.role==LP_SAID_CLAIM) { lp_push(claims,current.id); continue; }
        size_t n, marked=0;
        const lp_vertex *vertices=fo_tree_get(tree,&current.id,&n);
        for (size_t i=n; i-- > 0;) {
            Pending next = {vertices[i].id,vertices[i].said};
            if (!fo_role(next.role)) continue;
            marked++;
            for (uint32_t run=vertices[i].run ? vertices[i].run : 1; run; run--) lp_push(&pending,next);
        }
        if (!marked && current.role==LP_SAID_RECORD) lp_push(claims,current.id);
    }
    lp_vec_free(&pending);
}
/* A file's recorded text: every part but the OS's record and what is said of it (a claim, a record that is one claim);
 * a record of claims is content, its claims read as nothing. Its parts are read together. */
static char *fo_tree_text(const FoTree *tree, Reader *rd, const lp_id *file, size_t *tnp) {
    size_t nv;
    const lp_vertex *fv = fo_tree_get(tree, file, &nv);
    lp_vec(lp_id) parts = { 0 };
    lp_vec(uint32_t) runs = { 0 };
    for (size_t v = 0; v < nv; v++) {
        uint32_t sd = fv[v].said;
        if (sd == LP_SAID_METADATA || sd == LP_SAID_CLAIM) continue;
        if (sd == LP_SAID_RECORD) {
            size_t nr;
            const lp_vertex *rv = fo_tree_get(tree, &fv[v].id, &nr);
            int has = 0;
            for (size_t k = 0; k < nr; k++) if (rv[k].said == LP_SAID_CLAIM) has = 1;
            if (!has) continue;                 /* a record that is one claim says it; a record of claims is content */
        }
        lp_push(&parts, fv[v].id);
        lp_push(&runs, fv[v].run ? fv[v].run : 1);
    }
    reader_prefetch(rd, parts.v, parts.n, 0);
    size_t tn = 0, tcap = 1 << 16;
    char *text = malloc(tcap);
    text[0] = 0;
    for (size_t v = 0; v < parts.n; v++)
        for (uint32_t k = 0; k < runs.v[v]; k++) {
            char *t = reader_text(rd, &parts.v[v], (size_t)8 << 20);
            size_t l = strlen(t);
            if (tn + l + 1 > tcap) { while (tn + l + 1 > tcap) tcap *= 2; text = realloc(text, tcap); }
            memcpy(text + tn, t, l + 1);
            tn += l;
            free(t);
        }
    lp_vec_free(&parts);
    lp_vec_free(&runs);
    *tnp = tn;
    return text;
}
char *fo_text(PGconn *pg, Reader *rd, const lp_id *file, size_t *tnp) {
    FoTree tree;
    fo_tree(pg, file, &tree);
    char *text = fo_tree_text(&tree, rd, file, tnp);
    fo_tree_free(&tree);
    return text;
}
/* One file's outline: its recorded text (every part but the OS's record and what is said of the file) and the claims
 * [kind, name] said of it, in the file's order, each at the first line at or after the one before it where its name
 * stands as a word in that text (line 0: it does not). file_outline answers it; code_callers and code_callees read it. */
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
void fo_free(FoDecl *d, size_t n) {
    for (size_t k = 0; k < n; k++) { free(d[k].kind); free(d[k].name); free(d[k].source); }
    free(d);
}
size_t fo_outline(PGconn *pg, Reader *rd, const lp_id *file, FoDecl **declp, char **textp, size_t *tnp) {
    FoTree tree;
    fo_tree(pg, file, &tree);
    size_t tn = 0, nv;
    char *text = fo_tree_text(&tree, rd, file, &tn);
    const lp_vertex *fv = fo_tree_get(&tree, file, &nv);
    FoClaims claims = { 0 };
    fo_collect_claims(&tree, fv, nv, &claims);
    fo_tree_free(&tree);
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
    *declp = dl;
    *textp = text;
    *tnp = tn;
    return nd;
}
/* Declaration spans come from recorded grammar captures, not a second parser.
 * Select the smallest declaration that contains a recorded textual position. */
long fo_holder(const FoDecl *declarations, size_t count, size_t position) {
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
        /* up from the file's name through the OS's record of it, to the trunks whose first vertex is that record */
        HeldUp up;
        held_up(pg, &trunk.id, 1, HELD_NAME, -1, 20000, &up);
        for (size_t i = 0; i < up.n; i++) {
            if (!up.node[i].file) continue;
            char *path = api_cw_file_name(rd, &up.node[i].id);
            size_t pl = strlen(path), wl = strlen(want);
            if (pl >= wl && !strcmp(path + pl - wl, want) && (pl == wl || want[0] == '/' || path[pl - wl - 1] == '/'))
                lp_push(&files, up.node[i].id);
            free(path);
        }
        held_up_free(&up);
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
    uint64_t t0=api_nsec(); HeldUp d;
    held_up(pg,&trunk.id,1,HELD_CONTENT,-1,400000,&d);
    size_t nfiles=0; for(size_t i=0;i<d.n;i++) nfiles+=d.node[i].file;
    double walk_ms=(double)(api_nsec()-t0)/1e6;
    Reader *rd=reader_new(pg);
    typedef struct { CwFile rank; FoDecl *declarations; size_t count,length; char *text; } Found;
    Found *found=calloc(nfiles+1,sizeof *found); size_t nf=0;
    for(size_t i=0;i<d.n;i++){
        if(!d.node[i].file) continue;
        char *path=api_cw_file_name(rd,&d.node[i].id);
        if(under[0]&&!strstr(path,under)){ free(path); continue; }
        Found f={0}; f.rank.file=i; f.rank.path=path;
        f.count=fo_outline(pg,rd,&d.node[i].id,&f.declarations,&f.text,&f.length);
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
            idh,PQdb(pg),d.levels,d.n,nf,d.cut?"true":"false");
    size_t shown=0;
    for(size_t f=0;f<nf&&shown<(size_t)limit;f++){
        Found *file=&found[f]; char fh[33]; api_hex_id(&d.node[file->rank.file].id,fh);
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
    api_buf_fmt(&out,&on,&ocap,"],\"shown\":%zu,\"walk_ms\":%.3f,\"ms\":%.3f}",shown,walk_ms,(double)(api_nsec()-t0)/1e6);
    for(size_t f=0;f<nf;f++){fo_free(found[f].declarations,found[f].count);free(found[f].text);free(found[f].rank.path);}
    free(found);reader_free(rd);held_up_free(&d);
    api_http(c,200,out?out:"{\"error\":\"memory\"}");free(out);
}
