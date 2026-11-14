/* Laplace-MCP: a Laplace client. A text is decomposed into its entity ID and constituent tree here, with the same Native
 * decomposer every Laplace build uses, before any database sees it. Nothing is indexed here: what the repositories hold
 * is the database's (code_search, code_where and the other reads).
 */
#ifndef LPM_H
#define LPM_H

#include <stddef.h>
#include <stdint.h>

typedef struct Lpm Lpm;

typedef struct {
    uint8_t id[16];
    int64_t m[4];       /* physicality: fixed-point coordinate, value = m / 2^53 */
    uint64_t hilbert;
    uint32_t seen;
    uint8_t tier;
    uint8_t found;
} LpmRec;

Lpm *lpm_new(const char *tier0);          /* NULL maps the library's tier 0 */
void lpm_free(Lpm *);

/* The entity and physicality of a UTF-8 string, from the text decomposition. 0: the same ID as the string's flat
 * codepoint composition; 1: a hierarchical trunk; -1: not UTF-8. */
int lpm_identify(Lpm *, const char *s, size_t n, LpmRec *out);
/* Print a text's decomposition: trunk id, physicality, and the constituent tree. */
void lpm_show_text(Lpm *, const uint8_t *s, size_t n, void *file);

#endif
