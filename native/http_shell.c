/* Laplace::http — reusable Bearer-authenticated HTTP/1.1 listener and MCP JSON-RPC 2.0 shell.
 * Depends only on C17 standard headers, OS sockets, and Laplace-Native (laplace/json.h, lp_buf). */
#define _GNU_SOURCE
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <io.h>
#include <fcntl.h>
#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif
typedef intptr_t lp_ssize_t;
#define close_sock(s) closesocket(s)
#define strncasecmp _strnicmp
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include <strings.h>
typedef ssize_t lp_ssize_t;
#define close_sock(s) close(s)
#endif

#include "laplace/http.h"
#include "laplace/json.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char *shell_strdup_len(const char *s, size_t n) {
    char *p = malloc(n + 1);
    if (!p) abort();
    if (n) memcpy(p, s, n);
    p[n] = 0;
    return p;
}

static int send_all(int conn, const char *p, size_t len) {
    size_t sent = 0;
    while (sent < len) {
        size_t chunk = len - sent;
        if (chunk > (1u << 20)) chunk = 1u << 20;
        lp_ssize_t w = send(conn, p + sent, (int)chunk, MSG_NOSIGNAL);
        if (w <= 0) return 0;
        sent += (size_t)w;
    }
    return 1;
}

void lp_mcp_tools_json(lp_buf *out, const lp_mcp_tool *tools, size_t ntools) {
    lp_buf_cstr(out, "{\"tools\":[");
    for (size_t i = 0; i < ntools; i++) {
        const lp_mcp_tool *t = &tools[i];
        lp_buf_cstr(out, i ? ",{\"name\":" : "{\"name\":");
        lp_json_str(out, t->name ? t->name : "", strlen(t->name ? t->name : ""));
        lp_buf_cstr(out, ",\"description\":");
        lp_json_str(out, t->description ? t->description : "", strlen(t->description ? t->description : ""));
        lp_buf_cstr(out, ",\"inputSchema\":");
        lp_buf_cstr(out, (t->input_schema_json && t->input_schema_json[0]) ? t->input_schema_json : "{}");
        lp_buf_byte(out, '}');
    }
    lp_buf_cstr(out, "]}");
}

int lp_http_sse_begin(int conn) {
    static const char hdr[] =
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/event-stream\r\n"
        "Cache-Control: no-cache\r\n"
        "Connection: keep-alive\r\n\r\n";
    return send_all(conn, hdr, sizeof hdr - 1);
}

int lp_http_sse_event(int conn, const char *event, const char *data) {
    lp_buf out = { 0 };
    if (event && event[0]) {
        lp_buf_cstr(&out, "event: ");
        lp_buf_cstr(&out, event);
        lp_buf_cstr(&out, "\r\n");
    }
    const char *p = data ? data : "";
    for (;;) {
        const char *nl = strchr(p, '\n');
        size_t len = nl ? (size_t)(nl - p) : strlen(p);
        if (len && p[len - 1] == '\r') len--;
        lp_buf_cstr(&out, "data: ");
        if (len) lp_buf_put(&out, p, len);
        lp_buf_cstr(&out, "\r\n");
        if (!nl) break;
        p = nl + 1;
    }
    lp_buf_cstr(&out, "\r\n");
    int ok = send_all(conn, (const char *)out.b, out.n);
    lp_buf_free(&out);
    return ok;
}

void lp_http_sse_end(int conn) {
    (void)conn;
}

static int   capture;
static int   capture_code;
static char *capture_body;
static int   stdio_capture;
static int   stdio_code;
static char *stdio_body;

void lp_http_reply(int conn, int status_code, const char *json_body) {
    const char *payload = json_body ? json_body : "";
    if (capture) {
        capture_code = status_code;
        free(capture_body);
        capture_body = shell_strdup_len(payload, strlen(payload));
        return;
    }
    if (stdio_capture) {
        stdio_code = status_code;
        free(stdio_body);
        stdio_body = shell_strdup_len(payload, strlen(payload));
        return;
    }
    const char *why = status_code == 200 ? "OK"
                    : status_code == 202 ? "Accepted"
                    : status_code == 400 ? "Bad Request"
                    : status_code == 401 ? "Unauthorized"
                    : status_code == 404 ? "Not Found"
                                         : "Error";
    size_t blen = strlen(payload);
    char hdr[256];
    int n = snprintf(hdr, sizeof hdr,
                     "HTTP/1.1 %d %s\r\nContent-Type: application/json\r\nContent-Length: %zu\r\nConnection: close\r\n\r\n",
                     status_code, why, blen);
    if (n > 0) send_all(conn, hdr, (size_t)n);
    if (blen) send_all(conn, payload, blen);
}

void lp_http_capture_begin(int stdio_mode) {
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

const char *lp_http_capture_end(int stdio_mode, int *code_out) {
    if (stdio_mode) {
        stdio_capture = 0;
        if (code_out) *code_out = stdio_code;
        return stdio_body ? stdio_body : "";
    }
    capture = 0;
    if (code_out) *code_out = capture_code;
    return capture_body ? capture_body : "";
}

static void reply_rpc_error(int conn, const char *id_raw, int code, const char *message) {
    lp_buf out = { 0 };
    const char *id = id_raw ? id_raw : "null";
    char head[128];
    int hn = snprintf(head, sizeof head, "{\"jsonrpc\":\"2.0\",\"id\":%s,\"error\":{\"code\":%d,\"message\":", id, code);
    lp_buf_put(&out, head, (size_t)hn);
    lp_json_str(&out, message, strlen(message));
    lp_buf_put(&out, "}}", 2);
    lp_buf_put(&out, "", 1);
    lp_http_reply(conn, 200, (const char *)out.b);
    lp_buf_free(&out);
}

void lp_mcp_rpc(int conn, const char *body, const lp_http_shell *shell) {
    if (!body || !shell) {
        lp_http_reply(conn, 400, "{\"jsonrpc\":\"2.0\",\"id\":null,\"error\":{\"code\":-32600,\"message\":\"method\"}}");
        return;
    }
    lp_json *doc = lp_json_parse_prefix(body, strlen(body), NULL);
    size_t mn = 0;
    const char *method_s = doc ? lp_json_string(doc, lp_json_get(doc, LP_JSON_ROOT, "method"), &mn) : NULL;
    if (!method_s || memchr(method_s, 0, mn) || mn == 0) {
        lp_json_free(doc);
        lp_http_reply(conn, 400, "{\"jsonrpc\":\"2.0\",\"id\":null,\"error\":{\"code\":-32600,\"message\":\"method\"}}");
        return;
    }
    char *method = shell_strdup_len(method_s, mn);

    uint32_t id_node = lp_json_get(doc, LP_JSON_ROOT, "id");
    size_t id_len = 0;
    const char *id_tok = (id_node != LP_JSON_NONE) ? lp_json_text(doc, id_node, &id_len) : NULL;
    char *id_raw = (id_tok && id_len > 0) ? shell_strdup_len(id_tok, id_len) : shell_strdup_len("null", 4);

    if (!strcmp(method, "notifications/initialized") || !strncmp(method, "notifications/", 14)) {
        lp_json_free(doc);
        free(method);
        free(id_raw);
        lp_http_reply(conn, 202, "");
        return;
    }

    uint32_t params = lp_json_get(doc, LP_JSON_ROOT, "params");

    if (!strcmp(method, "initialize")) {
        const char *ver = "2025-03-26";
        char *ver_alloc = NULL;
        if (params != LP_JSON_NONE && lp_json_kind(doc, params) == LP_JSON_OBJECT) {
            size_t vn = 0;
            const char *vs = lp_json_string(doc, lp_json_get(doc, params, "protocolVersion"), &vn);
            if (vs && vn > 0 && !memchr(vs, 0, vn)) {
                ver_alloc = shell_strdup_len(vs, vn);
                ver = ver_alloc;
            }
        }
        const char *sname = (shell->server_name && shell->server_name[0]) ? shell->server_name : "laplace";
        const char *sver  = (shell->server_version && shell->server_version[0]) ? shell->server_version : "0.1";
        lp_buf out = { 0 };
        lp_buf_cstr(&out, "{\"jsonrpc\":\"2.0\",\"id\":");
        lp_buf_cstr(&out, id_raw);
        lp_buf_cstr(&out, ",\"result\":{\"protocolVersion\":");
        lp_json_str(&out, ver, strlen(ver));
        lp_buf_cstr(&out, ",\"capabilities\":{\"tools\":{}},\"serverInfo\":{\"name\":");
        lp_json_str(&out, sname, strlen(sname));
        lp_buf_cstr(&out, ",\"version\":");
        lp_json_str(&out, sver, strlen(sver));
        lp_buf_cstr(&out, "}}}");
        lp_buf_put(&out, "", 1);
        lp_json_free(doc);
        free(ver_alloc);
        free(method);
        free(id_raw);
        lp_http_reply(conn, 200, (const char *)out.b);
        lp_buf_free(&out);
        return;
    }

    if (!strcmp(method, "ping")) {
        lp_buf out = { 0 };
        lp_buf_cstr(&out, "{\"jsonrpc\":\"2.0\",\"id\":");
        lp_buf_cstr(&out, id_raw);
        lp_buf_cstr(&out, ",\"result\":{}}");
        lp_buf_put(&out, "", 1);
        lp_json_free(doc);
        free(method);
        free(id_raw);
        lp_http_reply(conn, 200, (const char *)out.b);
        lp_buf_free(&out);
        return;
    }

    if (!strcmp(method, "tools/list")) {
        lp_buf out = { 0 };
        lp_buf_cstr(&out, "{\"jsonrpc\":\"2.0\",\"id\":");
        lp_buf_cstr(&out, id_raw);
        lp_buf_cstr(&out, ",\"result\":");
        if (shell->tools && shell->ntools > 0)
            lp_mcp_tools_json(&out, shell->tools, shell->ntools);
        else
            lp_buf_cstr(&out, (shell->tools_json && shell->tools_json[0]) ? shell->tools_json : "{\"tools\":[]}");
        lp_buf_cstr(&out, "}");
        lp_buf_put(&out, "", 1);
        lp_json_free(doc);
        free(method);
        free(id_raw);
        lp_http_reply(conn, 200, (const char *)out.b);
        lp_buf_free(&out);
        return;
    }

    if (!strcmp(method, "tools/call")) {
        size_t tn = 0;
        const char *ts = (params != LP_JSON_NONE && lp_json_kind(doc, params) == LP_JSON_OBJECT)
                       ? lp_json_string(doc, lp_json_get(doc, params, "name"), &tn)
                       : NULL;
        if (!ts || memchr(ts, 0, tn) || tn == 0) {
            lp_json_free(doc);
            free(method);
            reply_rpc_error(conn, id_raw, -32602, "name");
            free(id_raw);
            return;
        }
        char *tool_name = shell_strdup_len(ts, tn);

        uint32_t args_node = lp_json_get(doc, params, "arguments");
        size_t alen = 0;
        const char *atok = (args_node != LP_JSON_NONE && lp_json_kind(doc, args_node) == LP_JSON_OBJECT)
                         ? lp_json_text(doc, args_node, &alen)
                         : NULL;
        char *args_json = (atok && alen > 0) ? shell_strdup_len(atok, alen) : shell_strdup_len("{}", 2);

        lp_json_free(doc);
        free(method);

        lp_http_capture_begin(0);
        int handled = 0;
        if (shell->tools && shell->ntools > 0) {
            for (size_t i = 0; i < shell->ntools; i++) {
                if (shell->tools[i].name && !strcmp(shell->tools[i].name, tool_name) && shell->tools[i].handler) {
                    shell->tools[i].handler(conn, args_json);
                    handled = 1;
                    break;
                }
            }
        } else if (shell->call_tool) {
            handled = shell->call_tool(conn, tool_name, args_json, shell->ctx);
        }
        free(tool_name);
        free(args_json);
        if (!handled) {
            (void)lp_http_capture_end(0, NULL);
            reply_rpc_error(conn, id_raw, -32602, "unknown tool");
            free(id_raw);
            return;
        }

        int cap_code = 500;
        const char *cap_body = lp_http_capture_end(0, &cap_code);
        int is_err = cap_code != 200;

        lp_buf out = { 0 };
        lp_buf_cstr(&out, "{\"jsonrpc\":\"2.0\",\"id\":");
        lp_buf_cstr(&out, id_raw);
        lp_buf_cstr(&out, ",\"result\":{\"content\":[{\"type\":\"text\",\"text\":");
        lp_json_str(&out, cap_body, strlen(cap_body));
        lp_buf_cstr(&out, is_err ? "}],\"isError\":true}}" : "}],\"isError\":false}}");
        lp_buf_put(&out, "", 1);
        free(id_raw);
        lp_http_reply(conn, 200, (const char *)out.b);
        lp_buf_free(&out);
        return;
    }

    lp_json_free(doc);
    free(method);
    reply_rpc_error(conn, id_raw, -32601, "method not found");
    free(id_raw);
}

static char   SERVE_TOKEN[513];
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

static int serve_token_load(const char *env_name, const char *file_env_name) {
    SERVE_TOKEN[0] = 0;
    SERVE_TOKEN_N = 0;
    const char *e = getenv(env_name);
    if (e && *e) return token_take(e, strlen(e));
    const char *f = getenv(file_env_name);
    if (!f || !*f) return 0;
    FILE *fp = fopen(f, "rb");
    if (!fp) { perror(f); return 0; }
    char line[1024], first[1024] = "";
    int ok = 0, lines = 0;
    size_t elen = strlen(env_name);
    while (fgets(line, sizeof line, fp)) {
        if (!lines++) snprintf(first, sizeof first, "%s", line);
        if (!strncmp(line, env_name, elen) && line[elen] == '=') {
            ok = token_take(line + elen + 1, strlen(line + elen + 1));
            break;
        }
    }
    fclose(fp);
    if (!ok && lines && !strchr(first, '=')) ok = token_take(first, strlen(first));
    memset(line, 0, sizeof line);
    memset(first, 0, sizeof first);
    return ok;
}

/* Constant-time comparison against the configured Bearer token. */
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
        for (size_t i = 0; i < SERVE_TOKEN_N; i++)
            d |= (unsigned)(unsigned char)SERVE_TOKEN[i] ^ (unsigned)(unsigned char)(i < n ? v[i] : 0);
        return d == 0 && SERVE_TOKEN_N > 0;
    }
    return 0;
}

int lp_http_serve(const char *host, int port, const lp_http_shell *shell) {
    const char *env_name      = (shell && shell->token_env && shell->token_env[0]) ? shell->token_env : "LAPLACE_MCP_TOKEN";
    const char *file_env_name = (shell && shell->token_file_env && shell->token_file_env[0]) ? shell->token_file_env : "LAPLACE_MCP_TOKEN_FILE";
    const char *sname         = (shell && shell->server_name && shell->server_name[0]) ? shell->server_name : "Laplace-MCP";

    if (!serve_token_load(env_name, file_env_name)) {
        fprintf(stderr, "lpm serve: no token: set %s, or %s to a file with its "
                        "%s line (at least 32 characters, no whitespace); refusing to listen unauthenticated\n",
                env_name, file_env_name, env_name);
        return 2;
    }
#ifdef _WIN32
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return 1;
#endif
    if (shell && shell->init && shell->init(shell->ctx) != 0) return 1;

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
    fprintf(stderr, "%s %s:%d\n", sname, host, port);
    for (;;) {
        int c = (int)accept(s, NULL, NULL);
        if (c < 0) continue;
        char *req = malloc(1 << 20);
        if (!req) { close_sock(c); continue; }
        size_t got = 0, rcap = 1 << 20;
        while (got + 1 < rcap) {
            lp_ssize_t r = recv(c, req + got, (int)(rcap - 1 - got), 0);
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
            send_all(c, deny, strlen(deny));
            free(req);
            close_sock(c);
            continue;
        }
        body = body ? body + 4 : "";
        if (shell && shell->dispatch_http)
            shell->dispatch_http(c, method, path, body, shell->ctx);
        else if (!strcmp(method, "GET") && !strcmp(path, "/health"))
            lp_http_reply(c, 200, "{\"ok\":true}");
        else if (!strcmp(method, "POST") && !strcmp(path, "/mcp"))
            lp_mcp_rpc(c, body, shell);
        else
            lp_http_reply(c, 404, "{\"error\":\"not found\"}");
        if (shell && shell->finish_request)
            shell->finish_request(shell->ctx);
        free(req);
        close_sock(c);
    }
}

int lp_mcp_stdio(const lp_http_shell *shell) {
#ifdef _WIN32
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
#endif
    if (shell && shell->init && shell->init(shell->ctx) != 0) return 1;
    size_t cap = 1 << 20;
    char *line = malloc(cap);
    if (!line) return 1;
    while (fgets(line, (int)cap, stdin)) {
        size_t len = strlen(line);
        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) line[--len] = 0;
        if (!len) continue;
        lp_http_capture_begin(1);
        lp_mcp_rpc(-1, line, shell);
        if (shell && shell->finish_request)
            shell->finish_request(shell->ctx);
        int scode = 500;
        const char *sbody = lp_http_capture_end(1, &scode);
        if (scode != 202 && sbody && sbody[0]) {
            fputs(sbody, stdout);
            fputc('\n', stdout);
            fflush(stdout);
        }
    }
    free(line);
    return 0;
}
