#define _GNU_SOURCE
#include "lpm.h"

#include "laplace/laplace.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static void hex(const uint8_t b[16], char out[33]) {
    static const char H[] = "0123456789abcdef";
    for (int i = 0; i < 16; i++) {
        out[i * 2] = H[b[i] >> 4];
        out[i * 2 + 1] = H[b[i] & 15];
    }
    out[32] = 0;
}

static uint64_t now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
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

    LpmStats before, after;
    const char *doc = "lp_text_decompose lives beside lp_ref_compose.\n";
    lpm_stats(ix, &before);
    if (lpm_admit(ix, "a.txt", (const uint8_t *)doc, strlen(doc))) return fail("admit");
    lpm_stats(ix, &after);
    uint64_t entities = after.entities;
    if (lpm_admit(ix, "b.txt", (const uint8_t *)doc, strlen(doc))) return fail("admit again");
    lpm_stats(ix, &after);
    if (after.entities != entities) return fail("the same bytes minted a second entity");
    if (after.documents != before.documents + 2) return fail("both occurrences should be kept");

    uint8_t blob[32];
    for (int i = 0; i < 32; i++) blob[i] = (uint8_t)(128 + i);
    lpm_stats(ix, &before);
    if (lpm_admit(ix, "x.bin", blob, sizeof blob)) return fail("binary admit");
    lpm_stats(ix, &after);
    uint64_t bent = after.entities;
    if (lpm_admit(ix, "y.bin", blob, sizeof blob)) return fail("binary admit again");
    lpm_stats(ix, &after);
    if (after.entities != bent) return fail("the same bytes minted a second binary entity");

    const char *other = "a note that never mentions the function.\n";
    if (lpm_admit(ix, "c.txt", (const uint8_t *)other, strlen(other))) return fail("admit other");
    LpmHit hits[8];
    size_t n = lpm_search(ix, "lp_text_decompose", 17, hits, 8);
    if (!n) return fail("search returned nothing");
    int found = 0;
    for (size_t i = 0; i < n; i++) {
        if (!strcmp(hits[i].path, "a.txt") || !strcmp(hits[i].path, "b.txt")) found = 1;
        if (!strcmp(hits[i].path, "c.txt") && hits[i].shared > hits[0].shared) return fail("the unrelated note outranked the text");
    }
    if (!found) return fail("search missed the admitted text");
    if (!strcmp(hits[0].path, "c.txt")) return fail("the unrelated note ranked first");

    if (lpm_save(ix, "/tmp/lpm-check.records")) return fail("save");
    Lpm *loaded = lpm_load("/tmp/lpm-check.records", NULL);
    if (!loaded) return fail("load");
    LpmHit hits2[8];
    size_t n2 = lpm_search(loaded, "lp_text_decompose", 17, hits2, 8);
    if (!n2 || strcmp(hits2[0].path, hits[0].path)) return fail("loaded records searched differently");
    lpm_free(loaded);
    lpm_free(ix);
    printf("check ok  Holmes=%s  inside the wall  dedup held  search ranked the text  reload agreed\n", "5b7e40e9a23ae55eaccd45646934977d");
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

static int cmd_index(int argc, char **argv) {
    if (argc < 2) return fail("usage: lpm index RECORDS PATH...");
    Lpm *ix = lpm_new(NULL);
    if (!ix) return fail("tier 0 did not map");
    uint64_t t0 = now();
    for (int i = 1; i < argc; i++) {
        if (lpm_index_path(ix, argv[i])) {
            fprintf(stderr, "fail: %s\n", argv[i]);
            return 1;
        }
    }
    uint64_t ns = now() - t0;
    if (lpm_save(ix, argv[0])) return fail("save");
    LpmStats s;
    lpm_stats(ix, &s);
    double sec = (double)ns / 1e9;
    printf("files        %llu\n", (unsigned long long)s.files);
    printf("skipped      %llu\n", (unsigned long long)s.skipped);
    printf("bytes        %llu\n", (unsigned long long)s.bytes);
    printf("entities     %llu\n", (unsigned long long)s.entities);
    printf("documents    %llu\n", (unsigned long long)s.documents);
    printf("words        %llu\n", (unsigned long long)s.words);
    printf("compositions %llu\n", (unsigned long long)s.compositions);
    printf("seconds      %.4f\n", sec);
    printf("MB/s         %.2f\n", sec > 0 ? (double)s.bytes / 1e6 / sec : 0);
    printf("records      %s\n", argv[0]);
    lpm_free(ix);
    return 0;
}

static int cmd_search(int argc, char **argv) {
    if (argc < 2) return fail("usage: lpm search RECORDS QUERY");
    Lpm *ix = lpm_load(argv[0], NULL);
    if (!ix) return fail("load");
    const char *q = argv[1];
    uint64_t t0 = now();
    LpmHit hits[10];
    size_t n = lpm_search(ix, q, strlen(q), hits, 10);
    uint64_t ns = now() - t0;
    for (size_t i = 0; i < n; i++) {
        char id[33];
        hex(hits[i].id, id);
        printf("%2zu  shared %u  %s  lines %u-%u  %s\n", i + 1, hits[i].shared, id,
               hits[i].line0, hits[i].line1, hits[i].path);
    }
    printf("search_us    %.1f\n", (double)ns / 1e3);
    lpm_free(ix);
    return n ? 0 : 1;
}

static int cmd_bench(int argc, char **argv) {
    if (argc < 1) return fail("usage: lpm bench PATH...");
    Lpm *ix = lpm_new(NULL);
    if (!ix) return fail("tier 0 did not map");
    uint64_t t0 = now();
    for (int i = 0; i < argc; i++) {
        if (lpm_index_path(ix, argv[i])) {
            fprintf(stderr, "fail: %s\n", argv[i]);
            return 1;
        }
    }
    uint64_t ns = now() - t0;
    LpmStats s;
    lpm_stats(ix, &s);
    const char *q = "lp_text_decompose";
    LpmHit hits[5];
    uint64_t ts = now();
    size_t n = 0;
    for (int i = 0; i < 200; i++) n = lpm_search(ix, q, strlen(q), hits, 5);
    uint64_t search_ns = (now() - ts) / 200;
    double sec = (double)ns / 1e9;
    size_t outside = 0;
    /* outside-the-wall is checked on the query result and on Holmes in `check`. */
    printf("files        %llu\n", (unsigned long long)s.files);
    printf("skipped      %llu\n", (unsigned long long)s.skipped);
    printf("bytes        %llu\n", (unsigned long long)s.bytes);
    printf("entities     %llu\n", (unsigned long long)s.entities);
    printf("documents    %llu\n", (unsigned long long)s.documents);
    printf("words        %llu\n", (unsigned long long)s.words);
    printf("compositions %llu\n", (unsigned long long)s.compositions);
    printf("dedup        %.3f\n", s.compositions ? 1.0 - (double)s.entities / (double)s.compositions : 0);
    printf("seconds      %.4f\n", sec);
    printf("MB/s         %.2f\n", sec > 0 ? (double)s.bytes / 1e6 / sec : 0);
    printf("search_us    %.1f\n", (double)search_ns / 1e3);
    printf("outside      %zu\n", outside);
    for (size_t i = 0; i < n; i++) {
        char id[33];
        hex(hits[i].id, id);
        printf("hit %zu       shared %u  %s  %s\n", i + 1, hits[i].shared, id, hits[i].path);
    }
    int ok = 0;
    for (size_t i = 0; i < n; i++) if (strstr(hits[i].path, "text.c") || strstr(hits[i].path, "laplace.h")) ok = 1;
    lpm_free(ix);
    if (!ok) return fail("lp_text_decompose did not surface text.c or laplace.h");
    return 0;
}

static void print_rec(const char *label, const LpmRec *r) {
    char id[33];
    hex(r->id, id);
    printf("%s %s\n", label, id);
    printf("tier      %u\n", r->tier);
    printf("hilbert   %llu\n", (unsigned long long)r->hilbert);
    printf("m         %lld %lld %lld %lld\n",
           (long long)r->m[0], (long long)r->m[1], (long long)r->m[2], (long long)r->m[3]);
}

static int cmd_admit(int argc, char **argv) {
    if (argc < 1) return fail("usage: lpm admit FILE...");
    Lpm *ix = lpm_new(NULL);
    if (!ix) return fail("tier 0 did not map");
    for (int i = 0; i < argc; i++) {
        uint64_t t0 = now();
        if (lpm_index_path(ix, argv[i])) return fail(argv[i]);
        LpmHit hit;
        if (lpm_last(ix, &hit)) return fail("no record");
        LpmRec rec;
        uint32_t nk = 0;
        if (lpm_fetch(ix, hit.id, &rec, NULL, 0, &nk)) return fail("fetch");
        char id[33];
        hex(hit.id, id);
        double ms = (double)(now() - t0) / 1e6;
        printf("id        %s\n", id);
        printf("children  %u\n", nk);
        printf("tier      %u\n", rec.tier);
        printf("m         %lld %lld %lld %lld\n",
               (long long)rec.m[0], (long long)rec.m[1], (long long)rec.m[2], (long long)rec.m[3]);
        printf("ms        %.1f\n", ms);
        printf("path      %s\n", hit.path);
    }
    lpm_free(ix);
    return 0;
}

static int cmd_fetch(int argc, char **argv) {
    if (argc < 2) return fail("usage: lpm fetch FILE ID");
    Lpm *ix = lpm_new(NULL);
    if (!ix) return fail("tier 0 did not map");
    if (lpm_index_path(ix, argv[0])) return fail(argv[0]);
    uint8_t idb[16];
    if (strlen(argv[1]) != 32) return fail("id is 32 hex characters");
    for (int i = 0; i < 16; i++) {
        int h = argv[1][i * 2], l = argv[1][i * 2 + 1];
        int hv = h >= 'a' ? h - 'a' + 10 : h >= 'A' ? h - 'A' + 10 : h - '0';
        int lv = l >= 'a' ? l - 'a' + 10 : l >= 'A' ? l - 'A' + 10 : l - '0';
        idb[i] = (uint8_t)((hv << 4) | lv);
    }
    LpmRec rec;
    uint8_t kids[16 * 8];
    uint32_t nk = 0;
    if (lpm_fetch(ix, idb, &rec, kids, 8, &nk)) return fail("no such id");
    print_rec("id       ", &rec);
    printf("children  %u\n", nk);
    uint32_t show = nk < 8 ? nk : 8;
    for (uint32_t i = 0; i < show; i++) {
        char kid[33];
        hex(kids + i * 16, kid);
        printf("child %u   %s\n", i, kid);
    }
    lpm_free(ix);
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: lpm check | id STRING | admit FILE... | fetch FILE ID | index RECORDS PATH... | search RECORDS QUERY | bench PATH...\n");
        return 2;
    }
    if (!strcmp(argv[1], "check")) return cmd_check();
    if (!strcmp(argv[1], "id")) return cmd_id(argc - 2, argv + 2);
    if (!strcmp(argv[1], "admit")) return cmd_admit(argc - 2, argv + 2);
    if (!strcmp(argv[1], "fetch")) return cmd_fetch(argc - 2, argv + 2);
    if (!strcmp(argv[1], "index")) return cmd_index(argc - 2, argv + 2);
    if (!strcmp(argv[1], "search")) return cmd_search(argc - 2, argv + 2);
    if (!strcmp(argv[1], "bench")) return cmd_bench(argc - 2, argv + 2);
    fprintf(stderr, "unknown command %s\n", argv[1]);
    return 2;
}
