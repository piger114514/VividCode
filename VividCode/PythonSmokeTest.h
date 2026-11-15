#pragma once
#include "VividCode.h"
#include "SemanticPanel.h"
#include "UnifiedGraphStore.h"
#include "UiTestHelpers.h"
#include <QtWidgets>

inline int runPythonSmokeTest(QApplication &app) {
    const auto fail = [](int line) { qCritical() << "FAIL: Python UI at PythonSmokeTest.h" << line; return 1; };
    QFontDatabase::addApplicationFont(qEnvironmentVariable("WINDIR", "C:/Windows") + "/Fonts/msyh.ttc");
    app.setFont(QFont("Microsoft YaHei", 10));
    QTemporaryDir directory(QDir::tempPath() + QStringLiteral("/vividcode Python 中文-XXXXXX"));
    if (!directory.isValid()) return fail(__LINE__);
    const auto write = [&](const QString &name, const QByteArray &bytes) {
        QFile file(directory.filePath(name)); return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
    };
    if (!write("worker.py", "def helper(x):\n    value = x * 2\n    return value\n\nclass Base:\n    def run(self):\n        return 1\n\nclass Worker(Base):\n    def run(self):\n        return helper(2)\n")) return fail(__LINE__);
    if (!write("use.pyw", "from worker import helper\ndef caller():\n    return helper(3)\n")) return fail(__LINE__);
    VividCodeWindow window; window.show();
    auto *panel = window.findChild<SemanticPanel *>();
    if (!panel) return fail(__LINE__);
    const auto wait = [&] {
        if (!panel->isBusy()) return true;
        QEventLoop loop; QTimer timer; timer.setSingleShot(true);
        QObject::connect(&timer, &QTimer::timeout, &loop, &QEventLoop::quit);
        QObject::connect(panel, &SemanticPanel::settled, &loop, &QEventLoop::quit);
        timer.start(30000); loop.exec(); return timer.isActive() && !panel->isBusy();
    };
    if (!window.openProject(directory.path()) || !wait()) return fail(__LINE__);
    const auto state = [&] {
        QFile file(VividCode::semanticDirectory(directory.path()) + "/python/config.json");
        return file.open(QIODevice::ReadOnly) ? QJsonDocument::fromJson(file.readAll()).object() : QJsonObject();
    };
    if (!panel->analysisLanguages().contains("python") || state().value("last_status") != "ok") {
        qCritical().noquote() << window.findChild<QPlainTextEdit *>("semanticDiagnostics")->toPlainText(); return fail(__LINE__);
    }
    auto *symbols = window.findChild<QTreeWidget *>("semanticSymbols");
    auto *relations = window.findChild<QTreeWidget *>("semanticRelations");
    auto *graph = window.findChild<RelationGraph *>();
    auto *scene = graph->findChild<QGraphicsScene *>("relationGraphScene");
    auto *source = window.findChild<QPlainTextEdit *>("sourceViewer");
    const auto symbolId = [&](const QString &name) {
        for (QTreeWidgetItemIterator it(symbols); *it; ++it) {
            const auto s = (*it)->data(0, Qt::UserRole).toJsonObject();
            if (s.value("qualified_name") == name) return s.value("id").toString();
        }
        return QString();
    };
    if (symbolId("Worker").isEmpty() || symbolId("Worker.run").isEmpty()) return fail(__LINE__);
    const QString caller = symbolId("caller"), helper = symbolId("helper");
    if (caller.isEmpty() || helper.isEmpty()) return fail(__LINE__);
    // 项目文件单击切回关系图，仅高亮真实文件，保留视口及布局。
    auto *fileTree = window.findChild<QTreeWidget *>("fileTree");
    auto *tabs = window.findChild<QTabWidget *>("workspaceTabs");
    auto *focusView = graph->findChild<QGraphicsView *>("relationGraphView");
    auto entries = fileTree->findItems("use.pyw", Qt::MatchExactly | Qt::MatchRecursive);
    if (entries.size() != 1) return fail(__LINE__);
    graph->setZoomLevel(1.1); tabs->setCurrentIndex(1);
    fileTree->setCurrentItem(entries.first()); app.processEvents();
    if (scene->selectedItems().isEmpty()) return fail(__LINE__);
    focusView->centerOn(10000, 10000); app.processEvents();
    const auto center = focusView->mapToScene(focusView->viewport()->rect().center());
    const auto rect = scene->sceneRect();
    QMap<QGraphicsItem *, QPointF> positions;
    for (auto *item : scene->items()) positions[item] = item->pos();
    fileTree->itemClicked(entries.first(), 0); app.processEvents();
    if (scene->selectedItems().isEmpty() || scene->sceneRect() != rect ||
        focusView->mapToScene(focusView->viewport()->rect().center()) != center ||
        !qFuzzyCompare(focusView->transform().m11(), 1.1)) return fail(__LINE__);
    for (auto *item : scene->items()) if (!positions.contains(item) || positions[item] != item->pos()) return fail(__LINE__);
    if (panel->focusFile("not-indexed.py")) return fail(__LINE__);
    // File/scope hierarchy and the double-click path must use symbol identity.
    if (symbols->topLevelItemCount() != 2 || symbolId("helper.x").isEmpty() || symbolId("helper.value").isEmpty()) return fail(__LINE__);
    QTreeWidgetItem *helperItem = nullptr, *parameterItem = nullptr;
    for (QTreeWidgetItemIterator it(symbols); *it; ++it) {
        const auto s = (*it)->data(0, Qt::UserRole).toJsonObject();
        if (s.value("id") == helper) helperItem = *it;
        if (s.value("qualified_name") == "helper.x") parameterItem = *it;
    }
    if (!helperItem || !parameterItem || parameterItem->parent() != helperItem || helperItem->parent()->text(0) != "worker.py") return fail(__LINE__);
    symbols->itemDoubleClicked(helperItem, 0); app.processEvents();
    for (qreal zoom : {0.15, 1.1}) {
        graph->setZoomLevel(zoom); QSet<QString> displayed;
        for (auto *item : scene->items()) if (item->data(0) == "symbol") {
            const auto s = item->data(1).toJsonObject(); if (s.value("kind") != "RelatedFile") return fail(__LINE__);
            displayed.insert(s.value("def_file").toString());
        }
        if (displayed != QSet<QString>{"worker.py", "use.pyw"}) return fail(__LINE__);
    }
    symbols->itemDoubleClicked(parameterItem, 0); app.processEvents();
    int variableFiles = 0;
    for (auto *item : scene->items()) if (item->data(0) == "symbol") {
        ++variableFiles; if (item->data(1).toJsonObject().value("def_file") != "worker.py") return fail(__LINE__);
        if (!triggerGraphSource(graph, item)) return fail(__LINE__);
        if (!source->toPlainText().startsWith("def helper")) return fail(__LINE__);
    }
    if (variableFiles != 1) return fail(__LINE__);
    clickOutsidePopup(window); app.processEvents();
    symbols->itemDoubleClicked(helperItem, 0); graph->fitGraph(); app.processEvents();
    if (!window.grab().save(QCoreApplication::applicationDirPath() + "/symbol-files-preview.png")) return fail(__LINE__);
    panel->focusSymbol(caller); graph->setZoomLevel(1.0); if (!showGraphDetails(graph)) return fail(__LINE__);
    QGraphicsItem *edge = nullptr, *method = nullptr;
    for (auto *item : scene->items()) {
        if (item->data(0).toString() == "symbol" && item->data(1).toJsonObject().value("id") == helper) method = item;
        if (item->data(0).toString() == "relation") {
            for (const auto &value : item->data(1).toJsonArray()) {
                const auto r = value.toObject();
                if (r.value("source_id") == caller && r.value("target_id") == helper && r.value("type") == "calls") edge = item;
            }
        }
    }
    if (!edge || !method || !triggerGraphSource(graph, edge)) return fail(__LINE__);
    if (source->textCursor().blockNumber() != 2 || !source->textCursor().block().text().contains("helper(3)")) return fail(__LINE__);
    if (!triggerGraphSource(graph, method) || source->textCursor().blockNumber() != 0 || !source->toPlainText().startsWith("def helper")) return fail(__LINE__);
    clickOutsidePopup(window); app.processEvents();
    panel->focusSymbol({}); graph->setZoomLevel(1.0); if (!showGraphDetails(graph)) return fail(__LINE__); app.processEvents();
    auto *view = graph->findChild<QGraphicsView *>("relationGraphView");
    view->fitInView(scene->itemsBoundingRect().adjusted(-30, -30, 30, 30), Qt::KeepAspectRatio);
    if (!window.grab().save(QCoreApplication::applicationDirPath() + "/python-relation-preview.png")) return fail(__LINE__);
    if (!window.openProject(directory.path()) || !wait() || state().value("reused").toInt() != 2) return fail(__LINE__);
    UnifiedGraphStore store(directory.path());
    const auto originalUnits = store.load().value("python").value("units");
    if (originalUnits.toArray().size() != 2) return fail(__LINE__);
    if (!write("use.pyw", "def broken(:\n")) return fail(__LINE__);
    if (!window.openProject(directory.path()) || !wait() || state().value("last_status") != "failed") return fail(__LINE__);
    if (symbolId("caller").isEmpty() || store.load().value("python").value("units") != originalUnits) return fail(__LINE__);
    if (!write("use.pyw", "from worker import helper\ndef changed():\n    return helper(9)\n")) return fail(__LINE__);
    // Add C++ to prove that Python results coexist in the same DB/canvas.
    if (!write("other.cpp", "int target() { return 1; }\nint callerCpp() { return target(); }\n")) return fail(__LINE__);
    const QJsonArray commands{QJsonObject{{"directory", directory.path()}, {"file", "other.cpp"},
        {"arguments", QJsonArray{"clang++", "-std=c++17", "-c", "other.cpp"}}}};
    if (!write("compile_commands.json", QJsonDocument(commands).toJson())) return fail(__LINE__);
    if (!window.openProject(directory.path()) || !wait() || symbolId("changed").isEmpty() || !symbolId("caller").isEmpty()) return fail(__LINE__);
    if (!panel->analysisLanguages().contains("c++") || store.load().size() != 2) return fail(__LINE__);
    bool pythonLabel = false, cppLabel = false;
    for (auto *item : scene->items()) if (auto *label = dynamic_cast<QGraphicsSimpleTextItem *>(item)) {
        pythonLabel |= label->text() == "Python"; cppLabel |= label->text() == "C++";
    }
    if (!pythonLabel || !cppLabel) return fail(__LINE__);
    if (!write("encoded.py", QByteArray("# coding: gbk\n# ") + QByteArray::fromHex("c4e3bac3") + "\n")) return fail(__LINE__);
    if (!window.openProject(directory.path()) || !wait()) return fail(__LINE__);
    auto *files = window.findChild<QTreeWidget *>("fileTree");
    const auto matches = files->findItems("encoded.py", Qt::MatchExactly | Qt::MatchRecursive);
    if (matches.isEmpty() || !triggerFileSource(files, matches.first()) || !source->toPlainText().contains(QStringLiteral("你好"))) return fail(__LINE__);
    for (const auto &name : {"worker.py", "use.pyw", "encoded.py"}) if (!QFile::remove(directory.filePath(name))) return fail(__LINE__);
    if (!window.openProject(directory.path()) || !wait() || state().value("total").toInt(-1) != 0) return fail(__LINE__);
    if (!store.load().value("python").value("units").toArray().isEmpty() || relations->topLevelItemCount() == 0) return fail(__LINE__);
    qInfo() << "PASS: Python detection, class/function graph, cross-file call/definition navigation, cache reuse, failure preservation, C++ mixed canvas, GBK source, final-file deletion";
    return 0;
}
