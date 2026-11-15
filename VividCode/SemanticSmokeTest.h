#pragma once
#include "VividCode.h"
#include "SemanticPanel.h"
#include "SharedRuntime.h"
#include "ProjectStore.h"
#include "RelationGraphTest.h"
#include <QtWidgets>

inline int semanticTestFailure(int line) { qCritical() << "FAIL: C++ UI check at SemanticSmokeTest.h line" << line; return 1; }

// 可选真实工程验收：显式指定项目与它的真实编译数据库，索引写入该项目的集中语义缓存。
inline int runRealGraphValidation(QApplication &app, const QString &project, const QString &database) {
    VividCodeWindow window; window.show();
    if (!window.openProject(project)) return semanticTestFailure(__LINE__);
    auto *panel = window.findChild<SemanticPanel *>();
    QEventLoop loop; QTimer deadline; deadline.setSingleShot(true);
    QObject::connect(&deadline, &QTimer::timeout, &loop, &QEventLoop::quit);
    QObject::connect(panel, &SemanticPanel::settled, &loop, &QEventLoop::quit);
    QTimer progress;
    QString previous;
    QObject::connect(&progress, &QTimer::timeout, panel, [&] {
        const auto text = window.findChild<QLabel *>("semanticStatus")->text();
        if (text != previous) { previous = text; qInfo().noquote() << text; }
    });
    deadline.start(300000); progress.start(2000);
    panel->importDatabase(database); loop.exec(); progress.stop();
    if (!deadline.isActive() || panel->isBusy()) { qCritical() << "real project timeout"; return semanticTestFailure(__LINE__); }
    const auto state = window.findChild<QLabel *>("semanticStatus")->text();
    if (state.contains("failed") || state.contains(QStringLiteral("失败"))) { qCritical().noquote() << state; return semanticTestFailure(__LINE__); }
    auto *units = window.findChild<QComboBox *>("semanticUnits");
    int unitIndex = -1;
    for (int i = 0; i < units->count(); ++i) if (units->itemText(i).contains("VividCode.cpp")) { unitIndex = i; break; }
    if (unitIndex < 0) { qCritical() << "real project unit missing"; return semanticTestFailure(__LINE__); }
    units->setCurrentIndex(unitIndex);
    auto *symbols = window.findChild<QTreeWidget *>("semanticSymbols");
    QString focusId;
    for (QTreeWidgetItemIterator it(symbols); *it; ++it) {
        const auto s = (*it)->data(0, Qt::UserRole).toJsonObject();
        if (s.value("qualified_name").toString() == "VividCodeWindow::openProject") { focusId = s.value("id").toString(); break; }
    }
    if (focusId.isEmpty()) return semanticTestFailure(__LINE__);
    panel->focusSymbol(focusId);
    auto *graph = window.findChild<RelationGraph *>();
    graph->fitGraph();
    app.processEvents();
    if (!window.grab().save(QCoreApplication::applicationDirPath() + "/relation-real-project-overview.png")) return semanticTestFailure(__LINE__);
    graph->setZoomLevel(1.0); if (!showGraphDetails(graph)) return semanticTestFailure(__LINE__);
    auto *scene = graph->findChild<QGraphicsScene *>("relationGraphScene");
    QGraphicsItem *scanNode = nullptr, *scanEdge = nullptr;
    for (auto *item : scene->items()) {
        if (item->data(0).toString() == "symbol" && item->data(1).toJsonObject().value("qualified_name") == "scanProject") scanNode = item;
    }
    if (!scanNode) { qCritical() << "real scanProject endpoint missing"; return semanticTestFailure(__LINE__); }
    const auto scanId = scanNode->data(1).toJsonObject().value("id").toString();
    for (auto *item : scene->items()) {
        if (item->data(0).toString() != "relation") continue;
        const auto r = item->data(1).toJsonArray().first().toObject();
        if (r.value("source_id") == focusId && r.value("target_id") == scanId && r.value("type") == "calls") scanEdge = item;
    }
    if (!scanEdge) return semanticTestFailure(__LINE__);
    scanEdge->setSelected(true);
    if (!triggerGraphSource(graph, scanEdge)) return semanticTestFailure(__LINE__);
    auto *source = window.findChild<QPlainTextEdit *>("sourceViewer");
    if (!source->textCursor().block().text().contains("scanProject(rootPath)")) { qCritical() << "real relation location mismatch"; return semanticTestFailure(__LINE__); }
    scene->clearSelection(); scanNode->setSelected(true);
    if (!triggerGraphSource(graph, scanNode)) return semanticTestFailure(__LINE__);
    if (!source->textCursor().block().text().contains("scanProject(const QString")) { qCritical() << "real header definition mismatch"; return semanticTestFailure(__LINE__); }
    app.processEvents();
    if (!window.grab().save(QCoreApplication::applicationDirPath() + "/relation-real-project-preview.png")) return semanticTestFailure(__LINE__);
    qInfo().noquote() << "PASS: real project:" << state << graph->findChild<QLabel *>("relationGraphSummary")->text();
    return 0;
}

inline int runSemanticSmokeTest(QApplication &app) {
    const QString fonts = qEnvironmentVariable("WINDIR", "C:/Windows") + "/Fonts/";
    QFontDatabase::addApplicationFont(fonts + "msyh.ttc");
    QFontDatabase::addApplicationFont(fonts + "consola.ttf");
    app.setFont(QFont("Microsoft YaHei", 10));
    if (!runRelationGraphModelTest()) return semanticTestFailure(__LINE__);
    const auto realProject = qEnvironmentVariable("VIVIDCODE_GRAPH_PROJECT");
    if (!realProject.isEmpty()) return runRealGraphValidation(app, realProject, qEnvironmentVariable("VIVIDCODE_GRAPH_DATABASE"));
    QTemporaryDir dir(QDir::tempPath() + QStringLiteral("/vividcode 语义-XXXXXX"));
    if (!dir.isValid()) return semanticTestFailure(__LINE__);
    const auto write = [&](const QString &name, const QByteArray &bytes) {
        QFile f(dir.filePath(name)); return f.open(QIODevice::WriteOnly) && f.write(bytes) == bytes.size();
    };
    if (!write("test.cpp", "struct Base {};\nstruct Derived : Base { int x; int get() { return x; } };\nint main() { Derived d; return d.get(); }\n")) return semanticTestFailure(__LINE__);
    QJsonArray commands{QJsonObject{{"directory", dir.path()}, {"file", "test.cpp"},
        {"arguments", QJsonArray{"clang++", "-std=c++17", "-c", "test.cpp"}}}};
    if (!write("compile_commands.json", QJsonDocument(commands).toJson())) return semanticTestFailure(__LINE__);
    VividCodeWindow window; window.show(); window.openProject(dir.path());
    auto *panel = window.findChild<SemanticPanel *>();
    if (!panel) return semanticTestFailure(__LINE__);
    window.findChild<QTabWidget *>()->setCurrentIndex(1);
    const auto wait = [&] {
        QEventLoop loop; QTimer timer; timer.setSingleShot(true);
        QObject::connect(&timer, &QTimer::timeout, &loop, &QEventLoop::quit);
        QObject::connect(panel, &SemanticPanel::settled, &loop, &QEventLoop::quit);
        timer.start(30000); loop.exec(); return timer.isActive() && !panel->isBusy();
    };
    panel->importDatabase(dir.filePath("compile_commands.json"));
    if (!wait()) { qCritical() << "semantic timeout"; return semanticTestFailure(__LINE__); }
    auto *symbols = window.findChild<QTreeWidget *>("semanticSymbols");
    auto *relations = window.findChild<QTreeWidget *>("semanticRelations");
    auto *source = window.findChild<QPlainTextEdit *>("sourceViewer");
    if (!symbols || !relations || symbols->topLevelItemCount() == 0 || relations->topLevelItemCount() == 0) {
        qCritical() << window.findChild<QLabel *>("semanticStatus")->text(); return semanticTestFailure(__LINE__);
    }
    auto *relation = relations->topLevelItem(0); const int line = relation->data(0,Qt::UserRole).toJsonObject().value("line").toInt();
    relations->setCurrentItem(relation);
    if (!triggerStructureSource(relations, relation)) return semanticTestFailure(__LINE__);
    if (!source->toPlainText().contains("Derived") || source->textCursor().blockNumber() != line - 1) return semanticTestFailure(__LINE__);
    app.processEvents(); window.grab().save(QCoreApplication::applicationDirPath() + "/semantic-preview.png");
    auto *graph = window.findChild<RelationGraph *>();
    auto *scene = graph ? graph->findChild<QGraphicsScene *>("relationGraphScene") : nullptr;
    if (!graph || !scene) return semanticTestFailure(__LINE__);
    QTreeWidgetItem *mainSymbol = nullptr;
    for (QTreeWidgetItemIterator it(symbols); *it; ++it)
        if ((*it)->data(0, Qt::UserRole).toJsonObject().value("qualified_name").toString() == "main") mainSymbol = *it;
    if (!mainSymbol) return semanticTestFailure(__LINE__);
    const auto mainId = mainSymbol->data(0, Qt::UserRole).toJsonObject().value("id").toString();
    panel->focusSymbol(mainId);
    graph->setZoomLevel(1.0); if (!showGraphDetails(graph)) return semanticTestFailure(__LINE__);
    window.findChild<QTabWidget *>("sidebarViews")->setCurrentIndex(0);
    QGraphicsItem *callEdge = nullptr, *getNode = nullptr;
    for (auto *item : scene->items()) {
        if (item->data(0).toString() == "relation") {
            const auto r = item->data(1).toJsonArray().first().toObject();
            if (r.value("type") == "calls" && r.value("line").toInt() == 3) callEdge = item;
        }
        if (item->data(0).toString() == "symbol" && item->data(1).toJsonObject().value("qualified_name").toString() == "Derived::get") getNode = item;
    }
    if (!callEdge || !getNode) { qCritical() << "real graph endpoints missing"; return semanticTestFailure(__LINE__); }
    callEdge->setSelected(true);
    if (!triggerGraphSource(graph, callEdge)) return semanticTestFailure(__LINE__);
    if (source->textCursor().blockNumber() != 2 || graph->findChild<QComboBox *>("relationGraphSites")->count() < 1) return semanticTestFailure(__LINE__);
    scene->clearSelection(); getNode->setSelected(true);
    if (!triggerGraphSource(graph, getNode)) return semanticTestFailure(__LINE__);
    if (source->textCursor().blockNumber() != 1) return semanticTestFailure(__LINE__);
    const auto getId = getNode->data(1).toJsonObject().value("id").toString();
    QGraphicsSceneMouseEvent doubleClick(QEvent::GraphicsSceneMouseDoubleClick);
    doubleClick.setButton(Qt::LeftButton); doubleClick.setButtons(Qt::LeftButton);
    scene->sendEvent(getNode, &doubleClick); app.processEvents();
    if (panel->focusedSymbol() != getId) return semanticTestFailure(__LINE__);
    auto *types = window.findChild<QComboBox *>("semanticRelationType");
    types->setCurrentIndex(types->findData("inherits"));
    // 精简后的详情栏不再输出关系计数说明，直接检查筛选后的真实连线。
    for (auto *item : scene->items()) if (item->data(0) == "relation") return semanticTestFailure(__LINE__);
    types->setCurrentIndex(0);
    QString derivedId;
    for (QTreeWidgetItemIterator it(symbols); *it; ++it) {
        const auto s = (*it)->data(0, Qt::UserRole).toJsonObject();
        if (s.value("qualified_name").toString() == "Derived") derivedId = s.value("id").toString();
    }
    if (derivedId.isEmpty()) return semanticTestFailure(__LINE__);
    panel->focusSymbol(derivedId); graph->setZoomLevel(0.7);
    if (!showGraphDetails(graph, false)) return semanticTestFailure(__LINE__);
    QGraphicsItem *classNode = nullptr;
    for (auto *item : scene->items()) {
        if (item->data(0).toString() != "symbol") continue;
        const auto s = item->data(1).toJsonObject();
        if (s.value("qualified_name") == "Derived::x") return semanticTestFailure(__LINE__);
        if (s.value("id") == derivedId) classNode = item;
    }
    if (!classNode) return semanticTestFailure(__LINE__);
    QGraphicsSceneMouseEvent expandClass(QEvent::GraphicsSceneMouseDoubleClick);
    expandClass.setButton(Qt::LeftButton); expandClass.setButtons(Qt::LeftButton);
    scene->sendEvent(classNode, &expandClass); app.processEvents();
    bool fieldShown = false, inheritanceShown = false;
    for (auto *item : scene->items()) {
        if (item->data(0).toString() == "symbol" && item->data(1).toJsonObject().value("qualified_name") == "Derived::x") fieldShown = true;
        if (item->data(0).toString() == "relation" && item->data(1).toJsonArray().first().toObject().value("type") == "inherits") inheritanceShown = true;
    }
    if (!fieldShown || !inheritanceShown || panel->focusedSymbol() != derivedId) return semanticTestFailure(__LINE__);
    app.processEvents();
    if (!window.grab().save(QCoreApplication::applicationDirPath() + "/relation-class-detail-preview.png")) return semanticTestFailure(__LINE__);
    panel->focusSymbol(mainId);
    app.processEvents();
    if (!window.grab().save(QCoreApplication::applicationDirPath() + "/relation-preview.png")) return semanticTestFailure(__LINE__);
    QFile db(VividCode::semanticDirectory(dir.path()) + "/index.db"); if (!db.open(QIODevice::ReadOnly)) return semanticTestFailure(__LINE__);
    const QByteArray before = db.readAll(); db.close();
    write("test.cpp", "#include \"missing.hpp\"\n");
    panel->setProject(dir.path()); if (!wait()) return semanticTestFailure(__LINE__);
    if (!window.findChild<QLabel *>("semanticStatus")->text().contains("failed")) return semanticTestFailure(__LINE__);
    if (!db.open(QIODevice::ReadOnly) || before != db.readAll()) { qCritical() << "old snapshot changed"; return semanticTestFailure(__LINE__); }
    db.close();
    write("test.cpp", "int main() { return 0; }\n");
    panel->setProject(dir.path());
    // Switching while export/index is pending must stop the old worker and clear its data.
    QTemporaryDir empty;
    window.openProject(empty.path()); app.processEvents();
    if (panel->isBusy() || symbols->topLevelItemCount() || relations->topLevelItemCount() || !scene->items().isEmpty()) return semanticTestFailure(__LINE__);
    if (!VividCode::removeGenerated(dir.path()).ok) return semanticTestFailure(__LINE__);
    qInfo() << "PASS: graph model, real backend graph, nodes/edges source position, focus, filter, failed-refresh preservation, project switch";
    return 0;
}
