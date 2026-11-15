'use strict';
// Only the bundled compiler is loaded. Target source/configuration is data.
const ts = require('./typescript/lib/typescript.js');
const fs = require('node:fs');
const path = require('node:path');
const crypto = require('node:crypto');
const root = path.resolve(process.argv[2]);
const policy = process.argv[3] ? JSON.parse(process.argv[3]) : {force: false, maxSourceFiles: 20000, maxEntries: 50000};
if (typeof policy.force !== 'boolean' || !Number.isSafeInteger(policy.maxSourceFiles) || policy.maxSourceFiles < 1 || policy.maxSourceFiles > 2147483647 || !Number.isSafeInteger(policy.maxEntries) || policy.maxEntries < 1) throw new Error('源码读取范围无效。');
const extensions = new Set(['.js', '.jsx', '.mjs', '.cjs', '.ts', '.tsx', '.mts', '.cts']);
const excluded = new Set(['.git', '.vs', '.codegraph', '.codemap', '.vividcode', 'build', 'out', 'x64', 'x86',
  'debug', 'release', 'node_modules', 'vendor', 'third_party', '_deps', 'cmakefiles', 'bin', 'obj', '__pycache__',
  '.venv', 'venv', 'env', '.pytest_cache', '.mypy_cache', '.tox', '.idea', 'dist', 'target', '.gradle', '.mvn',
  'coverage', '.next', '.nuxt', '.cache', '.parcel-cache']);
const slash = p => p.replace(/\\/g, '/');
const key = p => slash(path.resolve(p)).toLowerCase();
const relative = p => slash(path.relative(root, p));
const dependencies = new Map();
let readBytes = 0, entries = 0;
function plain(p) {
  let current = path.resolve(p);
  while (true) {
    if (fs.existsSync(current) && fs.lstatSync(current).isSymbolicLink()) throw new Error(`不读取链接路径：${current}`);
    const parent = path.dirname(current); if (parent === current) break; current = parent;
  }
}
function hash(bytes) { return crypto.createHash('sha256').update(bytes).digest('hex'); }
function read(p) {
  p = path.resolve(p); plain(p);
  if (!fs.existsSync(p)) { dependencies.set(p, null); return undefined; }
  const stat = fs.statSync(p); if (!stat.isFile()) return undefined;
  if (!policy.force && stat.size > 2 * 1024 * 1024) throw new Error(`文件超过 2 MiB：${p}`);
  readBytes += stat.size;
  if (readBytes > 256 * 1024 * 1024) throw new Error('依赖读取超过 256 MiB，保留旧图。');
  const bytes = fs.readFileSync(p); dependencies.set(p, hash(bytes));
  // TypeScript accepts UTF-8 and UTF-16 BOM. Qt source navigation uses the same decoding.
  if (bytes[0] === 0xff && bytes[1] === 0xfe) return new TextDecoder('utf-16le', {fatal: true}).decode(bytes.subarray(2));
  if (bytes[0] === 0xfe && bytes[1] === 0xff) {
    if (bytes.length % 2) throw new Error(`非法 UTF-16：${p}`);
    return new TextDecoder('utf-16be', {fatal: true}).decode(bytes.subarray(2));
  }
  return new TextDecoder('utf-8', {fatal: true}).decode(bytes);
}
function exists(p) {
  plain(p); const result = fs.existsSync(p) && fs.statSync(p).isFile();
  if (!result) dependencies.set(path.resolve(p), null);
  return result;
}
function skip(name) { const s = name.toLowerCase(); return excluded.has(s) || s.startsWith('.vividcode-') || s.startsWith('build-') || s.startsWith('cmake-build-'); }
function scan() {
  const files = [], configs = [], pending = [root];
  while (pending.length) {
    const dir = pending.pop(); plain(dir);
    for (const item of fs.readdirSync(dir, {withFileTypes: true})) {
      if (!policy.force && ++entries > policy.maxEntries) throw new Error('目录条目超过扫描上限；保留旧图。');
      if (item.isSymbolicLink()) continue;
      const p = path.join(dir, item.name);
      if (item.isDirectory()) { if (!skip(item.name)) pending.push(p); }
      else if (item.isFile()) {
        if (extensions.has(path.extname(p).toLowerCase())) files.push(p);
        if (['tsconfig.json', 'jsconfig.json'].includes(item.name.toLowerCase())) configs.push(p);
      }
      if (!policy.force && files.length > policy.maxSourceFiles) throw new Error('JS/TS 源文件超过全局数量上限；保留旧图。');
    }
  }
  return {files: files.sort(), configs: configs.sort()};
}
function diagnostic(d) {
  const loc = d.file && d.file.getLineAndCharacterOfPosition(d.start || 0);
  return `${d.file ? relative(d.file.fileName) + ':' + (loc.line + 1) + ':' + (loc.character + 1) + ' · ' : ''}${ts.flattenDiagnosticMessageText(d.messageText, '\n')}`;
}
function analyze() {
  const inventory = scan(), warnings = [], configs = new Map();
  const defaults = {target: ts.ScriptTarget.ESNext, module: ts.ModuleKind.NodeNext,
    moduleResolution: ts.ModuleResolutionKind.NodeNext, jsx: ts.JsxEmit.Preserve, allowJs: true, checkJs: true, noEmit: true};
  let configEntries = 0;
  const configHost = {
    useCaseSensitiveFileNames: false, readFile: read, fileExists: exists,
    readDirectory: (dir, exts, excludes, includes, depth) => ts.matchFiles(dir, exts, excludes, includes, false, root, depth,
      p => {
        plain(p); const files = [], directories = [];
        if (!fs.existsSync(p)) return {files, directories};
        for (const item of fs.readdirSync(p, {withFileTypes: true})) {
          if (!policy.force && ++configEntries > Math.max(100000, policy.maxEntries)) throw new Error('配置目录展开超过扫描上限。');
          if (item.isSymbolicLink()) continue;
          if (item.isDirectory()) { if (!skip(item.name)) directories.push(item.name); }
          else if (item.isFile()) files.push(item.name);
        }
        return {files, directories};
      }, p => { plain(p); return fs.realpathSync(p); }),
    onUnRecoverableConfigFileDiagnostic: d => { throw new Error(diagnostic(d)); },
    getCurrentDirectory: () => root
  };
  function config(p) {
    p = path.resolve(p); if (configs.has(key(p))) return;
    plain(p); const parsed = ts.getParsedCommandLineOfConfigFile(p, {noEmit: true}, configHost);
    if (!parsed) throw new Error(`无法读取配置：${p}`);
    const errors = parsed.errors.filter(d => d.code !== 18003);
    if (errors.length) throw new Error(errors.map(diagnostic).join('\n'));
    configs.set(key(p), {path: p, parsed});
    for (const ref of parsed.projectReferences || []) config(ts.resolveProjectReferencePath(ref));
  }
  inventory.configs.forEach(config);
  const programs = [], assigned = new Set();
  const display = new Set(inventory.files.map(key));
  for (const c of configs.values()) {
    const roots = c.parsed.fileNames.filter(p => display.has(key(p)));
    roots.forEach(p => assigned.add(key(p)));
    if (roots.length) programs.push({roots, options: {...c.parsed.options, noEmit: true}, scope: relative(c.path)});
  }
  // An ancestor config's include/exclude remains authoritative. Unconfigured files use defaults.
  const loose = inventory.files.filter(p => !assigned.has(key(p)) && ![...configs.values()].some(c =>
    key(p).startsWith(key(path.dirname(c.path)) + '/')));
  if (loose.length) programs.push({roots: loose, options: defaults, scope: '无 tsconfig 的项目源码'});
  const units = []; let unresolved = 0;
  for (const group of programs) {
    const host = ts.createCompilerHost(group.options, true);
    host.readFile = read; host.fileExists = exists; host.getCurrentDirectory = () => root;
    host.realpath = p => { plain(p); return fs.realpathSync(p); };
    host.directoryExists = p => { plain(p); const yes = fs.existsSync(p) && fs.statSync(p).isDirectory();
      dependencies.set(path.resolve(p), yes ? '!dir' : null); return yes; };
    host.getDirectories = p => {
      plain(p); if (!fs.existsSync(p)) { dependencies.set(path.resolve(p), null); return []; }
      const dirs = fs.readdirSync(p, {withFileTypes: true}).filter(d => d.isDirectory() && !d.isSymbolicLink()).map(d => d.name).sort();
      dependencies.set(path.resolve(p), '!dirs:' + hash(Buffer.from(JSON.stringify(dirs))));
      return dirs;
    };
    host.getSourceFile = (p, version) => {
      const text = read(p); return text === undefined ? undefined : ts.createSourceFile(p, text, version, true);
    };
    host.writeFile = () => { throw new Error('分析器不允许生成项目代码。'); };
    const program = ts.createProgram(group.roots, group.options, host);
    const syntax = program.getSyntacticDiagnostics();
    if (syntax.length) throw new Error(syntax.map(diagnostic).join('\n'));
    warnings.push(...program.getOptionsDiagnostics().map(diagnostic));
    const checker = program.getTypeChecker(), nodeIds = new Map(), symbols = new Map(), symbolIds = new Map();
    const sources = program.getSourceFiles().filter(s => display.has(key(s.fileName)));
    const isFunction = n => ts.isFunctionDeclaration(n) || ts.isFunctionExpression(n) || ts.isArrowFunction(n) ||
      ts.isMethodDeclaration(n) || ts.isMethodSignature(n) || ts.isConstructorDeclaration(n) || ts.isGetAccessor(n) || ts.isSetAccessor(n);
    const isClass = n => ts.isClassDeclaration(n) || ts.isClassExpression(n);
    const pos = (n, s) => { const p = s.getLineAndCharacterOfPosition(n.getStart(s)); return {line: p.line + 1, col: p.character + 1}; };
    function binding(n) {
      if ((ts.isArrowFunction(n) || ts.isFunctionExpression(n) || ts.isClassExpression(n)) &&
          (ts.isVariableDeclaration(n.parent) || ts.isPropertyDeclaration(n.parent) || ts.isPropertyAssignment(n.parent))) return n.parent;
      if ((ts.isArrowFunction(n) || ts.isFunctionExpression(n) || ts.isClassExpression(n)) &&
          ts.isBinaryExpression(n.parent) && n.parent.operatorToken.kind === ts.SyntaxKind.EqualsToken) return n.parent;
      return n;
    }
    function nameOf(n, s) {
      const b = binding(n);
      return b.name ? b.name.getText(s) : b.left ? b.left.getText(s) : ts.isConstructorDeclaration(n) ? 'constructor' : `匿名函数@${pos(n, s).line}:${pos(n, s).col}`;
    }
    function register(n, kind, owner, s) {
      const b = binding(n), loc = pos(b.name || b.left || n, s), name = nameOf(n, s);
      const sym = (b.name || b.left) && checker.getSymbolAtLocation(b.name || b.left);
      let id = sym && symbolIds.get(sym);
      const qualified = owner && !['ModuleDecl', 'FileScope'].includes(symbols.get(owner).kind) ? symbols.get(owner).qualified_name + '.' + name : name;
      if (!id) id = `javascript:${relative(s.fileName)}:${n.getStart(s)}:${kind}:${name}`;
      const end = s.getLineAndCharacterOfPosition(n.end);
      let signature = '';
      if (isFunction(n)) {
        try { const sig = checker.getSignatureFromDeclaration(n); signature = sig ? checker.signatureToString(sig) : ''; } catch {}
      }
      if (!symbols.has(id) || n.body) symbols.set(id, {id, kind, name, qualified_name: qualified, parent_id: owner || '',
        language: 'javascript', signature, decl_file: relative(s.fileName), decl_line: loc.line, decl_col: loc.col,
        def_file: s.isDeclarationFile || (isFunction(n) && !n.body) ? '' : relative(s.fileName),
        def_line: loc.line, def_col: loc.col, end_line: end.line + 1, is_external: false});
      nodeIds.set(n, id); nodeIds.set(b, id); if (sym) symbolIds.set(sym, id);
      return id;
    }
    function declarations(n, owner, s) {
      let next = owner;
      if (isClass(n)) next = register(n, 'ClassDecl', owner, s);
      else if (ts.isInterfaceDeclaration(n)) next = register(n, 'InterfaceDecl', owner, s);
      else if (ts.isModuleDeclaration(n)) next = register(n, 'Namespace', owner, s);
      else if (isFunction(n)) next = register(n, ts.isConstructorDeclaration(n) ? 'Constructor' :
        ts.isMethodDeclaration(n) || ts.isMethodSignature(n) || ts.isGetAccessor(n) || ts.isSetAccessor(n) ||
        (ts.isPropertyDeclaration(binding(n)) && owner && symbols.get(owner).kind === 'ClassDecl') ? 'CXXMethod' : 'FunctionDecl', owner, s);
      else if (ts.isPropertyDeclaration(n) || ts.isPropertySignature(n) || ts.isPropertyAssignment(n)) {
        if (!(n.initializer && (isFunction(n.initializer) || isClass(n.initializer)))) register(n, 'PropertyDecl', owner, s);
      } else if (ts.isEnumDeclaration(n)) next = register(n, 'EnumDecl', owner, s);
      else if ((ts.isVariableDeclaration(n) || ts.isParameter(n) || ts.isBindingElement(n)) && ts.isIdentifier(n.name) && !(n.initializer && (isFunction(n.initializer) || isClass(n.initializer)))) {
        let declaration = n;
        while (ts.isBindingElement(declaration) || ts.isObjectBindingPattern(declaration) || ts.isArrayBindingPattern(declaration)) declaration = declaration.parent;
        register(n, ts.isParameter(declaration) ? 'ParmDecl' : 'VarDecl', owner, s);
      }
      ts.forEachChild(n, c => declarations(c, next, s));
    }
    for (const s of sources) {
      const id = `javascript:${relative(s.fileName)}:module`;
      symbols.set(id, {id, kind: 'ModuleDecl', name: relative(s.fileName), qualified_name: relative(s.fileName), parent_id: '',
        language: 'javascript', signature: '', decl_file: relative(s.fileName), decl_line: 1, decl_col: 1,
        def_file: relative(s.fileName), def_line: 1, def_col: 1, is_external: false});
      nodeIds.set(s, id); ts.forEachChild(s, n => declarations(n, id, s));
    }
    function symbolTarget(sym) {
      if (!sym) return '';
      if (sym.flags & ts.SymbolFlags.Alias) { try { sym = checker.getAliasedSymbol(sym); } catch { return ''; } }
      if (symbolIds.has(sym)) return symbolIds.get(sym);
      for (const d of sym.declarations || []) if (nodeIds.has(d)) return nodeIds.get(d);
      return '';
    }
    const mutable = new Set();
    function writes(n) {
      if (ts.isBinaryExpression(n) && n.operatorToken.kind >= ts.SyntaxKind.FirstAssignment && n.operatorToken.kind <= ts.SyntaxKind.LastAssignment) {
        const expr = n.left, id = symbolTarget(checker.getSymbolAtLocation(ts.isPropertyAccessExpression(expr) ? expr.name : expr));
        if (id) mutable.add(id);
      }
      ts.forEachChild(n, writes);
    }
    sources.forEach(writes);
    function target(expr, call) {
      // A resolved signature may point to a function type/parameter, not its runtime implementation.
      if (call) {
        const sig = checker.getResolvedSignature(call), d = sig && sig.declaration;
        if (d && nodeIds.has(d) && isFunction(d)) return nodeIds.get(d);
      }
      let found = symbolTarget(checker.getSymbolAtLocation(ts.isPropertyAccessExpression(expr) ? expr.name : expr));
      if (found && ['VarDecl', 'ParmDecl', 'PropertyDecl'].includes(symbols.get(found).kind) && call) return '';
      return found;
    }
    const seen = new Set();
    function relation(owner, end, type, n, s, uncertain = false) {
      const loc = pos(n, s);
      if (uncertain && type === 'calls') type = 'possible_call';
      if (!end) {
        const expression = n.getText(s).slice(0, 160);
        end = `javascript:unresolved:${relative(s.fileName)}:${n.getStart(s)}`;
        symbols.set(end, {id: end, kind: 'UnresolvedCall', name: `未解析：${expression}`, qualified_name: `未解析：${expression}`,
          language: 'javascript', parent_id: '', is_external: true, unresolved: true, signature: ''});
        unresolved++; uncertain = true; if (type === 'calls') type = 'possible_call';
      }
      const r = {source_id: owner, target_id: end, type, confidence: symbols.get(end).unresolved ? 'unresolved' : uncertain ? 'possible' : 'static',
        file: relative(s.fileName), line: loc.line, col: loc.col, language: 'javascript'};
      const k = JSON.stringify(r); if (!seen.has(k)) { seen.add(k); relations.push(r); }
    }
    const relations = [];
    function visit(n, owner, s) {
      if (isClass(n) || isFunction(n) || ts.isModuleDeclaration(n)) owner = nodeIds.get(n) || owner;
      if ((isClass(n) || ts.isInterfaceDeclaration(n)) && n.heritageClauses) {
        for (const clause of n.heritageClauses) for (const t of clause.types)
          relation(nodeIds.get(n), target(t.expression), clause.token === ts.SyntaxKind.ImplementsKeyword ? 'implements' : 'inherits', t.expression, s);
      }
      if (ts.isCallExpression(n) || ts.isNewExpression(n) || ts.isTaggedTemplateExpression(n)) {
        const expr = ts.isTaggedTemplateExpression(n) ? n.tag : n.expression;
        const end = target(expr, ts.isTaggedTemplateExpression(n) ? undefined : n);
        const kind = end && symbols.get(end).kind;
        const reference = symbolTarget(checker.getSymbolAtLocation(ts.isPropertyAccessExpression(expr) ? expr.name : expr));
        const uncertain = ts.isPropertyAccessExpression(expr) || ts.isElementAccessExpression(expr) ||
          (kind === 'CXXMethod') || mutable.has(end) || mutable.has(reference) || (n.questionDotToken !== undefined);
        relation(owner, end, 'calls', expr, s, uncertain);
      }
      // JSX denotes a possible component invocation; intrinsic HTML tags are not project functions.
      if (ts.isJsxOpeningElement(n) || ts.isJsxSelfClosingElement(n)) {
        const expr = n.tagName;
        if (!ts.isIdentifier(expr) || !/^[a-z]/.test(expr.text)) relation(owner, target(expr), 'possible_call', expr, s, true);
      }
      if (ts.isPropertyAccessExpression(n) || ts.isElementAccessExpression(n)) {
        const end = target(n);
        if (end && ['PropertyDecl', 'VarDecl'].includes(symbols.get(end).kind)) relation(owner, end, 'member_ref', n, s);
      }
      if (ts.isIdentifier(n) && !(n.parent.name === n &&
          (ts.isVariableDeclaration(n.parent) || ts.isParameter(n.parent) || ts.isBindingElement(n.parent) || ts.isPropertyDeclaration(n.parent)))) {
        const end = target(n);
        if (end && ['VarDecl', 'ParmDecl', 'PropertyDecl'].includes(symbols.get(end).kind)) relation(owner, end, 'references', n, s);
      }
      ts.forEachChild(n, c => visit(c, owner, s));
    }
    for (const s of sources) {
      const start = relations.length; visit(s, nodeIds.get(s), s);
      const sites = relations.slice(start), endpoints = new Map();
      for (const [id, sym] of symbols) if (sym.decl_file === relative(s.fileName)) endpoints.set(id, sym);
      for (const r of sites) for (const id of [r.source_id, r.target_id]) endpoints.set(id, symbols.get(id));
      for (const sym of [...endpoints.values()]) {
        let parent = sym.parent_id;
        while (parent && !endpoints.has(parent)) { endpoints.set(parent, symbols.get(parent)); parent = symbols.get(parent).parent_id; }
      }
      units.push({file: relative(s.fileName), scope_name: `${group.scope} · ${relative(s.fileName)}`,
        config_id: `javascript-1:${group.scope}`, symbols: [...endpoints.values()], relations: sites});
    }
    // Type errors are visible diagnostics, not a reason to throw away usable syntax structure.
    warnings.push(...program.getSemanticDiagnostics().slice(0, 100).map(diagnostic));
  }
  return {schema: 1, parser: ts.version, inventory: inventory.files.map(relative),
    dependencies: Object.fromEntries(dependencies), units, unresolved, warnings: [...new Set(warnings)].slice(0, 100)};
}
try { const output = JSON.stringify(analyze()); if (Buffer.byteLength(output) > 128 * 1024 * 1024) throw new Error('JS/TS 结果超过 128 MiB。'); process.stdout.write(output); }
catch (error) { process.stderr.write(String(error.stack || error)); process.exitCode = 1; }
