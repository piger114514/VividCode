using System.Runtime.InteropServices;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;
using System.Text.Json.Nodes;

namespace VividCode.CSharp;

internal static class Store
{
    public const int Schema = 1;
    public const string Version = "1.0.0";
    public static string PathKey(string path) => Path.GetFullPath(path).Replace('\\', '/').TrimEnd('/').ToLowerInvariant();
    public static string Hash(string value) => Convert.ToHexString(SHA256.HashData(Encoding.UTF8.GetBytes(value))).ToLowerInvariant();
    public static string FileHash(string path) { using var f = File.OpenRead(path); return Convert.ToHexString(SHA256.HashData(f)).ToLowerInvariant(); }
    public static void Emit(object value) => Console.WriteLine(JsonSerializer.Serialize(value));
    public static JsonObject Read(string path)
    {
        Plain(path);
        return JsonNode.Parse(File.ReadAllText(path, Encoding.UTF8).TrimStart('\uFEFF'))?.AsObject() ?? throw new InvalidDataException("Invalid JSON: " + path);
    }
    public static void Plain(string path)
    {
        for (var part = Path.GetFullPath(path); !string.IsNullOrEmpty(part); part = Path.GetDirectoryName(part))
            if ((File.Exists(part) || Directory.Exists(part)) && (File.GetAttributes(part) & FileAttributes.ReparsePoint) != 0)
                throw new IOException("Link or junction rejected: " + part);
    }
    public static string Layout(string root)
    {
        Plain(root);
        var manifest = Read(Path.Combine(root, ".vividcode", "project.json"));
        if (manifest["app"]?.GetValue<string>() != "vividcode" || manifest["schema"]?.GetValue<int>() != 1)
            throw new InvalidDataException("Run vividcode -g from the project root first.");
        var home = Environment.GetEnvironmentVariable("VIVIDCODE_HOME") ?? Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "vividcode");
        if (!Path.IsPathFullyQualified(home)) throw new IOException("Shared directory must be an absolute path.");
        var sem = Path.Combine(home, "semantic", Hash(PathKey(root)));
        Plain(sem);
        if (Directory.Exists(sem))
        {
            var owner = Read(Path.Combine(sem, "project.json"));
            if (owner["app"]?.GetValue<string>() != "vividcode" || owner["schema"]?.GetValue<int>() != 1 || PathKey(owner["projectRoot"]!.GetValue<string>()) != PathKey(root))
                throw new IOException("Shared index ownership mismatch.");
        }
        return Path.Combine(sem, "csharp");
    }
    public static FileStream Lock(string root)
    {
        var path = Path.Combine(root, ".vividcode-semantic.lock"); Plain(path);
        return new FileStream(path, FileMode.OpenOrCreate, FileAccess.ReadWrite, FileShare.None, 4096, FileOptions.DeleteOnClose);
    }
    public static void Atomic(string path, JsonObject value)
    {
        Plain(path); Plain(path + ".tmp");
        File.WriteAllText(path + ".tmp", value.ToJsonString(), new UTF8Encoding(false));
        File.Move(path + ".tmp", path, true);
    }
    public static JsonObject Export(string cache)
    {
        var state = File.Exists(Path.Combine(cache, "config.json")) ? Read(Path.Combine(cache, "config.json")) : new JsonObject();
        var snapshot = File.Exists(Path.Combine(cache, "index.json")) ? Read(Path.Combine(cache, "index.json")) : new JsonObject { ["schema"] = Schema, ["units"] = new JsonArray() };
        if (snapshot["schema"]?.GetValue<int>() != Schema) throw new InvalidDataException("Incompatible C# index version.");
        snapshot["ok"] = true; snapshot["language"] = "c#"; snapshot["state"] = state;
        return snapshot;
    }
}

// The job handle is closed by Windows even when Qt kills the analyzer. Its MSBuild
// BuildHost children then exit too, so cancellation does not leave background workers.
internal sealed class WorkerJob : IDisposable
{
    private IntPtr handle;
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode)] static extern IntPtr CreateJobObject(IntPtr attributes, string? name);
    [DllImport("kernel32.dll")] static extern bool SetInformationJobObject(IntPtr job, int infoClass, IntPtr info, uint size);
    [DllImport("kernel32.dll")] static extern bool AssignProcessToJobObject(IntPtr job, IntPtr process);
    [DllImport("kernel32.dll")] static extern IntPtr GetCurrentProcess();
    [DllImport("kernel32.dll")] static extern bool CloseHandle(IntPtr handle);
    public WorkerJob()
    {
        if (!OperatingSystem.IsWindows()) throw new PlatformNotSupportedException("The C# analyzer requires Windows x64.");
        handle = CreateJobObject(IntPtr.Zero, null);
        // JOBOBJECT_EXTENDED_LIMIT_INFORMATION on Windows x64: LimitFlags at byte 16.
        var info = Marshal.AllocHGlobal(144);
        try
        {
            Marshal.Copy(new byte[144], 0, info, 144); Marshal.WriteInt32(info, 16, 0x2000);
            if (handle == IntPtr.Zero || !SetInformationJobObject(handle, 9, info, 144) || !AssignProcessToJobObject(handle, GetCurrentProcess()))
                throw new IOException("Could not establish analyzer child-process cancellation protection.");
        }
        finally { Marshal.FreeHGlobal(info); }
    }
    public void Dispose() { if (handle != IntPtr.Zero) { CloseHandle(handle); handle = IntPtr.Zero; } }
}
