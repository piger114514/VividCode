#pragma once
#include <QDir>
#include <QFileInfo>
#include <QSet>
#include <QStringList>
#include <algorithm>

struct ScanResult
{
    QStringList files; // 相对于项目根目录的路径
    int skippedDirectories = 0;
    bool truncated = false;
};

// 不跟随符号链接；迭代遍历避免深目录导致递归栈溢出。
inline ScanResult scanProject(const QString &root)
{
    const QSet<QString> extensions = {"c", "cc", "cpp", "cxx", "h", "hh", "hpp", "hxx", "inl", "ipp",
                                      "cs", "py", "pyw", "pyi",
                                      "js", "jsx", "mjs", "cjs", "ts", "tsx", "java"};
    const QSet<QString> excluded = {".git", ".vs", ".codegraph", ".codemap", ".vividcode", "build", "out", "x64", "x86",
        "debug", "release", "node_modules", "vendor", "third_party", "_deps", "cmakefiles",
        "bin", "obj", "__pycache__", ".venv", "venv", "env", ".pytest_cache", ".mypy_cache", ".tox", ".idea",
        "dist", "target", ".gradle", ".mvn", "coverage", ".next", ".nuxt", ".cache", ".parcel-cache"};
    ScanResult result;
    QStringList pending{root};
    const QDir base(root);
    int visited = 0;
    while (!pending.isEmpty()) {
        const QDir directory(pending.takeLast());
        const auto entries = directory.entryInfoList(QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot | QDir::Hidden | QDir::NoSymLinks);
        for (const QFileInfo &entry : entries) {
            if (++visited > 50000 || result.files.size() >= 5000) {
                result.truncated = true;
                break;
            }
            if (entry.isDir()) {
                const QString name = entry.fileName().toLower();
                if (excluded.contains(name) || name.startsWith("build-") || name.startsWith("cmake-build-"))
                    ++result.skippedDirectories;
                else
                    pending.append(entry.absoluteFilePath());
            } else if (extensions.contains(entry.suffix().toLower())) {
                result.files.append(base.relativeFilePath(entry.absoluteFilePath()));
            }
        }
        if (result.truncated) break;
    }
    std::sort(result.files.begin(), result.files.end());
    return result;
}
