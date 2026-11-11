#ifndef LPM_INTERNAL_H
#define LPM_INTERNAL_H

#include "lpm.h"
#include "laplace/laplace.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The compositions one decomposition made, by ID, each with its constituents: what lpm_show_text prints the tree from. */
typedef struct Ent {
    uint8_t id[16];
    int64_t m[4];
    uint64_t hilbert;
    uint32_t seen;
    uint8_t tier;
    uint8_t used;
    uint8_t *kids;
    uint32_t nkids;
} Ent;

struct Lpm {
    const lp_tier0_record *t0;
    lp_text *text;
    Ent *slot;
    size_t nslot, nent;
};

Ent *lpm_ent_find(const Lpm *ix, const uint8_t id[16]);
lp_ref lpm_on_compose(void *ud, const lp_ref *ch, uint32_t n, uint8_t tier);

#endif
