# Laplace-MCP

A client that admits a file and returns an entity id. Fetch that id for the record. The coordinates are the physicality on the record.

A `.tsv` or `.csv` is records and fields. A suffix with a built grammar in `/repos/build/grammars` is that grammar. A leaf, a field, and a file with no recipe are text (UAX #29). The same bytes are the same entity.

```
native/lpm check
native/lpm admit path
native/lpm fetch path ID
python3 laplace_mcp.py
```

`admit` and `fetch` are the MCP tools. The server speaks MCP over stdio.
