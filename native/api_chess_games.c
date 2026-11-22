/* Chess reads of recorded games (Sequence 27.2-27.3, Corpora: Games, Conflicts K2):
 *
 * A game occurrence G is line | [line, meta(, clocks, notes)] (or [meta, raw_movetext] when unreplayable):
 *   line    [P0, P1, ..., Pn] (P0 alone for n == 0 plies)
 *   meta    10 positional game-altitude slots: [seats, result, date, event, site, round, eco, tc, variant, counters]
 *   seats   [seat_0, seat_1] (0 = White, 1 = Black), each seat [name, elo|-] or [["FIDE", id], name, elo|-]
 *   clocks  [c_0, ..., c_{n-1}] integer deciseconds per ply ('-' where absent)
 * SAN, UCI, FEN, [%clk], and PGN headers are synthesized at this read edge from the canonical board trajectory and
 * positional occurrence slots; zero KVP tag pairs, raw FENs, or [%clk] strings exist in the database. */
#define _GNU_SOURCE
#include "api_internal.h"
#include "chess_rules.h"

#define CHESS_CAP 200000
#define GAMES_MAX 50000
#define SEARCH_CAP 20000
#define NAME_CAP 8192
#define PAGE_MAX 200
#define PLIES_MAX 2048
#define WALK_DEPTH 12

typedef struct {
    lp_id fide_node, name, elo;
    uint8_t has_fide, has_name, has_elo;
} SeatInfo;

typedef struct {
    lp_id rec, line, meta, clocks, notes;
    uint8_t has_line, has_meta, has_clocks, has_notes, replays;
    SeatInfo seat[2];
    uint32_t res_cp;              /* '1', '0', '=', '*', or 0 when no meta */
    lp_id date, event, site, round, eco, tc, variant, counters;
    uint8_t has_date, has_event, has_site, has_round, has_eco, has_tc, has_variant, has_counters;
    int64_t order;
    int side;                     /* 0 White, 1 Black, -1 none */
} Game;

typedef struct { Game *g; size_t n, cap; Reader *rd; } Load;

static lp_id T_FIDE;
static int fide_ready(void){
    static int done = 0;
    if (done) return 1;
    if (!TX || !lp_text_id(TX, "FIDE", 4, &T_FIDE)) return 0;
    done = 1;
    return 1;
}
static void unavailable(int c){ api_http(c, 503, "{\"error\":\"database\"}"); }
static int clamp_int(int v, int lo, int hi){ return v < lo ? lo : v > hi ? hi : v; }
static int is_cp(const lp_id *id){ return lp_tier0_codepoint(T0, id) >= 0; }
static int cp_is(const lp_id *id, uint32_t c){ return lp_tier0_codepoint(T0, id) == (int64_t)c; }

typedef struct { lp_id *id; uint32_t *said; size_t n; } Verts;
static Verts verts_of(const uint8_t *ewkb, size_t len){
    Verts v = { 0 };
    size_t nv = lp_path_vertices(ewkb, len, NULL, 0);
    if (!nv) return v;
    lp_vertex *x = malloc(sizeof(lp_vertex) * nv);
    lp_path_vertices(ewkb, len, x, nv);
    size_t total = 0;
    for (size_t i = 0; i < nv; i++) total += x[i].run ? x[i].run : 1;
    v.id = malloc(sizeof(lp_id) * total);
    v.said = malloc(sizeof(uint32_t) * total);
    for (size_t i = 0; i < nv; i++) {
        uint32_t run = x[i].run ? x[i].run : 1;
        for (uint32_t k = 0; k < run; k++) { v.id[v.n] = x[i].id; v.said[v.n++] = x[i].said; }
    }
    free(x);
    return v;
}
static void verts_free(Verts *v){ free(v->id); free(v->said); memset(v, 0, sizeof *v); }

typedef struct { lp_idmap *at; Verts *v; size_t n; } Paths;
static void paths_init(Paths *p){ memset(p, 0, sizeof *p); p->at = lp_idmap_sized(sizeof(int)); }
static void paths_add_ids(PGconn *pg, const lp_id *ids, size_t n, Paths *p){
    if (!p->at) paths_init(p);
    if (!n) return;
    lp_vec(lp_id) fresh_ids = { 0 };
    lp_idmap *queued = lp_idmap_new();
    for (size_t i = 0; i < n; i++) {
        if (is_cp(&ids[i]) || lp_idmap_find(p->at, &ids[i]) >= 0) continue;
        bool fr; lp_idmap_put(queued, &ids[i], &fr);
        if (fr) lp_push(&fresh_ids, ids[i]);
    }
    lp_idmap_free(queued);
    if (!fresh_ids.n) { lp_vec_free(&fresh_ids); return; }
    int nh = 0;
    Hold *h = paths_of(pg, fresh_ids.v, (int)fresh_ids.n, &nh);
    lp_vec_free(&fresh_ids);
    if (nh > 0) {
        p->v = realloc(p->v, (p->n + (size_t)nh) * sizeof(Verts));
        for (int k = 0; k < nh; k++) {
            bool fr; size_t s = lp_idmap_put(p->at, &h[k].entity, &fr);
            if (!fr) continue;
            *(int *)lp_idmap_at(p->at, s) = (int)p->n;
            p->v[p->n++] = verts_of(h[k].path, (size_t)h[k].path_len);
        }
    }
    holds_free(h, nh);
}
static void paths_read(PGconn *pg, const lp_id *ids, size_t n, Paths *p){ paths_init(p); paths_add_ids(pg, ids, n, p); }
static const Verts *path_at(const Paths *p, const lp_id *id){
    if (!p->at) return NULL;
    int64_t s = lp_idmap_find(p->at, id);
    return s < 0 ? NULL : &p->v[*(int *)lp_idmap_at(p->at, (size_t)s)];
}
static void paths_free(Paths *p){
    for (size_t i = 0; i < p->n; i++) verts_free(&p->v[i]);
    free(p->v);
    if (p->at) lp_idmap_free(p->at);
    memset(p, 0, sizeof *p);
}

static int parse_uint_str(const char *s, unsigned *out){
    if (!s || !*s || strlen(s) > 9) return 0;
    unsigned v = 0;
    for (const char *p = s; *p; p++) {
        if (*p < '0' || *p > '9') return 0;
        v = v * 10u + (unsigned)(*p - '0');
    }
    *out = v;
    return 1;
}
static int uint_from_reader(Reader *rd, const lp_id *id, unsigned *out){
    char *t = reader_text(rd, id, 32);
    int ok = parse_uint_str(t, out);
    free(t);
    return ok;
}

static int is_meta_verts(const Verts *v){
    if (!v || v->n != 10) return 0;
    int64_t r = lp_tier0_codepoint(T0, &v->id[1]);
    if (r != '1' && r != '0' && r != '=' && r != '*') return 0;
    return cp_is(&v->id[0], '-') || v->said[0] == LP_SAID_TUPLE;
}
static void decode_seat_verts(const Verts *sv, SeatInfo *out){
    memset(out, 0, sizeof *out);
    if (!sv || (sv->n != 2 && sv->n != 3)) return;
    size_t ni = sv->n == 3 ? 1 : 0, ei = sv->n == 3 ? 2 : 1;
    if (sv->n == 3) { out->fide_node = sv->id[0]; out->has_fide = 1; }
    out->name = sv->id[ni];
    out->has_name = !cp_is(&sv->id[ni], '-');
    if (!cp_is(&sv->id[ei], '-')) { out->elo = sv->id[ei]; out->has_elo = 1; }
}

/* Batch-hydrate Game structs (G -> meta -> seats -> seat_0/seat_1 -> composite date/tc/counters/fide) and prefetch texts. */
static void games_load(PGconn *pg, Load *L){
    if (!L->n) return;
    Paths p; paths_init(&p);
    lp_vec(lp_id) q = { 0 };
    for (size_t i = 0; i < L->n; i++) lp_push(&q, L->g[i].rec);
    paths_add_ids(pg, q.v, q.n, &p); q.n = 0;

    for (size_t i = 0; i < L->n; i++) {
        const Verts *v = path_at(&p, &L->g[i].rec);
        if (!v) continue;
        if (v->n >= 2 && v->said[0] == LP_SAID_TUPLE) lp_push(&q, v->id[0]);
        if (v->n >= 2 && v->said[1] == LP_SAID_TUPLE) lp_push(&q, v->id[1]);
    }
    paths_add_ids(pg, q.v, q.n, &p); q.n = 0;

    for (size_t i = 0; i < L->n; i++) {
        Game *g = &L->g[i];
        const Verts *v = path_at(&p, &g->rec);
        if (!v) { g->line = g->rec; g->has_line = 1; g->replays = 1; continue; }
        const Verts *m0 = v->n == 2 && v->said[0] == LP_SAID_TUPLE ? path_at(&p, &v->id[0]) : NULL;
        const Verts *m1 = v->n >= 2 && v->n <= 4 && v->said[1] == LP_SAID_TUPLE ? path_at(&p, &v->id[1]) : NULL;
        if (is_meta_verts(m0)) {
            g->meta = v->id[0]; g->has_meta = 1; g->replays = 0;
        } else if (is_meta_verts(m1)) {
            g->line = v->id[0]; g->has_line = 1; g->replays = 1;
            g->meta = v->id[1]; g->has_meta = 1;
            if (v->n >= 3 && !cp_is(&v->id[2], '-')) { g->clocks = v->id[2]; g->has_clocks = 1; }
            if (v->n >= 4 && !cp_is(&v->id[3], '-')) { g->notes = v->id[3]; g->has_notes = 1; }
        } else {
            g->line = g->rec; g->has_line = 1; g->replays = 1;
        }
        if (!g->has_meta) continue;
        const Verts *mv = is_meta_verts(m0) ? m0 : m1;
        if (!cp_is(&mv->id[0], '-')) lp_push(&q, mv->id[0]);
        if (!cp_is(&mv->id[2], '-') && mv->said[2] == LP_SAID_TUPLE) lp_push(&q, mv->id[2]);
        if (!cp_is(&mv->id[7], '-') && mv->said[7] == LP_SAID_TUPLE) lp_push(&q, mv->id[7]);
        if (!cp_is(&mv->id[9], '-') && mv->said[9] == LP_SAID_TUPLE) lp_push(&q, mv->id[9]);
    }
    paths_add_ids(pg, q.v, q.n, &p); q.n = 0;

    for (size_t i = 0; i < L->n; i++) {
        Game *g = &L->g[i];
        if (!g->has_meta) continue;
        const Verts *mv = path_at(&p, &g->meta);
        if (!mv || mv->n != 10) continue;
        const Verts *sv = !cp_is(&mv->id[0], '-') ? path_at(&p, &mv->id[0]) : NULL;
        if (sv && sv->n == 2) { lp_push(&q, sv->id[0]); lp_push(&q, sv->id[1]); }
    }
    paths_add_ids(pg, q.v, q.n, &p); q.n = 0;

    for (size_t i = 0; i < L->n; i++) {
        Game *g = &L->g[i];
        if (!g->has_meta) continue;
        const Verts *mv = path_at(&p, &g->meta);
        if (!mv || mv->n != 10) continue;
        const Verts *sv = !cp_is(&mv->id[0], '-') ? path_at(&p, &mv->id[0]) : NULL;
        if (sv && sv->n == 2) {
            for (int s = 0; s < 2; s++) {
                const Verts *seat = path_at(&p, &sv->id[s]);
                if (seat && seat->n == 3) lp_push(&q, seat->id[0]);
            }
        }
    }
    paths_add_ids(pg, q.v, q.n, &p); q.n = 0;

    lp_idmap *texts = lp_idmap_new();
    for (size_t i = 0; i < L->n; i++) {
        Game *g = &L->g[i];
        if (!g->has_meta) continue;
        const Verts *mv = path_at(&p, &g->meta);
        if (!mv || mv->n != 10) continue;
        int64_t rcp = lp_tier0_codepoint(T0, &mv->id[1]);
        g->res_cp = rcp > 0 ? (uint32_t)rcp : '*';
        const Verts *sv = !cp_is(&mv->id[0], '-') ? path_at(&p, &mv->id[0]) : NULL;
        if (sv && sv->n == 2) {
            for (int s = 0; s < 2; s++) {
                decode_seat_verts(path_at(&p, &sv->id[s]), &g->seat[s]);
                if (g->seat[s].has_name) lp_idmap_put(texts, &g->seat[s].name, NULL);
                if (g->seat[s].has_elo) lp_idmap_put(texts, &g->seat[s].elo, NULL);
                if (g->seat[s].has_fide) {
                    const Verts *fv = path_at(&p, &g->seat[s].fide_node);
                    if (fv && fv->n == 2) { g->seat[s].fide_node = fv->id[1]; lp_idmap_put(texts, &fv->id[1], NULL); }
                    else g->seat[s].has_fide = 0;
                }
            }
        }
        if (!cp_is(&mv->id[2], '-')) {
            g->date = mv->id[2]; g->has_date = 1;
            const Verts *dv = mv->said[2] == LP_SAID_TUPLE ? path_at(&p, &mv->id[2]) : NULL;
            if (dv && dv->n == 3) { for (int k = 0; k < 3; k++) lp_idmap_put(texts, &dv->id[k], NULL); }
            else lp_idmap_put(texts, &mv->id[2], NULL);
        }
        if (!cp_is(&mv->id[3], '-')) { g->event = mv->id[3]; g->has_event = 1; lp_idmap_put(texts, &g->event, NULL); }
        if (!cp_is(&mv->id[4], '-')) { g->site = mv->id[4]; g->has_site = 1; lp_idmap_put(texts, &g->site, NULL); }
        if (!cp_is(&mv->id[5], '-')) { g->round = mv->id[5]; g->has_round = 1; lp_idmap_put(texts, &g->round, NULL); }
        if (!cp_is(&mv->id[6], '-')) { g->eco = mv->id[6]; g->has_eco = 1; lp_idmap_put(texts, &g->eco, NULL); }
        if (!cp_is(&mv->id[7], '-')) {
            g->tc = mv->id[7]; g->has_tc = 1;
            const Verts *tv = mv->said[7] == LP_SAID_TUPLE ? path_at(&p, &mv->id[7]) : NULL;
            if (tv && tv->n == 2) { lp_idmap_put(texts, &tv->id[0], NULL); lp_idmap_put(texts, &tv->id[1], NULL); }
            else lp_idmap_put(texts, &mv->id[7], NULL);
        }
        if (!cp_is(&mv->id[8], '-')) { g->variant = mv->id[8]; g->has_variant = 1; lp_idmap_put(texts, &g->variant, NULL); }
        if (!cp_is(&mv->id[9], '-')) {
            g->counters = mv->id[9]; g->has_counters = 1;
            const Verts *cv = mv->said[9] == LP_SAID_TUPLE ? path_at(&p, &mv->id[9]) : NULL;
            if (cv && cv->n == 2) { lp_idmap_put(texts, &cv->id[0], NULL); lp_idmap_put(texts, &cv->id[1], NULL); }
            else lp_idmap_put(texts, &mv->id[9], NULL);
        }
    }
    if (lp_idmap_count(texts)) reader_prefetch(L->rd, lp_idmap_keys(texts), lp_idmap_count(texts), 512);
    lp_idmap_free(texts);
    paths_free(&p);
    lp_vec_free(&q);
}

static const char *result_str(const Game *g){
    if (!g->has_meta) return NULL;
    if (g->res_cp == '1') return "1-0";
    if (g->res_cp == '0') return "0-1";
    if (g->res_cp == '=') return "1/2-1/2";
    return "*";
}
static char *date_text(Reader *rd, const Game *g){
    if (!g->has_date) return NULL;
    lp_id parts[4];
    size_t n = reader_parts(rd, &g->date, parts, 4);
    if (n == 3) {
        unsigned y = 0, m = 0, d = 0;
        if (uint_from_reader(rd, &parts[0], &y) && uint_from_reader(rd, &parts[1], &m) && uint_from_reader(rd, &parts[2], &d)) {
            char ys[8], ms[4], ds[4], *out = malloc(16);
            if (y) snprintf(ys, sizeof ys, "%04u", y); else memcpy(ys, "????", 5);
            if (m) snprintf(ms, sizeof ms, "%02u", m); else memcpy(ms, "??", 3);
            if (d) snprintf(ds, sizeof ds, "%02u", d); else memcpy(ds, "??", 3);
            snprintf(out, 16, "%s.%s.%s", ys, ms, ds);
            return out;
        }
    }
    return reader_text(rd, &g->date, 64);
}
static char *tc_text(Reader *rd, const Game *g){
    if (!g->has_tc) return NULL;
    lp_id parts[3];
    size_t n = reader_parts(rd, &g->tc, parts, 3);
    if (n == 2) {
        unsigned base = 0, inc = 0;
        if (uint_from_reader(rd, &parts[0], &base) && uint_from_reader(rd, &parts[1], &inc)) {
            char *out = malloc(32);
            snprintf(out, 32, "%u+%u", base, inc);
            return out;
        }
    }
    return reader_text(rd, &g->tc, 64);
}
static void emit_str(lp_buf *o, const char *s){ lp_json_cstr(o, s); }
static void emit_opt_id(lp_buf *o, Reader *rd, const char *key, const lp_id *id, int has){
    char *t = has ? reader_text(rd, id, 512) : NULL;
    lp_buf_fmt(o, ",\"%s\":", key);
    emit_str(o, t);
    free(t);
}

/* ---- board position decoding and trajectory replay ---- */
typedef struct { char san[16], uci[8], fen[LP_CHESS_FEN + 24]; lp_id position; int white_moved; } Ply;
typedef struct { Ply *p; size_t n; char start[LP_CHESS_FEN + 24]; int complete, custom_start; } Replay;

static void uci_of(const Bd *b, const Mv *m, int c960, char *out){
    int to = m->to;
    if (m->drop) { snprintf(out, 8, "%c@%c%c", "?PNBRQK"[m->drop], 'a' + (m->to & 7), '1' + (m->to >> 3)); return; }
    if (m->castle && !c960) to = (m->to & 7) > (m->from & 7) ? (m->from & ~7) + 6 : (m->from & ~7) + 2;
    char *o = out;
    *o++ = (char)('a' + (m->from & 7)); *o++ = (char)('1' + (m->from >> 3));
    *o++ = (char)('a' + (to & 7));     *o++ = (char)('1' + (to >> 3));
    if (m->promo) *o++ = ".pnbrqk"[m->promo];
    *o = 0;
    (void)b;
}
static void fen_full(const Bd *b, int half, int full, char *out){
    size_t n = lp_chess_fen_of(b, out);
    snprintf(out + n, LP_CHESS_FEN + 24 - n, " %d %d", half, full);
}

/* Decode non-standard start board P0 directly from its canonical [board, side, castling, ep, ...] DAG node. */
static int decode_position_db(PGconn *pg, Reader *rd, const lp_id *pos_id, const LpChessLaw *law, Bd *b){
    Paths p; paths_init(&p);
    paths_add_ids(pg, pos_id, 1, &p);
    const Verts *k = path_at(&p, pos_id);
    if (!k || k->n < 4 || k->n > 7) { paths_free(&p); return 0; }
    int64_t side = lp_tier0_codepoint(T0, &k->id[1]);
    if (side != 'w' && side != 'b') { paths_free(&p); return 0; }
    lp_id lvl1[3] = { k->id[0], k->id[2], k->id[3] };
    paths_add_ids(pg, lvl1, 3, &p);
    const Verts *ps = path_at(&p, &k->id[0]), *cm = path_at(&p, &k->id[2]), *epv = path_at(&p, &k->id[3]);
    if (!ps || ps->n < 2 || !cm || cm->n != 2) { paths_free(&p); return 0; }
    paths_add_ids(pg, ps->id, ps->n, &p);
    lp_vec(lp_id) sqs = { 0 };
    for (size_t i = 0; i < ps->n; i++) {
        const Verts *two = path_at(&p, &ps->id[i]);
        if (!two || two->n != 2) { lp_vec_free(&sqs); paths_free(&p); return 0; }
        lp_push(&sqs, two->id[1]);
    }
    paths_add_ids(pg, sqs.v, sqs.n, &p);
    lp_vec_free(&sqs);

    memset(b, 0, sizeof *b);
    b->side = side == 'b';
    b->ep = -1;
    b->law = law;
    b->c960 = (uint8_t)(law && law->c960);
    for (size_t i = 0; i < ps->n; i++) {
        const Verts *two = path_at(&p, &ps->id[i]);
        const Verts *sq = two ? path_at(&p, &two->id[1]) : NULL;
        if (!two || !sq || sq->n != 2) { paths_free(&p); return 0; }
        int64_t pc = lp_tier0_codepoint(T0, &two->id[0]);
        int64_t fch = lp_tier0_codepoint(T0, &sq->id[0]), rch = lp_tier0_codepoint(T0, &sq->id[1]);
        const char *w = pc > 0 && pc < 128 && pc != '.' ? strchr(PIECES, toupper((int)pc)) : NULL;
        if (!w || fch < 'a' || fch > 'h' || rch < '1' || rch > '8') { paths_free(&p); return 0; }
        b->sq[(rch - '1') * 8 + (fch - 'a')] = (int8_t)((w - PIECES) * (isupper((int)pc) ? 1 : -1));
    }
    for (int c = 0; c < 2; c++) {
        unsigned m = 0;
        if (!uint_from_reader(rd, &cm->id[c], &m)) { paths_free(&p); return 0; }
        int ks = king_sq(b, c);
        for (int f = 0; f < 8; f++) if ((m & (0x80u >> f)) && !(ks >= 0 && (ks & 7) == f)) b->rights[c] |= (uint8_t)(1u << f);
    }
    if (epv && epv->n == 2) {
        int64_t ef = lp_tier0_codepoint(T0, &epv->id[0]), er = lp_tier0_codepoint(T0, &epv->id[1]);
        if (ef >= 'a' && ef <= 'h' && er >= '1' && er <= '8') b->ep = (int8_t)((ef - 'a') + 8 * (er - '1'));
    }
    for (size_t i = 4; i < k->n; i++) {
        char *xt = reader_text(rd, &k->id[i], 128);
        if (!xt || !*xt) { free(xt); paths_free(&p); return 0; }
        size_t xl = strlen(xt);
        if (xt[0] == '+') {
            unsigned wc = 0, bc = 0;
            if (sscanf(xt, "+%u+%u", &wc, &bc) != 2 || wc >= 256 || bc >= 256) { free(xt); paths_free(&p); return 0; }
            b->checks[0] = (uint8_t)wc; b->checks[1] = (uint8_t)bc;
        } else if (strspn(xt, "QRBNPqrbnp") == xl) {
            for (size_t j = 0; j < xl; j++) {
                int kind = (int)(strchr(PIECES, toupper((unsigned char)xt[j])) - PIECES);
                b->hand[islower((unsigned char)xt[j]) ? 1 : 0][kind]++;
            }
        } else {
            if (xl % 2) { free(xt); paths_free(&p); return 0; }
            for (size_t j = 0; j < xl; j += 2) {
                if (xt[j] < 'a' || xt[j] > 'h' || xt[j + 1] < '1' || xt[j + 1] > '8') { free(xt); paths_free(&p); return 0; }
                b->promoted |= (uint64_t)1 << ((xt[j] - 'a') + 8 * (xt[j + 1] - '1'));
            }
        }
        free(xt);
    }
    paths_free(&p);
    return 1;
}

/* Distinguish a multi-position line [P0, P1, ..., Pn] from a 0-ply single position P0 = [board, 'w'|'b', castling, ep]. */
static const Verts *line_verts(const Paths *p, const Game *g){
    const Verts *v = path_at(p, &g->line);
    if (!v || v->n < 2) return NULL;
    for (size_t i = 0; i < v->n; i++) if (v->said[i] != 0 || is_cp(&v->id[i])) return NULL;
    return v;
}

static void game_replay(PGconn *pg, const Load *L, const Paths *lines, const Game *g, Replay *r){
    memset(r, 0, sizeof *r);
    if (!g->has_line) return;
    char *variant = g->has_variant ? reader_text(L->rd, &g->variant, 64) : NULL;
    const LpChessLaw *law = lp_chess_law_named(variant, variant ? strlen(variant) : 0);
    free(variant);
    if (!law) return;

    const Verts *lv = line_verts(lines, g);
    const lp_id *P0 = lv ? &lv->id[0] : &g->line;
    Bd b, std_b;
    board_law_start(&std_b, law);
    lp_id std_id;
    id_position(&std_b, &std_id);

    int half = 0, full = 1;
    if (g->has_counters && g->replays) {
        lp_id cp[3];
        if (reader_parts(L->rd, &g->counters, cp, 3) == 2) {
            unsigned hv = 0, fv = 1;
            if (uint_from_reader(L->rd, &cp[0], &hv)) half = (int)hv;
            if (uint_from_reader(L->rd, &cp[1], &fv) && fv >= 1) full = (int)fv;
        }
    }

    if (lp_id_eq(&std_id, P0)) {
        b = std_b;
        r->custom_start = (half != 0 || full != 1);
    } else {
        if (!decode_position_db(pg, L->rd, P0, law, &b)) return;
        r->custom_start = 1;
    }

    int c960 = law->c960;
    fen_full(&b, half, full, r->start);
    size_t moves = lv ? lv->n - 1 : 0;
    r->p = calloc(moves ? moves : 1, sizeof(Ply));
    for (size_t i = 0; i < moves && i < PLIES_MAX; i++) {
        const lp_id *next = &lv->id[i + 1];
        Mv all[LP_CHESS_MOVES]; int na = 0;
        int k = lp_chess_find_transition(&b, next, all, &na);
        if (k < 0) return;
        Ply *p = &r->p[r->n];
        p->white_moved = !b.side;
        san_of(&b, &all[k], all, na, 1, p->san);
        uci_of(&b, &all[k], c960, p->uci);
        int zero = !all[k].castle && !all[k].drop && (b.sq[all[k].to] || all[k].ep || abs(b.sq[all[k].from]) == PAWN);
        make(&b, &all[k]);
        half = zero ? 0 : half + 1;
        if (!b.side) full++;
        fen_full(&b, half, full, p->fen);
        p->position = *next;
        r->n++;
    }
    r->complete = (r->n == moves);
}

static void emit_movetext(lp_buf *o, const Replay *r, const char *result){
    lp_buf s = { 0 };
    int full = 1, black_first = 0;
    { const char *f = r->start; for (int k = 0; k < 5 && f; k++) { f = strchr(f, ' '); if (f) f++; } if (f) full = atoi(f); if (full < 1) full = 1;
      const char *side = strchr(r->start, ' '); black_first = side && side[1] == 'b'; }
    for (size_t i = 0; i < r->n; i++) {
        if (r->p[i].white_moved) lp_buf_fmt(&s, "%s%d. ", i ? " " : "", full);
        else if (i == 0 && black_first) lp_buf_fmt(&s, "%d... ", full);
        else lp_buf_byte(&s, ' ');
        lp_buf_cstr(&s, r->p[i].san);
        if (!r->p[i].white_moved) full++;
    }
    if (result && *result) lp_buf_fmt(&s, "%s%s", s.n ? " " : "", result);
    lp_buf_byte(&s, 0);
    emit_str(o, (const char *)s.b);
    lp_buf_free(&s);
}

static void lines_read(PGconn *pg, const Load *L, const size_t *page, size_t n, Paths *p){
    lp_vec(lp_id) want = { 0 };
    for (size_t i = 0; i < n; i++) if (L->g[page[i]].has_line) lp_push(&want, L->g[page[i]].line);
    paths_read(pg, want.v, want.n, p);
    lp_vec_free(&want);
}

/* Format integer deciseconds from G's clocks vector back to PGN [%clk H:MM:SS(.d)] strings. */
static char *format_ds_clock(unsigned ds){
    unsigned secs = ds / 10u, tenths = ds % 10u;
    unsigned h = secs / 3600u, m = (secs % 3600u) / 60u, s = secs % 60u;
    char *out = malloc(32);
    if (tenths) snprintf(out, 32, "%u:%02u:%02u.%u", h, m, s, tenths);
    else snprintf(out, 32, "%u:%02u:%02u", h, m, s);
    return out;
}
static void clocks_read(PGconn *pg, Reader *rd, const Game *g, char **out, size_t nply){
    if (!g->has_clocks || !nply) return;
    if (nply == 1) {
        unsigned ds = 0;
        if (!cp_is(&g->clocks, '-') && uint_from_reader(rd, &g->clocks, &ds)) out[0] = format_ds_clock(ds);
        return;
    }
    Paths cp; paths_read(pg, &g->clocks, 1, &cp);
    const Verts *cv = path_at(&cp, &g->clocks);
    if (cv && cv->n == nply) {
        lp_idmap *want = lp_idmap_new();
        for (size_t i = 0; i < nply; i++) if (!cp_is(&cv->id[i], '-')) lp_idmap_put(want, &cv->id[i], NULL);
        if (lp_idmap_count(want)) reader_prefetch(rd, lp_idmap_keys(want), lp_idmap_count(want), 32);
        lp_idmap_free(want);
        for (size_t i = 0; i < nply; i++) {
            unsigned ds = 0;
            if (!cp_is(&cv->id[i], '-') && uint_from_reader(rd, &cv->id[i], &ds)) out[i] = format_ds_clock(ds);
        }
    }
    paths_free(&cp);
}

static void emit_row(lp_buf *o, PGconn *pg, const Load *L, const Paths *lines, const Game *g, int player){
    char hex[33]; api_hex_id(&g->rec, hex); lp_buf_fmt(o, "{\"id\":\"%s\",\"line\":", hex);
    if (g->has_line) { api_hex_id(&g->line, hex); lp_buf_fmt(o, "\"%s\"", hex); } else lp_buf_cstr(o, "null");
    emit_opt_id(o, L->rd, "white", &g->seat[0].name, g->seat[0].has_name);
    emit_opt_id(o, L->rd, "black", &g->seat[1].name, g->seat[1].has_name);
    const char *result = result_str(g);
    lp_buf_cstr(o, ",\"result\":"); emit_str(o, result);
    char *dt = date_text(L->rd, g); lp_buf_cstr(o, ",\"date\":"); emit_str(o, dt); free(dt);
    emit_opt_id(o, L->rd, "event", &g->event, g->has_event);
    emit_opt_id(o, L->rd, "eco", &g->eco, g->has_eco);
    if (player) {
        int w = g->side == 0;
        const SeatInfo *opp = &g->seat[w ? 1 : 0];
        char *opp_name = opp->has_name ? reader_text(L->rd, &opp->name, 512) : NULL;
        lp_buf_fmt(o, ",\"as_white\":%s,\"opponent\":", w ? "true" : "false"); emit_str(o, opp_name); free(opp_name);
        int outcome = -1;
        if (g->res_cp == '=') outcome = 1;
        else if (g->res_cp == '1') outcome = w ? 2 : 0;
        else if (g->res_cp == '0') outcome = w ? 0 : 2;
        if (outcome >= 0) lp_buf_fmt(o, ",\"outcome\":%d", outcome); else lp_buf_cstr(o, ",\"outcome\":null");
    }
    lp_buf_cstr(o, ",\"movetext\":");
    if (g->replays) {
        Replay r; game_replay(pg, L, lines, g, &r);
        if (r.complete) emit_movetext(o, &r, result); else lp_buf_cstr(o, "null");
        free(r.p);
    } else lp_buf_cstr(o, "null");
    lp_buf_byte(o, '}');
}

static const Load *SORT_L; static char **SORT_DATE;
static int newest_first(const void *a, const void *b){
    size_t x = *(const size_t *)a, y = *(const size_t *)b;
    const char *dx = SORT_DATE[x] ? SORT_DATE[x] : "", *dy = SORT_DATE[y] ? SORT_DATE[y] : "";
    int c = strcmp(dy, dx); if (c) return c;
    const Game *gx = &SORT_L->g[x], *gy = &SORT_L->g[y];
    if (gx->order != gy->order) return gx->order > gy->order ? -1 : 1;
    return memcmp(&gx->rec, &gy->rec, 16);
}
static size_t *ordered(const Load *L){
    size_t *idx = malloc(sizeof(size_t) * (L->n ? L->n : 1));
    char **date = calloc(L->n ? L->n : 1, sizeof(char *));
    for (size_t i = 0; i < L->n; i++) { idx[i] = i; date[i] = date_text(L->rd, &L->g[i]); }
    SORT_L = L; SORT_DATE = date; qsort(idx, L->n, sizeof(size_t), newest_first);
    for (size_t i = 0; i < L->n; i++) free(date[i]);
    free(date);
    return idx;
}
static void load_free(Load *L){ free(L->g); if (L->rd) reader_free(L->rd); memset(L, 0, sizeof *L); }
static Game *game_add(Load *L, const lp_id *rec){
    if (L->n == L->cap) { L->cap = L->cap ? L->cap * 2 : 256; L->g = realloc(L->g, sizeof(Game) * L->cap); }
    Game *g = &L->g[L->n++]; memset(g, 0, sizeof *g); g->rec = *rec; g->side = -1; return g;
}
static void emit_page(lp_buf *o, PGconn *pg, const Load *L, const size_t *idx, int offset, int limit, int player){
    size_t from = (size_t)offset < L->n ? (size_t)offset : L->n;
    size_t to = from + (size_t)limit < L->n ? from + (size_t)limit : L->n;
    Paths lines; lines_read(pg, L, idx + from, to - from, &lines);
    lp_buf_byte(o, '[');
    for (size_t i = from; i < to; i++) { if (i > from) lp_buf_byte(o, ','); emit_row(o, pg, L, &lines, &L->g[idx[i]], player); }
    lp_buf_byte(o, ']');
    paths_free(&lines);
}

/* Upward GIN walk: candidate seat IDs -> seats = [seat_0, seat_1] -> meta (10 slots, endpoint=1) -> game G. */
typedef struct { lp_id rec; int owner; int side; } SeatGameHit;
typedef lp_vec(SeatGameHit) SeatGameHits;
static void games_from_seats(PGconn *pg, const lp_id *seat_ids, const int *seat_owner, size_t nseats, SeatGameHits *out, uint8_t *owner_capped){
    if (!nseats) return;
    lp_vec(lp_id) pairs = { 0 }; lp_vec(int) pair_owner = { 0 }; lp_vec(int) pair_side = { 0 };
    { int nh = 0, *hub = calloc(nseats * 2, sizeof(int));
      Hold *h = holds_capped(pg, seat_ids, (int)nseats, CHESS_CAP, 0, 2, hub, &nh);
      for (size_t i = 0; i < nseats; i++) if (owner_capped && hub[2 * i + 1]) owner_capped[seat_owner[i]] = 1;
      for (int k = 0; k < nh; k++) {
          if (h[k].src < 0 || (size_t)h[k].src >= nseats || h[k].claim) continue;
          Verts v = verts_of(h[k].path, (size_t)h[k].path_len);
          if (v.n == 2 && v.said[0] == LP_SAID_TUPLE && v.said[1] == LP_SAID_TUPLE) {
              for (int s = 0; s < 2; s++) if (lp_id_eq(&v.id[s], &seat_ids[h[k].src])) {
                  lp_push(&pairs, h[k].entity);
                  lp_push(&pair_owner, seat_owner[h[k].src]);
                  lp_push(&pair_side, s);
              }
          }
          verts_free(&v);
      }
      holds_free(h, nh); free(hub); }
    if (!pairs.n) { lp_vec_free(&pairs); lp_vec_free(&pair_owner); lp_vec_free(&pair_side); return; }

    /* Deduplicate seats IDs per (owner, seats_id) before probing meta. */
    lp_vec(lp_id) u_pairs = { 0 }; lp_idmap *pair_idx = lp_idmap_sized(sizeof(int));
    for (size_t i = 0; i < pairs.n; i++) {
        bool fr; size_t at = lp_idmap_put(pair_idx, &pairs.v[i], &fr);
        if (fr) { *(int *)lp_idmap_at(pair_idx, at) = (int)u_pairs.n; lp_push(&u_pairs, pairs.v[i]); }
    }

    lp_vec(lp_id) metas = { 0 }; lp_vec(int) meta_u = { 0 };
    { int nh = 0, *hub = calloc(u_pairs.n * 2, sizeof(int));
      Hold *h = holds_endpoint(pg, u_pairs.v, (int)u_pairs.n, CHESS_CAP, 0, 2, 1, hub, &nh);
      for (size_t i = 0; i < pairs.n; i++) {
          int u = *(int *)lp_idmap_at(pair_idx, (size_t)lp_idmap_find(pair_idx, &pairs.v[i]));
          if (owner_capped && hub[2 * u + 1]) owner_capped[pair_owner.v[i]] = 1;
      }
      for (int k = 0; k < nh; k++) {
          if (h[k].src < 0 || (size_t)h[k].src >= u_pairs.n || h[k].claim) continue;
          Verts v = verts_of(h[k].path, (size_t)h[k].path_len);
          if (is_meta_verts(&v) && lp_id_eq(&v.id[0], &u_pairs.v[h[k].src])) {
              lp_push(&metas, h[k].entity);
              lp_push(&meta_u, h[k].src);
          }
          verts_free(&v);
      }
      holds_free(h, nh); free(hub); }

    lp_vec(lp_id) u_metas = { 0 }; lp_idmap *meta_idx = lp_idmap_sized(sizeof(int));
    for (size_t i = 0; i < metas.n; i++) {
        bool fr; size_t at = lp_idmap_put(meta_idx, &metas.v[i], &fr);
        if (fr) { *(int *)lp_idmap_at(meta_idx, at) = (int)u_metas.n; lp_push(&u_metas, metas.v[i]); }
    }

    if (u_metas.n) {
        int nh = 0, *hub = calloc(u_metas.n * 2, sizeof(int));
        Hold *h = holds_capped(pg, u_metas.v, (int)u_metas.n, CHESS_CAP, 0, 2, hub, &nh);
        /* Map u_meta -> list of metas indices -> u_pair -> pairs indices. */
        for (int k = 0; k < nh; k++) {
            if (h[k].src < 0 || (size_t)h[k].src >= u_metas.n || h[k].claim) continue;
            Verts v = verts_of(h[k].path, (size_t)h[k].path_len);
            int valid_g = (v.n >= 2 && v.n <= 4 && v.said[1] == LP_SAID_TUPLE && lp_id_eq(&v.id[1], &u_metas.v[h[k].src]))
                       || (v.n == 2 && v.said[0] == LP_SAID_TUPLE && lp_id_eq(&v.id[0], &u_metas.v[h[k].src]));
            verts_free(&v);
            if (!valid_g) continue;
            const lp_id *mid = &u_metas.v[h[k].src];
            for (size_t mi = 0; mi < metas.n; mi++) if (lp_id_eq(&metas.v[mi], mid)) {
                const lp_id *pid = &u_pairs.v[meta_u.v[mi]];
                for (size_t pi = 0; pi < pairs.n; pi++) if (lp_id_eq(&pairs.v[pi], pid)) {
                    lp_push(out, ((SeatGameHit){ h[k].entity, pair_owner.v[pi], pair_side.v[pi] }));
                }
            }
        }
        for (size_t mi = 0; mi < metas.n; mi++) {
            int um = *(int *)lp_idmap_at(meta_idx, (size_t)lp_idmap_find(meta_idx, &metas.v[mi]));
            if (owner_capped && hub[2 * um + 1]) {
                const lp_id *pid = &u_pairs.v[meta_u.v[mi]];
                for (size_t pi = 0; pi < pairs.n; pi++) if (lp_id_eq(&pairs.v[pi], pid)) owner_capped[pair_owner.v[pi]] = 1;
            }
        }
        holds_free(h, nh); free(hub);
    }

    lp_idmap_free(meta_idx); lp_vec_free(&u_metas); lp_vec_free(&metas); lp_vec_free(&meta_u);
    lp_idmap_free(pair_idx); lp_vec_free(&u_pairs);
    lp_vec_free(&pairs); lp_vec_free(&pair_owner); lp_vec_free(&pair_side);
}

/* Walk trunks down to record entities [rec, claims...] or rec. */
static void walk_trunks_records(PGconn *pg, const lp_id *trunks, size_t ntrunks, Load *L){
    typedef lp_vec(lp_id) Ids;
    Ids level = { 0 }, held = { 0 };
    lp_idmap *seen = lp_idmap_new();
    for (size_t i = 0; i < ntrunks; i++) {
        bool fresh; lp_idmap_put(seen, &trunks[i], &fresh);
        if (fresh) lp_push(&level, trunks[i]);
    }
    for (int depth = 0; depth < WALK_DEPTH && level.n && held.n < GAMES_MAX; depth++) {
        Paths lp; paths_read(pg, level.v, level.n, &lp); Ids next = { 0 };
        for (size_t i = 0; i < level.n; i++) {
            const Verts *v = path_at(&lp, &level.v[i]); if (!v) continue;
            for (size_t k = 0; k < v->n && held.n < GAMES_MAX; k++) {
                if (v->said[k] != LP_SAID_RECORD && v->said[k] != LP_SAID_HOLDS && !(v->said[k] == 0 && depth < 2 && k > 0)) continue;
                bool fresh; lp_idmap_put(seen, &v->id[k], &fresh); if (!fresh) continue;
                if (v->said[k] == LP_SAID_RECORD) lp_push(&held, v->id[k]); else lp_push(&next, v->id[k]);
            }
        }
        paths_free(&lp); lp_vec_free(&level); level = next;
    }
    lp_vec_free(&level); lp_idmap_free(seen);
    Paths hp; paths_read(pg, held.v, held.n, &hp);
    lp_idmap *rec_once = lp_idmap_new();
    for (size_t i = 0; i < held.n; i++) {
        const Verts *v = path_at(&hp, &held.v[i]); int claims = 0;
        if (v) for (size_t k = 1; k < v->n; k++) claims |= v->said[k] == LP_SAID_CLAIM;
        const lp_id *rid = claims ? &v->id[0] : &held.v[i];
        bool fresh; lp_idmap_put(rec_once, rid, &fresh);
        if (fresh) { Game *g = game_add(L, rid); g->order = (int64_t)i; }
    }
    lp_idmap_free(rec_once); paths_free(&hp); lp_vec_free(&held);
}

/* Probe witness trunks along their single first-record spine (<= 1 ID per witness per depth) to select only witnesses
 * whose records are chess games with player seats, never scanning non-chess corpora (OEWN, Unicode, etc.). */
static void discover_chess_game_witnesses(PGconn *pg, Ids *chess_trunks){
    char err[256];
    PGresult *q = db_call_try(pg, LP_SQL_WITNESS_TRUST_ID_ORDERED, NULL, err, sizeof err);
    if (!q) return;
    int nw = PQntuples(q);
    if (nw <= 0) { PQclear(q); return; }
    lp_id *roots = malloc(sizeof(lp_id) * (size_t)nw);
    lp_id *cur = malloc(sizeof(lp_id) * (size_t)nw);
    uint8_t *done = calloc((size_t)nw, 1); /* 0 walking, 1 found RECORD, 2 dead */
    for (int i = 0; i < nw; i++) roots[i] = cur[i] = *col_id(q, i, 0);
    PQclear(q);

    for (int depth = 0; depth < WALK_DEPTH; depth++) {
        lp_vec(lp_id) ask = { 0 };
        for (int i = 0; i < nw; i++) if (done[i] == 0) lp_push(&ask, cur[i]);
        if (!ask.n) { lp_vec_free(&ask); break; }
        Paths lp; paths_read(pg, ask.v, ask.n, &lp); lp_vec_free(&ask);
        for (int i = 0; i < nw; i++) {
            if (done[i] != 0) continue;
            const Verts *v = path_at(&lp, &cur[i]);
            if (!v || !v->n) { done[i] = 2; continue; }
            int moved = 0;
            for (size_t k = 0; k < v->n; k++) {
                if (v->said[k] == LP_SAID_RECORD) { cur[i] = v->id[k]; done[i] = 1; moved = 1; break; }
                if (v->said[k] == LP_SAID_HOLDS || (v->said[k] == 0 && depth < 2 && k > 0)) { cur[i] = v->id[k]; moved = 1; break; }
            }
            if (!moved) done[i] = 2;
        }
        paths_free(&lp);
    }

    lp_vec(lp_id) sample = { 0 };
    for (int i = 0; i < nw; i++) if (done[i] == 1) lp_push(&sample, cur[i]);
    if (sample.n) {
        Paths sp; paths_init(&sp);
        paths_add_ids(pg, sample.v, sample.n, &sp);
        lp_vec(lp_id) inner = { 0 };
        for (int i = 0; i < nw; i++) if (done[i] == 1) {
            const Verts *v = path_at(&sp, &cur[i]);
            int claims = 0;
            if (v) for (size_t k = 1; k < v->n; k++) claims |= v->said[k] == LP_SAID_CLAIM;
            if (claims) { cur[i] = v->id[0]; lp_push(&inner, cur[i]); }
        }
        paths_add_ids(pg, inner.v, inner.n, &sp); inner.n = 0;
        for (int i = 0; i < nw; i++) if (done[i] == 1) {
            const Verts *gv = path_at(&sp, &cur[i]);
            if (gv && gv->n >= 2 && gv->said[0] == LP_SAID_TUPLE) lp_push(&inner, gv->id[0]);
            if (gv && gv->n >= 2 && gv->said[1] == LP_SAID_TUPLE) lp_push(&inner, gv->id[1]);
        }
        paths_add_ids(pg, inner.v, inner.n, &sp); lp_vec_free(&inner);
        for (int i = 0; i < nw; i++) if (done[i] == 1) {
            const Verts *gv = path_at(&sp, &cur[i]);
            const Verts *m0 = gv && gv->n == 2 && gv->said[0] == LP_SAID_TUPLE ? path_at(&sp, &gv->id[0]) : NULL;
            const Verts *m1 = gv && gv->n >= 2 && gv->n <= 4 && gv->said[1] == LP_SAID_TUPLE ? path_at(&sp, &gv->id[1]) : NULL;
            if (is_meta_verts(m0) || is_meta_verts(m1)) lp_push(chess_trunks, roots[i]);
        }
        paths_free(&sp);
    }
    lp_vec_free(&sample);
    free(roots); free(cur); free(done);
}

/* POST /v1/chess_players {search, limit, offset}: {search, offset, total, players: [{name, games, games_complete}]} */
void on_chess_players(int c, const char *body){
    char search[256] = ""; int limit = 25, offset = 0;
    api_jint(body, "limit", &limit); api_jint(body, "offset", &offset);
    limit = clamp_int(limit, 1, PAGE_MAX); offset = clamp_int(offset, 0, 1 << 30);
    (void)api_jstr(body, "search", search, sizeof search);
    if (!fide_ready()) { api_http(c, 500, "{\"error\":\"tier0\"}"); return; }
    PGconn *pg = api_db(); if (!pg) { unavailable(c); return; }

    typedef struct { lp_id name; long games; uint8_t capped; } PlayerEntry;
    lp_vec(PlayerEntry) roster = { 0 };
    lp_idmap *by_name = lp_idmap_sized(sizeof(int));

    if (*search) {
        lp_id sid;
        if (!lp_text_id(TX, search, strlen(search), &sid)) { api_http(c, 200, "{\"error\":\"search: no text\"}"); return; }
        lp_vec(lp_id) values = { 0 };
        lp_push(&values, sid);
        { int nh = 0, hub[2] = { 0 };
          Hold *h = holds_capped(pg, &sid, 1, SEARCH_CAP, 0, 2, hub, &nh);
          lp_idmap *once = lp_idmap_new(); lp_idmap_put(once, &sid, NULL);
          for (int k = 0; k < nh; k++) {
              if (h[k].claim) continue;
              Verts v = verts_of(h[k].path, (size_t)h[k].path_len);
              int plain = v.n >= 2;
              for (size_t j = 0; plain && j < v.n; j++) if (v.said[j] != 0) plain = 0;
              if (plain) {
                  bool fresh; lp_idmap_put(once, &h[k].entity, &fresh);
                  if (fresh) lp_push(&values, h[k].entity);
              }
              verts_free(&v);
          }
          lp_idmap_free(once); holds_free(h, nh); }

        /* Also support searching by numeric FIDE ID via ["FIDE", digits]. */
        unsigned fide_q = 0;
        if (parse_uint_str(search, &fide_q) && fide_q > 0) {
            lp_id fk[2] = { T_FIDE, sid }, fnode;
            lp_id_compose(fk, 2, &fnode);
            lp_push(&values, fnode);
        }

        lp_vec(lp_id) seat_ids = { 0 }; lp_vec(int) seat_owner = { 0 };
        { int nh = 0, *hub = calloc(values.n * 2, sizeof(int));
          Hold *h = holds_capped(pg, values.v, (int)values.n, NAME_CAP, 0, 2, hub, &nh);
          lp_strmap *seat_seen = lp_strmap_sized(1);
          for (int k = 0; k < nh; k++) {
              if (h[k].src < 0 || (size_t)h[k].src >= values.n || h[k].claim) continue;
              Verts sv = verts_of(h[k].path, (size_t)h[k].path_len);
              if (sv.n == 2 || sv.n == 3) {
                  size_t ni = sv.n == 3 ? 1 : 0;
                  int matched = lp_id_eq(&sv.id[ni], &values.v[h[k].src]) || (sv.n == 3 && lp_id_eq(&sv.id[0], &values.v[h[k].src]));
                  const lp_id *nm = &sv.id[ni];
                  if (matched && !cp_is(nm, '?') && !cp_is(nm, '-')) {
                      bool nf; size_t slot = lp_idmap_put(by_name, nm, &nf);
                      if (nf) {
                          *(int *)lp_idmap_at(by_name, slot) = (int)roster.n;
                          PlayerEntry *pe = lp_vec_add(&roster);
                          memset(pe, 0, sizeof *pe); pe->name = *nm;
                      }
                      int owner = *(int *)lp_idmap_at(by_name, slot);
                      if (hub[2 * h[k].src + 1]) roster.v[owner].capped = 1;
                      uint8_t skey[20]; memcpy(skey, h[k].entity.b, 16); memcpy(skey + 16, &owner, 4);
                      bool sf;
                      lp_strmap_put(seat_seen, skey, sizeof skey, &sf);
                      if (sf) { lp_push(&seat_ids, h[k].entity); lp_push(&seat_owner, owner); }
                  }
              }
              verts_free(&sv);
          }
          lp_strmap_free(seat_seen); holds_free(h, nh); free(hub); }
        lp_vec_free(&values);

        if (seat_ids.n && roster.n) {
            uint8_t *ocap = calloc(roster.n, 1);
            SeatGameHits hits = { 0 };
            games_from_seats(pg, seat_ids.v, seat_owner.v, seat_ids.n, &hits, ocap);
            lp_strmap *og_seen = lp_strmap_sized(1);
            for (size_t i = 0; i < hits.n; i++) {
                uint8_t key[20]; memcpy(key, hits.v[i].rec.b, 16); memcpy(key + 16, &hits.v[i].owner, 4);
                bool fr; lp_strmap_put(og_seen, key, sizeof key, &fr);
                if (fr) roster.v[hits.v[i].owner].games++;
            }
            for (size_t i = 0; i < roster.n; i++) roster.v[i].capped |= ocap[i];
            lp_strmap_free(og_seen); lp_vec_free(&hits); free(ocap);

            /* Prune candidate super-strings that were never in an actual chess game. */
            size_t keep = 0;
            for (size_t i = 0; i < roster.n; i++) if (roster.v[i].games > 0) roster.v[keep++] = roster.v[i];
            roster.n = keep;
        } else {
            roster.n = 0;
        }
        lp_vec_free(&seat_ids); lp_vec_free(&seat_owner);
    } else {
        /* Unfiltered roster: walk chess game witnesses down to game records and tally distinct games per player. */
        Ids trunks = { 0 };
        discover_chess_game_witnesses(pg, &trunks);
        if (trunks.n) {
            Load L = { 0 }; L.rd = reader_new(pg);
            walk_trunks_records(pg, trunks.v, trunks.n, &L);
            games_load(pg, &L);
            for (size_t i = 0; i < L.n; i++) {
                const Game *g = &L.g[i];
                for (int s = 0; s < 2; s++) {
                    if (!g->seat[s].has_name || cp_is(&g->seat[s].name, '?') || cp_is(&g->seat[s].name, '-')) continue;
                    /* Avoid double-counting self-play in one game. */
                    if (s == 1 && g->seat[0].has_name && lp_id_eq(&g->seat[0].name, &g->seat[1].name)) continue;
                    bool nf; size_t slot = lp_idmap_put(by_name, &g->seat[s].name, &nf);
                    if (nf) {
                        *(int *)lp_idmap_at(by_name, slot) = (int)roster.n;
                        PlayerEntry *pe = lp_vec_add(&roster);
                        memset(pe, 0, sizeof *pe); pe->name = g->seat[s].name;
                    }
                    roster.v[*(int *)lp_idmap_at(by_name, slot)].games++;
                }
            }
            load_free(&L);
        }
        lp_vec_free(&trunks);
    }
    lp_idmap_free(by_name);

    size_t *idx = malloc(sizeof(size_t) * (roster.n ? roster.n : 1));
    for (size_t i = 0; i < roster.n; i++) idx[i] = i;
    for (size_t i = 1; i < roster.n; i++) {
        size_t x = idx[i], j = i;
        while (j && (roster.v[idx[j - 1]].games < roster.v[x].games ||
               (roster.v[idx[j - 1]].games == roster.v[x].games && memcmp(&roster.v[idx[j - 1]].name, &roster.v[x].name, 16) > 0))) {
            idx[j] = idx[j - 1]; j--;
        }
        idx[j] = x;
    }

    size_t from = (size_t)offset < roster.n ? (size_t)offset : roster.n;
    size_t to = from + (size_t)limit < roster.n ? from + (size_t)limit : roster.n;
    Reader *rd = reader_new(pg);
    lp_vec(lp_id) shown = { 0 };
    for (size_t i = from; i < to; i++) lp_push(&shown, roster.v[idx[i]].name);
    if (shown.n) reader_prefetch(rd, shown.v, shown.n, 512);

    lp_buf o = { 0 };
    lp_buf_cstr(&o, "{\"search\":"); emit_str(&o, search);
    lp_buf_fmt(&o, ",\"offset\":%d,\"total\":%zu,\"players\":[", offset, roster.n);
    for (size_t i = from; i < to; i++) {
        const PlayerEntry *pe = &roster.v[idx[i]];
        char *t = reader_text(rd, &pe->name, 512);
        lp_buf_fmt(&o, "%s{\"name\":", i > from ? "," : ""); emit_str(&o, t ? t : ""); free(t);
        lp_buf_fmt(&o, ",\"games\":%ld,\"games_complete\":%s}", pe->games, pe->capped ? "false" : "true");
    }
    lp_buf_cstr(&o, "]}");
    api_reply_buf(c, 200, &o);
    reader_free(rd); lp_vec_free(&shown); free(idx); lp_vec_free(&roster);
}

typedef struct { long games, wins, draws, losses, unscored; } Record;
static void emit_record(lp_buf *o, const char *key, const Record *r){
    long scored = r->wins + r->draws + r->losses;
    lp_buf_fmt(o, ",\"%s\":{\"games\":%ld,\"wins\":%ld,\"draws\":%ld,\"losses\":%ld,\"unscored\":%ld,\"score\":",
               key, r->games, r->wins, r->draws, r->losses, r->unscored);
    if (scored) lp_buf_fmt(o, "%.6g}", (double)r->wins + r->draws / 2.0); else lp_buf_cstr(o, "null}");
}

/* POST /v1/chess_player {name, limit, offset, opponents} */
void on_chess_player(int c, const char *body){
    char name[512]; int limit = 25, offset = 0, opponents = 25;
    api_jint(body, "limit", &limit); api_jint(body, "offset", &offset); api_jint(body, "opponents", &opponents);
    limit = clamp_int(limit, 1, PAGE_MAX); offset = clamp_int(offset, 0, 1 << 30); opponents = clamp_int(opponents, 0, PAGE_MAX);
    if (!api_jstr(body, "name", name, sizeof name) || !*name) { api_http(c, 200, "{\"error\":\"name: as a White or Black tag writes it\"}"); return; }
    if (!fide_ready()) { api_http(c, 500, "{\"error\":\"tier0\"}"); return; }
    lp_id value; if (!lp_text_id(TX, name, strlen(name), &value)) { api_http(c, 200, "{\"error\":\"name: no text\"}"); return; }
    PGconn *pg = api_db(); if (!pg) { unavailable(c); return; }

    lp_vec(lp_id) keys = { 0 };
    lp_push(&keys, value);
    unsigned fide_q = 0;
    if (parse_uint_str(name, &fide_q) && fide_q > 0) {
        lp_id fk[2] = { T_FIDE, value }, fnode;
        lp_id_compose(fk, 2, &fnode);
        lp_push(&keys, fnode);
    }

    lp_vec(lp_id) seat_ids = { 0 }; lp_vec(int) seat_owner = { 0 };
    { int nh = 0, hub[4] = { 0 };
      Hold *h = holds_capped(pg, keys.v, (int)keys.n, CHESS_CAP, 0, 2, hub, &nh);
      lp_idmap *once = lp_idmap_new();
      for (int k = 0; k < nh; k++) {
          if (h[k].src < 0 || (size_t)h[k].src >= keys.n || h[k].claim) continue;
          Verts sv = verts_of(h[k].path, (size_t)h[k].path_len);
          int match = (sv.n == 2 && lp_id_eq(&sv.id[0], &value))
                   || (sv.n == 3 && (lp_id_eq(&sv.id[1], &value) || (keys.n == 2 && lp_id_eq(&sv.id[0], &keys.v[1]))));
          verts_free(&sv);
          if (!match) continue;
          bool fr; lp_idmap_put(once, &h[k].entity, &fr);
          if (fr) { lp_push(&seat_ids, h[k].entity); lp_push(&seat_owner, 0); }
      }
      lp_idmap_free(once); holds_free(h, nh); }
    lp_vec_free(&keys);

    Load L = { 0 }; L.rd = reader_new(pg);
    if (seat_ids.n) {
        SeatGameHits hits = { 0 };
        games_from_seats(pg, seat_ids.v, seat_owner.v, seat_ids.n, &hits, NULL);
        lp_idmap *seen_g = lp_idmap_new();
        for (size_t i = 0; i < hits.n && L.n < GAMES_MAX; i++) {
            bool fr; lp_idmap_put(seen_g, &hits.v[i].rec, &fr);
            if (fr) game_add(&L, &hits.v[i].rec)->side = hits.v[i].side;
        }
        lp_idmap_free(seen_g); lp_vec_free(&hits);
    }
    lp_vec_free(&seat_ids); lp_vec_free(&seat_owner);
    games_load(pg, &L);

    Record all = { 0 }, by[2] = { { 0 } };
    typedef struct { lp_id name; Record r; } Rival;
    typedef struct { int rating; long games; } Elo;
    lp_vec(Rival) rivals = { 0 }; lp_vec(Elo) elos = { 0 };
    lp_idmap *rival_at = lp_idmap_sized(sizeof(int));

    for (size_t i = 0; i < L.n; i++) {
        Game *g = &L.g[i]; int s = g->side; if (s < 0) continue;
        Record *x[3] = { &all, &by[s], NULL };
        const SeatInfo *opp = &g->seat[s ? 0 : 1];
        if (opp->has_name && !cp_is(&opp->name, '?')) {
            bool fresh; size_t at = lp_idmap_put(rival_at, &opp->name, &fresh);
            if (fresh) {
                *(int *)lp_idmap_at(rival_at, at) = (int)rivals.n;
                Rival *v = lp_vec_add(&rivals); memset(v, 0, sizeof *v); v->name = opp->name;
            }
            x[2] = &rivals.v[*(int *)lp_idmap_at(rival_at, at)].r;
        }
        if (g->seat[s].has_elo) {
            unsigned ev = 0;
            if (uint_from_reader(L.rd, &g->seat[s].elo, &ev) && ev > 0) {
                int v = (int)ev; size_t k = 0;
                while (k < elos.n && elos.v[k].rating != v) k++;
                if (k == elos.n) { Elo *n = lp_vec_add(&elos); n->rating = v; n->games = 0; }
                elos.v[k].games++;
            }
        }
        for (int j = 0; j < 3; j++) {
            if (!x[j]) continue;
            x[j]->games++;
            if (g->res_cp == '=') x[j]->draws++;
            else if (g->res_cp == '1' || g->res_cp == '0') {
                if ((g->res_cp == '1') == (s == 0)) x[j]->wins++; else x[j]->losses++;
            } else x[j]->unscored++;
        }
    }

    size_t *idx = ordered(&L); lp_buf o = { 0 };
    lp_buf_cstr(&o, "{\"name\":"); emit_str(&o, name);
    emit_record(&o, "overall", &all); emit_record(&o, "as_white", &by[0]); emit_record(&o, "as_black", &by[1]);
    for (size_t i = 1; i < elos.n; i++) {
        Elo x = elos.v[i]; size_t j = i;
        while (j && elos.v[j - 1].rating < x.rating) { elos.v[j] = elos.v[j - 1]; j--; }
        elos.v[j] = x;
    }
    for (size_t i = 1; i < rivals.n; i++) {
        Rival x = rivals.v[i]; size_t j = i;
        while (j && (rivals.v[j - 1].r.games < x.r.games ||
               (rivals.v[j - 1].r.games == x.r.games && memcmp(&rivals.v[j - 1].name, &x.name, 16) > 0))) {
            rivals.v[j] = rivals.v[j - 1]; j--;
        }
        rivals.v[j] = x;
    }
    if (elos.n) lp_buf_fmt(&o, ",\"peak_rating\":%d", elos.v[0].rating); else lp_buf_cstr(&o, ",\"peak_rating\":null");
    lp_buf_cstr(&o, ",\"ratings\":[");
    for (size_t i = 0; i < elos.n; i++) lp_buf_fmt(&o, "%s{\"rating\":%d,\"games\":%ld}", i ? "," : "", elos.v[i].rating, elos.v[i].games);
    lp_buf_fmt(&o, "],\"opponents_total\":%zu,\"opponents\":[", rivals.n);
    for (size_t i = 0; i < rivals.n && i < (size_t)opponents; i++) {
        char *t = reader_text(L.rd, &rivals.v[i].name, 512);
        lp_buf_fmt(&o, "%s{\"name\":", i ? "," : ""); emit_str(&o, t ? t : ""); free(t);
        const Record *rr = &rivals.v[i].r; long scored = rr->wins + rr->draws + rr->losses;
        lp_buf_fmt(&o, ",\"games\":%ld,\"wins\":%ld,\"draws\":%ld,\"losses\":%ld,\"unscored\":%ld,\"score\":",
                   rr->games, rr->wins, rr->draws, rr->losses, rr->unscored);
        if (scored) lp_buf_fmt(&o, "%.6g}", (double)rr->wins + rr->draws / 2.0); else lp_buf_cstr(&o, "null}");
    }
    lp_buf_cstr(&o, "]");
    lp_buf_fmt(&o, ",\"total\":%zu,\"offset\":%d,\"games\":", L.n, offset);
    emit_page(&o, pg, &L, idx, offset, limit, 1);
    lp_buf_byte(&o, '}');
    api_reply_buf(c, 200, &o);
    free(idx); load_free(&L); lp_vec_free(&rivals); lp_vec_free(&elos); lp_idmap_free(rival_at);
}

/* POST /v1/chess_games {witness, limit, offset} */
void on_chess_games(int c, const char *body){
    char witness[256]; int limit = 25, offset = 0;
    api_jint(body, "limit", &limit); api_jint(body, "offset", &offset);
    limit = clamp_int(limit, 1, PAGE_MAX); offset = clamp_int(offset, 0, 1 << 30);
    if (!api_jstr(body, "witness", witness, sizeof witness) || !*witness) { api_http(c, 200, "{\"error\":\"witness: a source's witness, as its recipe names it\"}"); return; }
    if (!fide_ready()) { api_http(c, 500, "{\"error\":\"tier0\"}"); return; }
    PGconn *pg = api_db(); if (!pg) { unavailable(c); return; }

    char err[256];
    PGresult *q = db_call_try(pg, LP_SQL_WITNESS_TRUST_ID_ORDERED, NULL, err, sizeof err);
    if (!q) { unavailable(c); return; }
    lp_vec(lp_id) trunks = { 0 };
    { Reader *names = reader_new(pg);
      for (int r = 0; r < PQntuples(q); r++) {
          lp_id first[2]; const lp_id *w = col_id(q, r, 0);
          char *nm = reader_parts(names, w, first, 2) ? reader_text(names, &first[0], 256) : NULL;
          if (nm && !strcmp(nm, witness)) lp_push(&trunks, *w);
          free(nm);
      }
      reader_free(names); }
    PQclear(q);

    Load L = { 0 }; L.rd = reader_new(pg);
    walk_trunks_records(pg, trunks.v, trunks.n, &L);
    games_load(pg, &L);
    size_t *idx = ordered(&L); lp_buf o = { 0 };
    lp_buf_cstr(&o, "{\"witness\":"); emit_str(&o, witness);
    lp_buf_fmt(&o, ",\"total\":%zu,\"offset\":%d,\"games\":", L.n, offset);
    emit_page(&o, pg, &L, idx, offset, limit, 0);
    lp_buf_byte(&o, '}');
    api_reply_buf(c, 200, &o);
    free(idx); load_free(&L); lp_vec_free(&trunks);
}

static void emit_tag_kv(lp_buf *o, int *first, const char *name, const char *val){
    if (!val) return;
    lp_buf_fmt(o, "%s{\"name\":", *first ? "" : ",");
    *first = 0;
    emit_str(o, name);
    lp_buf_cstr(o, ",\"value\":");
    emit_str(o, val);
    lp_buf_byte(o, '}');
}
static void emit_tag_id(lp_buf *o, Reader *rd, int *first, const char *name, const lp_id *id, int has){
    if (!has) return;
    char *t = reader_text(rd, id, 4096);
    if (t) emit_tag_kv(o, first, name, t);
    free(t);
}

/* POST /v1/chess_game {id} */
void on_chess_game(int c, const char *body){
    char hexin[64];
    if (!api_jstr(body, "id", hexin, sizeof hexin) || !api_valid_id(hexin)) { api_http(c, 200, "{\"error\":\"id: a record's ID\"}"); return; }
    if (!fide_ready()) { api_http(c, 500, "{\"error\":\"tier0\"}"); return; }
    PGconn *pg = api_db(); if (!pg) { unavailable(c); return; }
    lp_id id; lp_id_unhex(hexin, &id);
    { int nh = 0; Hold *h = paths_of(pg, &id, 1, &nh); holds_free(h, nh); if (!nh) { api_http(c, 200, "{\"error\":\"id: no record\"}"); return; } }

    Load L = { 0 }; L.rd = reader_new(pg);
    Game *g = game_add(&L, &id);
    games_load(pg, &L);
    g = &L.g[0];
    size_t one = 0; Paths lines; lines_read(pg, &L, &one, 1, &lines);
    Replay r = { 0 };
    if (g->replays) game_replay(pg, &L, &lines, g, &r);

    lp_buf o = { 0 }; char hex[33];
    api_hex_id(&g->rec, hex); lp_buf_fmt(&o, "{\"id\":\"%s\",\"line\":", hex);
    if (g->has_line) { api_hex_id(&g->line, hex); lp_buf_fmt(&o, "\"%s\"", hex); } else lp_buf_cstr(&o, "null");

    /* Synthesize standard PGN tags at the read edge from the occurrence's structured slots. */
    const char *result = result_str(g);
    char *dt = date_text(L.rd, g), *tc = tc_text(L.rd, g);
    lp_buf_cstr(&o, ",\"tags\":[");
    int first_tag = 1;
    emit_tag_id(&o, L.rd, &first_tag, "Event", &g->event, g->has_event);
    emit_tag_id(&o, L.rd, &first_tag, "Site", &g->site, g->has_site);
    if (dt) emit_tag_kv(&o, &first_tag, "Date", dt);
    emit_tag_id(&o, L.rd, &first_tag, "Round", &g->round, g->has_round);
    emit_tag_id(&o, L.rd, &first_tag, "White", &g->seat[0].name, g->seat[0].has_name);
    emit_tag_id(&o, L.rd, &first_tag, "Black", &g->seat[1].name, g->seat[1].has_name);
    if (result) emit_tag_kv(&o, &first_tag, "Result", result);
    emit_tag_id(&o, L.rd, &first_tag, "WhiteElo", &g->seat[0].elo, g->seat[0].has_elo);
    emit_tag_id(&o, L.rd, &first_tag, "BlackElo", &g->seat[1].elo, g->seat[1].has_elo);
    emit_tag_id(&o, L.rd, &first_tag, "WhiteFideId", &g->seat[0].fide_node, g->seat[0].has_fide);
    emit_tag_id(&o, L.rd, &first_tag, "BlackFideId", &g->seat[1].fide_node, g->seat[1].has_fide);
    emit_tag_id(&o, L.rd, &first_tag, "ECO", &g->eco, g->has_eco);
    if (tc) emit_tag_kv(&o, &first_tag, "TimeControl", tc);
    emit_tag_id(&o, L.rd, &first_tag, "Variant", &g->variant, g->has_variant);
    if (r.custom_start && *r.start) {
        emit_tag_kv(&o, &first_tag, "SetUp", "1");
        emit_tag_kv(&o, &first_tag, "FEN", r.start);
    } else if (!g->replays && g->has_counters) {
        emit_tag_kv(&o, &first_tag, "SetUp", "1");
        emit_tag_id(&o, L.rd, &first_tag, "FEN", &g->counters, 1);
    }
    lp_buf_byte(&o, ']');

    emit_opt_id(&o, L.rd, "white", &g->seat[0].name, g->seat[0].has_name);
    emit_opt_id(&o, L.rd, "black", &g->seat[1].name, g->seat[1].has_name);
    lp_buf_cstr(&o, ",\"result\":"); emit_str(&o, result);
    lp_buf_cstr(&o, ",\"date\":"); emit_str(&o, dt);
    emit_opt_id(&o, L.rd, "event", &g->event, g->has_event);
    emit_opt_id(&o, L.rd, "eco", &g->eco, g->has_eco);
    free(dt); free(tc);

    char **clock = calloc(r.n ? r.n : 1, sizeof(char *));
    if (r.complete) clocks_read(pg, L.rd, g, clock, r.n);
    lp_buf_cstr(&o, ",\"start_fen\":"); emit_str(&o, *r.start ? r.start : NULL);
    lp_buf_cstr(&o, ",\"movetext\":"); if (r.complete) emit_movetext(&o, &r, result); else lp_buf_cstr(&o, "null");
    lp_buf_fmt(&o, ",\"complete\":%s,\"plies\":[", r.complete ? "true" : "false");
    for (size_t i = 0; i < r.n; i++) {
        const Ply *p = &r.p[i]; api_hex_id(&p->position, hex);
        lp_buf_fmt(&o, "%s{\"ply\":%zu,\"san\":\"%s\",\"uci\":\"%s\",\"fen\":\"%s\",\"white_moved\":%s,\"position\":\"%s\",\"clock\":",
                   i ? "," : "", i + 1, p->san, p->uci, p->fen, p->white_moved ? "true" : "false", hex);
        emit_str(&o, clock[i]); lp_buf_byte(&o, '}');
        free(clock[i]);
    }
    lp_buf_cstr(&o, "]}");
    api_reply_buf(c, 200, &o);
    free(clock); free(r.p); paths_free(&lines); load_free(&L);
}
