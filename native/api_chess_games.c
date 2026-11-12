/* Chess reads of recorded games: the players a record names, a player's games, one game, and the games a witness holds.
 *
 * A game is what the Engine recorded of it (src/chess.c), nothing more: a record [line, tags, layer], its line
 * [P0, M0, P1, ..., Pn] (a line of no moves its position), its tags [[name, value], ...] as written, each pair a
 * TUPLE vertex; a record that does not replay [tags, movetext]. Under the file each record is held as
 * [record, claims...] (the [P, M, P'] its moves claim), said RECORD, and that is what its witness attests. A player is
 * a name as written in a White or Black tag: no identity is minted from a name and no rating of a player is read (a
 * player's rating is not a standing, Sequence 27). SAN, UCI and FEN are written again at this edge from the line, by
 * the Engine's own rules (chess_rules.c): each move the legal move whose position is the line's next one.
 *
 * Every read is a containment walk, a level a round trip over every ID at once:
 *   players  the name searched, and the texts that hold it (a word of a name), up to the [White|Black, name] pairs
 *   player   the name's two pairs, up to the tag lists that hold them, up to the records that hold those
 *   games    the witnesses whose trunk begins with the name asked, and down each trunk the records its files hold
 *   game     one record
 * and then, for the records reached, their tag lists, their pairs and the pairs' texts (games_load), and for the page
 * shown, their lines (movetext). */
#define _GNU_SOURCE
#include "api_internal.h"
#include "chess_rules.h"

#define CHESS_CAP 200000          /* the holders read of one key; more is a hub, said so */
#define GAMES_MAX 50000           /* the records one read assembles */
#define SEARCH_CAP 20000          /* the texts read that hold a word searched */
#define NAME_CAP 4096             /* the holders read of one such text, its tag pairs among them */
#define PAGE_MAX 200
#define PLIES_MAX 2048
#define WALK_DEPTH 12              /* the levels from a witness's trunk down to its records */

typedef struct { lp_id name, value; uint8_t has_value; } TagPair;
typedef struct {
    lp_id rec, line, tags, layer; uint8_t has_line, has_tags, has_layer, replays;
    size_t t0, nt;                /* its tag pairs, in Load.pairs */
    int64_t order;                /* the place its witness gave it, where read so */
    int side;                     /* the named player's colour: 0 White, 1 Black, -1 none */
} Game;
typedef struct { Game *g; size_t n, cap; lp_vec(TagPair) pairs; Reader *rd; } Load;

static lp_id T_WHITE, T_BLACK, T_RESULT, T_DATE, T_EVENT, T_ECO, T_FEN, T_VARIANT, T_SETUP, T_WHITEELO, T_BLACKELO;
static int tag_ids(void){
    static int done = 0; if (done) return 1; if (!TX) return 0;
    struct { const char *s; lp_id *id; } t[] = { {"White", &T_WHITE}, {"Black", &T_BLACK}, {"Result", &T_RESULT}, {"Date", &T_DATE},
        {"Event", &T_EVENT}, {"ECO", &T_ECO}, {"FEN", &T_FEN}, {"Variant", &T_VARIANT}, {"SetUp", &T_SETUP},
        {"WhiteElo", &T_WHITEELO}, {"BlackElo", &T_BLACKELO} };
    for (size_t i = 0; i < sizeof t / sizeof t[0]; i++) if (!lp_text_id(TX, t[i].s, strlen(t[i].s), t[i].id)) return 0;
    done = 1; return 1;
}
static void unavailable(int c){ api_http(c, 503, "{\"error\":\"database\"}"); }
static int clamp(int v, int lo, int hi){ return v < lo ? lo : v > hi ? hi : v; }

/* A path's vertices, runs written out, with what each is said to be. */
typedef struct { lp_id *id; uint32_t *said; size_t n; } Verts;
static Verts verts_of(const uint8_t *ewkb, size_t len){
    Verts v = { 0 }; size_t nv = lp_path_vertices(ewkb, len, NULL, 0); if (!nv) return v;
    lp_vertex *x = malloc(sizeof(lp_vertex) * nv); lp_path_vertices(ewkb, len, x, nv);
    size_t total = 0; for (size_t i = 0; i < nv; i++) total += x[i].run ? x[i].run : 1;
    v.id = malloc(sizeof(lp_id) * total); v.said = malloc(sizeof(uint32_t) * total);
    for (size_t i = 0; i < nv; i++) for (uint32_t k = 0; k < (x[i].run ? x[i].run : 1); k++) { v.id[v.n] = x[i].id; v.said[v.n++] = x[i].said; }
    free(x); return v;
}
static void verts_free(Verts *v){ free(v->id); free(v->said); memset(v, 0, sizeof *v); }
/* paths_of, as one map: ID -> its vertices. */
typedef struct { lp_idmap *at; Verts *v; size_t n; } Paths;
static void paths_read(PGconn *pg, const lp_id *ids, size_t n, Paths *p){
    memset(p, 0, sizeof *p); p->at = lp_idmap_sized(sizeof(int)); if (!n) return;
    int nh = 0; Hold *h = paths_of(pg, ids, (int)n, &nh); p->v = calloc((size_t)(nh ? nh : 1), sizeof(Verts));
    for (int k = 0; k < nh; k++) { bool fresh; size_t s = lp_idmap_put(p->at, &h[k].entity, &fresh); if (!fresh) continue;
        *(int *)lp_idmap_at(p->at, s) = (int)p->n; p->v[p->n++] = verts_of(h[k].path, (size_t)h[k].path_len); }
    holds_free(h, nh);
}
static const Verts *path_at(const Paths *p, const lp_id *id){ int64_t s = lp_idmap_find(p->at, id); return s < 0 ? NULL : &p->v[*(int *)lp_idmap_at(p->at, (size_t)s)]; }
static void paths_free(Paths *p){ for (size_t i = 0; i < p->n; i++) verts_free(&p->v[i]); free(p->v); if (p->at) lp_idmap_free(p->at); memset(p, 0, sizeof *p); }
static int is_tags(const Verts *v){ if (!v) return 0; for (size_t i = 0; i < v->n; i++) if (v->said[i] == LP_SAID_TUPLE) return 1; return 0; }

/* The records' lines and tags, their tag pairs and the pairs' texts, three levels for every record at once. */
static void games_load(PGconn *pg, Load *L){
    lp_vec(lp_id) want = { 0 };
    for (size_t i = 0; i < L->n; i++) lp_push(&want, L->g[i].rec);
    Paths recs; paths_read(pg, want.v, want.n, &recs); want.n = 0;
    for (size_t i = 0; i < L->n; i++) { const Verts *v = path_at(&recs, &L->g[i].rec); if (v && v->n >= 2) { lp_push(&want, v->id[0]); lp_push(&want, v->id[1]); } }
    Paths parts; paths_read(pg, want.v, want.n, &parts); want.n = 0;
    lp_idmap *pairs = lp_idmap_new();
    for (size_t i = 0; i < L->n; i++) { Game *g = &L->g[i]; const Verts *v = path_at(&recs, &g->rec);
        if (!v) { g->line = g->rec; g->has_line = 1; g->replays = 1; continue; }             /* a position: a game of no moves and no tags */
        const Verts *a = v->n >= 2 ? path_at(&parts, &v->id[0]) : NULL, *b = v->n >= 2 ? path_at(&parts, &v->id[1]) : NULL;
        if (is_tags(a)) { g->tags = v->id[0]; g->has_tags = 1; }                             /* [tags, movetext]: it did not replay */
        else if (v->n >= 2 && v->n <= 3 && is_tags(b)) { g->line = v->id[0]; g->tags = v->id[1]; g->has_line = g->has_tags = g->replays = 1;
            if (v->n == 3) { g->layer = v->id[2]; g->has_layer = 1; } }
        else if (v->n == 2) { g->line = v->id[0]; g->layer = v->id[1]; g->has_line = g->has_layer = g->replays = 1; }   /* [line, layer] */
        else { g->line = g->rec; g->has_line = g->replays = 1; }                               /* the record is its line */
        if (!g->has_tags) continue;
        const Verts *t = lp_id_eq(&g->tags, &v->id[0]) ? a : b;
        g->t0 = L->pairs.n; g->nt = 0;
        for (size_t k = 0; k < t->n; k++) { TagPair *tp = lp_vec_add(&L->pairs); g->nt++;
            if (t->said[k] == LP_SAID_TUPLE) { tp->name = t->id[k]; tp->has_value = 1; bool fresh; lp_idmap_put(pairs, &t->id[k], &fresh); if (fresh) lp_push(&want, t->id[k]); }
            else tp->name = t->id[k]; }
    }
    Paths pp; paths_read(pg, want.v, want.n, &pp); want.n = 0;
    lp_idmap *texts = lp_idmap_new();
    for (size_t i = 0; i < L->pairs.n; i++) { TagPair *tp = &L->pairs.v[i];
        if (tp->has_value) { const Verts *v = path_at(&pp, &tp->name);
            if (v && v->n == 2) { tp->value = v->id[1]; tp->name = v->id[0]; } else tp->has_value = 0; }
        lp_idmap_put(texts, &tp->name, NULL); if (tp->has_value) lp_idmap_put(texts, &tp->value, NULL); }
    if (lp_idmap_count(texts)) reader_prefetch(L->rd, lp_idmap_keys(texts), lp_idmap_count(texts), 512);
    lp_idmap_free(texts); lp_idmap_free(pairs); paths_free(&pp); paths_free(&parts); paths_free(&recs); lp_vec_free(&want);
}
static const TagPair *tag_of(const Load *L, const Game *g, const lp_id *name){
    for (size_t i = 0; i < g->nt; i++) { const TagPair *t = &L->pairs.v[g->t0 + i]; if (t->has_value && lp_id_eq(&t->name, name)) return t; }
    return NULL;
}
static char *tag_text(const Load *L, const Game *g, const lp_id *name){ const TagPair *t = tag_of(L, g, name); return t ? reader_text(L->rd, &t->value, 512) : NULL; }
static void emit_str(char **o, size_t *on, size_t *oc, const char *s){
    if (!s) { api_buf_fmt(o, on, oc, "null"); return; } api_buf_fmt(o, on, oc, "\""); api_cw_str(o, on, oc, s); api_buf_fmt(o, on, oc, "\"");
}
static void emit_tag(char **o, size_t *on, size_t *oc, const Load *L, const Game *g, const char *key, const lp_id *name){
    char *t = tag_text(L, g, name); api_buf_fmt(o, on, oc, ",\"%s\":", key); emit_str(o, on, oc, t); free(t);
}

/* ---- a line written again: SAN, UCI and FEN of each ply, by the Engine's rules */
typedef struct { char san[16], uci[8], fen[112]; lp_id position; int white_moved; } Ply;
typedef struct { Ply *p; size_t n; char start[112]; int complete; } Replay;
static void uci_of(const Bd *b, const Mv *m, int c960, char *out){
    int to = m->to;
    if (m->castle && !c960) to = (m->to & 7) > (m->from & 7) ? (m->from & ~7) + 6 : (m->from & ~7) + 2;   /* a king's own two steps */
    char *o = out; *o++ = (char)('a' + (m->from & 7)); *o++ = (char)('1' + (m->from >> 3)); *o++ = (char)('a' + (to & 7)); *o++ = (char)('1' + (to >> 3));
    if (m->promo) *o++ = ".pnbrqk"[m->promo]; *o = 0; (void)b;
}
/* fen of b with its clocks: halfmove since the last capture or pawn move, the move number */
static void fen_full(const Bd *b, int half, int full, char *out){ size_t n = lp_chess_fen_of(b, out); snprintf(out + n, 112 - n, " %d %d", half, full); }
/* The line's positions replayed from the game's start: its FEN tag's position (Chess960 where its Variant says so), or
 * the standard one; every move the legal one whose position is the next. complete 0: a move no legal move makes. */
static void replay_line(const Verts *line, const lp_id *one, const char *fen, const char *variant, Replay *r){
    memset(r, 0, sizeof *r); Bd b; int c960 = variant && (strstr(variant, "960") || strstr(variant, "Fischer"));
    int half = 0, full = 1;
    if (fen && *fen) { if (!board_fen(&b, fen, strlen(fen), c960)) return;
        const char *s = fen; for (int f = 0; f < 4 && s; f++) { s = strchr(s, ' '); if (s) s++; }
        if (s) { half = atoi(s); s = strchr(s, ' '); if (s) full = atoi(s + 1); if (full < 1) full = 1; } }
    else board_start(&b);
    lp_id p0; id_position(&b, &p0);
    const lp_id *P0 = line ? &line->id[0] : one;
    if (!lp_id_eq(&p0, P0)) return;
    fen_full(&b, half, full, r->start);
    size_t moves = line ? (line->n - 1) / 2 : 0; r->p = calloc(moves ? moves : 1, sizeof(Ply));
    for (size_t i = 0; i < moves && i < PLIES_MAX; i++) {
        const lp_id *next = &line->id[2 * i + 2]; Mv all[256]; int na = gen(&b, all), k = -1;
        for (int j = 0; j < na && k < 0; j++) { Bd t = b; make(&t, &all[j]); lp_id id; id_position(&t, &id); if (lp_id_eq(&id, next)) k = j; }
        if (k < 0) return;
        Ply *p = &r->p[r->n]; p->white_moved = !b.side; san_of(&b, &all[k], all, na, 1, p->san); uci_of(&b, &all[k], c960, p->uci);
        int zero = !all[k].castle && (b.sq[all[k].to] || all[k].ep || abs(b.sq[all[k].from]) == PAWN);
        make(&b, &all[k]); half = zero ? 0 : half + 1; if (!b.side) full++;
        fen_full(&b, half, full, p->fen); p->position = *next; r->n++;
    }
    r->complete = r->n == moves;
}
static void emit_movetext(char **o, size_t *on, size_t *oc, const Replay *r, const char *result){
    char *s = NULL; size_t sn = 0, sc = 0; int full = 1, black_first = 0;
    { const char *f = r->start; for (int k = 0; k < 5 && f; k++) { f = strchr(f, ' '); if (f) f++; } if (f) full = atoi(f); if (full < 1) full = 1;
      const char *side = strchr(r->start, ' '); black_first = side && side[1] == 'b'; }
    for (size_t i = 0; i < r->n; i++) {
        if (r->p[i].white_moved) api_buf_fmt(&s, &sn, &sc, "%s%d. ", i ? " " : "", full);
        else if (i == 0 && black_first) api_buf_fmt(&s, &sn, &sc, "%d... ", full);
        else api_buf_fmt(&s, &sn, &sc, " ");
        api_buf_fmt(&s, &sn, &sc, "%s", r->p[i].san); if (!r->p[i].white_moved) full++;
    }
    if (result && *result) api_buf_fmt(&s, &sn, &sc, "%s%s", sn ? " " : "", result);
    emit_str(o, on, oc, s ? s : ""); free(s);
}
/* The page's lines, one level: id -> vertices. */
static void lines_read(PGconn *pg, const Load *L, const size_t *page, size_t n, Paths *p){
    lp_vec(lp_id) want = { 0 }; for (size_t i = 0; i < n; i++) if (L->g[page[i]].has_line) lp_push(&want, L->g[page[i]].line);
    paths_read(pg, want.v, want.n, p); lp_vec_free(&want);
}
/* A line's own vertices when it is a line ([P0, M0, P1, ...]); NULL for a position, a game of no moves. */
static const Verts *line_verts(const Paths *p, const Game *g){ const Verts *v = path_at(p, &g->line); return v && (v->n & 1) && v->n >= 3 ? v : NULL; }
static void game_replay(const Load *L, const Paths *lines, const Game *g, Replay *r){
    char *fen = tag_text(L, g, &T_FEN), *variant = tag_text(L, g, &T_VARIANT);
    if (g->has_line) replay_line(line_verts(lines, g), &g->line, fen, variant, r); else memset(r, 0, sizeof *r);
    free(fen); free(variant);
}

/* The clock each ply's mover had left, as its comment writes it ([%clk 0:02:59.9] gives "0:02:59.9"), from the record's
 * layer (src/chess.c): an element a ply, the move itself where nothing annotates it, else [move, annotation...], with one
 * element before them for what stands before the first move. Two round trips: the layer, then its annotated elements;
 * then the annotations' texts. out[i]: ply i's clock, or NULL; the caller frees each. */
static void clocks_read(PGconn *pg, Reader *rd, const Game *g, const Verts *line, char **out, size_t nply){
    if (!g->has_layer || !line || !nply) return;
    Paths lp; paths_read(pg, &g->layer, 1, &lp); const Verts *yk = path_at(&lp, &g->layer);
    const lp_id *Y = &g->layer; size_t ny = 1; int lead = 0; Paths first; int has_first = 0;
    if (nply == 1) { if (yk && yk->n == 2) { paths_read(pg, &yk->id[0], 1, &first); has_first = 1; const Verts *e0 = path_at(&first, &yk->id[0]);
            if (e0 && e0->n && lp_id_eq(&e0->id[0], &line->id[0])) { Y = yk->id; ny = 2; lead = 1; } } }
    else if (yk) { Y = yk->id; ny = yk->n; lead = yk->n == nply + 1; }
    lp_vec(lp_id) want = { 0 };
    for (size_t i = 0; i < nply && i + (size_t)lead < ny; i++) if (!lp_id_eq(&Y[i + (size_t)lead], &line->id[2 * i + 1])) lp_push(&want, Y[i + (size_t)lead]);
    Paths ep; paths_read(pg, want.v, want.n, &ep); want.n = 0;
    for (size_t i = 0; i < nply && i + (size_t)lead < ny; i++) { const Verts *e = path_at(&ep, &Y[i + (size_t)lead]);
        if (e && e->n > 1 && lp_id_eq(&e->id[0], &line->id[2 * i + 1])) for (size_t k = 1; k < e->n; k++) lp_push(&want, e->id[k]); }
    if (want.n) reader_prefetch(rd, want.v, want.n, 256);
    for (size_t i = 0; i < nply && i + (size_t)lead < ny; i++) { const Verts *e = path_at(&ep, &Y[i + (size_t)lead]);
        if (!e || e->n < 2 || !lp_id_eq(&e->id[0], &line->id[2 * i + 1])) continue;
        for (size_t k = 1; k < e->n && !out[i]; k++) { char *t = reader_text(rd, &e->id[k], 256); const char *c = t ? strstr(t, "[%clk ") : NULL;
            if (c) { c += 6; size_t n = strcspn(c, "]"); if (c[n] == ']' && n && n < 32) { out[i] = malloc(n + 1); memcpy(out[i], c, n); out[i][n] = 0; } }
            free(t); } }
    lp_vec_free(&want); paths_free(&ep); if (has_first) paths_free(&first); paths_free(&lp);
}

/* A row: the record, its line, the players, the result and the tags a list shows, and its movetext. */
static void emit_row(char **o, size_t *on, size_t *oc, const Load *L, const Paths *lines, const Game *g, int player){
    char hex[33]; api_hex_id(&g->rec, hex); api_buf_fmt(o, on, oc, "{\"id\":\"%s\",\"line\":", hex);
    if (g->has_line) { api_hex_id(&g->line, hex); api_buf_fmt(o, on, oc, "\"%s\"", hex); } else api_buf_fmt(o, on, oc, "null");
    emit_tag(o, on, oc, L, g, "white", &T_WHITE); emit_tag(o, on, oc, L, g, "black", &T_BLACK);
    char *result = tag_text(L, g, &T_RESULT); api_buf_fmt(o, on, oc, ",\"result\":"); emit_str(o, on, oc, result);
    emit_tag(o, on, oc, L, g, "date", &T_DATE); emit_tag(o, on, oc, L, g, "event", &T_EVENT); emit_tag(o, on, oc, L, g, "eco", &T_ECO);
    if (player) {
        int w = g->side == 0; char *opp = tag_text(L, g, w ? &T_BLACK : &T_WHITE);
        api_buf_fmt(o, on, oc, ",\"as_white\":%s,\"opponent\":", w ? "true" : "false"); emit_str(o, on, oc, opp); free(opp);
        int outcome = -1; if (result) { if (!strcmp(result, "1/2-1/2")) outcome = 1; else if (!strcmp(result, "1-0")) outcome = w ? 2 : 0; else if (!strcmp(result, "0-1")) outcome = w ? 0 : 2; }
        if (outcome >= 0) api_buf_fmt(o, on, oc, ",\"outcome\":%d", outcome); else api_buf_fmt(o, on, oc, ",\"outcome\":null");
    }
    api_buf_fmt(o, on, oc, ",\"movetext\":");
    if (g->replays) { Replay r; game_replay(L, lines, g, &r); if (r.complete) emit_movetext(o, on, oc, &r, result); else api_buf_fmt(o, on, oc, "null"); free(r.p); }
    else api_buf_fmt(o, on, oc, "null");
    api_buf_fmt(o, on, oc, "}"); free(result);
}

/* Newest first: the Date tag as written (PGN's YYYY.MM.DD sorts as text), then the witness's own order, then the ID. */
static const Load *SORT_L; static char **SORT_DATE;
static int newest_first(const void *a, const void *b){
    size_t x = *(const size_t *)a, y = *(const size_t *)b; const char *dx = SORT_DATE[x] ? SORT_DATE[x] : "", *dy = SORT_DATE[y] ? SORT_DATE[y] : "";
    int c = strcmp(dy, dx); if (c) return c;
    const Game *gx = &SORT_L->g[x], *gy = &SORT_L->g[y]; if (gx->order != gy->order) return gx->order > gy->order ? -1 : 1;
    return memcmp(&gx->rec, &gy->rec, 16);
}
static size_t *ordered(const Load *L){
    size_t *idx = malloc(sizeof(size_t) * (L->n ? L->n : 1)); char **date = calloc(L->n ? L->n : 1, sizeof(char *));
    for (size_t i = 0; i < L->n; i++) { idx[i] = i; date[i] = tag_text(L, &L->g[i], &T_DATE); }
    SORT_L = L; SORT_DATE = date; qsort(idx, L->n, sizeof(size_t), newest_first);
    for (size_t i = 0; i < L->n; i++) free(date[i]); free(date); return idx;
}
static void load_free(Load *L){ free(L->g); lp_vec_free(&L->pairs); if (L->rd) reader_free(L->rd); memset(L, 0, sizeof *L); }
static Game *game_add(Load *L, const lp_id *rec){
    if (L->n == L->cap) { L->cap = L->cap ? L->cap * 2 : 256; L->g = realloc(L->g, sizeof(Game) * L->cap); }
    Game *g = &L->g[L->n++]; memset(g, 0, sizeof *g); g->rec = *rec; g->side = -1; return g;
}
static void emit_page(char **o, size_t *on, size_t *oc, PGconn *pg, const Load *L, const size_t *idx, int offset, int limit, int player){
    size_t from = (size_t)offset < L->n ? (size_t)offset : L->n, to = from + (size_t)limit < L->n ? from + (size_t)limit : L->n;
    Paths lines; lines_read(pg, L, idx + from, to - from, &lines);
    api_buf_fmt(o, on, oc, "[");
    for (size_t i = from; i < to; i++) { if (i > from) api_buf_fmt(o, on, oc, ","); emit_row(o, on, oc, L, &lines, &L->g[idx[i]], player); }
    api_buf_fmt(o, on, oc, "]"); paths_free(&lines);
}

/* POST /v1/chess_players {search, limit, offset}: {search, offset, total, players: [{name, games, games_complete}]}. The
 * names in a White or Black tag that are the text searched, or hold it (a word of a name, "Carlsen" of "Carlsen,
 * Magnus"); games: the tag lists naming it, the games recorded with it, at most CHESS_CAP a colour. */
void on_chess_players(int c, const char *body){
    char search[256]; int limit = 25, offset = 0; api_jint(body, "limit", &limit); api_jint(body, "offset", &offset);
    limit = clamp(limit, 1, PAGE_MAX); offset = clamp(offset, 0, 1 << 30);
    if (!api_jstr(body, "search", search, sizeof search) || !*search) { api_http(c, 200, "{\"error\":\"search: the name, or a word of it\"}"); return; }
    if (!tag_ids()) { api_http(c, 500, "{\"error\":\"tier0\"}"); return; }
    lp_id sid; if (!lp_text_id(TX, search, strlen(search), &sid)) { api_http(c, 200, "{\"error\":\"search: no text\"}"); return; }
    PGconn *pg = api_db(); if (!pg) { unavailable(c); return; }
    /* 1: the text itself, and every text that holds it */
    lp_vec(lp_id) values = { 0 }; lp_push(&values, sid);
    { int nh = 0, hub[2] = { 0 }; Hold *h = holds_capped(pg, &sid, 1, SEARCH_CAP, 0, 2, hub, &nh);
      lp_idmap *once = lp_idmap_new(); lp_idmap_put(once, &sid, NULL);
      for (int k = 0; k < nh; k++) { bool fresh; lp_idmap_put(once, &h[k].entity, &fresh); if (fresh) lp_push(&values, h[k].entity); }
      lp_idmap_free(once); holds_free(h, nh); }
    /* 2: the [White|Black, value] pairs among their holders */
    typedef struct { lp_id value, pair[2]; uint8_t has[2]; } Named;
    lp_vec(Named) names = { 0 }; lp_vec(lp_id) pairs = { 0 };
    { int nh = 0, *hub = calloc(values.n * 2, sizeof(int)); Hold *h = holds_capped(pg, values.v, (int)values.n, NAME_CAP, 0, 2, hub, &nh);
      lp_idmap *at = lp_idmap_sized(sizeof(int));
      for (int k = 0; k < nh; k++) { lp_id part[3]; if (h[k].src < 0 || (size_t)h[k].src >= values.n || lp_path_ids(h[k].path, (size_t)h[k].path_len, part, 3) != 2) continue;
          int side = lp_id_eq(&part[0], &T_WHITE) ? 0 : lp_id_eq(&part[0], &T_BLACK) ? 1 : -1;
          if (side < 0 || !lp_id_eq(&part[1], &values.v[h[k].src])) continue;
          bool fresh; size_t s = lp_idmap_put(at, &part[1], &fresh);
          if (fresh) { *(int *)lp_idmap_at(at, s) = (int)names.n; Named *n = lp_vec_add(&names); n->value = part[1]; }
          Named *n = &names.v[*(int *)lp_idmap_at(at, s)]; if (!n->has[side]) { n->pair[side] = h[k].entity; n->has[side] = 1; } }
      lp_idmap_free(at); holds_free(h, nh); free(hub); }
    /* 3: the tag lists that hold each pair, counted */
    long *games = calloc(names.n ? names.n : 1, sizeof(long)); uint8_t *capped = calloc(names.n ? names.n : 1, 1);
    int *owner = malloc(sizeof(int) * (names.n * 2 + 1));
    for (size_t i = 0; i < names.n; i++) for (int s = 0; s < 2; s++) if (names.v[i].has[s]) { owner[pairs.n] = (int)i; lp_push(&pairs, names.v[i].pair[s]); }
    if (pairs.n) { int nh = 0, *hub = calloc(pairs.n * 2, sizeof(int)); Hold *h = holds_capped(pg, pairs.v, (int)pairs.n, CHESS_CAP, 0, 2, hub, &nh);
        for (int k = 0; k < nh; k++) if (h[k].src >= 0 && (size_t)h[k].src < pairs.n) games[owner[h[k].src]]++;
        for (size_t i = 0; i < pairs.n; i++) if (hub[2 * i + 1]) capped[owner[i]] = 1;
        holds_free(h, nh); free(hub); }
    /* most games first, then the name's ID: a stable page */
    size_t *idx = malloc(sizeof(size_t) * (names.n ? names.n : 1)); for (size_t i = 0; i < names.n; i++) idx[i] = i;
    for (size_t i = 1; i < names.n; i++) { size_t x = idx[i], j = i;
        while (j && (games[idx[j - 1]] < games[x] || (games[idx[j - 1]] == games[x] && memcmp(&names.v[idx[j - 1]].value, &names.v[x].value, 16) > 0))) { idx[j] = idx[j - 1]; j--; }
        idx[j] = x; }
    size_t from = (size_t)offset < names.n ? (size_t)offset : names.n, to = from + (size_t)limit < names.n ? from + (size_t)limit : names.n;
    Reader *rd = reader_new(pg); lp_vec(lp_id) shown = { 0 }; for (size_t i = from; i < to; i++) lp_push(&shown, names.v[idx[i]].value);
    if (shown.n) reader_prefetch(rd, shown.v, shown.n, 512);
    char *o = NULL; size_t on = 0, oc = 0; api_buf_fmt(&o, &on, &oc, "{\"search\":"); emit_str(&o, &on, &oc, search);
    api_buf_fmt(&o, &on, &oc, ",\"offset\":%d,\"total\":%zu,\"players\":[", offset, names.n);
    for (size_t i = from; i < to; i++) { const Named *n = &names.v[idx[i]]; char *t = reader_text(rd, &n->value, 512);
        api_buf_fmt(&o, &on, &oc, "%s{\"name\":", i > from ? "," : ""); emit_str(&o, &on, &oc, t ? t : ""); free(t);
        api_buf_fmt(&o, &on, &oc, ",\"games\":%ld,\"games_complete\":%s}", games[idx[i]], capped[idx[i]] ? "false" : "true"); }
    api_buf_fmt(&o, &on, &oc, "]}");
    api_http(c, 200, o ? o : "{\"error\":\"memory\"}"); free(o);
    reader_free(rd); lp_vec_free(&shown); free(idx); free(owner); free(games); free(capped); lp_vec_free(&pairs); lp_vec_free(&names); lp_vec_free(&values);
}

/* The records that hold a tag list at its place: [line, tags(, layer)] or [tags, movetext]. side: the colour each list
 * named the player by. */
static void records_of_tags(PGconn *pg, const lp_id *tags, const int *side, size_t n, Load *L){
    if (!n) return; int nh = 0, *hub = calloc(n * 2, sizeof(int)); Hold *h = holds_capped(pg, tags, (int)n, CHESS_CAP, 0, 2, hub, &nh);
    lp_idmap *once = lp_idmap_new();
    for (int k = 0; k < nh && L->n < GAMES_MAX; k++) { if (h[k].src < 0 || (size_t)h[k].src >= n || h[k].claim) continue;
        lp_id part[4]; size_t np = lp_path_ids(h[k].path, (size_t)h[k].path_len, part, 4);
        int at = np >= 2 && np <= 3 && lp_id_eq(&part[1], &tags[h[k].src]) ? 1 : np == 2 && lp_id_eq(&part[0], &tags[h[k].src]) ? 0 : -1;
        if (at < 0) continue;
        bool fresh; lp_idmap_put(once, &h[k].entity, &fresh); if (!fresh) continue;
        game_add(L, &h[k].entity)->side = side[h[k].src]; }
    lp_idmap_free(once); holds_free(h, nh); free(hub);
}
typedef struct { long games, wins, draws, losses, unscored; } Record;
static void emit_record(char **o, size_t *on, size_t *oc, const char *key, const Record *r){
    long scored = r->wins + r->draws + r->losses;
    api_buf_fmt(o, on, oc, ",\"%s\":{\"games\":%ld,\"wins\":%ld,\"draws\":%ld,\"losses\":%ld,\"unscored\":%ld,\"score\":", key, r->games, r->wins, r->draws, r->losses, r->unscored);
    if (scored) api_buf_fmt(o, on, oc, "%.6g}", (double)r->wins + r->draws / 2.0); else api_buf_fmt(o, on, oc, "null}");
}
/* POST /v1/chess_player {name, limit, offset}: {name, overall, as_white, as_black, total, offset, games: [...]}, a
 * record {games, wins, draws, losses, unscored, score} over the games naming name as White or Black, score the wins and
 * half the draws of the scored ones; games newest first, each {id, line, white, black, result, date, event, eco,
 * as_white, opponent, outcome (2 a win, 1 a draw, 0 a loss, null unscored), movetext}; peak_rating and ratings [{rating,
 * games}], what the player's own WhiteElo or BlackElo tag wrote, highest first (a source's rating of a player, never a
 * standing); opponents [{name, games, wins, draws, losses, unscored, score}], the players met, most games first, the
 * first `opponents` of opponents_total. */
void on_chess_player(int c, const char *body){
    char name[512]; int limit = 25, offset = 0, opponents = 25; api_jint(body, "limit", &limit); api_jint(body, "offset", &offset); api_jint(body, "opponents", &opponents);
    limit = clamp(limit, 1, PAGE_MAX); offset = clamp(offset, 0, 1 << 30); opponents = clamp(opponents, 0, PAGE_MAX);
    if (!api_jstr(body, "name", name, sizeof name) || !*name) { api_http(c, 200, "{\"error\":\"name: as a White or Black tag writes it\"}"); return; }
    if (!tag_ids()) { api_http(c, 500, "{\"error\":\"tier0\"}"); return; }
    lp_id value; if (!lp_text_id(TX, name, strlen(name), &value)) { api_http(c, 200, "{\"error\":\"name: no text\"}"); return; }
    PGconn *pg = api_db(); if (!pg) { unavailable(c); return; }
    lp_id pair[2], two[2] = { T_WHITE, value }; lp_id_compose(two, 2, &pair[0]); two[0] = T_BLACK; lp_id_compose(two, 2, &pair[1]);
    /* the tag lists that hold either pair, each with the colour it names the player by */
    lp_vec(lp_id) tags = { 0 }; lp_vec(int) side = { 0 };
    { int nh = 0, hub[4] = { 0 }; Hold *h = holds_capped(pg, pair, 2, CHESS_CAP, 0, 2, hub, &nh); lp_idmap *once = lp_idmap_new();
      for (int k = 0; k < nh; k++) { if (h[k].src < 0 || h[k].src > 1 || h[k].claim) continue; bool fresh; lp_idmap_put(once, &h[k].entity, &fresh);
          if (fresh) { lp_push(&tags, h[k].entity); lp_push(&side, h[k].src); } }
      lp_idmap_free(once); holds_free(h, nh); }
    Load L = { 0 }; L.rd = reader_new(pg); records_of_tags(pg, tags.v, side.v, tags.n, &L); games_load(pg, &L);
    Record all = { 0 }, by[2] = { { 0 } };
    typedef struct { lp_id name; Record r; } Rival; typedef struct { int rating; long games; } Elo;
    lp_vec(Rival) rivals = { 0 }; lp_vec(Elo) elos = { 0 }; lp_idmap *rival_at = lp_idmap_sized(sizeof(int));
    for (size_t i = 0; i < L.n; i++) { Game *g = &L.g[i]; int s = g->side; if (s < 0) continue; char *r = tag_text(&L, g, &T_RESULT);
        Record *x[3] = { &all, &by[s], NULL }; const TagPair *opp = tag_of(&L, g, s ? &T_WHITE : &T_BLACK);
        if (opp) { bool fresh; size_t at = lp_idmap_put(rival_at, &opp->value, &fresh);
            if (fresh) { *(int *)lp_idmap_at(rival_at, at) = (int)rivals.n; Rival *v = lp_vec_add(&rivals); memset(v, 0, sizeof *v); v->name = opp->value; }
            x[2] = &rivals.v[*(int *)lp_idmap_at(rival_at, at)].r; }
        { char *e = tag_text(&L, g, s ? &T_BLACKELO : &T_WHITEELO); int v = e ? atoi(e) : 0; free(e);
          if (v > 0) { size_t k = 0; while (k < elos.n && elos.v[k].rating != v) k++; if (k == elos.n) { Elo *n = lp_vec_add(&elos); n->rating = v; n->games = 0; } elos.v[k].games++; } }
        for (int j = 0; j < 3; j++) { if (!x[j]) continue; x[j]->games++;
            if (r && !strcmp(r, "1/2-1/2")) x[j]->draws++;
            else if (r && (!strcmp(r, "1-0") || !strcmp(r, "0-1"))) { if ((r[0] == '1') == (s == 0)) x[j]->wins++; else x[j]->losses++; }
            else x[j]->unscored++; }
        free(r); }
    size_t *idx = ordered(&L); char *o = NULL; size_t on = 0, oc = 0;
    api_buf_fmt(&o, &on, &oc, "{\"name\":"); emit_str(&o, &on, &oc, name);
    emit_record(&o, &on, &oc, "overall", &all); emit_record(&o, &on, &oc, "as_white", &by[0]); emit_record(&o, &on, &oc, "as_black", &by[1]);
    /* the ratings the player's own Elo tag wrote, highest first, and the players met, most games first then the name's ID */
    for (size_t i = 1; i < elos.n; i++) { Elo x = elos.v[i]; size_t j = i; while (j && elos.v[j - 1].rating < x.rating) { elos.v[j] = elos.v[j - 1]; j--; } elos.v[j] = x; }
    for (size_t i = 1; i < rivals.n; i++) { Rival x = rivals.v[i]; size_t j = i;
        while (j && (rivals.v[j - 1].r.games < x.r.games || (rivals.v[j - 1].r.games == x.r.games && memcmp(&rivals.v[j - 1].name, &x.name, 16) > 0))) { rivals.v[j] = rivals.v[j - 1]; j--; }
        rivals.v[j] = x; }
    if (elos.n) api_buf_fmt(&o, &on, &oc, ",\"peak_rating\":%d", elos.v[0].rating); else api_buf_fmt(&o, &on, &oc, ",\"peak_rating\":null");
    api_buf_fmt(&o, &on, &oc, ",\"ratings\":[");
    for (size_t i = 0; i < elos.n; i++) api_buf_fmt(&o, &on, &oc, "%s{\"rating\":%d,\"games\":%ld}", i ? "," : "", elos.v[i].rating, elos.v[i].games);
    api_buf_fmt(&o, &on, &oc, "],\"opponents_total\":%zu,\"opponents\":[", rivals.n);
    for (size_t i = 0; i < rivals.n && i < (size_t)opponents; i++) { char *t = reader_text(L.rd, &rivals.v[i].name, 512);
        api_buf_fmt(&o, &on, &oc, "%s{\"name\":", i ? "," : ""); emit_str(&o, &on, &oc, t ? t : ""); free(t);
        const Record *rr = &rivals.v[i].r; long scored = rr->wins + rr->draws + rr->losses;
        api_buf_fmt(&o, &on, &oc, ",\"games\":%ld,\"wins\":%ld,\"draws\":%ld,\"losses\":%ld,\"unscored\":%ld,\"score\":", rr->games, rr->wins, rr->draws, rr->losses, rr->unscored);
        if (scored) api_buf_fmt(&o, &on, &oc, "%.6g}", (double)rr->wins + rr->draws / 2.0); else api_buf_fmt(&o, &on, &oc, "null}"); }
    api_buf_fmt(&o, &on, &oc, "]");
    api_buf_fmt(&o, &on, &oc, ",\"total\":%zu,\"offset\":%d,\"games\":", L.n, offset); emit_page(&o, &on, &oc, pg, &L, idx, offset, limit, 1);
    api_buf_fmt(&o, &on, &oc, "}"); api_http(c, 200, o ? o : "{\"error\":\"memory\"}"); free(o);
    free(idx); load_free(&L); lp_vec_free(&tags); lp_vec_free(&side); lp_vec_free(&rivals); lp_vec_free(&elos); lp_idmap_free(rival_at);
}

/* POST /v1/chess_games {witness, limit, offset}: {witness, total, offset, games: [...]}, the records under every witness
 * whose trunk begins with that name (a source's witness, or a witness named at run time: "Laplace" for the games
 * Laplace plays, laplace witness --as Laplace), newest first, each {id, line, white, black, result, date, event, eco,
 * movetext}. */
void on_chess_games(int c, const char *body){
    char witness[256]; int limit = 25, offset = 0; api_jint(body, "limit", &limit); api_jint(body, "offset", &offset);
    limit = clamp(limit, 1, PAGE_MAX); offset = clamp(offset, 0, 1 << 30);
    if (!api_jstr(body, "witness", witness, sizeof witness) || !*witness) { api_http(c, 200, "{\"error\":\"witness: a source's witness, as its recipe names it\"}"); return; }
    if (!tag_ids()) { api_http(c, 500, "{\"error\":\"tier0\"}"); return; }
    PGconn *pg = api_db(); if (!pg) { unavailable(c); return; }
    /* the witnesses whose trunk begins with a record of that name: a source's trunk is [its record, its files...] */
    Args a = { 0 };
    PGresult *q = ask_try(pg, "SELECT witness FROM @extschema@.laplace_witness_trust()", &a);
    if (!q) { args_free(&a); unavailable(c); return; }
    lp_vec(lp_id) trunks = { 0 };
    { Reader *names = reader_new(pg);
      for (int r = 0; r < PQntuples(q); r++) { lp_id first[2]; const lp_id *w = col_id(q, r, 0);
          char *nm = reader_parts(names, w, first, 2) ? reader_text(names, &first[0], 256) : NULL;
          if (nm && !strcmp(nm, witness)) lp_push(&trunks, *w);
          free(nm); }
      reader_free(names); }
    PQclear(q);
    Load L = { 0 }; L.rd = reader_new(pg);
    /* Down each trunk to its records, a level a round trip: a vertex said HOLDS is a part with records below it (a file,
     * a block of a file's content), one said RECORD is a record as its file holds it, [record, its claims...], or the
     * record itself where it claims nothing. A record's place is the order the walk reaches it in: its file's order. */
    { typedef lp_vec(lp_id) Ids; Ids level = { 0 }, held = { 0 }; lp_idmap *seen = lp_idmap_new();
      for (size_t i = 0; i < trunks.n; i++) { bool fresh; lp_idmap_put(seen, &trunks.v[i], &fresh); if (fresh) lp_push(&level, trunks.v[i]); }
      for (int depth = 0; depth < WALK_DEPTH && level.n && held.n < GAMES_MAX; depth++) {
          Paths lp; paths_read(pg, level.v, level.n, &lp); Ids next = { 0 };
          for (size_t i = 0; i < level.n; i++) { const Verts *v = path_at(&lp, &level.v[i]); if (!v) continue;
              for (size_t k = 0; k < v->n && held.n < GAMES_MAX; k++) {
                  if (v->said[k] != LP_SAID_RECORD && v->said[k] != LP_SAID_HOLDS) continue;
                  bool fresh; lp_idmap_put(seen, &v->id[k], &fresh); if (!fresh) continue;
                  if (v->said[k] == LP_SAID_RECORD) lp_push(&held, v->id[k]); else lp_push(&next, v->id[k]); } }
          paths_free(&lp); lp_vec_free(&level); level = next; }
      lp_vec_free(&level); lp_idmap_free(seen);
      Paths hp; paths_read(pg, held.v, held.n, &hp);
      for (size_t i = 0; i < held.n; i++) { const Verts *v = path_at(&hp, &held.v[i]); int claims = 0;
          if (v) for (size_t k = 1; k < v->n; k++) claims |= v->said[k] == LP_SAID_CLAIM;
          Game *g = game_add(&L, claims ? &v->id[0] : &held.v[i]); g->order = (int64_t)i; }
      paths_free(&hp); lp_vec_free(&held); }
    args_free(&a); games_load(pg, &L);
    size_t *idx = ordered(&L); char *o = NULL; size_t on = 0, oc = 0;
    api_buf_fmt(&o, &on, &oc, "{\"witness\":"); emit_str(&o, &on, &oc, witness);
    api_buf_fmt(&o, &on, &oc, ",\"total\":%zu,\"offset\":%d,\"games\":", L.n, offset); emit_page(&o, &on, &oc, pg, &L, idx, offset, limit, 0);
    api_buf_fmt(&o, &on, &oc, "}"); api_http(c, 200, o ? o : "{\"error\":\"memory\"}"); free(o);
    free(idx); load_free(&L); lp_vec_free(&trunks);
}

/* POST /v1/chess_game {id}: {id, line, tags: [{name, value}], white, black, result, date, event, eco, start_fen,
 * movetext, complete, plies: [{ply, san, uci, fen, white_moved, position, clock}]}: a record, its tags as written in their
 * order (value null where a tag has none) and its line replayed. complete false: a move no legal move makes, or a game
 * that did not replay (its movetext, as written, is the record's text). */
void on_chess_game(int c, const char *body){
    char hexin[64]; if (!api_jstr(body, "id", hexin, sizeof hexin) || !api_valid_id(hexin)) { api_http(c, 200, "{\"error\":\"id: a record's ID\"}"); return; }
    if (!tag_ids()) { api_http(c, 500, "{\"error\":\"tier0\"}"); return; }
    PGconn *pg = api_db(); if (!pg) { unavailable(c); return; }
    lp_id id; lp_id_unhex(hexin, &id);
    { int nh = 0; Hold *h = paths_of(pg, &id, 1, &nh); holds_free(h, nh); if (!nh) { api_http(c, 200, "{\"error\":\"id: no record\"}"); return; } }
    Load L = { 0 }; L.rd = reader_new(pg); Game *g = game_add(&L, &id); games_load(pg, &L); g = &L.g[0];
    size_t one = 0; Paths lines; lines_read(pg, &L, &one, 1, &lines);
    char *o = NULL; size_t on = 0, oc = 0; char hex[33];
    api_hex_id(&g->rec, hex); api_buf_fmt(&o, &on, &oc, "{\"id\":\"%s\",\"line\":", hex);
    if (g->has_line) { api_hex_id(&g->line, hex); api_buf_fmt(&o, &on, &oc, "\"%s\"", hex); } else api_buf_fmt(&o, &on, &oc, "null");
    api_buf_fmt(&o, &on, &oc, ",\"tags\":[");
    for (size_t i = 0; i < g->nt; i++) { const TagPair *t = &L.pairs.v[g->t0 + i]; char *nm = reader_text(L.rd, &t->name, 512), *v = t->has_value ? reader_text(L.rd, &t->value, 4096) : NULL;
        api_buf_fmt(&o, &on, &oc, "%s{\"name\":", i ? "," : ""); emit_str(&o, &on, &oc, nm ? nm : ""); api_buf_fmt(&o, &on, &oc, ",\"value\":"); emit_str(&o, &on, &oc, v); api_buf_fmt(&o, &on, &oc, "}"); free(nm); free(v); }
    api_buf_fmt(&o, &on, &oc, "]");
    emit_tag(&o, &on, &oc, &L, g, "white", &T_WHITE); emit_tag(&o, &on, &oc, &L, g, "black", &T_BLACK);
    char *result = tag_text(&L, g, &T_RESULT); api_buf_fmt(&o, &on, &oc, ",\"result\":"); emit_str(&o, &on, &oc, result);
    emit_tag(&o, &on, &oc, &L, g, "date", &T_DATE); emit_tag(&o, &on, &oc, &L, g, "event", &T_EVENT); emit_tag(&o, &on, &oc, &L, g, "eco", &T_ECO);
    Replay r = { 0 }; if (g->replays) game_replay(&L, &lines, g, &r);
    char **clock = calloc(r.n ? r.n : 1, sizeof(char *)); if (r.complete) clocks_read(pg, L.rd, g, line_verts(&lines, g), clock, r.n);
    api_buf_fmt(&o, &on, &oc, ",\"start_fen\":"); emit_str(&o, &on, &oc, *r.start ? r.start : NULL);
    api_buf_fmt(&o, &on, &oc, ",\"movetext\":"); if (r.complete) emit_movetext(&o, &on, &oc, &r, result); else api_buf_fmt(&o, &on, &oc, "null");
    api_buf_fmt(&o, &on, &oc, ",\"complete\":%s,\"plies\":[", r.complete ? "true" : "false");
    for (size_t i = 0; i < r.n; i++) { const Ply *p = &r.p[i]; api_hex_id(&p->position, hex);
        api_buf_fmt(&o, &on, &oc, "%s{\"ply\":%zu,\"san\":\"%s\",\"uci\":\"%s\",\"fen\":\"%s\",\"white_moved\":%s,\"position\":\"%s\",\"clock\":", i ? "," : "", i + 1, p->san, p->uci, p->fen, p->white_moved ? "true" : "false", hex);
        emit_str(&o, &on, &oc, clock[i]); api_buf_fmt(&o, &on, &oc, "}"); free(clock[i]); }
    api_buf_fmt(&o, &on, &oc, "]}"); api_http(c, 200, o ? o : "{\"error\":\"memory\"}"); free(o);
    free(clock); free(r.p); free(result); paths_free(&lines); load_free(&L);
}
