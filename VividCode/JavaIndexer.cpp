#include "JavaIndexer.h"
#include "AnalysisEnvironment.h"
#include "ProjectScanner.h"
#include <QtCore>
#include <stdexcept>

namespace {
constexpr qint64 limit = 128 * 1024 * 1024;
void fail(const QString &text) { throw std::runtime_error(text.toStdString()); }
void plain(const QString &path) {
    for (QString current = QFileInfo(path).absoluteFilePath();;) {
        const QFileInfo info(current); if (info.isSymLink() || info.isJunction()) fail(QStringLiteral("Linked cache or input rejected: ") + current);
        const QString parent = info.absolutePath(); if (parent == current) break; current = parent;
    }
}
QJsonObject readJson(const QString &path, bool optional = false) {
    plain(path); QFile file(path); if (optional && !file.exists()) return {};
    if (!file.open(QIODevice::ReadOnly) || file.size() > limit) fail(QStringLiteral("Could not read JSON: ") + path);
    QJsonParseError error; const auto doc = QJsonDocument::fromJson(file.readAll(), &error);
    if (error.error != QJsonParseError::NoError || !doc.isObject()) fail(QStringLiteral("Invalid JSON: ") + path);
    return doc.object();
}
void atomic(const QString &path, const QJsonObject &object) {
    plain(path); const auto bytes = QJsonDocument(object).toJson(QJsonDocument::Compact);
    if (bytes.size() > limit) fail(QStringLiteral("Java cache exceeds 128 MiB."));
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit()) fail(QStringLiteral("Could not write cache: ") + file.errorString());
}
QString digestFile(const QString &path) {
    QFile file(path); QCryptographicHash hash(QCryptographicHash::Sha256);
    if (!file.open(QIODevice::ReadOnly) || !hash.addData(&file) || file.error() != QFileDevice::NoError) fail(QStringLiteral("Could not read dependency: ") + path);
    return QString::fromLatin1(hash.result().toHex());
}
void emitJson(QJsonObject object) {
    object["protocol"] = 1; QTextStream out(stdout); out.setEncoding(QStringConverter::Utf8);
    out << QJsonDocument(object).toJson(QJsonDocument::Compact) << '\n'; out.flush();
}
struct Layout { QString root, shared, cache; };
Layout layout(const QString &project) {
    const QFileInfo info(project); if (!info.isDir() || info.fileName().compare(".vividcode", Qt::CaseInsensitive) == 0) fail(QStringLiteral("Select the project root directory."));
    const QString root = info.canonicalFilePath(); plain(root + "/.vividcode");
    const auto manifest = readJson(root + "/.vividcode/project.json");
    if (manifest.value("app") != "vividcode" || manifest.value("schema").toInt() != 1) fail(QStringLiteral("Project is not initialized or project.json is incompatible."));
    const QString shared = VividCode::semanticDirectory(root); plain(shared);
    if (QFileInfo::exists(shared)) {
        for (const auto &entry : QDir(shared).entryInfoList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden)) plain(entry.absoluteFilePath());
        const auto owner = readJson(shared + "/project.json");
        if (owner.value("app") != "vividcode" || owner.value("schema").toInt() != 1 ||
            QFileInfo(owner.value("projectRoot").toString()).canonicalFilePath().compare(root, Qt::CaseInsensitive)) fail(QStringLiteral("Shared cache ownership mismatch."));
    }
    const QString cache = shared + "/java"; plain(cache); plain(cache + "/config.json"); plain(cache + "/index.json");
    return {root, shared, cache};
}
class Lifecycle {
    HANDLE handle = INVALID_HANDLE_VALUE;
public:
    explicit Lifecycle(const QString &root) {
        const QString path = root + "/.vividcode-semantic.lock"; plain(path);
        handle = CreateFileW(reinterpret_cast<LPCWSTR>(path.utf16()), GENERIC_READ | GENERIC_WRITE | DELETE, 0, nullptr, OPEN_ALWAYS, FILE_FLAG_DELETE_ON_CLOSE, nullptr);
        if (handle == INVALID_HANDLE_VALUE) fail(QStringLiteral("Another indexing, generation or deletion operation is running."));
    }
    ~Lifecycle() { if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle); }
};
struct Inputs { QJsonObject hashes; QStringList sources, options; };
Inputs inputs(const QString &root) {
    Inputs result;
    const auto policy = VividCode::readPolicy(root);
    const QString configPath = root + "/vividcode.java.json";
    const auto config = readJson(configPath, true);
    if (QFileInfo::exists(configPath)) {
        if (config.value("schema").toInt() != 1) fail(QStringLiteral("vividcode.java.json requires schema: 1."));
        const QSet<QString> allowed{"schema", "sourceRoots", "classpath", "modulePath", "release"};
        for (auto it = config.begin(); it != config.end(); ++it) if (!allowed.contains(it.key())) fail(QStringLiteral("Unknown Java setting: ") + it.key());
        result.hashes[configPath] = digestFile(configPath);
    }
    const auto paths = [&](const QString &key) {
        QStringList values;
        if (!config.contains(key)) return values;
        if (!config.value(key).isArray()) fail(key + QStringLiteral(" must be an array of paths."));
        for (const auto &v : config.value(key).toArray()) {
            if (!v.isString() || v.toString().trimmed().isEmpty()) fail(key + QStringLiteral(" contains an empty path or non-string value."));
            const QString path = QDir::cleanPath(QDir(root).absoluteFilePath(v.toString())); plain(path);
            if (!QFileInfo::exists(path)) fail(QStringLiteral("Java configuration path does not exist: ") + path);
            values.append(path);
        }
        return values;
    };
    QStringList sourceRoots = paths("sourceRoots");
    if (sourceRoots.isEmpty() && config.contains("sourceRoots")) fail(QStringLiteral("sourceRoots cannot be empty."));
    if (sourceRoots.isEmpty()) sourceRoots.append(root);
    for (const auto &path : sourceRoots) if (!QFileInfo(path).isDir() || (path.compare(root, Qt::CaseInsensitive) && !path.startsWith(root + '/', Qt::CaseInsensitive))) fail(QStringLiteral("Source roots must be directories within the project: ") + path);
    const QSet<QString> excluded{".git", ".vs", ".codegraph", ".codemap", ".vividcode", "build", "out", "x64", "x86", "debug", "release", "node_modules", "vendor", "third_party", "_deps", "cmakefiles", "bin", "obj", "__pycache__", ".venv", "venv", "env", ".pytest_cache", ".mypy_cache", ".tox", ".idea", "dist", "target", ".gradle", ".mvn", "coverage", ".next", ".nuxt", ".cache", ".parcel-cache"};
    QStringList pending{root}; qint64 visited=0; QSet<QString> found;
    while (!pending.isEmpty()) {
        const QString path=pending.takeLast(); plain(path);
        if (!QFileInfo(path).isReadable()) fail(QStringLiteral("Directory is not readable: ") + path);
        for (const auto &entry : QDir(path).entryInfoList(QDir::Files|QDir::Dirs|QDir::NoDotAndDotDot|QDir::Hidden, QDir::Name)) {
            if (!policy.force && ++visited>policy.maxEntries()) fail(QStringLiteral("Java scan exceeded the directory limit. Previous graph retained."));
            if (entry.isSymLink()||entry.isJunction()) continue;
            const QString name=entry.fileName().toLower(), file=entry.absoluteFilePath();
            if (entry.isDir()) {if (!excluded.contains(name)&&!name.startsWith(".vividcode-")&&!name.startsWith("build-")&&!name.startsWith("cmake-build-")) pending.append(file);}
            else if (entry.suffix().compare("java", Qt::CaseInsensitive)==0) {
                bool selected=false; for (const auto &source : sourceRoots) selected |= file.startsWith(source+'/',Qt::CaseInsensitive);
                if (!selected) continue;
                if (!policy.force && (entry.size()>2*1024*1024 || found.size()>=policy.maxFiles)) fail(QStringLiteral("Java source exceeded the read limits. Previous graph retained."));
                found.insert(file); result.hashes[file]=digestFile(file);
            }
        }
    }
    result.sources=found.values(); result.sources.sort();
    // Explicit source files are the entire input. Prevent javac from implicitly
    // reading source in classpath directories or outside the selected roots.
    const bool namedModule = std::any_of(result.sources.cbegin(), result.sources.cend(), [](const QString &s) { return QFileInfo(s).fileName()=="module-info.java"; });
    result.options << "--source-path" << (namedModule ? sourceRoots.join(QDir::listSeparator()) : QString());
    for (const auto &key : {QString("classpath"),QString("modulePath")}) {
        const auto entries=paths(key);
        for (const auto &entry : entries) {
            if (QFileInfo(entry).isFile()) {
                if (QFileInfo(entry).suffix().compare("jar",Qt::CaseInsensitive)) fail(QStringLiteral("Dependency files must be JARs: ")+entry);
                result.hashes[entry]=digestFile(entry);
            } else {
                QStringList dirs{entry};
                while (!dirs.isEmpty()) {
                    const auto dir=dirs.takeLast(); plain(dir);
                    if (!QFileInfo(dir).isReadable()) fail(QStringLiteral("Dependency directory is not readable: ")+dir);
                    for (const auto &file : QDir(dir).entryInfoList(QDir::Files|QDir::Dirs|QDir::NoDotAndDotDot|QDir::Hidden)) {
                        if (!policy.force && ++visited>policy.maxEntries()) fail(QStringLiteral("Java dependency directories exceeded the scan limit."));
                        plain(file.absoluteFilePath());
                        if (file.isDir()) dirs.append(file.absoluteFilePath());
                        else if (QSet<QString>{"class","jar","jmod"}.contains(file.suffix().toLower())) result.hashes[file.absoluteFilePath()]=digestFile(file.absoluteFilePath());
                    }
                }
            }
        }
        // Always set an empty classpath to avoid inheriting the current folder or CLASSPATH.
        if (key=="classpath" || !entries.isEmpty()) result.options << (key=="classpath"?"--class-path":"--module-path") << entries.join(QDir::listSeparator());
        if (key=="modulePath" && !entries.isEmpty() && !namedModule) result.options << "--add-modules" << "ALL-MODULE-PATH";
    }
    if (config.contains("release")) {
        const QString release=config.value("release").toString();
        if (!QRegularExpression("^[0-9]+$").match(release).hasMatch()) fail(QStringLiteral("release must be a Java version string, such as 17."));
        result.options << "--release" << release;
    }
    return result;
}
QJsonObject worker(const Layout &where,const Inputs &input,const QString &java,const QString &script) {
    QTemporaryFile request(where.cache+"/request-XXXXXX.txt");
    if (!request.open()) fail(QStringLiteral("Could not create the Java request file."));
    QByteArray bytes=where.root.toUtf8().toBase64()+'\n';
    for (const auto &option : input.options) bytes += "O\t"+option.toUtf8().toBase64()+'\n';
    for (const auto &file : input.sources) bytes += "F\t"+file.toUtf8().toBase64()+'\n';
    if (request.write(bytes)!=bytes.size()||!request.flush()) fail(QStringLiteral("Could not write the Java request."));
    const QString requestPath=request.fileName(); request.close();
    HANDLE job=CreateJobObjectW(nullptr,nullptr); if (!job) fail(QStringLiteral("Could not create Java cancellation protection."));
    struct Close {HANDLE h;~Close(){if(h) CloseHandle(h);}} guard{job};
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{}; limits.BasicLimitInformation.LimitFlags=JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!SetInformationJobObject(job,JobObjectExtendedLimitInformation,&limits,sizeof(limits))) fail(QStringLiteral("Could not configure Java cancellation protection."));
    QProcess process; auto env=QProcessEnvironment::systemEnvironment();
    for (const auto &key : {"JAVA_TOOL_OPTIONS","_JAVA_OPTIONS","JDK_JAVA_OPTIONS","CLASSPATH"}) env.remove(key);
    process.setProcessEnvironment(env); process.setWorkingDirectory(where.cache);
    process.start(java,{"-Xmx1536m","-Dfile.encoding=UTF-8","-Dstdout.encoding=UTF-8","-Dstderr.encoding=UTF-8","--add-modules","jdk.compiler",script,requestPath});
    if (!process.waitForStarted(5000)) fail(QStringLiteral("Could not start local Java: ")+process.errorString());
    HANDLE child=OpenProcess(PROCESS_SET_QUOTA|PROCESS_TERMINATE,FALSE,static_cast<DWORD>(process.processId()));
    const bool attached=child&&AssignProcessToJobObject(job,child);if(child) CloseHandle(child);
    if(!attached){process.kill();process.waitForFinished(5000);fail(QStringLiteral("Could not protect the Java child process."));}
    QByteArray output,errors;QElapsedTimer timer;timer.start();
    while(process.state()!=QProcess::NotRunning) {
        process.waitForFinished(100);output+=process.readAllStandardOutput();errors+=process.readAllStandardError();
        if(output.size()>limit||errors.size()>limit||timer.elapsed()>180000){process.kill();process.waitForFinished(5000);fail(QStringLiteral("Java analysis exceeded the time or result size limit. Previous graph retained."));}
    }
    output+=process.readAllStandardOutput();errors+=process.readAllStandardError();
    if(process.exitStatus()!=QProcess::NormalExit||process.exitCode()) fail(QString::fromUtf8(errors.left(16000)));
    QJsonParseError error;const auto doc=QJsonDocument::fromJson(output,&error);
    if(output.size()>limit||error.error!=QJsonParseError::NoError||!doc.isObject()) fail(QStringLiteral("Invalid Java analysis results."));
    return doc.object();
}
int indexProject(Layout where) {
    Lifecycle lock(where.root);where=layout(where.root);
    if(!QDir().mkpath(where.cache)) fail(QStringLiteral("Could not create the Java cache."));
    atomic(where.shared+"/project.json",{{"app","vividcode"},{"schema",1},{"projectRoot",where.root}});
    QJsonObject state{{"schema",1},{"language","java"},{"project_root",where.root},{"last_status","running"}};atomic(where.cache+"/config.json",state);
    try {
        const QString java=VividCode::localJava(),script=VividCode::sharedRuntime()+"/java-analyzer/VividCodeJava.java";plain(script);
        const QString home=QFileInfo(java).absolutePath()+"/..";
        const auto policy=VividCode::readPolicy(where.root);
        const QString signature="java-native-1:"+QString::number(policy.maxFiles)+':'+QString::number(policy.force)+':'+digestFile(QCoreApplication::applicationFilePath())+':'+java+':'+digestFile(java)+':'+digestFile(home+"/release")+':'+digestFile(home+"/lib/modules")+':'+digestFile(script);
        const auto current=inputs(where.root);auto payload=readJson(where.cache+"/index.json",true);
        const bool reused=payload.value("version")==signature&&payload.value("inventory")==current.hashes;
        emitJson({{"type","progress"},{"current",0},{"total",current.sources.size()},{"file",QStringLiteral("Check the JDK, source code and explicitly configured dependencies")}});
        if(!reused) payload=worker(where,current,java,script);
        const auto finalPolicy=VividCode::readPolicy(where.root);
        if(inputs(where.root).hashes!=current.hashes||finalPolicy.force!=policy.force||finalPolicy.maxFiles!=policy.maxFiles) fail(QStringLiteral("Inputs or read policy changed during analysis. Previous graph retained; refresh to retry."));
        if(!payload.value("units").isArray()) fail(QStringLiteral("Invalid Java snapshot format."));
        payload["version"]=signature;payload["inventory"]=current.hashes;
        state["last_status"]="ok";state["parser"]=payload.value("parser");state["java"]=java;state["total"]=current.sources.size();
        state["indexed"]=reused?0:current.sources.size();state["reused"]=reused?current.sources.size():0;
        state["message"]=QStringLiteral("Java: JDK semantic analysis. Virtual methods are possible calls; method references do not imply execution. Configure source roots, classpath/modulePath/release in vividcode.java.json. Builds and annotation processors are not executed. Dependency changes trigger a full reindex.");
        atomic(where.cache+"/index.json",payload);atomic(where.cache+"/config.json",state);state["type"]="result";state["ok"]=true;emitJson(state);return 0;
    } catch(const std::exception &e) {
        state["last_status"]="failed";state["message"]=QString::fromUtf8(e.what());atomic(where.cache+"/config.json",state);state["type"]="result";state["ok"]=false;emitJson(state);return 1;
    }
}
}
int runJavaIndexer(const QStringList &arguments) {
    try {
        const int i=arguments.indexOf("--project");if(i<0||i+1>=arguments.size()) fail(QStringLiteral("Missing --project argument."));
        auto where=layout(arguments.at(i+1));
        if(arguments.value(2)=="index") return indexProject(where);
        if(arguments.value(2)!="export") fail(QStringLiteral("Java protocol supports index/export only."));
        Lifecycle lock(where.root);where=layout(where.root);auto state=readJson(where.cache+"/config.json",true);
        if(state.value("last_status")=="running"){state["last_status"]="interrupted";state["message"]=QStringLiteral("Previous Java analysis was interrupted. Previous graph retained; press F5 to retry.");}
        const auto snapshot=readJson(where.cache+"/index.json",true);
        emitJson({{"ok",true},{"schema",1},{"language","java"},{"state",state},{"units",snapshot.value("units").toArray()}});return 0;
    } catch(const std::exception &e){emitJson({{"type","result"},{"ok",false},{"message",QString::fromUtf8(e.what())}});return 1;}
}
