#pragma once
#include <QtWidgets>
#include <QProcess>
#include "PiRichText.h"
#include <atomic>
#include <memory>

namespace VividCode {
// A project-bound RPC client. Pi owns its tools, credentials and source edits.
class PiAgentPanel : public QWidget {
    Q_OBJECT
public:
    explicit PiAgentPanel(QWidget *parent = nullptr);
    ~PiAgentPanel() override;
    QWidget *outputView() const { return outputPanel; }
    void setProject(const QString &path);
    bool isBusy() const;
    void finishRefresh(const QString &message);
    void report(const QString &message);
signals:
    void refreshRequested(const QStringList &changedFiles);
private:
    bool eventFilter(QObject *object, QEvent *event) override;
    QWidget *outputPanel;
    void refreshHistory();
    bool projectSession(const QString &path) const;
    void changeSession(const QString &path);
    void showMessage(const QJsonObject &message);
    QComboBox *history;
    QPushButton *newSessionButton, *historyRefresh;
    QString sessionFile;
    bool historyBusy = false, autoConnectPending = false;
    QTimer historyTimer;
    void configure();
    void saveSettings();
    void connectAgent();
    void disconnectAgent();
    void sendPrompt();
    void stopTask();
    void snapshot(bool before);
    void consumeOutput();
    void record(const QJsonObject &object);
    void send(QJsonObject command);
    void settle(const QString &message);
    void updateControls();
    void requestUi(const QJsonObject &object);
    QProcess process;
    QTimer startupTimer, stopTimer;
    QString root, piPath, nodePath, promptId, pendingPrompt;
    QByteArray output;
    QMap<QString, QByteArray> beforeHashes;
    QThread *scanner = nullptr;
    std::shared_ptr<std::atomic_bool> scanCancelled;
    quint64 serial = 0;
    bool ready = false, running = false, scanning = false, updating = false;
    bool closing = false, shuttingDown = false, autoThisRun = false;
    bool startedThisPrompt = false;
    void *job = nullptr;
    QLabel *status;
    PiRichText *conversation;
    QPlainTextEdit *input;
    QPushButton *connectionButton, *settingsButton, *sendButton, *stopButton;
    QCheckBox *autoRefresh;
};
}
