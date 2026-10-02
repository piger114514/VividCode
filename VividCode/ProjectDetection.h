#pragma once
#include "SharedRuntime.h"
#include <QtCore>

struct AnalysisEntry {
    QString language, path, label;
};

// 语言来自工程入口及已扫描源码；C++ 始终需要真实编译数据库，不从 .sln 猜编译参数。
inline QList<AnalysisEntry> detectAnalysisEntries(const QString &root, const QStringList &sources) {
    QList<AnalysisEntry> result;
    QSet<QString> seen;
    const auto add = [&](const QString &language, const QString &path) {
        const QFileInfo info(path);
        if (!info.isFile() || !info.isReadable() || info.isSymLink() || info.isJunction()) return;
        const QString absolute = info.absoluteFilePath();
        const QString key = language + ':' + absolute.toLower();
        if (seen.contains(key)) return;
        seen.insert(key);
        result.append({language, absolute, (language == "c#" ? "C# · " : "C++ · ") + QDir(root).relativeFilePath(absolute)});
    };
    const auto state = [&](const QString &suffix) {
        QFile file(VividCode::semanticDirectory(root) + suffix);
        return file.open(QIODevice::ReadOnly) ? QJsonDocument::fromJson(file.readAll()).object() : QJsonObject();
    };
    const auto entries = QDir(root).entryInfoList(QDir::Files | QDir::Readable | QDir::NoSymLinks, QDir::Name | QDir::IgnoreCase);
    bool solutionHasCpp = false;
    for (const auto &entry : entries) {
        if (entry.suffix().compare("sln", Qt::CaseInsensitive) != 0 || entry.isJunction()) continue;
        QFile file(entry.absoluteFilePath()); if (!file.open(QIODevice::ReadOnly)) continue;
        const QString text = QString::fromUtf8(file.readAll());
        const QRegularExpression projectLine("^Project\\([^\\r\\n]*\\)\\s*=\\s*\"[^\"]*\"\\s*,\\s*\"([^\"]+)\"", QRegularExpression::MultilineOption);
        bool hasCs = false, hasCpp = false;
        auto matches = projectLine.globalMatch(text);
        while (matches.hasNext()) {
            const QString suffix = QFileInfo(matches.next().captured(1)).suffix().toLower();
            hasCs |= suffix == "csproj"; hasCpp |= suffix == "vcxproj";
        }
        solutionHasCpp |= hasCpp;
        if (hasCs || !hasCpp) add("c#", entry.absoluteFilePath());
    }
    const bool hasSolution = !result.isEmpty();
    if (!hasSolution) {
        const QString previous = state("/csharp/config.json").value("project_file").toString();
        if (!previous.isEmpty()) add("c#", previous);
        for (const auto &entry : entries)
            if (entry.suffix().compare("csproj", Qt::CaseInsensitive) == 0) add("c#", entry.absoluteFilePath());
    }
    const QString previousCpp = state("/config.json").value("compile_commands").toString();
    if (!previousCpp.isEmpty()) add("c++", previousCpp);
    add("c++", QDir(root).filePath("compile_commands.json"));
    add("c++", QDir(root).filePath("build/compile_commands.json"));
    bool csSources = false, cppSources = solutionHasCpp;
    const QSet<QString> cppExtensions{"c", "cc", "cpp", "cxx", "h", "hh", "hpp", "hxx", "inl", "ipp"};
    for (const auto &source : sources) {
        const QString suffix = QFileInfo(source).suffix().toLower();
        csSources |= suffix == "cs"; cppSources |= cppExtensions.contains(suffix);
    }
    bool csEntry = false, cppEntry = false;
    for (const auto &entry : result) { csEntry |= entry.language == "c#"; cppEntry |= entry.language == "c++"; }
    if (csSources && !csEntry) result.append({"c#", {}, QStringLiteral("C# 项目（尚未找到 .sln / .csproj）")});
    if (cppSources && !cppEntry) result.append({"c++", {}, QStringLiteral("C++ 项目（尚未找到编译数据库）")});
    return result;
}
