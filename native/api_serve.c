#include "api_internal.h"

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
    if (!os_run_captured(av, 0, (size_t)1 << 22, &log, &code)) {
        api_http(c, 500, "{\"error\":\"engine\"}");
        return;
    }
    lp_buf out = { 0 };
    lp_buf_fmt(&out, "{\"exit\":%d,\"engine\":\"laplace ingest\",\"log\":", code);
    lp_json_str(&out, log ? log : "", strlen(log ? log : ""));
    lp_buf_byte(&out, '}');
    free(log);
    api_reply_buf(c, code == 0 ? 200 : 500, &out);
}

int api_plain_arg(const char *s) {
    return s && s[0] && s[0] != '-' && strlen(s) < 4096;
}

void api_on_engine(int c, char **av) {
    char *log = NULL;
    int code = 127;
    if (!os_run_captured(av, 0, (size_t)1 << 22, &log, &code)) {
        api_http(c, 500, "{\"error\":\"engine\"}");
        return;
    }
    lp_buf out = { 0 };
    lp_buf_fmt(&out, "{\"exit\":%d,\"log\":", code);
    lp_json_str(&out, log ? log : "", strlen(log ? log : ""));
    lp_buf_byte(&out, '}');
    free(log);
    api_reply_buf(c, code == 0 ? 200 : 500, &out);
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

void on_turn(int c, const char *body) {
    char text[8192], session[256] = "", user[256] = "";
    if (!api_jstr(body, "input", text, sizeof text) || !api_plain_arg(text)) {
        api_http(c, 400, "{\"error\":\"input\"}");
        return;
    }
    int has_session = api_jstr(body, "session", session, sizeof session) && api_plain_arg(session);
    int has_user = api_jstr(body, "user", user, sizeof user) && api_plain_arg(user);
    int read_only = api_jbool(body, "read", !has_session);
    char *av[12];
    int k = 0;
    av[k++] = (char *)ENGINE;
    av[k++] = "turn";
    if (read_only) av[k++] = "--read";
    if (has_session) { av[k++] = "--session"; av[k++] = session; }
    if (has_user) { av[k++] = "--as"; av[k++] = user; }
    av[k++] = text;
    av[k] = NULL;
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

int api_serve(const char *host, int port) {
    return lp_http_serve(host, port, api_lpm_shell());
}
