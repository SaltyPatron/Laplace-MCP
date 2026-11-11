/* Chess reads: the openings a position is named by, and the openings a game passes through.
 *
 * A position's ID is the Engine's (chess_rules.c in laplace_engine_reader: a FEN, or a game's PGN parsed by the
 * engine's own pgn grammar and replayed); nothing here knows a chess rule. An opening book row names the position its
 * line ends in ([P, "opening name", name] and [P, "Encyclopaedia of Chess Openings code", eco], recipes/openings), and
 * its record holds the line [P0, M0, ..., Pn], its route. The read is one containment walk, batched per request:
 *   1. the recorded lines that hold the positions asked about (laplace_holders, each key, occurrences, capped);
 *   2. the positions at or before each one along those lines, with the positions asked about themselves;
 *   3. the claims that hold those positions (laplace_holders, claims, with their standings), kept where the position
 *      is the claim's subject and its predicate the opening name or the ECO code;
 *   4. each named position's own opening line, the shortest recorded line that ends in it (laplace_holders again);
 *   5. the names and codes as text (the reader's batched prefetch).
 * An opening named at or before a position along any recorded line through it is an opening of that position. Its
 * line is its own line and plies that line's moves. A position held by more recorded lines than LINES_CAP (the start
 * position, once games are recorded) is a hub: its own names are read, the routes through it are not. */
#define _GNU_SOURCE
#include "api_internal.h"
#include "os.h"
#include "chess_rules.h"

#define LINES_CAP 20000
#define GAME_PLIES 1024
#define GAMES_MAX 512

typedef struct { lp_id position, line, name, eco; int plies, has_line, has_name, has_eco, stood; lp_rating r; } Named;
typedef struct { int key; lp_id position; int plies; } Route;            /* a position at or before key's along a line */
typedef struct {
    Named *named; size_t nnamed; lp_idmap *at;    /* every named position the read reached; position -> its index */
    Route *routes; size_t nroutes, croutes;
    uint8_t *in_book;                             /* per key: held by a recorded line (not the one excluded), or a hub */
    Reader *rd;
} Book;

static int predicates(lp_id *name, lp_id *eco){
    return TX && lp_text_id(TX, "opening name", 12, name) && lp_text_id(TX, "Encyclopaedia of Chess Openings code", 36, eco);
}
static void book_free(Book *b){
    free(b->named); if (b->at) lp_idmap_free(b->at); free(b->routes); free(b->in_book); if (b->rd) reader_free(b->rd);
    memset(b, 0, sizeof *b);
}
static void route_add(Book *b, int key, const lp_id *position, int plies){
    if (b->nroutes == b->croutes) { b->croutes = b->croutes ? b->croutes * 2 : 256; b->routes = realloc(b->routes, sizeof(Route) * b->croutes); }
    b->routes[b->nroutes++] = (Route){ key, *position, plies };
}
static const Named *named_of(const Book *b, const lp_id *position){
    int64_t slot = lp_idmap_find(b->at, position); return slot < 0 ? NULL : &b->named[*(int *)lp_idmap_at(b->at, (size_t)slot)];
}

/* keys: the positions asked about. routes 1 reads the lines through them; 0 their own names alone (a list of games).
 * exclude: a line that is not evidence of a book (the game's own, when it is recorded), or NULL. */
static int book_read(PGconn *pg, const lp_id *keys, int nkeys, int routes, const lp_id *exclude, Book *b){
    memset(b, 0, sizeof *b); lp_id name_predicate, eco_predicate;
    if (!predicates(&name_predicate, &eco_predicate)) return 0;
    b->in_book = calloc((size_t)(nkeys ? nkeys : 1), 1); b->at = lp_idmap_sized(sizeof(int)); b->rd = reader_new(pg);
    for (int i = 0; i < nkeys; i++) route_add(b, i, &keys[i], -1);                          /* each position names itself */
    if (routes && nkeys) {                                                                  /* 1, 2: the lines through each */
        int *hub = calloc((size_t)nkeys * 2, sizeof(int)), nh = 0;
        Hold *h = holds_capped(pg, keys, nkeys, LINES_CAP, 0, 2, hub, &nh);
        for (int i = 0; i < nkeys; i++) if (hub[2 * i + 1]) b->in_book[i] = 1;
        for (int k = 0; k < nh; k++) {
            int key = h[k].src; if (key < 0 || key >= nkeys || h[k].claim || (exclude && lp_id_eq(&h[k].entity, exclude))) continue;
            Run run = run_of(h[k].path, (size_t)h[k].path_len);
            if (run.n & 1) for (int j = 0; j < run.n; j += 2) if (lp_id_eq(&run.id[j], &keys[key])) {        /* a line: positions at even places */
                b->in_book[key] = 1; for (int p = 0; p <= j; p += 2) route_add(b, key, &run.id[p], p / 2); break; }
            free(run.id);
        }
        holds_free(h, nh); free(hub);
    }
    /* 3: the claims of every position reached, each position once */
    lp_idmap *seen = lp_idmap_new(); lp_vec(lp_id) wanted = { 0 };
    for (size_t i = 0; i < b->nroutes; i++) { bool fresh; lp_idmap_put(seen, &b->routes[i].position, &fresh); if (fresh) lp_push(&wanted, b->routes[i].position); }
    lp_idmap_free(seen);
    lp_idmap *texts = lp_idmap_new(); size_t ncap = 0;
    if (wanted.n) {
        int nh = 0, *hub = calloc(wanted.n * 2, sizeof(int));
        Hold *h = holds_capped(pg, wanted.v, (int)wanted.n, -1, 1, 1, hub, &nh);
        for (int k = 0; k < nh; k++) {
            lp_id part[3]; if (!h[k].claim || lp_path_ids(h[k].path, (size_t)h[k].path_len, part, 3) != 3) continue;
            int is_name = lp_id_eq(&part[1], &name_predicate), is_eco = lp_id_eq(&part[1], &eco_predicate);
            if (!is_name && !is_eco) continue;
            bool fresh; size_t slot = lp_idmap_put(b->at, &part[0], &fresh); int *index = lp_idmap_at(b->at, slot);
            if (fresh) { if (b->nnamed == ncap) { ncap = ncap ? ncap * 2 : 64; b->named = realloc(b->named, sizeof(Named) * ncap); }
                memset(&b->named[b->nnamed], 0, sizeof(Named)); b->named[b->nnamed].position = part[0]; b->named[b->nnamed].plies = -1; *index = (int)b->nnamed++; }
            Named *n = &b->named[*index];
            /* a position two rows name: the name with the higher standing */
            if (is_name && (!n->has_name || (h[k].stood && (!n->stood || h[k].r.rating > n->r.rating)))) { n->name = part[2]; n->has_name = 1; n->stood = h[k].stood; n->r = h[k].r; }
            if (is_eco && !n->has_eco) { n->eco = part[2]; n->has_eco = 1; }
            lp_idmap_put(texts, &part[2], NULL);
        }
        holds_free(h, nh); free(hub);
    }
    lp_vec_free(&wanted);
    /* 4: each named position's own line, the shortest recorded line ending in it */
    if (b->nnamed) {
        lp_id *positions = malloc(sizeof(lp_id) * b->nnamed); for (size_t i = 0; i < b->nnamed; i++) positions[i] = b->named[i].position;
        int nh = 0, *hub = calloc(b->nnamed * 2, sizeof(int));
        Hold *h = holds_capped(pg, positions, (int)b->nnamed, LINES_CAP, 0, 2, hub, &nh);
        for (int k = 0; k < nh; k++) {
            if (h[k].claim || h[k].src < 0 || (size_t)h[k].src >= b->nnamed) continue;
            Named *n = &b->named[h[k].src]; Run run = run_of(h[k].path, (size_t)h[k].path_len);
            if ((run.n & 1) && lp_id_eq(&run.id[run.n - 1], &n->position) && (!n->has_line || run.n / 2 < n->plies)) { n->line = h[k].entity; n->plies = run.n / 2; n->has_line = 1; }
            free(run.id);
        }
        holds_free(h, nh); free(hub); free(positions);
    }
    if (lp_idmap_count(texts)) reader_prefetch(b->rd, lp_idmap_keys(texts), lp_idmap_count(texts), 240);         /* 5 */
    lp_idmap_free(texts);
    return 1;
}

static void emit_text(char **o, size_t *on, size_t *oc, Reader *rd, const lp_id *id, int has){
    if (!has) { api_buf_fmt(o, on, oc, "null"); return; }
    char *text = reader_text(rd, id, 240); api_buf_fmt(o, on, oc, "\""); api_cw_str(o, on, oc, text ? text : ""); api_buf_fmt(o, on, oc, "\""); free(text);
}
/* {line, name, eco, plies[, standing]}: line and plies its own line's, or the route's plies where none is recorded */
static void emit_opening(char **o, size_t *on, size_t *oc, const Book *b, const Named *n, int route_plies, int standing){
    char hex[33];
    if (n->has_line) { api_hex_id(&n->line, hex); api_buf_fmt(o, on, oc, "{\"line\":\"%s\",\"name\":", hex); } else api_buf_fmt(o, on, oc, "{\"line\":null,\"name\":");
    emit_text(o, on, oc, b->rd, &n->name, n->has_name); api_buf_fmt(o, on, oc, ",\"eco\":"); emit_text(o, on, oc, b->rd, &n->eco, n->has_eco);
    int plies = n->has_line ? n->plies : route_plies;
    if (plies >= 0) api_buf_fmt(o, on, oc, ",\"plies\":%d", plies); else api_buf_fmt(o, on, oc, ",\"plies\":null");
    if (!standing) { api_buf_fmt(o, on, oc, "}"); return; }
    if (n->stood) api_buf_fmt(o, on, oc, ",\"standing\":{\"rating\":%.3f,\"deviation\":%.3f}}", n->r.rating, n->r.deviation);
    else api_buf_fmt(o, on, oc, ",\"standing\":null}");
}
typedef struct { const Named *n; int plies; } Shown;
static int shown_order(const void *a, const void *b){
    const Shown *x = a, *y = b; unsigned px = (unsigned)x->plies, py = (unsigned)y->plies;     /* -1: last */
    return px != py ? (px < py ? -1 : 1) : memcmp(&x->n->position, &y->n->position, 16);
}
/* The openings of key: each named position along its routes once, by plies ascending. */
static void emit_openings(char **o, size_t *on, size_t *oc, const Book *b, int key){
    Shown *s = malloc(sizeof(Shown) * (b->nroutes ? b->nroutes : 1)); size_t n = 0; lp_idmap *once = lp_idmap_new();
    for (size_t i = 0; i < b->nroutes; i++) { if (b->routes[i].key != key) continue; const Named *named = named_of(b, &b->routes[i].position); if (!named) continue;
        bool fresh; lp_idmap_put(once, &named->position, &fresh); if (!fresh) continue;
        s[n++] = (Shown){ named, named->has_line ? named->plies : b->routes[i].plies }; }
    qsort(s, n, sizeof(Shown), shown_order);
    api_buf_fmt(o, on, oc, "[");
    for (size_t i = 0; i < n; i++) { if (i) api_buf_fmt(o, on, oc, ","); emit_opening(o, on, oc, b, s[i].n, s[i].plies, 1); }
    api_buf_fmt(o, on, oc, "]"); lp_idmap_free(once); free(s);
}
/* The deepest opening a game reaches: the named position latest along it. */
static void emit_deepest(char **o, size_t *on, size_t *oc, const Book *b, const lp_id *positions, size_t n){
    for (size_t i = n; i-- > 0;) { const Named *named = named_of(b, &positions[i]); if (named) { emit_opening(o, on, oc, b, named, (int)i, 0); return; } }
    api_buf_fmt(o, on, oc, "null");
}
static void db_unavailable(int c){
    char escaped[512], error[640]; api_json_esc(PG ? PQerrorMessage(PG) : "", escaped, sizeof escaped);
    snprintf(error, sizeof error, "{\"error\":\"database\",\"detail\":\"%s\"}", escaped); api_http(c, 503, error);
}

/* POST /v1/chess_position {fen}: {position, openings: [{line, name, eco, plies, standing}], games}. An input that is
 * no position answers {error} (the web host's 400). */
void on_chess_position(int c, const char *body){
    char fen[256]; lp_id position;
    if (!api_jstr(body, "fen", fen, sizeof fen) || !lp_chess_fen_position(fen, strlen(fen), &position)) { api_http(c, 200, "{\"error\":\"fen: no position\"}"); return; }
    PGconn *pg = api_db(); if (!pg) { db_unavailable(c); return; }
    Book b; if (!book_read(pg, &position, 1, 1, NULL, &b)) { book_free(&b); api_http(c, 500, "{\"error\":\"tier0\"}"); return; }
    long games = holds_count(pg, &position, 1, 0);
    char hex[33]; api_hex_id(&position, hex); char *o = NULL; size_t on = 0, oc = 0;
    api_buf_fmt(&o, &on, &oc, "{\"position\":\"%s\",\"openings\":", hex); emit_openings(&o, &on, &oc, &b, 0);
    api_buf_fmt(&o, &on, &oc, ",\"games\":%ld}", games);
    book_free(&b); api_http(c, 200, o ? o : "{\"error\":\"memory\"}"); free(o);
}

/* A game's positions, P0 first: its PGN (or bare movetext) read by the Engine's chess with the engine's own grammars,
 * or a recorded line's positions from its path. 0: it does not replay, or the ID is no recorded line. */
/* A tree-sitter grammar the engine compiled, by name, from LAPLACE_GRAMMARS (else the directory this build was
 * configured with); loaded once. NULL: no such grammar there. */
static const void *chess_grammar(const char *name) {
    static char names[8][32];
    static const void *langs[8];
    static int nlang;
    for (int i = 0; i < nlang; i++) if (!strcmp(names[i], name)) return langs[i];
    char so[512], sym[96];
    const char *dir = getenv("LAPLACE_GRAMMARS");
    snprintf(so, sizeof so, "%s/libtree-sitter-%s" OS_DLL_SUFFIX, dir && *dir ? dir : LPM_GRAMMARS_DEFAULT, name);
    if (!os_exists(so)) return NULL;
    void *h = os_dl_open(so);
    if (!h) return NULL;
    snprintf(sym, sizeof sym, "tree_sitter_%s", name);
    const void *(*fn)(void) = (const void *(*)(void))os_dl_sym(h, sym);
    if (!fn) return NULL;
    const void *lang = fn();
    if (nlang < 8) { snprintf(names[nlang], sizeof names[nlang], "%s", name); langs[nlang++] = lang; }
    return lang;
}

static size_t game_positions(PGconn *pg, const char *pgn, const lp_id *line_id, lp_id *positions, lp_id *line){
    if (pgn) {
        const void *full = chess_grammar("pgn"), *moves = chess_grammar("pgn_moves");
        size_t n = full ? lp_chess_pgn_positions(full, pgn, strlen(pgn), positions, GAME_PLIES + 1, line) : 0;
        if (!n && moves) n = lp_chess_pgn_positions(moves, pgn, strlen(pgn), positions, GAME_PLIES + 1, line);
        return n;
    }
    int np = 0; Hold *paths = paths_of(pg, line_id, 1, &np); size_t n = 0;
    if (np > 0) { Run run = run_of(paths[0].path, (size_t)paths[0].path_len);
        if (run.n & 1) for (int j = 0; j < run.n && n <= GAME_PLIES; j += 2) positions[n++] = run.id[j];
        free(run.id); }
    holds_free(paths, np); *line = *line_id; return n;
}

/* POST /v1/chess_game_openings {pgn} | {line} | {pgns: [...]}: per game {line, plies: [{ply, position, openings}],
 * deepest, out_of_book_at}; for pgns, [{deepest}] in their order (null where a game does not replay). */
void on_chess_game_openings(int c, const char *body){
    PGconn *pg = api_db(); if (!pg) { db_unavailable(c); return; }
    if (api_jvalue(body, "pgns")) {
        char *list[GAMES_MAX]; size_t ngames = api_parse_str_array(body, "pgns", list, GAMES_MAX), total = 0;
        lp_id *all = malloc(sizeof(lp_id) * (ngames ? ngames : 1) * (GAME_PLIES + 1)); size_t *count = calloc(ngames ? ngames : 1, sizeof(size_t));
        for (size_t g = 0; g < ngames; g++) { lp_id line; count[g] = game_positions(pg, list[g], NULL, all + total, &line); total += count[g]; free(list[g]); }
        Book b; if (!book_read(pg, all, (int)total, 0, NULL, &b)) { book_free(&b); free(all); free(count); api_http(c, 500, "{\"error\":\"tier0\"}"); return; }
        char *o = NULL; size_t on = 0, oc = 0, at = 0; api_buf_fmt(&o, &on, &oc, "[");
        for (size_t g = 0; g < ngames; g++) { api_buf_fmt(&o, &on, &oc, "%s{\"deepest\":", g ? "," : ""); emit_deepest(&o, &on, &oc, &b, all + at, count[g]); api_buf_fmt(&o, &on, &oc, "}"); at += count[g]; }
        api_buf_fmt(&o, &on, &oc, "]");
        book_free(&b); free(all); free(count); api_http(c, 200, o ? o : "[]"); free(o); return;
    }
    size_t cap = strlen(body) + 1; char *pgn = malloc(cap), idtext[64]; lp_id line_id, line, *positions = malloc(sizeof(lp_id) * (GAME_PLIES + 1));
    size_t n = 0; int recorded = 0;
    if (api_jstr(body, "pgn", pgn, cap)) n = game_positions(pg, pgn, NULL, positions, &line);
    else if (api_jstr(body, "line", idtext, sizeof idtext) && api_valid_id(idtext)) { lp_id_unhex(idtext, &line_id); n = game_positions(pg, NULL, &line_id, positions, &line); recorded = 1; }
    else { free(pgn); free(positions); api_http(c, 200, "{\"error\":\"pgn, line or pgns\"}"); return; }
    free(pgn);
    if (!n) { free(positions); api_http(c, 200, "{\"error\":\"the game does not replay, or the line is no recorded line\"}"); return; }
    Book b; if (!book_read(pg, positions, (int)n, 1, recorded ? &line : NULL, &b)) { book_free(&b); free(positions); api_http(c, 500, "{\"error\":\"tier0\"}"); return; }
    long out = -1; for (size_t i = 1; i < n && out < 0; i++) if (!b.in_book[i]) out = (long)i;     /* the first ply no other recorded line reaches */
    char hex[33]; api_hex_id(&line, hex); char *o = NULL; size_t on = 0, oc = 0;
    api_buf_fmt(&o, &on, &oc, "{\"line\":\"%s\",\"plies\":[", hex);
    for (size_t i = 0; i < n; i++) { api_hex_id(&positions[i], hex);
        api_buf_fmt(&o, &on, &oc, "%s{\"ply\":%zu,\"position\":\"%s\",\"openings\":", i ? "," : "", i, hex); emit_openings(&o, &on, &oc, &b, (int)i); api_buf_fmt(&o, &on, &oc, "}"); }
    api_buf_fmt(&o, &on, &oc, "],\"deepest\":"); emit_deepest(&o, &on, &oc, &b, positions, n);
    if (out >= 0) api_buf_fmt(&o, &on, &oc, ",\"out_of_book_at\":%ld}", out); else api_buf_fmt(&o, &on, &oc, ",\"out_of_book_at\":null}");
    book_free(&b); free(positions); api_http(c, 200, o ? o : "{\"error\":\"memory\"}"); free(o);
}

/* ---- the tablebase: what a position's recorded verdicts are, and its moves' (recipes/syzygy in the Engine)
 *
 * A Syzygy table's every position is [P, "win/draw/loss", verdict] and, where it is no draw, [P, "distance to zeroing",
 * plies], attested by the tablebase's witness; a position a game stands in is the same P, so the read is the claims that
 * hold P. The tables give no moves: the Engine's chess gives the legal moves and the position each makes
 * (lp_chess_successors), and every one of those positions' verdicts is read in the same batch, one containment probe
 * for the position and all its successors. A move is as good for the side making it as its position is bad for the side
 * to move there: the moves are ordered so, the best first; among wins the one that reaches a zeroing move soonest (a
 * capture or pawn move that keeps the win is that move), among losses the latest, as Fathom ranks a root's moves. */
#define TB_MOVES 256
#define TB_HUB 4096                         /* a position held by more claims is none of a tablebase's (the start position) */
typedef struct { int wdl, has_wdl, has_dtz; lp_id dtz; } Verdict;
static const char *const TB_WDL[5] = { "loss", "blessed loss", "draw", "cursed win", "win" };
static int verdict_dtz(Reader *rd, const Verdict *v, int *out){
    if (!v->has_dtz) return 0; char *t = reader_text(rd, &v->dtz, 32); if (!t) return 0;
    char *end; long n = strtol(t, &end, 10); int ok = end != t && !*end; free(t); if (ok) *out = (int)n; return ok;
}
static void emit_verdict(char **o, size_t *on, size_t *oc, Reader *rd, const Verdict *v){
    int dtz;
    if (v->has_wdl) api_buf_fmt(o, on, oc, "\"wdl\":\"%s\"", TB_WDL[v->wdl]); else api_buf_fmt(o, on, oc, "\"wdl\":null");
    if (verdict_dtz(rd, v, &dtz)) api_buf_fmt(o, on, oc, ",\"dtz\":%d", dtz);
    else api_buf_fmt(o, on, oc, ",\"dtz\":%s", v->has_wdl && v->wdl == 2 ? "0" : "null");     /* a draw has no zeroing to reach */
}
/* How good a move is for its mover: its position's verdict for the other side, negated; unknown last. */
typedef struct { int index, known, wdl, dtz, after; } Ranked;        /* dtz: the mover's, through the move; after: its position's own, unsigned */
static int ranked_order(const void *a, const void *b){
    const Ranked *x = a, *y = b;
    if (x->known != y->known) return y->known - x->known;
    if (!x->known) return x->index - y->index;
    int gx = 2 - x->wdl, gy = 2 - y->wdl;                                       /* the mover's result: 4 a win .. 0 a loss */
    if (gx != gy) return gy - gx;
    if (gx > 2 && x->dtz != y->dtz) return x->dtz - y->dtz;                     /* a win: zeroing soonest */
    if (gx < 2 && x->dtz != y->dtz) return y->dtz - x->dtz;                     /* a loss: latest */
    if (gx > 2 && x->after != y->after) return x->after - y->after;             /* then the position that zeroes again soonest */
    if (gx < 2 && x->after != y->after) return y->after - x->after;
    return x->index - y->index;
}
/* GET /v1/chess_tablebase {fen}: {position, wdl, dtz, moves: [{san, position, wdl, dtz}]}, the moves best first. */
void on_chess_tablebase(int c, const char *body){
    char fen[256]; lp_id keys[TB_MOVES + 1]; LpChessSuccessor next[TB_MOVES];
    if (!api_jstr(body, "fen", fen, sizeof fen)) { api_http(c, 200, "{\"error\":\"fen: no position\"}"); return; }
    int nmoves = lp_chess_successors(fen, strlen(fen), &keys[0], next, TB_MOVES);
    if (nmoves < 0) { api_http(c, 200, "{\"error\":\"fen: no position\"}"); return; }
    for (int i = 0; i < nmoves; i++) keys[i + 1] = next[i].position;
    lp_id rel_wdl, rel_dtz, word[5];
    if (!TX || !lp_text_id(TX, "win/draw/loss", 13, &rel_wdl) || !lp_text_id(TX, "distance to zeroing", 19, &rel_dtz)) { api_http(c, 500, "{\"error\":\"tier0\"}"); return; }
    for (int i = 0; i < 5; i++) lp_text_id(TX, TB_WDL[i], strlen(TB_WDL[i]), &word[i]);
    PGconn *pg = api_db(); if (!pg) { db_unavailable(c); return; }
    int nkeys = nmoves + 1, nh = 0, *hub = calloc((size_t)nkeys * 2, sizeof(int));
    Verdict *v = calloc((size_t)nkeys, sizeof(Verdict));
    Hold *h = holds_capped(pg, keys, nkeys, TB_HUB, 0, 1, hub, &nh);
    lp_vec(lp_id) texts = { 0 };
    for (int k = 0; k < nh; k++) {
        int key = h[k].src; lp_id part[3];
        if (key < 0 || key >= nkeys || !h[k].claim || lp_path_ids(h[k].path, (size_t)h[k].path_len, part, 3) != 3 || !lp_id_eq(&part[0], &keys[key])) continue;
        if (lp_id_eq(&part[1], &rel_wdl)) { for (int w = 0; w < 5; w++) if (lp_id_eq(&part[2], &word[w])) { v[key].wdl = w; v[key].has_wdl = 1; } }
        else if (lp_id_eq(&part[1], &rel_dtz)) { v[key].dtz = part[2]; v[key].has_dtz = 1; lp_push(&texts, part[2]); }
    }
    holds_free(h, nh); free(hub);
    Reader *rd = reader_new(pg); if (texts.n) reader_prefetch(rd, texts.v, texts.n, 32); lp_vec_free(&texts);
    Ranked *r = calloc((size_t)(nmoves ? nmoves : 1), sizeof(Ranked));
    for (int i = 0; i < nmoves; i++) { const Verdict *x = &v[i + 1]; r[i].index = i; r[i].known = x->has_wdl; r[i].wdl = x->wdl - 2; r[i].dtz = 0;
        /* the mover's distance to zeroing through this move (Fathom's root rule): a move that zeroes is 1 itself, any
         * other one more than the distance its position has */
        if (x->has_wdl) { int d = 0; if (verdict_dtz(rd, x, &d)) r[i].after = abs(d); r[i].dtz = next[i].zeroing ? 1 : r[i].after + 1; } }
    qsort(r, (size_t)nmoves, sizeof(Ranked), ranked_order);
    char hex[33]; api_hex_id(&keys[0], hex); char *o = NULL; size_t on = 0, oc = 0;
    api_buf_fmt(&o, &on, &oc, "{\"position\":\"%s\",", hex); emit_verdict(&o, &on, &oc, rd, &v[0]); api_buf_fmt(&o, &on, &oc, ",\"moves\":[");
    for (int i = 0; i < nmoves; i++) { int m = r[i].index; api_hex_id(&next[m].position, hex);
        api_buf_fmt(&o, &on, &oc, "%s{\"san\":\"%s\",\"position\":\"%s\",", i ? "," : "", next[m].san, hex); emit_verdict(&o, &on, &oc, rd, &v[m + 1]); api_buf_fmt(&o, &on, &oc, "}"); }
    api_buf_fmt(&o, &on, &oc, "]}");
    reader_free(rd); free(r); free(v); api_http(c, 200, o ? o : "{\"error\":\"memory\"}"); free(o);
}
