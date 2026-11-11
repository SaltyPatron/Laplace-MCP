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
#include "lpm_internal.h"
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
static size_t game_positions(PGconn *pg, const char *pgn, const lp_id *line_id, lp_id *positions, lp_id *line){
    if (pgn) {
        const void *full = lpm_load_lang("pgn"), *moves = lpm_load_lang("pgn_moves");
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
