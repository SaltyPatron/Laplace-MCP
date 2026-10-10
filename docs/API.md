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

`native/Makefile` builds against `LAPLACE_SRC`, `LAPLACE_DEPSRC`, `LAPLACE_BUILD`, `LAPLACE_DEPS`, `LAPLACE_ICU_DIR` and `LAPLACE_PG_DIR`, with the deployment target's values as defaults.

One process holds one database connection. `lpm serve` is that process. `laplace_mcp.py` is not.

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

`POST /v1/ingest` with `{"path":"/absolute/path"}` runs `/repos/build/Laplace-Engine/icx-release/laplace ingest` on that path. That is the engine pipeline: decompose, deduplicate trunk to leaf, record, attest. The body must be an absolute path that exists. The call blocks until ingest exits.

A proof file `/tmp/lpm-api-proof.txt`, one line `Sherlock Holmes kept his pipe on the mantel.` plus a newline, exited 0 in 0.2 s: 1 file, 15 compositions, 4 new entities, 11 subtrees already recorded, 0 attestations. Searching the exact bytes, ID `f27bdacd15c996339ccee43dd4153450`, returned one container, `221c6d0ef03f1be62c3625e9625f32db`, tier 4, whose text is the filename and that line.

## Surface

`GET /v1/surface` lists the routes this process serves, the database functions those routes call, the engine's twenty-one commands, and the reads that are specified and not yet a route.

Served now: embeddings, search, forward, shape, ingest, surface, health.

Engine commands that are not a route here: tier0, flags, highway, deploy, sources, forget, sweep, index, structure, tree, text, pull, turn, hop, translate, degrees, fills, status, bench, model. `hop` is the engine's program over the same container and claim reads search calls. `fills` in the engine is containers, then `lp_follows` for the exact run, then a parent walk through `laplace_fills`. `laplace_fills` by itself is that parent walk, so this service does not expose it as "what follows."

Specified past these routes, and not built as their own endpoints: translate, degrees, pull, turn, gaps, file metadata as its own read, DTW, EDR, Fréchet with skipped vertices, and the forward stages RESOLVE, COUPLE, ORIENT, ROUTE, SCAN, PROPOSE, STEER, SELECT, REALIZE, WITNESS. `laplace_couple` is the installed operator that returns strands, containment, and shape in one call. Search calls containers and claims as two reads inside one statement. It does not call `laplace_couple`.

## MCP

`POST /mcp` also speaks MCP JSON-RPC when the body contains `"jsonrpc"`. Tools: `search`, `embeddings`, `shape`, `forward`, `ingest`.

`~/.grok/config.toml` has:

```toml
[mcp_servers.laplace]
url = "http://127.0.0.1:5188/mcp"
```

`grok mcp doctor laplace` reported handshake OK, protocol 2025-11-25, 5 tools. A Grok process restricted to `laplace__search` called `search` on `Sherlock Holmes` and received the six containers and three claims. A session that was already open before `grok mcp add` does not gain the tool; `use_tool` in that session returns tool not found until a new session loads the config.

## nginx

```
curl -skS --resolve hart-server:8443:127.0.0.1 \
  -X POST https://hart-server:8443/mcp \
  -H 'Content-Type: application/json' \
  -d '{"op":"search","input":"Sherlock Holmes"}'
```

`op` is `embeddings`, `search`, `forward`, `shape`, `ingest`, or `surface`.
