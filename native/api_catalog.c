#define _GNU_SOURCE
#include "api_internal.h"

/* ---- ingest_status (Laplace-MCP#28): what the database holds and how current it is, read only. Each witness: its
 * name, trust, how many parts its trunk holds after its record, and readings, how many witnesses begin with the same record (a
 * source read again without superseding its earlier reading has more than one). Then the database's sessions that
 * are not idle, as pg_stat_activity shows them to this role, and the engine's per-source ingest logs
 * (LAPLACE_INGEST_LOGS, else $LAPLACE_WORK/logs/ingest), newest first, each with its last line. */
typedef struct { char name[256]; time_t mtime; long long bytes; } IsLog;
static int is_log_cmp(const void *a, const void *b) {
    const IsLog *x = a, *y = b;
    return x->mtime < y->mtime ? 1 : x->mtime > y->mtime ? -1 : strcmp(x->name, y->name);
}
void on_ingest_status(int c, const char *body) {
    char want[512] = "";
    api_jstr(body, "source", want, sizeof want);
    PGconn *pg = api_cc_db(c);
    if (!pg) return;
    uint64_t t0 = api_nsec();
    PGresult *r = db_exec_params(pg, "SELECT witness, trust FROM @extschema@.laplace_witness_trust() ORDER BY witness", 0, NULL, NULL, NULL, NULL, 1);
    if (PQresultStatus(r) != PGRES_TUPLES_OK) {
        char e[512], b[640];
        api_json_esc(PQerrorMessage(pg), e, sizeof e);
        snprintf(b, sizeof b, "{\"error\":\"witness\",\"detail\":\"%s\"}", e);
        PQclear(r);
        api_http(c, 500, b);
        return;
    }
    Reader *rd = reader_new(pg);
    int nw = PQntuples(r);
    size_t cap = (size_t)1 << 20;
    lp_id *kid = malloc(sizeof(lp_id) * cap), *recs = calloc((size_t)nw + 1, sizeof(lp_id));
    size_t *nfiles = calloc((size_t)nw + 1, sizeof(size_t));
    for (int i = 0; i < nw; i++) {
        lp_id id;
        memcpy(id.b, PQgetvalue(r, i, 0), 16);
        size_t m = reader_parts(rd, &id, kid, cap);
        if (m) recs[i] = kid[0];
        nfiles[i] = m;
    }
    char *out = NULL;
    size_t on = 0, ocap = 0;
    api_buf_fmt(&out, &on, &ocap, "{\"database\":\"%s\",\"sources\":[", PQdb(pg));
    int shown = 0;
    for (int i = 0; i < nw; i++) {
        lp_id id;
        memcpy(id.b, PQgetvalue(r, i, 0), 16);
        double trust;
        uint64_t tb = lp_be(PQgetvalue(r, i, 1), 8);
        memcpy(&trust, &tb, 8);
        if (want[0]) {                                  /* the whole name, not the shown 200 bytes of it */
            char *whole = nfiles[i] ? reader_text(rd, &recs[i], (size_t)1 << 16) : strdup("");
            int same = !strcmp(whole, want);
            free(whole);
            if (!same) continue;
        }
        char *nm = nfiles[i] ? reader_text(rd, &recs[i], 200) : strdup("");
        int readings = 0;
        for (int k = 0; k < nw; k++) if (nfiles[k] && !memcmp(recs[k].b, recs[i].b, 16)) readings++;
        char idh[33];
        api_hex_id(&id, idh);
        api_buf_fmt(&out, &on, &ocap, "%s{\"id\":\"%s\",\"name\":\"", shown++ ? "," : "", idh);
        api_cw_str(&out, &on, &ocap, nm);
        api_buf_fmt(&out, &on, &ocap, "\",\"trust\":%.3f,\"parts\":%zu%s,\"readings\":%d}", trust, nfiles[i] ? nfiles[i] - 1 : 0, nfiles[i] >= cap ? ",\"parts_cut\":true" : "", readings);
        free(nm);
    }
    PQclear(r);
    free(kid);
    free(recs);
    free(nfiles);
    reader_free(rd);
    api_buf_fmt(&out, &on, &ocap, "],\"activity\":[");
    r = db_exec(pg, "SELECT pid, coalesce(usename::text, ''), coalesce(state, ''), coalesce(extract(epoch FROM now() - query_start)::bigint, 0), "
                   "left(regexp_replace(coalesce(query, ''), '\\s+', ' ', 'g'), 200) FROM pg_stat_activity "
                   "WHERE datname = current_database() AND pid <> pg_backend_pid() AND state IS DISTINCT FROM 'idle' ORDER BY query_start NULLS LAST LIMIT 32");
    if (PQresultStatus(r) == PGRES_TUPLES_OK)
        for (int i = 0; i < PQntuples(r); i++) {
            api_buf_fmt(&out, &on, &ocap, "%s{\"pid\":%s,\"user\":\"", i ? "," : "", PQgetvalue(r, i, 0));
            api_cw_str(&out, &on, &ocap, PQgetvalue(r, i, 1));
            api_buf_fmt(&out, &on, &ocap, "\",\"state\":\"");
            api_cw_str(&out, &on, &ocap, PQgetvalue(r, i, 2));
            api_buf_fmt(&out, &on, &ocap, "\",\"seconds\":%s,\"query\":\"", PQgetvalue(r, i, 3));
            api_cw_str(&out, &on, &ocap, PQgetvalue(r, i, 4));
            api_buf_fmt(&out, &on, &ocap, "\"}");
        }
    PQclear(r);
    api_buf_fmt(&out, &on, &ocap, "],\"logs\":");
    char dir[4096] = "";
    const char *ld = getenv("LAPLACE_INGEST_LOGS"), *work = getenv("LAPLACE_WORK");
    if (ld && *ld) snprintf(dir, sizeof dir, "%s", ld);
    else if (work && *work) snprintf(dir, sizeof dir, "%s/logs/ingest", work);
    int listed = 0;
    os_list names = { 0 };
    if (dir[0] && !os_list_dir(dir, &names)) {
        lp_vec(IsLog) logs = { 0 };
        for (size_t entry = 0; entry < names.n; entry++) {
            const char *name = names.item[entry];
            size_t l = strlen(name);
            if (l < 5 || l >= 256 || strcmp(name + l - 4, ".log")) continue;
            char fp[4400];
            struct stat st;
            snprintf(fp, sizeof fp, "%s/%s", dir, name);
            if (stat(fp, &st) || !S_ISREG(st.st_mode)) continue;
            IsLog x = { "", st.st_mtime, (long long)st.st_size };
            memcpy(x.name, name, l - 4);
            x.name[l - 4] = 0;
            if (want[0] && strcmp(x.name, want)) continue;
            lp_push(&logs, x);
        }
        qsort(logs.v, logs.n, sizeof *logs.v, is_log_cmp);
        api_buf_fmt(&out, &on, &ocap, "{\"dir\":\"");
        api_cw_str(&out, &on, &ocap, dir);
        api_buf_fmt(&out, &on, &ocap, "\",\"runs\":[");
        for (size_t k = 0; k < logs.n && k < 20; k++) {
            char fp[4400], tail[4097] = "", when[32];
            snprintf(fp, sizeof fp, "%s/%s.log", dir, logs.v[k].name);
            FILE *f = fopen(fp, "rb");
            if (f) {
                long sz = logs.v[k].bytes > 4096 ? 4096 : (long)logs.v[k].bytes;
                fseek(f, -sz, SEEK_END);
                size_t got = fread(tail, 1, (size_t)sz, f);
                tail[got] = 0;
                fclose(f);
            }
            size_t tl = strlen(tail);
            while (tl && (tail[tl - 1] == '\n' || tail[tl - 1] == '\r' || tail[tl - 1] == ' ')) tail[--tl] = 0;
            char *last = strrchr(tail, '\n');
            last = last ? last + 1 : tail;
            if (strlen(last) > 240) last[240] = 0;
            for (char *p = last; *p; p++) if ((unsigned char)*p < 0x20 && *p != '\t') *p = ' ';
            struct tm tmv;
#ifdef _WIN32
            gmtime_s(&tmv, &logs.v[k].mtime);
#else
            gmtime_r(&logs.v[k].mtime, &tmv);
#endif
            strftime(when, sizeof when, "%Y-%m-%dT%H:%M:%SZ", &tmv);
            api_buf_fmt(&out, &on, &ocap, "%s{\"source\":\"", k ? "," : "");
            api_cw_str(&out, &on, &ocap, logs.v[k].name);
            api_buf_fmt(&out, &on, &ocap, "\",\"modified\":\"%s\",\"bytes\":%lld,\"last\":\"", when, logs.v[k].bytes);
            api_cw_str(&out, &on, &ocap, last);
            api_buf_fmt(&out, &on, &ocap, "\"}");
        }
        api_buf_fmt(&out, &on, &ocap, "]}");
        lp_vec_free(&logs);
        listed = 1;
    }
    os_list_free(&names);
    if (!listed) api_buf_fmt(&out, &on, &ocap, "null");
    api_buf_fmt(&out, &on, &ocap, ",\"ms\":%.3f}", (double)(api_nsec() - t0) / 1e6);
    api_http(c, 200, out);
    free(out);
}/* The sources the database holds: each witness, its name (the first part of its trunk, the record its source file
 * names) and its trust. */
void on_sources(int c, const char *body) {
    (void)body;
    PGconn *pg = api_db();
    if (!pg) { api_http(c, 503, "{\"error\":\"database\"}"); return; }
    PGresult *r = db_exec_params(pg, "SELECT witness, trust FROM @extschema@.laplace_witness_trust() ORDER BY witness", 0, NULL, NULL, NULL, NULL, 1);
    if (PQresultStatus(r) != PGRES_TUPLES_OK) {
        char e[512], b[640];
        api_json_esc(PQerrorMessage(pg), e, sizeof e);
        snprintf(b, sizeof b, "{\"error\":\"witness\",\"detail\":\"%s\"}", e);
        PQclear(r);
        api_http(c, 500, b);
        return;
    }
    Reader *rd = reader_new(pg);
    char *out = NULL;
    size_t on = 0, ocap = 0;
    api_buf_fmt(&out, &on, &ocap, "{\"database\":\"%s\",\"sources\":[", PQdb(pg));
    for (int i = 0; i < PQntuples(r); i++) {
        lp_id id;
        memcpy(id.b, PQgetvalue(r, i, 0), 16);
        double trust;
        uint64_t tb = lp_be(PQgetvalue(r, i, 1), 8);
        memcpy(&trust, &tb, 8);
        lp_id first[2];
        size_t m = reader_parts(rd, &id, first, 2);
        char *nm = m ? reader_text(rd, &first[0], 200) : strdup("");
        char idh[33], e[1300];
        api_hex_id(&id, idh);
        api_buf_fmt(&out, &on, &ocap, "%s{\"id\":\"%s\",\"name\":\"%s\",\"trust\":%.3f}", i ? "," : "", idh, api_json_esc(nm, e, sizeof e), trust);
        free(nm);
    }
    api_buf_add(&out, &on, &ocap, "]}", 2);
    PQclear(r);
    reader_free(rd);
    api_http(c, 200, out);
    free(out);
}

void on_catalog(int c,const char *body){
    (void)body;PGconn *pg=api_db();if(!pg){api_http(c,503,"{\"error\":\"database\"}");return;}
    PGresult *r=db_exec_params(pg,
        "SELECT json_build_object('database',current_database(),'read_only',current_setting('transaction_read_only')::boolean,"
        "'database_bytes',pg_database_size(current_database())::text,'entities',(SELECT count(*)::text FROM @extschema@.entity),"
        "'physicalities',(SELECT count(*)::text FROM @extschema@.physicality),'standing_rows',(SELECT count(*)::text FROM @extschema@.consensus),"
        "'attestation_rows',(SELECT count(*)::text FROM @extschema@.attestation),"
        "'registered_witness_rows',(SELECT count(DISTINCT witness)::text FROM @extschema@.attestation),'as_of',statement_timestamp(),'count_kind','exact')::text",
        0,NULL,NULL,NULL,NULL,0);
    if(PQresultStatus(r)!=PGRES_TUPLES_OK||PQntuples(r)!=1){PQclear(r);api_http(c,503,"{\"error\":\"catalog_unavailable\"}");return;}
    char *json=strdup(PQgetvalue(r,0,0));PQclear(r);api_http(c,200,json);free(json);
}

/* Discover stored provenance trunks through the native kind bank, regardless
 * of registry membership. This is the library's entry collection, not a
 * fabricated list of sample queries. Cursor order is canonical identity. */
void on_library(int c,const char *body){
    char after[64]="";api_jstr(body,"after",after,sizeof after);
    if(*after&&!api_valid_id(after)){api_http(c,400,"{\"error\":\"after\"}");return;}
    PGconn *pg=api_db();if(!pg){api_http(c,503,"{\"error\":\"database\"}");return;}
    const char *values[1]={after};
    PGresult *rows=db_exec_params(pg,
        "SELECT entity,tier,path,mask::text FROM @extschema@.physicality"
        " WHERE mask OPERATOR(@extschema@.?) @extschema@.laplace_bank_bit('kind','file')"
        " AND ($1='' OR entity OPERATOR(@extschema@.>) NULLIF($1,'')::@extschema@.blake3) ORDER BY entity LIMIT 65",
        1,NULL,values,NULL,NULL,1);
    if(PQresultStatus(rows)!=PGRES_TUPLES_OK){PQclear(rows);api_http(c,503,"{\"error\":\"library_unavailable\"}");return;}
    int count=PQntuples(rows),more=count>64;if(more)count=64;
    Reader *reader=reader_new(pg);lp_vec(lp_id) names={0};
    for(int i=0;i<count;i++){lp_id id;memcpy(&id,PQgetvalue(rows,i,0),16);lp_push(&names,id);}
    reader_prefetch(reader,names.v,names.n,160);
    char *out=NULL;size_t on=0,cap=0;
    api_buf_fmt(&out,&on,&cap,"{\"database\":\"%s\",\"collection\":\"stored-trunks\",\"scope\":\"native kind=file on physicality\",\"items\":[",PQdb(pg));
    for(int i=0;i<count;i++){
        lp_id id;memcpy(&id,PQgetvalue(rows,i,0),16);char hex[33];api_hex_id(&id,hex);
        lp_vertex first[2];size_t n=lp_path_vertices((const uint8_t *)PQgetvalue(rows,i,2),(size_t)PQgetlength(rows,i,2),first,2);
        int metadata=n>=2&&first[0].said==LP_SAID_METADATA;
        char *path=metadata?api_cw_file_name(reader,&id):strdup("");char *title=*path?strdup(path):reader_text(reader,metadata?&first[0].id:&id,160);
        api_buf_fmt(&out,&on,&cap,"%s{\"id\":\"%s\",\"tier\":%llu,\"kind\":\"%s\",\"title\":\"",i?",":"",hex,(unsigned long long)lp_be(PQgetvalue(rows,i,1),2),metadata?(*path?"file":"source"):"trunk");
        api_cw_str(&out,&on,&cap,title);api_buf_fmt(&out,&on,&cap,"\",\"mask\":\"");api_cw_str(&out,&on,&cap,PQgetvalue(rows,i,3));api_buf_fmt(&out,&on,&cap,"\"");
        if(metadata){
            char meta[33],content[33];api_hex_id(&first[0].id,meta);api_hex_id(&first[1].id,content);
            api_buf_fmt(&out,&on,&cap,",\"metadata\":\"%s\",\"content\":\"%s\"",meta,content);
            if(!*path){
                lp_id parts[4096];size_t np=reader_parts(reader,&first[1].id,parts,4096);size_t shown=np>4096?4096:np;
                api_buf_fmt(&out,&on,&cap,",\"children_complete\":%s,\"children\":[",np<=4096?"true":"false");
                for(size_t k=0;k<shown;k++){char child[33];api_hex_id(parts+k,child);api_buf_fmt(&out,&on,&cap,"%s\"%s\"",k?",":"",child);}api_buf_fmt(&out,&on,&cap,"]");
            }
        }
        api_buf_fmt(&out,&on,&cap,"}");free(path);free(title);
    }
    api_buf_fmt(&out,&on,&cap,"],\"has_more\":%s,\"next\":",more?"true":"false");
    if(more){char next[33];memcpy(names.v[0].b,PQgetvalue(rows,count-1,0),16);api_hex_id(names.v,next);api_buf_fmt(&out,&on,&cap,"\"%s\"",next);}else api_buf_fmt(&out,&on,&cap,"null");
    api_buf_fmt(&out,&on,&cap,"}");api_http(c,200,out);free(out);PQclear(rows);reader_free(reader);lp_vec_free(&names);
}

/* A bounded structural ancestry read. The graph is containment, not a claim
 * that every structural edge is an assertion. attested() above remains the
 * authority for claim/voice/outcome provenance. Source/file trunks exist as
 * entities paired with physicalities; registry membership only enriches them. */
typedef struct { int tier,expanded,parents,metadata,hub; } AncestryNode;
typedef struct { lp_id parent,child; lp_vertex vertex; size_t ordinal; } AncestryEdge;
void on_provenance(int c,const char *body){
    char value[64];lp_id start;
    if(!api_jstr(body,"id",value,sizeof value)||!api_valid_id(value)){api_http(c,400,"{\"error\":\"id\"}");return;}
    for(int i=0;i<16;i++){unsigned v=0;sscanf(value+2*i,"%2x",&v);start.b[i]=(uint8_t)v;}
    PGconn *pg=api_db();if(!pg){api_http(c,503,"{\"error\":\"database\"}");return;}
    lp_idmap *nodes=lp_idmap_sized(sizeof(AncestryNode));
    lp_idmap_put(nodes,&start,NULL);((AncestryNode *)lp_idmap_at(nodes,0))->tier=tier_max(pg,&start,1);
    int np=0;Hold *self=paths_of(pg,&start,1,&np);
    if(np){lp_vertex first;size_t nv=lp_path_vertices(self[0].path,(size_t)self[0].path_len,&first,1);((AncestryNode *)lp_idmap_at(nodes,0))->metadata=nv && first.said==LP_SAID_METADATA;}
    holds_free(self,np);
    lp_vec(lp_id) front={0};lp_push(&front,start);lp_vec(AncestryEdge) edges={0};
    int complete=1,levels=0;
    while(front.n && levels<16 && lp_idmap_count(nodes)<512){
        int nh=0;int *hubs=calloc(front.n*2,sizeof(int));
        Hold *holders=holds_capped(pg,front.v,(int)front.n,64,0,3,hubs,&nh);
        for(size_t i=0;i<front.n;i++){
            AncestryNode *node=lp_idmap_at(nodes,(size_t)lp_idmap_find(nodes,front.v+i));
            node->expanded=1;node->hub=hubs[2*i]||hubs[2*i+1];if(node->hub)complete=0;
        }
        lp_vec(lp_id) next={0};lp_idmap *current=lp_idmap_new();
        for(size_t i=0;i<front.n;i++)lp_idmap_put(current,front.v+i,NULL);
        for(int row=0;row<nh;row++){
            Hold *holder=holders+row;size_t nv=lp_path_vertices(holder->path,(size_t)holder->path_len,NULL,0);
            lp_vertex *vertices=malloc((nv?nv:1)*sizeof *vertices);lp_path_vertices(holder->path,(size_t)holder->path_len,vertices,nv);
            bool fresh;size_t at=lp_idmap_put(nodes,&holder->entity,&fresh);
            AncestryNode *parent=lp_idmap_at(nodes,at);parent->tier=holder->tier;parent->metadata|=nv && vertices[0].said==LP_SAID_METADATA;
            if(fresh)lp_push(&next,holder->entity);
            for(size_t i=0;i<nv;i++)if(lp_idmap_find(current,&vertices[i].id)>=0){
                AncestryNode *child=lp_idmap_at(nodes,(size_t)lp_idmap_find(nodes,&vertices[i].id));child->parents=1;
                int duplicate=0;for(size_t k=0;k<edges.n;k++)if(!memcmp(&edges.v[k].parent,&holder->entity,16)&&edges.v[k].ordinal==i){duplicate=1;break;}
                if(!duplicate){AncestryEdge edge={.parent=holder->entity,.child=vertices[i].id,.vertex=vertices[i],.ordinal=i};lp_push(&edges,edge);}
            }
            free(vertices);
            if(lp_idmap_count(nodes)>=512){
                complete=0;
                for(size_t k=0;k<front.n;k++)((AncestryNode *)lp_idmap_at(nodes,(size_t)lp_idmap_find(nodes,front.v+k)))->expanded=0;
                break;
            }
        }
        free(hubs);holds_free(holders,nh);lp_idmap_free(current);lp_vec_free(&front);front.v=next.v;front.n=next.n;front.cap=next.cap;levels++;
    }
    if(front.n)complete=0;
    size_t count=lp_idmap_count(nodes);const lp_id *ids=lp_idmap_keys(nodes);
    Root *roots=calloc(count?count:1,sizeof *roots);for(size_t i=0;i<count;i++)roots[i].id=ids[i];witnesses_known(pg,roots,count);
    Reader *reader=reader_new(pg);reader_prefetch(reader,ids,count,160);
    char *out=NULL;size_t on=0,cap=0;
    api_buf_fmt(&out,&on,&cap,"{\"id\":\"%s\",\"database\":\"%s\",\"complete\":%s,\"levels\":%d,\"limits\":{\"levels\":16,\"nodes\":512,\"holders_per_kind\":64},\"nodes\":[",value,PQdb(pg),complete?"true":"false",levels);
    for(size_t i=0;i<count;i++){
        AncestryNode *node=lp_idmap_at(nodes,i);char hex[33];api_hex_id(ids+i,hex);
        lp_id first[2];size_t n=node->metadata?reader_parts(reader,ids+i,first,2):0;
        char *file=node->metadata?api_cw_file_name(reader,ids+i):strdup("");
        char *label=*file?strdup(file):reader_text(reader,n?first:ids+i,160);
        api_buf_fmt(&out,&on,&cap,"%s{\"id\":\"%s\",\"tier\":%d,\"metadata_trunk\":%s,\"trunk_kind\":\"%s\",\"root\":%s,\"expanded\":%s,\"truncated\":%s,\"registered\":%s,\"trust\":%.6g,\"label\":\"",i?",":"",hex,node->tier,node->metadata?"true":"false",node->metadata?(*file?"file":"source"):"",node->expanded&&!node->parents&&!node->hub?"true":"false",node->expanded?"true":"false",node->hub?"true":"false",roots[i].witness?"true":"false",roots[i].trust);
        api_cw_str(&out,&on,&cap,label);api_buf_fmt(&out,&on,&cap,"\"}");free(label);
        free(file);
    }
    api_buf_fmt(&out,&on,&cap,"],\"edges\":[");
    for(size_t i=0;i<edges.n;i++){
        AncestryEdge *edge=edges.v+i;char parent[33],child[33];api_hex_id(&edge->parent,parent);api_hex_id(&edge->child,child);
        api_buf_fmt(&out,&on,&cap,"%s{\"parent\":\"%s\",\"child\":\"%s\",\"ordinal\":%zu,\"position\":%u,\"said\":%u,\"run\":%u,\"outcome\":%u}",i?",":"",parent,child,edge->ordinal,edge->vertex.position,edge->vertex.said,edge->vertex.run,edge->vertex.outcome);
    }
    api_buf_fmt(&out,&on,&cap,"]}");api_http(c,200,out);free(out);reader_free(reader);free(roots);lp_idmap_free(nodes);lp_vec_free(&front);lp_vec_free(&edges);
}

