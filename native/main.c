#define _GNU_SOURCE
#include "lpm.h"
#include "api.h"
#include "lpm_config.h"
#include "os.h"

#include "laplace/laplace.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifndef _WIN32
#include <unistd.h>
#endif

static void hex(const uint8_t b[16], char out[33]) {
    static const char H[] = "0123456789abcdef";
    for (int i = 0; i < 16; i++) {
        out[i * 2] = H[b[i] >> 4];
        out[i * 2 + 1] = H[b[i] & 15];
    }
    out[32] = 0;
}

static int fail(const char *m) {
    fprintf(stderr, "fail: %s\n", m);
    return 1;
}

static int cmd_check(void) {
    Lpm *ix = lpm_new(NULL);
    if (!ix) return fail("tier 0 did not map");
    LpmRec rec;
    int same = lpm_identify(ix, "Holmes", 6, &rec);
    char id[33];
    hex(rec.id, id);
    if (strcmp(id, "5b7e40e9a23ae55eaccd45646934977d")) {
        fprintf(stderr, "fail: Holmes id is %s\n", id);
        return 1;
    }
    if (same != 0) return fail("text decomposition and codepoint composition disagree on Holmes");
    lp_coord c;
    memcpy(c.m, rec.m, sizeof c.m);
    if (!lp_coord_inside(&c)) return fail("Holmes is outside the wall");

    LpmRec a;
    lpm_identify(ix, "A", 1, &a);
    hex(a.id, id);
    if (strcmp(id, "32684bfa28c0c84d6f210511aace0efc")) {
        fprintf(stderr, "fail: A id is %s\n", id);
        return 1;
    }

    lpm_free(ix);
    printf("check ok  Holmes=%s  inside the wall  A=%s\n", "5b7e40e9a23ae55eaccd45646934977d", "32684bfa28c0c84d6f210511aace0efc");
    return 0;
}

static int cmd_id(int argc, char **argv) {
    if (argc < 1) return fail("usage: lpm id STRING");
    Lpm *ix = lpm_new(NULL);
    if (!ix) return fail("tier 0 did not map");
    LpmRec rec;
    int same = lpm_identify(ix, argv[0], strlen(argv[0]), &rec);
    char id[33];
    hex(rec.id, id);
    lp_coord c;
    memcpy(c.m, rec.m, sizeof c.m);
    printf("id        %s\n", id);
    printf("tier      %u\n", rec.tier);
    printf("hilbert   %llu\n", (unsigned long long)rec.hilbert);
    printf("m         %lld %lld %lld %lld\n",
           (long long)rec.m[0], (long long)rec.m[1], (long long)rec.m[2], (long long)rec.m[3]);
    printf("inside    %s\n", lp_coord_inside(&c) ? "yes" : "no");
    printf("flat      %s\n", same == 0 ? "same composition" : same == 1 ? "hierarchical trunk" : "not utf-8");
    lpm_free(ix);
    return 0;
}

static int cmd_serve(int argc, char **argv) {
    /* serve [HOST [PORT]]: an argument, else LAPLACE_MCP_HOST and LAPLACE_MCP_PORT, else what this build was configured with */
    const char *eh = getenv("LAPLACE_MCP_HOST"), *ep = getenv("LAPLACE_MCP_PORT");
    const char *host = argc > 0 ? argv[0] : eh && *eh ? eh : LPM_HOST_DEFAULT;
    int port = argc > 1 ? atoi(argv[1]) : ep && *ep ? atoi(ep) : LPM_PORT_DEFAULT;
    return api_serve(host, port);
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: lpm check | id STRING | text STRING | serve [HOST [PORT]] | mcp\n");
        return 2;
    }
    if (!strcmp(argv[1], "check")) return cmd_check();
    if (!strcmp(argv[1], "id")) return cmd_id(argc - 2, argv + 2);
    if (!strcmp(argv[1], "text")) {
        if (argc < 3) return fail("usage: lpm text STRING");
        Lpm *ix = lpm_new(NULL);
        if (!ix) return fail("tier 0 did not map");
        lpm_show_text(ix, (const uint8_t *)argv[2], strlen(argv[2]), stdout);
        lpm_free(ix);
        return 0;
    }
    if (!strcmp(argv[1], "serve")) return cmd_serve(argc - 2, argv + 2);
    if (!strcmp(argv[1], "mcp")) return api_mcp_stdio();
    fprintf(stderr, "unknown command %s\n", argv[1]);
    return 2;
}
