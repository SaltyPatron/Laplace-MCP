using System.Text.Json;
using Microsoft.AspNetCore.StaticFiles;

var builder = WebApplication.CreateBuilder(args);
builder.Services.AddSingleton<NativeRuntime>();
var app = builder.Build();
var identityPath = builder.Configuration["SharedIdentityPath"] ?? "/v1/auth";
if (!identityPath.StartsWith('/') || identityPath.StartsWith("//") || identityPath.Contains('\\')
    || identityPath.Contains('?') || identityPath.Contains('#'))
    throw new InvalidOperationException("SharedIdentityPath must be a local absolute path.");

app.Use(async (context, next) =>
{
    context.Response.Headers.XContentTypeOptions = "nosniff";
    context.Response.Headers["Referrer-Policy"] = "strict-origin-when-cross-origin";
    context.Response.Headers.ContentSecurityPolicy = "default-src 'self'; script-src 'self'; style-src 'self' 'unsafe-inline'; img-src 'self' data:; connect-src 'self'; object-src 'none'; base-uri 'self'; frame-ancestors 'none'; form-action 'self'";
    context.Response.Headers.CacheControl = "no-cache";
    try { await next(context); }
    catch (Exception ex) when (ex is NativeUnavailableException or TaskCanceledException or IOException)
    {
        if (context.RequestAborted.IsCancellationRequested) return;
        await Results.Problem(statusCode: 503, title: "Native read unavailable", detail: ex is NativeUnavailableException ? ex.Message : "The reader did not complete the request. Retry the read.").ExecuteAsync(context);
    }
});

app.MapGet("/runtime.js", (HttpContext context) =>
{
    context.Response.Headers.CacheControl = "no-store";
    var settings = new
    {
        hosting = "iis",
        identityMode = "same-origin",
        identityPath,
        basePath = context.Request.PathBase.Value ?? "",
        dataMode = "native-live",
        build = builder.Configuration["ReleaseId"] ?? "source"
    };
    return Results.Text("window.LAPLACE_RUNTIME = " + JsonSerializer.Serialize(settings) + ";\n", "text/javascript");
});
app.MapGet("/health", (NativeRuntime native) => Results.Json(new
{
    status = "ok",
    service = "laplace-modular-web",
    release = builder.Configuration["ReleaseId"] ?? "source",
    identity = "shared-same-origin",
    data = "native-live",
    reader = native.ReaderEvidence()
}));

app.MapMethods("/mcp", ["POST", "GET", "DELETE"], async (HttpContext context, NativeRuntime native) =>
{
    var identityEndpoint = new Uri(builder.Configuration["McpIdentityEndpoint"] ?? throw new NativeUnavailableException("MCP identity is not configured."));
    if (context.Request.Headers.TryGetValue("Origin", out var origins)
        && (origins.Count != 1 || origins[0] != identityEndpoint.GetLeftPart(UriPartial.Authority)))
    { context.Response.StatusCode = 403; return; }
    if (context.Request.Headers.Authorization.Count != 1
        || !await native.AuthorizeMcp(context.Request.Headers.Authorization.ToString(), context.RequestAborted))
    { context.Response.Headers.WWWAuthenticate = "Bearer realm=\"laplace-native-mcp\""; context.Response.StatusCode = 401; return; }
    if (context.Request.Method == "DELETE") { context.Response.StatusCode = 204; return; }
    if (context.Request.Method != "POST") { context.Response.Headers.Allow = "POST, DELETE"; context.Response.StatusCode = 405; return; }
    if (!context.Request.HasJsonContentType()) { context.Response.StatusCode = 415; return; }
    using var body = new MemoryStream();
    var buffer = new byte[8192];
    int count;
    while ((count = await context.Request.Body.ReadAsync(buffer, context.RequestAborted)) != 0)
    {
        if (body.Length + count > 1024 * 1024) { context.Response.StatusCode = 413; return; }
        body.Write(buffer, 0, count);
    }
    JsonDocument message;
    try { message = JsonDocument.Parse(body.ToArray()); }
    catch (JsonException) { context.Response.StatusCode = 400; return; }
    using (message)
    {
        var value = message.RootElement;
        if (value.ValueKind != JsonValueKind.Object || !value.TryGetProperty("jsonrpc", out var version)
            || version.GetString() != "2.0" || !value.TryGetProperty("method", out var method) || method.ValueKind != JsonValueKind.String)
        { context.Response.StatusCode = 400; return; }
        if (!value.TryGetProperty("id", out _)) { context.Response.StatusCode = 202; return; }
        await Results.Json(await native.Read("/mcp", value, context.RequestAborted)).ExecuteAsync(context);
    }
});

static bool IsId(string id) => id.Length == 32 && id.All(Uri.IsHexDigit);
static bool IsReadOperation(string? name) => name is "forward" or "shape" or "embeddings";
app.MapGet("/api/lookup", async (string q, NativeRuntime native, CancellationToken ct) =>
{
    if (string.IsNullOrWhiteSpace(q) || System.Text.Encoding.UTF8.GetByteCount(q) > 4096) return Results.BadRequest(new { error = "Enter content or a canonical ID, up to 4096 UTF-8 bytes." });
    object input = IsId(q.Trim()) ? new { id = q.Trim().ToLowerInvariant(), fan = 24, display = true } : new { input = q, fan = 24, display = true };
    return Results.Json(await native.Read("/v1/search", input, ct));
});
app.MapGet("/api/resources/{id}", async (string id, NativeRuntime native, CancellationToken ct) => IsId(id)
    ? Results.Json(await native.Read("/v1/search", new { id = id.ToLowerInvariant(), fan = 24, display = true }, ct)) : Results.BadRequest());
app.MapGet("/api/continuations", async (string q, NativeRuntime native, CancellationToken ct) =>
{
    if (string.IsNullOrWhiteSpace(q) || System.Text.Encoding.UTF8.GetByteCount(q) > 4096)
        return Results.BadRequest(new { error = "Enter content, up to 4096 UTF-8 bytes." });
    // Forward reads observe spans and continuations. Canonical-ID queries use
    // the structural read rather than decomposing a hexadecimal display value.
    if (IsId(q.Trim())) return Results.Json(new { forward = Array.Empty<object>(), scope = "not applied to canonical IDs" });
    return Results.Json(await native.Read("/v1/forward", new { input = q }, ct));
});
app.MapGet("/api/provenance/{id}", async (string id, NativeRuntime native, CancellationToken ct) => IsId(id)
    ? Results.Json(await native.Read("/v1/provenance", new { id = id.ToLowerInvariant() }, ct)) : Results.BadRequest());
app.MapGet("/api/files/{id}", async (string id, NativeRuntime native, CancellationToken ct) => IsId(id)
    ? Results.Json(await native.Read("/v1/file_outline", new { file = id.ToLowerInvariant(), include_text = true }, ct)) : Results.BadRequest());
app.MapGet("/api/catalog", async (NativeRuntime native, CancellationToken ct) => Results.Json(await native.Read("/v1/catalog", new { }, ct)));
app.MapGet("/api/library", async (string? after, NativeRuntime native, CancellationToken ct) => string.IsNullOrEmpty(after) || IsId(after)
    ? Results.Json(await native.Read("/v1/library", new { after = after ?? "" }, ct)) : Results.BadRequest());
app.MapGet("/api/sources", async (NativeRuntime native, CancellationToken ct) => Results.Json(await native.Read("/v1/sources", new { }, ct)));
app.MapGet("/api/surface", async (NativeRuntime native, CancellationToken ct) => Results.Json(await native.Read("/v1/surface", null, ct)));
app.MapGet("/api/schema", async (NativeRuntime native, CancellationToken ct) =>
{
    var schema = await native.Read("/v1/schema", null, ct);
    // Discovery describes the native model; execution remains an explicit
    // server permission. Reuse the native MCP JSON Schemas for permitted forms.
    var operations = schema.GetProperty("operations").GetProperty("tools").EnumerateArray()
        .Where(tool => IsReadOperation(tool.GetProperty("name").GetString()))
        .Select(tool => new
        {
            name = tool.GetProperty("name").GetString(),
            description = tool.GetProperty("description"),
            inputSchema = new
            {
                type = "object",
                properties = tool.GetProperty("inputSchema").GetProperty("properties").EnumerateObject()
                    .Where(field => field.Name is "input" or "other")
                    .ToDictionary(field => field.Name, field => field.Value.Clone()),
                required = tool.GetProperty("name").GetString() == "shape" ? new[] { "input", "other" } : new[] { "input" }
            }
        }).ToArray();
    return Results.Json(new
    {
        schema = schema.GetProperty("schema"),
        highway = schema.GetProperty("highway"),
        identity = schema.GetProperty("identity"),
        views = schema.GetProperty("views"),
        banks = schema.GetProperty("banks"),
        vertex_roles = schema.GetProperty("vertex_roles"),
        operations
    });
});
// Chess reads. Position IDs are the Engine's (chess_rules.c, linked into the native reader); this host only forwards.
// A native {error} is the request's fault: a FEN that is no position, a game that does not replay.
static IResult NativeAnswer(JsonElement answer) =>
    answer.ValueKind == JsonValueKind.Object && answer.TryGetProperty("error", out _) ? Results.BadRequest(answer) : Results.Json(answer);
app.MapGet("/api/chess/position", async (string? fen, NativeRuntime native, CancellationToken ct) =>
{
    if (string.IsNullOrWhiteSpace(fen) || fen.Length > 128) return Results.BadRequest(new { error = "fen: a FEN, up to 128 characters" });
    return NativeAnswer(await native.Read("/v1/chess_position", new { fen = fen.Trim() }, ct));
});
app.MapPost("/api/chess/game-openings", async (JsonElement body, NativeRuntime native, CancellationToken ct) =>
{
    if (body.ValueKind != JsonValueKind.Object) return Results.BadRequest(new { error = "a JSON object: { pgn } or { line } or { pgns: [...] }" });
    if (body.TryGetProperty("pgns", out var pgns))
    {
        if (pgns.ValueKind != JsonValueKind.Array || pgns.GetArrayLength() > 512 || pgns.EnumerateArray().Any(p => p.ValueKind != JsonValueKind.String || p.GetString()!.Length > 65536))
            return Results.BadRequest(new { error = "pgns: up to 512 PGN strings" });
        return NativeAnswer(await native.Read("/v1/chess_game_openings", new { pgns = pgns.EnumerateArray().Select(p => p.GetString()).ToArray() }, ct));
    }
    if (body.TryGetProperty("pgn", out var pgn))
    {
        if (pgn.ValueKind != JsonValueKind.String || string.IsNullOrWhiteSpace(pgn.GetString()) || pgn.GetString()!.Length > 65536) return Results.BadRequest(new { error = "pgn: a PGN string" });
        return NativeAnswer(await native.Read("/v1/chess_game_openings", new { pgn = pgn.GetString() }, ct));
    }
    if (body.TryGetProperty("line", out var line) && line.ValueKind == JsonValueKind.String && IsId(line.GetString()!))
        return NativeAnswer(await native.Read("/v1/chess_game_openings", new { line = line.GetString()!.ToLowerInvariant() }, ct));
    return Results.BadRequest(new { error = "{ pgn } or { line } or { pgns: [...] }" });
});
// Explicit read operations only. No shell, arbitrary path, ingestion or write proxy.
app.MapPost("/api/operations/{operation}", async (string operation, JsonElement body, NativeRuntime native, CancellationToken ct) =>
{
    if (!IsReadOperation(operation)) return Results.NotFound();
    if (!body.TryGetProperty("input", out var value) || value.ValueKind != JsonValueKind.String || string.IsNullOrWhiteSpace(value.GetString()) || System.Text.Encoding.UTF8.GetByteCount(value.GetString()!) > 4096) return Results.BadRequest();
    if (operation == "shape")
    {
        if (!body.TryGetProperty("other", out var other) || other.ValueKind != JsonValueKind.String || string.IsNullOrWhiteSpace(other.GetString()) || System.Text.Encoding.UTF8.GetByteCount(other.GetString()!) > 4096) return Results.BadRequest();
        return Results.Json(await native.Read("/v1/shape", new { input = value.GetString(), other = other.GetString() }, ct));
    }
    return Results.Json(await native.Read("/v1/" + operation, new { input = value.GetString() }, ct));
});

var contentTypes = new FileExtensionContentTypeProvider();
contentTypes.Mappings[".md"] = "text/markdown";
app.UseDefaultFiles();
app.UseStaticFiles(new StaticFileOptions { ContentTypeProvider = contentTypes });
app.Run();
