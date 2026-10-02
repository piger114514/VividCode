#include "VividCode.h"
#include <QtWidgets/QApplication>
#include <QtWidgets/QStyleFactory>
#include "SmokeTest.h"
#include "SemanticSmokeTest.h"
#include "SharedRuntime.h"
#include <memory>

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    app.setStyle(QStyleFactory::create("Fusion"));
    QPalette palette;
    palette.setColor(QPalette::Window, QColor("#101010"));
    palette.setColor(QPalette::WindowText, QColor("#f0f0f0"));
    palette.setColor(QPalette::Base, QColor("#181818"));
    palette.setColor(QPalette::AlternateBase, QColor("#222222"));
    palette.setColor(QPalette::Text, QColor("#f0f0f0"));
    palette.setColor(QPalette::Button, QColor("#282828"));
    palette.setColor(QPalette::ButtonText, QColor("#f0f0f0"));
    palette.setColor(QPalette::ToolTipBase, QColor("#282828"));
    palette.setColor(QPalette::ToolTipText, QColor("#f0f0f0"));
    palette.setColor(QPalette::PlaceholderText, QColor("#aaaaaa"));
    palette.setColor(QPalette::Highlight, QColor("#285c91"));
    palette.setColor(QPalette::HighlightedText, Qt::white);
    palette.setColor(QPalette::Light, QColor("#484848"));
    palette.setColor(QPalette::Midlight, QColor("#383838"));
    palette.setColor(QPalette::Mid, QColor("#383838"));
    palette.setColor(QPalette::Dark, QColor("#080808"));
    palette.setColor(QPalette::Shadow, Qt::black);
    for (const auto role : {QPalette::WindowText, QPalette::Text, QPalette::ButtonText})
        palette.setColor(QPalette::Disabled, role, QColor("#888888"));
    app.setPalette(palette);
    app.setStyleSheet(R"(
        QToolBar { background:#181818; border-bottom:1px solid #383838; spacing:10px; padding:8px; }
        QToolButton { color:#f0f0f0; padding:5px 8px; border:1px solid transparent; border-radius:3px; }
        QToolButton:hover { background:#303030; }
        QToolButton:pressed { background:#285c91; }
        QToolButton:disabled { color:#888888; }
        QStatusBar { background:#181818; color:#cccccc; }
        QTreeWidget, QPlainTextEdit, QLineEdit, QComboBox {
            background:#181818; color:#f0f0f0; border:1px solid #383838;
            selection-background-color:#285c91; selection-color:white;
        }
        QLineEdit { padding:7px; }
        QTreeWidget::item { padding:4px; }
        QHeaderView::section { background:#282828; color:#f0f0f0; padding:6px; border:1px solid #383838; }
        QTabWidget::pane { border:1px solid #383838; }
        QTabBar::tab { background:#202020; color:#bbbbbb; padding:7px 12px; border:1px solid #383838; }
        QTabBar::tab:selected { background:#303030; color:white; border-bottom:2px solid #62a0ff; }
        QTabBar::tab:hover { background:#303030; color:white; }
        QPushButton { background:#282828; color:#f0f0f0; border:1px solid #484848; border-radius:3px; padding:6px 10px; }
        QPushButton:hover { background:#383838; }
        QPushButton:pressed { background:#285c91; }
        QPushButton:disabled { background:#202020; color:#888888; border-color:#383838; }
        QLineEdit:focus, QComboBox:focus, QPushButton:focus { border:1px solid #62a0ff; }
        QComboBox { padding:5px; }
        QComboBox QAbstractItemView { background:#181818; color:#f0f0f0; selection-background-color:#285c91; selection-color:white; }
        QSplitter::handle { background:#303030; }
        QToolTip { background:#282828; color:#f0f0f0; border:1px solid #484848; }
    )");
    QCoreApplication::setApplicationName("vividcode");
    std::unique_ptr<VividCode::RuntimeReadLock> sharedLock;
    if (QFileInfo::exists(VividCode::sharedRuntime() + "/installation.json")) {
        sharedLock = std::make_unique<VividCode::RuntimeReadLock>(VividCode::sharedHome());
        if (!sharedLock->valid()) return 1;
    }
    if (app.arguments().contains("--self-test")) return runSmokeTest(app);
    if (app.arguments().contains("--semantic-test")) return runSemanticSmokeTest(app);
    const auto args = app.arguments();
    QString project;
    const int option = args.indexOf("--project");
    if (option >= 0 && option + 1 < args.size()) project = args.at(option + 1);
    else if (args.size() == 2) project = args.at(1);
    // Old deployed viewers are supported during migration.
    if (project.isEmpty()) {
        QDir directory(QCoreApplication::applicationDirPath());
        if (directory.dirName().compare(".vividcode", Qt::CaseInsensitive) == 0) {
            directory.cdUp(); project = directory.canonicalPath();
        }
    }
    std::unique_ptr<QLockFile> projectLock;
    if (!project.isEmpty() && QFileInfo::exists(QDir(project).filePath(".vividcode/project.json"))) {
        const QString lockPath = QDir(project).filePath(".vividcode-viewer.lock");
        if (QFileInfo(lockPath).isSymLink() || QFileInfo(lockPath).isJunction()) return 1;
        projectLock = std::make_unique<QLockFile>(lockPath);
        projectLock->setStaleLockTime(0);
        if (!projectLock->tryLock()) {
            if (!args.contains("--verify-startup")) QMessageBox::warning(nullptr, "vividcode", QStringLiteral("此项目已经打开，或正在更新/删除。"));
            return 1;
        }
    }
    VividCodeWindow window;
    if (args.contains("--verify-startup")) return !project.isEmpty() && window.openProject(project) ? 0 : 1;
    if (!project.isEmpty() && !window.openProject(project)) return 1;
    window.show();
    return app.exec();
}
