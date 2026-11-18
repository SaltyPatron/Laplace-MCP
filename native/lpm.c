#define _GNU_SOURCE
#include "lpm_internal.h"

/* The compositions one decomposition made, kept in Native's ID map (lp_idmap: one open-addressed table keyed by ID,
 * the one Laplace uses everywhere), each entry an Ent. An entry's address holds until the next one is added. */
static Ent *ent_put(Lpm *ix, const lp_ref *r) {
    bool fresh;
    Ent *e = lp_idmap_get(ix->ents, &r->id, &fresh);
    if (!fresh) { e->seen++; return e; }
    memcpy(e->id, r->id.b, 16);
    memcpy(e->m, r->c.m, sizeof e->m);
    e->hilbert = lp_hilbert4(&r->c);
    e->tier = r->tier;
    e->seen = 1;
    return e;
}

Ent *lpm_ent_find(const Lpm *ix, const uint8_t id[16]) {
    lp_id key; memcpy(key.b, id, 16);
    return lp_idmap_lookup(ix->ents, &key);
}

lp_ref lpm_on_compose(void *ud, const lp_ref *ch, uint32_t n, uint8_t tier) {
    Lpm *ix = ud;
    lp_ref r = lp_ref_compose(ch, n, tier);
    for (uint32_t i = 0; i < n; i++) ent_put(ix, &ch[i]);
    Ent *e = ent_put(ix, &r);
    if (!e->kids && n && n <= 200000u && (e->kids = malloc((size_t)n * 16))) {
        for (uint32_t i = 0; i < n; i++) memcpy(e->kids + (size_t)i * 16, ch[i].id.b, 16);
        e->nkids = n;
    }
    return r;
}

Lpm *lpm_new(const char *tier0) {
    Lpm *ix = calloc(1, sizeof *ix);
    if (!ix) return NULL;
    ix->t0 = lp_tier0_map(tier0 && *tier0 ? tier0 : NULL);
    if (!ix->t0) { free(ix); return NULL; }
    ix->text = lp_text_new(ix->t0);
    if (!ix->text) { free(ix); return NULL; }
    ix->ents = lp_idmap_sized(sizeof(Ent));
    return ix;
}

void lpm_free(Lpm *ix) {
    if (!ix) return;
    for (size_t i = 0; i < lp_idmap_count(ix->ents); i++) free(((Ent *)lp_idmap_at(ix->ents, i))->kids);
    lp_idmap_free(ix->ents);
    lp_text_free(ix->text);
    free(ix);
}

int lpm_identify(Lpm *ix, const char *s, size_t n, LpmRec *out) {
    memset(out, 0, sizeof *out);
    lp_id flat;
    if (!lp_id_codepoints_utf8(s, n, &flat)) return -1;
    lp_ref trunk = lp_text_decompose(ix->text, (const uint8_t *)s, n, lpm_on_compose, ix);
    memcpy(out->id, trunk.id.b, 16);
    memcpy(out->m, trunk.c.m, sizeof out->m);
    out->hilbert = lp_hilbert4(&trunk.c);
    out->tier = trunk.tier;
    out->found = 1;
    Ent *e = lpm_ent_find(ix, trunk.id.b);
    if (e) out->seen = e->seen;
    return memcmp(flat.b, trunk.id.b, 16) ? 1 : 0;
}

static void emit_node(const Lpm *ix, const uint8_t id[16], FILE *o) {
    Ent *e = lpm_ent_find(ix, id);
    if (e && e->nkids > 1) {
        fputc('[', o);
        for (uint32_t i = 0; i < e->nkids; i++) {
            if (i) fputc(',', o);
            emit_node(ix, e->kids + (size_t)i * 16, o);
        }
        fputc(']', o);
        return;
    }
    int64_t cp = lp_tier0_codepoint(ix->t0, (const lp_id *)id);
    if (cp == ' ') { fputs("' '", o); return; }
    if (cp >= 32 && cp < 127) { fputc((int)cp, o); return; }
    if (cp >= 0) { fprintf(o, "U+%04X", (unsigned)cp); return; }
    if (e && e->nkids == 1) { emit_node(ix, e->kids, o); return; }
    fputs("?", o);
}

void lpm_show_text(Lpm *ix, const uint8_t *s, size_t n, void *file) {
    FILE *o = file;
    lp_ref trunk = lp_text_decompose(ix->text, s, n, lpm_on_compose, ix);
    char id[33];
    lp_id_hex(&trunk.id, id);
    fprintf(o, "id        %s\n", id);
    fprintf(o, "tier      %u\n", trunk.tier);
    fprintf(o, "hilbert   %llu\n", (unsigned long long)lp_hilbert4(&trunk.c));
    fprintf(o, "m         %lld %lld %lld %lld\n",
            (long long)trunk.c.m[0], (long long)trunk.c.m[1], (long long)trunk.c.m[2], (long long)trunk.c.m[3]);
    emit_node(ix, trunk.id.b, o);
    fputc('\n', o);
}
