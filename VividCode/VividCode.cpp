#include "UiLanguage.h"
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
    auto *open = new QAction(UiLanguage::text("Open Project..."), this);
    open->setObjectName("openProjectAction");
    addAction(open);
    open->setShortcut(QKeySequence::Open);
    auto *refresh = new QAction(UiLanguage::text("Refresh Files"), this);
    refresh->setObjectName("refreshProjectAction");
    addAction(refresh);
    refresh->setShortcut(QKeySequence::Refresh);
    sourcePopup = new SourcePopup(this);
    semantic = new SemanticPanel;
    auto *toolbar = addToolBar(UiLanguage::text("Analysis Settings"));
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
        if (piChangedFiles.isEmpty()) { pi->finishRefresh(UiLanguage::text("Graph checked. Source unchanged; no AI explanations requested.")); return; }
        piAwaitingAi = true;
        pi->report(UiLanguage::text("Graph check complete. Updating changed files and affected AI explanations..."));
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
    auto *aiSettings = new QAction(UiLanguage::text("AI Settings"), this); aiSettings->setObjectName("aiSettingsAction");
    auto *explain = toolbar->addAction(UiLanguage::text("AI Analyze")); explain->setObjectName("aiExplainAction");
    auto *cancelAi = toolbar->addAction(UiLanguage::text("Cancel AI")); cancelAi->setObjectName("aiCancelAction"); cancelAi->setEnabled(false); cancelAi->setVisible(false);
    toolbar->addAction(aiSettings);
    toolbar->addSeparator();
    auto *languageLabel = new QLabel(UiLanguage::text("Language / 语言"));
    toolbar->addWidget(languageLabel);
    auto *language = new QComboBox; language->setObjectName("interfaceLanguage");
    language->addItem(QStringLiteral("简体中文"), "zh"); language->addItem("English", "en");
    language->setCurrentIndex(language->findData(UiLanguage::preference()));
    language->setToolTip(UiLanguage::text("Language changes take effect next time you open vividcode."));
    language->setAccessibleName(UiLanguage::text("Language / 语言"));
    toolbar->addWidget(language);
    connect(language, &QComboBox::currentIndexChanged, this, [this, language](int) {
        const auto error = UiLanguage::save(language->currentData().toString());
        if (!error.isEmpty()) {
            const QSignalBlocker blocker(language);
            language->setCurrentIndex(language->findData(UiLanguage::preference()));
            QMessageBox::warning(this, "vividcode", UiLanguage::text("Could not save interface language: ") + error);
            return;
        }
        statusBar()->showMessage(UiLanguage::text("Language saved. Reopen vividcode to apply it."));
    });
    connect(aiSettings, &QAction::triggered, this, [this] {
        if (VividCode::editAiSettings(this)) ai->setProject(rootPath, files, completeFileScan);
    });
    connect(explain, &QAction::triggered, this, [this] {
        if (rootPath.isEmpty()) { statusBar()->showMessage(UiLanguage::text("Open a project, configure AI Settings, then click AI Analyze.")); return; }
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
    projectLabel = new QLabel(UiLanguage::text("No project open"));
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
    filter->setPlaceholderText(UiLanguage::text("Filter by file name or path..."));
    filter->setClearButtonEnabled(true);
    tree = new QTreeWidget;
    tree->setObjectName("fileTree");
    tree->setHeaderHidden(true);
    fileLayout->addWidget(filter);
    fileLayout->addWidget(tree);
    sidebarTabs->addTab(filePage, UiLanguage::text("Project Files"));
    sidebarTabs->addTab(semantic->structureView(), UiLanguage::text("Structure and Relations"));
    sideLayout->addWidget(sidebarTabs);
    splitter->addWidget(sidebar);
    connect(semantic, &SemanticPanel::sourceRequested, this, [this](const QString &path, int line, int column) {
        const QString absolute = QFileInfo(QDir(rootPath).filePath(path)).canonicalFilePath();
        const QString canonicalRoot = QFileInfo(rootPath).canonicalFilePath();
        if (absolute.isEmpty() || !absolute.startsWith(canonicalRoot + '/', Qt::CaseInsensitive)) {
            statusBar()->showMessage(UiLanguage::text("External symbol or file unavailable: ") + path); return;
        }
        const QString relative = QDir(rootPath).relativeFilePath(absolute);
        if (!files.contains(relative)) files.append(relative);
        showFile(relative, line, column);
    });
    auto *workspace = new QWidget;
    auto *workspaceLayout = new QVBoxLayout(workspace); workspaceLayout->setContentsMargins(0, 0, 0, 0);
    auto *workspaceTabs = new QTabWidget; workspaceTabs->setObjectName("workspaceTabs");
    workspaceTabs->addTab(semantic, UiLanguage::text("Relations"));
    workspaceTabs->addTab(pi->outputView(), QStringLiteral("pi"));
    workspaceLayout->addWidget(workspaceTabs, 1);
    workspaceLayout->addWidget(pi);
    splitter->addWidget(workspace);
    splitter->setSizes({350, 970});
    layout->addWidget(splitter, 1);
    setCentralWidget(body);
    statusBar()->showMessage(UiLanguage::text("Ready"));
    connect(open, &QAction::triggered, this, [this] {
        const QString directory = QFileDialog::getExistingDirectory(this, UiLanguage::text("Select Project Directory"), rootPath);
        if (!directory.isEmpty()) openProject(directory);
    });
    connect(refresh, &QAction::triggered, this, [this] {
        if (rootPath.isEmpty()) return;
        if (!openProject(rootPath)) {
            files.clear(); currentFile.clear(); sourcePopup->close(); rebuildViews();
            piRefreshPending = piAwaitingGraph = piAwaitingAi = false; pi->setProject({});
            semantic->setProject({}); ai->setProject({}, {}, false); completeFileScan = false;
            statusBar()->showMessage(UiLanguage::text("Project directory is unavailable. Open the project again."));
        }
    });
    connect(filter, &QLineEdit::textChanged, this, [this] { rebuildViews(); });
    const auto focusProjectFile = [this, workspaceTabs](QTreeWidgetItem *item) {
        if (!item || item->data(0, Qt::UserRole).toString().isEmpty()) return;
        currentFile = item->data(0, Qt::UserRole).toString();
        workspaceTabs->setCurrentWidget(semantic);
        if (!semantic->focusFile(currentFile))
            statusBar()->showMessage(UiLanguage::text("No graph node available for this file: ") + currentFile);
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
            if (!path.isEmpty()) { auto *source = menu->addAction(UiLanguage::text("View Source")); source->setObjectName("fileViewSourceAction"); connect(source, &QAction::triggered, this, [this, path] { showFile(path); }); }
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
        pi->report(UiLanguage::text("Waiting for current analysis before updating..."));
        QTimer::singleShot(300, this, &VividCodeWindow::refreshAfterPi); return;
    }
    piRefreshPending = false; piAwaitingGraph = true;
    piPreviousDependencies = semantic->fileDependencies();
    // openProject can synchronously emit settled() when no analyzer is available.
    if (!openProject(rootPath)) {
        piAwaitingGraph = false; pi->finishRefresh(UiLanguage::text("Project refresh failed. Check the directory and press F5 to retry."));
    }
}

bool VividCodeWindow::openProject(const QString &directory)
{
    const QFileInfo info(directory);
    if (!info.isDir() || !info.isReadable()) {
        statusBar()->showMessage(UiLanguage::text("Could not open directory: ") + directory);
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
    projectLabel->setText(UiLanguage::text("Project: %1  |  %2 source files").arg(QDir::toNativeSeparators(rootPath)).arg(files.size()));
    rebuildViews();
    statusBar()->showMessage(UiLanguage::text("Scan complete - %1 files - %2 generated/dependency directories excluded%3")
        .arg(files.size()).arg(result.skippedDirectories)
        .arg(result.truncated ? UiLanguage::text(" - Scan limit reached; use vividcode -c to adjust or -sg with care") : QString()));
    const auto record = VividCode::recordIfInitialized(rootPath);
    if (!record.message.isEmpty())
        statusBar()->showMessage(statusBar()->currentMessage() + (record.ok ? " | " : UiLanguage::text(" | Record update failed: ")) + record.message);
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
        statusBar()->showMessage(UiLanguage::text("File is outside the project or unavailable: ") + relativePath); return;
    }
    if (!file.open(QIODevice::ReadOnly)) {
        statusBar()->showMessage(UiLanguage::text("Could not read file: ") + file.errorString());
        return;
    }
    bool forced = false;
    try { forced = VividCode::readPolicy(rootPath).force; }
    catch (const std::exception &error) { statusBar()->showMessage(QString::fromUtf8(error.what())); return; }
    const qint64 maxBytes = forced ? file.size() : 2 * 1024 * 1024;
    if (file.size() > maxBytes) {
        statusBar()->showMessage(UiLanguage::text("This file exceeds 2 MiB. Use an external editor to view it."));
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
            statusBar()->showMessage(UiLanguage::text("Python source read failed or timed out: ") + reader.errorString()); return;
        }
        const auto result = QJsonDocument::fromJson(reader.readAllStandardOutput()).object();
        if (reader.exitStatus() != QProcess::NormalExit || reader.exitCode() || !result.value("ok").toBool()) {
            statusBar()->showMessage(UiLanguage::text("Python source read failed: ") + result.value("message").toString(QString::fromUtf8(reader.readAllStandardError()))); return;
        }
        text = result.value("text").toString();
    } else {
        const auto encoding = QStringDecoder::encodingForData(bytes).value_or(QStringConverter::Utf8);
        QStringDecoder decoder(encoding);
        text = decoder(bytes);
        if (decoder.hasError()) { statusBar()->showMessage(UiLanguage::text("Invalid source encoding: ") + relativePath); return; }
    }
    const QString requestedRoot = rootPath;
    // 等右键菜单完成关闭/销毁再打开弹窗，避免 Qt 的菜单弹出栈关闭新窗口。
    QTimer::singleShot(0, this, [this, text, line, column, requestedRoot, relativePath] {
        if (rootPath != requestedRoot) return;
        sourcePopup->showCode(text, line, column,
            mapToGlobal(QPoint(qMax(0, (width() - 760) / 2), qMax(0, (height() - 480) / 2))),
            relativePath, semantic->sourceHighlights(relativePath));
    });
}
