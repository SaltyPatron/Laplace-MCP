#include "semantic.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#  include <winsock2.h>
#  include <ws2tcpip.h>
typedef SOCKET sock_t;
#  define BAD_SOCK INVALID_SOCKET
#  define sock_close closesocket
#else
#  include <netdb.h>
#  include <sys/socket.h>
#  include <sys/time.h>
#  include <unistd.h>
typedef int sock_t;
#  define BAD_SOCK (-1)
#  define sock_close close
#endif
#ifndef MSG_NOSIGNAL
#  define MSG_NOSIGNAL 0
#endif

#define CODE_PREFIX "Find the most relevant code snippet given the following query:\n"
#define TEXT_PREFIX "Instruct: Given a question, retrieve passages that answer it\nQuery: "

typedef struct { char *p; size_t n, cap; } Buf;

static int buf_put(Buf *b, const void *s, size_t n) {
    if (b->n + n + 1 > b->cap) {
        size_t cap = b->cap ? b->cap * 2 : 4096;
        while (cap < b->n + n + 1) cap *= 2;
        char *p = realloc(b->p, cap);
        if (!p) return -1;
        b->p = p; b->cap = cap;
    }
    memcpy(b->p + b->n, s, n);
    b->n += n;
    b->p[b->n] = 0;
    return 0;
}
static int buf_str(Buf *b, const char *s) { return buf_put(b, s, strlen(s)); }

static int buf_json(Buf *b, const char *s) {
    if (buf_put(b, "\"", 1)) return -1;
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        char e[8];
        if (c == '"' || c == '\\') { e[0] = '\\'; e[1] = (char)c; if (buf_put(b, e, 2)) return -1; }
        else if (c == '\n') { if (buf_put(b, "\\n", 2)) return -1; }
        else if (c == '\r') { if (buf_put(b, "\\r", 2)) return -1; }
        else if (c == '\t') { if (buf_put(b, "\\t", 2)) return -1; }
        else if (c < 0x20) { snprintf(e, sizeof e, "\\u%04x", c); if (buf_put(b, e, 6)) return -1; }
        else if (buf_put(b, s, 1)) return -1;
    }
    return buf_put(b, "\"", 1);
}

static void fail(char *err, size_t cap, const char *fmt, const char *a, const char *b) {
    if (err && cap) snprintf(err, cap, fmt, a ? a : "", b ? b : "");
}

/* --- HTTP/1.1, one request per connection ------------------------------------------------------------------- */

static int http_post(const char *url, const char *auth_header, const char *body, Buf *resp, int *status,
                     char *err, size_t errcap) {
    char host[256], port[16] = "80", path[1024] = "/";
    if (strncmp(url, "http://", 7)) { fail(err, errcap, "%s: only http:// URLs are supported", url, NULL); return -1; }
    const char *h = url + 7, *slash = strchr(h, '/'), *end = slash ? slash : h + strlen(h);
    const char *colon = memchr(h, ':', (size_t)(end - h));
    size_t hl = (size_t)((colon ? colon : end) - h);
    if (hl == 0 || hl >= sizeof host) { fail(err, errcap, "%s: bad host", url, NULL); return -1; }
    memcpy(host, h, hl); host[hl] = 0;
    if (colon) {
        size_t pl = (size_t)(end - colon - 1);
        if (pl == 0 || pl >= sizeof port) { fail(err, errcap, "%s: bad port", url, NULL); return -1; }
        memcpy(port, colon + 1, pl); port[pl] = 0;
    }
    if (slash) snprintf(path, sizeof path, "%s", slash);

#ifdef _WIN32
    static int wsa;
    if (!wsa) { WSADATA w; if (WSAStartup(MAKEWORD(2, 2), &w)) { fail(err, errcap, "WSAStartup failed", NULL, NULL); return -1; } wsa = 1; }
#endif
    struct addrinfo hints = {0}, *ai = NULL;
    hints.ai_family = AF_UNSPEC; hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host, port, &hints, &ai) || !ai) { fail(err, errcap, "%s: cannot resolve %s", url, host); return -1; }
    sock_t s = BAD_SOCK;
    for (struct addrinfo *a = ai; a; a = a->ai_next) {
        s = socket(a->ai_family, a->ai_socktype, a->ai_protocol);
        if (s == BAD_SOCK) continue;
#ifdef _WIN32
        DWORD tmo = 60000;
        setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char *)&tmo, sizeof tmo);
        setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (const char *)&tmo, sizeof tmo);
#else
        struct timeval tmo = { 60, 0 };
        setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tmo, sizeof tmo);
        setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tmo, sizeof tmo);
#endif
        if (connect(s, a->ai_addr, (int)a->ai_addrlen) == 0) break;
        sock_close(s); s = BAD_SOCK;
    }
    freeaddrinfo(ai);
    if (s == BAD_SOCK) { fail(err, errcap, "%s: connection refused or unreachable (%s)", url, host); return -1; }

    Buf req = {0};
    char line[1400];
    size_t blen = strlen(body);
    snprintf(line, sizeof line, "POST %s HTTP/1.1\r\nHost: %s:%s\r\nContent-Type: application/json\r\n"
             "Content-Length: %zu\r\nConnection: close\r\n", path, host, port, blen);
    int bad = buf_str(&req, line);
    if (auth_header && *auth_header) bad |= buf_str(&req, auth_header) | buf_str(&req, "\r\n");
    bad |= buf_str(&req, "\r\n") | buf_put(&req, body, blen);
    if (bad) { free(req.p); sock_close(s); fail(err, errcap, "out of memory", NULL, NULL); return -1; }
    for (size_t off = 0; off < req.n;) {
        int k = (int)send(s, req.p + off, (int)(req.n - off), MSG_NOSIGNAL);
        if (k <= 0) { free(req.p); sock_close(s); fail(err, errcap, "%s: send failed", url, NULL); return -1; }
        off += (size_t)k;
    }
    free(req.p);
    Buf raw = {0};
    char chunk[16384];
    for (;;) {
        int k = (int)recv(s, chunk, sizeof chunk, 0);
        if (k < 0) { free(raw.p); sock_close(s); fail(err, errcap, "%s: no response (timeout or reset)", url, NULL); return -1; }
        if (k == 0) break;
        if (buf_put(&raw, chunk, (size_t)k)) { free(raw.p); sock_close(s); fail(err, errcap, "out of memory", NULL, NULL); return -1; }
    }
    sock_close(s);
    char *hdr_end = raw.p ? strstr(raw.p, "\r\n\r\n") : NULL;
    if (!raw.p || strncmp(raw.p, "HTTP/1.", 7) || !hdr_end) { free(raw.p); fail(err, errcap, "%s: not an HTTP response", url, NULL); return -1; }
    *status = atoi(raw.p + 9);
    *hdr_end = 0;
    int chunked = 0;
    for (char *q = raw.p; *q; q++) {
        if (q[0] == '\n' && !strncmp(q + 1, "Transfer-Encoding:", 18)) chunked = strstr(q, "chunked") != NULL;
        if (q[0] == '\n' && (q[1] == 't' || q[1] == 'T') && !strncmp(q + 2, "ransfer-encoding:", 17)) chunked = strstr(q, "chunked") != NULL;
    }
    const char *b = hdr_end + 4, *bend = raw.p + raw.n;
    resp->n = 0;
    if (!chunked) buf_put(resp, b, (size_t)(bend - b));
    else
        while (b < bend) {
            char *e;
            unsigned long len = strtoul(b, &e, 16);
            if (e == b || !(e = strstr(e, "\r\n"))) break;
            b = e + 2;
            if (len == 0 || b + len > bend) break;
            buf_put(resp, b, len);
            b += len + 2;
        }
    if (!resp->p) buf_put(resp, "", 0);
    free(raw.p);
    return 0;
}

/* --- JSON reading: just enough for these two responses ------------------------------------------------------ */

static void put_utf8(char **o, char *end, unsigned cp) {
    char t[4]; int n;
    if (cp < 0x80) { t[0] = (char)cp; n = 1; }
    else if (cp < 0x800) { t[0] = (char)(0xC0 | cp >> 6); t[1] = (char)(0x80 | (cp & 63)); n = 2; }
    else if (cp < 0x10000) { t[0] = (char)(0xE0 | cp >> 12); t[1] = (char)(0x80 | (cp >> 6 & 63)); t[2] = (char)(0x80 | (cp & 63)); n = 3; }
    else { t[0] = (char)(0xF0 | cp >> 18); t[1] = (char)(0x80 | (cp >> 12 & 63)); t[2] = (char)(0x80 | (cp >> 6 & 63)); t[3] = (char)(0x80 | (cp & 63)); n = 4; }
    if (*o + n < end) { memcpy(*o, t, (size_t)n); *o += n; }
}

static unsigned hex4(const char *s) {
    unsigned v = 0;
    for (int i = 0; i < 4; i++) {
        char c = s[i];
        if (!isxdigit((unsigned char)c)) return 0xFFFFFFFFu;
        v = v * 16 + (unsigned)(isdigit((unsigned char)c) ? c - '0' : (tolower((unsigned char)c) - 'a' + 10));
    }
    return v;
}

const char *sem_json_string(const char *s, char *out, size_t cap) {
    char *o = out, *end = out + (cap ? cap - 1 : 0);
    for (; *s && *s != '"'; s++) {
        if (*s != '\\') { if (o < end) *o++ = *s; continue; }
        s++;
        switch (*s) {
        case 'n': if (o < end) *o++ = '\n'; break;
        case 't': if (o < end) *o++ = '\t'; break;
        case 'r': if (o < end) *o++ = '\r'; break;
        case 'b': if (o < end) *o++ = '\b'; break;
        case 'f': if (o < end) *o++ = '\f'; break;
        case 'u': {
            unsigned cp = hex4(s + 1);
            if (cp == 0xFFFFFFFFu) return NULL;
            s += 4;
            if (cp >= 0xD800 && cp < 0xDC00 && s[1] == '\\' && s[2] == 'u') {
                unsigned lo = hex4(s + 3);
                if (lo >= 0xDC00 && lo < 0xE000) { cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00); s += 6; }
            }
            put_utf8(&o, end, cp);
            break;
        }
        case 0: return NULL;
        default: if (o < end) *o++ = *s;
        }
    }
    if (cap) *o = 0;
    return *s == '"' ? s + 1 : NULL;
}

static const char *skip_string(const char *s) {   /* s just after the opening quote */
    for (; *s && *s != '"'; s++) if (*s == '\\' && s[1]) s++;
    return *s ? s + 1 : s;
}

/* End of the JSON value (object or array) that opens at s. */
static const char *value_end(const char *s) {
    int depth = 0;
    for (; *s; s++) {
        if (*s == '"') { s = skip_string(s + 1) - 1; continue; }
        if (*s == '{' || *s == '[') depth++;
        else if (*s == '}' || *s == ']') { if (--depth == 0) return s + 1; }
    }
    return s;
}

/* The value of "key" directly inside the object [s, e), skipping nested values. */
static const char *member(const char *s, const char *e, const char *key) {
    size_t kl = strlen(key);
    if (s < e && *s == '{') s++;
    while (s < e) {
        while (s < e && (isspace((unsigned char)*s) || *s == ',')) s++;
        if (s >= e || *s != '"') return NULL;
        const char *k = s + 1, *ke = skip_string(k);
        int hit = (size_t)(ke - 1 - k) == kl && !memcmp(k, key, kl);
        s = ke;
        while (s < e && (isspace((unsigned char)*s) || *s == ':')) s++;
        if (hit) return s;
        if (*s == '"') s = skip_string(s + 1);
        else if (*s == '{' || *s == '[') s = value_end(s);
        else while (s < e && *s != ',' && *s != '}') s++;
    }
    return NULL;
}

static char *error_text(const Buf *b) {   /* a short, printable piece of an error body */
    static char t[300];
    size_t n = b->n < sizeof t - 1 ? b->n : sizeof t - 1;
    for (size_t i = 0; i < n; i++) t[i] = (b->p[i] == '\n' || b->p[i] == '\r') ? ' ' : b->p[i];
    t[n] = 0;
    return t;
}

/* --- embeddings and Qdrant ---------------------------------------------------------------------------------- */

static int embed(const char *url, const char *key, const char *text, float **vec, size_t *dim, char *err, size_t errcap) {
    Buf body = {0}, resp = {0};
    char auth[512] = "";
    if (key && *key) snprintf(auth, sizeof auth, "Authorization: Bearer %s", key);
    buf_str(&body, "{\"input\":[");
    buf_json(&body, text);
    buf_str(&body, "]}");
    int status = 0;
    int rc = http_post(url, auth, body.p, &resp, &status, err, errcap);
    free(body.p);
    if (rc) { free(resp.p); return -1; }
    if (status != 200) {
        char st[16]; snprintf(st, sizeof st, "%d", status);
        fail(err, errcap, "embedding %s: HTTP %s", url, st);
        size_t l = strlen(err); snprintf(err + l, errcap - l, ": %s", error_text(&resp));
        free(resp.p); return -1;
    }
    const char *e = resp.p ? strstr(resp.p, "\"embedding\"") : NULL;
    e = e ? strchr(e, '[') : NULL;
    if (!e) { fail(err, errcap, "embedding %s: response has no embedding", url, NULL); free(resp.p); return -1; }
    size_t cap = 2048, n = 0;
    float *v = malloc(cap * sizeof *v);
    for (const char *p = e + 1; v && *p && *p != ']';) {
        char *q;
        double x = strtod(p, &q);
        if (q == p) { p++; continue; }
        if (n == cap) { float *w = realloc(v, (cap *= 2) * sizeof *v); if (!w) { free(v); v = NULL; break; } v = w; }
        v[n++] = (float)x;
        p = q;
    }
    free(resp.p);
    if (!v || n == 0) { free(v); fail(err, errcap, "embedding %s: empty vector", url, NULL); return -1; }
    *vec = v; *dim = n;
    return 0;
}

static int qdrant(const char *base, const char *key, const char *coll, const float *v, size_t dim, size_t limit,
                  SemHit **hits, size_t *nh, size_t *cap, char *err, size_t errcap) {
    char url[1200], auth[512] = "", num[48];
    snprintf(url, sizeof url, "%s/collections/%s/points/search", base, coll);
    if (key && *key) snprintf(auth, sizeof auth, "api-key: %s", key);
    Buf body = {0}, resp = {0};
    buf_str(&body, "{\"vector\":[");
    for (size_t i = 0; i < dim; i++) { snprintf(num, sizeof num, i ? ",%.9g" : "%.9g", v[i]); buf_str(&body, num); }
    snprintf(num, sizeof num, "],\"limit\":%zu,\"with_payload\":true}", limit);
    buf_str(&body, num);
    int status = 0;
    int rc = http_post(url, auth, body.p, &resp, &status, err, errcap);
    free(body.p);
    if (rc) { free(resp.p); return -1; }
    if (status != 200) {
        char st[16]; snprintf(st, sizeof st, "%d", status);
        fail(err, errcap, "qdrant %s: HTTP %s", url, st);
        size_t l = strlen(err); snprintf(err + l, errcap - l, ": %s", error_text(&resp));
        free(resp.p); return -1;
    }
    const char *all = resp.p, *end = resp.p + resp.n;
    const char *res = member(all, end, "result");
    if (!res || *res != '[') { fail(err, errcap, "qdrant %s: response has no result", url, NULL); free(resp.p); return -1; }
    const char *p = res + 1, *rend = value_end(res);
    while (p < rend) {
        while (p < rend && *p != '{' && *p != ']') p++;
        if (p >= rend || *p == ']') break;
        const char *oe = value_end(p);
        const char *sc = member(p, oe, "score"), *pl = member(p, oe, "payload");
        if (sc && pl && *pl == '{') {
            const char *ple = value_end(pl);
            const char *path = member(pl, ple, "path"), *text = member(pl, ple, "text");
            const char *st = member(pl, ple, "start"), *en = member(pl, ple, "end");
            if (path && *path == '"' && text && *text == '"' && st && en) {
                if (*nh == *cap) {
                    size_t nc = *cap ? *cap * 2 : 64;
                    SemHit *w = realloc(*hits, nc * sizeof *w);
                    if (!w) { fail(err, errcap, "out of memory", NULL, NULL); free(resp.p); return -1; }
                    *hits = w; *cap = nc;
                }
                SemHit *h = &(*hits)[*nh];
                memset(h, 0, sizeof *h);
                sem_json_string(path + 1, h->path, sizeof h->path);
                snprintf(h->collection, sizeof h->collection, "%s", coll);
                size_t tl = (size_t)(skip_string(text + 1) - text);
                h->text = malloc(tl + 1);
                if (!h->text) { fail(err, errcap, "out of memory", NULL, NULL); free(resp.p); return -1; }
                sem_json_string(text + 1, h->text, tl + 1);
                h->cosine = strtod(sc, NULL);
                h->line0 = (uint32_t)strtoul(st, NULL, 10);
                h->line1 = (uint32_t)strtoul(en, NULL, 10);
                (*nh)++;
            }
        }
        p = oe;
    }
    free(resp.p);
    return 0;
}

int sem_enabled(void) {
    const char *e = getenv("LAPLACE_CODE_SEMANTIC");
    return e && *e && strcmp(e, "0");
}

static const char *need(const char *name, char *err, size_t errcap) {
    const char *v = getenv(name);
    if (!v || !*v) fail(err, errcap, "LAPLACE_CODE_SEMANTIC is set but %s is not", name, NULL);
    return v && *v ? v : NULL;
}

/* One group: an embedding endpoint and the collections whose vectors it made. */
static int group(const char *query, const char *url, const char *key, const char *prefix, const char *colls,
                 const char *qd, const char *qkey, size_t limit, SemHit **hits, size_t *nh, size_t *cap,
                 char *err, size_t errcap) {
    size_t ql = strlen(prefix) + strlen(query) + 1;
    char *q = malloc(ql);
    if (!q) { fail(err, errcap, "out of memory", NULL, NULL); return -1; }
    snprintf(q, ql, "%s%s", prefix, query);
    float *v = NULL; size_t dim = 0;
    int rc = embed(url, key, q, &v, &dim, err, errcap);
    free(q);
    if (rc) return -1;
    char name[128];
    for (const char *c = colls; *c;) {
        const char *e = strchr(c, ',');
        size_t l = e ? (size_t)(e - c) : strlen(c);
        while (l && isspace((unsigned char)*c)) { c++; l--; }
        while (l && isspace((unsigned char)c[l - 1])) l--;
        if (l && l < sizeof name) {
            memcpy(name, c, l); name[l] = 0;
            if (qdrant(qd, qkey, name, v, dim, limit, hits, nh, cap, err, errcap)) { free(v); return -1; }
        }
        if (!e) break;
        c = e + 1;
    }
    free(v);
    return 0;
}

int sem_search(const char *query, size_t per_collection, SemHit **out, size_t *n, char *err, size_t errcap) {
    *out = NULL; *n = 0;
    const char *qd = need("LAPLACE_CODE_QDRANT", err, errcap);
    if (!qd) return -1;
    const char *url = getenv("LAPLACE_CODE_EMBED_URL"), *colls = getenv("LAPLACE_CODE_COLLECTIONS");
    const char *turl = getenv("LAPLACE_CODE_TEXT_EMBED_URL"), *tcolls = getenv("LAPLACE_CODE_TEXT_COLLECTIONS");
    int code = url && *url && colls && *colls, text = turl && *turl && tcolls && *tcolls;
    if ((url && *url) != (colls && *colls)) { fail(err, errcap, "%s needs %s", url && *url ? "LAPLACE_CODE_EMBED_URL" : "LAPLACE_CODE_COLLECTIONS", url && *url ? "LAPLACE_CODE_COLLECTIONS" : "LAPLACE_CODE_EMBED_URL"); return -1; }
    if ((turl && *turl) != (tcolls && *tcolls)) { fail(err, errcap, "%s needs %s", turl && *turl ? "LAPLACE_CODE_TEXT_EMBED_URL" : "LAPLACE_CODE_TEXT_COLLECTIONS", turl && *turl ? "LAPLACE_CODE_TEXT_COLLECTIONS" : "LAPLACE_CODE_TEXT_EMBED_URL"); return -1; }
    if (!code && !text) { fail(err, errcap, "LAPLACE_CODE_SEMANTIC is set but neither LAPLACE_CODE_EMBED_URL nor LAPLACE_CODE_TEXT_EMBED_URL is", NULL, NULL); return -1; }
    const char *qkey = getenv("LAPLACE_CODE_QDRANT_KEY");
    const char *pre = getenv("LAPLACE_CODE_QUERY_PREFIX"), *tpre = getenv("LAPLACE_CODE_TEXT_QUERY_PREFIX");
    SemHit *hits = NULL; size_t nh = 0, cap = 0;
    int rc = 0;
    if (code) rc = group(query, url, getenv("LAPLACE_CODE_EMBED_KEY"), pre ? pre : CODE_PREFIX, colls, qd, qkey,
                         per_collection, &hits, &nh, &cap, err, errcap);
    if (!rc && text) {
        const char *tkey = getenv("LAPLACE_CODE_TEXT_EMBED_KEY");
        rc = group(query, turl, tkey ? tkey : getenv("LAPLACE_CODE_EMBED_KEY"), tpre ? tpre : TEXT_PREFIX, tcolls,
                   qd, qkey, per_collection, &hits, &nh, &cap, err, errcap);
    }
    if (rc) { sem_free(hits, nh); return -1; }
    *out = hits; *n = nh;
    return 0;
}

void sem_free(SemHit *hits, size_t n) {
    for (size_t i = 0; i < n; i++) free(hits[i].text);
    free(hits);
}
