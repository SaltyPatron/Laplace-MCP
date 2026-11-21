#define _GNU_SOURCE
#include "api_internal.h"

/* ---- ingest_status (Laplace-MCP#28): what the database holds and how current it is, read only. Each witness: its
 * name, trust, how many parts its trunk holds after its record, and readings, how many witnesses begin with the same record (a
 * source read again without superseding its earlier reading has more than one). Then the database's sessions that
 * are not idle, as pg_stat_activity shows them to this role, and the engine's per-source ingest logs
 * (LAPLACE_INGEST_LOGS, else $LAPLACE_WORK/logs/ingest), newest first, each with its last line, and the newest
 * Laplace-Operations ingest run (LAPLACE_INGEST_RUNS, else $LAPLACE_WORK/logs/ingest-runs), its steps with their
 * exit status. A log directory that is absent is reported as such, never as null. */
typedef struct { char name[256]; time_t mtime; long long bytes; } IsLog;
static int is_log_cmp(const void *a, const void *b) {
    const IsLog *x = a, *y = b;
    return x->mtime < y->mtime ? 1 : x->mtime > y->mtime ? -1 : strcmp(x->name, y->name);
}
static void is_logs(lp_buf *o, const char *dir, const char *unset, const char *want, int run);
void on_ingest_status(int c, const char *body) {
    char want[512] = "";
    api_jstr(body, "source", want, sizeof want);
    PGconn *pg = api_cc_db(c);
    if (!pg) return;
    uint64_t t0 = os_monotonic_ns();
    char err[512];
    PGresult *r = db_call_try(pg, LP_SQL_WITNESS_TRUST_ORDERED, NULL, err, sizeof err);
    if (!r) {
        api_reply_error(c, 500, "witness", err);
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
    lp_buf out = { 0 };
    lp_buf_cstr(&out, "{\"database\":");
    lp_json_str(&out, PQdb(pg), strlen(PQdb(pg)));
    lp_buf_cstr(&out, ",\"sources\":[");
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
        lp_buf_fmt(&out, "%s{\"id\":\"%s\",\"name\":", shown++ ? "," : "", idh);
        lp_json_str(&out, nm, strlen(nm));
        lp_buf_fmt(&out, ",\"trust\":%.3f,\"parts\":%zu%s,\"readings\":%d}", trust, nfiles[i] ? nfiles[i] - 1 : 0, nfiles[i] >= cap ? ",\"parts_cut\":true" : "", readings);
        free(nm);
    }
    PQclear(r);
    free(kid);
    free(recs);
    free(nfiles);
    reader_free(rd);
    lp_buf_cstr(&out, "],\"activity\":[");
    r = db_call_try(pg, LP_SQL_MCP_ACTIVITY, NULL, err, sizeof err);
    if (r) {
        for (int i = 0; i < PQntuples(r); i++) {
            const char *u = col_str(r, i, 1), *st = col_str(r, i, 2), *q = col_str(r, i, 4);
            lp_buf_fmt(&out, "%s{\"pid\":%d,\"user\":", i ? "," : "", col_i32(r, i, 0));
            lp_json_str(&out, u, strlen(u));
            lp_buf_cstr(&out, ",\"state\":");
            lp_json_str(&out, st, strlen(st));
            lp_buf_fmt(&out, ",\"seconds\":%" PRId64 ",\"query\":", col_i64(r, i, 3));
            lp_json_str(&out, q, strlen(q));
            lp_buf_byte(&out, '}');
        }
        PQclear(r);
    }
    lp_buf_cstr(&out, "],\"logs\":");
    const char *ld = getenv("LAPLACE_INGEST_LOGS"), *lr = getenv("LAPLACE_INGEST_RUNS"), *work = getenv("LAPLACE_WORK");
    char dir[4096] = "", runs[4096] = "";
    if (ld && *ld) snprintf(dir, sizeof dir, "%s", ld);
    else if (work && *work) snprintf(dir, sizeof dir, "%s/logs/ingest", work);
    if (lr && *lr) snprintf(runs, sizeof runs, "%s", lr);
    else if (work && *work) snprintf(runs, sizeof runs, "%s/logs/ingest-runs", work);
    is_logs(&out, dir, "LAPLACE_INGEST_LOGS", want, 0);
    /* Laplace-Operations ingest.sh: one directory per run, named by its UTC start (20261113T040506Z), each step's
     * output in <step>.log and its exit status in <step>.exit once it ends. The newest run is the greatest name. */
    lp_buf_cstr(&out, ",\"ingest_run\":");
    os_list names = { 0 };
    if (!runs[0]) lp_buf_cstr(&out, "{\"error\":\"no ingest run directory: neither LAPLACE_INGEST_RUNS nor LAPLACE_WORK is set\"}");
    else if (os_list_dir(runs, &names)) {
        lp_buf_cstr(&out, "{\"error\":\"no ingest run directory at ");
        lp_json_escape(&out, runs, strlen(runs));
        lp_buf_cstr(&out, "\"}");
    } else {
        const char *newest = NULL;
        for (size_t entry = 0; entry < names.n; entry++) {
            const char *name = names.item[entry];
            char fp[4400];
            if (strlen(name) != 16 || name[8] != 'T' || name[15] != 'Z' || (newest && strcmp(name, newest) <= 0)) continue;
            snprintf(fp, sizeof fp, "%s/%s", runs, name);
            if (os_is_dir(fp)) newest = name;
        }
        if (!newest) {
            lp_buf_cstr(&out, "{\"error\":\"no ingest run under ");
            lp_json_escape(&out, runs, strlen(runs));
            lp_buf_cstr(&out, "\"}");
        } else {
            char run[4400];
            snprintf(run, sizeof run, "%s/%s", runs, newest);
            is_logs(&out, run, NULL, want, 1);
        }
    }
    os_list_free(&names);
    lp_buf_fmt(&out, ",\"ms\":%.3f}", (double)(os_monotonic_ns() - t0) / 1e6);
    api_reply_buf(c, 200, &out);
}

/* The *.log files of one directory, newest first (20), each with its last line; a run's steps also carry the exit
 * status their .exit file holds, null while the step has not ended (or was cut off before it wrote one). A directory
 * that cannot be read says so instead of being left out. */
static pthread_mutex_t gm_mu = PTHREAD_MUTEX_INITIALIZER;
static void is_logs(lp_buf *o, const char *dir, const char *unset, const char *want, int run) {
    os_list names = { 0 };
    if (!dir[0]) {
        lp_buf_fmt(o, "{\"error\":\"no ingest log directory: neither %s nor LAPLACE_WORK is set\"}", unset);
        return;
    }
    if (os_list_dir(dir, &names)) {
        lp_buf_cstr(o, "{\"error\":\"no ingest log directory at ");
        lp_json_escape(o, dir, strlen(dir));
        lp_buf_cstr(o, "\"}");
        os_list_free(&names);
        return;
    }
    {
        lp_vec(IsLog) logs = { 0 };
        for (size_t entry = 0; entry < names.n; entry++) {
            const char *name = names.item[entry];
            size_t l = strlen(name);
            if (l < 5 || l >= 256 || strcmp(name + l - 4, ".log")) continue;
            char fp[4400];
            struct stat st;
            snprintf(fp, sizeof fp, "%s/%s", dir, name);
            if (stat(fp, &st) || ((st.st_mode & S_IFMT) != S_IFREG)) continue;
            IsLog x = { "", st.st_mtime, (long long)st.st_size };
            memcpy(x.name, name, l - 4);
            x.name[l - 4] = 0;
            if (want[0] && strcmp(x.name, want)) continue;
            lp_push(&logs, x);
        }
        qsort(logs.v, logs.n, sizeof *logs.v, is_log_cmp);
        lp_buf_cstr(o, "{\"dir\":");
        lp_json_str(o, dir, strlen(dir));
        lp_buf_fmt(o, ",\"%s\":[", run ? "steps" : "runs");
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
            pthread_mutex_lock(&gm_mu);
            struct tm *gmt = gmtime(&logs.v[k].mtime);
            if (gmt) tmv = *gmt; else memset(&tmv, 0, sizeof tmv);
            pthread_mutex_unlock(&gm_mu);
            strftime(when, sizeof when, "%Y-%m-%dT%H:%M:%SZ", &tmv);
            lp_buf_fmt(o, "%s{\"source\":", k ? "," : "");
            lp_json_str(o, logs.v[k].name, strlen(logs.v[k].name));
            lp_buf_fmt(o, ",\"modified\":\"%s\",\"bytes\":%lld,\"last\":", when, logs.v[k].bytes);
            lp_json_str(o, last, strlen(last));
            if (run) {
                char status[32] = "";
                snprintf(fp, sizeof fp, "%s/%s.exit", dir, logs.v[k].name);
                FILE *e = fopen(fp, "rb");
                if (e) { if (!fgets(status, sizeof status, e)) status[0] = 0; fclose(e); }
                char *end = NULL;
                long code = strtol(status, &end, 10);
                if (end != status) lp_buf_fmt(o, ",\"exit\":%ld", code);
                else lp_buf_cstr(o, ",\"exit\":null");
            }
            lp_buf_byte(o, '}');
        }
        lp_buf_cstr(o, "]}");
        lp_vec_free(&logs);
    }
    os_list_free(&names);
}

/* Readiness, apart from /health's liveness: the database this process reads answers a trivial query and holds the
 * laplace extension. 503 with the reason when it does not; authenticated like every route but /health. */
void on_ready(int c) {
    uint64_t t0 = os_monotonic_ns();
    PGconn *pg = api_cc_db(c);
    if (!pg) return;
    char err[512];
    PGresult *r = db_call_try(pg, LP_SQL_MCP_READY, NULL, err, sizeof err);
    if (!r || PQntuples(r) != 1) {
        const char *msg = r ? PQerrorMessage(pg) : err;
        lp_buf b = { 0 };
        lp_buf_cstr(&b, "{\"ok\":false,\"error\":\"query\",\"detail\":");
        lp_json_str(&b, msg, strlen(msg));
        lp_buf_byte(&b, '}');
        PQclear(r);
        api_reply_buf(c, 503, &b);
        return;
    }
    lp_buf out = { 0 };
    const char *db_name = col_str(r, 0, 0), *ext_ver = col_str(r, 0, 1);
    int ext = *ext_ver != 0;
    lp_buf_fmt(&out, "{\"ok\":%s,\"database\":", ext ? "true" : "false");
    lp_json_str(&out, db_name, strlen(db_name));
    if (ext) {
        lp_buf_cstr(&out, ",\"laplace\":");
        lp_json_str(&out, ext_ver, strlen(ext_ver));
    } else lp_buf_cstr(&out, ",\"error\":\"extension\",\"detail\":\"the database does not hold the laplace extension\"");
    lp_buf_fmt(&out, ",\"ms\":%.3f}", (double)(os_monotonic_ns() - t0) / 1e6);
    PQclear(r);
    api_reply_buf(c, ext ? 200 : 503, &out);
}

/* The sources the database holds: each witness, its name (the first part of its trunk, the record its source file
 * names) and its trust. */
void on_sources(int c, const char *body) {
    (void)body;
    PGconn *pg = api_db();
    if (!pg) { api_http(c, 503, "{\"error\":\"database\"}"); return; }
    char err[512];
    PGresult *r = db_call_try(pg, LP_SQL_WITNESS_TRUST_ORDERED, NULL, err, sizeof err);
    if (!r) {
        api_reply_error(c, 500, "witness", err);
        return;
    }
    Reader *rd = reader_new(pg);
    lp_buf out = { 0 };
    lp_buf_cstr(&out, "{\"database\":");
    lp_json_str(&out, PQdb(pg), strlen(PQdb(pg)));
    lp_buf_cstr(&out, ",\"sources\":[");
    for (int i = 0; i < PQntuples(r); i++) {
        lp_id id;
        memcpy(id.b, PQgetvalue(r, i, 0), 16);
        double trust;
        uint64_t tb = lp_be(PQgetvalue(r, i, 1), 8);
        memcpy(&trust, &tb, 8);
        lp_id first[2];
        size_t m = reader_parts(rd, &id, first, 2);
        char *nm = m ? reader_text(rd, &first[0], 200) : strdup("");
        char idh[33];
        api_hex_id(&id, idh);
        lp_buf_fmt(&out, "%s{\"id\":\"%s\",\"name\":", i ? "," : "", idh);
        lp_json_str(&out, nm, strlen(nm));
        lp_buf_fmt(&out, ",\"trust\":%.3f}", trust);
        free(nm);
    }
    lp_buf_cstr(&out, "]}");
    PQclear(r);
    reader_free(rd);
    api_reply_buf(c, 200, &out);
}

void on_catalog(int c,const char *body){
    (void)body;PGconn *pg=api_db();if(!pg){api_http(c,503,"{\"error\":\"database\"}");return;}
    char err[512];
    PGresult *r=db_call_try(pg,LP_SQL_MCP_CATALOG,NULL,err,sizeof err);
    if(!r||PQntuples(r)!=1){PQclear(r);api_http(c,503,"{\"error\":\"catalog_unavailable\"}");return;}
    char *json=strdup(col_str(r,0,0));PQclear(r);api_http(c,200,json);free(json);
}

/* Discover stored provenance trunks through the native kind bank, regardless
 * of registry membership. This is the library's entry collection, not a
 * fabricated list of sample queries. Cursor order is canonical identity. */
void on_library(int c,const char *body){
    char after[64]="";api_jstr(body,"after",after,sizeof after);
    if(*after&&!api_valid_id(after)){api_http(c,400,"{\"error\":\"after\"}");return;}
    PGconn *pg=api_db();if(!pg){api_http(c,503,"{\"error\":\"database\"}");return;}
    Args args={0};
    if(*after){lp_id cursor={0};lp_id_unhex(after,&cursor);arg_id(&args,&cursor);}else arg_null(&args);
    char err[512];
    PGresult *rows=db_call_try(pg,LP_SQL_LIBRARY_PAGE,&args,err,sizeof err);
    args_free(&args);
    if(!rows){api_http(c,503,"{\"error\":\"library_unavailable\"}");return;}
    int count=PQntuples(rows),more=count>64;if(more)count=64;
    Reader *reader=reader_new(pg);lp_vec(lp_id) names={0};
    for(int i=0;i<count;i++){lp_id id;memcpy(&id,PQgetvalue(rows,i,0),16);lp_push(&names,id);}
    reader_prefetch(reader,names.v,names.n,160);
    lp_buf out={0};
    lp_buf_cstr(&out,"{\"database\":");lp_json_str(&out,PQdb(pg),strlen(PQdb(pg)));
    lp_buf_cstr(&out,",\"collection\":\"stored-trunks\",\"scope\":\"native kind=file on physicality\",\"items\":[");
    for(int i=0;i<count;i++){
        lp_id id;memcpy(&id,PQgetvalue(rows,i,0),16);char hex[33];api_hex_id(&id,hex);
        lp_vertex first[2];size_t n=lp_path_vertices((const uint8_t *)PQgetvalue(rows,i,2),(size_t)PQgetlength(rows,i,2),first,2);
        int metadata=n>=2&&first[0].said==LP_SAID_METADATA;
        char *path=metadata?api_cw_file_name(reader,&id):strdup("");char *title=*path?strdup(path):reader_text(reader,metadata?&first[0].id:&id,160);
        lp_buf_fmt(&out,"%s{\"id\":\"%s\",\"tier\":%llu,\"kind\":\"%s\",\"title\":",i?",":"",hex,(unsigned long long)lp_be(PQgetvalue(rows,i,1),2),metadata?(*path?"file":"source"):"trunk");
        lp_json_str(&out,title,strlen(title));lp_buf_cstr(&out,",\"mask\":");
        lp_json_str(&out,PQgetvalue(rows,i,3),strlen(PQgetvalue(rows,i,3)));
        if(metadata){
            char meta[33],content[33];api_hex_id(&first[0].id,meta);api_hex_id(&first[1].id,content);
            lp_buf_fmt(&out,",\"metadata\":\"%s\",\"content\":\"%s\"",meta,content);
            if(!*path){
                lp_id parts[4096];size_t np=reader_parts(reader,&first[1].id,parts,4096);size_t shown=np>4096?4096:np;
                lp_buf_fmt(&out,",\"children_complete\":%s,\"children\":[",np<=4096?"true":"false");
                for(size_t k=0;k<shown;k++){char child[33];api_hex_id(parts+k,child);lp_buf_fmt(&out,"%s\"%s\"",k?",":"",child);}lp_buf_byte(&out,']');
            }
        }
        lp_buf_byte(&out,'}');free(path);free(title);
    }
    lp_buf_fmt(&out,"],\"has_more\":%s,\"next\":",more?"true":"false");
    if(more){char next[33];memcpy(names.v[0].b,PQgetvalue(rows,count-1,0),16);api_hex_id(names.v,next);lp_buf_fmt(&out,"\"%s\"",next);}else lp_buf_cstr(&out,"null");
    lp_buf_byte(&out,'}');PQclear(rows);reader_free(reader);lp_vec_free(&names);api_reply_buf(c,200,&out);
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
    lp_id_unhex(value,&start);
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
    lp_buf out={0};
    lp_buf_fmt(&out,"{\"id\":\"%s\",\"database\":",value);lp_json_str(&out,PQdb(pg),strlen(PQdb(pg)));
    lp_buf_fmt(&out,",\"complete\":%s,\"levels\":%d,\"limits\":{\"levels\":16,\"nodes\":512,\"holders_per_kind\":64},\"nodes\":[",complete?"true":"false",levels);
    for(size_t i=0;i<count;i++){
        AncestryNode *node=lp_idmap_at(nodes,i);char hex[33];api_hex_id(ids+i,hex);
        lp_id first[2];size_t n=node->metadata?reader_parts(reader,ids+i,first,2):0;
        char *file=node->metadata?api_cw_file_name(reader,ids+i):strdup("");
        char *label=*file?strdup(file):reader_text(reader,n?first:ids+i,160);
        lp_buf_fmt(&out,"%s{\"id\":\"%s\",\"tier\":%d,\"metadata_trunk\":%s,\"trunk_kind\":\"%s\",\"root\":%s,\"expanded\":%s,\"truncated\":%s,\"registered\":%s,\"trust\":%.6g,\"label\":",i?",":"",hex,node->tier,node->metadata?"true":"false",node->metadata?(*file?"file":"source"):"",node->expanded&&!node->parents&&!node->hub?"true":"false",node->expanded?"true":"false",node->hub?"true":"false",roots[i].witness?"true":"false",roots[i].trust);
        lp_json_str(&out,label,strlen(label));lp_buf_byte(&out,'}');free(label);
        free(file);
    }
    lp_buf_cstr(&out,"],\"edges\":[");
    for(size_t i=0;i<edges.n;i++){
        AncestryEdge *edge=edges.v+i;char parent[33],child[33];api_hex_id(&edge->parent,parent);api_hex_id(&edge->child,child);
        lp_buf_fmt(&out,"%s{\"parent\":\"%s\",\"child\":\"%s\",\"ordinal\":%zu,\"position\":%u,\"said\":%u,\"run\":%u,\"outcome\":%u}",i?",":"",parent,child,edge->ordinal,edge->vertex.position,edge->vertex.said,edge->vertex.run,edge->vertex.outcome);
    }
    lp_buf_cstr(&out,"]}");reader_free(reader);free(roots);lp_idmap_free(nodes);lp_vec_free(&front);lp_vec_free(&edges);
    api_reply_buf(c,200,&out);
}

