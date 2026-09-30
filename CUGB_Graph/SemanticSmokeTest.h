#pragma once
#include "CUGB_Graph.h"
#include "SemanticPanel.h"
#include "RelationGraphTest.h"
#include <QtWidgets>

// 可选真实工程验收：显式指定项目与它的真实编译数据库，索引写入项目 .vividcode。
inline int runRealGraphValidation(QApplication &app, const QString &project, const QString &database) {
    CUGB_Graph window; window.show();
    if (!window.openProject(project)) return 1;
    auto *panel = window.findChild<SemanticPanel *>();
    QEventLoop loop; QTimer deadline; deadline.setSingleShot(true);
    QObject::connect(&deadline, &QTimer::timeout, &loop, &QEventLoop::quit);
    QObject::connect(panel, &SemanticPanel::settled, &loop, &QEventLoop::quit);
    QTimer progress;
    QString previous;
    QObject::connect(&progress, &QTimer::timeout, panel, [&] {
        const auto text = panel->findChild<QLabel *>("semanticStatus")->text();
        if (text != previous) { previous = text; qInfo().noquote() << text; }
    });
    deadline.start(300000); progress.start(2000);
    panel->importDatabase(database); loop.exec(); progress.stop();
    if (!deadline.isActive() || panel->isBusy()) { qCritical() << "real project timeout"; return 1; }
    const auto state = panel->findChild<QLabel *>("semanticStatus")->text();
    if (state.contains("failed") || state.contains(QStringLiteral("失败"))) { qCritical().noquote() << state; return 1; }
    auto *units = panel->findChild<QComboBox *>("semanticUnits");
    int unitIndex = -1;
    for (int i = 0; i < units->count(); ++i) if (units->itemText(i).startsWith("CUGB_Graph.cpp ")) { unitIndex = i; break; }
    if (unitIndex < 0) { qCritical() << "real project unit missing"; return 1; }
    units->setCurrentIndex(unitIndex);
    auto *symbols = panel->findChild<QTreeWidget *>("semanticSymbols");
    QString focusId;
    for (QTreeWidgetItemIterator it(symbols); *it; ++it) {
        const auto s = (*it)->data(0, Qt::UserRole).toJsonObject();
        if (s.value("qualified_name").toString() == "CUGB_Graph::openProject") { focusId = s.value("id").toString(); break; }
    }
    auto *focus = panel->findChild<QComboBox *>("semanticFocus");
    if (focusId.isEmpty() || focus->findData(focusId) < 0) return 1;
    focus->setCurrentIndex(focus->findData(focusId));
    auto *graph = panel->findChild<RelationGraph *>();
    graph->fitGraph();
    app.processEvents();
    if (!window.grab().save(QCoreApplication::applicationDirPath() + "/relation-real-project-overview.png")) return 1;
    graph->setZoomLevel(1.0);
    auto *scene = graph->findChild<QGraphicsScene *>("relationGraphScene");
    QGraphicsItem *scanNode = nullptr, *scanEdge = nullptr;
    for (auto *item : scene->items()) {
        if (item->data(0).toString() == "symbol" && item->data(1).toJsonObject().value("qualified_name") == "scanProject") scanNode = item;
    }
    if (!scanNode) { qCritical() << "real scanProject endpoint missing"; return 1; }
    const auto scanId = scanNode->data(1).toJsonObject().value("id").toString();
    for (auto *item : scene->items()) {
        if (item->data(0).toString() != "relation") continue;
        const auto r = item->data(1).toJsonArray().first().toObject();
        if (r.value("source_id") == focusId && r.value("target_id") == scanId && r.value("type") == "calls") scanEdge = item;
    }
    if (!scanEdge) return 1;
    scanEdge->setSelected(true);
    auto *source = window.findChild<QPlainTextEdit *>("sourceViewer");
    if (!source->textCursor().block().text().contains("scanProject(rootPath)")) { qCritical() << "real relation location mismatch"; return 1; }
    scene->clearSelection(); scanNode->setSelected(true);
    if (!source->textCursor().block().text().contains("scanProject(const QString")) { qCritical() << "real header definition mismatch"; return 1; }
    app.processEvents();
    if (!window.grab().save(QCoreApplication::applicationDirPath() + "/relation-real-project-preview.png")) return 1;
    qInfo().noquote() << "PASS: real project:" << state << graph->findChild<QLabel *>("relationGraphSummary")->text();
    return 0;
}

inline int runSemanticSmokeTest(QApplication &app) {
    const QString fonts = qEnvironmentVariable("WINDIR", "C:/Windows") + "/Fonts/";
    QFontDatabase::addApplicationFont(fonts + "msyh.ttc");
    QFontDatabase::addApplicationFont(fonts + "consola.ttf");
    app.setFont(QFont("Microsoft YaHei", 10));
    if (!runRelationGraphModelTest()) return 1;
    const auto realProject = qEnvironmentVariable("VIVIDCODE_GRAPH_PROJECT");
    if (!realProject.isEmpty()) return runRealGraphValidation(app, realProject, qEnvironmentVariable("VIVIDCODE_GRAPH_DATABASE"));
    QTemporaryDir dir(QDir::tempPath() + QStringLiteral("/vividcode 语义-XXXXXX"));
    if (!dir.isValid()) return 1;
    const auto write = [&](const QString &name, const QByteArray &bytes) {
        QFile f(dir.filePath(name)); return f.open(QIODevice::WriteOnly) && f.write(bytes) == bytes.size();
    };
    if (!write("test.cpp", "struct Base {};\nstruct Derived : Base { int x; int get() { return x; } };\nint main() { Derived d; return d.get(); }\n")) return 1;
    QJsonArray commands{QJsonObject{{"directory", dir.path()}, {"file", "test.cpp"},
        {"arguments", QJsonArray{"clang++", "-std=c++17", "-c", "test.cpp"}}}};
    if (!write("compile_commands.json", QJsonDocument(commands).toJson())) return 1;
    CUGB_Graph window; window.show(); window.openProject(dir.path());
    auto *panel = window.findChild<SemanticPanel *>();
    if (!panel) return 1;
    window.findChild<QTabWidget *>()->setCurrentIndex(1);
    const auto wait = [&] {
        QEventLoop loop; QTimer timer; timer.setSingleShot(true);
        QObject::connect(&timer, &QTimer::timeout, &loop, &QEventLoop::quit);
        QObject::connect(panel, &SemanticPanel::settled, &loop, &QEventLoop::quit);
        timer.start(30000); loop.exec(); return timer.isActive() && !panel->isBusy();
    };
    panel->importDatabase(dir.filePath("compile_commands.json"));
    if (!wait()) { qCritical() << "semantic timeout"; return 1; }
    auto *symbols = panel->findChild<QTreeWidget *>("semanticSymbols");
    auto *relations = panel->findChild<QTreeWidget *>("semanticRelations");
    auto *source = window.findChild<QPlainTextEdit *>("sourceViewer");
    if (!symbols || !relations || symbols->topLevelItemCount() == 0 || relations->topLevelItemCount() == 0) {
        qCritical() << panel->findChild<QLabel *>("semanticStatus")->text(); return 1;
    }
    auto *relation = relations->topLevelItem(0); const int line = relation->data(0,Qt::UserRole).toJsonObject().value("line").toInt();
    relations->setCurrentItem(relation);
    if (!source->toPlainText().contains("Derived") || source->textCursor().blockNumber() != line - 1) return 1;
    app.processEvents(); window.grab().save(QCoreApplication::applicationDirPath() + "/semantic-preview.png");
    auto *focus = panel->findChild<QComboBox *>("semanticFocus");
    auto *graph = panel->findChild<RelationGraph *>();
    auto *scene = graph ? graph->findChild<QGraphicsScene *>("relationGraphScene") : nullptr;
    if (!focus || !graph || !scene) return 1;
    QTreeWidgetItem *mainSymbol = nullptr;
    for (QTreeWidgetItemIterator it(symbols); *it; ++it)
        if ((*it)->data(0, Qt::UserRole).toJsonObject().value("qualified_name").toString() == "main") mainSymbol = *it;
    if (!mainSymbol) return 1;
    const auto mainId = mainSymbol->data(0, Qt::UserRole).toJsonObject().value("id").toString();
    focus->setCurrentIndex(focus->findData(mainId));
    graph->setZoomLevel(1.0);
    window.findChild<QTabWidget *>("semanticViews")->setCurrentIndex(0);
    QGraphicsItem *callEdge = nullptr, *getNode = nullptr;
    for (auto *item : scene->items()) {
        if (item->data(0).toString() == "relation") {
            const auto r = item->data(1).toJsonArray().first().toObject();
            if (r.value("type") == "calls" && r.value("line").toInt() == 3) callEdge = item;
        }
        if (item->data(0).toString() == "symbol" && item->data(1).toJsonObject().value("qualified_name").toString() == "Derived::get") getNode = item;
    }
    if (!callEdge || !getNode) { qCritical() << "real graph endpoints missing"; return 1; }
    callEdge->setSelected(true);
    if (source->textCursor().blockNumber() != 2 || graph->findChild<QComboBox *>("relationGraphSites")->count() < 1) return 1;
    scene->clearSelection(); getNode->setSelected(true);
    if (source->textCursor().blockNumber() != 1) return 1;
    const auto getId = getNode->data(1).toJsonObject().value("id").toString();
    QGraphicsSceneMouseEvent doubleClick(QEvent::GraphicsSceneMouseDoubleClick);
    doubleClick.setButton(Qt::LeftButton); doubleClick.setButtons(Qt::LeftButton);
    scene->sendEvent(getNode, &doubleClick); app.processEvents();
    if (focus->currentData().toString() != getId) return 1;
    auto *types = panel->findChild<QComboBox *>("semanticRelationType");
    types->setCurrentIndex(types->findData("inherits"));
    if (!graph->findChild<QLabel *>("relationGraphDetails")->text().contains(QStringLiteral("没有索引关系"))) return 1;
    types->setCurrentIndex(0);
    QString derivedId;
    for (QTreeWidgetItemIterator it(symbols); *it; ++it) {
        const auto s = (*it)->data(0, Qt::UserRole).toJsonObject();
        if (s.value("qualified_name").toString() == "Derived") derivedId = s.value("id").toString();
    }
    if (derivedId.isEmpty()) return 1;
    focus->setCurrentIndex(focus->findData(derivedId)); graph->setZoomLevel(0.7);
    QGraphicsItem *classNode = nullptr;
    for (auto *item : scene->items()) {
        if (item->data(0).toString() != "symbol") continue;
        const auto s = item->data(1).toJsonObject();
        if (s.value("qualified_name") == "Derived::x") return 1;
        if (s.value("id") == derivedId) classNode = item;
    }
    if (!classNode) return 1;
    QGraphicsSceneMouseEvent expandClass(QEvent::GraphicsSceneMouseDoubleClick);
    expandClass.setButton(Qt::LeftButton); expandClass.setButtons(Qt::LeftButton);
    scene->sendEvent(classNode, &expandClass); app.processEvents();
    bool fieldShown = false, inheritanceShown = false;
    for (auto *item : scene->items()) {
        if (item->data(0).toString() == "symbol" && item->data(1).toJsonObject().value("qualified_name") == "Derived::x") fieldShown = true;
        if (item->data(0).toString() == "relation" && item->data(1).toJsonArray().first().toObject().value("type") == "inherits") inheritanceShown = true;
    }
    if (!fieldShown || !inheritanceShown || focus->currentData().toString() != derivedId) return 1;
    app.processEvents();
    if (!window.grab().save(QCoreApplication::applicationDirPath() + "/relation-class-detail-preview.png")) return 1;
    focus->setCurrentIndex(focus->findData(mainId));
    app.processEvents();
    if (!window.grab().save(QCoreApplication::applicationDirPath() + "/relation-preview.png")) return 1;
    QFile db(dir.filePath(".vividcode/semantic/index.db")); if (!db.open(QIODevice::ReadOnly)) return 1;
    const QByteArray before = db.readAll(); db.close();
    write("test.cpp", "#include \"missing.hpp\"\n");
    panel->setProject(dir.path()); if (!wait()) return 1;
    if (!panel->findChild<QLabel *>("semanticStatus")->text().contains("failed")) return 1;
    if (!db.open(QIODevice::ReadOnly) || before != db.readAll()) { qCritical() << "old snapshot changed"; return 1; }
    db.close();
    write("test.cpp", "int main() { return 0; }\n");
    panel->setProject(dir.path());
    // Switching while export/index is pending must stop the old worker and clear its data.
    QTemporaryDir empty;
    window.openProject(empty.path()); app.processEvents();
    if (panel->isBusy() || symbols->topLevelItemCount() || relations->topLevelItemCount() || !scene->items().isEmpty()) return 1;
    qInfo() << "PASS: graph model, real backend graph, nodes/edges source position, focus, filter, failed-refresh preservation, project switch";
    return 0;
}
