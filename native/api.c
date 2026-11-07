#define _GNU_SOURCE
#include "api_internal.h"

const lp_tier0_record *T0;
lp_text *TX;
PGconn *PG;
Lpm *CODE_IX;
const char *ENGINE = LPM_ENGINE_DEFAULT;
static const char *CONN_DEFAULT = LPM_CONNINFO_DEFAULT;
static const lp_highway *VIEW_HIGHWAY;
const lp_highway *api_view_highway(void) { if (!VIEW_HIGHWAY) VIEW_HIGHWAY = lp_highway_map(NULL); return VIEW_HIGHWAY; }

void api_engine_resolve(void) {
    const char *e = getenv("LAPLACE_ENGINE");
    if (e && *e) ENGINE = e;
}

uint64_t api_nsec(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

void api_hex_id(const lp_id *id, char out[33]) {
    static const char H[] = "0123456789abcdef";
    for (int i = 0; i < 16; i++) {
        out[i * 2] = H[id->b[i] >> 4];
        out[i * 2 + 1] = H[id->b[i] & 15];
    }
    out[32] = 0;
}

int api_valid_id(const char *s) {
    if (!s || strlen(s) != 32) return 0;
    for (int i = 0; i < 32; i++) {
        char c = s[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) return 0;
    }
    return 1;
}

PGconn *api_db(void) {
    const char *ci = getenv("LAPLACE_CONNINFO");
    if (!ci || !*ci) ci = CONN_DEFAULT;
    if(!PG || PQstatus(PG)!=CONNECTION_OK){
        if(PG){lpm_read_end();PQfinish(PG);}
        PG=PQconnectdb(ci);db_notices(PG);
    }
    if (PQstatus(PG) != CONNECTION_OK) return NULL;
    if(!db_read_snapshot(PG) && !db_read_begin(PG,NULL))return NULL;
    lpm_read_scope(PG,ci);
    return PG;
}

static int capture;
static int capture_code;
static char *capture_body;
static int stdio_capture;
static int stdio_code;
static char *stdio_body;

void api_http(int c, int code, const char *body) {
    lpm_read_end();
    if (capture) {
        capture_code = code;
        free(capture_body);
        capture_body = strdup(body ? body : "");
        return;
    }
    if (stdio_capture) {
        stdio_code = code;
        free(stdio_body);
        stdio_body = strdup(body ? body : "");
        return;
    }
    const char *why = code == 200 ? "OK" : code == 202 ? "Accepted" : code == 400 ? "Bad Request" : code == 401 ? "Unauthorized" : code == 404 ? "Not Found" : "Error";
    char hdr[256];
    int n = snprintf(hdr, sizeof hdr,
                     "HTTP/1.1 %d %s\r\nContent-Type: application/json\r\nContent-Length: %zu\r\nConnection: close\r\n\r\n",
                     code, why, strlen(body));
    send(c, hdr, n, MSG_NOSIGNAL);
    send(c, body, (int)strlen(body), MSG_NOSIGNAL);
}

char *api_json_esc(const char *s, char *out, size_t cap) {
    size_t n = 0;
    if (!s) s = "";
    for (const unsigned char *p = (const unsigned char *)s; *p && n + 6 < cap; p++) {
        if (*p == '"' || *p == '\\') {
            out[n++] = '\\';
            out[n++] = (char)*p;
        } else if (*p == '\n') {
            out[n++] = '\\';
            out[n++] = 'n';
        } else if (*p < 0x20) {
            n += (size_t)snprintf(out + n, cap - n, "\\u%04x", *p);
        } else out[n++] = (char)*p;
    }
    out[n] = 0;
    return out;
}

/* A field of this object only. Quoted content and nested objects cannot shadow
 * the transport envelope or another argument. Full JSON validation belongs to
 * the caller; this bounded reader selects the protocol's scalar fields. */
const char *api_jvalue(const char *body,const char *key){
    const char *p=body;while(isspace((unsigned char)*p))p++;if(*p++!='{')return NULL;
    while(*p){
        while(isspace((unsigned char)*p)||*p==',')p++;if(*p!='"')return NULL;
        const char *name=++p;while(*p&&*p!='"'){if(*p=='\\'&&p[1])p++;p++;}if(!*p)return NULL;
        size_t len=(size_t)(p-name);p++;while(isspace((unsigned char)*p))p++;if(*p++!=':')return NULL;
        while(isspace((unsigned char)*p))p++;
        if(strlen(key)==len&&!memcmp(name,key,len))return p;
        int depth=0,quoted=0;
        for(;*p;p++){
            if(quoted){if(*p=='\\'&&p[1])p++;else if(*p=='"')quoted=0;continue;}
            if(*p=='"'){quoted=1;continue;}if(*p=='{'||*p=='[')depth++;
            else if(*p=='}'||*p==']'){if(!depth)return NULL;depth--;}
            else if(*p==','&&!depth){p++;break;}
        }
    }
    return NULL;
}
static int jhex4(const char *p,uint32_t *value){
    *value=0;for(int i=0;i<4;i++){unsigned c=(unsigned char)p[i];if(!c)return 0;
        unsigned n=c>='0'&&c<='9'?c-'0':c>='a'&&c<='f'?c-'a'+10:c>='A'&&c<='F'?c-'A'+10:16;
        if(n>15)return 0;*value=(*value<<4)|n;}return 1;
}
/* JSON strings, including UTF-16 escape pairs, decoded as UTF-8 once. */
int api_jstr(const char *body, const char *key, char *out, size_t cap) {
    const char *p=api_jvalue(body,key);if(!p)return 0;
    if (*p != '"') return 0;
    p++;
    size_t n = 0;
    while (*p && *p != '"') {
        if(n+1>=cap)return 0;
        if (*p == '\\' && p[1]) {
            p++;
            if (*p == 'n') out[n++] = '\n';
            else if(*p=='r')out[n++]='\r';else if(*p=='t')out[n++]='\t';else if(*p=='b')out[n++]='\b';else if(*p=='f')out[n++]='\f';
            else if(*p=='u'){
                uint32_t cp;if(!jhex4(p+1,&cp))return 0;p+=4;
                if(cp>=0xd800&&cp<=0xdbff){uint32_t low;if(p[1]!='\\'||p[2]!='u'||!jhex4(p+3,&low)||low<0xdc00||low>0xdfff)return 0;cp=0x10000+((cp-0xd800)<<10)+low-0xdc00;p+=6;}
                else if(cp>=0xdc00&&cp<=0xdfff)return 0;
                if(!cp)return 0;
                if(cp<0x80)out[n++]=(char)cp;
                else if(cp<0x800){if(n+2>=cap)return 0;out[n++]=(char)(0xc0|(cp>>6));out[n++]=(char)(0x80|(cp&63));}
                else if(cp<0x10000){if(n+3>=cap)return 0;out[n++]=(char)(0xe0|(cp>>12));out[n++]=(char)(0x80|((cp>>6)&63));out[n++]=(char)(0x80|(cp&63));}
                else{if(n+4>=cap)return 0;out[n++]=(char)(0xf0|(cp>>18));out[n++]=(char)(0x80|((cp>>12)&63));out[n++]=(char)(0x80|((cp>>6)&63));out[n++]=(char)(0x80|(cp&63));}
            }
            else if(*p=='"'||*p=='\\'||*p=='/')out[n++]=*p;else return 0;
            p++;
        } else out[n++] = *p++;
    }
    if(*p!='"')return 0;
    out[n] = 0;
    return 1;
}

/* A flag: JSON true or false, or the strings "true" and "false". Absent or anything else: dflt. */
int api_jbool(const char *body, const char *key, int dflt) {
    const char *p=api_jvalue(body,key);if(!p)return dflt;
    if (!strncmp(p, "true", 4) || !strncmp(p, "\"true\"", 6)) return 1;
    if (!strncmp(p, "false", 5) || !strncmp(p, "\"false\"", 7)) return 0;
    return dflt;
}

int api_name_entity(const char *text, lp_ref *trunk, lp_ref *parts, size_t cap, size_t *nparts) {
    if (!TX) return 0;
    *trunk = lp_text_parts(TX, (const uint8_t *)text, strlen(text), parts, cap, nparts);
    return 1;
}

void api_parts_json(const lp_ref *parts, size_t n, char *out, size_t cap) {
    size_t u = 0;
    out[0] = 0;
    for (size_t i = 0; i < n && u + 40 < cap; i++) {
        char id[33];
        api_hex_id(&parts[i].id, id);
        u += (size_t)snprintf(out + u, cap - u, "%s\"%s\"", i ? "," : "", id);
    }
}

int api_q1(PGconn *pg, const char *sql, const char *a, char **out, char *err, size_t errcap) {
    const char *v[1] = { a };
    PGresult *r = db_exec_params(pg, sql, 1, NULL, v, NULL, NULL, 0);
    if (PQresultStatus(r) != PGRES_TUPLES_OK || PQntuples(r) < 1) {
        snprintf(err, errcap, "%s", PQerrorMessage(pg));
        PQclear(r);
        PQclear(db_exec(pg, "ROLLBACK"));
        return 0;
    }
    *out = strdup(PQgetvalue(r, 0, 0));
    PQclear(r);
    return *out != NULL;
}

int api_q2(PGconn *pg, const char *sql, const char *a, const char *b, char **out, char *err, size_t errcap) {
    const char *v[2] = { a, b };
    PGresult *r = db_exec_params(pg, sql, 2, NULL, v, NULL, NULL, 0);
    if (PQresultStatus(r) != PGRES_TUPLES_OK || PQntuples(r) < 1) {
        snprintf(err, errcap, "%s", PQerrorMessage(pg));
        PQclear(r);
        PQclear(db_exec(pg, "ROLLBACK"));
        return 0;
    }
    *out = strdup(PQgetvalue(r, 0, 0));
    PQclear(r);
    return *out != NULL;
}

int api_jint(const char *body, const char *key, int *out) {
    const char *p=api_jvalue(body,key);if(!p)return 0;
    if (*p == '"') return 0;
    char *end = NULL;
    long v = strtol(p, &end, 10);
    if (end == p) return 0;
    *out = (int)v;
    return 1;
}

int api_mask_claim(const char *m, int len) {
    if (len < 5) return 0;
    return ((const unsigned char *)m)[4] & 0x80;
}
/* PostgreSQL bit(64)'s binary carrier has a network-order length and MSB-first
 * bit positions. Decode transport bytes here; Native's manifest APIs own every
 * field's enum/flags semantics and canonical value identities. */
void api_emit_mask_values(char **out,size_t *n,size_t *cap,const char *wire,int len){
    const lp_highway *highway=api_view_highway();api_buf_fmt(out,n,cap,"[");
    if(highway&&len==12&&lp_be(wire,4)==64){
        lp_mask mask=0;const uint8_t *bytes=(const uint8_t *)wire+4;
        for(unsigned bit=0;bit<64;bit++)if(bytes[bit>>3]&(0x80u>>(bit&7)))mask|=UINT64_C(1)<<bit;
        int shown=0;
        for(size_t i=0;i<highway->nbanks;i++){
            const lp_bank *bank=highway->bank+i;if(!(mask&lp_mask_field(bank)))continue;
            api_buf_fmt(out,n,cap,"%s{\"bank\":\"%s\",\"carrier\":\"%s\",\"encoding\":\"%s\",\"values\":[",shown++?",":"",bank->name,bank->carrier,bank->flags?"flags":"enum");
            unsigned values=0;for(uint32_t slot=0;slot<bank->list->count;slot++)if(lp_mask_has(mask,bank,slot)){
                const lp_tier0_record *record=lp_highway_at(highway,bank->list,slot);if(!record)continue;
                char id[33];api_hex_id(&record->id,id);const char *label=lp_highway_identifier(highway,bank->list,slot);
                api_buf_fmt(out,n,cap,"%s{\"slot\":%u,\"id\":\"%s\",\"label\":\"",values++?",":"",slot,id);api_cw_str(out,n,cap,label?label:"");api_buf_fmt(out,n,cap,"\"}");
            }
            api_buf_fmt(out,n,cap,"]}");
        }
    }
    api_buf_fmt(out,n,cap,"]");
}

const char *API_FORWARD_SQL =
    "SELECT COALESCE((SELECT json_agg(x) FROM ("
    "  SELECT json_build_object('i', i, 'j', j, 'paths', paths, 'runs', runs,"
    "    'next', next::text, 'times', times) AS x"
    "  FROM @extschema@.laplace_forward($1::@extschema@.blake3[], 64) LIMIT 128) s), '[]'::json)::text";

const char *API_SHAPE_SQL =
    "SELECT @extschema@.laplace_frechet4d("
    " (SELECT @postgis@.ST_MakeLine(e.coord ORDER BY u.ord) FROM unnest(string_to_array($1, ',')::@extschema@.blake3[])"
    "    WITH ORDINALITY AS u(id, ord) JOIN @extschema@.entity e ON e.id OPERATOR(@extschema@.=) u.id),"
    " (SELECT @postgis@.ST_MakeLine(e.coord ORDER BY u.ord) FROM unnest(string_to_array($2, ',')::@extschema@.blake3[])"
    "    WITH ORDINALITY AS u(id, ord) JOIN @extschema@.entity e ON e.id OPERATOR(@extschema@.=) u.id)"
    ")::text";

int api_part_list(const lp_ref *parts, size_t n, char *out, size_t cap) {
    size_t u = 0;
    if (!n) return 0;
    for (size_t i = 0; i < n; i++) {
        char id[33];
        api_hex_id(&parts[i].id, id);
        int w = snprintf(out + u, cap - u, "%s%s", i ? "," : "", id);
        if (w < 0 || u + (size_t)w >= cap) return 0;
        u += (size_t)w;
    }
    return 1;
}

void api_coord_json(const lp_ref *r, char *out, size_t cap) {
    snprintf(out, cap, "[%.17g,%.17g,%.17g,%.17g]",
             (double)r->c.m[0] / LP_FIXED_ONE, (double)r->c.m[1] / LP_FIXED_ONE,
             (double)r->c.m[2] / LP_FIXED_ONE, (double)r->c.m[3] / LP_FIXED_ONE);
}

void api_unit_coord_json(const lp_ref *r, char *out, size_t cap) {
    double v[4] = {
        (double)r->c.m[0] / LP_FIXED_ONE,
        (double)r->c.m[1] / LP_FIXED_ONE,
        (double)r->c.m[2] / LP_FIXED_ONE,
        (double)r->c.m[3] / LP_FIXED_ONE,
    };
    double s2 = v[0] * v[0] + v[1] * v[1] + v[2] * v[2] + v[3] * v[3];
    if (s2 > 1e-30) {
        double inv = 1.0 / sqrt(s2);
        for (int d = 0; d < 4; d++) v[d] *= inv;
    } else {
        v[0] = 1.0; v[1] = 0.0; v[2] = 0.0; v[3] = 0.0;
    }
    snprintf(out, cap, "[%.17g,%.17g,%.17g,%.17g]", v[0], v[1], v[2], v[3]);
}

size_t api_parse_str_array(const char *body, const char *key, char **items, size_t max_items) {
    char pat[80];
    snprintf(pat, sizeof pat, "\"%s\"", key);
    const char *p = strstr(body, pat);
    if (!p) return 0;
    p += strlen(pat);
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
    if (*p != ':') return 0;
    p++;
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
    if (*p != '[') return 0;
    p++;
    size_t count = 0;
    while (*p && count < max_items) {
        while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r' || *p == ',') p++;
        if (*p == ']' || !*p) break;
        if (*p == '{') {
            const char *end = strchr(p, '}');
            if (!end) break;
            size_t len = (size_t)(end - p + 1);
            char *sub = malloc(len + 1);
            memcpy(sub, p, len);
            sub[len] = 0;
            char txt[16384] = "";
            if (api_jstr(sub, "text", txt, sizeof txt)) items[count++] = strdup(txt);
            free(sub);
            p = end + 1;
            continue;
        }
        if (*p != '"') break;
        p++;
        size_t cap = 4096, u = 0;
        char *s = malloc(cap);
        while (*p && *p != '"') {
            if (u + 4 >= cap) { cap *= 2; s = realloc(s, cap); }
            if (*p == '\\' && p[1]) {
                p++;
                if (*p == 'n') s[u++] = '\n';
                else if (*p == 'r') s[u++] = '\r';
                else if (*p == 't') s[u++] = '\t';
                else s[u++] = *p;
                p++;
            } else s[u++] = *p++;
        }
        s[u] = 0;
        if (*p == '"') p++;
        items[count++] = s;
    }
    return count;
}

void api_buf_add(char **b, size_t *n, size_t *cap, const char *s, size_t m) {
    if (*n + m + 1 > *cap) {
        size_t c = *cap ? *cap : 4096;
        while (*n + m + 1 > c) c *= 2;
        char *p = realloc(*b, c);
        if (!p) return;
        *b = p;
        *cap = c;
    }
    memcpy(*b + *n, s, m);
    *n += m;
    (*b)[*n] = 0;
}

void api_buf_fmt(char **b, size_t *n, size_t *cap, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    char tmp[1200];
    int w = vsnprintf(tmp, sizeof tmp, fmt, ap);
    va_end(ap);
    if (w < 0) return;
    if ((size_t)w < sizeof tmp) { api_buf_add(b, n, cap, tmp, (size_t)w); return; }
    char *p = malloc((size_t)w + 1);
    if (!p) return;
    va_start(ap, fmt);
    vsnprintf(p, (size_t)w + 1, fmt, ap);
    va_end(ap);
    api_buf_add(b, n, cap, p, (size_t)w);
    free(p);
}


int api_engine_run(char **av, char **outp, int *code, int timeout_s) {
#ifdef _WIN32
    (void)timeout_s;
    const char *tmpdir = getenv("TEMP");
    if (!tmpdir || !*tmpdir) tmpdir = getenv("TMP");
    if (!tmpdir || !*tmpdir) tmpdir = ".";
    char logpath[1024];
    snprintf(logpath, sizeof logpath, "%s/lpm-engine-%llu.log", tmpdir, (unsigned long long)api_nsec());
    int rc = os_run_logged(av[0], av, logpath, NULL, NULL);
    if (rc < 0) return 0;
    *code = rc;
    FILE *f = fopen(logpath, "rb");
    size_t cap = 1 << 22, n = 0;
    char *buf = malloc(cap);
    if (f && buf) {
        n = fread(buf, 1, cap - 1, f);
        fclose(f);
    }
    remove(logpath);
    if (!buf) return 0;
    buf[n] = '\0';
    *outp = buf;
    return 1;
#else
    int fds[2];
    if (pipe(fds) != 0) return 0;
    pid_t pid = fork();
    if (pid < 0) {
        close(fds[0]);
        close(fds[1]);
        return 0;
    }
    if (pid == 0) {
        setpgid(0, 0);
        dup2(fds[1], 1);
        dup2(fds[1], 2);
        close(fds[0]);
        close(fds[1]);
        execv(ENGINE, av);
        _exit(127);
    }
    setpgid(pid, pid);
    close(fds[1]);
    size_t cap = 1 << 22, n = 0;
    char *buf = malloc(cap);
    uint64_t deadline = timeout_s > 0 ? api_nsec() + (uint64_t)timeout_s * 1000000000ull : 0;
    int timed = 0;
    while (n + 1 < cap) {
        int left = -1;
        if (timeout_s > 0) {
            int64_t ms = ((int64_t)deadline - (int64_t)api_nsec()) / 1000000ll;
            if (ms <= 0) { timed = 1; break; }
            left = (int)ms;
        }
        struct pollfd p = { .fd = fds[0], .events = POLLIN };
        int w = poll(&p, 1, left);
        if (w == 0) { timed = 1; break; }
        if (w < 0) break;
        ssize_t r = read(fds[0], buf + n, cap - 1 - n);
        if (r <= 0) break;
        n += (size_t)r;
    }
    close(fds[0]);
    buf[n] = '\0';
    if (timed) {
        kill(-pid, SIGTERM);
        snprintf(buf + n, cap - n, "\n[stopped after %d s]\n", timeout_s);
    }
    int st = 0;
    waitpid(pid, &st, 0);
    *code = timed ? 124 : (WIFEXITED(st) ? WEXITSTATUS(st) : 128);
    *outp = buf;
    return 1;
#endif
}

void on_ingest(int c, const char *body) {
    char path[4096];
    if (!api_jstr(body, "path", path, sizeof path) || !(path[0] == '/' || (path[0] && path[1] == ':'))) {
        api_http(c, 400, "{\"error\":\"path\"}");
        return;
    }
    if (!os_exists(path)) {
        api_http(c, 400, "{\"error\":\"path not found\"}");
        return;
    }
    char *av[] = { (char *)ENGINE, "ingest", path, NULL };
    char *log = NULL;
    int code = 127;
    if (!api_engine_run(av, &log, &code, 0)) {
        api_http(c, 500, "{\"error\":\"engine\"}");
        return;
    }
    size_t elen = strlen(log) * 6 + 64;
    char *esc = malloc(elen);
    api_json_esc(log, esc, elen);
    char *out = malloc(strlen(esc) + 128);
    snprintf(out, strlen(esc) + 128, "{\"exit\":%d,\"engine\":\"laplace ingest\",\"log\":\"%s\"}", code, esc);
    free(esc);
    free(log);
    api_http(c, code == 0 ? 200 : 500, out);
    free(out);
}

int api_plain_arg(const char *s) {
    return s && s[0] && s[0] != '-' && strlen(s) < 4096;
}

void api_on_engine(int c, char **av) {
    char *log = NULL;
    int code = 127;
    if (!api_engine_run(av, &log, &code, 0)) {
        api_http(c, 500, "{\"error\":\"engine\"}");
        return;
    }
    char *esc = malloc(strlen(log) * 6 + 64);
    api_json_esc(log, esc, strlen(log) * 6 + 64);
    char *out = malloc(strlen(esc) + 128);
    snprintf(out, strlen(esc) + 128, "{\"exit\":%d,\"log\":\"%s\"}", code, esc);
    free(esc);
    free(log);
    api_http(c, code == 0 ? 200 : 500, out);
    free(out);
}

void on_hop(int c, const char *body) {
    char text[8192];
    if (!api_jstr(body, "input", text, sizeof text) || !api_plain_arg(text)) {
        api_http(c, 400, "{\"error\":\"input\"}");
        return;
    }
    char *av[] = { (char *)ENGINE, "hop", text, NULL };
    api_on_engine(c, av);
}

void on_text(int c, const char *body) {
    char text[8192];
    if (!api_jstr(body, "input", text, sizeof text) || !api_plain_arg(text)) {
        api_http(c, 400, "{\"error\":\"input\"}");
        return;
    }
    char *av[] = { (char *)ENGINE, "text", text, NULL };
    api_on_engine(c, av);
}

void on_translate(int c, const char *body) {
    char word[512], from[32], to[32];
    if (!api_jstr(body, "input", word, sizeof word) || !api_jstr(body, "from", from, sizeof from) ||
        !api_jstr(body, "to", to, sizeof to) || !api_plain_arg(word) || !api_plain_arg(from) || !api_plain_arg(to)) {
        api_http(c, 400, "{\"error\":\"input, from, to\"}");
        return;
    }
    char *av[] = { (char *)ENGINE, "translate", word, from, to, NULL };
    api_on_engine(c, av);
}

void on_pull(int c, const char *body) {
    char text[8192];
    if (!api_jstr(body, "input", text, sizeof text) || !api_plain_arg(text)) {
        api_http(c, 400, "{\"error\":\"input\"}");
        return;
    }
    char *av[] = { (char *)ENGINE, "pull", text, NULL };
    api_on_engine(c, av);
}

void on_degrees(int c, const char *body) {
    char a[8192], b[8192];
    if (!api_jstr(body, "input", a, sizeof a) || !api_jstr(body, "other", b, sizeof b) || !api_plain_arg(a) || !api_plain_arg(b)) {
        api_http(c, 400, "{\"error\":\"input and other\"}");
        return;
    }
    char *av[] = { (char *)ENGINE, "degrees", a, b, NULL };
    api_on_engine(c, av);
}



void api_capture_begin(int stdio_mode) {
    if (stdio_mode) {
        stdio_capture = 1;
        stdio_code = 500;
        free(stdio_body);
        stdio_body = NULL;
    } else {
        capture = 1;
        capture_code = 500;
        free(capture_body);
        capture_body = NULL;
    }
}

const char *api_capture_end(int stdio_mode, int *code_out) {
    if (stdio_mode) {
        stdio_capture = 0;
        if (code_out) *code_out = stdio_code;
        return stdio_body ? stdio_body : "";
    } else {
        capture = 0;
        if (code_out) *code_out = capture_code;
        return capture_body ? capture_body : "";
    }
}

/* ---- who may call the listener. Every request but GET /health carries Authorization: Bearer TOKEN, the one the
 * installation made for this service (Laplace-Operations: /etc/laplace/secrets/modular-mcp.env, made once, as the
 * monorepo MCP's mcp.env is). The token is LAPLACE_MCP_TOKEN, else the LAPLACE_MCP_TOKEN line of the file
 * LAPLACE_MCP_TOKEN_FILE names (or its first line). With no token of at least 32 characters and no whitespace the
 * listener does not start: it fails closed. stdio (lpm mcp) is the calling user's own process and takes none. */
static char SERVE_TOKEN[513];
static size_t SERVE_TOKEN_N;
static int token_take(const char *v, size_t n) {
    while (n && (v[n - 1] == '\n' || v[n - 1] == '\r')) n--;
    if (n >= 2 && (v[0] == '"' || v[0] == '\'') && v[n - 1] == v[0]) { v++; n -= 2; }
    if (n < 32 || n >= sizeof SERVE_TOKEN) return 0;
    for (size_t i = 0; i < n; i++) if ((unsigned char)v[i] <= ' ' || v[i] == 0x7f) return 0;
    memcpy(SERVE_TOKEN, v, n);
    SERVE_TOKEN[n] = 0;
    SERVE_TOKEN_N = n;
    return 1;
}
static int serve_token_load(void) {
    const char *e = getenv("LAPLACE_MCP_TOKEN");
    if (e && *e) return token_take(e, strlen(e));
    const char *f = getenv("LAPLACE_MCP_TOKEN_FILE");
    if (!f || !*f) return 0;
    FILE *fp = fopen(f, "rb");
    if (!fp) { perror(f); return 0; }
    char line[1024], first[1024] = "";
    int ok = 0, lines = 0;
    while (fgets(line, sizeof line, fp)) {
        if (!lines++) snprintf(first, sizeof first, "%s", line);
        if (!strncmp(line, "LAPLACE_MCP_TOKEN=", 18)) { ok = token_take(line + 18, strlen(line + 18)); break; }
    }
    fclose(fp);
    if (!ok && lines && !strchr(first, '=')) ok = token_take(first, strlen(first));
    memset(line, 0, sizeof line);
    memset(first, 0, sizeof first);
    return ok;
}
/* The request's bearer token against the service's, in time that does not depend on where they differ. */
static int request_authorized(const char *req, const char *head_end) {
    const char *p = req;
    while ((p = strstr(p, "\r\n")) && p < head_end) {
        p += 2;
        if (strncasecmp(p, "Authorization:", 14)) continue;
        const char *v = p + 14;
        while (*v == ' ' || *v == '\t') v++;
        if (strncasecmp(v, "Bearer ", 7)) return 0;
        v += 7;
        while (*v == ' ') v++;
        size_t n = strcspn(v, "\r\n");
        while (n && (v[n - 1] == ' ' || v[n - 1] == '\t')) n--;
        unsigned d = n != SERVE_TOKEN_N;
        for (size_t i = 0; i < SERVE_TOKEN_N; i++) d |= (unsigned)(unsigned char)SERVE_TOKEN[i] ^ (unsigned)(unsigned char)(i < n ? v[i] : 0);
        return d == 0 && SERVE_TOKEN_N > 0;
    }
    return 0;
}

int api_serve(const char *host, int port) {
    if (!serve_token_load()) {
        fprintf(stderr, "lpm serve: no token: set LAPLACE_MCP_TOKEN, or LAPLACE_MCP_TOKEN_FILE to a file with its "
                        "LAPLACE_MCP_TOKEN line (at least 32 characters, no whitespace); refusing to listen unauthenticated\n");
        return 2;
    }
#ifdef _WIN32
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return 1;
#endif
    api_engine_resolve();
    T0 = lp_tier0_map(NULL);
    if (!T0) {
        fprintf(stderr, "tier 0 did not map\n");
        return 1;
    }
    TX = lp_text_new(T0);
    if (!TX) return 1;
    int s = (int)socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) return 1;
    int one = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (const char *)&one, sizeof one);
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_port = htons((uint16_t)port);
    if (inet_pton(AF_INET, host, &a.sin_addr) != 1) return 1;
    if (bind(s, (struct sockaddr *)&a, sizeof a) != 0) {
        perror("bind");
        return 1;
    }
    if (listen(s, 64) != 0) return 1;
    fprintf(stderr, "Laplace-MCP %s:%d\n", host, port);
    for (;;) {
        int c = (int)accept(s, NULL, NULL);
        if (c < 0) continue;
        char *req = malloc(1 << 20);
        if (!req) { close_sock(c); continue; }
        size_t got = 0, rcap = 1 << 20;
        while (got + 1 < rcap) {
            ssize_t r = recv(c, req + got, (int)(rcap - 1 - got), 0);
            if (r <= 0) break;
            got += (size_t)r;
            req[got] = 0;
            char *hb = strstr(req, "\r\n\r\n");
            if (!hb) continue;
            long need = 0;
            const char *cl = strstr(req, "Content-Length:");
            if (!cl) cl = strstr(req, "content-length:");
            if (cl) need = strtol(cl + 15, NULL, 10);
            if ((long)(got - (size_t)(hb + 4 - req)) >= need) break;
        }
        req[got] = 0;
        char method[16] = "GET", path[256] = "/";
        sscanf(req, "%15s %255s", method, path);
        char *q = strchr(path, '?');
        if (q) *q = 0;
        char *body = strstr(req, "\r\n\r\n");
        int open_route = !strcmp(method, "GET") && !strcmp(path, "/health");
        if (!open_route && (!body || !request_authorized(req, body))) {
            static const char deny[] = "HTTP/1.1 401 Unauthorized\r\nContent-Type: application/json\r\nWWW-Authenticate: Bearer\r\n"
                                       "Content-Length: 24\r\nConnection: close\r\n\r\n{\"error\":\"unauthorized\"}";
            send(c, deny, (int)strlen(deny), MSG_NOSIGNAL);
            free(req);
            close_sock(c);
            continue;
        }
        body = body ? body + 4 : "";
        api_dispatch(c, method, path, body);
        free(req);
        close_sock(c);
    }
}
