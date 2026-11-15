#include "VividCode.h"
#include "AnalysisEnvironment.h"
#include "ProjectScanner.h"
#include "ProjectStore.h"
#include "SemanticPanel.h"
#include "SourcePopup.h"
#include "AiExplanation.h"
#include "RelationGraph.h"
#include "PiAgent.h"
#include <QtWidgets>

VividCodeWindow::VividCodeWindow(QWidget *parent) : QMainWindow(parent)
{
    setObjectName("VividCodeWindow");
    setWindowTitle(QStringLiteral("vividcode"));
    resize(1320, 900);
    setMinimumSize(900, 600);
    auto *open = new QAction(QStringLiteral("打开项目…"), this);
    open->setObjectName("openProjectAction");
    addAction(open);
    open->setShortcut(QKeySequence::Open);
    auto *refresh = new QAction(QStringLiteral("刷新文件"), this);
    refresh->setObjectName("refreshProjectAction");
    addAction(refresh);
    refresh->setShortcut(QKeySequence::Refresh);
    sourcePopup = new SourcePopup(this);
    semantic = new SemanticPanel;
    auto *toolbar = addToolBar(QStringLiteral("分析设置"));
    toolbar->setObjectName("analysisSettingsBar"); toolbar->setMovable(false);
    toolbar->addAction(open);
    toolbar->addAction(refresh);
    toolbar->addSeparator();
    toolbar->addWidget(semantic->analysisControls());
    toolbar->addSeparator();
    ai = new VividCode::AiExplanation(this);
    pi = new VividCode::PiAgentPanel;

    connect(pi, &VividCode::PiAgentPanel::refreshRequested, this, [this](const QStringList &changed) {
        piChangedFiles = changed; piRefreshRoot = rootPath; piRefreshPending = true; refreshAfterPi();
    });
    connect(semantic, &SemanticPanel::settled, this, [this] {
        if (!piAwaitingGraph || piRefreshRoot != rootPath) return;
        piAwaitingGraph = false;
        if (piChangedFiles.isEmpty()) { pi->finishRefresh(QStringLiteral("关系图已检查；源码无变化，未请求 AI 解释。")); return; }
        piAwaitingAi = true;
        pi->report(QStringLiteral("关系图检查完成，正在更新变更文件和受影响的 AI 解释…"));
        auto dependencies = semantic->fileDependencies();
        for (auto it = piPreviousDependencies.begin(); it != piPreviousDependencies.end(); ++it) {
            dependencies[it.key()].append(it.value()); dependencies[it.key()].removeDuplicates();
        }
        ai->start(dependencies, piChangedFiles);
    });
    connect(ai, &VividCode::AiExplanation::finished, this, [this] {
        if (!piAwaitingAi || piRefreshRoot != rootPath) return;
        piAwaitingAi = false;
        pi->finishRefresh(ai->report().section('\n', -1));
    });
    auto *aiSettings = new QAction(QStringLiteral("AI 设置"), this); aiSettings->setObjectName("aiSettingsAction");
    auto *explain = toolbar->addAction(QStringLiteral("AI 解析")); explain->setObjectName("aiExplainAction");
    auto *cancelAi = toolbar->addAction(QStringLiteral("取消 AI")); cancelAi->setObjectName("aiCancelAction"); cancelAi->setEnabled(false); cancelAi->setVisible(false);
    toolbar->addAction(aiSettings);
    connect(aiSettings, &QAction::triggered, this, [this] {
        if (VividCode::editAiSettings(this)) ai->setProject(rootPath, files, completeFileScan);
    });
    connect(explain, &QAction::triggered, this, [this] {
        if (rootPath.isEmpty()) { statusBar()->showMessage(QStringLiteral("请先打开项目，再配置 AI 并点击 AI 解析。")); return; }
        const auto result = scanProject(rootPath);
        if (!result.error.isEmpty()) { statusBar()->showMessage(result.error); return; }
        files = result.files; completeFileScan = !result.truncated;
        ai->setProject(rootPath, files, completeFileScan); rebuildViews();
        ai->start(semantic->fileDependencies());
    });
    connect(cancelAi, &QAction::triggered, ai, &VividCode::AiExplanation::cancel);
    connect(ai, &VividCode::AiExplanation::statusChanged, this, [this, explain, cancelAi, aiSettings](const QString &message) {
        explain->setEnabled(!ai->isBusy()); cancelAi->setEnabled(ai->isBusy()); cancelAi->setVisible(ai->isBusy()); aiSettings->setEnabled(!ai->isBusy());
        statusBar()->showMessage(message);
    });
    semantic->findChild<RelationGraph *>()->setFileExplanationProvider([this](const QString &path) {
        return ai->tooltip(QDir::isAbsolutePath(path) ? QDir(rootPath).relativeFilePath(path) : path);
    });

    auto *body = new QWidget;
    auto *layout = new QVBoxLayout(body);
    layout->setContentsMargins(16, 12, 16, 12);
    projectLabel = new QLabel(QStringLiteral("未打开项目"));
    projectLabel->setTextFormat(Qt::PlainText);
    projectLabel->setWordWrap(true);
    layout->addWidget(projectLabel);
    auto *splitter = new QSplitter;
    auto *sidebar = new QWidget;
    auto *sideLayout = new QVBoxLayout(sidebar);
    sideLayout->setContentsMargins(0, 0, 8, 0);
    auto *sidebarTabs = new QTabWidget; sidebarTabs->setObjectName("sidebarViews");
    auto *filePage = new QWidget;
    auto *fileLayout = new QVBoxLayout(filePage); fileLayout->setContentsMargins(0, 0, 0, 0);
    filter = new QLineEdit;
    filter->setObjectName("fileFilter");
    filter->setPlaceholderText(QStringLiteral("筛选文件名或路径…"));
    filter->setClearButtonEnabled(true);
    tree = new QTreeWidget;
    tree->setObjectName("fileTree");
    tree->setHeaderHidden(true);
    fileLayout->addWidget(filter);
    fileLayout->addWidget(tree);
    sidebarTabs->addTab(filePage, QStringLiteral("项目文件"));
    sidebarTabs->addTab(semantic->structureView(), QStringLiteral("结构与关系"));
    sideLayout->addWidget(sidebarTabs);
    splitter->addWidget(sidebar);
    connect(semantic, &SemanticPanel::sourceRequested, this, [this](const QString &path, int line, int column) {
        const QString absolute = QFileInfo(QDir(rootPath).filePath(path)).canonicalFilePath();
        const QString canonicalRoot = QFileInfo(rootPath).canonicalFilePath();
        if (absolute.isEmpty() || !absolute.startsWith(canonicalRoot + '/', Qt::CaseInsensitive)) {
            statusBar()->showMessage(QStringLiteral("外部符号或文件不可用：") + path); return;
        }
        const QString relative = QDir(rootPath).relativeFilePath(absolute);
        if (!files.contains(relative)) files.append(relative);
        showFile(relative, line, column);
    });
    auto *workspace = new QWidget;
    auto *workspaceLayout = new QVBoxLayout(workspace); workspaceLayout->setContentsMargins(0, 0, 0, 0);
    auto *workspaceTabs = new QTabWidget; workspaceTabs->setObjectName("workspaceTabs");
    workspaceTabs->addTab(semantic, QStringLiteral("关系图"));
    workspaceTabs->addTab(pi->outputView(), QStringLiteral("pi"));
    workspaceLayout->addWidget(workspaceTabs, 1);
    workspaceLayout->addWidget(pi);
    splitter->addWidget(workspace);
    splitter->setSizes({350, 970});
    layout->addWidget(splitter, 1);
    setCentralWidget(body);
    statusBar()->showMessage(QStringLiteral("就绪"));
    connect(open, &QAction::triggered, this, [this] {
        const QString directory = QFileDialog::getExistingDirectory(this, QStringLiteral("选择项目目录"), rootPath);
        if (!directory.isEmpty()) openProject(directory);
    });
    connect(refresh, &QAction::triggered, this, [this] {
        if (rootPath.isEmpty()) return;
        if (!openProject(rootPath)) {
            files.clear(); currentFile.clear(); sourcePopup->close(); rebuildViews();
            piRefreshPending = piAwaitingGraph = piAwaitingAi = false; pi->setProject({});
            semantic->setProject({}); ai->setProject({}, {}, false); completeFileScan = false;
            statusBar()->showMessage(QStringLiteral("项目目录已不可用，请重新打开项目。"));
        }
    });
    connect(filter, &QLineEdit::textChanged, this, [this] { rebuildViews(); });
    const auto focusProjectFile = [this, workspaceTabs](QTreeWidgetItem *item) {
        if (!item || item->data(0, Qt::UserRole).toString().isEmpty()) return;
        currentFile = item->data(0, Qt::UserRole).toString();
        workspaceTabs->setCurrentWidget(semantic);
        if (!semantic->focusFile(currentFile))
            statusBar()->showMessage(QStringLiteral("该文件尚无可定位的图谱节点：") + currentFile);
        else statusBar()->showMessage(currentFile);
    };
    connect(tree, &QTreeWidget::currentItemChanged, this, focusProjectFile);
    // 再次点击当前文件也能找回已经移出视口的节点。
    connect(tree, &QTreeWidget::itemClicked, this, [focusProjectFile](QTreeWidgetItem *item) { focusProjectFile(item); });
    connect(semantic, &SemanticPanel::refreshRequested, refresh, &QAction::trigger);
    tree->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(tree, &QTreeWidget::customContextMenuRequested, this, [this, open, refresh](const QPoint &point) {
        auto *menu = new QMenu(tree); menu->setAttribute(Qt::WA_DeleteOnClose); menu->setObjectName("fileContextMenu");
        if (auto *item = tree->itemAt(point)) {
            const QString path = item->data(0, Qt::UserRole).toString();
            if (!path.isEmpty()) { auto *source = menu->addAction(QStringLiteral("查看源代码")); source->setObjectName("fileViewSourceAction"); connect(source, &QAction::triggered, this, [this, path] { showFile(path); }); }
        }
        menu->addAction(refresh); menu->addAction(open); semantic->addContextActions(menu);
        menu->popup(tree->viewport()->mapToGlobal(point));
    });
    rebuildViews();
}

VividCodeWindow::~VividCodeWindow() {
    piRefreshPending = piAwaitingGraph = piAwaitingAi = false;
    disconnect(ai, nullptr, this, nullptr); disconnect(semantic, nullptr, this, nullptr); disconnect(pi, nullptr, this, nullptr);
    delete pi; delete ai;
}

void VividCodeWindow::refreshAfterPi() {
    if (!piRefreshPending || piRefreshRoot != rootPath) return;
    if (semantic->isBusy() || ai->isBusy()) {
        pi->report(QStringLiteral("等待当前分析完成后更新…"));
        QTimer::singleShot(300, this, &VividCodeWindow::refreshAfterPi); return;
    }
    piRefreshPending = false; piAwaitingGraph = true;
    piPreviousDependencies = semantic->fileDependencies();
    // openProject can synchronously emit settled() when no analyzer is available.
    if (!openProject(rootPath)) {
        piAwaitingGraph = false; pi->finishRefresh(QStringLiteral("项目刷新失败；请检查目录并使用 F5 重试。"));
    }
}

bool VividCodeWindow::openProject(const QString &directory)
{
    const QFileInfo info(directory);
    if (!info.isDir() || !info.isReadable()) {
        statusBar()->showMessage(QStringLiteral("无法打开目录：") + directory);
        return false;
    }
    const QString newRoot = info.absoluteFilePath();
    const bool sameProject = rootPath == newRoot;
    if (!sameProject) {
        piRefreshPending = piAwaitingGraph = piAwaitingAi = false;
        piChangedFiles.clear(); piRefreshRoot.clear();
    }
    sourcePopup->close();
    const ScanResult result = scanProject(newRoot);
    if (!result.error.isEmpty()) { statusBar()->showMessage(result.error); return false; }
    rootPath = newRoot;
    pi->setProject(rootPath);
    files = result.files;
    completeFileScan = !result.truncated;
    ai->setProject(rootPath, files, completeFileScan);
    if (!sameProject || !files.contains(currentFile)) {
        currentFile.clear();
    }
    { const QSignalBlocker blocker(filter); filter->clear(); }
    projectLabel->setText(QStringLiteral("项目：%1  |  %2 个源码文件").arg(QDir::toNativeSeparators(rootPath)).arg(files.size()));
    rebuildViews();
    statusBar()->showMessage(QStringLiteral("扫描完成 · %1 个文件 · 排除 %2 个生成/依赖目录%3")
        .arg(files.size()).arg(result.skippedDirectories)
        .arg(result.truncated ? QStringLiteral(" · 已达扫描上限，可用 vividcode -c 设置，或谨慎使用 -sg") : QString()));
    const auto record = VividCode::recordIfInitialized(rootPath);
    if (!record.message.isEmpty())
        statusBar()->showMessage(statusBar()->currentMessage() + (record.ok ? " | " : QStringLiteral(" | 记录更新失败：")) + record.message);
    semantic->setProject(rootPath, files, !result.truncated);
    return true;
}

void VividCodeWindow::rebuildViews()
{
    const QSignalBlocker treeBlocker(tree);
    tree->clear();
    QMap<QString, QTreeWidgetItem *> folders;
    const QString query = filter->text().trimmed();
    for (const QString &path : files) {
        if (!path.contains(query, Qt::CaseInsensitive)) continue;
        const QStringList parts = path.split('/');
        QTreeWidgetItem *parent = nullptr;
        QString prefix;
        for (int i = 0; i + 1 < parts.size(); ++i) {
            prefix += parts[i] + '/';
            if (!folders.contains(prefix)) {
                auto *folder = parent ? new QTreeWidgetItem(parent) : new QTreeWidgetItem(tree);
                folder->setText(0, parts[i]);
                folders.insert(prefix, folder);
            }
            parent = folders.value(prefix);
        }
        auto *item = parent ? new QTreeWidgetItem(parent) : new QTreeWidgetItem(tree);
        item->setText(0, parts.last());
        item->setToolTip(0, path);
        item->setData(0, Qt::UserRole, path);
        if (path == currentFile) tree->setCurrentItem(item);
    }
    tree->expandToDepth(1);
}

void VividCodeWindow::showFile(const QString &relativePath, int line, int column)
{
    if (!files.contains(relativePath)) return;
    currentFile = relativePath;
    QFile file(QDir(rootPath).filePath(relativePath));
    const QString canonical = QFileInfo(file).canonicalFilePath();
    if (canonical.isEmpty() || !canonical.startsWith(QFileInfo(rootPath).canonicalFilePath() + '/', Qt::CaseInsensitive)) {
        statusBar()->showMessage(QStringLiteral("项目外文件或文件不可用：") + relativePath); return;
    }
    if (!file.open(QIODevice::ReadOnly)) {
        statusBar()->showMessage(QStringLiteral("无法读取文件：") + file.errorString());
        return;
    }
    bool forced = false;
    try { forced = VividCode::readPolicy(rootPath).force; }
    catch (const std::exception &error) { statusBar()->showMessage(QString::fromUtf8(error.what())); return; }
    const qint64 maxBytes = forced ? file.size() : 2 * 1024 * 1024;
    if (file.size() > maxBytes) {
        statusBar()->showMessage(QStringLiteral("该文件超过 2 MiB，请使用外部编辑器查看。"));
        return;
    }
    const QByteArray bytes = file.read(maxBytes);
    const bool pythonFile = QSet<QString>{"py", "pyw", "pyi"}.contains(QFileInfo(file).suffix().toLower());
    const QString firstLines = QString::fromLatin1(bytes).section('\n', 0, 1);
    const QRegularExpression cookie("^[ \\t\\f]*#.*?coding[:=][ \\t]*([-\\w.]+)", QRegularExpression::MultilineOption);
    const QString encodingCookie = pythonFile ? cookie.match(firstLines).captured(1).toLower().replace('_', '-') : QString();
    QString text;
    if (pythonFile && !encodingCookie.isEmpty() && encodingCookie != "utf-8" && encodingCookie != "utf8" && encodingCookie != "utf-8-sig") {
        // Use Python's own encoding-cookie/BOM rules, also for GBK source files.
        const QString base = VividCode::sharedRuntime() + "/analyzer";
        QProcess reader;
        try {
            const auto python = VividCode::localPython(); reader.setProgram(python.program);
            reader.setArguments(python.prefix + QStringList{base + "/python_indexer.py", "source", "--project", rootPath, "--file", relativePath});
        } catch (const std::exception &e) { statusBar()->showMessage(QString::fromUtf8(e.what())); return; }
        reader.start();
        if (!reader.waitForFinished(5000)) {
            reader.kill(); reader.waitForFinished(1000);
            statusBar()->showMessage(QStringLiteral("Python 源码读取失败或超时：") + reader.errorString()); return;
        }
        const auto result = QJsonDocument::fromJson(reader.readAllStandardOutput()).object();
        if (reader.exitStatus() != QProcess::NormalExit || reader.exitCode() || !result.value("ok").toBool()) {
            statusBar()->showMessage(QStringLiteral("Python 源码读取失败：") + result.value("message").toString(QString::fromUtf8(reader.readAllStandardError()))); return;
        }
        text = result.value("text").toString();
    } else {
        const auto encoding = QStringDecoder::encodingForData(bytes).value_or(QStringConverter::Utf8);
        QStringDecoder decoder(encoding);
        text = decoder(bytes);
        if (decoder.hasError()) { statusBar()->showMessage(QStringLiteral("源码编码无效：") + relativePath); return; }
    }
    const QString requestedRoot = rootPath;
    // 等右键菜单完成关闭/销毁再打开弹窗，避免 Qt 的菜单弹出栈关闭新窗口。
    QTimer::singleShot(0, this, [this, text, line, column, requestedRoot] {
        if (rootPath != requestedRoot) return;
        sourcePopup->showCode(text, line, column,
            mapToGlobal(QPoint(qMax(0, (width() - 760) / 2), qMax(0, (height() - 480) / 2))));
    });
}
