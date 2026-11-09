#ifndef LPM_API_INTERNAL_H
#define LPM_API_INTERNAL_H

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <io.h>
#include <fcntl.h>
#define LAPLACE_WIN_ARPA_INET_H
#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
void api_capture_begin(int stdio_mode);
const char *api_capture_end(int stdio_mode, int *code_out);
void api_dispatch(int c, const char *method, const char *path, const char *body);

#endif
typedef intptr_t ssize_t;
#define close_sock(s) closesocket(s)
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>
#define close_sock(s) close(s)
void api_capture_begin(int stdio_mode);
const char *api_capture_end(int stdio_mode, int *code_out);
void api_dispatch(int c, const char *method, const char *path, const char *body);

#endif

#include "api.h"
#include "lpm.h"
#include "lpm_config.h"
#include "engine.h"
#include "walk.h"
#include "db_bridge.h"
#include "laplace/laplace.h"
#include "laplace/highway.h"

#include <ctype.h>
#include <inttypes.h>
#include <libpq-fe.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

extern const lp_tier0_record *T0;
extern lp_text *TX;
extern PGconn *PG;
extern Lpm *CODE_IX;
extern const char *ENGINE;

const lp_highway *api_view_highway(void);
void api_engine_resolve(void);
uint64_t api_nsec(void);
void api_hex_id(const lp_id *id, char out[33]);
int api_valid_id(const char *s);
PGconn *api_db(void);
PGconn *api_cc_db(int c);
void api_http(int c, int code, const char *body);
char *api_json_esc(const char *s, char *out, size_t cap);
const char *api_jvalue(const char *body, const char *key);
int api_jstr(const char *body, const char *key, char *out, size_t cap);
int api_jbool(const char *body, const char *key, int dflt);
int api_jint(const char *body, const char *key, int *out);
size_t api_parse_str_array(const char *body, const char *key, char **items, size_t max_items);
void api_buf_add(char **b, size_t *n, size_t *cap, const char *s, size_t m);
void api_buf_fmt(char **b, size_t *n, size_t *cap, const char *fmt, ...);
void api_cw_str(char **out, size_t *on, size_t *ocap, const char *s);
char *api_cw_file_name(Reader *rd, const lp_id *file);

typedef struct { lp_id id; uint8_t file; } CwNode;
typedef struct { CwNode *n; size_t nn, cn; lp_idmap *m; } CwDag;
typedef struct { char *kind, *name, *source; size_t line, start, length, name_at; } FoDecl;

int cw_walk_many(PGconn *pg, const lp_id *names, size_t count, CwDag *d, size_t max_nodes, int *levels, int *cut);
int cw_walk(PGconn *pg, const lp_id *name, CwDag *d, size_t max_nodes, int *levels, int *cut);
size_t cw_lines(const char *text, size_t upto);
int fo_word(char c);
void fo_free(FoDecl *d, size_t n);
size_t fo_outline(PGconn *pg, Reader *rd, const lp_id *file, FoDecl **declp, char **textp, size_t *tnp);
long fo_holder(const FoDecl *declarations, size_t count, size_t position);

int api_name_entity(const char *text, lp_ref *trunk, lp_ref *parts, size_t cap, size_t *nparts);
void api_parts_json(const lp_ref *parts, size_t n, char *out, size_t cap);
int api_part_list(const lp_ref *parts, size_t n, char *out, size_t cap);
void api_coord_json(const lp_ref *r, char *out, size_t cap);
void api_unit_coord_json(const lp_ref *r, char *out, size_t cap);
int api_q1(PGconn *pg, const char *sql, const char *a, char **out, char *err, size_t errcap);
int api_q2(PGconn *pg, const char *sql, const char *a, const char *b, char **out, char *err, size_t errcap);
int api_mask_claim(const char *m, int len);
void api_emit_mask_values(char **out, size_t *n, size_t *cap, const char *wire, int len);

int api_engine_run(char **av, char **outp, int *code, int timeout_s);
int api_plain_arg(const char *s);
void api_on_engine(int c, char **av);

extern const char *API_FORWARD_SQL;
extern const char *API_SHAPE_SQL;

/* Route handlers */
void on_embeddings(int c, const char *body);
void on_search(int c, const char *body);
void on_by_parts(int c, const char *body, const char *sql, const char *kind);
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
void on_library(int c, const char *body);
void on_provenance(int c, const char *body);

void api_capture_begin(int stdio_mode);
const char *api_capture_end(int stdio_mode, int *code_out);
void api_dispatch(int c, const char *method, const char *path, const char *body);

#endif
