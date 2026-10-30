using System.Text.Json;
using Microsoft.AspNetCore.StaticFiles;

var builder = WebApplication.CreateBuilder(args);
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
    await next(context);
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
        dataMode = "captured-readback",
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
    data = "captured-readback"
}));

var contentTypes = new FileExtensionContentTypeProvider();
contentTypes.Mappings[".md"] = "text/markdown";
app.UseDefaultFiles();
app.UseStaticFiles(new StaticFileOptions { ContentTypeProvider = contentTypes });
app.Run();
