#include "ProjectStore.h"
#include "ProjectScanner.h"
#include "SharedRuntime.h"
#include <QtCore>
#include <stdexcept>
#ifdef Q_OS_WIN
#define NOMINMAX
#include <windows.h>
#endif

namespace {
[[noreturn]] void fail(const QString &message)
{
    throw std::runtime_error(message.toUtf8().constData());
}

bool isLink(const QString &path)
{
#ifdef Q_OS_WIN
    const DWORD attributes = GetFileAttributesW(reinterpret_cast<LPCWSTR>(path.utf16()));
    if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_REPARSE_POINT)) return true;
#endif
    return QFileInfo(path).isSymLink();
}

// 删除前检查整棵目标树，拒绝 junction / 符号链接，避免触及目标目录外的数据。
void checkTree(const QString &path, int depth = 0)
{
    if (depth > 128) fail(QStringLiteral("目录嵌套过深，已停止操作：") + path);
    if (isLink(path)) fail(QStringLiteral("拒绝操作符号链接或目录联接：") + path);
    const QFileInfo info(path);
    if (info.isDir()) {
        if (!info.isReadable()) fail(QStringLiteral("无法读取目录：") + path);
        const auto children = QDir(path).entryInfoList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System);
        for (const auto &child : children) checkTree(child.absoluteFilePath(), depth + 1);
    }
}

QString canonicalRoot(const QString &path)
{
    const QFileInfo info(path);
    if (!info.isDir() || info.canonicalFilePath().isEmpty()) fail(QStringLiteral("项目目录不可用：") + path);
    const QString root = info.canonicalFilePath();
    if (QFileInfo(root).fileName().compare(".vividcode", Qt::CaseInsensitive) == 0)
        fail(QStringLiteral("请在项目根目录执行命令，不要在 .vividcode 内执行。"));
    return root;
}

QString checkedLockPath(const QString &root, const QString &name)
{
    const QString path = QDir(root).filePath(name);
    if (isLink(path)) fail(QStringLiteral("锁文件不能是链接：") + path);
    return path;
}

struct StoreAccess {
    QString root;
    QString directory;
    QLockFile lock;
#ifdef Q_OS_WIN
    struct SemanticGuard {
        HANDLE handle = INVALID_HANDLE_VALUE;
        ~SemanticGuard() { if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle); }
    } semanticGuard;
#endif
    explicit StoreAccess(const QString &path)
        : root(canonicalRoot(path)), directory(QDir(root).filePath(".vividcode")),
          lock(checkedLockPath(root, ".vividcode.lock"))
    {
        lock.setStaleLockTime(0);
        if (!lock.tryLock()) fail(QStringLiteral("另一个 vividcode 操作正在进行，或项目目录不可写。"));
#ifdef Q_OS_WIN
        const QString semanticLock = checkedLockPath(root, ".vividcode-semantic.lock");
        semanticGuard.handle = CreateFileW(reinterpret_cast<LPCWSTR>(semanticLock.utf16()), GENERIC_READ | GENERIC_WRITE | DELETE,
            0, nullptr, OPEN_ALWAYS, FILE_FLAG_DELETE_ON_CLOSE, nullptr);
        if (semanticGuard.handle == INVALID_HANDLE_VALUE) fail(QStringLiteral("语义分析正在运行，请取消分析后重试。"));
#endif
        checkTree(directory);
        if (QFileInfo::exists(directory) && !QFileInfo(directory).isDir())
            fail(QStringLiteral(".vividcode 已存在但不是文件夹。"));
    }
};

void saveBytes(const QString &path, const QByteArray &bytes)
{
    if (isLink(path)) fail(QStringLiteral("拒绝覆盖链接：") + path);
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit())
        fail(QStringLiteral("写入失败：") + path + " / " + file.errorString());
}

void saveJson(const QString &path, const QJsonObject &object)
{
    saveBytes(path, QJsonDocument(object).toJson(QJsonDocument::Indented));
}

QJsonObject readJson(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly) || file.size() > 16 * 1024 * 1024)
        fail(QStringLiteral("无法读取记录：") + path);
    QJsonParseError error;
    const auto document = QJsonDocument::fromJson(file.readAll(), &error);
    if (error.error != QJsonParseError::NoError || !document.isObject())
        fail(QStringLiteral("记录 JSON 已损坏：") + path);
    return document.object();
}

bool validId(const QString &id)
{
    static const QRegularExpression pattern("^record-[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$");
    return pattern.match(id).hasMatch();
}

QJsonObject loadManifest(const QString &directory)
{
    const auto manifest = readJson(directory + "/project.json");
    if (manifest.value("app") != "vividcode" || manifest.value("schema").toInt() != 1 ||
        !manifest.value("history").isArray() || !manifest.value("latest").isString())
        fail(QStringLiteral(".vividcode 不是受支持的 vividcode 项目。"));
    QSet<QString> ids;
    for (const auto &value : manifest.value("history").toArray()) {
        const auto id = value.toString();
        if (!validId(id) || ids.contains(id)) fail(QStringLiteral("历史记录 ID 无效或重复，未执行清理。"));
        ids.insert(id);
    }
    const QString latest = manifest.value("latest").toString();
    if ((!latest.isEmpty() && !ids.contains(latest)) || (latest.isEmpty() && !ids.isEmpty()))
        fail(QStringLiteral("最新记录指针无效。"));
    return manifest;
}

QJsonObject loadRecord(const QString &directory, const QString &id)
{
    if (!validId(id)) fail(QStringLiteral("记录 ID 无效。"));
    const auto record = readJson(directory + "/records/" + id + "/index.json");
    if (record.value("app") != "vividcode" || record.value("id") != id ||
        !record.value("files").isArray() || !record.value("fingerprint").isString())
        fail(QStringLiteral("记录内容无效：") + id);
    return record;
}

QString recordSnapshot(StoreAccess &store, QJsonObject manifest)
{
    const ScanResult scan = scanProject(store.root);
    if (scan.truncated) fail(QStringLiteral("扫描达到上限。请选择更小的项目。"));
    QJsonArray files;
    for (const auto &relative : scan.files) {
        const QString path = QDir(store.root).filePath(relative);
        if (isLink(path)) fail(QStringLiteral("源码文件在扫描后变成链接：") + path);
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly)) fail(QStringLiteral("源码读取失败：") + relative);
        QCryptographicHash hash(QCryptographicHash::Sha256);
        const qint64 size = file.size();
        if (!hash.addData(&file) || file.error() != QFileDevice::NoError || file.size() != size)
            fail(QStringLiteral("源码读取失败或扫描期间发生变化，请重试：") + relative);
        const QString digest = QString::fromLatin1(hash.result().toHex());
        files.append(QJsonObject{{"path", relative}, {"bytes", size}, {"sha256", digest}});
    }
    const QString fingerprint = QString::fromLatin1(QCryptographicHash::hash(
        QJsonDocument(files).toJson(QJsonDocument::Compact), QCryptographicHash::Sha256).toHex());
    const QString latest = manifest.value("latest").toString();
    if (!latest.isEmpty() && loadRecord(store.directory, latest).value("fingerprint") == fingerprint)
        return QStringLiteral("未检测到更改代码：") + latest;

    const QString id = "record-" + QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QString recordDirectory = store.directory + "/records/" + id;
    if (!QDir().mkpath(recordDirectory)) fail(QStringLiteral("无法创建记录目录。"));
    const QString timestamp = QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
    const QJsonObject record{{"app", "vividcode"}, {"schema", 1}, {"id", id}, {"createdUtc", timestamp},
        {"fingerprint", fingerprint}, {"files", files}};
    // 先完成 JSON 记录，再原子更新 manifest。未提交记录不会成为“最新”。
    saveJson(recordDirectory + "/index.json", record);
    auto history = manifest.value("history").toArray();
    history.append(id);
    manifest["history"] = history;
    manifest["latest"] = id;
    saveJson(store.directory + "/project.json", manifest);
    return QStringLiteral("已保存最新记录（%1 个文件）：%2").arg(files.size()).arg(id);
}

void copyEntry(const QString &source, const QString &target)
{
    checkTree(source);
    if (QFileInfo(source).isDir()) {
        if (!QDir().mkpath(target)) fail(QStringLiteral("无法创建运行目录：") + target);
        for (const auto &entry : QDir(source).entryInfoList(QDir::AllEntries | QDir::NoDotAndDotDot))
            copyEntry(entry.absoluteFilePath(), QDir(target).filePath(entry.fileName()));
        return;
    }
    QFile input(source);
    QSaveFile output(target);
    if (!input.open(QIODevice::ReadOnly) || !output.open(QIODevice::WriteOnly))
        fail(QStringLiteral("无法复制运行文件：") + source + " -> " + target);
    while (!input.atEnd()) {
        const QByteArray chunk = input.read(1024 * 1024);
        if (input.error() != QFileDevice::NoError || output.write(chunk) != chunk.size())
            fail(QStringLiteral("运行文件复制失败：") + source);
    }
    if (!output.commit()) fail(QStringLiteral("无法更新运行文件，请先关闭查看器：") + target);
}


void checkParents(const QString &path) {
    QString current = QFileInfo(path).absoluteFilePath();
    while (!current.isEmpty()) {
        if (isLink(current)) fail(QStringLiteral("拒绝通过链接访问共享目录：") + current);
        const QString parent = QFileInfo(current).absolutePath();
        if (parent == current) break;
        current = parent;
    }
}
QString checkedCache(const QString &root, bool create) {
    const QString cache = VividCode::semanticDirectory(root);
    checkParents(cache);
    checkTree(cache);
    const QString ownerFile = cache + "/project.json";
    if (QFileInfo::exists(cache)) {
        const auto owner = readJson(ownerFile);
        if (owner.value("app") != "vividcode" || owner.value("schema").toInt() != 1 || owner.value("projectRoot").toString().compare(root, Qt::CaseInsensitive))
            fail(QStringLiteral("共享索引归属不匹配，未修改：") + cache);
    } else if (create) {
        if (!QDir().mkpath(cache)) fail(QStringLiteral("无法创建共享索引目录：") + cache);
        saveJson(ownerFile, {{"app", "vividcode"}, {"schema", 1}, {"projectRoot", root}});
    }
    return cache;
}
QByteArray fileDigest(const QString &path) {
    QFile file(path); QCryptographicHash hash(QCryptographicHash::Sha256);
    if (!file.open(QIODevice::ReadOnly) || !hash.addData(&file) || file.error() != QFileDevice::NoError)
        fail(QStringLiteral("无法校验迁移文件：") + path);
    return hash.result();
}
void migrateSemantic(StoreAccess &store) {
    const QString source = store.directory + "/semantic";
    if (!QFileInfo::exists(source)) return;
    const QString cache = checkedCache(store.root, true);
    const auto entries = QDir(source).entryInfoList(QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot | QDir::Hidden);
    const QSet<QString> allowed{"config.json", "index.db", "index.db-journal", "index.db-wal", "index.db-shm"};
    // Preflight all files before copying, and never overwrite a different shared snapshot.
    for (const auto &entry : entries) {
        if (!entry.isFile() || !allowed.contains(entry.fileName()))
            fail(QStringLiteral("旧语义目录含未知文件，已保留：") + entry.absoluteFilePath());
        const QString target = cache + '/' + entry.fileName();
        if (QFileInfo::exists(target) && fileDigest(target) != fileDigest(entry.absoluteFilePath()))
            fail(QStringLiteral("旧索引与共享索引不同，已保留两份数据，请先解决冲突。"));
    }
    for (const auto &entry : entries) {
        const QString target = cache + '/' + entry.fileName();
        if (!QFileInfo::exists(target)) copyEntry(entry.absoluteFilePath(), target);
        if (fileDigest(target) != fileDigest(entry.absoluteFilePath())) fail(QStringLiteral("索引迁移校验失败，旧文件已保留。"));
    }
    if (!QDir(source).removeRecursively()) fail(QStringLiteral("共享索引已校验，但旧索引清理失败，请重试。"));
}
void removeLegacyRuntime(StoreAccess &store, const QJsonObject &manifest) {
    for (const QString &name : {"analyzer", "demo", "platforms", "styles", "imageformats", "iconengines", "networkinformation", "tls", "generic"}) {
        const QString path = store.directory + '/' + name;
        if (QFileInfo::exists(path) && !QDir(path).removeRecursively()) fail(QStringLiteral("无法清理旧运行目录：") + path);
    }
    for (const auto &dll : QDir(store.directory).entryInfoList({"*.dll"}, QDir::Files))
        if (!QFile::remove(dll.absoluteFilePath())) fail(QStringLiteral("无法清理旧运行依赖：") + dll.fileName());
    QStringList obsolete{store.directory + "/README.md"};
    for (const auto &id : manifest.value("history").toArray()) obsolete << store.directory + "/records/" + id.toString() + "/code.md";
    for (const QString &path : obsolete)
        if (QFileInfo::exists(path) && !QFile::remove(path)) fail(QStringLiteral("无法清理旧文档：") + path);
}

template<class Function> VividCode::Result attempt(Function operation)
{
    try { return {true, operation()}; }
    catch (const std::exception &error) { return {false, QString::fromUtf8(error.what())}; }
}
}

namespace VividCode {
Result generate(const QString &projectRoot)
{
    return attempt([&] {
        StoreAccess store(projectRoot);
        QLockFile viewer(checkedLockPath(store.root, ".vividcode-viewer.lock"));
        viewer.setStaleLockTime(0);
        if (!viewer.tryLock()) fail(QStringLiteral("请先关闭此项目的 vividcode 查看器，再生成或更新程序文件。"));
        const QDir runtime(sharedRuntime());
        checkParents(runtime.absolutePath());
        RuntimeReadLock runtimeLock(sharedHome());
        if (!runtimeLock.valid()) fail(QStringLiteral("共享运行环境未安装或正在更新，请先运行 install-command.ps1。"));
        const QStringList required = {"vividcode-launcher.exe", "vividcode-viewer.exe", "Qt6Core.dll", "Qt6Gui.dll", "Qt6Widgets.dll", "platforms/qwindows.dll",
            "analyzer/analyzer.py", "analyzer/extractor.py", "analyzer/clang_bridge.py", "analyzer/python/python.exe", "analyzer/python/python313.dll",
            "analyzer/python/python313.zip", "analyzer/python/python313._pth", "analyzer/python/_sqlite3.pyd", "analyzer/python/sqlite3.dll", "analyzer/lib/libclang.dll", "analyzer/lib/clang/23/include/stddef.h", "analyzer/runtime.json", "installation.json"};
        for (const auto &name : required)
            if (!QFileInfo(runtime.filePath(name)).isFile()) fail(QStringLiteral("共享运行包不完整，请重新运行 install-command.ps1：") + name);
        checkTree(runtime.absolutePath());
        const auto installation = readJson(runtime.filePath("installation.json"));
        if (installation.value("app") != "vividcode" || installation.value("schema").toInt() != 1)
            fail(QStringLiteral("共享安装信息无效，请重新安装。"));
        QJsonObject manifest;
        if (QFileInfo::exists(store.directory)) manifest = loadManifest(store.directory);
        else {
            if (!QDir().mkpath(store.directory)) fail(QStringLiteral("无法创建 .vividcode。"));
            manifest = {{"app", "vividcode"}, {"schema", 1}, {"projectRoot", ".."}, {"latest", ""}, {"history", QJsonArray{}}};
        }
        for (const auto &id : manifest.value("history").toArray()) loadRecord(store.directory, id.toString());
        const QString result = recordSnapshot(store, manifest);
        manifest = loadManifest(store.directory);
        migrateSemantic(store);
        copyEntry(runtime.filePath("vividcode-launcher.exe"), store.directory + "/vividcode-viewer.exe");
        removeLegacyRuntime(store, manifest);
        manifest["deployment"] = "shared";
        manifest["cacheKey"] = projectCacheKey(store.root);
        saveJson(store.directory + "/project.json", manifest);
        return result + QStringLiteral("\n项目启动器已生成：") + QDir::toNativeSeparators(store.directory + "/vividcode-viewer.exe");
    });
}

Result removeGenerated(const QString &projectRoot)
{
    return attempt([&] {
        StoreAccess store(projectRoot);
        QLockFile viewer(checkedLockPath(store.root, ".vividcode-viewer.lock"));
        viewer.setStaleLockTime(0);
        if (!viewer.tryLock()) fail(QStringLiteral("请先关闭此项目的 vividcode 查看器。"));
        const QString cache = checkedCache(store.root, false);
        if (QFileInfo::exists(cache) && !QDir(cache).removeRecursively()) fail(QStringLiteral("共享索引清理失败，请关闭分析进程后重试。"));
        // Fixed project directory and cache hash; never use arbitrary manifest paths for deletion.
        if (QFileInfo::exists(store.directory) && !QDir(store.directory).removeRecursively()) fail(QStringLiteral("删除未完成：文件可能被占用或不可写，请关闭查看器后重试。"));
        return QStringLiteral("已清除项目 .vividcode 和对应共享索引；共享程序保留。");
    });
}

Result cleanHistory(const QString &projectRoot)
{
    return attempt([&] {
        StoreAccess store(projectRoot);
        if (!QFileInfo::exists(store.directory)) fail(QStringLiteral("当前目录尚未初始化，请先运行 vividcode -g。"));
        auto manifest = loadManifest(store.directory);
        const QString latest = manifest.value("latest").toString();
        if (latest.isEmpty()) fail(QStringLiteral("没有完整的最新记录，请先运行 vividcode -g。"));
        loadRecord(store.directory, latest); // 最新记录不完整时，保留所有历史。
        auto history = manifest.value("history").toArray();
        int removed = 0;
        for (qsizetype i = history.size() - 1; i >= 0; --i) {
            const QString id = history.at(i).toString();
            if (id == latest) continue;
            const QString target = store.directory + "/records/" + id;
            if (QFileInfo::exists(target) && !QDir(target).removeRecursively())
                fail(QStringLiteral("历史清理未完成，可重试：") + id);
            history.removeAt(i);
            manifest["history"] = history;
            saveJson(store.directory + "/project.json", manifest);
            ++removed;
        }
        return QStringLiteral("已清理 %1 份旧记录，保留最新记录：%2").arg(removed).arg(latest);
    });
}

Result recordIfInitialized(const QString &projectRoot)
{
    return attempt([&] {
        if (!QFileInfo::exists(QDir(projectRoot).filePath(".vividcode"))) return QString();
        StoreAccess store(projectRoot);
        return recordSnapshot(store, loadManifest(store.directory));
    });
}
}
