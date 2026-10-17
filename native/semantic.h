/* Semantic candidates for code_search: the query embedded by an OpenAI-compatible embeddings endpoint and the
 * nearest chunks from Qdrant collections that hold chunks of the same files (code-search's collections). Laplace
 * reranks them with the lexical candidates; this file only fetches. Configured from the environment; a call that
 * is configured and fails says why, it does not answer without them. */
#ifndef LPM_SEMANTIC_H
#define LPM_SEMANTIC_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    char path[1024];        /* as the collection stores it: repository label, then the path in it */
    char collection[128];
    uint32_t line0, line1;
    double cosine;
    char *text;
} SemHit;

/* 1 when LAPLACE_CODE_SEMANTIC is set to anything but 0. */
int sem_enabled(void);
/* Up to per_collection hits from every configured collection. 0 on success; else -1 with err set. */
int sem_search(const char *query, size_t per_collection, SemHit **out, size_t *n, char *err, size_t errcap);
void sem_free(SemHit *hits, size_t n);

/* Exposed for tests: decode the JSON string starting at s (just after its opening quote) into out. Returns the
 * position after the closing quote, or NULL. */
const char *sem_json_string(const char *s, char *out, size_t cap);

#endif
