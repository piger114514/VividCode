#pragma once
#include <QtCore>
#include <QtNetwork>
#include <QtSql>
#include <atomic>
#include <memory>

class QWidget;

namespace VividCode {
struct AiSettings {
    QString baseUrl, model, apiKey, language = "auto";
    QString audience = "intermediate";
    int sourceKiB = 256;
    QString resolvedLanguage() const;
    QString audienceInstruction() const;
    QUrl endpoint() const;
    QString profile() const;
    static AiSettings load();
    void save() const;
};
bool editAiSettings(QWidget *parent);

// Owns only AI explanations, never source files or the semantic graph.
class AiExplanation : public QObject {
    Q_OBJECT
public:
    explicit AiExplanation(QObject *parent = nullptr);
    ~AiExplanation() override;
    void setProject(const QString &root, const QStringList &files, bool complete);
    void start(const QMap<QString, QStringList> &dependencies, const QStringList &changedFiles = {});
    void cancel();
    bool isBusy() const { return busy; }
    QString tooltip(const QString &path) const;
    QString report() const { return messages.join('\n'); }
signals:
    void statusChanged(const QString &message);
    void finished();
private:
    struct Task { QString path, reason, context; };
    bool openCache();
    void closeCache();
    void plan(const QMap<QString, QString> &hashes, const QStringList &errors);
    void next();
    void receive(QNetworkReply *reply, Task task, QByteArray hash, bool removed);
    void complete();
    void log(const QString &text);
    QJsonObject record(const QString &path) const;
    void persistPending(const Task &task);
    void enqueue(const Task &task);
    QString sourcePath(const QString &relative) const;
    QString root, connection;
    QStringList sources, messages;
    bool scanComplete = false, busy = false;
    int succeeded = 0, failed = 0, reused = 0;
    AiSettings settings;
    QSqlDatabase database;
    QNetworkAccessManager network;
    QPointer<QNetworkReply> active;
    QThread *scanner = nullptr;
    std::unique_ptr<QLockFile> requestLock;
    std::unique_ptr<QLockFile> cacheLock;
    std::shared_ptr<std::atomic_bool> cancelled;
    QMap<QString, QString> hashes;
    QMap<QString, QStringList> dependencies;
    QList<Task> tasks;
    QSet<QString> queued, attempted;
    QSet<QString> changedScope;
};
}
