#pragma once
#include "VividCode.h"
#include "ProjectScanner.h"
#include <QtWidgets>

// 不依赖额外测试框架的端到端冒烟检查；只修改临时目录。
inline int runSmokeTest(QApplication &app)
{
    // offscreen 插件不会自动发现 Windows 系统字体，显式加载以验证中文布局。
    const QString fonts = qEnvironmentVariable("WINDIR", "C:/Windows") + "/Fonts/";
    QFontDatabase::addApplicationFont(fonts + "msyh.ttc");
    QFontDatabase::addApplicationFont(fonts + "consola.ttf");
    app.setFont(QFont("Microsoft YaHei", 10));
    const auto require = [](bool condition, const char *message) {
        if (!condition) qCritical() << "FAIL:" << message;
        return condition;
    };
    QTemporaryDir temporary;
    if (!require(temporary.isValid(), "temporary directory")) return 1;
    const QDir root(temporary.path());
    root.mkpath("src"); root.mkpath("build"); root.mkpath(".git");
    const auto write = [&root](const QString &path, const QByteArray &bytes) {
        QFile file(root.filePath(path));
        return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
    };
    if (!write("src/main.cpp", "int main() { return 0; }\n") ||
        !write(QStringLiteral("中文.hpp"), "// UTF-8\n") ||
        !write("build/ignored.cpp", "generated") || !write(".git/ignored.h", "ignored") ||
        !write("notes.txt", "not source")) return 1;
    auto result = scanProject(root.path());
    if (!require(result.files.size() == 2 && result.skippedDirectories == 2, "scan and exclusions")) return 1;
    VividCodeWindow window;
    if (!require(window.openProject(root.path()), "open project")) return 1;
    window.show(); app.processEvents();
    auto *tree = window.findChild<QTreeWidget *>("fileTree");
    auto *viewer = window.findChild<QPlainTextEdit *>("sourceViewer");
    auto *filter = window.findChild<QLineEdit *>("fileFilter");
    auto *scene = window.findChild<QGraphicsScene *>();
    if (!require(tree && viewer && filter && scene && scene->items().isEmpty(), "UI components and unconfigured relation graph")) return 1;
    QTreeWidgetItem *source = nullptr;
    for (QTreeWidgetItemIterator it(tree); *it; ++it)
        if ((*it)->data(0, Qt::UserRole).toString() == "src/main.cpp") source = *it;
    if (!require(source != nullptr, "nested file tree")) return 1;
    tree->setCurrentItem(source);
    if (!require(viewer->toPlainText().contains("return 0"), "tree selection loads source")) return 1;
    if (!write("src/main.cpp", "int main() { return 7; }\n") || !write("new.h", "// added")) return 1;
    window.openProject(root.path());
    if (!require(viewer->toPlainText().contains("return 7") && scanProject(root.path()).files.size() == 3, "refresh modified and added files")) return 1;
    filter->setText("no-such-file");
    if (!require(tree->topLevelItemCount() == 0, "filter empty result")) return 1;
    filter->clear();
    const auto added = tree->findItems("new.h", Qt::MatchExactly | Qt::MatchRecursive);
    if (!require(!added.isEmpty(), "new file in tree")) return 1;
    tree->setCurrentItem(added.first());
    if (!require(viewer->toPlainText() == "// added", "new file selection loads source")) return 1;
    QFile::remove(root.filePath("new.h"));
    window.openProject(root.path());
    if (!require(viewer->toPlainText().isEmpty() && scanProject(root.path()).files.size() == 2, "refresh deleted file")) return 1;
    if (!require(!window.openProject(root.filePath("missing")), "reject missing directory")) return 1;
    root.mkpath("empty");
    window.openProject(root.filePath("empty"));
    if (!require(tree->topLevelItemCount() == 0, "empty project")) return 1;
    // 验证 JS/TS/Java 扩展名与对应生成目录排除。
    root.mkpath("multi/src"); root.mkpath("multi/node_modules"); root.mkpath("multi/dist"); root.mkpath("multi/target");
    if (!write("multi/src/Greeter.js", "// js\n") ||
        !write("multi/src/Greeter.ts", "// ts\n") ||
        !write("multi/src/Greeter.java", "// java\n") ||
        !write("multi/node_modules/x.js", "skip") ||
        !write("multi/dist/x.js", "skip") ||
        !write("multi/target/x.java", "skip")) return 1;
    const auto multiResult = scanProject(root.filePath("multi"));
    if (!require(multiResult.files.size() == 3 && multiResult.skippedDirectories == 3, "JS/TS/Java scan and exclusions")) return 1;
    // 用真实演示源码渲染截图，供布局检查。
    if (!require(window.openProject(QCoreApplication::applicationDirPath() + "/demo"), "bundled demo")) return 1;
    const auto matches = tree->findItems("Greeter.h", Qt::MatchExactly | Qt::MatchRecursive);
    if (!require(!matches.isEmpty(), "demo header")) return 1;
    tree->setCurrentItem(matches.first());
    app.processEvents();
    if (!require(window.grab().save(QCoreApplication::applicationDirPath() + "/prototype-preview.png"), "render preview")) return 1;
    qInfo() << "PASS: scan, exclusions, UTF-8 paths, tree selection, unconfigured graph, filter, refresh add/edit/delete, invalid/empty directories, JS/TS/Java scan, demo render";
    return 0;
}
