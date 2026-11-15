#pragma once
#include "SharedRuntime.h"
#include "UiTranslations.h"

namespace UiLanguage {
inline QString text(const char *source) {
    return QCoreApplication::translate("VividCode", source);
}
class Translator final : public QTranslator {
public:
    using QTranslator::QTranslator;
    bool isEmpty() const override { return false; }
    QString translate(const char *, const char *source, const char * = nullptr, int = -1) const override {
        return translations().value(source);
    }
};
inline QString settingsPath() { return VividCode::sharedHome() + "/ui-settings.json"; }
inline QString preference() {
    QFile file(settingsPath());
    if (QFileInfo(file).isSymLink() || !file.open(QIODevice::ReadOnly) || file.size() > 16384) return "en";
    const QString value = QJsonDocument::fromJson(file.readAll()).object().value("language").toString();
    return value == "zh" ? "zh" : "en";
}
inline QString save(const QString &language) {
    if (language != "zh" && language != "en") return "Invalid interface language";
    const QString path = settingsPath();
    for (QString part = path; !part.isEmpty();) {
        const QFileInfo info(part);
        if (info.isSymLink() || info.isJunction()) return "Linked settings path rejected";
        const QString parent = info.absolutePath(); if (parent == part) break; part = parent;
    }
    if (!QDir().mkpath(QFileInfo(path).absolutePath())) return "Could not create settings directory";
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) return file.errorString();
    const auto bytes = QJsonDocument(QJsonObject{{"schema", 1}, {"language", language}}).toJson();
    if (file.write(bytes) != bytes.size() || !file.commit()) return file.errorString();
    return {};
}
inline void initialize(QCoreApplication &app, const QString &language) {
    if (auto *previous = app.findChild<QTranslator *>("uiTranslator", Qt::FindDirectChildrenOnly)) {
        app.removeTranslator(previous); delete previous;
    }
    const QString selected = language == "zh" ? "zh" : "en";
    app.setProperty("vividcode.uiLanguage", selected);
    if (selected == "zh") {
        auto *translator = new Translator(&app); translator->setObjectName("uiTranslator");
        app.installTranslator(translator);
    }
}
}
