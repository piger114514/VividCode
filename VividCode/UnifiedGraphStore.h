#pragma once
#include "UiLanguage.h"
#include "SharedRuntime.h"
#include <QtSql>

// 分析器保留各自的增量缓存；所有语言的成功结果进入同一份展示数据库。
class UnifiedGraphStore {
public:
    explicit UnifiedGraphStore(const QString &root) {
        const QString directory = VividCode::semanticDirectory(root);
        for (QString part = directory; !part.isEmpty();) {
            QFileInfo info(part);
            if (info.isSymLink() || info.isJunction()) { error = UiLanguage::text("Linked cache directory rejected: "); return; }
            const QString parent = info.absolutePath(); if (parent == part) break; part = parent;
        }
        QFile owner(directory + "/project.json");
        if (!owner.open(QIODevice::ReadOnly)) return;
        const auto identity = QJsonDocument::fromJson(owner.readAll()).object();
        if (identity.value("app") != "vividcode" || identity.value("schema").toInt() != 1 ||
            QFileInfo(identity.value("projectRoot").toString()).canonicalFilePath().compare(QFileInfo(root).canonicalFilePath(), Qt::CaseInsensitive)) {
            error = UiLanguage::text("Shared graph database ownership mismatch."); return;
        }
        const QString path = directory + "/graph.db";
        for (const QString &suffix : {QString(), QStringLiteral("-journal"), QStringLiteral("-wal"), QStringLiteral("-shm")}) {
            QFileInfo info(path + suffix);
            if (info.isSymLink() || info.isJunction()) { error = UiLanguage::text("Linked graph database rejected."); return; }
        }
        connection = "vividcode-graph-" + QUuid::createUuid().toString(QUuid::WithoutBraces);
        db = QSqlDatabase::addDatabase("QSQLITE", connection);
        db.setDatabaseName(path); db.setConnectOptions("QSQLITE_BUSY_TIMEOUT=2000");
        if (!db.open()) { error = db.lastError().text(); return; }
        QSqlQuery query(db);
        if (!query.exec("PRAGMA user_version") || !query.next()) { error = query.lastError().text(); return; }
        const int schema = query.value(0).toInt();
        if (schema != 0 && schema != 1) { error = UiLanguage::text("Incompatible graph database version."); return; }
        if (!schema) {
            if (!query.exec("SELECT name FROM sqlite_master WHERE type='table'") || query.next()) { error = UiLanguage::text("Unknown graph database; nothing overwritten."); return; }
            if (!db.transaction() || !query.exec("CREATE TABLE language_states(language TEXT PRIMARY KEY,state TEXT NOT NULL)") ||
                !query.exec("CREATE TABLE graph_units(language TEXT NOT NULL,unit_key TEXT NOT NULL,payload TEXT NOT NULL,PRIMARY KEY(language,unit_key))") ||
                !query.exec("PRAGMA user_version=1") || !db.commit()) { db.rollback(); error = query.lastError().text(); return; }
        }
        ready = true;
    }
    ~UnifiedGraphStore() {
        if (!connection.isEmpty()) { db.close(); db = {}; QSqlDatabase::removeDatabase(connection); }
    }
    bool saveLanguage(const QString &language, const QJsonObject &snapshot) {
        if (!ready || snapshot.value("state").toObject().value("last_status") != "ok") return false;
        QSqlQuery query(db);
        const auto failed = [&] { error = query.lastError().text(); db.rollback(); return false; };
        if (!db.transaction()) { error = db.lastError().text(); return false; }
        query.prepare("DELETE FROM graph_units WHERE language=?"); query.addBindValue(language);
        if (!query.exec()) return failed();
        for (const auto &value : snapshot.value("units").toArray()) {
            const auto unit = value.toObject();
            // C# 的汇总范围由界面统一生成，避免与各项目重复保存和展示。
            if (unit.value("config_id") == "csharp-solution") continue;
            const QString id = unit.value("file").toString() + '\n' + unit.value("config_id").toString();
            query.prepare("INSERT INTO graph_units(language,unit_key,payload) VALUES(?,?,?)");
            query.addBindValue(language); query.addBindValue(id);
            query.addBindValue(QString::fromUtf8(QJsonDocument(unit).toJson(QJsonDocument::Compact)));
            if (!query.exec()) return failed();
        }
        query.prepare("INSERT OR REPLACE INTO language_states(language,state) VALUES(?,?)");
        query.addBindValue(language); query.addBindValue(QString::fromUtf8(QJsonDocument(snapshot.value("state").toObject()).toJson(QJsonDocument::Compact)));
        if (!query.exec() || !db.commit()) return failed();
        return true;
    }
    QMap<QString,QJsonObject> load() {
        QMap<QString,QJsonObject> snapshots;
        if (!ready) return snapshots;
        QSqlQuery query(db);
        if (!query.exec("SELECT language,state FROM language_states ORDER BY language")) { error = query.lastError().text(); return {}; }
        while (query.next()) snapshots.insert(query.value(0).toString(), {{"ok", true}, {"state", QJsonDocument::fromJson(query.value(1).toString().toUtf8()).object()}, {"units", QJsonArray()}});
        if (!query.exec("SELECT language,payload FROM graph_units ORDER BY language,unit_key")) { error = query.lastError().text(); return {}; }
        while (query.next()) {
            const QString language = query.value(0).toString(); auto snapshot = snapshots.value(language);
            auto units = snapshot.value("units").toArray(); units.append(QJsonDocument::fromJson(query.value(1).toString().toUtf8()).object());
            snapshot["units"] = units; snapshots[language] = snapshot;
        }
        return snapshots;
    }
    QString error;
private:
    QString connection;
    QSqlDatabase db;
    bool ready = false;
};
