#include "VividCode.h"
#include "ProjectScanner.h"
#include "ProjectStore.h"
#include "SemanticPanel.h"
#include "RelationGraph.h"
#include "SourcePopup.h"
#include <QtWidgets>

VividCodeWindow::VividCodeWindow(QWidget *parent) : QMainWindow(parent)
{
    ui.setupUi(this);
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
    ui.mainToolBar->hide();
    ui.menuBar->hide();
    sourcePopup = new SourcePopup(this);
    semantic = new SemanticPanel;
    auto *toolbar = addToolBar(QStringLiteral("分析设置"));
    toolbar->setObjectName("analysisSettingsBar"); toolbar->setMovable(false);
    toolbar->addWidget(semantic->analysisControls());

    auto *body = new QWidget;
    auto *layout = new QVBoxLayout(body);
    layout->setContentsMargins(16, 12, 16, 12);
    projectLabel = new QLabel(QStringLiteral("选择包含源码文件的项目文件夹。"));
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
    tree->setHeaderLabel(QStringLiteral("项目文件"));
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
    splitter->addWidget(semantic);
    splitter->setSizes({350, 970});
    layout->addWidget(splitter, 1);
    setCentralWidget(body);
    statusBar()->showMessage(QStringLiteral("就绪 · Ctrl+O 打开项目 · F5 刷新"));
    connect(open, &QAction::triggered, this, [this] {
        const QString directory = QFileDialog::getExistingDirectory(this, QStringLiteral("选择项目目录"), rootPath);
        if (!directory.isEmpty()) openProject(directory);
    });
    connect(refresh, &QAction::triggered, this, [this] {
        if (rootPath.isEmpty()) return;
        if (!openProject(rootPath)) {
            files.clear(); currentFile.clear(); sourcePopup->close(); rebuildViews();
            semantic->setProject({});
            statusBar()->showMessage(QStringLiteral("项目目录已不可用，请重新打开项目。"));
        }
    });
    connect(filter, &QLineEdit::textChanged, this, [this] { rebuildViews(); });
    connect(tree, &QTreeWidget::currentItemChanged, this, [this](QTreeWidgetItem *item) {
        if (item && !item->data(0, Qt::UserRole).toString().isEmpty()) currentFile = item->data(0, Qt::UserRole).toString();
    });
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

VividCodeWindow::~VividCodeWindow() = default;

bool VividCodeWindow::openProject(const QString &directory)
{
    const QFileInfo info(directory);
    if (!info.isDir() || !info.isReadable()) {
        statusBar()->showMessage(QStringLiteral("无法打开目录：") + directory);
        return false;
    }
    const QString newRoot = info.absoluteFilePath();
    const bool sameProject = rootPath == newRoot;
    rootPath = newRoot;
    sourcePopup->close();
    const ScanResult result = scanProject(rootPath);
    files = result.files;
    if (!sameProject || !files.contains(currentFile)) {
        currentFile.clear();
    }
    { const QSignalBlocker blocker(filter); filter->clear(); }
    projectLabel->setText(QStringLiteral("项目：%1  |  %2 个源码文件").arg(QDir::toNativeSeparators(rootPath)).arg(files.size()));
    rebuildViews();
    statusBar()->showMessage(QStringLiteral("扫描完成 · %1 个文件 · 排除 %2 个生成/依赖目录%3")
        .arg(files.size()).arg(result.skippedDirectories)
        .arg(result.truncated ? QStringLiteral(" · 已达扫描上限，请选择更小的项目目录") : QString()));
    const auto record = VividCode::recordIfInitialized(rootPath);
    if (!record.message.isEmpty())
        statusBar()->showMessage(statusBar()->currentMessage() + (record.ok ? " | " : QStringLiteral(" | 记录更新失败：")) + record.message);
    semantic->setProject(rootPath, files);
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
    constexpr qint64 maxBytes = 2 * 1024 * 1024;
    if (file.size() > maxBytes) {
        statusBar()->showMessage(QStringLiteral("该文件超过 2 MiB，请使用外部编辑器查看。"));
        return;
    }
    const QString text = QString::fromUtf8(file.read(maxBytes));
    const QString requestedRoot = rootPath;
    // 等右键菜单完成关闭/销毁再打开弹窗，避免 Qt 的菜单弹出栈关闭新窗口。
    QTimer::singleShot(0, this, [this, text, line, column, requestedRoot] {
        if (rootPath != requestedRoot) return;
        sourcePopup->showCode(text, line, column,
            mapToGlobal(QPoint(qMax(0, (width() - 760) / 2), qMax(0, (height() - 480) / 2))));
    });
}
