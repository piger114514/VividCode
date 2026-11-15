#pragma once
#include "SharedRuntime.h"
#include <QtCore>
#include <stdexcept>

namespace VividCode {
struct ReadPolicy {
    qint64 maxFiles = 20000;
    bool force = false;
    qint64 maxEntries() const { return qMax<qint64>(50000, maxFiles * 5 / 2); }
};
inline ReadPolicy readPolicy(const QString &root) {
    ReadPolicy policy;
    const auto read = [](const QString &path) {
        QFile file(path);
        if (!file.exists()) return QJsonObject{};
        if (QFileInfo(path).isSymLink() || QFileInfo(path).isJunction() || !file.open(QIODevice::ReadOnly) || file.size() > 65536)
            throw std::runtime_error("Cannot read file limit settings.");
        QJsonParseError error; const auto document = QJsonDocument::fromJson(file.readAll(), &error);
        if (error.error != QJsonParseError::NoError || !document.isObject()) throw std::runtime_error("Invalid file limit settings JSON.");
        return document.object();
    };
    const auto global = read(sharedHome() + "/read-policy.json");
    if (!global.isEmpty()) {
        const double count = global.value("maxSourceFiles").toDouble();
        if (global.value("app") != "vividcode" || global.value("schema").toInt() != 1 ||
            count < 1 || count > 2147483647 || count != static_cast<qint64>(count))
            throw std::runtime_error("Invalid file limit. Use vividcode -c.");
        policy.maxFiles = static_cast<qint64>(count);
    }
    if (!root.isEmpty()) {
        const auto project = read(QDir(root).filePath(".vividcode/project.json"));
        policy.force = project.value("app") == "vividcode" && project.value("schema").toInt() == 1 && project.value("forceRead").toBool();
    }
    return policy;
}
}
