#pragma once
#include "VividCode.h"
#include "ProjectScanner.h"
#include "UiTestHelpers.h"
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
    auto *popup = dynamic_cast<SourcePopup *>(window.findChild<QWidget *>("sourcePopup"));
    if (!require(popup && !popup->isVisible() && window.findChild<QTabWidget *>("sidebarViews")->count() == 2 && !window.findChild<QComboBox *>("semanticFocus") && !window.findChild<QComboBox *>("semanticLanguage"), "sidebar and removed controls")) return 1;
    if (!require(triggerFileSource(tree, source) && popup->isVisible() && viewer->toPlainText().contains("return 0"), "file context source popup")) return 1;
    clickOutsidePopup(window);
    if (!require(!popup->isVisible(), "outside click closes source popup")) return 1;
    if (!write("src/main.cpp", "int main() { return 7; }\n") || !write("new.h", "// added")) return 1;
    window.openProject(root.path());
    source = nullptr;
    for (QTreeWidgetItemIterator it(tree); *it; ++it) if ((*it)->data(0, Qt::UserRole).toString() == "src/main.cpp") source = *it;
    if (!require(source && triggerFileSource(tree, source) && viewer->toPlainText().contains("return 7") && scanProject(root.path()).files.size() == 3, "refresh modified and added files")) return 1;
    window.findChild<QToolButton *>("sourcePopupClose")->click();
    if (!require(!popup->isVisible(), "close button dismisses popup")) return 1;
    filter->setText("no-such-file");
    if (!require(tree->topLevelItemCount() == 0, "filter empty result")) return 1;
    filter->clear();
    const auto added = tree->findItems("new.h", Qt::MatchExactly | Qt::MatchRecursive);
    if (!require(!added.isEmpty(), "new file in tree")) return 1;
    tree->setCurrentItem(added.first());
    if (!require(triggerFileSource(tree, added.first()) && viewer->toPlainText() == "// added", "new file source popup")) return 1;
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
    root.mkpath("unity/Assets/Library"); root.mkpath("unity/ProjectSettings"); root.mkpath("unity/Library"); root.mkpath("unity/Temp"); root.mkpath("unity/Logs"); root.mkpath("unity/UserSettings");
    if (!write("unity/ProjectSettings/ProjectVersion.txt", "m_EditorVersion: fixture\n") ||
        !write("unity/Assets/Player.cs", "class Player { }\n") || !write("unity/Assets/Library/Kept.cs", "class Kept { }\n") ||
        !write("unity/Library/Generated.cs", "skip") || !write("unity/Temp/Generated.cs", "skip") ||
        !write("unity/Logs/Generated.cs", "skip") || !write("unity/UserSettings/Generated.cs", "skip")) return 1;
    const auto unityResult = scanProject(root.filePath("unity"));
    if (!require(unityResult.files.size() == 2 && unityResult.skippedDirectories == 4, "Unity root generated directories excluded, Assets/Library retained")) return 1;
    // 真实文件数量跨越旧的 5,000 上限，并核对 20,000 边界及超额提示。
    root.mkpath("scan-limit");
    for (int index = 0; index < 20000; ++index)
        if (!write("scan-limit/" + QString::number(index) + ".cpp", {})) return 1;
    const auto fullScan = scanProject(root.filePath("scan-limit"));
    if (!require(fullScan.files.size() == 20000 && !fullScan.truncated, "exactly 20000 source files scanned completely")) return 1;
    if (!write("scan-limit/extra.cpp", {})) return 1;
    const auto limitedScan = scanProject(root.filePath("scan-limit"));
    if (!require(limitedScan.files.size() == 20000 && limitedScan.truncated, "source files beyond 20000 report truncation")) return 1;
    VividCode::ReadPolicy forced; forced.force = true;
    const auto forcedScan = scanProject(root.filePath("scan-limit"), forced);
    if (!require(forcedScan.files.size() == 20001 && !forcedScan.truncated, "forced scan exceeds normal file limit")) return 1;
    // 真实菜单读取超过 2 MiB 的文件：普通模式拒绝，强制项目读取完整内容。
    root.mkpath("force-preview");
    const QByteArray bigSource = "// " + QByteArray(2 * 1024 * 1024 + 1, 'x') + "\n// END";
    if (!write("force-preview/big.cpp", bigSource) || !window.openProject(root.filePath("force-preview"))) return 1;
    auto bigItems = tree->findItems("big.cpp", Qt::MatchExactly | Qt::MatchRecursive);
    if (!require(bigItems.size() == 1 && triggerFileSource(tree, bigItems.first()) && !popup->isVisible(), "normal preview rejects oversized file")) return 1;
    root.mkpath("force-preview/.vividcode");
    if (!write("force-preview/.vividcode/project.json", "{\"app\":\"vividcode\",\"schema\":1,\"latest\":\"\",\"history\":[],\"forceRead\":true}") || !window.openProject(root.filePath("force-preview"))) return 1;
    bigItems = tree->findItems("big.cpp", Qt::MatchExactly | Qt::MatchRecursive);
    if (!require(bigItems.size() == 1 && triggerFileSource(tree, bigItems.first()) && popup->isVisible() && viewer->toPlainText().endsWith("// END"), "forced preview reads complete oversized source")) return 1;
    popup->close();
    // 用临时项目渲染截图，不依赖随程序部署的演示目录。
    if (!require(window.openProject(root.filePath("multi")), "temporary source project")) return 1;
    const auto matches = tree->findItems("Greeter.js", Qt::MatchExactly | Qt::MatchRecursive);
    if (!require(!matches.isEmpty(), "temporary project source")) return 1;
    tree->setCurrentItem(matches.first());
    app.processEvents();
    if (!require(window.grab().save(QCoreApplication::applicationDirPath() + "/prototype-preview.png"), "render preview")) return 1;
    qInfo() << "PASS: scan, exclusions, UTF-8 paths, sidebar tabs, removed selectors, file context source popup, outside/close dismissal, filter, refresh add/edit/delete, invalid/empty directories and file-only languages";
    return 0;
}
