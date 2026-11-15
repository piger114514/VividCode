#include "PiAgent.h"
#include "ProjectScanner.h"
#include "SharedRuntime.h"
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QSaveFile>
#include <QCryptographicHash>
#ifdef Q_OS_WIN
#define NOMINMAX
#include <windows.h>
#endif

namespace VividCode {
namespace {
QString configPath() { return sharedHome() + "/pi-agent.json"; }
void check(bool ok, const QString &message) { if (!ok) throw std::runtime_error(message.toUtf8().constData()); }
void plainPath(QString path) {
    for (;;) {
        const QFileInfo info(path);
        check(!info.isSymLink() && !info.isJunction(), QStringLiteral("Pi 设置路径不能经过链接：") + path);
        const auto parent = info.dir().absolutePath(); if (parent == path) break; path = parent;
    }
}
QString messageText(const QJsonObject &message) {
    QStringList parts;
    if (message.value("content").isString()) return message.value("content").toString();
    for (const auto &part : message.value("content").toArray())
        if (part.toObject().value("type") == "text") parts.append(part.toObject().value("text").toString());
    return parts.join('\n');
}
QString piScript(const QString &command) {
    const QFileInfo info(command); const QString suffix = info.suffix().toLower();
    if (suffix == "js" || suffix == "mjs" || suffix == "cjs") return info.absoluteFilePath();
    if (suffix != "cmd" && suffix != "bat" && suffix != "ps1") return {};
    // Do not execute shell command strings. Resolve the official managed or npm entry.
    const QDir dir = info.dir();
    if (QFileInfo::exists(dir.filePath("pi-launcher.js"))) return dir.filePath("pi-launcher.js");
    for (const auto &package : {"@earendil-works/pi-coding-agent", "@mariozechner/pi-coding-agent"}) {
        const QString packageDir = dir.filePath("node_modules/" + QString::fromLatin1(package));
        QFile manifest(packageDir + "/package.json");
        if (!manifest.open(QIODevice::ReadOnly)) continue;
        const auto bin = QJsonDocument::fromJson(manifest.read(256 * 1024)).object().value("bin");
        const QString relative = bin.isString() ? bin.toString() : bin.toObject().value("pi").toString();
        const QString cli = QFileInfo(QDir(packageDir).filePath(relative)).canonicalFilePath();
        const QString base = QFileInfo(packageDir).canonicalFilePath();
        if (!relative.isEmpty() && !base.isEmpty() && cli.startsWith(base + '/', Qt::CaseInsensitive)) return cli;
    }
    throw std::runtime_error("无法识别此 Pi 启动脚本。请在 Pi 设置中选择官方 pi.cmd、Pi CLI 的 .js 文件或可执行文件。");
}
}

PiAgentPanel::PiAgentPanel(QWidget *parent) : QWidget(parent) {
#ifdef Q_OS_WIN
    // The managed launcher starts another Node process. Give both a hidden console to inherit.
    process.setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments *arguments) {
        arguments->flags &= ~CREATE_NO_WINDOW;
        arguments->flags |= CREATE_NEW_CONSOLE;
        arguments->startupInfo->dwFlags |= STARTF_USESHOWWINDOW;
        arguments->startupInfo->wShowWindow = SW_HIDE;
    });
#endif
    setObjectName("piAgentPanel");
    auto *layout = new QVBoxLayout(this); layout->setContentsMargins(6, 4, 6, 4); layout->setSpacing(4);
    outputPanel = new QWidget(this); outputPanel->setObjectName("piOutputPanel");
    auto *outputLayout = new QVBoxLayout(outputPanel);
    auto *row = new QHBoxLayout;
    status = new QLabel(QStringLiteral("打开项目后连接本机 Pi；使用 Pi 已配置的模型和登录。"));
    status->setObjectName("piStatus"); status->setTextFormat(Qt::PlainText); status->setWordWrap(true);
    connectionButton = new QPushButton(QStringLiteral("连接 Pi")); connectionButton->setObjectName("piConnect"); row->addWidget(connectionButton);
    settingsButton = new QPushButton(QStringLiteral("Pi 设置…")); settingsButton->setObjectName("piSettings"); row->addWidget(settingsButton); 

    history = new QComboBox; history->setObjectName("piHistory"); history->setMinimumContentsLength(12); history->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    newSessionButton = new QPushButton(QStringLiteral("新会话")); newSessionButton->setObjectName("piNewSession");
    historyRefresh = new QPushButton(QStringLiteral("刷新历史")); historyRefresh->setObjectName("piRefreshHistory");
    row->insertWidget(0,history,1); row->insertWidget(1,newSessionButton); row->insertWidget(2,historyRefresh); outputLayout->addLayout(row); outputLayout->addWidget(status);
    connect(history,qOverload<int>(&QComboBox::activated),this,[this](int index){changeSession(history->itemData(index).toString());});
    connect(newSessionButton,&QPushButton::clicked,this,[this]{changeSession({});});
    connect(historyRefresh,&QPushButton::clicked,this,[this]{send({{"type","get_state"},{"id","history-state"}});});
    historyTimer.setSingleShot(true); connect(&historyTimer,&QTimer::timeout,this,[this]{report(QStringLiteral("会话读取超时，请重新连接。"));disconnectAgent();});
    conversation = new PiRichText; conversation->setObjectName("piConversation"); conversation->setReadOnly(true);
    conversation->setMinimumHeight(65); outputLayout->addWidget(conversation, 1);
    autoRefresh = new QCheckBox(QStringLiteral("每次任务结束后更新代码关系和 AI 解释")); autoRefresh->setObjectName("piAutoRefresh");
    autoRefresh->setToolTip(QStringLiteral("先刷新关系图，再用现有 AI 解析配置更新变更文件及受影响的解释，可能产生接口费用。默认关闭。")); outputLayout->addWidget(autoRefresh);
    auto *composer = new QHBoxLayout;
    input = new QPlainTextEdit; input->setObjectName("piInput"); input->setPlaceholderText(QStringLiteral(""));
    input->setFixedHeight(58); composer->addWidget(input, 1);
    sendButton = new QPushButton(QStringLiteral("发送")); sendButton->setObjectName("piSend"); auto *actions = new QVBoxLayout; actions->setSpacing(4); actions->addWidget(sendButton);
    stopButton = new QPushButton(QStringLiteral("停止")); stopButton->setObjectName("piStop"); actions->addWidget(stopButton); composer->addLayout(actions); layout->addLayout(composer);
    outputPanel->hide();
    try {
        plainPath(configPath()); QFile file(configPath());
        if (file.exists()) {
            check(file.open(QIODevice::ReadOnly), QStringLiteral("无法读取 Pi 设置。"));
            QJsonParseError error; const auto doc = QJsonDocument::fromJson(file.read(256 * 1024), &error);
            check(error.error == QJsonParseError::NoError && doc.object().value("schema").toInt() == 1, QStringLiteral("Pi 设置格式无效，请重新配置。"));
            piPath = doc.object().value("piPath").toString(); nodePath = doc.object().value("nodePath").toString();
            autoRefresh->setChecked(doc.object().value("autoRefresh").toBool());
        }
    } catch (const std::exception &e) { report(QString::fromUtf8(e.what())); }
    connect(autoRefresh, &QCheckBox::toggled, this, &PiAgentPanel::saveSettings);
    connect(settingsButton, &QPushButton::clicked, this, &PiAgentPanel::configure);
    connect(connectionButton, &QPushButton::clicked, this, [this] { if (process.state() == QProcess::NotRunning) connectAgent(); else disconnectAgent(); });
    connect(sendButton, &QPushButton::clicked, this, &PiAgentPanel::sendPrompt);
    connect(stopButton, &QPushButton::clicked, this, &PiAgentPanel::stopTask);
    input->installEventFilter(this);
    startupTimer.setSingleShot(true); stopTimer.setSingleShot(true);
    connect(&startupTimer, &QTimer::timeout, this, [this] { report(QStringLiteral("Pi 连接超时，请检查安装和登录状态。")); disconnectAgent(); });
    connect(&stopTimer, &QTimer::timeout, this, [this] { report(QStringLiteral("Pi 未及时停止，正在关闭后台进程。")); disconnectAgent(); });
    connect(&process, &QProcess::started, this, [this] {
#ifdef Q_OS_WIN
        // Cover launcher descendants and tool processes when the panel closes.
        job = CreateJobObjectW(nullptr, nullptr);
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION info{}; info.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        HANDLE child = OpenProcess(PROCESS_SET_QUOTA | PROCESS_TERMINATE, FALSE, DWORD(process.processId()));
        if (!job || !SetInformationJobObject(HANDLE(job), JobObjectExtendedLimitInformation, &info, sizeof(info)) || !child || !AssignProcessToJobObject(HANDLE(job), child)) {
            if (child) CloseHandle(child); if (job) CloseHandle(HANDLE(job)); job = nullptr;
            report(QStringLiteral("无法管理 Pi 后台进程树，连接已关闭。")); disconnectAgent(); return;
        }
        CloseHandle(child);
#endif
        send({{"type", "get_state"}, {"id", "handshake"}});
    });
    connect(&process, &QProcess::readyReadStandardOutput, this, &PiAgentPanel::consumeOutput);
    connect(&process, &QProcess::readyReadStandardError, this, [this] { report(QString::fromUtf8(process.readAllStandardError()).left(1000)); });
    connect(&process, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart) { startupTimer.stop(); ready = false; report(QStringLiteral("Pi 启动失败：") + process.errorString()); updateControls(); }
    });
    connect(&process, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this, [this](int code, QProcess::ExitStatus) {
        consumeOutput(); startupTimer.stop(); stopTimer.stop(); historyTimer.stop(); historyBusy=false; ready = false; shuttingDown = false;
#ifdef Q_OS_WIN
        if (job) { CloseHandle(HANDLE(job)); job = nullptr; }
#endif
        if (!closing) {
            report(QStringLiteral("Pi 已断开（退出码 %1）。").arg(code));
            if (running) settle(QStringLiteral("任务中断；检查已写入的源码变化。"));
        }
        updateControls();
        if (autoConnectPending && !root.isEmpty()) { autoConnectPending=false; QTimer::singleShot(0,this,&PiAgentPanel::connectAgent); }
    });
    updateControls();
}
PiAgentPanel::~PiAgentPanel() {
    closing = true; ready = false; process.disconnect(this);
    if (scanCancelled) scanCancelled->store(true);
    if (scanner) { scanner->wait(); delete scanner; }
    process.closeWriteChannel();
    if (!process.waitForFinished(600)) {
#ifdef Q_OS_WIN
        if (job) { TerminateJobObject(HANDLE(job), 1); }
#endif
        process.kill(); process.waitForFinished(1000);
    }
#ifdef Q_OS_WIN
    if (job) CloseHandle(HANDLE(job));
#endif
}
bool PiAgentPanel::isBusy() const { return running || scanning || updating || historyBusy; }
void PiAgentPanel::setProject(const QString &path) {
    const QString canonical = path.isEmpty() ? QString() : QFileInfo(path).canonicalFilePath();
    if (canonical == root) return;
    if (scanCancelled) scanCancelled->store(true);
    if (scanner) { scanner->wait(); delete scanner; scanner = nullptr; }
    running = scanning = updating = false; pendingPrompt.clear(); beforeHashes.clear();
    disconnectAgent(); root = canonical; sessionFile.clear(); history->clear(); conversation->clear(); input->clear();
    report(root.isEmpty() ? QStringLiteral("请先打开项目。") : QStringLiteral("当前项目：") + QDir::toNativeSeparators(root)); updateControls();
    autoConnectPending = !root.isEmpty();
    if (autoConnectPending && process.state()==QProcess::NotRunning) {
        autoConnectPending=false; const QString expected=root;
        QTimer::singleShot(0,this,[this,expected]{if(root==expected)connectAgent();});
    }
}
void PiAgentPanel::report(const QString &message) { status->setText(message); }
bool PiAgentPanel::projectSession(const QString &path) const {
    QFileInfo info(path); if(info.isSymLink()||!info.isFile())return false;
    QFile file(path); if(!file.open(QIODevice::ReadOnly))return false;
    const auto header=QJsonDocument::fromJson(file.readLine(65536)).object();
    return header.value("type")=="session" && QFileInfo(header.value("cwd").toString()).canonicalFilePath().compare(root,Qt::CaseInsensitive)==0;
}
void PiAgentPanel::refreshHistory() {
    const QSignalBlocker blocker(history); history->clear();
    if(!sessionFile.isEmpty()) {
        const auto files=QDir(QFileInfo(sessionFile).absolutePath()).entryInfoList({"*.jsonl"},QDir::Files,QDir::Time);
        for(const auto &file:files) if(projectSession(file.absoluteFilePath()))
            history->addItem(file.lastModified().toString("yyyy-MM-dd HH:mm")+" · "+file.completeBaseName().right(12),file.absoluteFilePath());
    }
    int selected=history->findData(sessionFile);
    if(selected<0){history->insertItem(0,QStringLiteral("当前会话"),sessionFile);selected=0;}history->setCurrentIndex(selected);
}
void PiAgentPanel::changeSession(const QString &path) {
    if(!ready||isBusy())return;
    if(!path.isEmpty()&&!projectSession(path)){report(QStringLiteral("会话不属于当前项目或已不可用。"));refreshHistory();return;}
    historyBusy=true;updateControls();historyTimer.start(15000);
    QJsonObject command{{"id","session-change"},{"type",path.isEmpty()?"new_session":"switch_session"}};
    if(!path.isEmpty())command["sessionPath"]=path;send(command);
}
void PiAgentPanel::showMessage(const QJsonObject &message) {
    const QString role=message.value("role").toString();
    if(role!="user"&&role!="assistant")return;
    if(role=="assistant") {
        if(message.value("stopReason")=="toolUse")return;
        for(const auto &part:message.value("content").toArray())if(part.toObject().value("type")=="toolCall")return;
    }
    const QString text=messageText(message);if(!text.trimmed().isEmpty())conversation->appendMessage(role,text);
}
void PiAgentPanel::saveSettings() {
    try {
        plainPath(configPath()); check(QDir().mkpath(sharedHome()), QStringLiteral("无法创建设置目录。"));
        QSaveFile file(configPath()); check(file.open(QIODevice::WriteOnly), QStringLiteral("无法保存 Pi 设置。"));
        const auto bytes = QJsonDocument(QJsonObject{{"schema", 1}, {"piPath", piPath}, {"nodePath", nodePath}, {"autoRefresh", autoRefresh->isChecked()}}).toJson();
        check(file.write(bytes) == bytes.size() && file.commit(), QStringLiteral("Pi 设置保存失败。"));
    } catch (const std::exception &e) { report(QString::fromUtf8(e.what())); }
}
void PiAgentPanel::configure() {
    QDialog dialog(this); dialog.setWindowTitle(QStringLiteral("vividcode · Pi 设置")); dialog.resize(640, 220);
    auto *layout = new QVBoxLayout(&dialog); auto *form = new QFormLayout; layout->addLayout(form);
    auto addPath = [&](const QString &label, const QString &value) {
        auto *row = new QWidget; auto *line = new QHBoxLayout(row); line->setContentsMargins(0,0,0,0);
        auto *edit = new QLineEdit(value); edit->setPlaceholderText(QStringLiteral("留空自动检测")); line->addWidget(edit, 1);
        auto *browse = new QPushButton(QStringLiteral("浏览…")); line->addWidget(browse); form->addRow(label, row);
        connect(browse, &QPushButton::clicked, &dialog, [&, edit, label] { const auto file = QFileDialog::getOpenFileName(&dialog, label, edit->text()); if (!file.isEmpty()) edit->setText(file); }); return edit;
    };
    auto *pi = addPath(QStringLiteral("Pi 路径"), piPath); auto *node = addPath(QStringLiteral("Node.js 路径"), nodePath);
    auto *help = new QLabel(QStringLiteral("支持官方 Pi 安装及 npm 安装的 pi.cmd、CLI .js 或可执行文件。模型、登录和工具沿用本机 Pi 配置；连接后可直接修改当前项目。")); help->setWordWrap(true); layout->addWidget(help);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel); layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept); connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    if (dialog.exec() == QDialog::Accepted) { piPath = pi->text().trimmed(); nodePath = node->text().trimmed(); saveSettings(); }
}
void PiAgentPanel::connectAgent() {
    if (root.isEmpty() || process.state() != QProcess::NotRunning || shuttingDown) return;
    try {
        QString command = piPath;
        if (command.isEmpty()) command = QStandardPaths::findExecutable("pi");
        if (command.isEmpty() && QFileInfo::exists(QDir::homePath() + "/.pi/agent/bin/pi.cmd")) command = QDir::homePath() + "/.pi/agent/bin/pi.cmd";
        check(!command.isEmpty() && QFileInfo(command).isFile(), QStringLiteral("找不到本机 Pi，请先安装并登录，或在 Pi 设置中选择路径。"));
        const auto script = piScript(command); QString program = command; QStringList args;
        if (!script.isEmpty()) {
            program = nodePath;
            if (program.isEmpty()) program = QStandardPaths::findExecutable("node");
            if (program.isEmpty()) program = qEnvironmentVariable("LOCALAPPDATA") + "/pi-node/current/node.exe";
            check(QFileInfo(program).isFile(), QStringLiteral("找不到 Node.js，请在 Pi 设置中指定路径。")); args.append(script);
        }
        args << "--mode" << "rpc";
        output.clear(); ready = false; process.setWorkingDirectory(root); process.setProgram(program); process.setArguments(args);
        auto env = QProcessEnvironment::systemEnvironment(); env.remove("NODE_OPTIONS"); env.remove("NODE_PATH"); process.setProcessEnvironment(env);
        conversation->setNode(script.isEmpty()?nodePath:program); report(QStringLiteral("正在连接本机 Pi…")); process.start(); startupTimer.start(30000); updateControls();
    } catch (const std::exception &e) { report(QString::fromUtf8(e.what())); }
}
void PiAgentPanel::disconnectAgent() {
    startupTimer.stop(); stopTimer.stop(); historyTimer.stop(); historyBusy=false; ready = false;
    if (process.state() != QProcess::NotRunning) {
        shuttingDown = true; process.closeWriteChannel();
        QTimer::singleShot(1200, this, [this] {
            if (!shuttingDown || process.state() == QProcess::NotRunning) return;
#ifdef Q_OS_WIN
            if (job) TerminateJobObject(HANDLE(job), 1);
#endif
            process.kill();
        });
    }
    updateControls();
}
void PiAgentPanel::send(QJsonObject command) {
    if (!command.contains("id")) command["id"] = QString::number(++serial);
    const auto bytes = QJsonDocument(command).toJson(QJsonDocument::Compact) + '\n';
    if (process.write(bytes) != bytes.size()) { report(QStringLiteral("无法向 Pi 写入命令。")); disconnectAgent(); }
}
bool PiAgentPanel::eventFilter(QObject *object, QEvent *event) {
    if (object == input && event->type() == QEvent::KeyPress) {
        auto *key = static_cast<QKeyEvent *>(event);
        if ((key->key() == Qt::Key_Return || key->key() == Qt::Key_Enter) && !(key->modifiers() & Qt::ShiftModifier)) {
            if (!key->isAutoRepeat()) sendPrompt();
            return true;
        }
    }
    return QWidget::eventFilter(object, event);
}
void PiAgentPanel::sendPrompt() {
    if (!ready || historyBusy || scanning || updating || shuttingDown || stopTimer.isActive() || root.isEmpty() || input->toPlainText().trimmed().isEmpty()) return;
    pendingPrompt = input->toPlainText();
    if (pendingPrompt.toUtf8().size() > 256 * 1024) { report(QStringLiteral("单次输入不能超过 256 KiB。")); pendingPrompt.clear(); return; }
    if (running) {
        showMessage({{"role","user"},{"content",pendingPrompt}});
        conversation->setThinking(true);
        send({{"type", "steer"}, {"message", pendingPrompt}});
        pendingPrompt.clear(); input->clear(); return;
    }
    showMessage({{"role","user"},{"content",pendingPrompt}}); conversation->setThinking(true);
    autoThisRun = autoRefresh->isChecked(); snapshot(true);
}
void PiAgentPanel::snapshot(bool before) {
    scanning = true; updateControls(); report(before ? QStringLiteral("正在记录任务前的源码状态…") : QStringLiteral("正在检查任务后的源码变化…"));
    scanCancelled = std::make_shared<std::atomic_bool>(false); const auto token = scanCancelled; const QString scanRoot = root;
    QPointer<PiAgentPanel> owner(this);
    scanner = QThread::create([owner, token, scanRoot, before] {
        QMap<QString, QByteArray> hashes; QString error;
        try {
            auto checkpoint = [token] { if (token->load()) throw std::runtime_error("cancelled"); };
            check(QFileInfo(scanRoot).isDir(), QStringLiteral("项目目录已不可用。"));
            const auto result = scanProject(scanRoot, readPolicy(scanRoot), checkpoint);
            check(!result.truncated && result.error.isEmpty(), QStringLiteral("文件扫描未完成；请调整文件上限后重试。"));
            for (const auto &relative : result.files) {
                checkpoint(); const QString path = QDir(scanRoot).filePath(relative); plainPath(path);
                check(QFileInfo(path).canonicalFilePath().startsWith(scanRoot + '/', Qt::CaseInsensitive), QStringLiteral("源码超出项目边界。"));
                QFile file(path); check(file.open(QIODevice::ReadOnly), relative + ": " + file.errorString());
                QCryptographicHash hash(QCryptographicHash::Sha256);
                while (!file.atEnd()) { checkpoint(); const auto bytes = file.read(1024 * 1024); check(!bytes.isEmpty() || file.error() == QFileDevice::NoError, relative + ": " + file.errorString()); hash.addData(bytes); }
                check(file.error() == QFileDevice::NoError, file.errorString()); hashes[relative] = hash.result();
            }
        } catch (const std::exception &e) { error = QString::fromUtf8(e.what()); }
        if (owner && !token->load()) QMetaObject::invokeMethod(owner, [owner, token, hashes, error, before] {
            if (!owner || token->load()) return;
            owner->scanner->wait(); delete owner->scanner; owner->scanner = nullptr; owner->scanning = false;
            if (!error.isEmpty()) { owner->conversation->setThinking(false); owner->report(error + QStringLiteral(" 可使用 F5 和顶部 AI 解析手动更新。")); owner->pendingPrompt.clear(); owner->updateControls(); return; }
            if (before) {
                owner->beforeHashes = hashes;
                if (!owner->ready) { owner->conversation->setThinking(false); owner->report(QStringLiteral("Pi 已断开，请重新连接后发送。")); owner->updateControls(); return; }
                owner->running = true; owner->startedThisPrompt = false; owner->promptId = QString::number(++owner->serial);

                owner->send({{"id", owner->promptId}, {"type", "prompt"}, {"message", owner->pendingPrompt}});
                owner->pendingPrompt.clear(); owner->input->clear(); owner->status->setText(QStringLiteral("Pi 正在执行…"));
            } else {
                QSet<QString> paths; for (auto it = hashes.begin(); it != hashes.end(); ++it) paths.insert(it.key());
                for (auto it = owner->beforeHashes.begin(); it != owner->beforeHashes.end(); ++it) paths.insert(it.key());
                QStringList changed; for (const auto &path : paths) if (hashes.value(path) != owner->beforeHashes.value(path)) changed.append(path); changed.sort();
                owner->report(QStringLiteral("任务结束，%1 个源码文件发生变化。").arg(changed.size()));
                if (owner->autoThisRun) { owner->updating = true; emit owner->refreshRequested(changed); }
            }
            owner->updateControls();
        }, Qt::QueuedConnection);
    }); scanner->start();
}
void PiAgentPanel::stopTask() {
    if (!running || stopTimer.isActive()) return;
    send({{"type", "clear_queue"}}); send({{"type", "abort"}, {"id", "abort-task"}});
    report(QStringLiteral("正在停止 Pi；已写入的文件会保留。")); stopTimer.start(10000); updateControls();
}
void PiAgentPanel::settle(const QString &message) {
    conversation->setThinking(false);
    if (!running) return; running = false; stopTimer.stop(); report(message); if(ready)send({{"type","get_state"},{"id","history-state"}}); snapshot(false);
}
void PiAgentPanel::consumeOutput() {
    output += process.readAllStandardOutput();
    while (output.contains('\n')) {
        const auto index = output.indexOf('\n'); auto line = output.left(index); output.remove(0, index + 1); if (line.endsWith('\r')) line.chop(1);
        if (line.isEmpty()) continue;
        if (line.size() > 16 * 1024 * 1024) { report(QStringLiteral("Pi 单条消息超过 16 MiB，已断开。")); disconnectAgent(); return; }
        QJsonParseError error; const auto doc = QJsonDocument::fromJson(line, &error);
        if (error.error != QJsonParseError::NoError || !doc.isObject()) { report(QStringLiteral("Pi 返回了无效协议数据，已断开。")); disconnectAgent(); return; }
        record(doc.object());
    }
    if (output.size() > 16 * 1024 * 1024) { output.clear(); report(QStringLiteral("Pi 消息过大，已断开。")); disconnectAgent(); }
}
void PiAgentPanel::record(const QJsonObject &object) {
    const auto type = object.value("type").toString();
    if (type == "response") {
        const QString id = object.value("id").toString();
        if (!object.value("success").toBool()) {
            report(QStringLiteral("Pi 请求失败：") + object.value("error").toString());
            if(id=="session-change"||id=="history-state"||id=="history-messages"){historyBusy=false;historyTimer.stop();refreshHistory();updateControls();}
            if (id == "handshake") disconnectAgent();
            if (id == promptId && running) { running = false; updateControls(); }
            return;
        }
        if(id=="session-change") {
            if(object.value("data").toObject().value("cancelled").toBool()){historyBusy=false;historyTimer.stop();refreshHistory();report(QStringLiteral("会话切换已取消。"));updateControls();return;}
            conversation->clear();input->clear();send({{"type","get_state"},{"id","history-state"}});send({{"type","get_messages"},{"id","history-messages"}});return;
        }
        if(id=="history-state") {sessionFile=QDir::fromNativeSeparators(object.value("data").toObject().value("sessionFile").toString());refreshHistory();return;}
        if(id=="history-messages") {
            conversation->clear();for(const auto &message:object.value("data").toObject().value("messages").toArray())showMessage(message.toObject());
            historyBusy=false;historyTimer.stop();report(QStringLiteral("会话已就绪。"));updateControls();return;
        }
        if (id == "handshake") {
            startupTimer.stop(); ready = true; sessionFile=QDir::fromNativeSeparators(object.value("data").toObject().value("sessionFile").toString()); refreshHistory(); historyBusy=true;historyTimer.start(15000);send({{"type","get_messages"},{"id","history-messages"}}); const auto model = object.value("data").toObject().value("model").toObject();
            report(QStringLiteral("已连接 Pi · %1").arg(model.value("id").toString(QStringLiteral("模型由 Pi 配置")))); updateControls();
        } else if (id == promptId && object.value("data").toObject().value("disposition") == "handled" && running && !startedThisPrompt) {
            // A consumed extension command need not start an agent run.
            settle(QStringLiteral("Pi 已处理此命令。"));
        } else if (id == "abort-task" && running) settle(QStringLiteral("Pi 已停止。"));
    } else if (type == "agent_start") {
        startedThisPrompt = true;
        if (!running && !scanning && !updating) { running = true; autoThisRun = false; beforeHashes.clear(); }
        status->setText(QStringLiteral("Pi 正在执行…")); updateControls();
    } else if (type == "message_end" && object.value("message").toObject().value("role")=="assistant") {
        const auto message=object.value("message").toObject();showMessage(message);
        if(!message.value("errorMessage").toString().isEmpty())report(message.value("errorMessage").toString());
    } else if (type == "agent_settled") settle(object.value("aborted").toBool() ? QStringLiteral("Pi 已停止。") : QStringLiteral("Pi 本轮任务已完成。"));
    else if (type == "extension_ui_request") requestUi(object);
    else if (type == "auto_retry_start" || type == "auto_compaction_start") status->setText(QStringLiteral("Pi 正在重试或整理上下文…"));
    // agent_end is deliberately not completion: retries/follow-up may still run.
}
void PiAgentPanel::requestUi(const QJsonObject &object) {
    const auto method = object.value("method").toString(); const auto id = object.value("id").toString();
    if (method == "notify" || method == "setStatus" || method == "setTitle") { report(object.value("message").toString(object.value("text").toString(object.value("title").toString()))); return; }
    if (method == "set_editor_text") { input->setPlainText(object.value("text").toString()); return; }
    if (method == "setWidget") return;
    if (!QStringList{"select", "confirm", "input", "editor"}.contains(method)) { send({{"type", "extension_ui_response"}, {"id", id}, {"cancelled", true}}); return; }
    // Non-modal dialogs keep stdout draining, and are dismissed on disconnect.
    auto *dialog = new QDialog(this); dialog->setAttribute(Qt::WA_DeleteOnClose); dialog->setWindowTitle("Pi · " + object.value("title").toString());
    auto *layout = new QVBoxLayout(dialog); auto *label = new QLabel(object.value("message").toString()); label->setTextFormat(Qt::PlainText); label->setWordWrap(true); layout->addWidget(label);
    QComboBox *select = nullptr; QPlainTextEdit *editor = nullptr;
    if (method == "select") { select = new QComboBox; for (const auto &item : object.value("options").toArray()) select->addItem(item.toString()); layout->addWidget(select); }
    if (method == "input" || method == "editor") { editor = new QPlainTextEdit; editor->setPlainText(object.value("prefill").toString()); editor->setPlaceholderText(object.value("placeholder").toString()); layout->addWidget(editor); }
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel); layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, dialog, &QDialog::accept); connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::reject);
    connect(&process, qOverload<int,QProcess::ExitStatus>(&QProcess::finished), dialog, &QDialog::reject);
    connect(dialog, &QDialog::finished, this, [this, id, method, select, editor](int result) {
        if (!ready) return; QJsonObject response{{"type", "extension_ui_response"}, {"id", id}};
        if (result != QDialog::Accepted) response["cancelled"] = true;
        else if (method == "confirm") response["confirmed"] = true;
        else response["value"] = select ? select->currentText() : editor->toPlainText(); send(response);
    });
    const int timeout = object.value("timeout").toInt(); if (timeout > 0) QTimer::singleShot(timeout, dialog, &QDialog::reject); dialog->show();
}
void PiAgentPanel::finishRefresh(const QString &message) { updating = false; report(message); updateControls(); }
void PiAgentPanel::updateControls() {
    const bool disconnected = process.state() == QProcess::NotRunning;
    history->setEnabled(ready&&!isBusy());newSessionButton->setEnabled(ready&&!isBusy());historyRefresh->setEnabled(ready&&!isBusy());
    connectionButton->setText(disconnected ? QStringLiteral("连接 Pi") : QStringLiteral("断开 Pi"));
    connectionButton->setEnabled(!root.isEmpty() && !isBusy() && !shuttingDown);
    settingsButton->setEnabled(disconnected && !isBusy()); sendButton->setEnabled(ready && !historyBusy && !scanning && !updating && !shuttingDown && !stopTimer.isActive());
    stopButton->setEnabled(running && !stopTimer.isActive()); autoRefresh->setEnabled(!isBusy());
}
}
