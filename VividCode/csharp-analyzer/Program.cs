using Microsoft.Build.Locator;
using Microsoft.CodeAnalysis;
using Microsoft.CodeAnalysis.CSharp;
using Microsoft.CodeAnalysis.MSBuild;
using System.Text;
using System.Text.Json.Nodes;
using System.Xml.Linq;

namespace VividCode.CSharp;

internal static class Program
{
    static async Task<int> Main(string[] args)
    {
        Console.OutputEncoding = new UTF8Encoding(false);
        JsonObject? state = null; string? cache = null;
        try
        {
            if (args.Length == 0 || args[0] is not ("index" or "export")) throw new ArgumentException("Usage: index/export --project ROOT [--project-file .csproj/.sln --configuration Debug/Release --framework FRAMEWORK].");
            var options = new Dictionary<string, string>();
            for (int i = 1; i < args.Length; i += 2)
                if (i + 1 >= args.Length || !new[] { "--project", "--project-file", "--configuration", "--framework" }.Contains(args[i]) || !options.TryAdd(args[i], args[i + 1])) throw new ArgumentException("Invalid or duplicate arguments.");
            var root = Path.GetFullPath(options["--project"]);
            cache = Store.Layout(root);
            using var guard = Store.Lock(root);
            cache = Store.Layout(root);
            if (args[0] == "export") { Store.Emit(Export(cache)); return 0; }
            if (!options.TryGetValue("--project-file", out var input)) throw new ArgumentException("Provide --project-file.");
            input = Path.GetFullPath(input); Store.Plain(input);
            if (!Store.PathKey(input).StartsWith(Store.PathKey(root) + "/", StringComparison.Ordinal)) throw new ArgumentException("Imported project must be within the current project root.");
            var extension = Path.GetExtension(input).ToLowerInvariant();
            if (extension is not (".csproj" or ".sln")) throw new ArgumentException("Select .csproj or .sln.");
            string configuration = options.GetValueOrDefault("--configuration", "Debug"), framework = options.GetValueOrDefault("--framework", "");
            if (configuration is not ("Debug" or "Release")) throw new ArgumentException("Configuration must be Debug or Release.");
            Directory.CreateDirectory(cache);
            var owner = Path.Combine(Path.GetDirectoryName(cache)!, "project.json");
            if (!File.Exists(owner)) Store.Atomic(owner, new JsonObject { ["app"] = "vividcode", ["schema"] = 1, ["projectRoot"] = root.Replace('\\', '/') });
            state = new JsonObject { ["language"] = "c#", ["schema"] = Store.Schema, ["project_file"] = input, ["configuration"] = configuration, ["framework"] = framework, ["last_status"] = "running" };
            Store.Atomic(Path.Combine(cache, "config.json"), state);
            // Keep the handle alive until process exit; closing it while this process is
            // still in the job would terminate the process itself.
            var job = new WorkerJob();
            Directory.SetCurrentDirectory(Path.GetDirectoryName(input)!);
            var instance = MSBuildLocator.RegisterDefaults();
            await Index(root, cache, input, state, instance.MSBuildPath);
            GC.KeepAlive(job);
            Store.Emit(new { protocol = 1, type = "result", ok = true });
            return 0;
        }
        catch (Exception exception)
        {
            // Never change the last complete index on failure.
            var message = exception.Message;
            if (state != null && cache != null)
            {
                state["last_status"] = "failed"; state["errors"] = new JsonArray(new JsonObject { ["message"] = message });
                try { using var guard = Store.Lock(Path.GetFullPath(args[Array.IndexOf(args, "--project") + 1])); Store.Atomic(Path.Combine(cache, "config.json"), state); } catch { }
            }
            Store.Emit(new { protocol = 1, type = "result", ok = false, message }); return 1;
        }
    }
    static JsonObject Export(string cache)
    {
        var result = Store.Export(cache);
        var units = result["units"]!.AsArray();
        // An aggregate permits true cross-project incoming/outgoing relationships.
        if (units.Count > 1)
        {
            var symbols = new Dictionary<string, JsonObject>(); var relations = new Dictionary<string, JsonObject>();
            foreach (var unit in units.OfType<JsonObject>())
            {
                foreach (var s in unit["symbols"]!.AsArray().OfType<JsonObject>())
                {
                    var id = s["id"]!.GetValue<string>();
                    if (!symbols.TryGetValue(id, out var old) || string.IsNullOrEmpty(old["decl_file"]?.GetValue<string>())) symbols[id] = s;
                }
                foreach (var r in unit["relations"]!.AsArray().OfType<JsonObject>()) relations.TryAdd(r.ToJsonString(), r);
            }
            units.Insert(0, new JsonObject { ["file"] = result["state"]?["project_file"]?.DeepClone(), ["config_id"] = "csharp-solution", ["language"] = "c#", ["scope_name"] = "Entire C# project (loaded configuration)",
                ["symbols"] = new JsonArray(symbols.Values.Select(s => s.DeepClone()).ToArray()), ["relations"] = new JsonArray(relations.Values.Select(r => r.DeepClone()).ToArray()) });
        }
        return result;
    }
    sealed class Inputs
    {
        public readonly Dictionary<string, string> Files = new(StringComparer.OrdinalIgnoreCase);
        public string Capture(string path)
        {
            path = Path.GetFullPath(path); Store.Plain(path);
            if (!Files.TryGetValue(path, out var hash)) Files[path] = hash = Store.FileHash(path);
            return hash;
        }
        public void Verify()
        {
            foreach (var (path, hash) in Files)
                if (!File.Exists(path) || Store.FileHash(path) != hash) throw new IOException("Inputs changed during analysis. Retry: " + path);
        }
    }
    static async Task Index(string root, string cache, string input, JsonObject state, string msbuild)
    {
        var properties = new Dictionary<string, string> { ["Configuration"] = state["configuration"]!.GetValue<string>() };
        var framework = state["framework"]!.GetValue<string>(); if (framework.Length > 0) properties["TargetFramework"] = framework;
        using var workspace = MSBuildWorkspace.Create(properties);
        workspace.LoadMetadataForReferencedProjects = false;
        var workspaceFailures = new List<string>();
        workspace.WorkspaceFailed += (_, e) => { lock (workspaceFailures) { if (e.Diagnostic.Kind == WorkspaceDiagnosticKind.Failure) workspaceFailures.Add(e.Diagnostic.Message); } Store.Emit(new { protocol = 1, type = "diagnostic", message = e.Diagnostic.Message }); };
        Store.Emit(new { protocol = 1, type = "progress", current = 0, total = 0, file = "Loading C# project and compilation configuration..." });
        var inputs = new Inputs(); inputs.Capture(input);
        using var unity = new UnityProjectLoader();
        var isUnity = UnityProjectLoader.IsUnity(root, input);
        state["project_kind"] = isUnity ? "unity" : "dotnet";
        Solution solution;
        if (isUnity) solution = await unity.Load(root, input, state["configuration"]!.GetValue<string>(), framework, msbuild, inputs.Capture);
        else if (Path.GetExtension(input).Equals(".sln", StringComparison.OrdinalIgnoreCase)) solution = await workspace.OpenSolutionAsync(input);
        else { await workspace.OpenProjectAsync(input); solution = workspace.CurrentSolution; }
        if (workspaceFailures.Count > 0) throw new InvalidDataException(string.Join('\n', workspaceFailures.Take(15)) + "\nCheck the project SDK, workloads and NuGet dependencies; run dotnet restore in the project if needed.");
        var projects = solution.Projects.Where(p => p.Language == LanguageNames.CSharp).OrderBy(p => p.FilePath, StringComparer.Ordinal).ToArray();
        if (projects.Length == 0) throw new InvalidDataException("No loadable C# projects found.");
        if (solution.Projects.Any(p => p.Language != LanguageNames.CSharp)) throw new InvalidDataException("Only C# projects are supported. For mixed-language solutions, import the C# .csproj.");
        var keys = projects.ToDictionary(p => p.Id, p => Store.PathKey(p.FilePath!) + "|" + p.Name + "|" + state["configuration"] + "|" + framework);
        var assemblies = new Dictionary<string, string>();
        var local = new Dictionary<ProjectId, string>(); var force = new HashSet<ProjectId>();
        var global = new SortedDictionary<string, string>();
        global["version"] = Store.Version + "|" + inputs.Capture(typeof(Program).Assembly.Location) + "|" + typeof(Compilation).Assembly.FullName + "|" + msbuild + "|" + inputs.Capture(input);
        // Solution-wide build configuration changes invalidate projects conservatively.
        foreach (var path in BuildFiles(root)) global[path] = inputs.Capture(path);
        if (isUnity) global["unity-version"] = inputs.Capture(Path.Combine(root, "ProjectSettings", "ProjectVersion.txt"));
        foreach (var project in projects)
        {
            var signature = new List<string> { keys[project.Id], inputs.Capture(project.FilePath!) };
            foreach (var path in AncestorFiles(Path.GetDirectoryName(project.FilePath!)!)) { signature.Add(path + inputs.Capture(path)); if (HasImports(path)) force.Add(project.Id); }
            // An arbitrary MSBuild Import can use property expansion and external paths.
            // Until evaluated-import tracking exists, these projects never reuse stale data.
            if (isUnity) signature.Add(unity.Evaluations[project.FilePath!]);
            else if (HasImports(project.FilePath!)) force.Add(project.Id);
            foreach (var document in project.Documents.Concat(project.AdditionalDocuments).Concat(project.AnalyzerConfigDocuments).OrderBy(d => d.FilePath))
            {
                if (document.FilePath != null && File.Exists(document.FilePath)) signature.Add(document.FilePath + inputs.Capture(document.FilePath));
                signature.Add((await document.GetTextAsync()).ToString());
            }
            foreach (var reference in project.MetadataReferences.OfType<PortableExecutableReference>().OrderBy(r => r.FilePath))
                if (reference.FilePath != null) signature.Add(reference.FilePath + inputs.Capture(reference.FilePath));
            foreach (var analyzer in project.AnalyzerReferences) if (analyzer.FullPath != null && File.Exists(analyzer.FullPath)) signature.Add(analyzer.FullPath + inputs.Capture(analyzer.FullPath));
            var parse = (CSharpParseOptions)project.ParseOptions!; var options = (CSharpCompilationOptions)project.CompilationOptions!;
            signature.Add(string.Join('|', parse.LanguageVersion, string.Join(',', parse.PreprocessorSymbolNames), options.OutputKind, options.Platform, options.OptimizationLevel, options.NullableContextOptions, options.AllowUnsafe));
            var assets = Path.Combine(Path.GetDirectoryName(project.FilePath!)!, "obj", "project.assets.json"); if (File.Exists(assets)) signature.Add(inputs.Capture(assets));
            local[project.Id] = Store.Hash(string.Join('\n', signature) + string.Join('\n', global.Select(k => k.Key + k.Value)));
        }
        var fingerprints = new Dictionary<ProjectId, string>();
        string Fingerprint(Project project, HashSet<ProjectId> active)
        {
            if (fingerprints.TryGetValue(project.Id, out var cached)) return cached;
            if (!active.Add(project.Id)) throw new InvalidDataException("Project references contain a cycle.");
            var deps = project.ProjectReferences.OrderBy(r => keys[r.ProjectId]).Select(r => Fingerprint(solution.GetProject(r.ProjectId)!, active)).ToArray();
            active.Remove(project.Id);
            if (project.ProjectReferences.Any(r => force.Contains(r.ProjectId))) force.Add(project.Id);
            return fingerprints[project.Id] = Store.Hash(local[project.Id] + string.Join('|', deps));
        }
        foreach (var project in projects) Fingerprint(project, new());
        // Assembly identities are available only from compilation. Assembly names plus
        // source project keys remain stable; resolve exact identities before extraction.
        var compilations = new Dictionary<ProjectId, Compilation>();
        foreach (var project in projects)
        {
            var compilation = await project.GetCompilationAsync() ?? throw new InvalidDataException("Could not create compilation: " + project.Name);
            if (isUnity && project.AnalyzerReferences.Count > 0)
            {
                // Workspace's public compilation API does not expose generator
                // failures. Rerun from the original trees to inspect driver errors;
                // never publish a successful graph with silently missing output.
                var generatedTrees = new List<SyntaxTree>();
                foreach (var document in await project.GetSourceGeneratedDocumentsAsync())
                    if (await document.GetSyntaxTreeAsync() is { } tree) generatedTrees.Add(tree);
                compilation = compilation.RemoveSyntaxTrees(generatedTrees);
                var generators = project.AnalyzerReferences.SelectMany(r => r.GetGenerators(LanguageNames.CSharp));
                GeneratorDriver driver = CSharpGeneratorDriver.Create(generators, project.AnalyzerOptions.AdditionalFiles,
                    (CSharpParseOptions)project.ParseOptions!, project.AnalyzerOptions.AnalyzerConfigOptionsProvider);
                driver = driver.RunGeneratorsAndUpdateCompilation(compilation, out compilation, out var generatorDiagnostics);
                var failures = driver.GetRunResult().Results.Where(r => r.Exception != null).Select(r => r.Exception!.Message)
                    .Concat(generatorDiagnostics.Where(d => d.Severity == DiagnosticSeverity.Error).Select(d => d.ToString())).ToArray();
                if (failures.Length > 0) throw new InvalidDataException("Unity source generator failed. Previous complete index retained: \n" + string.Join('\n', failures));
            }
            compilations[project.Id] = compilation;
            if (!assemblies.TryAdd(compilation.Assembly.Identity.ToString(), keys[project.Id])) throw new InvalidDataException("Projects share an assembly identity; symbols cannot be distinguished reliably: " + project.Name);
        }
        var old = File.Exists(Path.Combine(cache, "index.json")) ? Store.Read(Path.Combine(cache, "index.json")) : null;
        if (old != null && old["schema"]?.GetValue<int>() != Store.Schema) throw new InvalidDataException("Incompatible C# index version.");
        var oldUnits = old?["units"]?.AsArray().OfType<JsonObject>().ToDictionary(u => u["config_id"]!.GetValue<string>()) ?? new();
        var units = new JsonArray(); int indexed = 0, reused = 0;
        foreach (var project in projects)
        {
            Store.Emit(new { protocol = 1, type = "progress", current = indexed + reused + 1, total = projects.Length, file = project.FilePath });
            string key = Store.Hash(keys[project.Id]); var compilation = compilations[project.Id];
            if (!force.Contains(project.Id) && oldUnits.TryGetValue(key, out var previous) && previous["fingerprint"]?.GetValue<string>() == fingerprints[project.Id]) { units.Add(previous.DeepClone()); reused++; continue; }
            var errors = compilation.GetDiagnostics().Where(d => d.Severity == DiagnosticSeverity.Error).Take(30).ToArray();
            if (errors.Length > 0) throw new InvalidDataException(string.Join('\n', errors.Select(d => d.ToString())) + "\nC# compilation diagnostics failed. Previous complete index retained.");
            var payload = new Extractor(assemblies, keys[project.Id]).Extract(compilation);
            payload["file"] = project.FilePath!.Replace('\\', '/'); payload["config_id"] = key; payload["fingerprint"] = fingerprints[project.Id]; payload["language"] = "c#";
            payload["scope_name"] = (isUnity ? "Unity · " : "") + project.Name + " · " + state["configuration"] + " · " + (framework.Length > 0 ? framework : "Project default target framework");
            units.Add(payload); indexed++;
        }
        if (workspaceFailures.Count > 0) throw new InvalidDataException(string.Join('\n', workspaceFailures));
        inputs.Verify();
        Store.Atomic(Path.Combine(cache, "index.json"), new JsonObject { ["schema"] = Store.Schema, ["units"] = units });
        state["last_status"] = "ok"; state["indexed"] = indexed; state["reused"] = reused;
        Store.Atomic(Path.Combine(cache, "config.json"), state);
    }
    static bool HasImports(string path)
    {
        if (Path.GetExtension(path) is not (".csproj" or ".props" or ".targets")) return false;
        return XDocument.Load(path).Descendants().Any(e => e.Name.LocalName == "Import");
    }
    static IEnumerable<string> AncestorFiles(string directory)
    {
        for (string? current = directory; current != null; current = Path.GetDirectoryName(current))
            foreach (var name in new[] { "Directory.Build.props", "Directory.Build.targets", "Directory.Packages.props", "global.json", "NuGet.Config", "nuget.config" })
                if (File.Exists(Path.Combine(current, name))) yield return Path.Combine(current, name);
    }
    static IEnumerable<string> BuildFiles(string root)
    {
        var dirs = new Stack<string>(); dirs.Push(root);
        while (dirs.Count > 0)
        {
            var dir = dirs.Pop(); Store.Plain(dir);
            foreach (var file in Directory.EnumerateFiles(dir))
                if (Path.GetExtension(file).ToLowerInvariant() is ".props" or ".targets" || Path.GetFileName(file) is "global.json" or "NuGet.Config" or "nuget.config") yield return file;
            foreach (var child in Directory.EnumerateDirectories(dir))
                if (!new[] { ".git", ".vs", ".codegraph", ".vividcode", ".work", "bin", "obj", "build", "node_modules" }.Contains(Path.GetFileName(child), StringComparer.OrdinalIgnoreCase) &&
                    !(UnityProjectLoader.HasUnityRoot(root) && dir == root && new[] { "Library", "Temp", "Logs", "UserSettings" }.Contains(Path.GetFileName(child), StringComparer.OrdinalIgnoreCase)) &&
                    (File.GetAttributes(child) & FileAttributes.ReparsePoint) == 0) dirs.Push(child);
        }
    }
}
