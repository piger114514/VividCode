using Microsoft.CodeAnalysis;
using Microsoft.CodeAnalysis.CSharp.Syntax;
using System.Text.Json.Nodes;

namespace VividCode.CSharp;

internal sealed class Extractor(IReadOnlyDictionary<string, string> assemblies, string projectKey)
{
    readonly Dictionary<string, JsonObject> symbols = new(StringComparer.Ordinal);
    readonly Dictionary<string, JsonObject> relations = new(StringComparer.Ordinal);
    static ISymbol Canonical(ISymbol s)
    {
        if (s is IMethodSymbol m) s = m.ReducedFrom ?? m.PartialDefinitionPart ?? m;
        return s.OriginalDefinition;
    }
    string Id(ISymbol symbol)
    {
        symbol = Canonical(symbol);
        string assembly = symbol.ContainingAssembly?.Identity.ToString() ?? "";
        string owner = assemblies.TryGetValue(assembly, out var key) ? key : "external:" + assembly;
        if (symbol is INamespaceSymbol) owner = projectKey;
        string identity = symbol.GetDocumentationCommentId() ?? symbol.ToDisplayString(SymbolDisplayFormat.FullyQualifiedFormat);
        if (symbol is IMethodSymbol { MethodKind: MethodKind.LocalFunction })
            identity += ":" + symbol.Locations.FirstOrDefault()?.SourceSpan.Start;
        return "c#:" + Store.Hash(owner + "|" + identity);
    }
    static bool Supported(ISymbol? s) => s is INamedTypeSymbol or IMethodSymbol or IFieldSymbol or IPropertySymbol or IEventSymbol || s is INamespaceSymbol { IsGlobalNamespace: false };
    static string Kind(ISymbol s) => s switch
    {
        INamespaceSymbol => "Namespace",
        INamedTypeSymbol { TypeKind: TypeKind.Interface } => "InterfaceDecl",
        INamedTypeSymbol { TypeKind: TypeKind.Struct } => "StructDecl",
        INamedTypeSymbol { TypeKind: TypeKind.Enum } => "EnumDecl",
        INamedTypeSymbol { TypeKind: TypeKind.Delegate } => "DelegateDecl",
        INamedTypeSymbol => "ClassDecl",
        IMethodSymbol { MethodKind: MethodKind.Constructor or MethodKind.StaticConstructor } => "Constructor",
        IMethodSymbol { MethodKind: MethodKind.Destructor } => "Destructor",
        IMethodSymbol => "CXXMethod", // Shared graph schema: the display label is 方法.
        IFieldSymbol => "FieldDecl",
        IPropertySymbol => "PropertyDecl",
        IEventSymbol => "EventDecl",
        _ => "Unknown"
    };
    static (string file, int line, int col) Position(Location? location)
    {
        if (location is null || !location.IsInSource) return ("", 0, 0);
        var span = location.GetLineSpan();
        return (span.Path.Replace('\\', '/'), span.StartLinePosition.Line + 1, span.StartLinePosition.Character + 1);
    }
    string Ensure(ISymbol symbol)
    {
        symbol = Canonical(symbol);
        var id = Id(symbol);
        if (symbols.ContainsKey(id)) return id;
        var declarations = symbol.Locations.Where(l => l.IsInSource).OrderBy(l => l.SourceTree?.FilePath, StringComparer.Ordinal).ThenBy(l => l.SourceSpan.Start).ToArray();
        var decl = Position(declarations.FirstOrDefault());
        var bodySymbol = symbol is IMethodSymbol m ? m.PartialImplementationPart ?? m : symbol;
        var definition = bodySymbol.DeclaringSyntaxReferences.Select(r => r.GetSyntax()).FirstOrDefault(n => n switch
        {
            BaseMethodDeclarationSyntax method => method.Body != null || method.ExpressionBody != null,
            LocalFunctionStatementSyntax local => local.Body != null || local.ExpressionBody != null,
            _ => true
        });
        var def = Position(definition?.GetLocation() ?? declarations.FirstOrDefault());
        string assembly = symbol.ContainingAssembly?.Identity.ToString() ?? "";
        bool external = !assemblies.ContainsKey(assembly) && symbol is not INamespaceSymbol;
        string name = symbol is INamespaceSymbol or INamedTypeSymbol ? symbol.ToDisplayString() : (symbol.ContainingType?.ToDisplayString() is string type ? type + "." : "") + symbol.Name;
        var entry = new JsonObject
        {
            ["id"] = id, ["name"] = symbol.Name, ["qualified_name"] = name, ["kind"] = Kind(symbol),
            ["signature"] = symbol.ToDisplayString(SymbolDisplayFormat.CSharpErrorMessageFormat),
            ["access"] = symbol.DeclaredAccessibility.ToString(), ["is_external"] = external,
            ["decl_file"] = decl.file, ["decl_line"] = decl.line, ["decl_col"] = decl.col,
            ["def_file"] = def.file, ["def_line"] = def.line, ["def_col"] = def.col,
            ["parent_id"] = ""
        };
        symbols[id] = entry;
        var parent = symbol is IMethodSymbol { AssociatedSymbol: not null } accessor ? accessor.AssociatedSymbol : symbol.ContainingSymbol;
        if (Supported(parent)) entry["parent_id"] = Ensure(parent!);
        return id;
    }
    void Edge(ISymbol source, ISymbol target, string kind, Location at, bool possible = false)
    {
        if (!Supported(source) || !Supported(target) || target is INamedTypeSymbol { TypeKind: TypeKind.Error }) return;
        var a = Ensure(source); var b = Ensure(target); var p = Position(at);
        if (string.IsNullOrEmpty(p.file)) return;
        var key = string.Join('|', a, b, kind, p.file, p.line, p.col);
        relations.TryAdd(key, new JsonObject { ["source_id"] = a, ["target_id"] = b, ["type"] = kind,
            ["file"] = p.file, ["line"] = p.line, ["col"] = p.col, ["confidence"] = possible ? "possible" : "certain" });
    }
    static ISymbol? Declaration(SemanticModel model, SyntaxNode node) => node switch
    {
        BaseTypeDeclarationSyntax or DelegateDeclarationSyntax or BaseMethodDeclarationSyntax or
        BasePropertyDeclarationSyntax or VariableDeclaratorSyntax or EnumMemberDeclarationSyntax or
        BaseNamespaceDeclarationSyntax or LocalFunctionStatementSyntax or AccessorDeclarationSyntax => model.GetDeclaredSymbol(node),
        _ => null
    };
    static ISymbol? Owner(SemanticModel model, SyntaxNode node)
    {
        for (var ancestor = node.Parent; ancestor != null; ancestor = ancestor.Parent)
        {
            var symbol = Declaration(model, ancestor);
            if (Supported(symbol)) return symbol;
        }
        // Top-level statements belong to Roslyn's synthesized entry point.
        var enclosing = model.GetEnclosingSymbol(node.SpanStart);
        while (enclosing != null && !Supported(enclosing)) enclosing = enclosing.ContainingSymbol;
        return enclosing;
    }
    void OverrideAndInterfaces(ISymbol declaration)
    {
        var position = declaration.Locations.FirstOrDefault(l => l.IsInSource);
        if (position == null) return;
        ISymbol? overridden = declaration switch
        {
            IMethodSymbol m => m.OverriddenMethod,
            IPropertySymbol p => p.OverriddenProperty,
            IEventSymbol e => e.OverriddenEvent,
            _ => null
        };
        if (overridden != null) Edge(declaration, overridden, "overrides", position);
        if (declaration is not INamedTypeSymbol type) return;
        foreach (var iface in type.AllInterfaces)
            foreach (var member in iface.GetMembers())
            {
                var implementation = type.FindImplementationForInterfaceMember(member);
                if (implementation != null && SymbolEqualityComparer.Default.Equals(implementation.ContainingType, type))
                    Edge(implementation, member, "implements", implementation.Locations.FirstOrDefault(l => l.IsInSource) ?? position);
            }
    }
    public JsonObject Extract(Compilation compilation)
    {
        foreach (var tree in compilation.SyntaxTrees)
        {
            var model = compilation.GetSemanticModel(tree);
            foreach (var node in tree.GetRoot().DescendantNodesAndSelf())
            {
                var declaration = Declaration(model, node);
                if (Supported(declaration)) { Ensure(declaration!); OverrideAndInterfaces(declaration!); }
                if (node is BaseTypeSyntax baseType && Owner(model, node) is INamedTypeSymbol derived && model.GetTypeInfo(baseType.Type).Type is INamedTypeSymbol baseSymbol)
                    Edge(derived, baseSymbol, baseSymbol.TypeKind == TypeKind.Interface && derived.TypeKind != TypeKind.Interface ? "implements" : "inherits", baseType.Type.GetLocation());
                var owner = Owner(model, node);
                if (owner == null) continue;
                if (node is InvocationExpressionSyntax or BaseObjectCreationExpressionSyntax or ConstructorInitializerSyntax)
                {
                    var info = model.GetSymbolInfo(node);
                    if (info.Symbol is IMethodSymbol method)
                    {
                        bool baseCall = node is InvocationExpressionSyntax { Expression: MemberAccessExpressionSyntax { Expression: BaseExpressionSyntax } };
                        bool possible = !baseCall && method.MethodKind != MethodKind.Constructor && !method.IsStatic &&
                            (method.IsVirtual || method.IsAbstract || method.IsOverride || method.ContainingType.TypeKind == TypeKind.Interface) && !method.IsSealed;
                        // A delegate invocation names its Invoke contract, never guesses its bound target.
                        possible |= method.MethodKind == MethodKind.DelegateInvoke;
                        Edge(owner, method, possible ? "possible_call" : "calls", node.GetLocation(), possible);
                    }
                }
                else if (node is SimpleNameSyntax)
                {
                    var target = model.GetSymbolInfo(node).Symbol;
                    if (target is IFieldSymbol or IPropertySymbol or IEventSymbol) Edge(owner, target, "member_ref", node.GetLocation());
                    else if (target is INamedTypeSymbol) Edge(owner, target, "references", node.GetLocation());
                    else if (target is IMethodSymbol && node.Parent is not MemberAccessExpressionSyntax { Parent: InvocationExpressionSyntax } && node.Parent is not InvocationExpressionSyntax)
                        Edge(owner, target, "references", node.GetLocation());
                }
            }
        }
        return new JsonObject { ["symbols"] = new JsonArray(symbols.OrderBy(p => p.Key).Select(p => (JsonNode)p.Value).ToArray()),
            ["relations"] = new JsonArray(relations.OrderBy(p => p.Key).Select(p => (JsonNode)p.Value).ToArray()), ["diagnostics"] = new JsonArray() };
    }
}
