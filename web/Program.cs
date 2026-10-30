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
app.MapGet("/health", () => Results.Json(new
{
    status = "ok",
    service = "laplace-modular-web",
    release = builder.Configuration["ReleaseId"] ?? "source",
    identity = "shared-same-origin",
    data = "native-live"
}));

static bool IsId(string id) => id.Length == 32 && id.All(Uri.IsHexDigit);
app.MapGet("/api/lookup", async (string q, NativeRuntime native, CancellationToken ct) =>
{
    if (string.IsNullOrWhiteSpace(q) || System.Text.Encoding.UTF8.GetByteCount(q) > 4096) return Results.BadRequest(new { error = "Enter content or a canonical ID, up to 4096 UTF-8 bytes." });
    object input = IsId(q.Trim()) ? new { id = q.Trim().ToLowerInvariant(), fan = 24, display = true } : new { input = q, fan = 24, display = true };
    return Results.Json(await native.Read("/v1/search", input, ct));
});
app.MapGet("/api/resources/{id}", async (string id, NativeRuntime native, CancellationToken ct) => IsId(id)
    ? Results.Json(await native.Read("/v1/search", new { id = id.ToLowerInvariant(), fan = 24, display = true }, ct)) : Results.BadRequest());
app.MapGet("/api/provenance/{id}", async (string id, NativeRuntime native, CancellationToken ct) => IsId(id)
    ? Results.Json(await native.Read("/v1/provenance", new { id = id.ToLowerInvariant() }, ct)) : Results.BadRequest());
app.MapGet("/api/catalog", async (NativeRuntime native, CancellationToken ct) => Results.Json(await native.Read("/v1/catalog", new { }, ct)));
app.MapGet("/api/library", async (string? after, NativeRuntime native, CancellationToken ct) => string.IsNullOrEmpty(after) || IsId(after)
    ? Results.Json(await native.Read("/v1/library", new { after = after ?? "" }, ct)) : Results.BadRequest());
app.MapGet("/api/sources", async (NativeRuntime native, CancellationToken ct) => Results.Json(await native.Read("/v1/sources", new { }, ct)));
app.MapGet("/api/surface", async (NativeRuntime native, CancellationToken ct) => Results.Json(await native.Read("/v1/surface", null, ct)));
// Explicit read operations only. No shell, arbitrary path, ingestion or write proxy.
app.MapPost("/api/operations/{operation}", async (string operation, JsonElement body, NativeRuntime native, CancellationToken ct) =>
{
    if (operation is not ("forward" or "shape" or "embeddings")) return Results.NotFound();
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
