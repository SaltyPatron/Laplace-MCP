using System.Diagnostics;
using System.Net;
using System.Net.Http.Headers;
using System.Net.Sockets;
using System.Runtime.InteropServices;
using System.Security.Cryptography;
using System.Text;
using System.Text.Encodings.Web;
using System.Text.Json;
using Microsoft.Win32.SafeHandles;

// This host owns transport and process lifetime. Identity, structure, provenance,
// rendering and database queries remain in the native reader.
sealed class NativeRuntime(IConfiguration configuration, ILogger<NativeRuntime> logger) : IDisposable
{
    readonly SemaphoreSlim gate = new(1, 1);
    readonly HttpClient client = new() { Timeout = Timeout.InfiniteTimeSpan };
    readonly HttpClient identity = new(new HttpClientHandler { AllowAutoRedirect = false }) { Timeout = TimeSpan.FromSeconds(15) };
    static readonly JsonSerializerOptions Json = new() { Encoder = JavaScriptEncoder.UnsafeRelaxedJsonEscaping };
    Process? process;
    SafeFileHandle? job;
    Uri? origin;
    string token = "";
    string imageHash = "";
    bool disposed;

    public object? ReaderEvidence()
    {
        try
        {
            return process is { HasExited: false } reader ? new
            {
                pid = reader.Id,
                parent_pid = Environment.ProcessId,
                started_utc = reader.StartTime.ToUniversalTime().ToString("o"),
                executable_sha256 = imageHash
            } : null;
        }
        catch (InvalidOperationException) { return null; }
    }

    public async Task<bool> AuthorizeMcp(string authorization, CancellationToken cancellationToken)
    {
        if (!AuthenticationHeaderValue.TryParse(authorization, out var bearer)
            || !string.Equals(bearer.Scheme, "Bearer", StringComparison.OrdinalIgnoreCase)
            || string.IsNullOrWhiteSpace(bearer.Parameter)) return false;
        if (!Uri.TryCreate(configuration["McpIdentityEndpoint"], UriKind.Absolute, out var endpoint)
            || endpoint.Scheme != Uri.UriSchemeHttps)
            throw new NativeUnavailableException("The MCP identity endpoint is not configured.");
        using var request = new HttpRequestMessage(HttpMethod.Post, endpoint);
        request.Headers.Authorization = bearer;
        request.Headers.Accept.ParseAdd("application/json, text/event-stream");
        request.Content = new StringContent(JsonSerializer.Serialize(new
        {
            jsonrpc = "2.0", id = 1, method = "initialize",
            @params = new { protocolVersion = "2025-06-18", capabilities = new { }, clientInfo = new { name = "laplace-native-iis", version = "1" } }
        }), Encoding.UTF8, "application/json");
        using var response = await identity.SendAsync(request, cancellationToken);
        if (response.StatusCode is HttpStatusCode.Unauthorized or HttpStatusCode.Forbidden) return false;
        if (!response.IsSuccessStatusCode || !response.Headers.TryGetValues("Mcp-Session-Id", out var sessions))
            throw new NativeUnavailableException("The MCP identity service did not verify this request.");
        var session = sessions.SingleOrDefault();
        if (string.IsNullOrWhiteSpace(session)) throw new NativeUnavailableException("The MCP identity service did not issue a verification session.");
        try
        {
            using var document = JsonDocument.Parse(await response.Content.ReadAsStringAsync(cancellationToken));
            return document.RootElement.TryGetProperty("result", out var result)
                && result.TryGetProperty("serverInfo", out var server)
                && server.GetProperty("name").GetString() == "laplace-substrate";
        }
        finally
        {
            using var close = new HttpRequestMessage(HttpMethod.Delete, endpoint);
            close.Headers.Authorization = bearer;
            close.Headers.Add("Mcp-Session-Id", session);
            using var closed = await identity.SendAsync(close, cancellationToken);
            if (closed.StatusCode != HttpStatusCode.NoContent)
                throw new NativeUnavailableException("The MCP identity verification session could not be closed.");
        }
    }

    public async Task<JsonElement> Read(string path, object? body, CancellationToken cancellationToken)
    {
        using var budget = CancellationTokenSource.CreateLinkedTokenSource(cancellationToken);
        budget.CancelAfter(TimeSpan.FromSeconds(path == "/mcp" ? 300 : 45));
        cancellationToken = budget.Token;
        await gate.WaitAsync(cancellationToken);
        try
        {
            ObjectDisposedException.ThrowIf(disposed, this);
            await EnsureStarted(cancellationToken);
            using var request = new HttpRequestMessage(body is null ? HttpMethod.Get : HttpMethod.Post, new Uri(origin!, path));
            request.Headers.Authorization = new AuthenticationHeaderValue("Bearer", token);
            if (body is not null) request.Content = new StringContent(JsonSerializer.Serialize(body, Json), Encoding.UTF8, "application/json");
            using var response = await client.SendAsync(request, cancellationToken);
            var data = await response.Content.ReadAsStringAsync(cancellationToken);
            if (!response.IsSuccessStatusCode)
            {
                // Do not relay database errors or filesystem paths into the public browser.
                logger.LogWarning("Native read {Path} returned HTTP {Status}", path, (int)response.StatusCode);
                throw new NativeUnavailableException("The native reader could not complete this read. Retry the request.");
            }
            using var document = JsonDocument.Parse(data);
            return document.RootElement.Clone();
        }
        catch (HttpRequestException ex)
        {
            logger.LogWarning("Native reader transport failed: {Type}", ex.GetType().Name);
            Stop();
            throw new NativeUnavailableException("The native reader is reconnecting. Retry the request.");
        }
        catch (System.ComponentModel.Win32Exception ex)
        {
            logger.LogWarning("Native process startup failed with Windows error {NativeErrorCode}", ex.NativeErrorCode);
            Stop();
            throw new NativeUnavailableException("The installed native reader could not be started. Retry the request.");
        }
        finally { gate.Release(); }
    }

    async Task EnsureStarted(CancellationToken cancellationToken)
    {
        if (process is { HasExited: false }) return;
        Stop();
        var configuredFile = configuration["NativeConfigFile"] ?? throw new NativeUnavailableException("Native reader installation is not configured.");
        var file = Path.GetFullPath(configuredFile, AppContext.BaseDirectory);
        using var document = JsonDocument.Parse(await File.ReadAllTextAsync(file, cancellationToken));
        var config = document.RootElement;
        string Value(string name) => config.GetProperty(name).GetString() ?? throw new InvalidOperationException($"Missing native setting: {name}");
        var release = Value("release");
        var bin = Path.Combine(release, "bin");
        var prepared = Path.Combine(release, "share", "prepared");
        var executable = Path.Combine(AppContext.BaseDirectory, "native", "lpm.exe");
        var highway = Path.Combine(AppContext.BaseDirectory, "native", "tier0.highway");
        if (!Directory.Exists(bin) || !File.Exists(executable) || !File.Exists(highway) || !File.Exists(Path.Combine(prepared, "tier0.bin")))
            throw new NativeUnavailableException("The installed native reader or perf-cache is unavailable.");
        using var listener = new TcpListener(IPAddress.Loopback, 0);
        listener.Start();
        var port = ((IPEndPoint)listener.LocalEndpoint).Port;
        listener.Stop();
        token = Convert.ToHexString(RandomNumberGenerator.GetBytes(32));
        var start = new ProcessStartInfo(executable) { UseShellExecute = false, CreateNoWindow = true, WorkingDirectory = bin, RedirectStandardError = true, RedirectStandardOutput = true };
        start.ArgumentList.Add("serve"); start.ArgumentList.Add("127.0.0.1"); start.ArgumentList.Add(port.ToString(System.Globalization.CultureInfo.InvariantCulture));
        static string Quote(string value) => "'" + value.Replace("\\", "\\\\").Replace("'", "\\'") + "'";
        start.Environment["LAPLACE_CONNINFO"] = $"host={Quote(Value("host"))} hostaddr=127.0.0.1 dbname={Quote(Value("database"))} user={Quote(Value("role"))} sslmode=verify-full sslrootcert={Quote(Value("certificate"))} require_auth=sspi connect_timeout=5 passfile={Quote(Path.Combine(Path.GetDirectoryName(file)!, "no-password-file"))} options='-c default_transaction_read_only=on -c statement_timeout=20000'";
        start.Environment["LAPLACE_MCP_TOKEN"] = token;
        start.Environment["LAPLACE_TIER0"] = Path.Combine(prepared, "tier0.bin");
        start.Environment["LAPLACE_FLAGS"] = Path.Combine(prepared, "tier0.flags");
        start.Environment["LAPLACE_HIGHWAY"] = highway;
        start.Environment["LAPLACE_MANIFEST"] = Path.Combine(release, "share", "manifest");
        start.Environment["LAPLACE_RECIPES"] = Path.Combine(release, "share", "recipes");
        start.Environment["LAPLACE_GRAMMARS"] = Path.Combine(release, "share", "grammars");
        start.Environment["LAPLACE_FIRMWARE"] = Path.Combine(release, "share", "firmware", "program.firmware");
        start.Environment["LAPLACE_ENGINE"] = Path.Combine(bin, "laplace.exe");
        // Source declarations root at $LAPLACE_DATA, $LAPLACE_SRC and $LAPLACE_MODELS; the endgame tables the tablebase read
        // probes are under $LAPLACE_SYZYGY, else $LAPLACE_DATA/Games/Chess/syzygy (Laplace-Engine chess_tablebase.c).
        foreach (var (property, variable) in new[] { ("data", "LAPLACE_DATA"), ("src", "LAPLACE_SRC"), ("models", "LAPLACE_MODELS"), ("syzygy", "LAPLACE_SYZYGY") })
            if (config.TryGetProperty(property, out var located) && located.GetString() is { Length: > 0 } path)
                start.Environment[variable] = path;
        start.Environment["LAPLACE_CODE_ROOTS"] = config.TryGetProperty("code_roots", out var roots)
            ? string.Join(Path.PathSeparator, roots.EnumerateArray().Select(root => root.GetString())) : "";
        if (config.TryGetProperty("work", out var work))
        {
            var directory = work.GetString() ?? throw new NativeUnavailableException("Native work storage is not configured.");
            Directory.CreateDirectory(directory);
            start.Environment["LAPLACE_WORK"] = directory;
            start.Environment["LAPLACE_CODE_RECORDS"] = Path.Combine(directory, "workspace.lpm");
        }
        var threads = config.TryGetProperty("threads", out var selectedThreads) ? Math.Clamp(selectedThreads.GetInt32(), 1, 8) : 2;
        start.Environment["OMP_NUM_THREADS"] = threads.ToString(System.Globalization.CultureInfo.InvariantCulture);
        start.Environment["MKL_NUM_THREADS"] = "1";
        start.Environment["PATH"] = bin + Path.PathSeparator + Environment.GetEnvironmentVariable("PATH");
        process = Process.Start(start) ?? throw new NativeUnavailableException("The native reader did not start.");
        imageHash = Convert.ToHexStringLower(SHA256.HashData(await File.ReadAllBytesAsync(executable, cancellationToken)));
        // IIS recycling and unexpected host termination must not orphan readers.
        job = CreateJobObject(IntPtr.Zero, null);
        var limits = new JobLimits { Basic = new BasicLimits { LimitFlags = 0x2000 } }; // KILL_ON_JOB_CLOSE
        if (job.IsInvalid || !SetInformationJobObject(job, 9, ref limits, (uint)Marshal.SizeOf<JobLimits>()) || !AssignProcessToJobObject(job, process.Handle))
        {
            Stop(); throw new NativeUnavailableException("The native reader lifetime could not be attached to IIS.");
        }
        process.ErrorDataReceived += (_, e) => { if (e.Data is not null) logger.LogDebug("Native reader: {Message}", e.Data); };
        process.BeginErrorReadLine(); process.BeginOutputReadLine();
        origin = new Uri($"http://127.0.0.1:{port}");
        for (var attempt = 0; attempt < 100; attempt++)
        {
            if (process.HasExited) { Stop(); throw new NativeUnavailableException("The native reader exited during startup."); }
            try
            {
                using var probe = new HttpRequestMessage(HttpMethod.Get, new Uri(origin, "/health"));
                probe.Headers.Authorization = new AuthenticationHeaderValue("Bearer", token);
                using var response = await client.SendAsync(probe, cancellationToken);
                if (response.IsSuccessStatusCode) return;
            }
            catch (HttpRequestException) { }
            await Task.Delay(100, cancellationToken);
        }
        Stop(); throw new NativeUnavailableException("The native reader did not become ready.");
    }

    void Stop()
    {
        job?.Dispose(); job = null;
        if (process is not null) { try { if (!process.HasExited) process.Kill(true); } catch (InvalidOperationException) { } process.Dispose(); process = null; }
    }
    public void Dispose() { disposed = true; Stop(); client.Dispose(); identity.Dispose(); gate.Dispose(); }
    [StructLayout(LayoutKind.Sequential)] struct BasicLimits { public long ProcessTime, JobTime; public uint LimitFlags; public UIntPtr MinimumWorkingSet, MaximumWorkingSet; public uint ActiveProcessLimit; public UIntPtr Affinity; public uint PriorityClass, SchedulingClass; }
    [StructLayout(LayoutKind.Sequential)] struct IoCounters { public ulong ReadOperations, WriteOperations, OtherOperations, ReadBytes, WriteBytes, OtherBytes; }
    [StructLayout(LayoutKind.Sequential)] struct JobLimits { public BasicLimits Basic; public IoCounters Io; public UIntPtr ProcessMemory, JobMemory, PeakProcessMemory, PeakJobMemory; }
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode)] static extern SafeFileHandle CreateJobObject(IntPtr security, string? name);
    [DllImport("kernel32.dll")] [return: MarshalAs(UnmanagedType.Bool)] static extern bool SetInformationJobObject(SafeFileHandle job, int infoClass, ref JobLimits information, uint length);
    [DllImport("kernel32.dll")] [return: MarshalAs(UnmanagedType.Bool)] static extern bool AssignProcessToJobObject(SafeFileHandle job, IntPtr process);
}
sealed class NativeUnavailableException(string message) : Exception(message);
