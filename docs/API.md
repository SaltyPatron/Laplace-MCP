# Laplace-MCP API

Laplace-MCP is the HTTP service in front of Laplace. A caller sends content or an ID it already holds. This process maps tier 0, names the entity, and calls the database functions. The caller is not linked to Postgres and does not send SQL.

The process listens on `127.0.0.1:5188`. nginx `laplace-managed` proxies `POST https://hart-server:8443/mcp` to `http://127.0.0.1:5188/mcp`. The same bodies are served on `/v1/...` directly.

`LAPLACE_CONNINFO` selects the database. Unset, the process uses `host=/tmp port=5432 user=laplace dbname=laplace`.

Nothing about the machine is compiled in but a default. The process reads, from its environment (the unit takes the machine's `/etc/laplace/machine.env` when there is one):

| Name | What | Unset |
|---|---|---|
| `LAPLACE_MCP_HOST`, `LAPLACE_MCP_PORT` | the address and port `lpm serve` listens on; `lpm serve HOST PORT` still wins | `127.0.0.1`, `5188` |
| `LAPLACE_CONNINFO` | the database | `host=/tmp port=5432 user=laplace dbname=laplace` |
| `LAPLACE_ENGINE` | the engine program the routes run | `$LAPLACE_BUILD/Laplace-Engine/icx-release/laplace`, or `/repos/build/...` without `LAPLACE_BUILD` |
| `LAPLACE_GRAMMARS` | the directory of compiled tree-sitter grammars | `/repos/build/grammars` |

It is built as the engine is (`CMakeLists.txt`): Laplace-Native as part of it, with icx under the floating-point contract and laplace-math, and the engine's reader (`read.c`, `pg.c`) from `LAPLACE_ENGINE_SRC` (the engine's `src` beside this repository). The defaults in the table are what the build was configured with (`native/config.h.in`): the value in the environment at configure time, else the deployment target's.

One process holds one database connection. `lpm serve` is that process. `laplace_mcp.py` is not.

## Who may call it

`lpm serve` takes a bearer token: `LAPLACE_MCP_TOKEN`, or the `LAPLACE_MCP_TOKEN=` line of the file `LAPLACE_MCP_TOKEN_FILE` names. Without one of at least 32 characters and no whitespace it does not listen; it exits 2. Every request but `GET /health` sends `Authorization: Bearer <token>`; any other gets `401` with `WWW-Authenticate: Bearer` and `{"error":"unauthorized"}`, and nothing is dispatched. On hart-server setup makes the token once, in `/etc/laplace/secrets/modular-mcp.env` (root and `laplace-runner`, 0640), and the unit reads it. `lpm mcp` over stdio takes none: it is the calling user's own process.

## What a caller sends

Jina's embedding API is `POST https://api.jina.ai/v1/embeddings` with a JSON body of `model` and `input` (a string or a list of strings, and for some models an image or a PDF). The response is a list of float vectors. See [Jina embeddings](https://jina.ai/embeddings/).

`POST /v1/embeddings` takes one string in `input` and returns the entity instead of a vector.

```json
{"input":"Sherlock Holmes"}
```

```json
{"object":"list","model":"laplace","data":[{"object":"record","index":0,"id":"d91895c654f33a108a63bb64f10ca69f","tier":3,"coord":[-0.43097229504208512,-0.40779169540644233,-0.30074172951672307,-0.71703307803402827],"hilbert":"05f75fb4b4f82a49","parts":["ee7048d5a4b2e11e5223e57a5bd579a4","00263ca9f57f7177f495e3711f8cdd59","5b7e40e9a23ae55eaccd45646934977d"]}],"ms":{"identity":0.123}}
```

The ID, tier, coordinate, Hilbert value, and parts are computed here from the mapped tier 0 (`lp_text_parts`). Measured identity time on this call was 0.123 ms. A second call on the same process was 0.025 ms. No database is used.

`input` is one string, up to 8192 bytes. A list of inputs, an image, and a PDF are not accepted yet.

## Search

`POST /v1/search` with `{"input":"Sherlock Holmes"}` or `{"id":"d91895c654f33a108a63bb64f10ca69f"}`.

The string is named here. The database is then called with that one ID:

- `laplace_containers(ARRAY[id], '{}')` returns every path that holds it, with its tier and whether mask bit 0 is set (a claim).
- `laplace_claims(ARRAY[id], 64, '{}', '{}')` returns the claims that hold it, with rating, deviation, volatility, and matches.

Through nginx, `POST /mcp` with `{"op":"search","input":"Sherlock Holmes"}` returned the same ID and six containers and three claims. The three claims are the WordNet records: part of speech noun, the lexical entry, and the sense. Ratings on this database were 1575.95, deviation 176.20, volatility 0.060, one match each.

`{"text":true}` adds `laplace_text` of each returned entity, cut at 400 characters. Without it, the rows are IDs and standings.

Measured on this machine, 2026-10-03, against the loaded `laplace` database (13,764,010 entities at the start of the measurement):

| Call | Time |
|---|---|
| Identity, tier 0 already mapped | 0.020–0.288 ms |
| `laplace_containers` alone, earlier `EXPLAIN ANALYZE` | 3.96 ms execution, 408 cached pages, 6 rows |
| This endpoint, containers and claims together, in `psql` | 244 ms |
| The same endpoint on the service | about 205–255 ms |
| The same with `laplace_text` rendered | about 210–300 ms |

The per-partition GIN probes in an unpruned scan of `physicality` were a few microseconds each. The plan that appends every partition, and the claims read joined to consensus, are what the caller waits on. This endpoint does not claim a microsecond response.

## Shape

`POST /v1/shape` with `{"input":"...","other":"..."}`.

Both strings are decomposed here. `laplace_frechet4d` is called on two linestrings built with `ST_MakeLine` of the stored `entity.coord` of those constituents, in constituent order. The comparison is the database function.

| Pair | Fréchet | Time |
|---|---|---|
| Sherlock Holmes against itself | 0 | 15.3 ms |
| Sherlock Holmes against Holmes (`5b7e40e9a23ae55eaccd45646934977d`) | 0.22124873411843002 | 25.9 ms |

`laplace_frechet4d(geometry, geometry, integer)` (up to 8 skipped vertices), `laplace_dtw4d`, and `laplace_edr4d` are installed and are not separate endpoints yet.

## Forward

`POST /v1/forward` with `{"input":"..."}` calls `laplace_forward` on the constituent IDs.

For `Sherlock Holmes` the service returned, in 16.4 ms, the span of the two words held by 2 paths as a run twice, followed by Holmes (`5b7e40e9a23ae55eaccd45646934977d`, times 2), and the word Holmes held by 6 paths.

## Ingest

`POST /v1/ingest` with `{"path":"/absolute/path"}` runs the engine (`LAPLACE_ENGINE`) as `laplace ingest` on that path. That is the engine pipeline: decompose, deduplicate trunk to leaf, record, attest. The body must be an absolute path that exists. The call blocks until ingest exits.

A proof file `/tmp/lpm-api-proof.txt`, one line `Sherlock Holmes kept his pipe on the mantel.` plus a newline, exited 0 in 0.2 s: 1 file, 15 compositions, 4 new entities, 11 subtrees already recorded, 0 attestations. Searching the exact bytes, ID `f27bdacd15c996339ccee43dd4153450`, returned one container, `221c6d0ef03f1be62c3625e9625f32db`, tier 4, whose text is the filename and that line.

## Surface

`GET /v1/surface` lists the routes this process serves, the database functions those routes call, the engine's twenty-one commands, and the reads that are specified and not yet a route.

`GET /health` answers without the database.

Served now: embeddings, rerank, code_search, reindex, code_where, code_callers, code_callees, sources, ingest_status, file_outline, recipe_plan, search, forward, shape, ingest, hop, text, translate, pull, degrees, surface, health. `hop`, `text`, `translate`, `pull` and `degrees` run the engine command of that name on `input` (`translate` also takes `from` and `to`; `degrees` takes `other`) and return its exit code and log.

Engine commands that are not a route here: tier0, flags, highway, deploy, forget, sweep, index, structure, tree, turn, fills, status, bench, model; `sources` is read from the database rather than run. `hop` is the engine's program over the same container and claim reads search calls. `fills` in the engine is containers, then `lp_follows` for the exact run, then a parent walk through `laplace_fills`. `laplace_fills` by itself is that parent walk, so this service does not expose it as "what follows."

Specified past these routes, and not built as their own endpoints: turn, gaps, file metadata as its own read, DTW, EDR, Fréchet with skipped vertices, and the forward stages RESOLVE, COUPLE, ORIENT, ROUTE, SCAN, PROPOSE, STEER, SELECT, REALIZE, WITNESS. `laplace_couple` is the installed operator that returns strands, containment, and shape in one call. Search calls containers and claims as two reads inside one statement. It does not call `laplace_couple`.

## MCP

`POST /mcp` also speaks MCP JSON-RPC when the body contains `"jsonrpc"`. Tools: `embeddings`, `search`, `forward`, `shape`, `ingest`, `code_search`, `rerank`, `reindex`, `code_where`, `code_callers`, `code_callees`, `sources`, `ingest_status`, `file_outline`, `recipe_plan`, `hop`, `text`, `translate`, `pull`, `degrees`. `tools/list` gives each one's arguments.

`~/.grok/config.toml` has:

```toml
[mcp_servers.laplace]
url = "http://127.0.0.1:5188/mcp"
headers = { Authorization = "Bearer ${LAPLACE_MODULAR_MCP_TOKEN}" }
```

`grok mcp doctor laplace` reported handshake OK, protocol 2025-11-25, 5 tools. A Grok process restricted to `laplace__search` called `search` on `Sherlock Holmes` and received the six containers and three claims. A session that was already open before `grok mcp add` does not gain the tool; `use_tool` in that session returns tool not found until a new session loads the config.

## nginx

```
curl -skS --resolve hart-server:8443:127.0.0.1 \
  -X POST https://hart-server:8443/mcp \
  -H 'Content-Type: application/json' -H "Authorization: Bearer $LAPLACE_MODULAR_MCP_TOKEN" \
  -d '{"op":"search","input":"Sherlock Holmes"}'
```

`op` is any route by its name (`search`, `code_where`, `file_outline`, ...), `embed` for `embeddings`, `frechet` for `shape`, or `surface`.

## Code index

`code_search`, `reindex` and `rerank` work on a local code index held by the process, not on the database. The index is built on the client the way content is admitted: a file whose suffix has a compiled tree-sitter grammar (`LAPLACE_GRAMMARS`) is its syntax tree, `.tsv`/`.csv` are records, anything else that is text is UAX #29. Documents are:

- each declaration-sized grammar node (3 to 120 lines) with no ancestor that is already a document; a larger node, such as a namespace or a class, is not a document and its members are;
- for text with no grammar and more than 48 lines, blocks of 8 to 48 lines cut at blank lines and before `#` headings;
- every file as a whole.

A query is decomposed the same way. Documents are ranked by BM25 over the query constituents they hold (presence only, IDF `log(1 + N/df)`, `k1 = 1.2`, `b = 0.75` on document bytes), so a declaration that holds the query ranks above the file around it. A span that overlaps a better hit in the same file is not returned again. Paths are returned with `/` separators.

| Name | What |
|---|---|
| `LAPLACE_CODE_ROOTS` | directories indexed and kept up to date, separated by `;` (or `:` on Linux) |
| `LAPLACE_CODE_RECORDS` | records file: loaded on the first call when it exists and is this version, else the index is built from the roots and saved; saved again whenever a refresh changes anything |
| `LAPLACE_CODE_REFRESH_SECONDS` | how often a `code_search` call first brings the index up to date with its roots; `0` never (default `30`) |

Records (version 2) hold every entity with its constituents, every file with its size, modification time and content hash, every document, and the postings, so a loaded index answers as the one that was saved. A refresh, `reindex`, or `lpm update` stats each file under the roots: a file whose size and time agree is not read, one whose content hash agrees is not admitted again, a changed or new file is admitted, and a file no longer there is removed. Documents of changed and removed files stay in the records as stale and are never answers; when they pass 30% of all documents the index is built again from the roots. On the Laplace workspace (4014 files, 36457 documents) the records are 317 MB, a refresh with nothing changed takes 0.1 s, and loading them about 0.9 s.

`lpm index RECORDS PATH...` builds records from the command line, `lpm update RECORDS PATH...` brings them up to date, `lpm search RECORDS QUERY` queries them, and `lpm fetch RECORDS ID` shows an entity and its constituents.

An agent on the same machine runs the server over stdio (`lpm mcp`, one JSON-RPC message per line). For example, in `~/.codex/config.toml`:

```toml
[mcp_servers.laplace-code]
command = "D:/Libraries/build/Laplace/Laplace-MCP/icx-release/lpm.exe"
args = ["mcp"]
env = { LAPLACE_CODE_RECORDS = "E:/Data/Laplace/work/code-index/workspace.lpm" }
```

`code_search` takes `query` (required), `limit` (1 to 64, default 15) and `path` (a file or directory to add before searching). Each hit has `path`, `line0`, `line1`, `id`, `hilbert`, `shared` (query constituents held), `frechet` (4D Fréchet of the query and snippet word trajectories), `rerank` (the `rerank` tool's score of the snippet, 0 to 1) and `snippet` (up to 60 lines read from the file).

`rerank` scores a candidate text against the query in [0, 1]. Each query constituent the candidate holds counts with its IDF weight times the chance that holding it is not an accident of length: in a text of L bytes, a constituent found in a fraction r of the index's documents (mean length A) is present by chance with probability 1 - (1 - r)^(L/A), so the match is worth its weight times (1 - that). The lexical evidence is the sum over the weight of all query constituents; the 4D Fréchet shape of the query inside the candidate and the centroid distance scale it (`lexical * (0.55 + 0.35 shape + 0.10 centroid)`), so text with none of the query scores 0 and the same evidence in a longer text scores lower. `lpm rerank RECORDS QUERY FILE[:L0-L1]...` prints the score and its terms.

`reindex` takes `path` (optional: one file or directory; otherwise every root) and returns `unchanged`, `changed`, `added`, `removed`, `rebuilt`, `files`, `docs`, `stale`, `entities` and `ms`.
### Semantic candidates

With `LAPLACE_CODE_SEMANTIC=1`, `code_search` also embeds the query with an OpenAI-compatible `/v1/embeddings` endpoint and takes the nearest chunks from Qdrant collections built from the same trees (code-search's collections: payload `path` is the repository label then the path inside it, with `start`, `end` and `text`). A label resolves to the root in `LAPLACE_CODE_ROOTS` with that directory name. The lexical candidates (three times `limit`) and the semantic ones are joined; a semantic chunk that overlaps a lexical document is that document, with the chunk's cosine. Each candidate's text is scored by Laplace `rerank` against the query. The union is ordered by weighted reciprocal rank fusion: a candidate scores the sum of weight/(60 + rank) over the rankings that hold it. Those rankings are each collection's own order (one model's cosines are not compared with another's) and the rerank's order. A code collection weighs 1; the rerank and a prose collection (`LAPLACE_CODE_TEXT_COLLECTIONS`) weigh 0.5 each unless `LAPLACE_CODE_RERANK_WEIGHT` or `LAPLACE_CODE_PROSE_WEIGHT` (0 to 1; 0 drops that ranking) say otherwise. `scope` (or `LAPLACE_CODE_SCOPE`) chooses which collections are asked: `code` (the default when code collections are configured; otherwise `all`), `docs` (the prose ones), or `all`; asking a scope with no collection configured is an error. Prose is opt-in because this is a code search: on the eval the prose collection never held an answer and took about half the first ten places (MRR 0.600 with `code`, 0.553 with `all`). Documentation is still found through the lexical retrieval, and through `docs` or `all` when asked. The response names its `scope` and the `weights` it used. The lexical retrieval contributes candidates, and the rerank already weighs what they share. `fusion: "rerank"` (or `LAPLACE_CODE_FUSION=rerank`) orders the union by the rerank alone, as before. Each hit has `fused` and `ranks` (`rerank`, `lexical` when lexical retrieval found it, and the rank in each collection that returned it). `explain: true` returns every candidate, not only the first `limit`.

`tests/code-search-eval.py` asks ten questions about the Laplace repositories through `lpm mcp` and reports where each order puts the file that answers them (hit@1, @5, @10, mean reciprocal rank). It runs wherever the semantic configuration is set, and says it skipped otherwise.

| Name | What |
|---|---|
| `LAPLACE_CODE_SEMANTIC` | `1` to use semantic candidates; unset or `0` lexical only |
| `LAPLACE_CODE_QDRANT` | Qdrant base URL, for example `http://192.168.1.2:6333` |
| `LAPLACE_CODE_QDRANT_KEY` | sent as `api-key` when set |
| `LAPLACE_CODE_EMBED_URL`, `LAPLACE_CODE_EMBED_KEY` | code embedding endpoint and its bearer key |
| `LAPLACE_CODE_COLLECTIONS` | comma-separated collections whose vectors that endpoint made |
| `LAPLACE_CODE_QUERY_PREFIX` | text before the query (default `Find the most relevant code snippet given the following query:` and a newline, for jina-code-embeddings) |
| `LAPLACE_CODE_TEXT_EMBED_URL`, `LAPLACE_CODE_TEXT_EMBED_KEY` | optional second endpoint for prose collections (key defaults to `LAPLACE_CODE_EMBED_KEY`) |
| `LAPLACE_CODE_TEXT_COLLECTIONS` | collections for the second endpoint |
| `LAPLACE_CODE_TEXT_QUERY_PREFIX` | default `Instruct: Given a question, retrieve passages that answer it` then a newline and `Query: `, for Qwen3-Embedding |
| `LAPLACE_CODE_SCOPE` | `code` (default when `LAPLACE_CODE_COLLECTIONS` is set, else `all`), `docs` or `all`: the collections asked when a call does not say |
| `LAPLACE_CODE_RERANK_WEIGHT`, `LAPLACE_CODE_PROSE_WEIGHT` | the weight of the rerank's ranking and of each prose collection's in the fusion, 0 to 1 (default 0.5 each; a code collection weighs 1) |
| `LAPLACE_CODE_SEMANTIC_CANDIDATES` | nearest chunks taken per collection (default three times `limit`) |

Only `http://` URLs are supported. When semantic candidates are configured and an endpoint is unreachable, refuses the key, or a collection does not exist, `code_search` is an error naming the URL and the response; it does not answer from the lexical index alone. The result reports `semantic`, `semantic_ms` and `candidates` (`lexical`, `semantic`, `union`); each hit has `source` (`lexical`, `semantic` or `both`), and a hit with a semantic source has `cosine` and `collection`. A semantic-only hit whose file is under a root shows those lines from the file; otherwise it shows the chunk text.

On HART-DESKTOP the services are the llama.cpp servers on `127.0.0.1:8711` (code) and `:8713` (prose) and the collections `laplace-desktop-code` and `laplace-desktop-text` on hart-server's Qdrant.

## Code from the database

`code_where`, `code_callers`, `code_callees`, `sources`, `ingest_status` and `file_outline` read the database (`LAPLACE_CONNINFO`), not the local index: they answer for code a recipe source ingested, such as Laplace-Engine's `workspace-code`. Nothing is read from a working tree.

`code_where` takes `query` (required: a name, or any exact span of code), `path` (a substring the file's path must contain), `defines` (true: only files that define it), `limit` (files, default 20), `occurrences` (per file, default 3, at most 16) and `context` (bytes, default 120). It walks up the container index from the query's entity through every path that holds it to the files: a file is a trunk over its metadata and its content, and the walk stops there. Each hit has `path` (the file's pathname record), `file` (its trunk ID), `defines` (the declaration kinds the code recipe said of the name in that file, such as `function_definition` or `class_declaration`; files that define it come first), `occurrences` (how many places in its syntax tree hold it) and `at`: for each place, `line`, `context` (the innermost enclosing node of at least `context` bytes), `enclosing` (the first line of the outermost enclosing node under 8 KiB below the file, usually the declaration) with its span `line0`..`line1`, and `depth`. Lines are counted in the file's recorded content. The answer also has `files`, `nodes` and `levels` walked, and `cut` when the walk reached 400,000 nodes and stopped (a name as common as `return`).

`sources` returns each witness in the database: `id`, `name` (the first part of its trunk) and `trust`.

`file_outline` takes `path` (the end of a file's pathname, matched at a `/` boundary) or `file` (the file's ID, as `code_where` returns it). For each matching file (up to 16) it returns `path`, `file`, `bytes`, and `declarations`: the claims `[kind, name]` its code recipe said of it, in the order said, each with `line`, the first line at or after the previous declaration where `name` stands as a whole word. A C recipe says parameters as well as functions, so a C function's parameters follow it.

`code_callers` takes `query` (required: a declared name), `path` (a substring the file's path must contain) and `limit` (callers, default 50). Each place `code_where` finds the name (a path up from it that is not a claim) is a chain of nodes below its file, each found where it stands in the file's recorded content. A node that starts on the line of a declaration in the file's outline, and holds that declaration's name in its first line before the place, is that declaration's node; the innermost one holds the place. `callers` lists those declarations: `name`, `kind`, `path`, `file`, `line` (the declaration's) and `at` (the lines of its uses). A place that no declaration holds is listed in `top_level` (`path`, `file`, `at`). A declaration's own name, its head, is not a use of it. Same-named declarations in different files are not told apart. The answer also has `files` and `cut`, as `code_where` has.

`code_callees` takes `query` (required: a declared name) and `path` (a substring the defining file's path must contain). For each definition of the name (a file that defines it, at a line its outline gives), its code is the outermost node of the place's chain that starts on that line. `definitions` lists `path`, `file`, `kind`, `line0`..`line1` and `callees`: each name in that code followed by `(` that a code recipe declared, meaning a claim `[kind, name]` said by a file's record of claims, or, when that record would hold only the one claim, the claim said in its place as the record (the engine says a record of one claim as that claim). Each callee has `name`, `kind`, `declared_in` (how many files) and up to 4 `files` (`path`, `file`). `outside` lists the names it calls that nothing in the database declares, such as a library's or the language's.

`ingest_status` takes `source` (optional: one source's name) and reads only. `sources` lists each witness with `id`, `name` (the first 200 bytes of its record), `trust`, `parts` (how many parts its trunk holds after its record) and `readings` (how many witnesses begin with the same record). More than one reading is a source read again without superseding its earlier reading (Laplace-Engine `supersede`). `activity` lists the database's sessions that are not idle, as `pg_stat_activity` shows them to this role: `pid`, `user`, `state`, `seconds` and `query` (200 bytes). `logs` lists the engine's per-source ingest logs from `LAPLACE_INGEST_LOGS`, else `$LAPLACE_WORK/logs/ingest`, newest first, at most 20: `source`, `modified` (UTC), `bytes` and `last` (the last line). It is `null` when neither directory is set or readable.

`recipe_plan` takes `input` (optional: a source name or a path) and returns `exit` and `log` from `laplace ingest --plan`: which recipe reads which files, with nothing read into the database. Without `input` it plans every source in order.