#define _GNU_SOURCE
#include "lpm_internal.h"

int lpm_identify(Lpm *ix, const char *s, size_t n, LpmRec *out) {
    memset(out, 0, sizeof *out);
    lp_id flat;
    if (!lp_id_codepoints_utf8(s, n, &flat)) return -1;
    ix->collecting = 0;
    lp_ref trunk = lp_text_decompose(ix->text, (const uint8_t *)s, n, lpm_on_compose, ix);
    memcpy(out->id, trunk.id.b, 16);
    memcpy(out->m, trunk.c.m, sizeof out->m);
    out->hilbert = lp_hilbert4(&trunk.c);
    out->tier = trunk.tier;
    out->found = 1;
    Ent *e = lpm_ent_find(ix, trunk.id.b);
    if (e) out->seen = e->seen;
    (void)flat;
    return memcmp(flat.b, trunk.id.b, 16) ? 1 : 0;
}

int lpm_lookup(const Lpm *ix, const uint8_t id[16], LpmRec *out) {
    memset(out, 0, sizeof *out);
    Ent *e = lpm_ent_find(ix, id);
    if (!e) return -1;
    memcpy(out->id, e->id, 16);
    memcpy(out->m, e->m, sizeof out->m);
    out->hilbert = e->hilbert;
    out->tier = e->tier;
    out->seen = e->seen;
    out->found = 1;
    return 0;
}

int lpm_fetch(const Lpm *ix, const uint8_t id[16], LpmRec *out, uint8_t *kids, uint32_t cap, uint32_t *nkids) {
    if (lpm_lookup(ix, id, out)) { if (nkids) *nkids = 0; return -1; }
    Ent *e = lpm_ent_find(ix, id);
    uint32_t n = e && e->kids ? e->nkids : 0;
    if (nkids) *nkids = n;
    if (kids && cap && n) memcpy(kids, e->kids, (n < cap ? n : cap) * 16);
    return 0;
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
    if (cp >= 32 && cp < 127) { fputc(cp, o); return; }
    if (cp >= 0) { fprintf(o, "U+%04X", (unsigned)cp); return; }
    if (e && e->nkids == 1) { emit_node(ix, e->kids, o); return; }
    fputs("?", o);
}

void lpm_show_text(Lpm *ix, const uint8_t *s, size_t n, void *file) {
    FILE *o = file;
    static const char H[] = "0123456789abcdef";
    ix->collecting = 0;
    lp_ref trunk = lp_text_decompose(ix->text, s, n, lpm_on_compose, ix);
    char id[33];
    for (int b = 0; b < 16; b++) { id[b * 2] = H[trunk.id.b[b] >> 4]; id[b * 2 + 1] = H[trunk.id.b[b] & 15]; }
    id[32] = 0;
    fprintf(o, "id        %s\n", id);
    fprintf(o, "tier      %u\n", trunk.tier);
    fprintf(o, "hilbert   %llu\n", (unsigned long long)lp_hilbert4(&trunk.c));
    fprintf(o, "m         %lld %lld %lld %lld\n",
            (long long)trunk.c.m[0], (long long)trunk.c.m[1], (long long)trunk.c.m[2], (long long)trunk.c.m[3]);
    emit_node(ix, trunk.id.b, o);
    fputc('\n', o);
}

int lpm_last(const Lpm *ix, LpmHit *out) {
    if (!ix->ndocs) return -1;
    Doc *d = &ix->docs[ix->ndocs - 1];
    memset(out, 0, sizeof *out);
    memcpy(out->id, d->id, 16);
    memcpy(out->m, d->m, sizeof out->m);
    out->hilbert = d->hilbert;
    out->line0 = d->line0;
    out->line1 = d->line1;
    out->bytes = d->bytes;
    snprintf(out->path, sizeof out->path, "%s", ix->paths + d->path);
    return 0;
}

void lpm_stats(const Lpm *ix, LpmStats *s) {
    memset(s, 0, sizeof *s);
    for (size_t i = 0; i < ix->nfl; i++) if (!ix->fl[i].dead) { s->files++; s->bytes += ix->fl[i].size; }
    s->skipped = ix->skipped;
    s->entities = ix->nent;
    s->documents = ix->ndocs - ix->dead_docs;
    s->stale = ix->dead_docs;
    s->compositions = ix->compositions;
    for (size_t i = 0; i < ix->nslot; i++) if (ix->slot[i].used && ix->slot[i].nd) s->words++;
}

static void wr(FILE *f, const void *p, size_t n) {
    if (fwrite(p, 1, n, f) != n) abort();
}

/* Records, version 2: entities with their constituents, files with what reindexing compares, documents,
 * postings. Everything an index held is there when it is loaded. */
int lpm_save(const Lpm *ix, const char *path) {
    char tmp[4200];
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *f = fopen(tmp, "wb");
    if (!f) return -1;
    setvbuf(f, NULL, _IOFBF, 1 << 20);
    wr(f, "LPM1", 4);
    uint32_t ver = 2;
    wr(f, &ver, 4);
    uint64_t nent = ix->nent, nd = ix->ndocs, nf = ix->nfl;
    wr(f, &nent, 8);
    for (size_t i = 0; i < ix->nslot; i++) {
        Ent *e = &ix->slot[i];
        if (!e->used) continue;
        wr(f, e->id, 16);
        wr(f, e->m, 32);
        wr(f, &e->hilbert, 8);
        wr(f, &e->seen, 4);
        wr(f, &e->tier, 1);
        uint8_t pad[3] = {0};
        wr(f, pad, 3);
        wr(f, &e->nkids, 4);
        if (e->nkids) wr(f, e->kids, (size_t)e->nkids * 16);
    }
    wr(f, &nf, 8);
    for (size_t i = 0; i < ix->nfl; i++) {
        const LFile *fl = &ix->fl[i];
        const char *p = ix->paths + fl->path;
        uint32_t pl = (uint32_t)strlen(p);
        wr(f, &pl, 4);
        wr(f, p, pl);
        wr(f, &fl->size, 8);
        wr(f, &fl->mtime, 8);
        wr(f, &fl->hash, 8);
        wr(f, &fl->doc0, 4);
        wr(f, &fl->ndoc, 4);
        wr(f, &fl->dead, 1);
    }
    wr(f, &nd, 8);
    for (size_t i = 0; i < ix->ndocs; i++) {
        Doc *d = &ix->docs[i];
        wr(f, d->id, 16);
        wr(f, d->m, 32);
        wr(f, &d->hilbert, 8);
        wr(f, &d->line0, 4);
        wr(f, &d->line1, 4);
        wr(f, &d->bytes, 4);
        wr(f, &d->tier, 1);
        wr(f, &d->file, 4);
    }
    uint64_t np = 0;
    for (size_t i = 0; i < ix->nslot; i++) if (ix->slot[i].used && ix->slot[i].nd) np++;
    wr(f, &np, 8);
    for (size_t i = 0; i < ix->nslot; i++) {
        Ent *e = &ix->slot[i];
        if (!e->used || !e->nd) continue;
        wr(f, e->id, 16);
        wr(f, &e->nd, 4);
        wr(f, e->docs, e->nd * 4);
    }
    if (fclose(f) != 0) return -1;
    remove(path);
    return rename(tmp, path) == 0 ? 0 : -1;
}

static int rd(FILE *f, void *p, size_t n) {
    return fread(p, 1, n, f) == n;
}

/* NULL when the file is missing, not records, another version, or cut short. */
Lpm *lpm_load(const char *path, const char *tier0) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    setvbuf(f, NULL, _IOFBF, 1 << 20);
    char mag[4];
    uint32_t ver = 0;
    if (!rd(f, mag, 4) || memcmp(mag, "LPM1", 4) || !rd(f, &ver, 4) || ver != 2) { fclose(f); return NULL; }
    Lpm *ix = lpm_new(tier0);
    if (!ix) { fclose(f); return NULL; }
    uint64_t nent;
    if (!rd(f, &nent, 8)) goto bad;
    {
        size_t nslot = 1024;
        while ((uint64_t)nslot * 7 <= nent * 10) nslot *= 2;
        lpm_rehash(ix, nslot);
    }
    for (uint64_t i = 0; i < nent; i++) {
        lp_ref r;
        memset(&r, 0, sizeof r);
        uint32_t seen, nk;
        uint8_t tier, pad[3];
        uint64_t hilbert;
        if (!rd(f, r.id.b, 16) || !rd(f, r.c.m, 32) || !rd(f, &hilbert, 8) || !rd(f, &seen, 4) || !rd(f, &tier, 1) ||
            !rd(f, pad, 3) || !rd(f, &nk, 4)) goto bad;
        r.tier = tier;
        int fresh = 0;
        Ent *e = lpm_ent_put(ix, &r, &fresh);
        e->seen = seen;
        e->hilbert = hilbert;
        if (nk) {
            uint8_t *k = malloc((size_t)nk * 16);
            if (!k) abort();
            if (!rd(f, k, (size_t)nk * 16)) { free(k); goto bad; }
            free(e->kids);
            e->kids = k;
            e->nkids = nk;
        }
    }
    uint64_t nf;
    if (!rd(f, &nf, 8)) goto bad;
    for (uint64_t i = 0; i < nf; i++) {
        uint32_t pl;
        if (!rd(f, &pl, 4) || pl > 65535) goto bad;
        char *p = malloc((size_t)pl + 1);
        if (!p) abort();
        if (!rd(f, p, pl)) { free(p); goto bad; }
        p[pl] = 0;
        uint64_t size, hash;
        int64_t mtime;
        uint32_t doc0, ndoc;
        uint8_t dead;
        if (!rd(f, &size, 8) || !rd(f, &mtime, 8) || !rd(f, &hash, 8) || !rd(f, &doc0, 4) || !rd(f, &ndoc, 4) ||
            !rd(f, &dead, 1)) { free(p); goto bad; }
        uint32_t off = lpm_add_path(ix, p);
        free(p);
        if (ix->nfl == ix->cfl) {
            ix->cfl = ix->cfl ? ix->cfl * 2 : 256;
            ix->fl = lpm_grow(ix->fl, ix->cfl * sizeof *ix->fl);
        }
        LFile *fl = &ix->fl[ix->nfl++];
        memset(fl, 0, sizeof *fl);
        fl->path = off;
        fl->size = size;
        fl->mtime = mtime;
        fl->hash = hash;
        fl->doc0 = doc0;
        fl->ndoc = ndoc;
        fl->dead = dead;
        fl->seen = 1;
        if (dead) ix->dead_docs += ndoc;
        else ix->files++;
    }
    size_t cap = 1024;
    while (cap < (ix->nfl + 1) * 2) cap *= 2;
    lpm_fmap_rehash(ix, cap);
    uint64_t nd;
    if (!rd(f, &nd, 8)) goto bad;
    for (uint64_t i = 0; i < nd; i++) {
        if (ix->ndocs == ix->cdocs) {
            ix->cdocs = ix->cdocs ? ix->cdocs * 2 : 256;
            ix->docs = lpm_grow(ix->docs, ix->cdocs * sizeof *ix->docs);
        }
        Doc *d = &ix->docs[ix->ndocs];
        memset(d, 0, sizeof *d);
        uint8_t tier;
        if (!rd(f, d->id, 16) || !rd(f, d->m, 32) || !rd(f, &d->hilbert, 8) || !rd(f, &d->line0, 4) ||
            !rd(f, &d->line1, 4) || !rd(f, &d->bytes, 4) || !rd(f, &tier, 1) || !rd(f, &d->file, 4)) goto bad;
        if (d->file >= ix->nfl) goto bad;
        d->tier = tier;
        d->path = ix->fl[d->file].path;
        ix->ndocs++;
    }
    uint64_t np;
    if (!rd(f, &np, 8)) goto bad;
    for (uint64_t i = 0; i < np; i++) {
        uint8_t id[16];
        uint32_t count;
        if (!rd(f, id, 16) || !rd(f, &count, 4)) goto bad;
        Ent *e = lpm_ent_find(ix, id);
        if (!e) goto bad;
        e->cd = count;
        e->docs = malloc((count ? count : 1) * sizeof *e->docs);
        if (!e->docs) abort();
        if (!rd(f, e->docs, (size_t)count * 4)) goto bad;
        e->nd = count;
    }
    fclose(f);
    return ix;
bad:
    fclose(f);
    lpm_free(ix);
    return NULL;
}
