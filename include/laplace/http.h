/* Reusable HTTP listener and MCP JSON-RPC 2.0 shell for Laplace services. */
#ifndef LAPLACE_HTTP_H
#define LAPLACE_HTTP_H

#include "laplace/laplace.h"
#include <stddef.h>

typedef struct lp_mcp_tool {
    const char *name;
    const char *description;
    const char *input_schema_json;
    void (*handler)(int conn, const char *args_json);
} lp_mcp_tool;

typedef struct lp_http_shell {
    const char *server_name;
    const char *server_version;
    const char *tools_json;
    const char *token_env;
    const char *token_file_env;
    int  (*init)(void *ctx);
    void (*finish_request)(void *ctx);
    int  (*call_tool)(int conn, const char *name, const char *args_json, void *ctx);
    void (*dispatch_http)(int conn, const char *method, const char *path, const char *body, void *ctx);
    void *ctx;
    const lp_mcp_tool *tools;
    size_t ntools;
} lp_http_shell;

void        lp_mcp_tools_json(lp_buf *out, const lp_mcp_tool *tools, size_t ntools);
void        lp_http_reply(int conn, int status_code, const char *json_body);
int         lp_http_sse_begin(int conn);
int         lp_http_sse_event(int conn, const char *event, const char *data);
void        lp_http_sse_end(int conn);
void        lp_http_capture_begin(int stdio_mode);
const char *lp_http_capture_end(int stdio_mode, int *code_out);
void        lp_mcp_rpc(int conn, const char *body, const lp_http_shell *shell);
int         lp_http_serve(const char *host, int port, const lp_http_shell *shell);
int         lp_mcp_stdio(const lp_http_shell *shell);

#endif
