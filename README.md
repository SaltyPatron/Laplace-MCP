# Laplace-MCP

HTTP service in front of Laplace. Callers send content or an ID. The process maps tier 0, names the entity, and calls the database. Callers are not linked to Postgres.

```
native/lpm serve 127.0.0.1 5188
```

nginx proxies `POST /mcp` on port 8443 to that process. Direct routes are under `/v1/`. The contract, the measurements, and what is not a route yet are in [docs/API.md](docs/API.md).

`laplace_mcp.py` is not the service. The `lpm` file commands (`check`, `text`, `admit`, `index`, `search`) are the in-process client. `serve` is the API, and the records it reads and writes are the Laplace database.
