/* Laplace-MCP: a Laplace client. Content is decomposed into entity IDs and
 * physicalities here, before any database sees it. Retrieval is by those IDs.
 */
#ifndef LPM_H
#define LPM_H

#include <stddef.h>
#include <stdint.h>

typedef struct Lpm Lpm;

typedef struct {
    uint64_t files, skipped, bytes, entities, documents, words, compositions, ns;
} LpmStats;

typedef struct {
    uint8_t id[16];
    int64_t m[4];       /* physicality: fixed-point coordinate, value = m / 2^53 */
    uint64_t hilbert;
    uint32_t line0, line1;
    uint32_t shared;    /* query constituent IDs this document contains */
    uint32_t bytes;
    char path[512];
} LpmHit;

typedef struct {
    uint8_t id[16];
    int64_t m[4];
    uint64_t hilbert;
    uint32_t seen;
    uint8_t tier;
    uint8_t found;
} LpmRec;

Lpm *lpm_new(const char *tier0);          /* NULL maps the library's tier 0 */
void lpm_free(Lpm *);

/* One buffer, by its recipe. .tsv and .csv are records of fields. A suffix with a
 * built tree-sitter library is that grammar. Anything else that is text is UAX #29.
 * A field, a leaf, and a file with no recipe are text. Same bytes, same entity. */
int lpm_admit(Lpm *, const char *path, const uint8_t *s, size_t n);

/* A file, or a directory walked as a repository. */
int lpm_index_path(Lpm *, const char *path);

/* Rank documents by how many of the query's constituent IDs they contain. */
size_t lpm_search(Lpm *, const char *q, size_t n, LpmHit *out, size_t cap);

/* The entity and physicality of a UTF-8 string, from the text decomposition. */
int lpm_identify(Lpm *, const char *s, size_t n, LpmRec *out);

int lpm_lookup(const Lpm *, const uint8_t id[16], LpmRec *out);
/* The record for an id: physicality in out, constituent ids in kids (cap of them). */
int lpm_fetch(const Lpm *, const uint8_t id[16], LpmRec *out, uint8_t *kids, uint32_t cap, uint32_t *nkids);
int lpm_last(const Lpm *, LpmHit *out);
void lpm_stats(const Lpm *, LpmStats *);

int lpm_save(const Lpm *, const char *path);
Lpm *lpm_load(const char *path, const char *tier0);

#endif
