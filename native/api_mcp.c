#define _GNU_SOURCE
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
    "{\"method\":\"GET\",\"path\":\"/health\"},"
    "{\"method\":\"POST\",\"path\":\"/mcp\",\"body\":\"JSON-RPC 2.0 MCP or op: embeddings|rerank|code_search|reindex|code_where|code_callers|code_callees|sources|ingest_status|file_outline|recipe_plan|recipe_catalog|search|forward|shape|frechet|ingest|hop|text|translate|degrees|pull|turn|surface\"}"
    "],"
    "\"database_functions\":[\"laplace_containers\",\"laplace_claims\",\"laplace_claims_each\",\"laplace_text\",\"laplace_fills\",\"laplace_forward\",\"laplace_frechet4d\",\"laplace_dtw4d\",\"laplace_edr4d\",\"laplace_distance4d\",\"laplace_couple\",\"laplace_follows\",\"laplace_paths\",\"laplace_attested\",\"laplace_compose\",\"laplace_parts\"],"
    "\"engine_commands\":[\"tier0\",\"flags\",\"highway\",\"deploy\",\"sources\",\"ingest\",\"forget\",\"sweep\",\"index\",\"structure\",\"tree\",\"text\",\"pull\",\"turn\",\"hop\",\"translate\",\"degrees\",\"fills\",\"status\",\"bench\",\"model\"]}";


static void rpc_id(const char *body, char *out, size_t cap) {
    const char *p = api_jvalue(body,"id");
    out[0] = 'n'; out[1] = 'u'; out[2] = 'l'; out[3] = 'l'; out[4] = 0;
    if (!p) return;
    size_t n = 0;
    if (*p == '"') {
        const char *start=p++;
        while(*p&&*p!='"'){if(*p=='\\'&&p[1])p++;p++;}
        if(!*p)return;n=(size_t)(p-start)+1;if(n>=cap)return;
        memcpy(out,start,n);out[n]=0;
        return;
    }
    while (*p && *p != ',' && *p != '}' && *p != ' ' && n + 1 < cap) out[n++] = *p++;
    out[n] = 0;
}

static const char *TOOLS =
    "{\"tools\":["
    "{\"name\":\"library\",\"description\":\"Browse stored source and file trunks using their physicality kind role, independently of witness registration. Canonical-ID cursor pagination.\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"after\":{\"type\":\"string\"}},\"required\":[]}},"
    "{\"name\":\"catalog\",\"description\":\"Exact table counts and connected database/read-only scope. Registry row counts do not count all witness trunks.\",\"inputSchema\":{\"type\":\"object\",\"properties\":{},\"required\":[]}},"
    "{\"name\":\"provenance\",\"description\":\"Bounded structural ancestry of an exact canonical ID. Stored edge positions, run and said are retained. Source/file trunks are read from the DAG, and registry metadata is separate. complete=false means additional ancestry may exist.\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"id\":{\"type\":\"string\"}},\"required\":[\"id\"]}},"
    "{\"name\":\"code_search\",\"description\":\"Search code and documentation across indexed repositories using Laplace multi-tier Tree-sitter AST decomposition, UAX #29 segmentation, IDF constituent postings, and 4D S^3 Frechet trajectory distance. Returns matching files, exact line ranges (line0..line1), Laplace entity ID, Hilbert index, shared constituent count, 4D Frechet distance, rerank score, and snippet.\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"query\":{\"type\":\"string\"},\"path\":{\"type\":\"string\",\"description\":\"Refresh this local file or directory before searching all indexed roots.\"},\"limit\":{\"type\":\"integer\"},\"fusion\":{\"type\":\"string\",\"enum\":[\"rrf\",\"rerank\"]},\"bridge\":{\"type\":\"boolean\",\"description\":\"add to the query the words the database's wordnets reach from its words (laplace translate), for the lexical retrieval and the rerank\"},\"scope\":{\"type\":\"string\",\"enum\":[\"all\",\"code\",\"docs\"]},\"explain\":{\"type\":\"boolean\",\"description\":\"every candidate, not only the first limit\"}},\"required\":[\"query\"]}},"
    "{\"name\":\"code_where\",\"description\":\"Where a name or any exact span of code (query) stands in the code the Laplace database holds (an ingested source such as workspace-code), read off the containment above it, nothing from a working tree. Each file that holds it, by the pathname its record gives: defines, the declaration kinds its recipe said of the name there (function_definition, class_declaration...), files that define it first; occurrences, how many places in its syntax tree hold it; at, up to occurrences (default 3) of them: the line, the innermost enclosing node of at least context bytes (default 120), and the enclosing declaration's first line with its span line0..line1. defines true: only files that define it. path: a substring the file path must contain.\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"query\":{\"type\":\"string\"},\"path\":{\"type\":\"string\"},\"defines\":{\"type\":\"boolean\"},\"limit\":{\"type\":\"integer\"},\"occurrences\":{\"type\":\"integer\"},\"context\":{\"type\":\"integer\"}},\"required\":[\"query\"]}},"
    "{\"name\":\"code_callers\",\"description\":\"Who uses a name (query) in the code the Laplace database holds: each declaration (name, kind, path, its line) whose code holds a place the name stands, with the lines it does at (at). A place outside every declaration is its file's top level (top_level); a declaration's own name is not a use of it. Read off the containment code_where walks and the file's outline; same-named declarations in different files are not told apart. path: a substring the file path must contain.\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"query\":{\"type\":\"string\",\"description\":\"a declared name\"},\"path\":{\"type\":\"string\"},\"limit\":{\"type\":\"integer\"}},\"required\":[\"query\"]}},"
    "{\"name\":\"code_callees\",\"description\":\"What a declaration calls, in the code the Laplace database holds: for each definition of the name (query; path, kind, line0..line1), each name in its code followed by ( that a code recipe declared, with its kind and the files that declare it (callees), and the names it calls that nothing there declares (outside: a library's, a language's). path: a substring the defining file's path must contain.\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"query\":{\"type\":\"string\",\"description\":\"a declared name\"},\"path\":{\"type\":\"string\"}},\"required\":[\"query\"]}},"
    "{\"name\":\"ingest_status\",\"description\":\"What the Laplace database holds and how current it is, read only: each source's witness (name, trust, parts: what its trunk holds after its record, and readings: how many witnesses begin with its record; more than one is a source read again without superseding its earlier reading), the database's sessions that are not idle (a running ingest), and the engine's per-source ingest logs, newest first, with each one's last line. source: one source's name.\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"source\":{\"type\":\"string\"}},\"required\":[]}},"
    "{\"name\":\"sources\",\"description\":\"The sources the Laplace database holds: each witness ID, its name and trust.\",\"inputSchema\":{\"type\":\"object\",\"properties\":{},\"required\":[]}},"
    "{\"name\":\"file_outline\",\"description\":\"What a file in the Laplace database declares: the typed declaration claims its code recipe said of it, in the file's order, each with the line where its name first stands after the one before. The file by the end of its pathname (path) or its ID (file, as code_where returns it).\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"},\"file\":{\"type\":\"string\"},\"include_text\":{\"type\":\"boolean\",\"description\":\"Include the source text reconstructed from persisted records.\"}},\"required\":[]}},"
    "{\"name\":\"recipe_plan\",\"description\":\"Which recipe reads what, as laplace ingest --plan answers it, without reading anything into the database: a source name or a path (input), else every source in order.\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"input\":{\"type\":\"string\"}},\"required\":[]}},"
    "{\"name\":\"recipe_catalog\",\"description\":\"Complete available ingestion sources, short aliases, content locations, recipes, file formats and prerequisites from the Engine; no database or corpus required.\",\"inputSchema\":{\"type\":\"object\",\"properties\":{},\"required\":[]}},"
    "{\"name\":\"reindex\",\"description\":\"Bring the code index up to date with LAPLACE_CODE_ROOTS, or with one file or directory given as path: unchanged files (size and time, else content) are not read, changed and new files are admitted, files no longer there are removed. Returns the counts. The index also refreshes itself at most every LAPLACE_CODE_REFRESH_SECONDS.\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"}},\"required\":[]}},"
    "{\"name\":\"rerank\",\"description\":\"Deterministic Laplace reranker (drop-in replacement for neural cross-encoders). Scores candidate documents against a query using IDF-weighted constituent overlap and 4D discrete Frechet trajectory distance.\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"query\":{\"type\":\"string\"},\"documents\":{\"type\":\"array\",\"items\":{\"type\":\"string\"}},\"top_n\":{\"type\":\"integer\"}},\"required\":[\"query\",\"documents\"]}},"
    "{\"name\":\"search\",\"description\":\"Name text on the client from tier 0, or take an ID the caller already holds. The trunk and non-atomic constituent references are read with holds_above. A scalar stays in the path, in its place, with its codepoint. Discovery matches report database or canonical-cache presence; a calculated root is not a discovery match. Member query ordinals remain distinct. Stored tiers are returned separately from the computed text modality tier. fan, when sent, caps how many claims and occurrences are returned; omitted, all of them are.\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"input\":{\"type\":\"string\"},\"id\":{\"type\":\"string\"},\"fan\":{\"type\":\"integer\"}},\"required\":[]}},"
    "{\"name\":\"embeddings\",\"description\":\"Same call shape as an embedding endpoint. Returns the Laplace entity ID, tier, 4D coordinate (also exposed as OpenAI-compatible embedding array), Hilbert value, and constituent IDs. Does not touch the database.\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"input\":{\"type\":\"string\"},\"inputs\":{\"type\":\"array\",\"items\":{\"type\":\"string\"}}},\"required\":[]}},"
    "{\"name\":\"shape\",\"description\":\"Discrete Frechet distance between two texts, laplace_frechet4d on the stored coordinates of their constituents.\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"input\":{\"type\":\"string\"},\"other\":{\"type\":\"string\"}},\"required\":[\"input\",\"other\"]}},"
    "{\"name\":\"forward\",\"description\":\"laplace_forward on the constituent IDs of a prompt: which spans are observed and what follows them.\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"input\":{\"type\":\"string\"}},\"required\":[\"input\"]}},"
    "{\"name\":\"ingest\",\"description\":\"laplace ingest of an absolute filesystem path: decompose, deduplicate, record, attest into the Laplace database.\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"}},\"required\":[\"path\"]}},"
    "{\"name\":\"hop\",\"description\":\"laplace hop: claims that hold the named entity, and the tiers of the paths that contain it.\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"input\":{\"type\":\"string\"}},\"required\":[\"input\"]}},"
    "{\"name\":\"text\",\"description\":\"laplace text: the entity ID, coordinate, and constituents, computed on the client. No database.\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"input\":{\"type\":\"string\"}},\"required\":[\"input\"]}},"
    "{\"name\":\"translate\",\"description\":\"laplace translate: a word up through its concepts and down into another language. Languages as the resources write them, such as en, de, fr, ja.\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"input\":{\"type\":\"string\"},\"from\":{\"type\":\"string\"},\"to\":{\"type\":\"string\"}},\"required\":[\"input\",\"from\",\"to\"]}},"
    "{\"name\":\"degrees\",\"description\":\"laplace degrees: how far one entity is from another over rated claims.\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"input\":{\"type\":\"string\"},\"other\":{\"type\":\"string\"}},\"required\":[\"input\",\"other\"]}},"
    "{\"name\":\"pull\",\"description\":\"laplace pull: the forward pass. The prompt is decomposed, trajectories give precedes, contains and co-occurrence, claims tug back by their Glicko standing, and the firmware's take steps choose the segment. No fan cap is passed; the firmware's own fan and hops apply.\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"input\":{\"type\":\"string\"}},\"required\":[\"input\"]}},"
    "{\"name\":\"turn\",\"description\":\"laplace turn: the 11-stage stateful forward program (RESOLVE -> COUPLE -> ORIENT -> ROUTE -> SCAN -> COMPOSE -> PROPOSE -> STEER -> SELECT -> REALIZE -> WITNESS). Defaults to read=true when session is omitted.\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"input\":{\"type\":\"string\"},\"session\":{\"type\":\"string\"},\"user\":{\"type\":\"string\"},\"read\":{\"type\":\"boolean\"}},\"required\":[\"input\"]}}"
    "]}";

/* View projections are not entity subclasses. Every canonical ID still names
 * the same content when used as a claim, file, source or constituent. */
static void on_schema(int c){
    const lp_highway *highway=api_view_highway();
    if(!highway){api_http(c,503,"{\"error\":\"highway schema unavailable\"}");return;}
    char *out=NULL;size_t n=0,cap=0;uint8_t fingerprint[32];lp_highway_fingerprint(highway,fingerprint);
    api_buf_fmt(&out,&n,&cap,"{\"schema\":\"laplace-view-schema/v1\",\"highway\":\"");
    for(size_t i=0;i<32;i++)api_buf_fmt(&out,&n,&cap,"%02x",fingerprint[i]);
    api_buf_fmt(&out,&n,&cap,"\",\"identity\":{\"algorithm\":\"BLAKE3-128\",\"input\":\"ordered constituent IDs, repeats included\",\"singleton\":\"child\"},\"views\":{");
    static const char *view[][4]={
        {"entity","Entity","Entities","Canonical content shared across every structure that references its ID."},
        {"physicality","Physicality","Physicalities","The physical form of the paired entity: ordered constituent references and occurrence annotations."},
        {"claim","Claim","Claims","A composition used as a claim. Its identity is distinct from its assertions and standing."},
        {"container","Containing path","Containing paths","A parent composition whose physicality references this content."},
        {"attestation","Attestation","Attestations","A claim attributed through containment to a source trunk."},
        {"source","Source trunk","Source trunks","The source trunk in the same Merkle DAG as its files, records and content."},
        {"file","File trunk","File trunks","A composition referencing file metadata and its recorded content tree."}
    };
    for(size_t i=0;i<sizeof view/sizeof *view;i++)api_buf_fmt(&out,&n,&cap,"%s\"%s\":{\"name\":\"%s\",\"plural\":\"%s\",\"description\":\"%s\"}",i?",":"",view[i][0],view[i][1],view[i][2],view[i][3]);
    api_buf_fmt(&out,&n,&cap,"},\"banks\":[");
    for(size_t i=0;i<highway->nbanks;i++){
        const lp_bank *bank=highway->bank+i;
        api_buf_fmt(&out,&n,&cap,"%s{\"name\":\"%s\",\"group\":\"%s\",\"carrier\":\"%s\",\"list\":\"%s\",\"width\":%u,\"offset\":%u,\"encoding\":\"%s\",\"slots\":%u,\"values\":[",i?",":"",bank->name,bank->group,bank->carrier,bank->list->name,bank->width,bank->offset,bank->flags?"flags":"enum",bank->list->count);
        unsigned shown=0;
        for(uint32_t slot=0;slot<bank->list->count;slot++){
            const lp_tier0_record *record=lp_highway_at(highway,bank->list,slot);
            const char *label=lp_highway_identifier(highway,bank->list,slot);
            if(!record)continue;
            char id[33];api_hex_id(&record->id,id);
            api_buf_fmt(&out,&n,&cap,"%s{\"slot\":%u,\"id\":\"%s\",\"label\":\"",shown++?",":"",slot,id);
            api_cw_str(&out,&n,&cap,label?label:"");api_buf_fmt(&out,&n,&cap,"\"}");
        }
        api_buf_fmt(&out,&n,&cap,"]}");
    }
    api_buf_fmt(&out,&n,&cap,"],\"vertex_roles\":[");
    static const struct {unsigned value;const char *name;} roles[]={
        {0,"Constituent"},{LP_SAID_CLAIM,"Claim"},{LP_SAID_RECORD,"Record"},
        {LP_SAID_TUPLE,"Tuple"},{LP_SAID_METADATA,"Metadata"},{LP_SAID_HOLDS,"Content tree"},{LP_SAID_VOICE,"Voice"}
    };
    for(size_t i=0;i<sizeof roles/sizeof *roles;i++)api_buf_fmt(&out,&n,&cap,"%s{\"value\":%u,\"name\":\"%s\"}",i?",":"",roles[i].value,roles[i].name);
    api_buf_fmt(&out,&n,&cap,"],\"operations\":%s}",TOOLS);
    api_http(c,200,out);free(out);
}

static void mcp_rpc(int c, const char *body) {
    char method[64], id[64];
    if (!api_jstr(body, "method", method, sizeof method)) {
        api_http(c, 400, "{\"jsonrpc\":\"2.0\",\"id\":null,\"error\":{\"code\":-32600,\"message\":\"method\"}}");
        return;
    }
    rpc_id(body, id, sizeof id);
    if (!strcmp(method, "notifications/initialized") || !strncmp(method, "notifications/", 14)) {
        api_http(c, 202, "");
        return;
    }
    if (!strcmp(method, "initialize")) {
        char ver[40] = "2025-03-26";
        const char *params=api_jvalue(body,"params");if(params)api_jstr(params,"protocolVersion",ver,sizeof ver);
        char out[512];
        snprintf(out, sizeof out,
                 "{\"jsonrpc\":\"2.0\",\"id\":%s,\"result\":{\"protocolVersion\":\"%s\","
                 "\"capabilities\":{\"tools\":{}},\"serverInfo\":{\"name\":\"laplace\",\"version\":\"0.1\"}}}",
                 id, ver);
        api_http(c, 200, out);
        return;
    }
    if (!strcmp(method, "ping")) {
        char out[128];
        snprintf(out, sizeof out, "{\"jsonrpc\":\"2.0\",\"id\":%s,\"result\":{}}", id);
        api_http(c, 200, out);
        return;
    }
    if (!strcmp(method, "tools/list")) {
        char *out = malloc(strlen(TOOLS) + 80);
        snprintf(out, strlen(TOOLS) + 80, "{\"jsonrpc\":\"2.0\",\"id\":%s,\"result\":%s}", id, TOOLS);
        api_http(c, 200, out);
        free(out);
        return;
    }
    if (!strcmp(method, "tools/call")) {
        char name[32];
        const char *params=api_jvalue(body,"params");
        if (!params || !api_jstr(params, "name", name, sizeof name)) {
            char out[160];
            snprintf(out, sizeof out, "{\"jsonrpc\":\"2.0\",\"id\":%s,\"error\":{\"code\":-32602,\"message\":\"name\"}}", id);
            api_http(c, 200, out);
            return;
        }
        api_capture_begin(0);
        body=api_jvalue(params,"arguments");if(!body||*body!='{')body="{}";
        if (!strcmp(name, "code_search")) on_code_search(c, body);
        else if (!strcmp(name, "reindex")) on_reindex(c, body);
        else if (!strcmp(name, "code_where")) on_code_where(c, body);
        else if (!strcmp(name, "sources")) on_sources(c, body);
        else if (!strcmp(name, "catalog")) on_catalog(c, body);
        else if (!strcmp(name, "library")) on_library(c, body);
        else if (!strcmp(name, "provenance")) on_provenance(c, body);
        else if (!strcmp(name, "file_outline")) on_file_outline(c, body);
        else if (!strcmp(name, "code_callers")) on_code_callers(c, body);
        else if (!strcmp(name, "code_callees")) on_code_callees(c, body);
        else if (!strcmp(name, "ingest_status")) on_ingest_status(c, body);
        else if (!strcmp(name, "recipe_plan")) on_recipe_plan(c, body);
        else if (!strcmp(name, "recipe_catalog")) on_recipe_catalog(c, body);
        else if (!strcmp(name, "rerank")) on_rerank(c, body);
        else if (!strcmp(name, "search")) on_search(c, body);
        else if (!strcmp(name, "embeddings")) on_embeddings(c, body);
        else if (!strcmp(name, "shape")) on_shape(c, body);
        else if (!strcmp(name, "forward")) on_by_parts(c, body, API_FORWARD_SQL, "forward");
        else if (!strcmp(name, "ingest")) on_ingest(c, body);
        else if (!strcmp(name, "hop")) on_hop(c, body);
        else if (!strcmp(name, "text")) on_text(c, body);
        else if (!strcmp(name, "translate")) on_translate(c, body);
        else if (!strcmp(name, "degrees")) on_degrees(c, body);
        else if (!strcmp(name, "pull")) on_pull(c, body);
        else if (!strcmp(name, "turn")) on_turn(c, body);
        else {
            api_capture_end(0, NULL);
            char out[160];
            snprintf(out, sizeof out, "{\"jsonrpc\":\"2.0\",\"id\":%s,\"error\":{\"code\":-32602,\"message\":\"unknown tool\"}}", id);
            api_http(c, 200, out);
            return;
        }
        int cap_code = 500;
        const char *cap_body = api_capture_end(0, &cap_code);
        size_t elen = strlen(cap_body) * 6 + 64;
        char *esc = malloc(elen);
        api_json_esc(cap_body, esc, elen);
        int err = cap_code != 200;
        char *out = malloc(strlen(esc) + 160);
        snprintf(out, strlen(esc) + 160,
                 "{\"jsonrpc\":\"2.0\",\"id\":%s,\"result\":{\"content\":[{\"type\":\"text\",\"text\":\"%s\"}],\"isError\":%s}}",
                 id, esc, err ? "true" : "false");
        free(esc);
        api_http(c, 200, out);
        free(out);
        return;
    }
    char out[200];
    snprintf(out, sizeof out, "{\"jsonrpc\":\"2.0\",\"id\":%s,\"error\":{\"code\":-32601,\"message\":\"method not found\"}}", id);
    api_http(c, 200, out);
}

void api_dispatch(int c, const char *method, const char *path, const char *body) {
    char op[32] = "";
    if (!strcmp(path, "/mcp") && strstr(body, "\"jsonrpc\"")) {
        mcp_rpc(c, body);
        return;
    }
    if (!strcmp(path, "/mcp")) {
        if (!api_jstr(body, "op", op, sizeof op)) {
            api_http(c, 400, "{\"error\":\"op\"}");
            return;
        }
        if (!strcmp(op, "embeddings") || !strcmp(op, "embed")) path = "/v1/embeddings";
        else if (!strcmp(op, "rerank")) path = "/v1/rerank";
        else if (!strcmp(op, "code_search")) path = "/v1/code_search";
        else if (!strcmp(op, "reindex")) path = "/v1/reindex";
        else if (!strcmp(op, "code_where")) path = "/v1/code_where";
        else if (!strcmp(op, "sources")) path = "/v1/sources";
        else if (!strcmp(op, "catalog")) path = "/v1/catalog";
        else if (!strcmp(op, "library")) path = "/v1/library";
        else if (!strcmp(op, "provenance")) path = "/v1/provenance";
        else if (!strcmp(op, "file_outline")) path = "/v1/file_outline";
        else if (!strcmp(op, "code_callers")) path = "/v1/code_callers";
        else if (!strcmp(op, "code_callees")) path = "/v1/code_callees";
        else if (!strcmp(op, "ingest_status")) path = "/v1/ingest_status";
        else if (!strcmp(op, "recipe_plan")) path = "/v1/recipe_plan";
        else if (!strcmp(op, "recipe_catalog")) path = "/v1/recipe_catalog";
        else if (!strcmp(op, "search")) path = "/v1/search";
        else if (!strcmp(op, "forward")) path = "/v1/forward";
        else if (!strcmp(op, "shape") || !strcmp(op, "frechet")) path = "/v1/shape";
        else if (!strcmp(op, "ingest")) path = "/v1/ingest";
        else if (!strcmp(op, "hop")) path = "/v1/hop";
        else if (!strcmp(op, "text")) path = "/v1/text";
        else if (!strcmp(op, "translate")) path = "/v1/translate";
        else if (!strcmp(op, "degrees")) path = "/v1/degrees";
        else if (!strcmp(op, "pull")) path = "/v1/pull";
        else if (!strcmp(op, "turn")) path = "/v1/turn";
        else if (!strcmp(op, "surface")) path = "/v1/surface";
        else {
            api_http(c, 400, "{\"error\":\"op\"}");
            return;
        }
        method = !strcmp(path, "/v1/surface") ? "GET" : "POST";
    }
    if (!strcmp(method, "GET") && !strcmp(path, "/health")) {
        api_http(c, 200, "{\"ok\":true}");
        return;
    }
    if (!strcmp(method, "GET") && !strcmp(path, "/v1/surface")) {
        api_http(c, 200, SURFACE);
        return;
    }
    if (!strcmp(method, "GET") && !strcmp(path, "/v1/schema")) {
        on_schema(c);
        return;
    }
    if (strcmp(method, "POST")) {
        api_http(c, 404, "{\"error\":\"not found\"}");
        return;
    }
    if (!strcmp(path, "/v1/embeddings") || !strcmp(path, "/embeddings")) on_embeddings(c, body);
    else if (!strcmp(path, "/v1/rerank") || !strcmp(path, "/rerank")) on_rerank(c, body);
    else if (!strcmp(path, "/v1/code_search")) on_code_search(c, body);
    else if (!strcmp(path, "/v1/reindex")) on_reindex(c, body);
    else if (!strcmp(path, "/v1/code_where")) on_code_where(c, body);
    else if (!strcmp(path, "/v1/sources")) on_sources(c, body);
    else if (!strcmp(path, "/v1/catalog")) on_catalog(c, body);
    else if (!strcmp(path, "/v1/library")) on_library(c, body);
    else if (!strcmp(path, "/v1/provenance")) on_provenance(c, body);
    else if (!strcmp(path, "/v1/file_outline")) on_file_outline(c, body);
    else if (!strcmp(path, "/v1/code_callers")) on_code_callers(c, body);
    else if (!strcmp(path, "/v1/code_callees")) on_code_callees(c, body);
    else if (!strcmp(path, "/v1/ingest_status")) on_ingest_status(c, body);
    else if (!strcmp(path, "/v1/recipe_plan")) on_recipe_plan(c, body);
    else if (!strcmp(path, "/v1/recipe_catalog")) on_recipe_catalog(c, body);
    else if (!strcmp(path, "/v1/search")) on_search(c, body);
    else if (!strcmp(path, "/v1/forward")) on_by_parts(c, body, API_FORWARD_SQL, "forward");
    else if (!strcmp(path, "/v1/shape")) on_shape(c, body);
    else if (!strcmp(path, "/v1/ingest")) on_ingest(c, body);
    else if (!strcmp(path, "/v1/hop")) on_hop(c, body);
    else if (!strcmp(path, "/v1/text")) on_text(c, body);
    else if (!strcmp(path, "/v1/translate")) on_translate(c, body);
    else if (!strcmp(path, "/v1/degrees")) on_degrees(c, body);
    else if (!strcmp(path, "/v1/pull")) on_pull(c, body);
    else if (!strcmp(path, "/v1/turn")) on_turn(c, body);
    else api_http(c, 404, "{\"error\":\"not found\"}");
}

int api_mcp_stdio(void) {
#ifdef _WIN32
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
#endif
    api_engine_resolve();
    T0 = lp_tier0_map(NULL);
    if (!T0) {
        fprintf(stderr, "tier 0 did not map\n");
        return 1;
    }
    TX = lp_text_new(T0);
    if (!TX) return 1;
    size_t cap = 1 << 20;
    char *line = malloc(cap);
    if (!line) return 1;
    while (fgets(line, (int)cap, stdin)) {
        size_t len = strlen(line);
        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) line[--len] = 0;
        if (!len) continue;
        api_capture_begin(1);
        mcp_rpc(-1, line);
        int scode = 500;
        const char *sbody = api_capture_end(1, &scode);
        if (scode != 202 && sbody && sbody[0]) {
            fputs(sbody, stdout);
            fputc('\n', stdout);
            fflush(stdout);
        }
    }
    free(line);
    return 0;
}

