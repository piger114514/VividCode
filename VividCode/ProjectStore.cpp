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
    if (depth > 128) fail(QStringLiteral("Directory nesting limit exceeded: ") + path);
    if (isLink(path)) fail(QStringLiteral("Linked path rejected: ") + path);
    const QFileInfo info(path);
    if (info.isDir()) {
        if (!info.isReadable()) fail(QStringLiteral("Cannot read directory: ") + path);
        const auto children = QDir(path).entryInfoList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System);
        for (const auto &child : children) checkTree(child.absoluteFilePath(), depth + 1);
    }
}

QString canonicalRoot(const QString &path)
{
    const QFileInfo info(path);
    if (!info.isDir() || info.canonicalFilePath().isEmpty()) fail(QStringLiteral("Project directory is unavailable: ") + path);
    const QString root = info.canonicalFilePath();
    if (QFileInfo(root).fileName().compare(".vividcode", Qt::CaseInsensitive) == 0)
        fail(QStringLiteral("Run from the project root, not inside .vividcode."));
    return root;
}

QString checkedLockPath(const QString &root, const QString &name)
{
    const QString path = QDir(root).filePath(name);
    if (isLink(path)) fail(QStringLiteral("Lock file cannot be a link: ") + path);
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
        if (!lock.tryLock()) fail(QStringLiteral("Project is busy or not writable."));
#ifdef Q_OS_WIN
        const QString semanticLock = checkedLockPath(root, ".vividcode-semantic.lock");
        semanticGuard.handle = CreateFileW(reinterpret_cast<LPCWSTR>(semanticLock.utf16()), GENERIC_READ | GENERIC_WRITE | DELETE,
            0, nullptr, OPEN_ALWAYS, FILE_FLAG_DELETE_ON_CLOSE, nullptr);
        if (semanticGuard.handle == INVALID_HANDLE_VALUE) fail(QStringLiteral("Analysis is running. Cancel it and retry."));
#endif
        checkTree(directory);
        if (QFileInfo::exists(directory) && !QFileInfo(directory).isDir())
            fail(QStringLiteral(".vividcode is not a directory."));
    }
};

void saveBytes(const QString &path, const QByteArray &bytes)
{
    if (isLink(path)) fail(QStringLiteral("Cannot overwrite linked path: ") + path);
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit())
        fail(QStringLiteral("Write failed: ") + path + " / " + file.errorString());
}

void saveJson(const QString &path, const QJsonObject &object)
{
    saveBytes(path, QJsonDocument(object).toJson(QJsonDocument::Indented));
}

QJsonObject readJson(const QString &path, bool largeRecord = false)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly) || (!largeRecord && file.size() > 16 * 1024 * 1024))
        fail(QStringLiteral("Cannot read record: ") + path);
    QJsonParseError error;
    const auto document = QJsonDocument::fromJson(file.readAll(), &error);
    if (error.error != QJsonParseError::NoError || !document.isObject())
        fail(QStringLiteral("Invalid record JSON: ") + path);
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
        fail(QStringLiteral("Unsupported .vividcode project."));
    QSet<QString> ids;
    for (const auto &value : manifest.value("history").toArray()) {
        const auto id = value.toString();
        if (!validId(id) || ids.contains(id)) fail(QStringLiteral("Invalid or duplicate history IDs. Nothing cleared."));
        ids.insert(id);
    }
    const QString latest = manifest.value("latest").toString();
    if ((!latest.isEmpty() && !ids.contains(latest)) || (latest.isEmpty() && !ids.isEmpty()))
        fail(QStringLiteral("Invalid latest record reference."));
    return manifest;
}

QJsonObject loadRecord(const QString &directory, const QString &id)
{
    if (!validId(id)) fail(QStringLiteral("Invalid record ID."));
    const auto record = readJson(directory + "/records/" + id + "/index.json", true);
    if (record.value("app") != "vividcode" || record.value("id") != id ||
        !record.value("files").isArray() || !record.value("fingerprint").isString())
        fail(QStringLiteral("Invalid record: ") + id);
    return record;
}

QString recordSnapshot(StoreAccess &store, QJsonObject manifest, const VividCode::ReadPolicy &policy, const std::function<void()> &checkpoint = {},
    const std::function<void(qint64, qint64)> &progress = {})
{
    const ScanResult scan = scanProject(store.root, policy, checkpoint);
    if (!scan.error.isEmpty()) fail(scan.error);
    if (scan.truncated) fail(QStringLiteral("File limit reached. Use vividcode -c or -sg."));
    QJsonArray files;
    if (progress) progress(0, scan.files.size());
    for (const auto &relative : scan.files) {
        if (checkpoint) checkpoint();
        const QString path = QDir(store.root).filePath(relative);
        if (isLink(path)) fail(QStringLiteral("Source became a link: ") + path);
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly)) fail(QStringLiteral("Cannot read source: ") + relative);
        QCryptographicHash hash(QCryptographicHash::Sha256);
        const qint64 size = file.size();
        while (!file.atEnd()) {
            if (checkpoint) checkpoint();
            const auto chunk = file.read(1024 * 1024);
            if (file.error() != QFileDevice::NoError) fail(QStringLiteral("Cannot read source: ") + relative);
            hash.addData(chunk);
        }
        if (file.error() != QFileDevice::NoError || file.size() != size)
            fail(QStringLiteral("Source changed or became unreadable. Retry: ") + relative);
        const QString digest = QString::fromLatin1(hash.result().toHex());
        files.append(QJsonObject{{"path", relative}, {"bytes", size}, {"sha256", digest}});
        if (progress) progress(files.size(), scan.files.size());
    }
    const QString fingerprint = QString::fromLatin1(QCryptographicHash::hash(
        QJsonDocument(files).toJson(QJsonDocument::Compact), QCryptographicHash::Sha256).toHex());
    if (checkpoint) checkpoint();
    const QString latest = manifest.value("latest").toString();
    if (!latest.isEmpty() && loadRecord(store.directory, latest).value("fingerprint") == fingerprint)
        return QStringLiteral("No code changes detected: ") + latest;

    const QString id = "record-" + QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QString recordDirectory = store.directory + "/records/" + id;
    if (!QDir().mkpath(recordDirectory)) fail(QStringLiteral("Cannot create record directory."));
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
    return QStringLiteral("Saved the latest record (%1 files): %2").arg(files.size()).arg(id);
}

void copyEntry(const QString &source, const QString &target, const std::function<void()> &checkpoint = {})
{
    if (checkpoint) checkpoint();
    if (isLink(source)) fail(QStringLiteral("Cannot copy linked path: ") + source);
    if (QFileInfo(source).isDir()) {
        if (!QDir().mkpath(target)) fail(QStringLiteral("Cannot create runtime directory: ") + target);
        for (const auto &entry : QDir(source).entryInfoList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System))
            copyEntry(entry.absoluteFilePath(), QDir(target).filePath(entry.fileName()), checkpoint);
        return;
    }
    QFile input(source);
    QSaveFile output(target);
    if (!input.open(QIODevice::ReadOnly) || !output.open(QIODevice::WriteOnly))
        fail(QStringLiteral("Cannot copy runtime file: ") + source + " -> " + target);
    while (!input.atEnd()) {
        if (checkpoint) checkpoint();
        const QByteArray chunk = input.read(1024 * 1024);
        if (input.error() != QFileDevice::NoError || output.write(chunk) != chunk.size())
            fail(QStringLiteral("Runtime copy failed: ") + source);
    }
    if (!output.commit()) fail(QStringLiteral("Close the viewer before updating: ") + target);
    QFile saved(target);
    if (!saved.open(QIODevice::ReadWrite) || !saved.setFileTime(QFileInfo(source).lastModified(), QFileDevice::FileModificationTime))
        fail(QStringLiteral("Cannot preserve modification time: ") + target);
    saved.close();
#ifdef Q_OS_WIN
    const DWORD attributes = GetFileAttributesW(reinterpret_cast<LPCWSTR>(source.utf16()));
    if (attributes == INVALID_FILE_ATTRIBUTES || !SetFileAttributesW(reinterpret_cast<LPCWSTR>(target.utf16()), attributes))
        fail(QStringLiteral("Cannot preserve file attributes: ") + target);
#endif
}


void checkParents(const QString &path) {
    QString current = QFileInfo(path).absoluteFilePath();
    while (!current.isEmpty()) {
        if (isLink(current)) fail(QStringLiteral("Linked shared path rejected: ") + current);
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
            fail(QStringLiteral("Index ownership mismatch. Nothing changed: ") + cache);
    } else if (create) {
        if (!QDir().mkpath(cache)) fail(QStringLiteral("Cannot create index directory: ") + cache);
        saveJson(ownerFile, {{"app", "vividcode"}, {"schema", 1}, {"projectRoot", root}});
    }
    return cache;
}
QByteArray fileDigest(const QString &path) {
    QFile file(path); QCryptographicHash hash(QCryptographicHash::Sha256);
    if (!file.open(QIODevice::ReadOnly) || !hash.addData(&file) || file.error() != QFileDevice::NoError)
        fail(QStringLiteral("Cannot verify migration file: ") + path);
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
            fail(QStringLiteral("Unknown legacy index file preserved: ") + entry.absoluteFilePath());
        const QString target = cache + '/' + entry.fileName();
        if (QFileInfo::exists(target) && fileDigest(target) != fileDigest(entry.absoluteFilePath()))
            fail(QStringLiteral("Index conflict. Both copies preserved."));
    }
    for (const auto &entry : entries) {
        const QString target = cache + '/' + entry.fileName();
        if (!QFileInfo::exists(target)) copyEntry(entry.absoluteFilePath(), target, checkpoint);
        if (fileDigest(target) != fileDigest(entry.absoluteFilePath())) fail(QStringLiteral("Migration verification failed. Old files preserved."));
    }
    if (!QDir(source).removeRecursively()) fail(QStringLiteral("Index verified; legacy cleanup failed. Retry."));
}
void removeLegacyRuntime(StoreAccess &store, const QJsonObject &manifest) {
    for (const QString &name : {"analyzer", "demo", "platforms", "styles", "imageformats", "iconengines", "networkinformation", "tls", "generic"}) {
        const QString path = store.directory + '/' + name;
        if (QFileInfo::exists(path) && !QDir(path).removeRecursively()) fail(QStringLiteral("Cannot clear legacy runtime directory: ") + path);
    }
    for (const auto &dll : QDir(store.directory).entryInfoList({"*.dll"}, QDir::Files))
        if (!QFile::remove(dll.absoluteFilePath())) fail(QStringLiteral("Cannot clear legacy dependency: ") + dll.fileName());
    QStringList obsolete{store.directory + "/README.md"};
    for (const auto &id : manifest.value("history").toArray()) obsolete << store.directory + "/records/" + id.toString() + "/code.md";
    for (const QString &path : obsolete)
        if (QFileInfo::exists(path) && !QFile::remove(path)) fail(QStringLiteral("Cannot clear legacy document: ") + path);
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
        if (QFileInfo::exists(path) && !QDir(path).removeRecursively()) fail(QStringLiteral("Temporary cleanup failed; data preserved: ") + path);
    }
    void install() {
        if (QFileInfo::exists(target)) {
            if (!QDir().rename(target, previous)) fail(QStringLiteral("Cannot stage original directory; data unchanged: ") + target);
            moved = true;
        }
        if (!QDir().rename(stage, target)) fail(QStringLiteral("Cannot publish refresh: ") + target);
        published = true;
    }
    void restore() {
        if (published) { erase(target); published = false; }
        if (moved) {
            if (!QDir().rename(previous, target)) fail(QStringLiteral("Restore failed. Original data is at: ") + previous);
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
        if (!viewer.tryLock()) fail(QStringLiteral("Close this project's viewer and retry."));
        const QDir runtime(sharedRuntime());
        checkParents(runtime.absolutePath());
        RuntimeReadLock runtimeLock(sharedHome());
        if (!runtimeLock.valid()) fail(QStringLiteral("Runtime unavailable. Run install-command.ps1."));
        const QStringList required = {"vividcode.exe", "vividcode-launcher.exe", "vividcode-viewer.exe", "Qt6Core.dll", "Qt6Gui.dll", "Qt6Widgets.dll",
            "Qt6Sql.dll", "Qt6Network.dll", "tls/qschannelbackend.dll", "sqldrivers/qsqlite.dll", "platforms/qwindows.dll", "installation.json"};
        for (const auto &name : required)
            if (!QFileInfo(runtime.filePath(name)).isFile()) fail(QStringLiteral("Incomplete runtime. Reinstall: ") + name);
        checkTree(runtime.absolutePath());
        const auto installation = readJson(runtime.filePath("installation.json"));
        if (installation.value("app") != "vividcode" || installation.value("schema").toInt() != 1)
            fail(QStringLiteral("Invalid runtime installation. Reinstall."));
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
            if (!QDir().mkpath(project.stage)) fail(QStringLiteral("Cannot create refresh staging directory."));
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
                if (!QDir().mkpath(cache.stage)) fail(QStringLiteral("Cannot prepare index migration."));
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
                fail(existed ? QStringLiteral("Cancelled. Original .vividcode, history and semantic index unchanged; this record was discarded.")
                             : QStringLiteral("Cancelled. Temporary directory removed; no .vividcode or records left behind."));
            throw;
        }
        QString warning;
        try { Publication::erase(project.previous); Publication::erase(cache.previous); }
        catch (const std::exception &error) {
            warning = "\n" + QString::fromUtf8(error.what());
            if (feedback.warning) feedback.warning(warning.trimmed());
        }
        return recordMessage + QStringLiteral("\nProject launcher generated: ") + QDir::toNativeSeparators(directory + "/vividcode-viewer.exe") + warning;
    });
}

Result setFileLimit(qint64 count) {
    return attempt([&] {
        if (count < 1 || count > 2147483647) fail(QStringLiteral("Enter an integer from 1 to 2147483647."));
        const QString home = sharedHome(); checkParents(home);
        if (!QDir().mkpath(home)) fail(QStringLiteral("Cannot create settings directory."));
        QLockFile lock(checkedLockPath(home, "read-policy.lock"));
        if (!lock.tryLock()) fail(QStringLiteral("File limit settings are busy. Retry."));
        const QString path = home + "/read-policy.json";
        saveJson(path, {{"app", "vividcode"}, {"schema", 1}, {"maxSourceFiles", count}});
        return QStringLiteral("Global source file limit set to %1. Applies on the next generation/refresh in all projects; -sg ignores this limit.").arg(count);
    });
}

Result removeGenerated(const QString &projectRoot)
{
    return attempt([&] {
        StoreAccess store(projectRoot);
        QLockFile viewer(checkedLockPath(store.root, ".vividcode-viewer.lock"));
        viewer.setStaleLockTime(0);
        if (!viewer.tryLock()) fail(QStringLiteral("Close this project's viewer and retry."));
        const QString cache = checkedCache(store.root, false);
        const QString aiCache = sharedHome() + "/ai/" + projectCacheKey(store.root);
        checkParents(aiCache); checkTree(aiCache);
        if (QFileInfo::exists(aiCache)) {
            const QString aiLockPath = aiCache + ".cache.lock"; checkParents(aiLockPath);
            QLockFile aiLock(aiLockPath);
            if (!aiLock.tryLock()) fail(QStringLiteral("Close this project's viewer and retry."));
            const auto owner = readJson(aiCache + "/project.json");
            if (owner.value("app") != "vividcode" || owner.value("schema").toInt() != 1 || owner.value("projectRoot").toString().compare(store.root, Qt::CaseInsensitive))
                fail(QStringLiteral("AI cache ownership mismatch. Nothing cleared: ") + aiCache);
            if (!QDir(aiCache).removeRecursively()) fail(QStringLiteral("AI cache is busy or not writable. Close the viewer and retry."));
        }
        if (QFileInfo::exists(cache) && !QDir(cache).removeRecursively()) fail(QStringLiteral("Cannot clear index. Close analysis processes and retry."));
        // Fixed project directory and cache hash; never use arbitrary manifest paths for deletion.
        if (QFileInfo::exists(store.directory) && !QDir(store.directory).removeRecursively()) fail(QStringLiteral("Project cleanup incomplete. Close the viewer and check permissions."));
        return QStringLiteral("Project .vividcode, shared index and AI cache cleared. Shared runtime and global settings retained.");
    });
}

Result cleanHistory(const QString &projectRoot)
{
    return attempt([&] {
        StoreAccess store(projectRoot);
        if (!QFileInfo::exists(store.directory)) fail(QStringLiteral("No project data. Run vividcode -g first."));
        auto manifest = loadManifest(store.directory);
        const QString latest = manifest.value("latest").toString();
        if (latest.isEmpty()) fail(QStringLiteral("No complete latest record. Run vividcode -g first."));
        loadRecord(store.directory, latest); // 最新记录不完整时，保留所有历史。
        auto history = manifest.value("history").toArray();
        int removed = 0;
        for (qsizetype i = history.size() - 1; i >= 0; --i) {
            const QString id = history.at(i).toString();
            if (id == latest) continue;
            const QString target = store.directory + "/records/" + id;
            if (QFileInfo::exists(target) && !QDir(target).removeRecursively())
                fail(QStringLiteral("History cleanup incomplete. Retry: ") + id);
            history.removeAt(i);
            manifest["history"] = history;
            saveJson(store.directory + "/project.json", manifest);
            ++removed;
        }
        return QStringLiteral("Cleared %1 old records; latest record retained: %2").arg(removed).arg(latest);
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
