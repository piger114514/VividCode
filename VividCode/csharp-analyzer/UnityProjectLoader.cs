using System.Collections.Immutable;
using Microsoft.CodeAnalysis;
using Microsoft.CodeAnalysis.CSharp;
using Microsoft.CodeAnalysis.Diagnostics;
using Microsoft.CodeAnalysis.Text;
using System.Diagnostics;
using System.Reflection;
using System.Text;
using System.Text.Json.Nodes;
using System.Text.RegularExpressions;
using System.Xml.Linq;

namespace VividCode.CSharp;

// Unity supplies explicit framework/engine references. Evaluate its generated
// projects without running build/restore targets, then reuse Roslyn extraction.
internal sealed class UnityProjectLoader : IDisposable
{
    readonly AdhocWorkspace workspace = new();
    public readonly Dictionary<string, string> Evaluations = new(StringComparer.OrdinalIgnoreCase);
    public static bool HasUnityRoot(string root) => Directory.Exists(Path.Combine(root, "Assets")) && File.Exists(Path.Combine(root, "ProjectSettings", "ProjectVersion.txt"));
    public static bool IsUnity(string root, string input) => HasUnityRoot(root) && ProjectFiles(input) is { Length: > 0 } files && files.All(IsGenerated);

    static bool IsGenerated(string path)
    {
        Store.Plain(path);
        var xml = XDocument.Load(path);
        return xml.Descendants().Any(e => e.Name.LocalName == "UnityVersion" ||
            e.Name.LocalName == "ProjectCapability" && (string?)e.Attribute("Include") == "Unity");
    }
    static string[] ProjectFiles(string input)
    {
        if (Path.GetExtension(input).Equals(".csproj", StringComparison.OrdinalIgnoreCase)) return new[] { input };
        Store.Plain(input);
        var paths = Regex.Matches(File.ReadAllText(input), "^Project\\([^\\r\\n]*\\)\\s*=\\s*\"[^\"]*\"\\s*,\\s*\"([^\"]+)\"", RegexOptions.Multiline)
            .Select(m => m.Groups[1].Value).Where(p => Path.GetExtension(p).Equals(".csproj", StringComparison.OrdinalIgnoreCase))
            .Select(p => Path.GetFullPath(p, Path.GetDirectoryName(input)!)).Distinct(StringComparer.OrdinalIgnoreCase).ToArray();
        return paths;
    }
    static async Task<JsonObject> Evaluate(string path, string configuration, string framework, string msbuild)
    {
        var start = new ProcessStartInfo("dotnet") { UseShellExecute = false, CreateNoWindow = true,
            RedirectStandardOutput = true, RedirectStandardError = true, StandardOutputEncoding = Encoding.UTF8,
            StandardErrorEncoding = Encoding.UTF8, WorkingDirectory = Path.GetDirectoryName(path)! };
        foreach (var arg in new[] { Path.Combine(msbuild, "MSBuild.dll"), path, "-nologo", "-verbosity:quiet",
            "-getProperty:AssemblyName,LangVersion,DefineConstants,AllowUnsafeBlocks,Nullable,Optimize,PlatformTarget,NoStdLib,NoStandardLibraries,TargetFramework,MSBuildAllProjects",
            "-getItem:Compile,Reference,ProjectReference,Analyzer,AdditionalFiles,EditorConfigFiles",
            "-property:Configuration=" + configuration, "-property:Platform=AnyCPU" }) start.ArgumentList.Add(arg);
        if (framework.Length > 0) start.ArgumentList.Add("-property:TargetFramework=" + framework);
        using var process = Process.Start(start) ?? throw new IOException("Could not start Unity project configuration evaluation.");
        var stdout = process.StandardOutput.ReadToEndAsync(); var stderr = process.StandardError.ReadToEndAsync();
        using var timeout = new CancellationTokenSource(TimeSpan.FromSeconds(60));
        try { await process.WaitForExitAsync(timeout.Token); }
        catch (OperationCanceledException) { process.Kill(true); throw new IOException("Unity project configuration evaluation timed out: " + path); }
        var output = await stdout; var errors = await stderr;
        if (process.ExitCode != 0) throw new InvalidDataException("Unity project configuration evaluation failed. A .NET SDK with MSBuild 17.8+ is required: \n" + output + errors);
        try { return JsonNode.Parse(output)!.AsObject(); }
        catch (Exception e) { throw new InvalidDataException("Unity configuration evaluation did not return valid JSON: " + output + errors, e); }
    }
    static IEnumerable<JsonObject> Items(JsonObject data, string name) => data["Items"]?[name]?.AsArray().OfType<JsonObject>() ?? Enumerable.Empty<JsonObject>();
    static string Value(JsonObject item, string name) => item[name]?.GetValue<string>() ?? "";
    static string ItemPath(JsonObject item, string project, string field = "FullPath") => Path.GetFullPath(Value(item, field), Path.GetDirectoryName(project)!);
    static void RequireFile(string path)
    {
        Store.Plain(path);
        if (!File.Exists(path)) throw new FileNotFoundException("Missing Unity project dependency: " + path + "\nOpen the project in the matching Unity version, wait for compilation and regenerate .sln/.csproj, then refresh.", path);
    }

    public async Task<Solution> Load(string root, string input, string configuration, string framework, string msbuild, Func<string, string> capture)
    {
        capture(Path.Combine(root, "ProjectSettings", "ProjectVersion.txt"));
        var queue = new Queue<string>(ProjectFiles(input));
        var evaluated = new Dictionary<string, JsonObject>(StringComparer.OrdinalIgnoreCase);
        while (queue.Count > 0)
        {
            var path = queue.Dequeue(); if (evaluated.ContainsKey(path)) continue;
            if (!Store.PathKey(path).StartsWith(Store.PathKey(root) + "/", StringComparison.Ordinal)) throw new InvalidDataException("Unity project reference is outside the root: " + path);
            RequireFile(path); capture(path);
            if (!IsGenerated(path)) throw new InvalidDataException("Unity solution contains a C# project not generated by Unity. Import it separately: " + path);
            Store.Emit(new { protocol = 1, type = "progress", current = 0, total = 0, file = "Reading Unity compilation configuration: " + Path.GetFileName(path) });
            var data = await Evaluate(path, configuration, framework, msbuild);
            // MSBuild includes volatile filesystem timestamps, including last access.
            // Content hashes below track inputs; timestamps must not defeat reuse.
            foreach (var group in data["Items"]!.AsObject())
                foreach (var item in group.Value!.AsArray().OfType<JsonObject>())
                    foreach (var name in new[] { "ModifiedTime", "CreatedTime", "AccessedTime" }) item.Remove(name);
            evaluated.Add(path, data);
            foreach (var reference in Items(data, "ProjectReference")) queue.Enqueue(ItemPath(reference, path));
        }
        if (evaluated.Count == 0) throw new InvalidDataException("Unity solution has no C# projects. Regenerate the project files.");
        var ids = evaluated.Keys.ToDictionary(p => p, p => ProjectId.CreateNewId(), StringComparer.OrdinalIgnoreCase);
        var assemblyNames = evaluated.Values.Select(d => Value(d["Properties"]!.AsObject(), "AssemblyName")).ToHashSet(StringComparer.OrdinalIgnoreCase);
        var solution = workspace.CurrentSolution;
        foreach (var (path, data) in evaluated)
        {
            var props = data["Properties"]!.AsObject(); string Property(string name) => Value(props, name);
            if (!bool.TryParse(Property("NoStdLib"), out var noStdLib) || !noStdLib)
                if (!bool.TryParse(Property("NoStandardLibraries"), out noStdLib) || !noStdLib) throw new InvalidDataException("Unity project lacks complete explicit framework references: " + path);
            var lang = Property("LangVersion");
            if (!LanguageVersionFacts.TryParse(lang.Length == 0 ? "default" : lang, out var version)) throw new InvalidDataException("Unrecognized Unity C# language version: " + lang);
            var parse = new CSharpParseOptions(version, preprocessorSymbols: Property("DefineConstants").Split(';', StringSplitOptions.RemoveEmptyEntries | StringSplitOptions.TrimEntries).Distinct());
            var platformText = Property("PlatformTarget");
            if (!Enum.TryParse<Platform>(platformText.Length == 0 ? "AnyCpu" : platformText, true, out var platform)) throw new InvalidDataException("Unrecognized Unity compilation platform: " + platformText);
            var options = new CSharpCompilationOptions(OutputKind.DynamicallyLinkedLibrary, allowUnsafe: Property("AllowUnsafeBlocks").Equals("true", StringComparison.OrdinalIgnoreCase), platform: platform,
                optimizationLevel: Property("Optimize").Equals("true", StringComparison.OrdinalIgnoreCase) ? OptimizationLevel.Release : OptimizationLevel.Debug);
            if (Enum.TryParse<NullableContextOptions>(Property("Nullable"), true, out var nullable)) options = options.WithNullableContextOptions(nullable);
            var references = new List<MetadataReference>();
            foreach (var reference in Items(data, "Reference"))
            {
                // A source ProjectReference must win over a stale built DLL.
                if (Items(data, "ProjectReference").Any(r => assemblyNames.Contains(Value(reference, "Identity").Split(',')[0]) &&
                    Value(evaluated[ItemPath(r, path)]["Properties"]!.AsObject(), "AssemblyName") == Value(reference, "Identity").Split(',')[0])) continue;
                var hint = Value(reference, "HintPath");
                if (hint.Length == 0 && !Value(reference, "Identity").EndsWith(".dll", StringComparison.OrdinalIgnoreCase)) throw new InvalidDataException("Unity reference lacks an explicit DLL path: " + Value(reference, "Identity"));
                var file = hint.Length == 0 ? ItemPath(reference, path) : ItemPath(reference, path, "HintPath");
                RequireFile(file); capture(file);
                var aliases = Value(reference, "Aliases").Split(',', StringSplitOptions.RemoveEmptyEntries | StringSplitOptions.TrimEntries);
                references.Add(MetadataReference.CreateFromFile(file, new MetadataReferenceProperties(aliases: aliases.ToImmutableArray(), embedInteropTypes: Value(reference, "EmbedInteropTypes").Equals("true", StringComparison.OrdinalIgnoreCase))));
            }
            var projectReferences = Items(data, "ProjectReference").Where(r => !Value(r, "ReferenceOutputAssembly").Equals("false", StringComparison.OrdinalIgnoreCase))
                .Select(r => new ProjectReference(ids[ItemPath(r, path)], aliases: Value(r, "Aliases").Split(',', StringSplitOptions.RemoveEmptyEntries | StringSplitOptions.TrimEntries).ToImmutableArray()));
            var loader = new AssemblyLoader(); var analyzers = new List<AnalyzerReference>();
            var dependencySignatures = new SortedDictionary<string, string>(StringComparer.OrdinalIgnoreCase);
            foreach (var analyzer in Items(data, "Analyzer"))
            {
                var file = ItemPath(analyzer, path);
                // This known Unity IDE analyzer reports style diagnostics only; it
                // does not generate source. A removed IDE extension need not block
                // engine/source semantics. Missing unknown generators still fail.
                if (!File.Exists(file) && Path.GetFileName(file) == "Microsoft.Unity.Analyzers.dll")
                {
                    Store.Plain(file);
                    Store.Emit(new { protocol = 1, type = "diagnostic", message = "Unity IDE diagnostic analyzer missing; skipped (source generation is unaffected): " + file });
                    dependencySignatures[file] = "missing-diagnostic-analyzer"; continue;
                }
                RequireFile(file); capture(file); loader.AddDependencyLocation(file);
                // Adjacent dependencies also affect generator output and cache reuse.
                foreach (var dependency in Directory.EnumerateFiles(Path.GetDirectoryName(file)!, "*.dll")) { loader.AddDependencyLocation(dependency); dependencySignatures[dependency] = capture(dependency); }
                var reference = new AnalyzerFileReference(file, loader);
                reference.AnalyzerLoadFailed += (_, e) => throw new InvalidDataException("Unity analyzer/generator load failed: " + file + " · " + e.Message);
                analyzers.Add(reference);
            }
            solution = solution.AddProject(ProjectInfo.Create(ids[path], VersionStamp.Create(), Path.GetFileNameWithoutExtension(path), Property("AssemblyName"), LanguageNames.CSharp,
                filePath: path, compilationOptions: options, parseOptions: parse, metadataReferences: references, projectReferences: projectReferences, analyzerReferences: analyzers));
            foreach (var document in Items(data, "Compile"))
            {
                var file = ItemPath(document, path); RequireFile(file); capture(file);
                solution = solution.AddDocument(DocumentId.CreateNewId(ids[path]), Path.GetFileName(file), SourceText.From(File.ReadAllText(file)), filePath: file);
            }
            foreach (var kind in new[] { "AdditionalFiles", "EditorConfigFiles" })
                foreach (var document in Items(data, kind))
                {
                    var file = ItemPath(document, path); RequireFile(file); capture(file);
                    var id = DocumentId.CreateNewId(ids[path]); var text = SourceText.From(File.ReadAllText(file));
                    solution = kind == "AdditionalFiles" ? solution.AddAdditionalDocument(id, Path.GetFileName(file), text, filePath: file) : solution.AddAnalyzerConfigDocument(id, Path.GetFileName(file), text, filePath: file);
                }
            foreach (var import in Property("MSBuildAllProjects").Split(';', StringSplitOptions.RemoveEmptyEntries)) { RequireFile(import); dependencySignatures[import] = capture(import); }
            Evaluations[path] = data.ToJsonString() + string.Join('\n', dependencySignatures.Select(p => p.Key + p.Value));
        }
        return solution;
    }
    sealed class AssemblyLoader : IAnalyzerAssemblyLoader
    {
        public void AddDependencyLocation(string fullPath) { }
        public Assembly LoadFromPath(string fullPath) => Assembly.LoadFrom(fullPath);
    }
    public void Dispose() => workspace.Dispose();
}
