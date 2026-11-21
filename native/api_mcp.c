#include "api_internal.h"

static const char *SURFACE =
    "{\"service\":\"Laplace-MCP\","
    "\"caller\":\"sends content or an ID; this process holds the database connection and AST/constituent index\","
    "\"endpoints\":["
    "{\"method\":\"POST\",\"path\":\"/v1/embeddings\",\"body\":\"{\\\"input\\\":\\\"text\\\"} or {\\\"input\\\":[\\\"a\\\",\\\"b\\\"]}\",\"does\":\"names the entity from tier 0 and returns OpenAI-compatible 4D coordinate embedding + Laplace ID/Hilbert/parts\"},"
    "{\"method\":\"POST\",\"path\":\"/v1/rerank\",\"body\":\"{\\\"query\\\":\\\"text\\\",\\\"documents\\\":[\\\"a\\\",\\\"b\\\"],\\\"top_n\\\":10}\",\"does\":\"deterministic IDF constituent overlap + 4D Frechet trajectory reranker\"},"
    "{\"method\":\"POST\",\"path\":\"/v1/code_search\",\"body\":\"{\\\"query\\\":\\\"text\\\",\\\"path\\\":\\\"optional\\\",\\\"limit\\\":15}\",\"does\":\"Tree-sitter AST declaration + UAX #29 code search with 4D Frechet trajectory ranking\"},"
    "{\"method\":\"POST\",\"path\":\"/v1/reindex\",\"body\":\"{\\\"path\\\":\\\"optional\\\"}\",\"does\":\"rebuilds the AST/constituent code index\"},"
    "{\"method\":\"POST\",\"path\":\"/v1/search\",\"body\":\"{\\\"input\\\":\\\"text\\\"} or {\\\"id\\\":\\\"32 hex\\\"}; \\\"text\\\":true renders laplace_text\",\"does\":\"one ID into laplace_containers and laplace_claims\"},"
    "{\"method\":\"POST\",\"path\":\"/v1/forward\",\"body\":\"{\\\"input\\\":\\\"prompt\\\"}\",\"does\":\"laplace_forward on the prompt constituents\"},"
    "{\"method\":\"POST\",\"path\":\"/v1/shape\",\"body\":\"{\\\"input\\\":\\\"text\\\",\\\"other\\\":\\\"text\\\"}\",\"does\":\"laplace_frechet4d on stored constituent coordinates\"},"
    "{\"method\":\"POST\",\"path\":\"/v1/ingest\",\"body\":\"{\\\"path\\\":\\\"/absolute/path\\\"}\",\"does\":\"laplace ingest: decompose, deduplicate, record, attest\"},"
    "{\"method\":\"POST\",\"path\":\"/v1/code_where\",\"body\":\"{\\\"query\\\":\\\"name or span\\\",\\\"path\\\":\\\"optional\\\",\\\"defines\\\":true}\",\"does\":\"where a name or span of code stands in the code the database holds, read off the containment above it\"},"
    "{\"method\":\"POST\",\"path\":\"/v1/file_outline\",\"body\":\"{\\\"path\\\":\\\"end of a pathname\\\"} or {\\\"file\\\":\\\"32 hex\\\"}\",\"does\":\"the typed declaration claims a code recipe said of a file, in order, with lines\"},"
    "{\"method\":\"POST\",\"path\":\"/v1/code_callers\",\"body\":\"{\\\"query\\\":\\\"declared name\\\",\\\"path\\\":\\\"optional\\\"}\",\"does\":\"the declarations whose code uses a name, with the lines\"},"
    "{\"method\":\"POST\",\"path\":\"/v1/code_callees\",\"body\":\"{\\\"query\\\":\\\"declared name\\\",\\\"path\\\":\\\"optional\\\"}\",\"does\":\"what a declaration calls that a code recipe declared, and where\"},"
    "{\"method\":\"POST\",\"path\":\"/v1/sources\",\"does\":\"the witnesses the database holds\"},"
    "{\"method\":\"POST\",\"path\":\"/v1/library\",\"body\":\"{\\\"after\\\":\\\"optional canonical ID cursor\\\"}\",\"does\":\"stored source and file trunks by the native kind bank, independent of registration\"},"
    "{\"method\":\"POST\",\"path\":\"/v1/catalog\",\"does\":\"exact qualified table counts and connected database/read-only status\"},"
    "{\"method\":\"POST\",\"path\":\"/v1/provenance\",\"body\":\"{\\\"id\\\":\\\"32 hex\\\"}\",\"does\":\"bounded containment ancestry with ordered stored vertices, source/file labels, and separate registry metadata\"},"
    "{\"method\":\"POST\",\"path\":\"/v1/ingest_status\",\"body\":\"{\\\"source\\\":\\\"optional\\\"}\",\"does\":\"each witness with its parts and readings, the database's busy sessions, the ingest logs\"},"
    "{\"method\":\"POST\",\"path\":\"/v1/recipe_plan\",\"body\":\"{\\\"input\\\":\\\"optional source or path\\\"}\",\"does\":\"laplace ingest --plan\"},"
    "{\"method\":\"POST\",\"path\":\"/v1/recipe_catalog\",\"body\":\"{}\",\"does\":\"all available sources, aliases, content locations, decomposers and prerequisites; no database or corpus required\"},"
    "{\"method\":\"POST\",\"path\":\"/v1/hop\",\"body\":\"{\\\"input\\\":\\\"text\\\"}\",\"does\":\"laplace hop\"},"
    "{\"method\":\"POST\",\"path\":\"/v1/text\",\"body\":\"{\\\"input\\\":\\\"32 hex\\\"}\",\"does\":\"laplace text\"},"
    "{\"method\":\"POST\",\"path\":\"/v1/translate\",\"body\":\"{\\\"input\\\":\\\"word\\\",\\\"from\\\":\\\"en\\\",\\\"to\\\":\\\"fr\\\"}\",\"does\":\"laplace translate\"},"
    "{\"method\":\"POST\",\"path\":\"/v1/pull\",\"body\":\"{\\\"input\\\":\\\"text\\\"}\",\"does\":\"laplace pull\"},"
    "{\"method\":\"POST\",\"path\":\"/v1/turn\",\"body\":\"{\\\"input\\\":\\\"prompt\\\",\\\"session\\\":\\\"optional\\\",\\\"user\\\":\\\"optional\\\",\\\"read\\\":true}\",\"does\":\"laplace turn: 11-stage stateful forward program (RESOLVE..WITNESS)\"},"
    "{\"method\":\"POST\",\"path\":\"/v1/degrees\",\"body\":\"{\\\"input\\\":\\\"text\\\",\\\"other\\\":\\\"text\\\"}\",\"does\":\"laplace degrees\"},"
    "{\"method\":\"GET\",\"path\":\"/v1/surface\"},"
    "{\"method\":\"GET\",\"path\":\"/v1/schema\",\"does\":\"native view descriptors, mask banks, occurrence roles and operation input schemas\"},"
    "{\"method\":\"GET\",\"path\":\"/health\",\"does\":\"liveness: the process answers; no token, no database read\"},"
    "{\"method\":\"GET\",\"path\":\"/health/ready\",\"does\":\"readiness: the database answers a trivial query and holds the laplace extension; 503 with the reason when it does not\"},"
    "{\"method\":\"POST\",\"path\":\"/mcp\",\"body\":\"JSON-RPC 2.0 MCP or op: embeddings|rerank|code_search|reindex|code_where|code_callers|code_callees|sources|ingest_status|file_outline|recipe_plan|recipe_catalog|search|forward|shape|frechet|ingest|hop|text|translate|degrees|pull|turn|surface\"}"
    "],"
    "\"database_functions\":[\"laplace_containers\",\"laplace_claims\",\"laplace_claims_each\",\"laplace_text\",\"laplace_fills\",\"laplace_forward\",\"laplace_frechet4d\",\"laplace_dtw4d\",\"laplace_edr4d\",\"laplace_distance4d\",\"laplace_couple\",\"laplace_follows\",\"laplace_paths\",\"laplace_attested\",\"laplace_compose\",\"laplace_parts\"],"
    "\"engine_commands\":[\"tier0\",\"flags\",\"highway\",\"deploy\",\"sources\",\"ingest\",\"forget\",\"sweep\",\"index\",\"structure\",\"tree\",\"text\",\"pull\",\"turn\",\"hop\",\"translate\",\"degrees\",\"fills\",\"status\",\"bench\",\"model\"]}";

static const lp_mcp_tool LPM_TOOLS[] = {
    { "library",
      "Browse stored source and file trunks using their physicality kind role, independently of witness registration. Canonical-ID cursor pagination.",
      "{\"type\":\"object\",\"properties\":{\"after\":{\"type\":\"string\"}},\"required\":[]}",
      on_library },
    { "catalog",
      "Exact table counts and connected database/read-only scope. Registry row counts do not count all witness trunks.",
      "{\"type\":\"object\",\"properties\":{},\"required\":[]}",
      on_catalog },
    { "provenance",
      "Bounded structural ancestry of an exact canonical ID. Stored edge positions, run and said are retained. Source/file trunks are read from the DAG, and registry metadata is separate. complete=false means additional ancestry may exist.",
      "{\"type\":\"object\",\"properties\":{\"id\":{\"type\":\"string\"}},\"required\":[\"id\"]}",
      on_provenance },
    { "code_search",
      "Search the code and documentation an ingested source holds (default the Laplace Workspace witness, the workspace-code ingest), over the database alone: the query's terms (its UAX #29 word segments and its runs of segments with no space between them, such as identifiers) are walked up the container index to the source's files, each file scored by the inverse document frequency of the terms it holds, and its answer is the lowest node of its content that holds them (their common ancestor). Returns files, line ranges (line0..line1) of that node in the file's recorded text, its entity ID and Hilbert index, shared term count, score (rerank, normalized to the query; bm25, raw) and snippet, and each term's file frequency. path: a substring the file path must contain. source: a witness name.",
      "{\"type\":\"object\",\"properties\":{\"query\":{\"type\":\"string\"},\"path\":{\"type\":\"string\",\"description\":\"a substring the file path must contain\"},\"source\":{\"type\":\"string\",\"description\":\"the witness whose files are searched (default LAPLACE_CODE_SOURCE, else Laplace Workspace)\"},\"limit\":{\"type\":\"integer\"},\"bridge\":{\"type\":\"boolean\",\"description\":\"add to the query the words the database's wordnets reach from its words (laplace translate)\"},\"scope\":{\"type\":\"string\",\"enum\":[\"all\",\"code\",\"docs\"]},\"explain\":{\"type\":\"boolean\",\"description\":\"every candidate, not only the first limit\"}},\"required\":[\"query\"]}",
      on_code_search },
    { "code_where",
      "Where a name or any exact span of code (query) stands in the code the Laplace database holds (an ingested source such as workspace-code), read off the containment above it, nothing from a working tree. Each file that holds it, by the pathname its record gives: defines, the declaration kinds its recipe said of the name there (function_definition, class_declaration...), files that define it first; occurrences, how many places in its syntax tree hold it; at, up to occurrences (default 3) of them: the line, the innermost enclosing node of at least context bytes (default 120), and the enclosing declaration's first line with its span line0..line1. defines true: only files that define it. path: a substring the file path must contain.",
      "{\"type\":\"object\",\"properties\":{\"query\":{\"type\":\"string\"},\"path\":{\"type\":\"string\"},\"defines\":{\"type\":\"boolean\"},\"limit\":{\"type\":\"integer\"},\"occurrences\":{\"type\":\"integer\"},\"context\":{\"type\":\"integer\"}},\"required\":[\"query\"]}",
      on_code_where },
    { "code_callers",
      "Who uses a name (query) in the code the Laplace database holds: each declaration (name, kind, path, its line) whose code holds a place the name stands, with the lines it does at (at). A place outside every declaration is its file's top level (top_level); a declaration's own name is not a use of it. Read off the containment code_where walks and the file's outline; same-named declarations in different files are not told apart. path: a substring the file path must contain.",
      "{\"type\":\"object\",\"properties\":{\"query\":{\"type\":\"string\",\"description\":\"a declared name\"},\"path\":{\"type\":\"string\"},\"limit\":{\"type\":\"integer\"}},\"required\":[\"query\"]}",
      on_code_callers },
    { "code_callees",
      "What a declaration calls, in the code the Laplace database holds: for each definition of the name (query; path, kind, line0..line1), each name in its code followed by ( that a code recipe declared, with its kind and the files that declare it (callees), and the names it calls that nothing there declares (outside: a library's, a language's). path: a substring the defining file's path must contain.",
      "{\"type\":\"object\",\"properties\":{\"query\":{\"type\":\"string\",\"description\":\"a declared name\"},\"path\":{\"type\":\"string\"}},\"required\":[\"query\"]}",
      on_code_callees },
    { "ingest_status",
      "What the Laplace database holds and how current it is, read only: each source's witness (name, trust, parts: what its trunk holds after its record, and readings: how many witnesses begin with its record; more than one is a source read again without superseding its earlier reading), the database's sessions that are not idle (a running ingest), and the engine's per-source ingest logs, newest first, with each one's last line. source: one source's name.",
      "{\"type\":\"object\",\"properties\":{\"source\":{\"type\":\"string\"}},\"required\":[]}",
      on_ingest_status },
    { "sources",
      "The sources the Laplace database holds: each witness ID, its name and trust.",
      "{\"type\":\"object\",\"properties\":{},\"required\":[]}",
      on_sources },
    { "file_outline",
      "What a file in the Laplace database declares: the typed declaration claims its code recipe said of it, in the file's order, each with the line where its name first stands after the one before. The file by the end of its pathname (path) or its ID (file, as code_where returns it).",
      "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"},\"file\":{\"type\":\"string\"},\"include_text\":{\"type\":\"boolean\",\"description\":\"Include the source text reconstructed from persisted records.\"}},\"required\":[]}",
      on_file_outline },
    { "recipe_plan",
      "Which recipe reads what, as laplace ingest --plan answers it, without reading anything into the database: a source name or a path (input), else every source in order.",
      "{\"type\":\"object\",\"properties\":{\"input\":{\"type\":\"string\"}},\"required\":[]}",
      on_recipe_plan },
    { "recipe_catalog",
      "Complete available ingestion sources, short aliases, content locations, recipes, file formats and prerequisites from the Engine; no database or corpus required.",
      "{\"type\":\"object\",\"properties\":{},\"required\":[]}",
      on_recipe_catalog },
    { "reindex",
      "Bring the database's code up to date: laplace ingest of the code source again (LAPLACE_CODE_INGEST, default workspace-code, whose recipe supersedes what changed), or of one absolute file or directory given as path. Returns the engine's exit code and log.",
      "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"}},\"required\":[]}",
      on_reindex },
    { "rerank",
      "Deterministic Laplace reranker: scores candidate documents against a query by the inverse frequency, over the database's containment index, of the query's terms (UAX #29 word segments and unspaced runs) each document holds, as a share of all the query's terms.",
      "{\"type\":\"object\",\"properties\":{\"query\":{\"type\":\"string\"},\"documents\":{\"type\":\"array\",\"items\":{\"type\":\"string\"}},\"top_n\":{\"type\":\"integer\"}},\"required\":[\"query\",\"documents\"]}",
      on_rerank },
    { "search",
      "Name text on the client from tier 0, or take an ID the caller already holds. The trunk and non-atomic constituent references are read with holds_above. A scalar stays in the path, in its place, with its codepoint. Discovery matches report database or canonical-cache presence; a calculated root is not a discovery match. Member query ordinals remain distinct. Stored tiers are returned separately from the computed text modality tier. fan, when sent, caps how many claims and occurrences are returned; omitted, all of them are.",
      "{\"type\":\"object\",\"properties\":{\"input\":{\"type\":\"string\"},\"id\":{\"type\":\"string\"},\"fan\":{\"type\":\"integer\"}},\"required\":[]}",
      on_search },
    { "embeddings",
      "Same call shape as an embedding endpoint. Returns the Laplace entity ID, tier, 4D coordinate (also exposed as OpenAI-compatible embedding array), Hilbert value, and constituent IDs. Does not touch the database.",
      "{\"type\":\"object\",\"properties\":{\"input\":{\"type\":\"string\"},\"inputs\":{\"type\":\"array\",\"items\":{\"type\":\"string\"}}},\"required\":[]}",
      on_embeddings },
    { "shape",
      "Discrete Frechet distance between two texts, laplace_frechet4d on the stored coordinates of their constituents.",
      "{\"type\":\"object\",\"properties\":{\"input\":{\"type\":\"string\"},\"other\":{\"type\":\"string\"}},\"required\":[\"input\",\"other\"]}",
      on_shape },
    { "forward",
      "laplace_forward on the constituent IDs of a prompt: which spans are observed and what follows them.",
      "{\"type\":\"object\",\"properties\":{\"input\":{\"type\":\"string\"}},\"required\":[\"input\"]}",
      on_forward },
    { "ingest",
      "laplace ingest of an absolute filesystem path: decompose, deduplicate, record, attest into the Laplace database.",
      "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"}},\"required\":[\"path\"]}",
      on_ingest },
    { "hop",
      "laplace hop: claims that hold the named entity, and the tiers of the paths that contain it.",
      "{\"type\":\"object\",\"properties\":{\"input\":{\"type\":\"string\"}},\"required\":[\"input\"]}",
      on_hop },
    { "text",
      "laplace text: the entity ID, coordinate, and constituents, computed on the client. No database.",
      "{\"type\":\"object\",\"properties\":{\"input\":{\"type\":\"string\"}},\"required\":[\"input\"]}",
      on_text },
    { "translate",
      "laplace translate: a word up through its concepts and down into another language. Languages as the resources write them, such as en, de, fr, ja.",
      "{\"type\":\"object\",\"properties\":{\"input\":{\"type\":\"string\"},\"from\":{\"type\":\"string\"},\"to\":{\"type\":\"string\"}},\"required\":[\"input\",\"from\",\"to\"]}",
      on_translate },
    { "degrees",
      "laplace degrees: how far one entity is from another over rated claims.",
      "{\"type\":\"object\",\"properties\":{\"input\":{\"type\":\"string\"},\"other\":{\"type\":\"string\"}},\"required\":[\"input\",\"other\"]}",
      on_degrees },
    { "pull",
      "laplace pull: the forward pass. The prompt is decomposed, trajectories give precedes, contains and co-occurrence, claims tug back by their Glicko standing, and the firmware's take steps choose the segment. No fan cap is passed; the firmware's own fan and hops apply.",
      "{\"type\":\"object\",\"properties\":{\"input\":{\"type\":\"string\"}},\"required\":[\"input\"]}",
      on_pull },
    { "turn",
      "laplace turn: the 11-stage stateful forward program (RESOLVE -> COUPLE -> ORIENT -> ROUTE -> SCAN -> COMPOSE -> PROPOSE -> STEER -> SELECT -> REALIZE -> WITNESS). Defaults to read=true when session is omitted.",
      "{\"type\":\"object\",\"properties\":{\"input\":{\"type\":\"string\"},\"session\":{\"type\":\"string\"},\"user\":{\"type\":\"string\"},\"read\":{\"type\":\"boolean\"}},\"required\":[\"input\"]}",
      on_turn },
};

/* View projections are not entity subclasses. Every canonical ID still names
 * the same content when used as a claim, file, source or constituent. */
static void on_schema(int c) {
    const lp_highway *highway = api_view_highway();
    if (!highway) { api_http(c, 503, "{\"error\":\"highway schema unavailable\"}"); return; }
    lp_buf out = { 0 };
    uint8_t fingerprint[32];
    lp_highway_fingerprint(highway, fingerprint);
    lp_buf_cstr(&out, "{\"schema\":\"laplace-view-schema/v1\",\"highway\":\"");
    for (size_t i = 0; i < 32; i++) lp_buf_fmt(&out, "%02x", fingerprint[i]);
    lp_buf_cstr(&out, "\",\"identity\":{\"algorithm\":\"BLAKE3-128\",\"input\":\"ordered constituent IDs, repeats included\",\"singleton\":\"child\"},\"views\":{");
    static const char *view[][4] = {
        {"entity","Entity","Entities","Canonical content shared across every structure that references its ID."},
        {"physicality","Physicality","Physicalities","The physical form of the paired entity: ordered constituent references and occurrence annotations."},
        {"claim","Claim","Claims","A composition used as a claim. Its identity is distinct from its assertions and standing."},
        {"container","Containing path","Containing paths","A parent composition whose physicality references this content."},
        {"attestation","Attestation","Attestations","A claim attributed through containment to a source trunk."},
        {"source","Source trunk","Source trunks","The source trunk in the same Merkle DAG as its files, records and content."},
        {"file","File trunk","File trunks","A composition referencing file metadata and its recorded content tree."}
    };
    for (size_t i = 0; i < sizeof view / sizeof *view; i++)
        lp_buf_fmt(&out, "%s\"%s\":{\"name\":\"%s\",\"plural\":\"%s\",\"description\":\"%s\"}", i ? "," : "", view[i][0], view[i][1], view[i][2], view[i][3]);
    lp_buf_cstr(&out, "},\"banks\":[");
    for (size_t i = 0; i < highway->nbanks; i++) {
        const lp_bank *bank = highway->bank + i;
        lp_buf_fmt(&out, "%s{\"name\":\"%s\",\"group\":\"%s\",\"carrier\":\"%s\",\"list\":\"%s\",\"width\":%u,\"offset\":%u,\"encoding\":\"%s\",\"slots\":%u,\"values\":[",
                   i ? "," : "", bank->name, bank->group, bank->carrier, bank->list->name, bank->width, bank->offset, bank->flags ? "flags" : "enum", bank->list->count);
        unsigned shown = 0;
        for (uint32_t slot = 0; slot < bank->list->count; slot++) {
            const lp_tier0_record *record = lp_highway_at(highway, bank->list, slot);
            const char *label = lp_highway_identifier(highway, bank->list, slot);
            if (!record) continue;
            char id[33];
            api_hex_id(&record->id, id);
            lp_buf_fmt(&out, "%s{\"slot\":%u,\"id\":\"%s\",\"label\":", shown++ ? "," : "", slot, id);
            lp_json_str(&out, label ? label : "", strlen(label ? label : ""));
            lp_buf_byte(&out, '}');
        }
        lp_buf_cstr(&out, "]}");
    }
    lp_buf_cstr(&out, "],\"vertex_roles\":[");
    static const struct { unsigned value; const char *name; } roles[] = {
        {0,"Constituent"},{LP_SAID_CLAIM,"Claim"},{LP_SAID_RECORD,"Record"},
        {LP_SAID_TUPLE,"Tuple"},{LP_SAID_METADATA,"Metadata"},{LP_SAID_HOLDS,"Content tree"},{LP_SAID_VOICE,"Voice"}
    };
    for (size_t i = 0; i < sizeof roles / sizeof *roles; i++)
        lp_buf_fmt(&out, "%s{\"value\":%u,\"name\":\"%s\"}", i ? "," : "", roles[i].value, roles[i].name);
    lp_buf_cstr(&out, "],\"operations\":");
    lp_mcp_tools_json(&out, LPM_TOOLS, sizeof LPM_TOOLS / sizeof LPM_TOOLS[0]);
    lp_buf_byte(&out, '}');
    api_reply_buf(c, 200, &out);
}

static int lpm_shell_init(void *ctx) {
    (void)ctx;
    api_engine_resolve();
    T0 = lp_tier0_map(NULL);
    if (!T0) {
        fprintf(stderr, "tier 0 did not map\n");
        return 1;
    }
    TX = lp_text_new(T0);
    if (!TX) return 1;
    return 0;
}

static void lpm_shell_finish(void *ctx) {
    (void)ctx;
    api_db_release();
}

static void lpm_dispatch_http(int c, const char *method, const char *path, const char *body, void *ctx) {
    (void)ctx;
    api_dispatch(c, method, path, body);
}

static const lp_http_shell LPM_SHELL = {
    .server_name    = "laplace",
    .server_version = "0.1",
    .token_env      = "LAPLACE_MCP_TOKEN",
    .token_file_env = "LAPLACE_MCP_TOKEN_FILE",
    .init           = lpm_shell_init,
    .finish_request = lpm_shell_finish,
    .dispatch_http  = lpm_dispatch_http,
    .tools          = LPM_TOOLS,
    .ntools         = sizeof LPM_TOOLS / sizeof LPM_TOOLS[0],
};

const lp_http_shell *api_lpm_shell(void) {
    return &LPM_SHELL;
}

typedef struct { const char *route; void (*handler)(int c, const char *body); } HttpRoute;
static const HttpRoute HTTP_POST_ROUTES[] = {
    { "/v1/embeddings",          on_embeddings },
    { "/embeddings",             on_embeddings },
    { "/v1/rerank",              on_rerank },
    { "/rerank",                 on_rerank },
    { "/v1/code_search",         on_code_search },
    { "/v1/reindex",             on_reindex },
    { "/v1/code_where",          on_code_where },
    { "/v1/sources",             on_sources },
    { "/v1/catalog",             on_catalog },
    { "/v1/library",             on_library },
    { "/v1/provenance",          on_provenance },
    { "/v1/file_outline",        on_file_outline },
    { "/v1/code_callers",        on_code_callers },
    { "/v1/code_callees",        on_code_callees },
    { "/v1/ingest_status",       on_ingest_status },
    { "/v1/recipe_plan",         on_recipe_plan },
    { "/v1/recipe_catalog",      on_recipe_catalog },
    { "/v1/search",              on_search },
    { "/v1/forward",             on_forward },
    { "/v1/shape",               on_shape },
    { "/v1/ingest",              on_ingest },
    { "/v1/hop",                 on_hop },
    { "/v1/text",                on_text },
    { "/v1/translate",           on_translate },
    { "/v1/degrees",             on_degrees },
    { "/v1/pull",                on_pull },
    { "/v1/turn",                on_turn },
    { "/v1/chess_position",      on_chess_position },
    { "/v1/chess_game_openings", on_chess_game_openings },
    { "/v1/chess_tablebase",     on_chess_tablebase },
    { "/v1/chess_players",       on_chess_players },
    { "/v1/chess_player",        on_chess_player },
    { "/v1/chess_games",         on_chess_games },
    { "/v1/chess_game",          on_chess_game },
};

void api_dispatch(int c, const char *method, const char *path, const char *body) {
    if (!strcmp(path, "/mcp") && strstr(body, "\"jsonrpc\"")) {
        lp_mcp_rpc(c, body, api_lpm_shell());
        return;
    }
    if (!strcmp(path, "/mcp")) {
        char op[32] = "";
        if (!api_jstr(body, "op", op, sizeof op) || !op[0]) {
            api_http(c, 400, "{\"error\":\"op\"}");
            return;
        }
        if (!strcmp(op, "surface")) { api_http(c, 200, SURFACE); return; }
        const char *mapped = !strcmp(op, "embed") ? "embeddings" : !strcmp(op, "frechet") ? "shape" : op;
        for (size_t i = 0; i < LPM_SHELL.ntools; i++) {
            if (!strcmp(LPM_TOOLS[i].name, mapped)) {
                LPM_TOOLS[i].handler(c, body);
                return;
            }
        }
        api_http(c, 400, "{\"error\":\"op\"}");
        return;
    }
    if (!strcmp(method, "GET")) {
        if (!strcmp(path, "/health")) { api_http(c, 200, "{\"ok\":true}"); return; }
        if (!strcmp(path, "/health/ready")) { on_ready(c); return; }
        if (!strcmp(path, "/v1/surface")) { api_http(c, 200, SURFACE); return; }
        if (!strcmp(path, "/v1/schema")) { on_schema(c); return; }
        api_http(c, 404, "{\"error\":\"not found\"}");
        return;
    }
    if (!strcmp(method, "POST")) {
        for (size_t i = 0; i < sizeof HTTP_POST_ROUTES / sizeof HTTP_POST_ROUTES[0]; i++) {
            if (!strcmp(path, HTTP_POST_ROUTES[i].route)) {
                HTTP_POST_ROUTES[i].handler(c, body);
                return;
            }
        }
    }
    api_http(c, 404, "{\"error\":\"not found\"}");
}

int api_mcp_stdio(void) {
    return lp_mcp_stdio(api_lpm_shell());
}
