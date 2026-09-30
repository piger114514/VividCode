#include "ProjectStore.h"
#include "ProjectScanner.h"
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
        fail(QStringLiteral(".vividcode 不是受支持的 vividcode 项目，未修改其内容。"));
    QSet<QString> ids;
    for (const auto &value : manifest.value("history").toArray()) {
        const auto id = value.toString();
        if (!validId(id) || ids.contains(id)) fail(QStringLiteral("历史记录 ID 无效或重复，未执行清理。"));
        ids.insert(id);
    }
    const QString latest = manifest.value("latest").toString();
    if ((!latest.isEmpty() && !ids.contains(latest)) || (latest.isEmpty() && !ids.isEmpty()))
        fail(QStringLiteral("最新记录指针无效，未修改历史。"));
    return manifest;
}

QJsonObject loadRecord(const QString &directory, const QString &id)
{
    if (!validId(id)) fail(QStringLiteral("记录 ID 无效。"));
    const auto record = readJson(directory + "/records/" + id + "/index.json");
    if (record.value("app") != "vividcode" || record.value("id") != id ||
        !record.value("files").isArray() || !record.value("fingerprint").isString())
        fail(QStringLiteral("记录内容无效：") + id);
    if (!QFileInfo(directory + "/records/" + id + "/code.md").isFile())
        fail(QStringLiteral("记录文档缺失：") + id);
    return record;
}

QString recordSnapshot(StoreAccess &store, QJsonObject manifest)
{
    const ScanResult scan = scanProject(store.root);
    if (scan.truncated) fail(QStringLiteral("扫描达到上限，未保存不完整记录。请选择更小的项目。"));
    QJsonArray files;
    QString markdown = QStringLiteral("# vividcode 代码文件概览\n\n这是文件级记录，不是类或函数语义分析，也不是源码备份。\n\n");
    markdown += QStringLiteral("| 文件 | 字节数 | SHA-256 |\n| --- | ---: | --- |\n");
    for (const auto &relative : scan.files) {
        const QString path = QDir(store.root).filePath(relative);
        if (isLink(path)) fail(QStringLiteral("源码文件在扫描后变成链接：") + path);
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly)) fail(QStringLiteral("源码读取失败，未保存新记录：") + relative);
        QCryptographicHash hash(QCryptographicHash::Sha256);
        const qint64 size = file.size();
        if (!hash.addData(&file) || file.error() != QFileDevice::NoError || file.size() != size)
            fail(QStringLiteral("源码读取失败或扫描期间发生变化，请重试：") + relative);
        const QString digest = QString::fromLatin1(hash.result().toHex());
        files.append(QJsonObject{{"path", relative}, {"bytes", size}, {"sha256", digest}});
        QString escaped = relative.toHtmlEscaped();
        escaped.replace('|', "&#124;").replace('\n', "&#10;").replace('\r', "&#13;").replace('`', "&#96;");
        markdown += QStringLiteral("| %1 | %2 | %3 |\n").arg(escaped).arg(size).arg(digest);
    }
    const QString fingerprint = QString::fromLatin1(QCryptographicHash::hash(
        QJsonDocument(files).toJson(QJsonDocument::Compact), QCryptographicHash::Sha256).toHex());
    const QString latest = manifest.value("latest").toString();
    if (!latest.isEmpty() && loadRecord(store.directory, latest).value("fingerprint") == fingerprint)
        return QStringLiteral("代码未变化，继续保留最新记录：") + latest;

    const QString id = "record-" + QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QString recordDirectory = store.directory + "/records/" + id;
    if (!QDir().mkpath(recordDirectory)) fail(QStringLiteral("无法创建记录目录。"));
    const QString timestamp = QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
    const QJsonObject record{{"app", "vividcode"}, {"schema", 1}, {"id", id}, {"createdUtc", timestamp},
        {"fingerprint", fingerprint}, {"files", files}};
    // 先完成两个记录文件，再原子更新 manifest。未提交的记录不会成为“最新”。
    saveJson(recordDirectory + "/index.json", record);
    saveBytes(recordDirectory + "/code.md", (markdown + "\nUTC: " + timestamp + "\n").toUtf8());
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

template<class Function> VividCode::Result attempt(Function operation)
{
    try { return {true, operation()}; }
    catch (const std::exception &error) { return {false, QString::fromUtf8(error.what())}; }
}
}

namespace VividCode {
Result generate(const QString &projectRoot, const QString &runtimeDirectory)
{
    return attempt([&] {
        StoreAccess store(projectRoot);
        QLockFile viewer(checkedLockPath(store.root, ".vividcode-viewer.lock"));
        viewer.setStaleLockTime(0);
        if (!viewer.tryLock()) fail(QStringLiteral("请先关闭此项目的 vividcode 查看器，再生成或更新程序文件。"));
        const QDir runtime(runtimeDirectory);
        const QStringList required = {"vividcode-viewer.exe", "Qt6Core.dll", "Qt6Gui.dll", "Qt6Widgets.dll", "platforms/qwindows.dll",
            "analyzer/analyzer.py", "analyzer/extractor.py", "analyzer/clang_bridge.py", "analyzer/python/python.exe", "analyzer/python/python313.dll",
            "analyzer/python/python313.zip", "analyzer/python/python313._pth", "analyzer/python/_sqlite3.pyd", "analyzer/python/sqlite3.dll", "analyzer/lib/libclang.dll", "analyzer/lib/clang/23/include/stddef.h", "analyzer/runtime.json"};
        for (const auto &name : required)
            if (!QFileInfo(runtime.filePath(name)).isFile()) fail(QStringLiteral("运行包不完整，请先执行 build.ps1：") + name);
        QJsonObject manifest;
        if (QFileInfo::exists(store.directory)) manifest = loadManifest(store.directory);
        else {
            if (!QDir().mkpath(store.directory)) fail(QStringLiteral("无法创建 .vividcode。"));
            manifest = {{"app", "vividcode"}, {"schema", 1}, {"projectRoot", ".."}, {"latest", ""}, {"history", QJsonArray{}}};
            saveJson(store.directory + "/project.json", manifest);
        }
        copyEntry(runtime.filePath("vividcode-viewer.exe"), store.directory + "/vividcode-viewer.exe");
        for (const auto &dll : runtime.entryInfoList({"*.dll"}, QDir::Files))
            copyEntry(dll.absoluteFilePath(), store.directory + '/' + dll.fileName());
        for (const QString &name : {"platforms", "styles", "imageformats", "iconengines", "networkinformation", "tls", "generic", "demo", "analyzer"})
            if (QFileInfo(runtime.filePath(name)).isDir()) copyEntry(runtime.filePath(name), store.directory + '/' + name);
        const QString result = recordSnapshot(store, manifest);
        saveBytes(store.directory + "/README.md", QStringLiteral(
            "# vividcode\n\n双击 vividcode-viewer.exe 查看上一级项目；启动或 F5 刷新会检查代码变化并保存新记录。\n\n"
            "project.json 的 latest 指向 records/<记录ID>/code.md 和 index.json。\n\n"
            "在项目根目录运行 vividcode --c 清理旧记录；vividcode --d 删除整个 .vividcode。\n").toUtf8());
        return result + QStringLiteral("\n程序已生成：") + QDir::toNativeSeparators(store.directory + "/vividcode-viewer.exe");
    });
}

Result removeGenerated(const QString &projectRoot)
{
    return attempt([&] {
        StoreAccess store(projectRoot);
        if (!QFileInfo::exists(store.directory)) return QStringLiteral("当前目录没有 .vividcode。");
        QLockFile viewer(checkedLockPath(store.root, ".vividcode-viewer.lock"));
        viewer.setStaleLockTime(0);
        if (!viewer.tryLock()) fail(QStringLiteral("请先关闭此项目的 vividcode 查看器。"));
        // directory 由规范化后的项目根目录加固定子目录名构成，且整棵树已拒绝链接。
        if (!QDir(store.directory).removeRecursively()) fail(QStringLiteral("删除未完成：文件可能被占用或不可写，请关闭查看器后重试。"));
        return QStringLiteral("已清除 .vividcode。");
    });
}

Result cleanHistory(const QString &projectRoot)
{
    return attempt([&] {
        StoreAccess store(projectRoot);
        if (!QFileInfo::exists(store.directory)) fail(QStringLiteral("当前目录尚未初始化，请先运行 vividcode --g。"));
        auto manifest = loadManifest(store.directory);
        const QString latest = manifest.value("latest").toString();
        if (latest.isEmpty()) fail(QStringLiteral("没有完整的最新记录，请先运行 vividcode --g。"));
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
