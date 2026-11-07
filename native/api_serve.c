#define _GNU_SOURCE
#include "api_internal.h"

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
