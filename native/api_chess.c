/* Chess reads: the openings a position is named by, the openings a game passes through, and a position's tablebase read.
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
#include "api_internal.h"
#include "chess_rules.h"
#include "chess_tablebase.h"

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

/* Extract a pure board-state trajectory [P0, P1, ..., Pn] (n >= 2, said == 0 on every vertex, no Tier-0 leaf). */
static int line_run_of(const uint8_t *ewkb, size_t len, lp_id **out_ids){
    *out_ids = NULL;
    size_t nv = lp_path_vertices(ewkb, len, NULL, 0);
    if (!nv) return 0;
    lp_vertex *vx = malloc(sizeof(lp_vertex) * nv);
    lp_path_vertices(ewkb, len, vx, nv);
    size_t total = 0;
    for (size_t i = 0; i < nv; i++) {
        if (vx[i].said != 0 || lp_tier0_codepoint(T0, &vx[i].id) >= 0) { free(vx); return 0; }
        total += vx[i].run ? vx[i].run : 1;
    }
    if (total < 2 || total > INT_MAX) { free(vx); return 0; }
    lp_id *ids = malloc(sizeof(lp_id) * total);
    size_t at = 0;
    for (size_t i = 0; i < nv; i++) {
        uint32_t run = vx[i].run ? vx[i].run : 1;
        for (uint32_t k = 0; k < run; k++) ids[at++] = vx[i].id;
    }
    free(vx);
    *out_ids = ids;
    return (int)total;
}
static int is_position_path(const uint8_t *ewkb, size_t len){
    lp_id part[8];
    size_t n = lp_path_ids(ewkb, len, part, 8);
    if (n < 4 || n > 7) return 0;
    int64_t side = lp_tier0_codepoint(T0, &part[1]);
    return (side == 'w' || side == 'b') && lp_tier0_codepoint(T0, &part[0]) < 0;
}

/* keys: the positions asked about. routes 1 reads the lines through them; 0 their own names alone (a list of games).
 * exclude: a line that is not evidence of a book (the game's own, when it is recorded), or NULL. */
static int book_read(PGconn *pg, const lp_id *keys, int nkeys, int routes, const lp_id *exclude, Book *b){
    memset(b, 0, sizeof *b); lp_id name_predicate, eco_predicate, start_pos;
    if (!predicates(&name_predicate, &eco_predicate)) return 0;
    { Bd b0; lp_chess_board_start(&b0); lp_chess_id_position(&b0, &start_pos); }
    b->in_book = calloc((size_t)(nkeys ? nkeys : 1), 1); b->at = lp_idmap_sized(sizeof(int)); b->rd = reader_new(pg);
    for (int i = 0; i < nkeys; i++) route_add(b, i, &keys[i], -1);                          /* each position names itself */
    if (routes && nkeys) {                                                                  /* 1, 2: the lines through each */
        int *hub = calloc((size_t)nkeys * 2, sizeof(int)), nh = 0;
        Hold *h = holds_capped(pg, keys, nkeys, LINES_CAP, 0, 3, hub, &nh);
        for (int i = 0; i < nkeys; i++) if (hub[2 * i + 1]) b->in_book[i] = 1;
        for (int k = 0; k < nh; k++) {
            int key = h[k].src;
            if (key < 0 || key >= nkeys || (exclude && lp_id_eq(&h[k].entity, exclude))) continue;
            lp_id *ids = NULL; int n = line_run_of(h[k].path, (size_t)h[k].path_len, &ids);
            /* Accept non-claim trajectories [P0, P1, ..., Pn], and 1-ply [start_pos, P1] even when also a transition claim. */
            if (n >= 2 && (!h[k].claim || (n == 2 && lp_id_eq(&ids[0], &start_pos)))) {
                for (int j = 0; j < n; j++) if (lp_id_eq(&ids[j], &keys[key])) {
                    b->in_book[key] = 1;
                    for (int p = 0; p <= j; p++) route_add(b, key, &ids[p], p);
                    break;
                }
            }
            free(ids);
        }
        holds_free(h, nh); free(hub);
    }
    /* 3: the claims of every position reached, each position once */
    lp_idmap *seen = lp_idmap_new(); lp_vec(lp_id) wanted = { 0 };
    for (size_t i = 0; i < b->nroutes; i++) { bool fresh; lp_idmap_put(seen, &b->routes[i].position, &fresh); if (fresh) lp_push(&wanted, b->routes[i].position); }
    lp_idmap_free(seen);
    lp_idmap *texts = lp_idmap_new(); size_t ncap = 0;
    if (wanted.n) {
        const lp_id preds[2] = { name_predicate, eco_predicate };
        for (int pi = 0; pi < 2; pi++) {
            int nh = 0;
            Hold *h = holds_both(pg, wanted.v, (int)wanted.n, &preds[pi], &nh);
            for (int k = 0; k < nh; k++) {
                lp_id part[3]; if (!h[k].claim || lp_path_ids(h[k].path, (size_t)h[k].path_len, part, 3) != 3) continue;
                if (h[k].src < 0 || (size_t)h[k].src >= wanted.n || !lp_id_eq(&part[0], &wanted.v[h[k].src])) continue;
                int is_name = (pi == 0) && lp_id_eq(&part[1], &name_predicate);
                int is_eco  = (pi == 1) && lp_id_eq(&part[1], &eco_predicate);
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
            holds_free(h, nh);
        }
    }
    lp_vec_free(&wanted);
    /* 4: each named position's own line, the shortest recorded line from start_pos ending in it. A one-move line [P0, P1]
     * is the same entity as the transition claim [P0, P1], so kinds=3 reads both claims and observations while requiring
     * ids[0] == start_pos excludes midgame [P_{k-1}, P_k] transitions. */
    if (b->nnamed) {
        lp_id *positions = malloc(sizeof(lp_id) * b->nnamed); for (size_t i = 0; i < b->nnamed; i++) positions[i] = b->named[i].position;
        int nh = 0, *hub = calloc(b->nnamed * 2, sizeof(int));
        Hold *h = holds_endpoint(pg, positions, (int)b->nnamed, LINES_CAP, 0, 3, 2, hub, &nh);
        for (int k = 0; k < nh; k++) {
            if (h[k].src < 0 || (size_t)h[k].src >= b->nnamed) continue;
            Named *n = &b->named[h[k].src];
            lp_id *ids = NULL; int len = line_run_of(h[k].path, (size_t)h[k].path_len, &ids);
            if (len >= 2 && lp_id_eq(&ids[0], &start_pos) && lp_id_eq(&ids[len - 1], &n->position)
                && (!n->has_line || len - 1 < n->plies)) {
                n->line = h[k].entity; n->plies = len - 1; n->has_line = 1;
            }
            free(ids);
        }
        holds_free(h, nh); free(hub); free(positions);
    }
    if (lp_idmap_count(texts)) reader_prefetch(b->rd, lp_idmap_keys(texts), lp_idmap_count(texts), 240);         /* 5 */
    lp_idmap_free(texts);
    return 1;
}

static void emit_text(lp_buf *o, Reader *rd, const lp_id *id, int has){
    if (!has) { lp_buf_cstr(o, "null"); return; }
    char *text = reader_text(rd, id, 240);
    lp_json_cstr(o, text ? text : "");
    free(text);
}
/* {line, name, eco, plies[, standing]}: line and plies its own line's, or the route's plies where none is recorded */
static void emit_opening(lp_buf *o, const Book *b, const Named *n, int route_plies, int standing){
    char hex[33];
    if (n->has_line) { api_hex_id(&n->line, hex); lp_buf_fmt(o, "{\"line\":\"%s\",\"name\":", hex); } else lp_buf_cstr(o, "{\"line\":null,\"name\":");
    emit_text(o, b->rd, &n->name, n->has_name); lp_buf_cstr(o, ",\"eco\":"); emit_text(o, b->rd, &n->eco, n->has_eco);
    int plies = n->has_line ? n->plies : route_plies;
    if (plies >= 0) lp_buf_fmt(o, ",\"plies\":%d", plies); else lp_buf_cstr(o, ",\"plies\":null");
    if (!standing) { lp_buf_byte(o, '}'); return; }
    if (n->stood) lp_buf_fmt(o, ",\"standing\":{\"rating\":%.3f,\"deviation\":%.3f}}", n->r.rating, n->r.deviation);
    else lp_buf_cstr(o, ",\"standing\":null}");
}
typedef struct { const Named *n; int plies; } Shown;
static int shown_order(const void *a, const void *b){
    const Shown *x = a, *y = b; unsigned px = (unsigned)x->plies, py = (unsigned)y->plies;     /* -1: last */
    return px != py ? (px < py ? -1 : 1) : memcmp(&x->n->position, &y->n->position, 16);
}
/* The openings of key: each named position along its routes once, by plies ascending. */
static void emit_openings(lp_buf *o, const Book *b, int key){
    Shown *s = malloc(sizeof(Shown) * (b->nroutes ? b->nroutes : 1)); size_t n = 0; lp_idmap *once = lp_idmap_new();
    for (size_t i = 0; i < b->nroutes; i++) { if (b->routes[i].key != key) continue; const Named *named = named_of(b, &b->routes[i].position); if (!named) continue;
        bool fresh; lp_idmap_put(once, &named->position, &fresh); if (!fresh) continue;
        s[n++] = (Shown){ named, named->has_line ? named->plies : b->routes[i].plies }; }
    qsort(s, n, sizeof(Shown), shown_order);
    lp_buf_byte(o, '[');
    for (size_t i = 0; i < n; i++) { if (i) lp_buf_byte(o, ','); emit_opening(o, b, s[i].n, s[i].plies, 1); }
    lp_buf_byte(o, ']'); lp_idmap_free(once); free(s);
}
/* The deepest opening a game reaches: the named position latest along it. */
static void emit_deepest(lp_buf *o, const Book *b, const lp_id *positions, size_t n){
    for (size_t i = n; i-- > 0;) { const Named *named = named_of(b, &positions[i]); if (named) { emit_opening(o, b, named, (int)i, 0); return; } }
    lp_buf_cstr(o, "null");
}
static void db_unavailable(int c){
    api_reply_error(c, 503, "database", api_db_error());
}

/* POST /v1/chess_position {fen}: {position, openings: [{line, name, eco, plies, standing}], games}. An input that is
 * no position answers {error} (the web host's 400). */
void on_chess_position(int c, const char *body){
    char fen[256]; lp_id position;
    if (!api_jstr(body, "fen", fen, sizeof fen) || !lp_chess_fen_position(fen, strlen(fen), &position)) { api_reply_error(c, 200, "fen: no position", NULL); return; }
    PGconn *pg = api_db(); if (!pg) { db_unavailable(c); return; }
    Book b; if (!book_read(pg, &position, 1, 1, NULL, &b)) { book_free(&b); api_reply_error(c, 500, "tier0", NULL); return; }
    long games = holds_count(pg, &position, 1, 0);
    char hex[33]; api_hex_id(&position, hex); lp_buf o = { 0 };
    lp_buf_fmt(&o, "{\"position\":\"%s\",\"openings\":", hex); emit_openings(&o, &b, 0);
    lp_buf_fmt(&o, ",\"games\":%ld}", games);
    book_free(&b); api_reply_buf(c, 200, &o);
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
    if (np > 0) {
        if (is_position_path(paths[0].path, (size_t)paths[0].path_len)) {
            positions[n++] = *line_id;
        } else {
            lp_id *ids = NULL; int len = line_run_of(paths[0].path, (size_t)paths[0].path_len, &ids);
            for (int j = 0; j < len && n <= GAME_PLIES; j++) positions[n++] = ids[j];
            free(ids);
        }
    }
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
        Book b; if (!book_read(pg, all, (int)total, 0, NULL, &b)) { book_free(&b); free(all); free(count); api_reply_error(c, 500, "tier0", NULL); return; }
        lp_buf o = { 0 }; size_t at = 0; lp_buf_byte(&o, '[');
        for (size_t g = 0; g < ngames; g++) { lp_buf_fmt(&o, "%s{\"deepest\":", g ? "," : ""); emit_deepest(&o, &b, all + at, count[g]); lp_buf_byte(&o, '}'); at += count[g]; }
        lp_buf_byte(&o, ']');
        book_free(&b); free(all); free(count); api_reply_buf(c, 200, &o); return;
    }
    size_t cap = strlen(body) + 1; char *pgn = malloc(cap), idtext[64]; lp_id line_id, line, *positions = malloc(sizeof(lp_id) * (GAME_PLIES + 1));
    size_t n = 0; int recorded = 0;
    if (api_jstr(body, "pgn", pgn, cap)) n = game_positions(pg, pgn, NULL, positions, &line);
    else if (api_jstr(body, "line", idtext, sizeof idtext) && api_valid_id(idtext)) { lp_id_unhex(idtext, &line_id); n = game_positions(pg, NULL, &line_id, positions, &line); recorded = 1; }
    else { free(pgn); free(positions); api_reply_error(c, 200, "pgn, line or pgns", NULL); return; }
    free(pgn);
    if (!n) { free(positions); api_reply_error(c, 200, "the game does not replay, or the line is no recorded line", NULL); return; }
    Book b; if (!book_read(pg, positions, (int)n, 1, recorded ? &line : NULL, &b)) { book_free(&b); free(positions); api_reply_error(c, 500, "tier0", NULL); return; }
    long out = -1; for (size_t i = 1; i < n && out < 0; i++) if (!b.in_book[i]) out = (long)i;     /* the first ply no other recorded line reaches */
    char hex[33]; api_hex_id(&line, hex); lp_buf o = { 0 };
    lp_buf_fmt(&o, "{\"line\":\"%s\",\"plies\":[", hex);
    for (size_t i = 0; i < n; i++) { api_hex_id(&positions[i], hex);
        lp_buf_fmt(&o, "%s{\"ply\":%zu,\"position\":\"%s\",\"openings\":", i ? "," : "", i, hex); emit_openings(&o, &b, (int)i); lp_buf_byte(&o, '}'); }
    lp_buf_cstr(&o, "],\"deepest\":"); emit_deepest(&o, &b, positions, n);
    if (out >= 0) lp_buf_fmt(&o, ",\"out_of_book_at\":%ld}", out); else lp_buf_cstr(&o, ",\"out_of_book_at\":null}");
    book_free(&b); free(positions); api_reply_buf(c, 200, &o);
}

/* ---- the tablebase: a position's verdict, its moves and its best-play line, read from the Syzygy tables
 *
 * A verdict is a deterministic read of the tables, never a record: the Engine's probe (chess_tablebase.c, in
 * laplace_engine_reader, the one implementation `laplace tablebase` runs) gives the position's win/draw/loss and
 * distance to zeroing, every legal move ranked by what it leaves (Fathom's root rule, best first) and the best-play line
 * to its end, each move with the ID of its [P, M, P'] transition, the claim a game that plays it records and the
 * claim the syzygy source records of its lines. The tables are those under $LAPLACE_SYZYGY, else
 * $LAPLACE_DATA/Games/Chess/syzygy, mapped as probes need them (a bounded cache). What the database holds of the
 * position is read beside it, in one containment probe: the claims that hold the position and the line's positions,
 * and of those the moves' and the line's transitions with their standings (recorded: null where none is recorded). */
#define TB_HUB 4096                         /* a position held by more claims is read as a hub: its transitions' standings are not */
#define TB_LINE 1024
static const char *const TB_WDL[5] = { "loss", "blessed loss", "draw", "cursed win", "win" };
static void emit_recorded(lp_buf *o, lp_idmap *stood, const Hold *h, const lp_id *claim){
    int64_t at = stood ? lp_idmap_find(stood, claim) : -1;
    if (at < 0) { lp_buf_cstr(o, ",\"recorded\":null"); return; }
    const Hold *x = &h[*(int *)lp_idmap_at(stood, (size_t)at)];
    if (x->stood) lp_buf_fmt(o, ",\"recorded\":{\"rating\":%.3f,\"deviation\":%.3f,\"matches\":%lld}", x->r.rating, x->r.deviation, (long long)x->matches);
    else lp_buf_cstr(o, ",\"recorded\":{\"rating\":null,\"deviation\":null,\"matches\":0}");
}
/* /v1/chess_tablebase {fen}: {position, tables, covered, wdl, dtz, moves: [{san, uci, position, claim, wdl, dtz,
 * value, zeroing, mate, recorded}], line: {id, result, plies: [{san, uci, position, claim, recorded}]} | null, claims,
 * database}. wdl and dtz are the side to move's (a move's: the mover's, of the position it makes; value the mover's
 * distance to zeroing through it); null where the tables do not cover the position (more pieces than they hold,
 * castling rights). claims: how many recorded claims hold the position (-1 its holders are more than a hub's). */
void on_chess_tablebase(int c, const char *body){
    char fen[256]; if (!api_jstr(body, "fen", fen, sizeof fen)) { api_reply_error(c, 200, "fen: no position", NULL); return; }
    LpTablebase *tb = lp_tablebase_shared();
    LpTbProbe *p = malloc(sizeof *p); int covered = lp_tablebase_probe_fen(tb, fen, strlen(fen), p);
    if (covered < 0) { free(p); api_reply_error(c, 200, "fen: no position", NULL); return; }
    LpTbPly *ply = malloc(sizeof(LpTbPly) * TB_LINE); lp_id line_id; char result[8] = ""; int plies = covered ? lp_tablebase_line_fen(tb, fen, strlen(fen), ply, TB_LINE, &line_id, result) : -1;
    /* what the database holds: the claims holding the position and the line's positions before each ply */
    PGconn *pg = api_db(); int nkeys = 1 + (plies > 0 ? plies : 0), nh = 0, *hub = calloc((size_t)nkeys * 2, sizeof(int)); Hold *h = NULL; lp_idmap *stood = NULL; long claims = 0;
    if (pg) {
        lp_id *keys = malloc(sizeof(lp_id) * (size_t)nkeys); keys[0] = p->position;
        for (int i = 1; i < nkeys; i++) keys[i] = ply[i - 1].position;                      /* the position after ply i - 1: the one ply i is made from */
        h = holds_capped(pg, keys, nkeys, TB_HUB, 1, 1, hub, &nh); stood = lp_idmap_sized(sizeof(int));
        for (int k = 0; k < nh; k++) { if (!h[k].claim) continue; if (h[k].src == 0) claims++; *(int *)lp_idmap_get(stood, &h[k].entity, NULL) = k; }
        if (hub[0]) claims = -1;
        free(keys); }
    char hex[33]; lp_buf o = { 0 }; api_hex_id(&p->position, hex);
    lp_buf_fmt(&o, "{\"position\":\"%s\",\"tables\":%s,\"covered\":%s,", hex, tb ? "true" : "false", covered ? "true" : "false");
    if (covered) lp_buf_fmt(&o, "\"wdl\":\"%s\",\"dtz\":%d", TB_WDL[p->wdl + 2], p->dtz); else lp_buf_cstr(&o, "\"wdl\":null,\"dtz\":null");
    lp_buf_cstr(&o, ",\"moves\":[");
    for (int i = 0; i < p->nmoves; i++) { const LpTbMove *m = &p->move[i]; char ph[33], ch[33]; api_hex_id(&m->position, ph); api_hex_id(&m->claim, ch);
        lp_buf_fmt(&o, "%s{\"san\":\"%s\",\"uci\":\"%s\",\"position\":\"%s\",\"claim\":\"%s\",", i ? "," : "", m->san, m->uci, ph, ch);
        if (covered) lp_buf_fmt(&o, "\"wdl\":\"%s\",\"dtz\":%d,\"value\":%d,\"zeroing\":%s,\"mate\":%s", TB_WDL[m->wdl + 2], m->dtz, m->value, m->zeroing ? "true" : "false", m->mate ? "true" : "false");
        else lp_buf_cstr(&o, "\"wdl\":null,\"dtz\":null,\"value\":null,\"zeroing\":null,\"mate\":null");
        emit_recorded(&o, stood, h, &m->claim); lp_buf_byte(&o, '}'); }
    lp_buf_cstr(&o, "],\"line\":");
    if (plies > 0) { api_hex_id(&line_id, hex); lp_buf_fmt(&o, "{\"id\":\"%s\",\"result\":\"%s\",\"plies\":[", hex, result);
        for (int i = 0; i < plies; i++) { char ph[33], ch[33]; api_hex_id(&ply[i].position, ph); api_hex_id(&ply[i].claim, ch);
            lp_buf_fmt(&o, "%s{\"san\":\"%s\",\"uci\":\"%s\",\"position\":\"%s\",\"claim\":\"%s\"", i ? "," : "", ply[i].san, ply[i].uci, ph, ch);
            emit_recorded(&o, stood, h, &ply[i].claim); lp_buf_byte(&o, '}'); }
        lp_buf_cstr(&o, "]}"); }
    else lp_buf_cstr(&o, "null");
    if (pg) lp_buf_fmt(&o, ",\"claims\":%ld,\"database\":true}", claims); else lp_buf_cstr(&o, ",\"claims\":null,\"database\":false}");
    if (h) holds_free(h, nh); if (stood) lp_idmap_free(stood); free(hub); free(ply); free(p);
    api_reply_buf(c, 200, &o);
}