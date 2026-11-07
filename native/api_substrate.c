#define _GNU_SOURCE
#include "api_internal.h"

void on_embeddings(int c, const char *body) {
    char text[8192];
    uint64_t t0 = api_nsec();
    char *inputs[256];
    size_t ni = api_parse_str_array(body, "input", inputs, 256);
    if (!ni) ni = api_parse_str_array(body, "inputs", inputs, 256);
    if (!ni) {
        if (!api_jstr(body, "input", text, sizeof text) || !text[0]) {
            api_http(c, 400, "{\"error\":\"input\"}");
            return;
        }
        inputs[0] = strdup(text);
        ni = 1;
    }
    char *out = NULL;
    size_t un = 0, ucap = 0;
    api_buf_add(&out, &un, &ucap, "{\"object\":\"list\",\"model\":\"laplace\",\"data\":[", strlen("{\"object\":\"list\",\"model\":\"laplace\",\"data\":["));
    for (size_t i = 0; i < ni; i++) {
        lp_ref trunk, parts[64];
        size_t np = 0;
        if (!api_name_entity(inputs[i], &trunk, parts, 64, &np)) {
            for (size_t k = i; k < ni; k++) free(inputs[k]);
            free(out);
            api_http(c, 500, "{\"error\":\"tier0\"}");
            return;
        }
        size_t total_parts=np;
        if(np>64)np=64;
        char id[33], pj[64 * 40], cj[160], uj[160];
        api_hex_id(&trunk.id, id);
        api_parts_json(parts, np, pj, sizeof pj);
        api_coord_json(&trunk, cj, sizeof cj);
        api_unit_coord_json(&trunk, uj, sizeof uj);
        api_buf_fmt(&out, &un, &ucap,
                "%s{\"object\":\"embedding\",\"index\":%zu,\"embedding\":%s,"
                "\"id\":\"%s\",\"tier\":%u,\"coord\":%s,\"hilbert\":\"%016llx\",\"parts\":[%s],\"parts_total\":%zu,\"parts_complete\":%s}",
                i ? "," : "", i, uj, id, trunk.tier, cj, (unsigned long long)lp_hilbert4(&trunk.c), pj,total_parts,total_parts==np?"true":"false");
        free(inputs[i]);
    }
    double ms = (double)(api_nsec() - t0) / 1e6;
    api_buf_fmt(&out, &un, &ucap, "],\"usage\":{\"prompt_tokens\":%zu,\"total_tokens\":%zu},\"ms\":{\"identity\":%.3f}}", ni, ni, ms);
    api_http(c, 200, out ? out : "{\"object\":\"list\",\"data\":[]}");
    free(out);
}

/* One constituent reference in a mixed-tier path. Non-atomic content is read
 * the same way as the trunk: the paths that hold it, and their attestations.
 * Its own constituents stay on it, so a word's codepoints are not dropped and are not searched as the trunk. */
typedef struct {
    lp_id id;
    int tier;
    int stored_tier;
    int cp;
    int have_c;
    lp_coord c;
    Hold *h;
    int nh;
    Attested *att;
    int natt;
    lp_id *kids;
    int *ktier;
    int *kcp;
    int nkids;
} Down;

/* A deterministic identity is not presence. Prepared canonical content and
 * database recording are separate bases; neither implies testimony. */
static const char *content_storage(const lp_id *id, int stored_tier) {
    if (stored_tier >= 0) return "database";
    if (lp_tier0_codepoint(T0, id) >= 0) return "tier0-cache";
    return lp_highway_find(api_view_highway(), id) ? "highway-cache" : "unresolved";
}

static void emit_content_state(char **out, size_t *n, size_t *cap, const lp_id *id, int stored_tier) {
    const char *storage = content_storage(id, stored_tier);
    api_buf_fmt(out, n, cap, ",\"recorded\":%s,\"storage\":\"%s\",\"stored_tier\":", stored_tier >= 0 ? "true" : "false", storage);
    if (stored_tier >= 0) api_buf_fmt(out, n, cap, "%d", stored_tier);
    else api_buf_fmt(out, n, cap, "null");
}

static void emit_discovery_match(char **out, size_t *n, size_t *cap, const lp_id *id,
                                 int tier, int stored_tier, const char *kind,
                                 const lp_id *parent, int ordinal, int *shown) {
    if (!strcmp(content_storage(id, stored_tier), "unresolved")) return;
    char hex[33]; api_hex_id(id, hex);
    api_buf_fmt(out, n, cap, "%s{\"id\":\"%s\",\"tier\":%d,\"match\":\"%s\"", (*shown)++ ? "," : "", hex, tier, kind);
    emit_content_state(out, n, cap, id, stored_tier);
    if (parent) {
        api_hex_id(parent, hex);
        api_buf_fmt(out, n, cap, ",\"via\":\"%s\",\"ordinal\":%d", hex, ordinal);
    }
    api_buf_fmt(out, n, cap, "}");
}

static void downs_free(Down *d, int n) {
    if (!d) return;
    for (int i = 0; i < n; i++) {
        holds_free(d[i].h, d[i].nh);
        free(d[i].att);
        free(d[i].kids);
        free(d[i].ktier);
        free(d[i].kcp);
    }
    free(d);
}

static void emit_ids(char **b, size_t *n, size_t *cap, const uint8_t *path, int plen, int *nids) {
    size_t m = path && plen > 0 ? lp_path_ids(path, (size_t)plen, NULL, 0) : 0;
    *nids = (int)m;
    lp_id *ids = malloc(sizeof(lp_id) * (m ? m : 1));
    if (m) lp_path_ids(path, (size_t)plen, ids, m);
    api_buf_add(b, n, cap, "[", 1);
    for (size_t i = 0; i < m; i++) {
        char hex[33];
        api_hex_id(&ids[i], hex);
        api_buf_fmt(b, n, cap, "%s\"%s\"", i ? "," : "", hex);
    }
    api_buf_add(b, n, cap, "]", 1);
    free(ids);
}

/* What the records under one trunk said of one claim: games (the records that say it), tokens (with the times each says
 * it), score (the mean a game), position (the least any gave it; null when none did). */
static void emit_att_rows(char **b, size_t *n, size_t *cap, const Attested *att, int rows) {
    for (int i = 0; i < rows; i++) {
        char claim[33], trunk[33], pos[16] = "null";
        api_hex_id(&att[i].claim, claim);
        api_hex_id(&att[i].trunk, trunk);
        if (att[i].position) snprintf(pos, sizeof pos, "%u", att[i].position);
        api_buf_fmt(b, n, cap,
            "%s{\"claim\":\"%s\",\"trunk\":\"%s\",\"witness\":%s,\"trust\":%.6g,"
            "\"games\":%.0f,\"tokens\":%.0f,\"score\":%.6g,\"position\":%s}",
            i ? "," : "", claim, trunk, att[i].witness ? "true" : "false", att[i].trust,
            att[i].games, att[i].tokens, att[i].games > 0 ? att[i].score / att[i].games : 0.0, pos);
    }
}

/* One constituent. Tier 0 carries its codepoint. A tier at or above 2 carries the paths that hold it. */
static void emit_down(char **b, size_t *n, size_t *cap, const Down *d, int fan) {
    char eid[33], cj[160];
    api_hex_id(&d->id, eid);
    api_buf_fmt(b, n, cap, "{\"id\":\"%s\",\"tier\":%d", eid, d->tier);
    emit_content_state(b, n, cap, &d->id, d->stored_tier);
    if (d->cp >= 0) api_buf_fmt(b, n, cap, ",\"codepoint\":%d", d->cp);
    if (d->have_c) {
        lp_ref r;
        memset(&r, 0, sizeof r);
        r.c = d->c;
        api_coord_json(&r, cj, sizeof cj);
        api_buf_fmt(b, n, cap, ",\"coord\":%s", cj);
    }
    if (d->nkids > 0) {
        api_buf_add(b, n, cap, ",\"constituents\":[", strlen(",\"constituents\":["));
        for (int k = 0; k < d->nkids; k++) {
            char kid[33];
            api_hex_id(&d->kids[k], kid);
            api_buf_fmt(b, n, cap, "%s{\"id\":\"%s\",\"tier\":%d", k ? "," : "", kid, d->ktier[k]);
            if (d->kcp[k] >= 0) api_buf_fmt(b, n, cap, ",\"codepoint\":%d", d->kcp[k]);
            api_buf_add(b, n, cap, "}", 1);
        }
        api_buf_add(b, n, cap, "]", 1);
    }
    if (d->cp < 0) {
        int nh = d->nh;
        Hold *h = d->h;
        int *order = malloc(sizeof(int) * (size_t)(nh ? nh : 1));
        double *conf = calloc((size_t)(nh ? nh : 1), sizeof(double));
        int nco = 0, noo = 0;
        for (int i = 0; i < nh; i++) {
            if (h[i].claim && h[i].stood) conf[i] = lp_confidence(&h[i].r, 2.0);
            if (h[i].claim) order[nco++] = i;
            else noo++;
        }
        for (int a = 1; a < nco; a++) {
            int k = order[a], p = a;
            while (p > 0 && conf[order[p - 1]] < conf[k]) { order[p] = order[p - 1]; p--; }
            order[p] = k;
        }
        int ncret = nco, noret = noo;
        if (fan > 0) {
            if (ncret > fan) ncret = fan;
            if (noret > fan) noret = fan;
        }
        api_buf_fmt(b, n, cap, ",\"holds_read\":%d,\"claims_read\":%d,\"occurrences_read\":%d,\"claims\":[", nh, nco, noo);
        for (int i = 0; i < ncret; i++) {
            Hold *s = &h[order[i]];
            char id[33];
            int nids = 0;
            api_hex_id(&s->entity, id);
            api_buf_fmt(b, n, cap,
                    "%s{\"entity\":\"%s\",\"tier\":%d,\"stood\":%s,\"confidence\":%.6f,"
                    "\"rating\":%.3f,\"deviation\":%.3f,\"volatility\":%.6f,\"matches\":%" PRId64 ",\"constituents\":",
                    i ? "," : "", id, (int)s->tier, s->stood ? "true" : "false", conf[order[i]],
                    s->r.rating, s->r.deviation, s->r.volatility, s->matches);
            emit_ids(b, n, cap, s->path, s->path_len, &nids);
            api_buf_fmt(b, n, cap, ",\"n\":%d}", nids);
        }
        api_buf_add(b, n, cap, "],\"occurrences\":[", strlen("],\"occurrences\":["));
        for (int i = 0, seen = 0; i < nh && seen < noret; i++) {
            if (h[i].claim) continue;
            char id[33];
            int nids = 0;
            api_hex_id(&h[i].entity, id);
            api_buf_fmt(b, n, cap, "%s{\"entity\":\"%s\",\"tier\":%d,\"constituents\":", seen ? "," : "", id, (int)h[i].tier);
            emit_ids(b, n, cap, h[i].path, h[i].path_len, &nids);
            api_buf_fmt(b, n, cap, ",\"n\":%d}", nids);
            seen++;
        }
        api_buf_add(b, n, cap, "],\"attestations\":[", strlen("],\"attestations\":["));
        emit_att_rows(b, n, cap, d->att, d->natt);
        api_buf_add(b, n, cap, "]", 1);
        free(order);
        free(conf);
    }
    api_buf_add(b, n, cap, "}", 1);
}

void on_search(int c, const char *body) {
    char text[8192], idbuf[64];
    lp_ref trunk, parts[64];
    size_t np = 0;
    uint64_t t0 = api_nsec();
    if (api_jstr(body, "input", text, sizeof text) && text[0]) {
        if (!api_name_entity(text, &trunk, parts, 64, &np)) {
            api_http(c, 500, "{\"error\":\"tier0\"}");
            return;
        }
        api_hex_id(&trunk.id, idbuf);
    } else if (api_jstr(body, "id", idbuf, sizeof idbuf) && api_valid_id(idbuf)) {
        memset(&trunk, 0, sizeof trunk);
        np = 0;
    } else {
        api_http(c, 400, "{\"error\":\"input or id\"}");
        return;
    }
    size_t total_parts=np;
    if(np>64)np=64;
    double id_ms = (double)(api_nsec() - t0) / 1e6;
    PGconn *pg=api_db();
    if(!pg){
        char escaped[512],error[640];api_json_esc(PQerrorMessage(PG),escaped,sizeof escaped);
        snprintf(error,sizeof error,"{\"error\":\"database\",\"detail\":\"%s\"}",escaped);api_http(c,503,error);return;
    }
    int fan = 0;
    api_jint(body, "fan", &fan);
    int text_input=np>0;
    if(!text_input){lp_id_unhex(idbuf,&trunk.id);api_hex_id(&trunk.id,idbuf);}
    const lp_tier0_record *cached=lp_highway_find(api_view_highway(),&trunk.id);
    int64_t codepoint=lp_tier0_codepoint(T0,&trunk.id);
    if(!text_input && codepoint>=0){trunk=lp_ref_atom(T0,(uint32_t)codepoint);parts[0]=trunk;np=total_parts=1;}
    else if(!text_input && cached){
        if(!lp_highway_decompose(api_view_highway(),TX,&trunk.id,NULL,NULL,&trunk,parts,64,&total_parts)){
            api_http(c,503,"{\"error\":\"Highway components disagree with canonical content\"}");return;
        }
        np=total_parts>64?64:total_parts;
    }
    uint8_t ab[40];size_t al=ids_param(pg,ab,&trunk.id,1);
    const char *pv[1]={(const char *)ab};int pl[1]={(int)al},pf[1]={1};
    PGresult *entity_row=db_exec_params(pg,"SELECT tier,mask FROM @extschema@.laplace_entities($1::@extschema@.blake3[])",1,NULL,pv,pl,pf,1);
    if(PQresultStatus(entity_row)!=PGRES_TUPLES_OK){PQclear(entity_row);api_http(c,503,"{\"error\":\"entity\"}");return;}
    int recorded=PQntuples(entity_row)?(int)lp_be(PQgetvalue(entity_row,0,0),2):-1;
    if(!np && recorded>=0)trunk.tier=(uint8_t)recorded;
    t0=api_nsec();int nh=0;
    /* Cache membership supplies canonical content, never observations. Read
     * reverse references from PostgreSQL using its stored tier, if present. */
    Hold *h=holds_above(pg,&trunk.id,1,recorded,0,1,&nh);
    PGresult *self=db_exec_params(pg,
        "SELECT path,tier,mask,mask::text FROM @extschema@.laplace_physicalities($1::@extschema@.blake3[])",1,NULL,pv,pl,pf,1);
    if(PQresultStatus(self)!=PGRES_TUPLES_OK){
        char escaped[512],error[700];api_json_esc(PQerrorMessage(pg),escaped,sizeof escaped);
        snprintf(error,sizeof error,"{\"error\":\"physicality\",\"detail\":\"%s\"}",escaped);
        PQclear(self);PQclear(entity_row);holds_free(h,nh);api_http(c,503,error);return;
    }
    /* Who said this ID and every claim that holds it: a walk up the containment from them (the engine's attested). */
    int nclaim = 0;
    for (int i = 0; i < nh; i++) nclaim += h[i].claim;
    lp_id *cids = malloc(sizeof(lp_id) * (size_t)(nclaim + 1));
    int ncids = 0;
    cids[ncids++] = trunk.id;
    for (int i = 0; i < nh; i++) if (h[i].claim) cids[ncids++] = h[i].entity;
    int natt = 0;
    Attested *att = attested(pg, cids, ncids, &natt, NULL);
    /* Read non-atomic references by ID. Neither packaging depth nor a stored
     * tier is a semantic content-type test. */
    Down *down = NULL;
    int ndown = 0;
    if (np) {
        ndown = (int)np;
        down = calloc((size_t)ndown, sizeof *down);
        lp_id ids[64];int stored[64];
        for(int i=0;i<ndown;i++)ids[i]=parts[i].id;
        tiers_of(pg,ids,ndown,stored);
        for (int i = 0; i < ndown; i++) {
            down[i].id = parts[i].id;
            down[i].stored_tier = stored[i];
            down[i].tier = stored[i]>=0?stored[i]:parts[i].tier;
            down[i].cp = (int)lp_tier0_codepoint(T0, &parts[i].id);
            down[i].have_c = 1;
            down[i].c = parts[i].c;
        }
    } else if (PQntuples(self) > 0) {
        const uint8_t *pw = (const uint8_t *)PQgetvalue(self, 0, 0);
        int plen = PQgetlength(self, 0, 0);
        size_t m = lp_path_ids(pw, (size_t)plen, NULL, 0);
        ndown = (int)m;
        down = calloc((size_t)(ndown ? ndown : 1), sizeof *down);
        lp_id *ids = malloc(sizeof(lp_id) * (m ? m : 1));
        if (m) lp_path_ids(pw, (size_t)plen, ids, m);
        int *tiers=malloc(sizeof(int)*(m?m:1));tiers_of(pg,ids,ndown,tiers);
        for (int i = 0; i < ndown; i++) {
            down[i].id=ids[i];down[i].cp=(int)lp_tier0_codepoint(T0,ids+i);
            down[i].stored_tier=tiers[i];
            down[i].tier=down[i].cp>=0?0:tiers[i];
        }
        free(tiers);
        free(ids);
    }
    for (int i = 0; i < ndown; i++) {
        if (down[i].cp >= 0) continue;
        down[i].h = holds_above(pg,&down[i].id, 1, down[i].tier, 0, 1, &down[i].nh);
        int nc = 0;
        for (int k = 0; k < down[i].nh; k++) nc += down[i].h[k].claim;
        lp_id *cids2 = malloc(sizeof(lp_id) * (size_t)(nc + 1));
        int n2 = 0;
        cids2[n2++] = down[i].id;
        for (int k = 0; k < down[i].nh; k++) if (down[i].h[k].claim) cids2[n2++] = down[i].h[k].entity;
        down[i].att = attested(pg, cids2, n2, &down[i].natt, NULL);
        free(cids2);
    }
    /* One stored-path frontier and one tier frontier for the entire layer.
     * Duplicate constituent occurrences retain their own Down slots. */
    lp_vec(lp_id) roots={0};lp_vec(int) slots={0};
    for(int i=0;i<ndown;i++)if(down[i].cp<0){lp_push(&roots,down[i].id);lp_push(&slots,i);}
    int npaths=0;Hold *paths=paths_of(pg,roots.v,(int)roots.n,&npaths);
    lp_vec(lp_id) children={0};
    for(int row=0;row<npaths;row++){
        Down *node=down+slots.v[paths[row].src];
        size_t count=lp_path_ids(paths[row].path,(size_t)paths[row].path_len,NULL,0);
        node->nkids=(int)count;node->kids=malloc(sizeof(lp_id)*(count?count:1));
        node->ktier=malloc(sizeof(int)*(count?count:1));node->kcp=malloc(sizeof(int)*(count?count:1));
        if(count)lp_path_ids(paths[row].path,(size_t)paths[row].path_len,node->kids,count);
        for(size_t k=0;k<count;k++)lp_push(&children,node->kids[k]);
    }
    int *tiers=malloc(sizeof(int)*(children.n?children.n:1));tiers_of(pg,children.v,(int)children.n,tiers);
    size_t offset=0;
    /* paths are ID ordered, so distribute in that same order. */
    for(int row=0;row<npaths;row++){
        Down *node=down+slots.v[paths[row].src];
        for(int k=0;k<node->nkids;k++){
            node->kcp[k]=(int)lp_tier0_codepoint(T0,node->kids+k);
            node->ktier[k]=node->kcp[k]>=0?0:tiers[offset];offset++;
        }
    }
    free(tiers);holds_free(paths,npaths);lp_vec_free(&children);lp_vec_free(&slots);lp_vec_free(&roots);
    double db_ms = (double)(api_nsec() - t0) / 1e6;
    int *order = malloc(sizeof(int) * (size_t)(nh ? nh : 1));
    double *conf = calloc((size_t)(nh ? nh : 1), sizeof(double));
    int nco = 0, noo = 0;
    for (int i = 0; i < nh; i++) {
        if (h[i].claim && h[i].stood) conf[i] = lp_confidence(&h[i].r, 2.0);
        if (h[i].claim) order[nco++] = i;
        else noo++;
    }
    for (int a = 1; a < nco; a++) {
        int k = order[a];
        int b = a;
        while (b > 0 && conf[order[b - 1]] < conf[k]) { order[b] = order[b - 1]; b--; }
        order[b] = k;
    }
    int ncret = nco, noret = noo;
    if (fan > 0) {
        if (ncret > fan) ncret = fan;
        if (noret > fan) noret = fan;
    }
    char pj[64 * 40] = "", cj[160] = "null";
    if (np) {
        api_parts_json(parts, np, pj, sizeof pj);
        api_coord_json(&trunk, cj, sizeof cj);
    }
    char *out = NULL;
    size_t un = 0, ucap = 0;
    api_buf_fmt(&out, &un, &ucap,
            "{\"id\":\"%s\",\"tier\":%u,\"fan\":%d,\"coord\":%s,\"hilbert\":\"%016llx\","
            "\"parts\":[%s],\"parts_total\":%zu,\"parts_complete\":%s,\"ms\":{\"identity\":%.3f,\"database\":%.3f},"
            "\"holds_read\":%d,\"claims_read\":%d,\"occurrences_read\":%d,\"physicality\":[",
            idbuf, recorded>=0?(unsigned)recorded:trunk.tier, fan, np ? cj : "null",
            np ? (unsigned long long)lp_hilbert4(&trunk.c) : 0ull, pj,total_parts,total_parts==np?"true":"false",id_ms, db_ms, nh, nco, noo);
    for (int i = 0; i < PQntuples(self); i++) {
        char one[32];
        int nids = 0;
        int tier = (int)lp_be(PQgetvalue(self, i, 1), 2);
        int claim = api_mask_claim(PQgetvalue(self, i, 2), PQgetlength(self, i, 2));
        api_buf_fmt(&out, &un, &ucap, "%s{\"tier\":%d,\"claim\":%s,\"constituents\":", i ? "," : "", tier, claim ? "true" : "false");
        emit_ids(&out, &un, &ucap, (const uint8_t *)PQgetvalue(self, i, 0), PQgetlength(self, i, 0), &nids);
        snprintf(one, sizeof one, ",\"n\":%d,\"mask\":\"", nids);
        api_buf_add(&out, &un, &ucap, one, strlen(one));
        api_cw_str(&out,&un,&ucap,PQgetvalue(self,i,3));api_buf_fmt(&out,&un,&ucap,"\",\"vertices\":[");
        size_t nv=lp_path_vertices((const uint8_t *)PQgetvalue(self,i,0),(size_t)PQgetlength(self,i,0),NULL,0);
        lp_vertex *vertices=malloc((nv?nv:1)*sizeof *vertices);
        lp_path_vertices((const uint8_t *)PQgetvalue(self,i,0),(size_t)PQgetlength(self,i,0),vertices,nv);
        for(size_t k=0;k<nv;k++){
            char member[33];api_hex_id(&vertices[k].id,member);
            api_buf_fmt(&out,&un,&ucap,"%s{\"id\":\"%s\",\"ordinal\":%zu,\"position\":%u,\"run\":%u,\"said\":%u,\"outcome\":%u,\"spare\":%u}",k?",":"",member,k,vertices[k].position,vertices[k].run,vertices[k].said,vertices[k].outcome,vertices[k].spare);
        }
        free(vertices);api_buf_fmt(&out,&un,&ucap,"],\"facets\":");
        api_emit_mask_values(&out,&un,&ucap,PQgetvalue(self,i,2),PQgetlength(self,i,2));api_buf_fmt(&out,&un,&ucap,"}");
    }
    api_buf_add(&out, &un, &ucap, "],\"claims\":[", strlen("],\"claims\":["));
    int shown = 0;
    for (int i = 0; i < ncret; i++) {
        Hold *s = &h[order[i]];
        char eid[33];
        api_hex_id(&s->entity, eid);
        int nids = 0;
        api_buf_fmt(&out, &un, &ucap,
                "%s{\"entity\":\"%s\",\"tier\":%d,\"stood\":%s,\"confidence\":%.6f,"
                "\"rating\":%.3f,\"deviation\":%.3f,\"volatility\":%.6f,\"matches\":%" PRId64 ",\"constituents\":",
                shown ? "," : "", eid, (int)s->tier, s->stood ? "true" : "false", conf[order[i]],
                s->r.rating, s->r.deviation, s->r.volatility, s->matches);
        emit_ids(&out, &un, &ucap, s->path, s->path_len, &nids);
        api_buf_fmt(&out, &un, &ucap, ",\"n\":%d}", nids);
        shown++;
    }
    api_buf_add(&out, &un, &ucap, "],\"occurrences\":[", strlen("],\"occurrences\":["));
    shown = 0;
    for (int i = 0, seen = 0; i < nh && seen < noret; i++) {
        if (h[i].claim) continue;
        char eid[33];
        api_hex_id(&h[i].entity, eid);
        int nids = 0;
        api_buf_fmt(&out, &un, &ucap, "%s{\"entity\":\"%s\",\"tier\":%d,\"constituents\":", shown ? "," : "", eid, (int)h[i].tier);
        emit_ids(&out, &un, &ucap, h[i].path, h[i].path_len, &nids);
        api_buf_fmt(&out, &un, &ucap, ",\"n\":%d}", nids);
        shown++;
        seen++;
    }
    api_buf_add(&out, &un, &ucap, "],\"attestations\":[", strlen("],\"attestations\":["));
    emit_att_rows(&out, &un, &ucap, att, natt);
    api_buf_add(&out, &un, &ucap, "],\"constituents\":[", strlen("],\"constituents\":["));
    for (int i = 0; i < ndown; i++) {
        if (i) api_buf_add(&out, &un, &ucap, ",", 1);
        emit_down(&out, &un, &ucap, &down[i], fan);
    }
    api_buf_fmt(&out,&un,&ucap,"],\"database\":\"%s\",\"recorded\":%s",PQdb(pg),recorded>=0?"true":"false");
    api_buf_fmt(&out,&un,&ucap,",\"stored_tier\":");
    if(recorded>=0)api_buf_fmt(&out,&un,&ucap,"%d",recorded);else api_buf_fmt(&out,&un,&ucap,"null");
    api_buf_fmt(&out,&un,&ucap,",\"computed_tier\":");
    if(text_input)api_buf_fmt(&out,&un,&ucap,"%u",trunk.tier);else api_buf_fmt(&out,&un,&ucap,"null");
    api_buf_fmt(&out,&un,&ucap,",\"canonical_tier\":");if(np)api_buf_fmt(&out,&un,&ucap,"%u",trunk.tier);else api_buf_fmt(&out,&un,&ucap,"null");
    api_buf_fmt(&out,&un,&ucap,",\"cache_tier\":");if(codepoint>=0)api_buf_fmt(&out,&un,&ucap,"0");else if(cached)api_buf_fmt(&out,&un,&ucap,"%u",cached->pad);else api_buf_fmt(&out,&un,&ucap,"null");
    api_buf_fmt(&out,&un,&ucap,",\"storage\":\"%s\",\"entity_facets\":",PQntuples(entity_row)?"database":codepoint>=0?"tier0-cache":cached?"highway-cache":text_input?"computed":"unresolved");
    if(PQntuples(entity_row))api_emit_mask_values(&out,&un,&ucap,PQgetvalue(entity_row,0,1),PQgetlength(entity_row,0,1));else api_buf_fmt(&out,&un,&ucap,"[]");
    api_buf_fmt(&out,&un,&ucap,",\"discovery\":{\"schema\":\"laplace-discovery/v1\",\"query_kind\":\"%s\",\"root_present\":%s,\"members_complete\":%s,\"scope\":\"returned-read\",\"matches\":[",
            text_input ? "text" : "id", strcmp(content_storage(&trunk.id,recorded),"unresolved") ? "true" : "false", total_parts==np ? "true" : "false");
    int discovered=0;
    emit_discovery_match(&out,&un,&ucap,&trunk.id,recorded>=0?recorded:trunk.tier,recorded,"exact",NULL,0,&discovered);
    for(int i=0;i<ndown;i++){
        if(!memcmp(&down[i].id,&trunk.id,sizeof(lp_id)))continue;
        emit_discovery_match(&out,&un,&ucap,&down[i].id,down[i].tier,down[i].stored_tier,"member",&trunk.id,i,&discovered);
    }
    api_buf_fmt(&out,&un,&ucap,"]}");
    if(api_jbool(body,"display",0)){
        /* Native's selected-prefix reader shares a batched frontier. Labels are
         * a display projection, never an alternate browser-side decomposer. */
        lp_idmap *names=lp_idmap_new();lp_idmap_put(names,&trunk.id,NULL);
        for(int i=0;i<natt;i++)lp_idmap_put(names,&att[i].trunk,NULL);
        for(int i=0;i<ndown;i++)lp_idmap_put(names,&down[i].id,NULL);
        for(int i=0;i<nh;i++){
            int wanted=h[i].claim?0:1;
            if(h[i].claim)for(int k=0;k<ncret;k++)if(order[k]==i){wanted=1;break;}
            if(!wanted)continue;
            lp_idmap_put(names,&h[i].entity,NULL);
            size_t count=lp_path_ids(h[i].path,(size_t)h[i].path_len,NULL,0);
            lp_id *ids=malloc((count?count:1)*sizeof *ids);
            lp_path_ids(h[i].path,(size_t)h[i].path_len,ids,count);
            for(size_t k=0;k<count;k++)lp_idmap_put(names,ids+k,NULL);
            free(ids);
            if(lp_idmap_count(names)>=1024)break;
        }
        size_t count=lp_idmap_count(names);if(count>1024)count=1024;
        const lp_id *ids=lp_idmap_keys(names);Reader *rd=reader_new(pg);
        reader_prefetch(rd,ids,count,240);
        if(PQntuples(self)>0){
            lp_vertex first[2];size_t nv=lp_path_vertices((const uint8_t *)PQgetvalue(self,0,0),(size_t)PQgetlength(self,0,0),first,2);
            if(nv>=2 && first[0].said==LP_SAID_METADATA){
                char *file=api_cw_file_name(rd,&trunk.id);char *title=*file?strdup(file):reader_text(rd,&first[0].id,160);
                char metadata[33],content[33];api_hex_id(&first[0].id,metadata);api_hex_id(&first[1].id,content);
                api_buf_fmt(&out,&un,&ucap,",\"display\":{\"trunk_kind\":\"%s\",\"metadata\":\"%s\",\"content\":\"%s\",\"title\":\"",*file?"file":"source",metadata,content);
                api_cw_str(&out,&un,&ucap,title);api_buf_fmt(&out,&un,&ucap,"\"}");free(file);free(title);
            }
        }
        api_buf_fmt(&out,&un,&ucap,",\"labels\":[");
        for(size_t i=0;i<count;i++){
            char hex[33];api_hex_id(ids+i,hex);char *value=reader_text(rd,ids+i,240);
            api_buf_fmt(&out,&un,&ucap,"%s{\"id\":\"%s\",\"text\":\"",i?",":"",hex);
            api_cw_str(&out,&un,&ucap,value);api_buf_fmt(&out,&un,&ucap,"\"}");free(value);
        }
        api_buf_fmt(&out,&un,&ucap,"],\"labels_complete\":%s",lp_idmap_count(names)<=1024?"true":"false");
        reader_free(rd);lp_idmap_free(names);
    }
    api_buf_add(&out,&un,&ucap,"}",1);
    PQclear(self);
    PQclear(entity_row);
    free(att);
    free(cids);
    free(order);
    free(conf);
    holds_free(h, nh);
    downs_free(down, ndown);
    if (!out) {
        api_http(c, 500, "{\"error\":\"memory\"}");
        return;
    }
    api_http(c, 200, out);
    free(out);
}
void on_by_parts(int c, const char *body, const char *sql, const char *kind) {
    char text[8192];
    if (!api_jstr(body, "input", text, sizeof text) || !text[0]) {
        api_http(c, 400, "{\"error\":\"input\"}");
        return;
    }
    lp_ref trunk, parts[256];
    size_t np = 0;
    if (!api_name_entity(text, &trunk, parts, 256, &np) || !np) {
        api_http(c, 500, "{\"error\":\"tier0\"}");
        return;
    }
    char list[256 * 34];
    if (!api_part_list(parts, np, list, sizeof list)) {
        api_http(c, 400, "{\"error\":\"phrase too long\"}");
        return;
    }
    PGconn *pg = api_db();
    if (!pg) {
        api_http(c, 503, "{\"error\":\"database\"}");
        return;
    }
    char id[33], *rows = NULL, err[512], braced[256 * 34 + 2];
    api_hex_id(&trunk.id, id);
    snprintf(braced, sizeof braced, "{%s}", list);
    uint64_t t0 = api_nsec();
    if (!api_q1(pg, sql, braced, &rows, err, sizeof err)) {
        char e[512], b[700];
        api_json_esc(err, e, sizeof e);
        snprintf(b, sizeof b, "{\"error\":\"%s\",\"detail\":\"%s\"}", kind, e);
        api_http(c, 500, b);
        return;
    }
    double ms = (double)(api_nsec() - t0) / 1e6;
    size_t cap = strlen(rows) + 128;
    char *out = malloc(cap);
    snprintf(out, cap, "{\"id\":\"%s\",\"ms\":%.3f,\"%s\":%s}", id, ms, kind, rows);
    free(rows);
    api_http(c, 200, out);
    free(out);
}

void on_shape(int c, const char *body) {
    char a[8192], b[8192];
    if (!api_jstr(body, "input", a, sizeof a) || !api_jstr(body, "other", b, sizeof b) || !a[0] || !b[0]) {
        api_http(c, 400, "{\"error\":\"input and other\"}");
        return;
    }
    lp_ref ta, tb, pa[256], pb[256];
    size_t na = 0, nb = 0;
    if (!api_name_entity(a, &ta, pa, 256, &na) || !api_name_entity(b, &tb, pb, 256, &nb)) {
        api_http(c, 500, "{\"error\":\"tier0\"}");
        return;
    }
    char la[256 * 34], lb[256 * 34], ida[33], idb[33];
    if (!api_part_list(pa, na, la, sizeof la) || !api_part_list(pb, nb, lb, sizeof lb)) {
        api_http(c, 400, "{\"error\":\"phrase too long\"}");
        return;
    }
    api_hex_id(&ta.id, ida);
    api_hex_id(&tb.id, idb);
    PGconn *pg = api_db();
    if (!pg) {
        api_http(c, 503, "{\"error\":\"database\"}");
        return;
    }
    char *dist = NULL, err[512];
    uint64_t t0 = api_nsec();
    if (!api_q2(pg, API_SHAPE_SQL, la, lb, &dist, err, sizeof err)) {
        char e[512], buf[700];
        api_json_esc(err, e, sizeof e);
        snprintf(buf, sizeof buf, "{\"error\":\"shape\",\"detail\":\"%s\"}", e);
        api_http(c, 500, buf);
        return;
    }
    double ms = (double)(api_nsec() - t0) / 1e6;
    char out[512];
    snprintf(out, sizeof out,
             "{\"id\":\"%s\",\"other\":\"%s\",\"frechet\":%s,\"ms\":%.3f}",
             ida, idb, dist, ms);
    free(dist);
    api_http(c, 200, out);
}

