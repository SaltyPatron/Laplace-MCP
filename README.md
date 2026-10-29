# Laplace-MCP

HTTP service in front of Laplace. Callers send content or an ID. The process maps tier 0, names the entity, and calls the database. Callers are not linked to Postgres.

For an installed service, use Laplace-Operations' `sudo ./setup.sh modular`
(also included by its default setup on declared hosts). The installed unit is
`laplace-modular-mcp.service`; its paths, identity and allocations come from the
host's modular installation declaration. Operations calls `deployment.py` to package
this service with its matching Engine and recipes, then verifies the candidate before
replacing the active release. Runtime database connections are supplied at launch;
they are never embedded in the package. The commands below are for development.

Installed releases contain their perf-caches, frozen Native manifests, compiled
grammars, recipes and firmware. Runtime launchers select those immutable files;
the configured `tier0`, `flags`, `highway` and `grammars` locations are packaging
inputs. Native verifies that the bundled manifests reproduce Highway without
corpora or a database. Grammar generator sources stay in the build estate.
The release also contains Operations' matching human-client controller; the
installed Linux command pins and loads it instead of retaining an old copy.
Resources share Operations' immutable file store under the modular prefix;
release retirement removes orphaned blobs after their last retained link.
The locked `incremental` build profile retains one detached source tree per
repository. Engine and MCP share one Native and Tree-sitter build graph. Ninja
rebuilds changed inputs; a toolchain fingerprint change or `--clean-build`
explicitly cleans that graph. Uncommitted edits or an attached branch in a build
snapshot stop packaging instead of being overwritten.
Engine-owned packaging grammars compile incrementally in this same profile from
the selected Engine revision. They override prepared external grammar inputs
when the release is assembled, so another product's parser build cannot select
an older packaging grammar for this release.

The service compiles the pinned Engine's selected-ID reader and connection-owned
SQL plan manager. Plans retain SQL contents and follow libpq reset/destruction;
the bridge leaves generic/custom and parallel planning to the caller's settings.
API handlers include the Engine's reader declarations directly. SQL and binary
arrays use that connection's installed Native/PostGIS namespaces and type OIDs.
Inspection shares the Engine's `read_ids.c`: stored paths and tiers are fetched
as complete frontiers through `laplace_entities` / `laplace_physicalities`.
Constituent layers retain request ordinals and repeated occurrences without
per-child SQL or client-side partition-name construction.
The database readback test installs both extensions in separate named schemas
with conflicting public objects and a restricted search path, then reads the
ingested source through search, forward, code and provenance tools.
Each database-backed response uses one connection-owned read-only snapshot.
The Engine's `read.c` delegates containment and exact cap classification to
Native `laplace_holders` / `laplace_containers`; typed standings come from
`laplace_standings`. There is no per-partition SQL or second read pool.
Response completion releases the transaction; the next request reuses the
connection and plans with a fresh snapshot.
`tests/read-snapshot.py` publishes another real source between persistent
requests and checks refreshed witness authority, idle transactions and PID reuse.
Observation counts retain their PostgreSQL 64-bit value in JSON responses.
Text display resolves scalars through Native's indexed floor,
with no codepoint-offset assumption or 256-scalar selection limit.

`code_search` searches all indexed roots. Its optional `path` refreshes a local
file or directory using the existing incremental updater: unchanged files keep
their records, changed files are replaced, and removed files leave the index.
A missing requested root fails explicitly. `LAPLACE_CODE_ROOTS` declares the
regular workspace roots; `reindex` refreshes them on demand.

Packaging serializes access to its build cache and retains the current build plus
the two most recent other builds. It removes only marked cache entries whose Git
snapshots still match their recorded revisions and contain no edits. Modified
snapshots and unrecognized directories remain available for reconciliation.

```
. ../Laplace-Operations/laplace.env
cmake -S . -B "$LAPLACE_BUILD/Laplace-MCP/icx-release" -G Ninja -DCMAKE_C_COMPILER=icx -DCMAKE_CXX_COMPILER=icpx -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build "$LAPLACE_BUILD/Laplace-MCP/icx-release"
"$LAPLACE_BUILD/Laplace-MCP/icx-release/lpm" serve     # LAPLACE_MCP_HOST and LAPLACE_MCP_PORT, or: lpm serve HOST PORT
```

nginx proxies `POST /mcp` on port 8443 to that process. Direct routes are under `/v1/`. The contract, the measurements, and what is not a route yet are in [docs/API.md](docs/API.md).

`laplace_mcp.py` is not the service. The `lpm` file commands (`check`, `text`, `admit`, `index`, `search`) are the in-process client. `serve` is the API, and the records it reads and writes are the Laplace database.
