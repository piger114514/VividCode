#include "JavaScriptIndexer.h"
#include "AnalysisEnvironment.h"
#include "ProjectScanner.h"
#include <QtCore>
#include <stdexcept>

namespace {
constexpr qint64 limit = 128 * 1024 * 1024;
void fail(const QString &text) { throw std::runtime_error(text.toStdString()); }
void plain(const QString &path) {
    for (QString current = QFileInfo(path).absoluteFilePath();;) {
        const QFileInfo info(current); if (info.isSymLink() || info.isJunction()) fail(QStringLiteral("拒绝链接缓存或输入：") + current);
        const QString parent = info.absolutePath(); if (parent == current) break; current = parent;
    }
}
QJsonObject readJson(const QString &path, bool optional = false) {
    plain(path); QFile file(path); if (optional && !file.exists()) return {};
    if (!file.open(QIODevice::ReadOnly) || file.size() > limit) fail(QStringLiteral("无法读取 JSON：") + path);
    QJsonParseError error; const auto doc = QJsonDocument::fromJson(file.readAll(), &error);
    if (error.error != QJsonParseError::NoError || !doc.isObject()) fail(QStringLiteral("JSON 损坏：") + path);
    return doc.object();
}
void atomic(const QString &path, const QJsonObject &object) {
    plain(path); const auto bytes = QJsonDocument(object).toJson(QJsonDocument::Compact);
    if (bytes.size() > limit) fail(QStringLiteral("JS/TS 缓存超过 128 MiB。"));
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit()) fail(QStringLiteral("缓存写入失败：") + file.errorString());
}
QString digestFile(const QString &path) {
    QFile file(path); QCryptographicHash hash(QCryptographicHash::Sha256);
    if (!file.open(QIODevice::ReadOnly) || !hash.addData(&file) || file.error() != QFileDevice::NoError) fail(QStringLiteral("无法读取依赖：") + path);
    return QString::fromLatin1(hash.result().toHex());
}
void emitJson(QJsonObject object) {
    object["protocol"] = 1; QTextStream out(stdout); out.setEncoding(QStringConverter::Utf8);
    out << QJsonDocument(object).toJson(QJsonDocument::Compact) << '\n'; out.flush();
}
struct Layout { QString root, shared, cache; };
Layout layout(const QString &project) {
    const QFileInfo info(project); if (!info.isDir() || info.fileName().compare(".vividcode", Qt::CaseInsensitive) == 0) fail(QStringLiteral("请选择项目根目录。"));
    const QString root = info.canonicalFilePath(); plain(root + "/.vividcode");
    const auto manifest = readJson(root + "/.vividcode/project.json");
    if (manifest.value("app") != "vividcode" || manifest.value("schema").toInt() != 1) fail(QStringLiteral("项目尚未初始化或 project.json 不兼容。"));
    const QString shared = VividCode::semanticDirectory(root); plain(shared);
    if (QFileInfo::exists(shared)) {
        for (const auto &entry : QDir(shared).entryInfoList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden)) plain(entry.absoluteFilePath());
        const auto owner = readJson(shared + "/project.json");
        if (owner.value("app") != "vividcode" || owner.value("schema").toInt() != 1 ||
            QFileInfo(owner.value("projectRoot").toString()).canonicalFilePath().compare(root, Qt::CaseInsensitive)) fail(QStringLiteral("共享缓存归属不匹配。"));
    }
    const QString cache = shared + "/javascript"; plain(cache); plain(cache + "/config.json"); plain(cache + "/index.json");
    return {root, shared, cache};
}
class Lifecycle {
    HANDLE handle = INVALID_HANDLE_VALUE;
public:
    explicit Lifecycle(const QString &root) {
        const QString path = root + "/.vividcode-semantic.lock"; plain(path);
        handle = CreateFileW(reinterpret_cast<LPCWSTR>(path.utf16()), GENERIC_READ | GENERIC_WRITE | DELETE, 0, nullptr, OPEN_ALWAYS, FILE_FLAG_DELETE_ON_CLOSE, nullptr);
        if (handle == INVALID_HANDLE_VALUE) fail(QStringLiteral("另一个索引或生成/删除操作正在进行。"));
    }
    ~Lifecycle() { if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle); }
};
QJsonObject inventory(const QString &root) {
    const auto policy = VividCode::readPolicy(root);
    const QSet<QString> extensions{"js", "jsx", "mjs", "cjs", "ts", "tsx", "mts", "cts"};
    const QSet<QString> excluded{".git", ".vs", ".codegraph", ".codemap", ".vividcode", "build", "out", "x64", "x86", "debug", "release", "node_modules", "vendor", "third_party", "_deps", "cmakefiles", "bin", "obj", "__pycache__", ".venv", "venv", "env", ".pytest_cache", ".mypy_cache", ".tox", ".idea", "dist", "target", ".gradle", ".mvn", "coverage", ".next", ".nuxt", ".cache", ".parcel-cache"};
    QJsonObject hashes; QStringList pending{root}; qint64 visited = 0, sources = 0;
    while (!pending.isEmpty()) {
        const QString dir = pending.takeLast(); plain(dir);
        const QDir directory(dir); if (!QFileInfo(dir).isReadable()) fail(QStringLiteral("目录不可读：") + dir);
        for (const auto &entry : directory.entryInfoList(QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot | QDir::Hidden)) {
            if (!policy.force && ++visited > policy.maxEntries()) fail(QStringLiteral("目录条目超过扫描上限；保留旧图。"));
            if (entry.isSymLink() || entry.isJunction()) continue;
            const QString name = entry.fileName().toLower();
            if (entry.isDir()) { if (!excluded.contains(name) && !name.startsWith(".vividcode-") && !name.startsWith("build-") && !name.startsWith("cmake-build-")) pending.append(entry.absoluteFilePath()); }
            else if (entry.isFile()) {
                const bool source = extensions.contains(entry.suffix().toLower());
                if (source || QSet<QString>{"tsconfig.json", "jsconfig.json", "package.json"}.contains(name)) {
                    if (!policy.force && entry.size() > 2 * 1024 * 1024) fail(QStringLiteral("文件超过 2 MiB：") + entry.absoluteFilePath());
                    hashes[QDir(root).relativeFilePath(entry.absoluteFilePath())] = digestFile(entry.absoluteFilePath());
                    if (source && !policy.force && ++sources > policy.maxFiles) fail(QStringLiteral("JS/TS 文件超过全局数量上限；保留旧图。"));
                }
            }
        }
    }
    return hashes;
}
bool unchanged(const QJsonObject &deps) {
    for (auto it = deps.begin(); it != deps.end(); ++it) {
        plain(it.key()); const QFileInfo info(it.key());
        if (it.value().isNull()) { if (info.exists()) return false; }
        else if (it.value() == "!dir") { if (!info.isDir()) return false; }
        else if (it.value().toString().startsWith("!dirs:")) {
            if (!info.isDir()) return false;
            QJsonArray names; auto dirs = QDir(it.key()).entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot | QDir::Hidden);
            QStringList sorted; for (const auto &dir : dirs) if (!dir.isSymLink() && !dir.isJunction()) sorted.append(dir.fileName());
            sorted.sort(Qt::CaseSensitive); for (const auto &name : sorted) names.append(name);
            const auto hash = QCryptographicHash::hash(QJsonDocument(names).toJson(QJsonDocument::Compact), QCryptographicHash::Sha256).toHex();
            if (it.value().toString() != "!dirs:" + QString::fromLatin1(hash)) return false;
        } else if (!info.isFile() || digestFile(it.key()) != it.value().toString()) return false;
    }
    return true;
}
QJsonObject worker(const Layout &where, const QString &node, const QString &script) {
    HANDLE job = CreateJobObjectW(nullptr, nullptr); if (!job) fail(QStringLiteral("无法创建 JS/TS 取消保护。"));
    struct Close { HANDLE h; ~Close() { if (h) CloseHandle(h); } } guard{job};
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{}; limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof(limits))) fail(QStringLiteral("无法配置 JS/TS 取消保护。"));
    QProcess process; auto env = QProcessEnvironment::systemEnvironment(); env.remove("NODE_OPTIONS"); env.remove("NODE_PATH"); process.setProcessEnvironment(env);
    const auto policy = VividCode::readPolicy(where.root);
    const QJsonObject settings{{"force", policy.force}, {"maxSourceFiles", policy.maxFiles}, {"maxEntries", policy.maxEntries()}};
    process.start(node, {"--max-old-space-size=1536", script, where.root, QString::fromUtf8(QJsonDocument(settings).toJson(QJsonDocument::Compact))});
    if (!process.waitForStarted(5000)) fail(QStringLiteral("本机 Node 无法启动：") + process.errorString());
    HANDLE child = OpenProcess(PROCESS_SET_QUOTA | PROCESS_TERMINATE, FALSE, static_cast<DWORD>(process.processId()));
    const bool attached = child && AssignProcessToJobObject(job, child); if (child) CloseHandle(child);
    if (!attached) { process.kill(); process.waitForFinished(5000); fail(QStringLiteral("无法保护 Node 子进程；未继续分析。")); }
    QByteArray output, errors; QElapsedTimer timer; timer.start();
    while (process.state() != QProcess::NotRunning) {
        process.waitForFinished(100); output += process.readAllStandardOutput(); errors += process.readAllStandardError();
        if (output.size() > limit || errors.size() > limit || timer.elapsed() > 180000) {
            process.kill(); process.waitForFinished(5000); fail(QStringLiteral("JS/TS 分析超过时间或结果上限；保留旧图。"));
        }
    }
    output += process.readAllStandardOutput(); errors += process.readAllStandardError();
    if (process.exitStatus() != QProcess::NormalExit || process.exitCode()) fail(QString::fromUtf8(errors.left(16000)));
    QJsonParseError error; const auto doc = QJsonDocument::fromJson(output, &error);
    if (output.size() > limit || error.error != QJsonParseError::NoError || !doc.isObject()) fail(QStringLiteral("JS/TS 分析结果无效。"));
    return doc.object();
}
int indexProject(Layout where) {
    Lifecycle lock(where.root); where = layout(where.root);
    if (!QDir().mkpath(where.cache)) fail(QStringLiteral("无法创建 JS/TS 缓存。"));
    atomic(where.shared + "/project.json", {{"app", "vividcode"}, {"schema", 1}, {"projectRoot", where.root}});
    QJsonObject state{{"schema", 1}, {"language", "javascript"}, {"project_root", where.root}, {"last_status", "running"}};
    atomic(where.cache + "/config.json", state);
    try {
        const QString node = VividCode::localNode(), base = VividCode::sharedRuntime() + "/javascript-analyzer";
        const QString script = base + "/indexer.cjs", compiler = base + "/typescript/lib/typescript.js"; plain(script); plain(compiler);
        const auto policy = VividCode::readPolicy(where.root);
        const QString signature = "javascript-native-2:" + QString::number(policy.maxFiles) + ':' + QString::number(policy.force) + ':' + digestFile(QCoreApplication::applicationFilePath()) + ':' + digestFile(node) + ':' + digestFile(script) + ':' + digestFile(compiler);
        const auto current = inventory(where.root); auto payload = readJson(where.cache + "/index.json", true);
        const bool reused = payload.value("version") == signature && payload.value("inventory") == current && unchanged(payload.value("dependencies").toObject());
        emitJson({{"type", "progress"}, {"current", 0}, {"total", current.size()}, {"file", QStringLiteral("检查本机 Node、工程配置和模块依赖")}});
        if (!reused) payload = worker(where, node, script);
        if (inventory(where.root) != current || !unchanged(payload.value("dependencies").toObject())) fail(QStringLiteral("分析期间源码或配置/依赖变化，请按 F5 重试；保留旧图。"));
        const auto finalPolicy = VividCode::readPolicy(where.root);
        if (finalPolicy.maxFiles != policy.maxFiles || finalPolicy.force != policy.force) fail(QStringLiteral("分析期间读取范围设置变化，请刷新重试；保留旧图。"));
        if (!payload.value("units").isArray() || !payload.value("dependencies").isObject()) fail(QStringLiteral("分析快照格式错误。"));
        payload["version"] = signature; payload["inventory"] = current;
        QSet<QString> files; for (const auto &u : payload.value("units").toArray()) files.insert(u.toObject().value("file").toString());
        state["last_status"] = "ok"; state["parser"] = payload.value("parser"); state["node"] = node;
        state["total"] = files.size(); state["indexed"] = reused ? 0 : files.size(); state["reused"] = reused ? files.size() : 0;
        state["unresolved"] = payload.value("unresolved"); state["warnings"] = payload.value("warnings"); state["analysis"] = "static";
        QString message = QStringLiteral("JS/TS 静态分析：使用本机 Node，读取 tsconfig/jsconfig 和模块依赖，不执行项目代码。实例/属性和 JSX 组件为可能调用；动态/外部目标保留未解析证据。依赖变化时当前整工程重建。共 %1 处未解析关系。").arg(payload.value("unresolved").toInt());
        for (const auto &warning : payload.value("warnings").toArray()) message += '\n' + warning.toString(); state["message"] = message;
        atomic(where.cache + "/index.json", payload); atomic(where.cache + "/config.json", state);
        state["type"] = "result"; state["ok"] = true; emitJson(state); return 0;
    } catch (const std::exception &e) {
        state["last_status"] = "failed"; state["message"] = QString::fromUtf8(e.what()); atomic(where.cache + "/config.json", state);
        state["type"] = "result"; state["ok"] = false; emitJson(state); return 1;
    }
}
}
int runJavaScriptIndexer(const QStringList &arguments) {
    try {
        const int projectIndex = arguments.indexOf("--project");
        if (projectIndex < 0 || projectIndex + 1 >= arguments.size()) fail(QStringLiteral("缺少 --project 参数。"));
        auto where = layout(arguments.at(projectIndex + 1));
        if (arguments.value(2) == "index") return indexProject(where);
        if (arguments.value(2) != "export") fail(QStringLiteral("JS/TS 协议只支持 index/export。"));
        Lifecycle lock(where.root); where = layout(where.root);
        auto state = readJson(where.cache + "/config.json", true);
        if (state.value("last_status") == "running") { state["last_status"] = "interrupted"; state["message"] = QStringLiteral("上次分析未完成，保留旧图；按 F5 重试。"); }
        const auto snapshot = readJson(where.cache + "/index.json", true);
        emitJson({{"ok", true}, {"schema", 1}, {"language", "javascript"}, {"state", state}, {"units", snapshot.value("units").toArray()}}); return 0;
    } catch (const std::exception &e) { emitJson({{"type", "result"}, {"ok", false}, {"message", QString::fromUtf8(e.what())}}); return 1; }
}
