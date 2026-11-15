#include "ProjectStore.h"
#include "ProjectScanner.h"
#include "SharedRuntime.h"
#include "GenerationControl.h"
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

QJsonObject readJson(const QString &path, bool largeRecord = false)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly) || (!largeRecord && file.size() > 16 * 1024 * 1024))
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
    const auto record = readJson(directory + "/records/" + id + "/index.json", true);
    if (record.value("app") != "vividcode" || record.value("id") != id ||
        !record.value("files").isArray() || !record.value("fingerprint").isString())
        fail(QStringLiteral("记录内容无效：") + id);
    return record;
}

QString recordSnapshot(StoreAccess &store, QJsonObject manifest, const VividCode::ReadPolicy &policy, const std::function<void()> &checkpoint = {},
    const std::function<void(qint64, qint64)> &progress = {})
{
    const ScanResult scan = scanProject(store.root, policy, checkpoint);
    if (!scan.error.isEmpty()) fail(scan.error);
    if (scan.truncated) fail(QStringLiteral("扫描达到上限。可用 vividcode -c 更改全局文件数量上限，或谨慎使用 vividcode -sg。"));
    QJsonArray files;
    if (progress) progress(0, scan.files.size());
    for (const auto &relative : scan.files) {
        if (checkpoint) checkpoint();
        const QString path = QDir(store.root).filePath(relative);
        if (isLink(path)) fail(QStringLiteral("源码文件在扫描后变成链接：") + path);
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly)) fail(QStringLiteral("源码读取失败：") + relative);
        QCryptographicHash hash(QCryptographicHash::Sha256);
        const qint64 size = file.size();
        while (!file.atEnd()) {
            if (checkpoint) checkpoint();
            const auto chunk = file.read(1024 * 1024);
            if (file.error() != QFileDevice::NoError) fail(QStringLiteral("源码读取失败：") + relative);
            hash.addData(chunk);
        }
        if (file.error() != QFileDevice::NoError || file.size() != size)
            fail(QStringLiteral("源码读取失败或扫描期间发生变化，请重试：") + relative);
        const QString digest = QString::fromLatin1(hash.result().toHex());
        files.append(QJsonObject{{"path", relative}, {"bytes", size}, {"sha256", digest}});
        if (progress) progress(files.size(), scan.files.size());
    }
    const QString fingerprint = QString::fromLatin1(QCryptographicHash::hash(
        QJsonDocument(files).toJson(QJsonDocument::Compact), QCryptographicHash::Sha256).toHex());
    if (checkpoint) checkpoint();
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

void copyEntry(const QString &source, const QString &target, const std::function<void()> &checkpoint = {})
{
    if (checkpoint) checkpoint();
    if (isLink(source)) fail(QStringLiteral("拒绝复制链接：") + source);
    if (QFileInfo(source).isDir()) {
        if (!QDir().mkpath(target)) fail(QStringLiteral("无法创建运行目录：") + target);
        for (const auto &entry : QDir(source).entryInfoList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System))
            copyEntry(entry.absoluteFilePath(), QDir(target).filePath(entry.fileName()), checkpoint);
        return;
    }
    QFile input(source);
    QSaveFile output(target);
    if (!input.open(QIODevice::ReadOnly) || !output.open(QIODevice::WriteOnly))
        fail(QStringLiteral("无法复制运行文件：") + source + " -> " + target);
    while (!input.atEnd()) {
        if (checkpoint) checkpoint();
        const QByteArray chunk = input.read(1024 * 1024);
        if (input.error() != QFileDevice::NoError || output.write(chunk) != chunk.size())
            fail(QStringLiteral("运行文件复制失败：") + source);
    }
    if (!output.commit()) fail(QStringLiteral("无法更新运行文件，请先关闭查看器：") + target);
    QFile saved(target);
    if (!saved.open(QIODevice::ReadWrite) || !saved.setFileTime(QFileInfo(source).lastModified(), QFileDevice::FileModificationTime))
        fail(QStringLiteral("无法保留文件修改时间：") + target);
    saved.close();
#ifdef Q_OS_WIN
    const DWORD attributes = GetFileAttributesW(reinterpret_cast<LPCWSTR>(source.utf16()));
    if (attributes == INVALID_FILE_ATTRIBUTES || !SetFileAttributesW(reinterpret_cast<LPCWSTR>(target.utf16()), attributes))
        fail(QStringLiteral("无法保留文件属性：") + target);
#endif
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
void migrateSemantic(StoreAccess &store, const QString &preparedCache, const std::function<void()> &checkpoint) {
    const QString source = store.directory + "/semantic";
    if (!QFileInfo::exists(source)) return;
    const QString cache = preparedCache;
    const auto entries = QDir(source).entryInfoList(QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot | QDir::Hidden);
    const QSet<QString> allowed{"config.json", "index.db", "index.db-journal", "index.db-wal", "index.db-shm"};
    // Preflight all files before copying, and never overwrite a different shared snapshot.
    for (const auto &entry : entries) {
        checkpoint();
        if (!entry.isFile() || !allowed.contains(entry.fileName()))
            fail(QStringLiteral("旧语义目录含未知文件，已保留：") + entry.absoluteFilePath());
        const QString target = cache + '/' + entry.fileName();
        if (QFileInfo::exists(target) && fileDigest(target) != fileDigest(entry.absoluteFilePath()))
            fail(QStringLiteral("旧索引与共享索引不同，已保留两份数据，请先解决冲突。"));
    }
    for (const auto &entry : entries) {
        const QString target = cache + '/' + entry.fileName();
        if (!QFileInfo::exists(target)) copyEntry(entry.absoluteFilePath(), target, checkpoint);
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

struct Publication {
    QString target, stage, previous;
    bool moved = false, published = false;
    explicit Publication(const QString &path) : target(path) {
        const QString id = QUuid::createUuid().toString(QUuid::WithoutBraces);
        stage = path + "-work-" + id; previous = path + "-old-" + id;
    }
    static void erase(const QString &path) {
        checkTree(path);
        if (QFileInfo::exists(path) && !QDir(path).removeRecursively()) fail(QStringLiteral("临时数据清理失败，原项目仍保留：") + path);
    }
    void install() {
        if (QFileInfo::exists(target)) {
            if (!QDir().rename(target, previous)) fail(QStringLiteral("无法暂存原目录，原数据未修改：") + target);
            moved = true;
        }
        if (!QDir().rename(stage, target)) fail(QStringLiteral("无法提交生成结果：") + target);
        published = true;
    }
    void restore() {
        if (published) { erase(target); published = false; }
        if (moved) {
            if (!QDir().rename(previous, target)) fail(QStringLiteral("恢复原目录失败，原数据保存在：") + previous);
            moved = false;
        }
    }
};

template<class Function> VividCode::Result attempt(Function operation)
{
    try { return {true, operation()}; }
    catch (const std::exception &error) { return {false, QString::fromUtf8(error.what())}; }
}
}

namespace VividCode {
Result generate(const QString &projectRoot, bool force, bool consoleInput, const GenerationFeedback &feedback)
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
        const QStringList required = {"vividcode.exe", "vividcode-launcher.exe", "vividcode-viewer.exe", "Qt6Core.dll", "Qt6Gui.dll", "Qt6Widgets.dll",
            "Qt6Sql.dll", "Qt6Network.dll", "tls/qschannelbackend.dll", "sqldrivers/qsqlite.dll", "platforms/qwindows.dll", "installation.json"};
        for (const auto &name : required)
            if (!QFileInfo(runtime.filePath(name)).isFile()) fail(QStringLiteral("共享运行包不完整，请重新运行 install-command.ps1：") + name);
        checkTree(runtime.absolutePath());
        const auto installation = readJson(runtime.filePath("installation.json"));
        if (installation.value("app") != "vividcode" || installation.value("schema").toInt() != 1)
            fail(QStringLiteral("共享安装信息无效，请重新安装。"));
        GenerationControl control(consoleInput);
        if (feedback.progress) feedback.progress(0, -1);
        const auto checkpoint = [&control] { control.checkpoint(); };
        auto policy = readPolicy(store.root); policy.force = force;
        const QString directory = store.directory;
        const bool existed = QFileInfo::exists(directory);
        Publication project(directory), cache(semanticDirectory(store.root));
        bool migrating = false;
        QString recordMessage;
        try {
        QJsonObject manifest;
        if (existed) {
            manifest = loadManifest(directory);
            copyEntry(directory, project.stage, checkpoint);
        }
        else {
            if (!QDir().mkpath(project.stage)) fail(QStringLiteral("无法创建生成临时目录。"));
            manifest = {{"app", "vividcode"}, {"schema", 1}, {"projectRoot", ".."}, {"latest", ""}, {"history", QJsonArray{}}};
        }
        store.directory = project.stage;
        for (const auto &id : manifest.value("history").toArray()) { checkpoint(); loadRecord(store.directory, id.toString()); }
        recordMessage = recordSnapshot(store, manifest, policy, checkpoint, feedback.progress);
        manifest = loadManifest(store.directory);
        if (QFileInfo::exists(store.directory + "/semantic")) {
            const QString existingCache = checkedCache(store.root, false);
            if (QFileInfo::exists(existingCache)) copyEntry(existingCache, cache.stage, checkpoint);
            else {
                checkParents(cache.stage);
                if (!QDir().mkpath(cache.stage)) fail(QStringLiteral("无法准备索引迁移。"));
                saveJson(cache.stage + "/project.json", {{"app", "vividcode"}, {"schema", 1}, {"projectRoot", store.root}});
            }
            migrating = true;
            migrateSemantic(store, cache.stage, checkpoint);
        }
        copyEntry(runtime.filePath("vividcode-launcher.exe"), store.directory + "/vividcode-viewer.exe", checkpoint);
        removeLegacyRuntime(store, manifest);
        manifest["deployment"] = "shared";
        manifest["cacheKey"] = projectCacheKey(store.root);
        manifest["forceRead"] = force;
        saveJson(store.directory + "/project.json", manifest);
        control.seal();
        project.install();
        if (migrating) cache.install();
        } catch (const std::exception &error) {
            const QString message = QString::fromUtf8(error.what());
            try {
                cache.restore(); project.restore();
                Publication::erase(cache.stage); Publication::erase(project.stage);
            } catch (const std::exception &cleanup) {
                fail(message + "\n" + QString::fromUtf8(cleanup.what()));
            }
            if (dynamic_cast<const GenerationCancelled *>(&error))
                fail(existed ? QStringLiteral("已停止生成：原 .vividcode、历史记录及语义索引未修改，本次记录已丢弃。")
                             : QStringLiteral("已停止生成：本次临时目录已清除，没有留下 .vividcode 或记录。"));
            throw;
        }
        QString warning;
        try { Publication::erase(project.previous); Publication::erase(cache.previous); }
        catch (const std::exception &error) {
            warning = "\n" + QString::fromUtf8(error.what());
            if (feedback.warning) feedback.warning(warning.trimmed());
        }
        return recordMessage + QStringLiteral("\n项目启动器已生成：") + QDir::toNativeSeparators(directory + "/vividcode-viewer.exe") + warning;
    });
}

Result setFileLimit(qint64 count) {
    return attempt([&] {
        if (count < 1 || count > 2147483647) fail(QStringLiteral("请输入 1 到 2147483647 之间的源码文件数量。"));
        const QString home = sharedHome(); checkParents(home);
        if (!QDir().mkpath(home)) fail(QStringLiteral("无法创建全局设置目录。"));
        QLockFile lock(checkedLockPath(home, "read-policy.lock"));
        if (!lock.tryLock()) fail(QStringLiteral("另一个源码范围设置正在保存。"));
        const QString path = home + "/read-policy.json";
        saveJson(path, {{"app", "vividcode"}, {"schema", 1}, {"maxSourceFiles", count}});
        return QStringLiteral("全局源码文件数量上限已设为 %1，所有项目下次生成/刷新生效；-sg 可跳过此上限。").arg(count);
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
        const QString aiCache = sharedHome() + "/ai/" + projectCacheKey(store.root);
        checkParents(aiCache); checkTree(aiCache);
        if (QFileInfo::exists(aiCache)) {
            const QString aiLockPath = aiCache + ".cache.lock"; checkParents(aiLockPath);
            QLockFile aiLock(aiLockPath);
            if (!aiLock.tryLock()) fail(QStringLiteral("请先关闭此项目的 AI 查看器，再清理缓存。"));
            const auto owner = readJson(aiCache + "/project.json");
            if (owner.value("app") != "vividcode" || owner.value("schema").toInt() != 1 || owner.value("projectRoot").toString().compare(store.root, Qt::CaseInsensitive))
                fail(QStringLiteral("AI 缓存归属不匹配，未删除：") + aiCache);
            if (!QDir(aiCache).removeRecursively()) fail(QStringLiteral("AI 解释缓存正在使用或不可写，请关闭查看器后重试。"));
        }
        if (QFileInfo::exists(cache) && !QDir(cache).removeRecursively()) fail(QStringLiteral("共享索引清理失败，请关闭分析进程后重试。"));
        // Fixed project directory and cache hash; never use arbitrary manifest paths for deletion.
        if (QFileInfo::exists(store.directory) && !QDir(store.directory).removeRecursively()) fail(QStringLiteral("删除未完成：文件可能被占用或不可写，请关闭查看器后重试。"));
        return QStringLiteral("已清除项目 .vividcode、共享索引和 AI 解释缓存；共享程序与全局设置保留。");
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
        return recordSnapshot(store, loadManifest(store.directory), readPolicy(store.root));
    });
}
}
