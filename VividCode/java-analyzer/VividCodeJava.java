import com.sun.source.tree.*;
import com.sun.source.util.*;
import javax.lang.model.element.*;
import javax.lang.model.type.*;
import javax.lang.model.util.*;
import javax.tools.*;
import java.nio.charset.StandardCharsets;
import java.nio.file.*;
import java.util.*;
import java.io.*;

/** JDK compiler frontend. No generate(), task.call(), processors or project code execution. */
public class VividCodeJava {
    static Map<String,Object> obj(Object... pairs) {
        var m = new LinkedHashMap<String,Object>();
        for (int i=0;i<pairs.length;i+=2) m.put((String)pairs[i],pairs[i+1]);
        return m;
    }
    static String json(Object v) {
        if (v==null) return "null";
        if (v instanceof Map<?,?> m) {
            var entries = new ArrayList<String>();
            m.forEach((k,x)->entries.add(json(k.toString())+":"+json(x)));
            return "{"+String.join(",",entries)+"}";
        }
        if (v instanceof Collection<?> c) return "["+String.join(",",c.stream().map(VividCodeJava::json).toList())+"]";
        if (v instanceof Number || v instanceof Boolean) return v.toString();
        var s = new StringBuilder("\"");
        for (char ch:v.toString().toCharArray()) {
            if(ch=='"'||ch=='\\') s.append('\\').append(ch);
            else if(ch<32 || Character.isSurrogate(ch)) s.append(String.format("\\u%04x",(int)ch));
            else s.append(ch);
        }
        return s.append('"').toString();
    }
    static class Source extends ForwardingJavaFileObject<JavaFileObject> {
        final String content;
        final JavaFileObject original;
        Source(Path file,JavaFileObject original) throws IOException {
            super(original);this.original=original;
            byte[] bytes=Files.readAllBytes(file);
            var charset=StandardCharsets.UTF_8; int start=0;
            if(bytes.length>=2 && (bytes[0]&255)==255 && (bytes[1]&255)==254) {charset=StandardCharsets.UTF_16LE;start=2;}
            else if(bytes.length>=2 && (bytes[0]&255)==254 && (bytes[1]&255)==255) {charset=StandardCharsets.UTF_16BE;start=2;}
            else if(bytes.length>=3 && (bytes[0]&255)==239 && (bytes[1]&255)==187 && (bytes[2]&255)==191) start=3;
            content=charset.newDecoder().decode(java.nio.ByteBuffer.wrap(bytes,start,bytes.length-start)).toString();
        }
        public CharSequence getCharContent(boolean ignored) {return content;}
    }
    static class Filesystem extends ForwardingJavaFileManager<StandardJavaFileManager> {
        final Map<java.net.URI,Source> selected=new HashMap<>();
        Filesystem(StandardJavaFileManager manager,List<Source> sources){super(manager);for(var source:sources) selected.put(source.toUri(),source);}
        JavaFileObject unwrap(JavaFileObject file){return file instanceof Source s?s.original:file;}
        public Iterable<JavaFileObject> list(Location location,String packageName,Set<JavaFileObject.Kind> kinds,boolean recurse) throws IOException {
            var files=new ArrayList<JavaFileObject>();
            for(var file:super.list(location,packageName,kinds,recurse)) {if(file.getKind()!=JavaFileObject.Kind.SOURCE) files.add(file);else if(selected.containsKey(file.toUri())) files.add(selected.get(file.toUri()));}
            return files;
        }
        public JavaFileObject getJavaFileForInput(Location location,String className,JavaFileObject.Kind kind) throws IOException {
            var file=super.getJavaFileForInput(location,className,kind);
            return file==null||kind!=JavaFileObject.Kind.SOURCE?file:selected.get(file.toUri());
        }
        public String inferBinaryName(Location location,JavaFileObject file){return super.inferBinaryName(location,unwrap(file));}
        public boolean isSameFile(FileObject a,FileObject b){return super.isSameFile(a instanceof Source s?s.original:a,b instanceof Source s?s.original:b);}
        public boolean contains(Location location,FileObject file) throws IOException{return super.contains(location,file instanceof Source s?s.original:file);}
        public Location getLocationForModule(Location location,JavaFileObject file) throws IOException{return super.getLocationForModule(location,unwrap(file));}
    }
    final Path root;
    final Trees trees;
    final Elements elements;
    final Types types;
    final Map<Element,String> ids=new HashMap<>();
    final Map<String,Map<String,Object>> symbols=new LinkedHashMap<>();
    final List<Map<String,Object>> relations=new ArrayList<>();
    final Set<String> relationKeys=new HashSet<>();
    int unresolved=0;
    VividCodeJava(Path root,JavacTask task) {this.root=root;trees=Trees.instance(task);elements=task.getElements();types=task.getTypes();}
    String relative(CompilationUnitTree unit) {return root.relativize(Path.of(unit.getSourceFile().toUri())).toString().replace('\\','/');}
    boolean supported(Element e) {return e instanceof TypeElement || e instanceof ExecutableElement || e instanceof VariableElement;}
    String name(Element e) {
        if(e instanceof TypeElement t) return t.getQualifiedName().length()>0?t.getQualifiedName().toString():elements.getBinaryName(t).toString();
        return e.getEnclosingElement() instanceof TypeElement t ? name(t)+"."+e.getSimpleName() : e.toString();
    }
    String identity(Element e) {
        if(e instanceof TypeElement t) return "java:type:"+elements.getBinaryName(t);
        if(e instanceof ExecutableElement m) return "java:method:"+identity(e.getEnclosingElement())+":"+m.getSimpleName()+"("+String.join(",",m.getParameters().stream().map(p->types.erasure(p.asType()).toString()).toList())+")";
        if(e instanceof VariableElement && !e.getKind().isField() && e.getKind()!=ElementKind.RECORD_COMPONENT) {
            TreePath path=trees.getPath(e);
            long start=path==null?-1:trees.getSourcePositions().getStartPosition(path.getCompilationUnit(),path.getLeaf());
            return "java:variable:"+identity(e.getEnclosingElement())+":"+e.getSimpleName()+":"+start;
        }
        return "java:field:"+identity(e.getEnclosingElement())+":"+e.getSimpleName();
    }
    Map<String,Object> position(CompilationUnitTree unit,Tree tree) {
        long pos=trees.getSourcePositions().getStartPosition(unit,tree);
        // LineMap expands tabs; navigation needs actual UTF-16 source columns.
        try {
            String text=unit.getSourceFile().getCharContent(true).toString();
            if(pos<0||pos>text.length()) return obj("file",relative(unit),"line",1,"col",1);
            int p=(int)pos, begin=p;
            while(begin>0&&text.charAt(begin-1)!='\n'&&text.charAt(begin-1)!='\r') --begin;
            return obj("file",relative(unit),"line",unit.getLineMap().getLineNumber(pos),"col",p-begin+1);
        } catch(IOException ex) {throw new UncheckedIOException(ex);}
    }
    String register(Element e,CompilationUnitTree unit,Tree tree) {
        if(!supported(e)) return null;
        String id=identity(e); ids.put(e,id);
        if(symbols.containsKey(id) && (unit==null || Boolean.FALSE.equals(symbols.get(id).get("is_external")))) return id;
        String kind=e instanceof TypeElement ? (e.getKind().isInterface()?"InterfaceDecl":e.getKind()==ElementKind.ENUM?"EnumDecl":"ClassDecl") :
            e instanceof ExecutableElement ? (e.getKind()==ElementKind.CONSTRUCTOR?"Constructor":"CXXMethod") :
            e.getKind()==ElementKind.PARAMETER ? "ParmDecl" : e.getKind().isField() || e.getKind()==ElementKind.RECORD_COMPONENT ? "FieldDecl" : "VarDecl";
        String parent=supported(e.getEnclosingElement())?register(e.getEnclosingElement(),null,null):null;
        var s=obj("id",id,"name",e.getSimpleName().toString(),"qualified_name",name(e),"kind",kind,"language","java",
            "parent_id",parent,"signature",e instanceof ExecutableElement?e.toString():e.asType().toString(),"is_external",unit==null);
        if(unit!=null) {
            var p=position(unit,tree);
            for(String key:List.of("file","line","col")) {s.put("decl_"+key,p.get(key));s.put("def_"+key,p.get(key));}
        }
        symbols.put(id,s);return id;
    }
    void edge(String source,Element target,String kind,CompilationUnitTree unit,Tree site,boolean possible) {
        if(source==null || !supported(target)) return;
        String id=register(target,null,null);
        var r=position(unit,site);r.putAll(obj("source_id",source,"target_id",id,"type",kind,"confidence",possible?"possible":"exact"));
        if(relationKeys.add(json(r))) relations.add(r);
    }
    boolean virtual(Element e,Tree expression) {
        if(!(e instanceof ExecutableElement m) || m.getKind()==ElementKind.CONSTRUCTOR) return false;
        Set<Modifier> mods=m.getModifiers();
        if(mods.contains(Modifier.STATIC)||mods.contains(Modifier.PRIVATE)||mods.contains(Modifier.FINAL)||m.getEnclosingElement().getModifiers().contains(Modifier.FINAL)) return false;
        if(expression instanceof MemberSelectTree s && (s.getExpression().toString().equals("super")||s.getExpression().toString().endsWith(".super"))) return false;
        return true;
    }
    class Scan extends TreePathScanner<Void,Void> {
        final CompilationUnitTree unit; String owner; final boolean declarations;
        Scan(CompilationUnitTree unit,boolean declarations) {this.unit=unit;this.declarations=declarations;}
        public Void visitClass(ClassTree t,Void p) {
            Element e=trees.getElement(getCurrentPath());String previous=owner;
            owner=register(e,unit,t);
            if(!declarations && e instanceof TypeElement type) {
                if(t.getExtendsClause()!=null) relationType(t.getExtendsClause(),type.getKind().isInterface()?"inherits":"inherits");
                for(Tree base:t.getImplementsClause()) relationType(base,type.getKind().isInterface()?"inherits":"implements");
            }
            super.visitClass(t,p);owner=previous;return null;
        }
        void relationType(Tree t,String kind) {edge(owner,trees.getElement(new TreePath(getCurrentPath(),t)),kind,unit,t,false);}
        public Void visitMethod(MethodTree t,Void p) {
            if(trees.getSourcePositions().getEndPosition(unit,t)<0) return null;
            Element e=trees.getElement(getCurrentPath());String previous=owner;owner=register(e,unit,t);
            if(!declarations && e instanceof ExecutableElement method && e.getEnclosingElement() instanceof TypeElement type) {
                var seen=new HashSet<String>(); var todo=new ArrayDeque<TypeMirror>(types.directSupertypes(type.asType()));
                while(!todo.isEmpty()) {
                    TypeMirror base=todo.remove();if(!seen.add(base.toString())) continue;
                    Element parent=types.asElement(base);
                    if(parent instanceof TypeElement b) for(Element candidate:b.getEnclosedElements())
                        if(candidate instanceof ExecutableElement m && elements.overrides(method,m,type)) edge(owner,m,"overrides",unit,t,false);
                    todo.addAll(types.directSupertypes(base));
                }
            }
            super.visitMethod(t,p);owner=previous;return null;
        }
        public Void visitVariable(VariableTree t,Void p) {
            Element e=trees.getElement(getCurrentPath());String previous=owner;
            if(supported(e)) {String id=register(e,unit,t);if(e.getKind().isField() || e.getKind()==ElementKind.RECORD_COMPONENT) owner=id;}
            super.visitVariable(t,p);owner=previous;return null;
        }
        void call(Tree expression,Tree site) {
            Element target=trees.getElement(new TreePath(getCurrentPath(),expression));
            if(target instanceof ExecutableElement) {boolean possible=virtual(target,expression);edge(owner,target,possible?"possible_call":"calls",unit,site,possible);}
            else {
                var pos=position(unit,site);String id="java:unresolved:"+pos.get("file")+":"+pos.get("line")+":"+pos.get("col");
                symbols.put(id,obj("id",id,"name",expression.toString(),"qualified_name","未解析："+expression,"kind","UnresolvedCall","language","java","is_external",true));
                pos.putAll(obj("source_id",owner,"target_id",id,"type","possible_call","confidence","unresolved"));
                if(owner!=null && relationKeys.add(json(pos))) {relations.add(pos);unresolved++;}
            }
        }
        public Void visitMethodInvocation(MethodInvocationTree t,Void p) {if(!declarations && trees.getSourcePositions().getEndPosition(unit,t)>=0) call(t.getMethodSelect(),t);return super.visitMethodInvocation(t,p);}
        public Void visitNewClass(NewClassTree t,Void p) {
            if(!declarations) edge(owner,trees.getElement(getCurrentPath()),"calls",unit,t,false);
            return super.visitNewClass(t,p);
        }
        public Void visitMemberReference(MemberReferenceTree t,Void p) {
            if(!declarations) edge(owner,trees.getElement(getCurrentPath()),"references",unit,t,false);
            return super.visitMemberReference(t,p);
        }
        void reference(Tree t) {
            if(declarations) return;
            Element e=trees.getElement(getCurrentPath());
            if(e instanceof TypeElement) edge(owner,e,"references",unit,t,false);
            else if(e instanceof VariableElement) edge(owner,e,e.getKind().isField()?"member_ref":"references",unit,t,false);
        }
        public Void visitIdentifier(IdentifierTree t,Void p) {reference(t);return super.visitIdentifier(t,p);}
        public Void visitMemberSelect(MemberSelectTree t,Void p) {reference(t);return super.visitMemberSelect(t,p);}
        // Import declarations are resolution inputs, not execution by the preceding class.
        public Void visitImport(ImportTree t,Void p) {return null;}
    }
    static String decode(String s) {return new String(Base64.getDecoder().decode(s),StandardCharsets.UTF_8);}
    public static void main(String[] args) throws Exception {
        try {
            var lines=Files.readAllLines(Path.of(args[0]),StandardCharsets.UTF_8);Path root=Path.of(decode(lines.get(0)));
            var files=new ArrayList<Path>();var options=new ArrayList<>(List.of("-proc:none","-implicit:none","-Xlint:none","-encoding","UTF-8"));
            for(String line:lines.subList(1,lines.size())) {if(line.startsWith("F\t")) files.add(Path.of(decode(line.substring(2))));else if(line.startsWith("O\t")) options.add(decode(line.substring(2)));else throw new IOException("Invalid request record");}
            JavaCompiler compiler=ToolProvider.getSystemJavaCompiler();
            if(compiler==null) throw new IOException("需要完整 JDK 17+，当前环境没有 jdk.compiler。");
            var diagnostics=new DiagnosticCollector<JavaFileObject>();
            try(var fm=compiler.getStandardFileManager(diagnostics,Locale.ROOT,StandardCharsets.UTF_8)) {
                var sources=new ArrayList<Source>();
                for(Path file:files) sources.add(new Source(file,fm.getJavaFileObjects(file).iterator().next()));
                var units=new ArrayList<Map<String,Object>>();
                if(!sources.isEmpty()) {
                    var task=(JavacTask)compiler.getTask(new StringWriter(),new Filesystem(fm,sources),diagnostics,options,null,sources);
                    var parsed=new ArrayList<CompilationUnitTree>();task.parse().forEach(parsed::add);
                    // All compiler errors fail the transaction; no guessed success on missing classpath.
                    if(diagnostics.getDiagnostics().stream().noneMatch(d->d.getKind()==Diagnostic.Kind.ERROR)) task.analyze();
                    var errors=diagnostics.getDiagnostics().stream().filter(d->d.getKind()==Diagnostic.Kind.ERROR).map(d->d.getSource()+":"+d.getLineNumber()+":"+d.getColumnNumber()+" · "+d.getMessage(Locale.ROOT)).toList();
                    if(!errors.isEmpty()) throw new IOException(String.join("\n",errors.subList(0,Math.min(50,errors.size()))));
                    var analyzer=new VividCodeJava(root,task);
                    for(var unit:parsed) analyzer.new Scan(unit,true).scan(unit,null);
                    for(var unit:parsed) analyzer.new Scan(unit,false).scan(unit,null);
                    for(var unit:parsed) {
                        String file=analyzer.relative(unit);
                        var own=analyzer.symbols.values().stream().filter(s->file.equals(s.get("decl_file"))).toList();
                        var edges=analyzer.relations.stream().filter(r->file.equals(r.get("file"))).toList();
                        // Cross-file targets are included by identity so every unit is independently navigable.
                        var all=new LinkedHashMap<String,Map<String,Object>>();for(var s:own) all.put((String)s.get("id"),s);
                        for(var r:edges) for(String key:List.of("source_id","target_id")) {String id=(String)r.get(key);if(analyzer.symbols.containsKey(id)) all.put(id,analyzer.symbols.get(id));}
                        var todo=new ArrayDeque<>(all.values());
                        while(!todo.isEmpty()) {var s=todo.remove();String parent=(String)s.get("parent_id");if(parent!=null&&!all.containsKey(parent)&&analyzer.symbols.containsKey(parent)){var ancestor=analyzer.symbols.get(parent);all.put(parent,ancestor);todo.add(ancestor);}}
                        units.add(obj("file",file,"config_id","java-project","language","java","symbols",all.values(),"relations",edges));
                    }
                }
                System.out.print(json(obj("units",units,"dependencies",obj(),"parser","JDK "+Runtime.version(),"warnings",List.of(),"unresolved",0)));
            }
        } catch(Exception ex) {System.err.println(ex.getMessage());System.exit(1);}
    }
}
