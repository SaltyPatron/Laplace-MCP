# Laplace-MCP

HTTP service in front of Laplace. Callers send content or an ID. The process maps tier 0, names the entity, and calls the database. Callers are not linked to Postgres.

For an installed service, use Laplace-Operations' `sudo ./setup.sh modular`
(also included by its default setup on declared hosts). The installed unit is
`laplace-modular-mcp.service`; its paths, identity and allocations come from the
host's modular installation declaration. Operations calls `deployment.py` to package
this service with its matching Engine and recipes, then verifies the candidate before
replacing the active release. Runtime database connections are supplied at launch;
they are never embedded in the package. The commands below are for development.

The service compiles the pinned Engine's selected-ID reader and connection-owned
SQL plan manager. Plans retain SQL contents and follow libpq reset/destruction;
the bridge leaves generic/custom and parallel planning to the caller's settings.
API handlers include the Engine's reader declarations directly. SQL and binary
arrays use that connection's installed Native/PostGIS namespaces and type OIDs.
The database readback test installs both extensions in separate named schemas
with conflicting public objects and a restricted search path, then reads the
ingested source through search, forward, code and provenance tools.
Text display resolves scalars through Native's indexed floor,
with no codepoint-offset assumption or 256-scalar selection limit.

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
