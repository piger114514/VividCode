#pragma once
#include <QDir>
#include <QFileInfo>
#include <QSet>
#include <QStringList>
#include <algorithm>
#include <QDirIterator>
#include <functional>
#include "ReadPolicy.h"

struct ScanResult
{
    QStringList files; // 相对于项目根目录的路径
    int skippedDirectories = 0;
    bool truncated = false;
    QString error;
};

// 不跟随符号链接；迭代遍历避免深目录导致递归栈溢出。
inline ScanResult scanProject(const QString &root, const VividCode::ReadPolicy &policy, const std::function<void()> &checkpoint = {})
{
    const QSet<QString> extensions = {"c", "cc", "cpp", "cxx", "h", "hh", "hpp", "hxx", "inl", "ipp",
                                      "cs", "py", "pyw", "pyi",
                                      "js", "jsx", "mjs", "cjs", "ts", "tsx", "mts", "cts", "java"};
    const QSet<QString> excluded = {".git", ".vs", ".codegraph", ".codemap", ".vividcode", "build", "out", "x64", "x86",
        "debug", "release", "node_modules", "vendor", "third_party", "_deps", "cmakefiles",
        "bin", "obj", "__pycache__", ".venv", "venv", "env", ".pytest_cache", ".mypy_cache", ".tox", ".idea",
        "dist", "target", ".gradle", ".mvn", "coverage", ".next", ".nuxt", ".cache", ".parcel-cache"};
    ScanResult result;
    const bool unity = QDir(root + "/Assets").exists() && QFileInfo::exists(root + "/ProjectSettings/ProjectVersion.txt");
    const QSet<QString> unityGenerated{"library", "temp", "logs", "usersettings"};
    QStringList pending{root};
    const QDir base(root);
    qint64 visited = 0;
    while (!pending.isEmpty()) {
        const QDir directory(pending.takeLast());
        QDirIterator entries(directory.absolutePath(), QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot | QDir::Hidden | QDir::NoSymLinks);
        while (entries.hasNext()) {
            if (checkpoint) checkpoint();
            entries.next(); const QFileInfo entry = entries.fileInfo();
            if (!policy.force && ++visited > policy.maxEntries()) {
                result.truncated = true;
                break;
            }
            if (entry.isDir()) {
                const QString name = entry.fileName().toLower();
                if (entry.isJunction() || excluded.contains(name) || name.startsWith(".vividcode-") || name.startsWith("build-") || name.startsWith("cmake-build-") ||
                    (unity && directory.absolutePath() == QDir(root).absolutePath() && unityGenerated.contains(name)))
                    ++result.skippedDirectories;
                else
                    pending.append(entry.absoluteFilePath());
            } else if (extensions.contains(entry.suffix().toLower())) {
                if (!policy.force && result.files.size() >= policy.maxFiles) { result.truncated = true; break; }
                result.files.append(base.relativeFilePath(entry.absoluteFilePath()));
            }
        }
        if (result.truncated) break;
    }
    std::sort(result.files.begin(), result.files.end());
    return result;
}

inline ScanResult scanProject(const QString &root) {
    try { return scanProject(root, VividCode::readPolicy(root)); }
    catch (const std::exception &error) { ScanResult result; result.error = QString::fromUtf8(error.what()); return result; }
}
