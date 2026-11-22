#ifndef LPM_API_INTERNAL_H
#define LPM_API_INTERNAL_H

#include "api.h"
#include "laplace/http.h"
#include "lpm.h"
#include "lpm_config.h"
#include "db_bridge.h"
#include "walk.h"
#include "laplace/laplace.h"
#include "laplace/json.h"
#include "laplace/highway.h"
#include "laplace/os.h"

#include <ctype.h>
#include <inttypes.h>
#include <libpq-fe.h>
#include <limits.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

extern const lp_tier0_record *T0;
extern lp_text *TX;
extern const char *ENGINE;

const lp_highway *api_view_highway(void);
void api_engine_resolve(void);
void api_hex_id(const lp_id *id, char out[33]);
int api_valid_id(const char *s);
PGconn *api_db(void);
const char *api_db_error(void);
void api_db_release(void);
PGconn *api_cc_db(int c);
void api_http(int c, int code, const char *body);
const char *api_jvalue(const char *body, const char *key);
int api_jstr(const char *body, const char *key, char *out, size_t cap);
int api_jbool(const char *body, const char *key, int dflt);
int api_jint(const char *body, const char *key, int *out);
size_t api_parse_str_array(const char *body, const char *key, char **items, size_t max_items);
char *api_cw_file_name(Reader *rd, const lp_id *file);

static inline void api_reply_buf(int c, int code, lp_buf *b) {
    lp_buf_byte(b, 0);
    api_http(c, code, (const char *)b->b);
    lp_buf_free(b);
}
static inline void api_reply_error(int c, int code, const char *err, const char *detail) {
    lp_buf b = { 0 };
    lp_buf_cstr(&b, "{\"error\":");
    lp_json_cstr(&b, err ? err : "error");
    if (detail) {
        lp_buf_cstr(&b, ",\"detail\":");
        lp_json_cstr(&b, detail);
    }
    lp_buf_byte(&b, '}');
    api_reply_buf(c, code, &b);
}

typedef struct { char *kind, *name, *source; size_t line, start, length, name_at; } FoDecl;

size_t cw_lines(const char *text, size_t upto);
int fo_word(char c);
void fo_free(FoDecl *d, size_t n);
size_t fo_outline(PGconn *pg, Reader *rd, const lp_id *file, FoDecl **declp, char **textp, size_t *tnp);
char *fo_text(PGconn *pg, Reader *rd, const lp_id *file, size_t *tnp);
void fo_texts(PGconn *pg, Reader *rd, const lp_id *files, size_t n, char **texts, size_t *lens);
long fo_holder(const FoDecl *declarations, size_t count, size_t position);

int api_name_entity(const char *text, lp_ref *trunk, lp_ref *parts, size_t cap, size_t *nparts);
void api_parts_json(const lp_ref *parts, size_t n, lp_buf *out);
void api_coord_json(const lp_ref *r, lp_buf *out);
void api_unit_coord_json(const lp_ref *r, lp_buf *out);
int api_mask_claim(const char *m, int len);
void api_emit_mask_values(lp_buf *out, const char *wire, int len);

int api_plain_arg(const char *s);
void api_on_engine(int c, char **av);

/* Route handlers */
void on_embeddings(int c, const char *body);
void on_search(int c, const char *body);
void on_forward(int c, const char *body);
void on_shape(int c, const char *body);
void on_ingest(int c, const char *body);
void on_hop(int c, const char *body);
void on_text(int c, const char *body);
void on_translate(int c, const char *body);
void on_pull(int c, const char *body);
void on_turn(int c, const char *body);
void on_degrees(int c, const char *body);
void on_code_search(int c, const char *body);
void on_reindex(int c, const char *body);
void on_rerank(int c, const char *body);
void on_file_outline(int c, const char *body);
void on_recipe_plan(int c, const char *body);
void on_recipe_catalog(int c, const char *body);
void on_code_where(int c, const char *body);
void on_code_callers(int c, const char *body);
void on_code_callees(int c, const char *body);
void on_ingest_status(int c, const char *body);
void on_sources(int c, const char *body);
void on_catalog(int c, const char *body);
void on_ready(int c);
void on_library(int c, const char *body);
void on_provenance(int c, const char *body);
void on_chess_position(int c, const char *body);
void on_chess_game_openings(int c, const char *body);
void on_chess_tablebase(int c, const char *body);
void on_chess_players(int c, const char *body);
void on_chess_player(int c, const char *body);
void on_chess_games(int c, const char *body);
void on_chess_game(int c, const char *body);

void api_capture_begin(int stdio_mode);
const char *api_capture_end(int stdio_mode, int *code_out);
void api_dispatch(int c, const char *method, const char *path, const char *body);
const lp_http_shell *api_lpm_shell(void);

#endif
