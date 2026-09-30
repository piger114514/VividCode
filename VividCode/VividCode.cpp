#include "VividCode.h"
#include "ProjectScanner.h"
#include "ProjectStore.h"
#include "SemanticPanel.h"
#include "RelationGraph.h"
#include <QtWidgets>

VividCodeWindow::VividCodeWindow(QWidget *parent) : QMainWindow(parent)
{
    ui.setupUi(this);
    setWindowTitle(QStringLiteral("vividcode"));
    resize(1320, 900);
    setMinimumSize(900, 600);
    auto *toolbar = addToolBar(QStringLiteral("项目"));
    toolbar->setMovable(false);
    auto *open = toolbar->addAction(QStringLiteral("打开项目…"));
    open->setShortcut(QKeySequence::Open);
    auto *demo = toolbar->addAction(QStringLiteral("打开演示"));
    auto *refresh = toolbar->addAction(QStringLiteral("刷新文件"));
    refresh->setShortcut(QKeySequence::Refresh);
    toolbar->addSeparator();
    auto *smaller = toolbar->addAction(QStringLiteral("缩小"));
    auto *larger = toolbar->addAction(QStringLiteral("放大"));
    auto *fit = toolbar->addAction(QStringLiteral("适应画布"));
    ui.mainToolBar->hide();
    ui.menuBar->hide();

    auto *body = new QWidget;
    auto *layout = new QVBoxLayout(body);
    layout->setContentsMargins(16, 12, 16, 12);
    projectLabel = new QLabel(QStringLiteral("从“打开演示”开始，或选择一个包含源码文件的文件夹（C/C++、C#、Python、JavaScript/TypeScript、Java）。"));
    projectLabel->setTextFormat(Qt::PlainText);
    projectLabel->setWordWrap(true);
    layout->addWidget(projectLabel);
    auto *notice = new QLabel(QStringLiteral("C++ 真实关系图 · 导入编译数据库，选择符号查看继承、调用和引用；左侧文件树可浏览各语言源码。"));
    notice->setWordWrap(true);
    notice->setStyleSheet("color:#aaaaaa; padding:6px 0;");
    layout->addWidget(notice);
    auto *splitter = new QSplitter;
    auto *sidebar = new QWidget;
    auto *sideLayout = new QVBoxLayout(sidebar);
    sideLayout->setContentsMargins(0, 0, 8, 0);
    filter = new QLineEdit;
    filter->setObjectName("fileFilter");
    filter->setPlaceholderText(QStringLiteral("筛选文件名或路径…"));
    filter->setClearButtonEnabled(true);
    tree = new QTreeWidget;
    tree->setObjectName("fileTree");
    tree->setHeaderLabel(QStringLiteral("项目文件"));
    sideLayout->addWidget(filter);
    sideLayout->addWidget(tree);
    splitter->addWidget(sidebar);
    auto *right = new QSplitter(Qt::Vertical);
    semantic = new SemanticPanel;
    right->addWidget(semantic);
    auto *graph = semantic->findChild<RelationGraph *>();
    connect(semantic, &SemanticPanel::sourceRequested, this, [this](const QString &path, int line, int column) {
        const QString absolute = QFileInfo(QDir(rootPath).filePath(path)).canonicalFilePath();
        const QString canonicalRoot = QFileInfo(rootPath).canonicalFilePath();
        if (absolute.isEmpty() || !absolute.startsWith(canonicalRoot + '/', Qt::CaseInsensitive)) {
            statusBar()->showMessage(QStringLiteral("外部符号或文件不可用：") + path); return;
        }
        const QString relative = QDir(rootPath).relativeFilePath(absolute);
        if (!files.contains(relative)) files.append(relative);
        showFile(relative);
        QTextCursor cursor(code->document()->findBlockByNumber(qMax(0,line-1)));
        if (!cursor.isNull()) { cursor.movePosition(QTextCursor::Right, QTextCursor::MoveAnchor, qMax(0,column-1)); code->setTextCursor(cursor); code->centerCursor(); }
    });
    auto *source = new QWidget;
    auto *sourceLayout = new QVBoxLayout(source);
    sourceLayout->setContentsMargins(0, 8, 0, 0);
    fileLabel = new QLabel(QStringLiteral("点击左侧文件或关系图节点查看源码"));
    fileLabel->setTextFormat(Qt::PlainText);
    fileLabel->setWordWrap(true);
    fileLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    code = new QPlainTextEdit;
    code->setObjectName("sourceViewer");
    code->setReadOnly(true);
    code->setLineWrapMode(QPlainTextEdit::NoWrap);
    code->setFont(QFont("Consolas", 11));
    code->setPlaceholderText(QStringLiteral("源码只读显示。使用外部编辑器修改后，点击“刷新文件”。"));
    sourceLayout->addWidget(fileLabel);
    sourceLayout->addWidget(code);
    right->addWidget(source);
    right->setSizes({540, 230});
    splitter->addWidget(right);
    splitter->setSizes({290, 990});
    layout->addWidget(splitter, 1);
    setCentralWidget(body);
    statusBar()->showMessage(QStringLiteral("就绪 · Ctrl+O 打开项目 · F5 刷新"));
    connect(open, &QAction::triggered, this, [this] {
        const QString directory = QFileDialog::getExistingDirectory(this, QStringLiteral("选择项目目录"), rootPath);
        if (!directory.isEmpty()) openProject(directory);
    });
    connect(demo, &QAction::triggered, this, [this] {
        const QString path = QCoreApplication::applicationDirPath() + "/demo";
        if (!openProject(path)) QMessageBox::warning(this, QStringLiteral("未找到演示"), QStringLiteral("请运行 build.ps1，将演示文件复制到程序旁边。"));
    });
    connect(refresh, &QAction::triggered, this, [this] {
        if (rootPath.isEmpty()) return;
        if (!openProject(rootPath)) {
            files.clear(); currentFile.clear(); code->clear(); rebuildViews();
            semantic->setProject({});
            fileLabel->setText(QStringLiteral("项目目录已不可用，请重新打开项目。"));
        }
    });
    connect(filter, &QLineEdit::textChanged, this, [this] { rebuildViews(); });
    connect(tree, &QTreeWidget::currentItemChanged, this, [this](QTreeWidgetItem *item) {
        if (item && !item->data(0, Qt::UserRole).toString().isEmpty()) showFile(item->data(0, Qt::UserRole).toString());
    });
    connect(smaller, &QAction::triggered, this, [graph] { graph->zoomBy(0.8); });
    connect(larger, &QAction::triggered, this, [graph] { graph->zoomBy(1.25); });
    connect(fit, &QAction::triggered, graph, &RelationGraph::fitGraph);
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
    const ScanResult result = scanProject(rootPath);
    files = result.files;
    if (!sameProject || !files.contains(currentFile)) {
        currentFile.clear(); code->clear();
        fileLabel->setText(QStringLiteral("点击文件查看源码"));
    }
    { const QSignalBlocker blocker(filter); filter->clear(); }
    projectLabel->setText(QStringLiteral("项目：%1  |  %2 个源码文件").arg(QDir::toNativeSeparators(rootPath)).arg(files.size()));
    rebuildViews();
    if (!currentFile.isEmpty()) showFile(currentFile);
    statusBar()->showMessage(QStringLiteral("扫描完成 · %1 个文件 · 排除 %2 个生成/依赖目录%3")
        .arg(files.size()).arg(result.skippedDirectories)
        .arg(result.truncated ? QStringLiteral(" · 已达扫描上限，请选择更小的项目目录") : QString()));
    const auto record = VividCode::recordIfInitialized(rootPath);
    if (!record.message.isEmpty())
        statusBar()->showMessage(statusBar()->currentMessage() + (record.ok ? " | " : QStringLiteral(" | 记录更新失败：")) + record.message);
    semantic->setProject(rootPath);
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

void VividCodeWindow::showFile(const QString &relativePath)
{
    if (!files.contains(relativePath)) return;
    currentFile = relativePath;
    QFile file(QDir(rootPath).filePath(relativePath));
    fileLabel->setText(relativePath + QStringLiteral(" · 只读 · UTF-8"));
    if (!file.open(QIODevice::ReadOnly)) {
        code->setPlainText(QStringLiteral("无法读取文件：") + file.errorString());
        return;
    }
    constexpr qint64 maxBytes = 2 * 1024 * 1024;
    if (file.size() > maxBytes) {
        code->setPlainText(QStringLiteral("该文件超过 2 MiB，基础版不加载超大文件。请使用外部编辑器查看。"));
        return;
    }
    code->setPlainText(QString::fromUtf8(file.read(maxBytes)));
    fileLabel->setText(relativePath + QStringLiteral(" · %1 行 · 只读 · UTF-8").arg(code->document()->blockCount()));
}
