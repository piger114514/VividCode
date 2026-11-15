#pragma once
#include "RelationGraph.h"
#include "RelationLayout.h"
#include "UiTestHelpers.h"
#include <QtWidgets>

inline bool runRelationGraphModelTest() {
    const auto require = [](bool ok, const char *message) { if (!ok) qCritical() << "FAIL graph:" << message; return ok; };
    const auto symbol = [](const QString &id, const QString &signature) {
        return QJsonObject{{"id", id}, {"qualified_name", "overload"}, {"signature", signature}};
    };
    const auto relation = [](const QString &a, const QString &b, const QString &type, int line) {
        return QJsonObject{{"source_id", a}, {"target_id", b}, {"type", type},
            {"confidence", type == "possible_call" ? "possible" : "definite"}, {"file", "test.cpp"}, {"line", line}, {"col", 3}};
    };
    const auto call = relation("focus", "int", "calls", 10);
    QJsonObject variableUnit{{"symbols", QJsonArray{
        QJsonObject{{"id", "one"}, {"name", "value"}, {"kind", "VarDecl"}, {"language", "c++"}, {"def_file", "a.cpp"}, {"def_line", 2}, {"decl_file", "api.hpp"}, {"decl_line", 3}},
        QJsonObject{{"id", "two"}, {"name", "value"}, {"kind", "VarDecl"}, {"language", "c++"}, {"def_file", "unrelated.cpp"}}}},
        {"relations", QJsonArray{
            QJsonObject{{"source_id", "f"}, {"target_id", "one"}, {"type", "references"}, {"file", "use.cpp"}, {"line", 8}, {"col", 2}},
            QJsonObject{{"source_id", "f"}, {"target_id", "one"}, {"type", "references"}, {"file", "use.cpp"}, {"line", 8}, {"col", 2}},
            QJsonObject{{"source_id", "g"}, {"target_id", "two"}, {"type", "references"}, {"file", "unrelated.cpp"}, {"line", 9}},
            QJsonObject{{"source_id", "f"}, {"target_id", "one"}, {"type", "references"}, {"file", "../outside.cpp"}, {"line", 1}}}}};
    const auto fileUnit = buildSymbolFileUnit(variableUnit, "one", QDir::currentPath());
    QSet<QString> relatedPaths;
    for (const auto &value : fileUnit.value("symbols").toArray()) relatedPaths.insert(value.toObject().value("def_file").toString());
    if (!require(relatedPaths == QSet<QString>{"a.cpp", "api.hpp", "use.cpp"} && fileUnit.value("relations").toArray().size() == 2,
        "related files use exact identity, retain declarations, deduplicate evidence and reject outside paths")) return false;
    for (bool expanded : {false, true}) {
        const auto files = buildRelationMap(fileUnit, {}, {}, expanded);
        if (!require(files.symbols.size() == 3 && files.containers.isEmpty() && files.edges.size() == 2,
            "related file nodes remain files across overview/detail zoom")) return false;
    }
    const auto filteredFiles = buildSymbolFileUnit(variableUnit, "one", QDir::currentPath(), "calls");
    if (!require(filteredFiles.value("symbols").toArray().size() == 2, "filter retains definition and declaration files")) return false;
    QJsonObject unit{{"symbols", QJsonArray{symbol("focus", "()"), symbol("int", "(int)"), symbol("double", "(double)")}},
        {"relations", QJsonArray{call, call, relation("focus", "int", "calls", 20),
            relation("double", "focus", "calls", 30), relation("focus", "focus", "calls", 40),
            relation("focus", "unknown", "possible_call", 50), relation("int", "double", "calls", 60)}}};
    const auto graph = buildRelationMap(unit, "focus", {}, true);
    if (!require(graph.totalSites == 5 && graph.displayedSites == 5 && graph.edges.size() == 4 && graph.symbols.size() == 4,
        "one hop, duplicate evidence, aggregated sites, incoming and self-call")) return false;
    if (!require(graph.symbols.value("int").value("signature") != graph.symbols.value("double").value("signature"), "overload identity")) return false;
    if (!require(graph.symbols.value("unknown").value("unresolved").toBool(), "missing endpoint remains explicit")) return false;
    const auto possible = buildRelationMap(unit, "focus", "possible_call", true);
    if (!require(possible.edges.size() == 1 && possible.edges.first().confidence == "possible" && possible.symbols.size() == 2, "uncertainty and type filtering")) return false;
    const auto capped = buildRelationMap(unit, "focus", {}, true, 2, 2);
    if (!require(capped.symbols.size() <= 2 && capped.edges.size() <= 2 && capped.displayedSites < capped.totalSites && capped.totalSites == 5, "honest display limits")) return false;
    const auto all = buildRelationMap(unit, {}, {}, true);
    if (!require(all.focusId.isEmpty() && all.symbols.size() == 4 && all.edges.size() == 5 && all.totalSites == 6,
        "overview shows every indexed relationship without requiring a focus")) return false;
    const auto classSymbol = [](const QString &id, const QString &kind, const QString &parent = QString()) {
        return QJsonObject{{"id", id}, {"name", id}, {"kind", kind}, {"parent_id", parent}, {"decl_file", "classes.cpp"}};
    };
    QJsonObject classes{{"symbols", QJsonArray{classSymbol("C", "ClassDecl"), classSymbol("C.f", "CXXMethod", "C"),
        classSymbol("C.second", "CXXMethod", "C"), classSymbol("C.field", "FieldDecl", "C"),
        classSymbol("D", "ClassDecl"), classSymbol("D.g", "CXXMethod", "D"), classSymbol("main", "FunctionDecl")}},
        {"relations", QJsonArray{relation("main", "C.f", "calls", 1), relation("C.f", "C.field", "member_ref", 2),
            relation("C.f", "D.g", "calls", 3), relation("C.f", "C.f", "calls", 4), relation("C.second", "D.g", "calls", 5)}}};
    const auto overview = buildRelationMap(classes, "C", {}, false);
    if (!require(overview.focusId == "C" && overview.symbols.size() == 3 && overview.edges.size() == 2 &&
        overview.totalSites == 5 && overview.displayedSites == 3 && overview.hiddenInternalSites == 2,
        "class scope, verified ownership, collapsed internal edges and aggregated member calls")) return false;
    const auto expanded = buildRelationMap(classes, "C", {}, true);
    if (!require(expanded.symbols.contains("C.f") && expanded.symbols.contains("C.second") && expanded.symbols.contains("C.field") &&
        expanded.totalSites == 5 && expanded.displayedSites == 5 && expanded.groups.value("C.f") == "C" && expanded.edges.size() == 5,
        "expanded class members preserve all original endpoints and source evidence")) return false;
    const auto method = buildRelationMap(classes, "C.f", {}, true);
    if (!require(method.symbols.contains("C.second") && method.totalSites == 4 && method.focusId == "C.f", "member structure with selected method neighborhood")) return false;
    RelationGraph widget;
    widget.setData(unit, "focus", {}, "test.cpp");
    auto *scene = widget.findChild<QGraphicsScene *>("relationGraphScene");
    const auto portsMatch = [&] {
        QMap<QString, QGraphicsItem *> nodes;
        for (auto *item : scene->items())
            if (item->data(0) == "symbol") nodes[item->data(1).toJsonObject().value("id").toString()] = item;
        int count = 0;
        for (auto *item : scene->items()) {
            if (item->data(0) != "relation") continue;
            ++count;
            const auto site = item->data(1).toJsonArray().first().toObject();
            auto *referenced = nodes.value(site.value("target_id").toString());
            auto *referrer = nodes.value(site.value("source_id").toString());
            if (!referenced || !referrer) return false;
            const auto a = referenced->sceneBoundingRect(), b = referrer->sceneBoundingRect();
            const auto path = static_cast<QGraphicsPathItem *>(item)->path();
            const auto start = path.elementAt(0), firstControl = path.elementAt(1);
            const auto lastControl = path.elementAt(path.elementCount() - 2), end = path.elementAt(path.elementCount() - 1);
            if (QLineF(QPointF(start.x, start.y), QPointF(a.right(), a.center().y())).length() > 0.001 ||
                QLineF(QPointF(end.x, end.y), QPointF(b.left(), b.center().y())).length() > 0.001 ||
                firstControl.x <= start.x || qAbs(firstControl.y - start.y) > 0.001 ||
                lastControl.x >= end.x || qAbs(lastControl.y - end.y) > 0.001) return false;
        }
        return count > 0;
    };
    if (!require(portsMatch(), "referenced right output to referrer left input, including self and unresolved calls")) return false;
    QGraphicsPathItem *selfEdge = nullptr; QGraphicsItem *focusNode = nullptr;
    for (auto *item : scene->items()) {
        if (item->data(0).toString() == "symbol" && item->data(1).toJsonObject().value("id") == "focus") focusNode = item;
        const auto sites = item->data(1).toJsonArray();
        if (item->data(0).toString() == "relation" && sites.first().toObject().value("source_id") == "focus" && sites.first().toObject().value("target_id") == "focus")
            selfEdge = dynamic_cast<QGraphicsPathItem *>(item);
    }
    if (!require(focusNode && selfEdge && !selfEdge->path().isEmpty(), "self-call curve")) return false;
    const auto before = selfEdge->path(); focusNode->setPos(focusNode->pos() + QPointF(50, 35));
    if (!require(before != selfEdge->path(), "edges follow dragged nodes")) return false;
    if (!require(portsMatch(), "drag keeps fixed input and output ports")) return false;
    QList<int> requestedLines;
    QObject::connect(&widget, &RelationGraph::sourceRequested, &widget, [&](const QString &, int line, int) { requestedLines.append(line); });
    QGraphicsItem *repeated = nullptr;
    for (auto *item : scene->items())
        if (item->data(0).toString() == "relation" && item->data(1).toJsonArray().size() == 2) repeated = item;
    if (!require(repeated, "repeated call evidence")) return false;
    repeated->setSelected(true);
    widget.findChild<QComboBox *>("relationGraphSites")->setCurrentIndex(1);
    if (!require(requestedLines.isEmpty(), "selection does not open source")) return false;
    if (!require(triggerGraphSource(&widget, repeated, 10) && triggerGraphSource(&widget, repeated, 20), "source context actions")) return false;
    if (!require(requestedLines == QList<int>{10, 20}, "every aggregated source site remains navigable")) return false;
    unit["relations"] = QJsonArray{call, relation("int", "focus", "calls", 11)};
    widget.setData(unit, "focus", {}, "test.cpp");
    QList<QPainterPath> curves;
    for (auto *item : scene->items())
        if (item->data(0).toString() == "relation") curves.append(static_cast<QGraphicsPathItem *>(item)->path());
    if (!require(curves.size() == 2 && curves[0] != curves[1].toReversed(), "reciprocal arrows have separate curves")) return false;
    if (!require(portsMatch(), "reciprocal arrows use referenced right and referrer left")) return false;
    QJsonArray allTypes;
    const QStringList types{"calls", "possible_call", "inherits", "implements", "member_ref", "references", "overrides"};
    for (const auto &type : types) allTypes.append(relation("focus", "int", type, 1));
    unit["relations"] = allTypes;
    widget.setData(unit, "focus", {}, "test.cpp");
    if (!require(portsMatch(), "all relationship types share fixed direction and ports")) return false;
    for (auto *item : scene->items())
        if (item->data(0) == "symbol" && item->data(1).toJsonObject().value("id") == "int") {
            item->setPos(-800, 170); break;
        }
    if (!require(portsMatch(), "ports remain fixed when dependency is moved left of referrer")) return false;
    widget.setData(classes, "C", {}, "classes.cpp");
    const auto hasMethod = [&] {
        for (auto *item : scene->items()) if (item->data(0).toString() == "symbol" && item->data(1).toJsonObject().value("id") == "C.f") return true;
        return false;
    };
    if (!require(!hasMethod(), "overview hides internal symbols")) return false;
    widget.setZoomLevel(0.95);
    if (!require(!hasMethod(), "zoom above former threshold keeps overview collapsed")) return false;
    widget.setZoomLevel(0.8);
    if (!require(!hasMethod(), "zoom does not expose internal symbols")) return false;
    widget.setZoomLevel(0.7);
    if (!require(!hasMethod(), "zoom out collapses members")) return false;
    widget.resize(1000, 700); widget.show(); QApplication::processEvents();
    auto *view = widget.findChild<QGraphicsView *>("relationGraphView");
    const auto wheel = [&](int delta) {
        const QPointF point(view->viewport()->rect().center());
        QWheelEvent event(point, view->viewport()->mapToGlobal(point.toPoint()), QPoint(), QPoint(0, delta),
            Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
        QApplication::sendEvent(view->viewport(), &event);
    };
    // 无焦点时跨旧阈值仍只缩放，不能展开或自动适配。
    widget.setData(classes, {}, {}, "classes.cpp");
    widget.setZoomLevel(0.8); wheel(120);
    if (!require(!hasMethod() && qAbs(view->transform().m11() - 0.96) < 0.0001, "overview wheel only scales the collapsed structure")) return false;
    wheel(-120); wheel(-120);
    if (!require(!hasMethod() && qAbs(view->transform().m11() - 2.0 / 3.0) < 0.0001, "wheel down preserves structure and requested zoom")) return false;
    for (int index = 0; index < 60; ++index) wheel(-120);
    if (!require(qAbs(view->transform().m11() - 0.05) < 0.0001, "repeated wheel down remains at minimum")) return false;
    wheel(120);
    if (!require(qAbs(view->transform().m11() - 0.06) < 0.0001, "wheel up from minimum is one step")) return false;
    const auto hasSymbol = [&](const QString &id) {
        for (auto *item : scene->items())
            if (item->data(0) == "symbol" && item->data(1).toJsonObject().value("id") == id) return true;
        return false;
    };
    const auto displayAction = [&](const QString &id, const QString &name) {
        QGraphicsItem *target = nullptr;
        for (auto *item : scene->items())
            if (item->data(0) == "symbol" && item->data(1).toJsonObject().value("id") == id) { target = item; break; }
        if (!target && !id.isEmpty()) return false;
        if (target) view->centerOn(target);
        widget.showContextMenu(target ? view->mapFromScene(target->sceneBoundingRect().center()) : QPoint(1, 1));
        auto *menu = widget.findChild<QMenu *>("graphContextMenu");
        auto *action = menu ? menu->findChild<QAction *>(name) : nullptr;
        if (!action) { if (menu) menu->close(); return false; }
        menu->close(); action->trigger();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete); QApplication::processEvents();
        return true;
    };
    if (!require(displayAction("C", "graphShowDetailsAction") && hasMethod() && !hasSymbol("D.g") &&
        qAbs(view->transform().m11() - 0.06) < 0.0001, "context expands only clicked class without zoom")) return false;
    widget.setData(classes, {}, {}, "classes.cpp");
    if (!require(hasMethod(), "manual class state survives data refresh")) return false;
    widget.setZoomLevel(0.95);
    if (!require(hasMethod() && !hasSymbol("D.g"), "zoom preserves manual details without expanding other classes")) return false;
    widget.setZoomLevel(0.05);
    if (!require(hasMethod(), "minimum zoom preserves manually expanded structure")) return false;
    widget.fitGraph();
    if (!require(hasMethod() && !hasSymbol("D.g"), "fit only changes framing and keeps structure visibility")) return false;
    if (!require(displayAction("C.f", "graphShowThumbnailAction") && !hasMethod() && !hasSymbol("D.g"), "member context collapses owning class only")) return false;
    widget.setZoomLevel(0.7); widget.setZoomLevel(0.95);
    if (!require(!hasMethod() && !hasSymbol("D.g"), "zoom preserves collapsed structure")) return false;
    QGraphicsItem *collapsedClass = nullptr;
    for (auto *item : scene->items()) if (item->data(0) == "symbol" && item->data(1).toJsonObject().value("id") == "C") collapsedClass = item;
    if (!require(collapsedClass, "double click fixture")) return false;
    QGraphicsSceneMouseEvent doubleClick(QEvent::GraphicsSceneMouseDoubleClick);
    doubleClick.setButton(Qt::LeftButton); doubleClick.setButtons(Qt::LeftButton);
    scene->sendEvent(collapsedClass, &doubleClick); QApplication::processEvents();
    if (!require(hasMethod() && !hasSymbol("D.g"), "double click expands only the clicked group")) return false;
    if (!require(displayAction("C.f", "graphShowThumbnailAction") && !hasMethod(), "context can collapse a double-click expansion")) return false;
    widget.setZoomLevel(0.06);
    const QString fileGroup = "vividcode:file-scope::classes.cpp";
    if (!require(displayAction(fileGroup, "graphShowDetailsAction") && hasSymbol("main") && !hasMethod(), "file scope has independent detail mode")) return false;
    for (int index = 0; index < 60; ++index) wheel(120);
    if (!require(qAbs(view->transform().m11() - 3.0) < 0.0001, "repeated wheel up remains at maximum")) return false;
    if (!require(displayAction({}, "graphSimplifyAllAction") && !hasMethod() && !hasSymbol("D.g") && !hasSymbol("main") &&
        qAbs(view->transform().m11() - 3.0) < 0.0001, "simplify all collapses every group at current zoom")) return false;
    widget.setZoomLevel(0.7); widget.setZoomLevel(0.95);
    widget.setData(classes, {}, "calls", "classes.cpp");
    if (!require(!hasMethod() && !hasSymbol("D.g") && !hasSymbol("main"), "simplify all survives zoom, refresh and filtering")) return false;
    if (!require(displayAction("C", "graphShowDetailsAction") && hasMethod() && !hasSymbol("D.g"), "individual groups can expand after simplify all")) return false;
    if (!require(displayAction("C.f", "graphSimplifyAllAction") && !hasMethod(), "simplify all also available from member context")) return false;
    widget.setData(classes, {}, {}, "classes.cpp");
    widget.fitGraph();
    if (!require(view->transform().m11() >= 0.05 && view->transform().m11() <= 0.65, "explicit fit obeys zoom bounds")) return false;
    auto otherProject = classes; otherProject["file"] = "other-project";
    widget.setData(otherProject, {}, {}, "classes.cpp");
    widget.setZoomLevel(0.7);
    if (!require(!hasSymbol("main"), "switching graph clears manual file state")) return false;
    QJsonArray manySymbols, manyRelations;
    for (int index = 0; index < 160; ++index) {
        auto s = classSymbol(QString::number(index), "FunctionDecl");
        s["decl_file"] = QString::number(index) + ".cpp"; manySymbols.append(s);
        if (index) manyRelations.append(relation(QString::number(index), QString::number(index - 1), "calls", index));
    }
    widget.resize(300, 300); QApplication::processEvents();
    widget.setData(QJsonObject{{"file", "large-project"}, {"symbols", manySymbols}, {"relations", manyRelations}}, {}, {}, "large");
    if (!require(qAbs(view->transform().m11() - 0.05) < 0.0001, "initial fit of large overview clamps to minimum")) return false;
    wheel(-120);
    if (!require(qAbs(view->transform().m11() - 0.05) < 0.0001, "wheel down after large fit cannot jump up")) return false;
    auto lateResults = classes; lateResults["file"] = "late-project";
    widget.setData(lateResults, {}, {}, "late");
    lateResults["symbols"] = manySymbols; lateResults["relations"] = manyRelations;
    widget.setData(lateResults, {}, {}, "late");
    if (!require(qAbs(view->transform().m11() - 0.05) < 0.0001, "late language results fit while user has not adjusted view")) return false;
    widget.setZoomLevel(0.2);
    lateResults["symbols"] = classes.value("symbols");
    lateResults["relations"] = classes.value("relations");
    widget.setData(lateResults, {}, {}, "late");
    if (!require(qAbs(view->transform().m11() - 0.2) < 0.0001, "late results preserve user zoom after interaction")) return false;
    widget.setData(QJsonObject{{"file", "waiting-project"}}, {}, {}, "waiting");
    widget.setZoomLevel(0.12);
    lateResults["file"] = "waiting-project";
    widget.setData(lateResults, {}, {}, "waiting");
    if (!require(qAbs(view->transform().m11() - 0.12) < 0.0001, "first results preserve zoom adjusted while analysis was pending")) return false;
    // Verify structural properties rather than fixed pixel snapshots.
    QMap<QString, QSizeF> boxes;
    for (const auto &id : QStringList{"root", "a", "b", "c", "d", "end", "cycle1", "cycle2", "alone"}) boxes[id] = QSizeF(260, 92);
    boxes["a"] = QSizeF(565, 430);
    const QList<QPair<QString, QString>> links{{"root", "a"}, {"root", "b"}, {"a", "d"}, {"b", "c"},
        {"c", "end"}, {"d", "end"}, {"cycle1", "cycle2"}, {"cycle2", "cycle1"}, {"end", "end"}, {"missing", "end"}};
    const auto arranged = layoutRelationGroups(boxes, links);
    const auto noOverlap = [](const QMap<QString, QPointF> &positions, const QMap<QString, QSizeF> &sizes) {
        for (auto a = positions.begin(); a != positions.end(); ++a) for (auto b = positions.begin(); b != a; ++b)
            if (QRectF(a.value(), sizes.value(a.key())).intersects(QRectF(b.value(), sizes.value(b.key())))) return false;
        return true;
    };
    if (!require(arranged.positions.size() == boxes.size() && noOverlap(arranged.positions, boxes), "variable group sizes and disconnected pieces never overlap")) return false;
    for (const auto &link : links) {
        if (!boxes.contains(link.first) || !boxes.contains(link.second) || link.first == link.second || link.first.startsWith("cycle")) continue;
        if (!require(arranged.positions[link.first].x() + boxes[link.first].width() < arranged.positions[link.second].x(), "DAG layers follow displayed dependency direction")) return false;
    }
    if (!require(arranged.cyclicGroups == QSet<QString>{"cycle1", "cycle2"} &&
        arranged.positions["cycle1"].x() == arranged.positions["cycle2"].x(), "cycles retain separate groups in one layer")) return false;
    auto reversedLinks = links; std::reverse(reversedLinks.begin(), reversedLinks.end());
    if (!require(layoutRelationGroups(boxes, reversedLinks).positions == arranged.positions, "layout is deterministic regardless of relation arrival order")) return false;
    for (auto it = boxes.begin(); it != boxes.end(); ++it) it.value() = QSizeF(260, 92);
    const auto reduced = layoutRelationGroups(boxes, links).positions;
    if (!require((reduced["a"].y() - reduced["b"].y()) * (reduced["d"].y() - reduced["c"].y()) > 0,
        "sweeps remove the crossing in a split and merge chain")) return false;
    if (!require(layoutRelationGroups({}, links).positions.isEmpty(), "empty layout is safe")) return false;
    QMap<QString, QSizeF> cycleBoxes;
    QList<QPair<QString, QString>> cycleLinks;
    for (int i = 0; i < 36; ++i) {
        cycleBoxes[QString::number(i)] = QSizeF(260, 92);
        cycleLinks.append({QString::number(i), QString::number((i + 1) % 36)});
    }
    const auto largeCycle = layoutRelationGroups(cycleBoxes, cycleLinks);
    if (!require(largeCycle.cyclicGroups.size() == 36 && noOverlap(largeCycle.positions, cycleBoxes) && largeCycle.size.height() < 1500 && largeCycle.size.width() < 3500,
        "large cyclic regions stay compact while keeping all groups")) return false;
    RelationGraph layoutWidget;
    layoutWidget.resize(1500, 960); layoutWidget.show(); QApplication::processEvents();
    QJsonArray scriptSymbols;
    for (const auto &id : QStringList{"ZData", "MMiddle", "ACaller", "CycleP", "CycleQ", "Independent"}) scriptSymbols.append(classSymbol(id, "ClassDecl"));
    const QJsonObject scripts{{"file", "layout-project"}, {"symbols", scriptSymbols}, {"relations", QJsonArray{
        relation("MMiddle", "ZData", "calls", 1), relation("ACaller", "MMiddle", "calls", 2),
        relation("CycleP", "CycleQ", "calls", 3), relation("CycleQ", "CycleP", "calls", 4)}}};
    layoutWidget.setData(scripts, {}, {}, "layout regression");
    auto *layoutScene = layoutWidget.findChild<QGraphicsScene *>("relationGraphScene");
    QMap<QString, QGraphicsItem *> scriptNodes;
    for (auto *item : layoutScene->items()) if (item->data(0) == "symbol") scriptNodes[item->data(1).toJsonObject().value("id").toString()] = item;
    if (!require(scriptNodes["ZData"]->x() < scriptNodes["MMiddle"]->x() && scriptNodes["MMiddle"]->x() < scriptNodes["ACaller"]->x(),
        "overview positions follow dependencies rather than names")) return false;
    if (!require(scriptNodes["CycleP"]->data(3).toBool() && scriptNodes["CycleQ"]->toolTip().contains(QStringLiteral("循环依赖")), "cycles have visible explanatory labels")) return false;
    for (auto *item : layoutScene->items()) if (item->data(0) == "relation") {
        const auto path = static_cast<QGraphicsPathItem *>(item)->path();
        for (auto node = scriptNodes.begin(); node != scriptNodes.end(); ++node)
            if (node.key() != item->data(2).toString() && node.key() != item->data(3).toString())
                if (!require(!path.intersects(node.value()->sceneBoundingRect()), "cycle routing stays clear of unrelated component cards")) return false;
    }
    const QPointF beforeSelection = scriptNodes["ACaller"]->pos();
    scriptNodes["ZData"]->setSelected(true);
    if (!require(scriptNodes["ZData"]->opacity() == 1 && scriptNodes["MMiddle"]->opacity() == 1 && scriptNodes["ACaller"]->opacity() < 0.5 &&
        scriptNodes["Independent"]->opacity() < 0.5 && scriptNodes["ACaller"]->pos() == beforeSelection, "node selection highlights one hop without moving nodes")) return false;
    int lit = 0, dim = 0;
    for (auto *item : layoutScene->items()) if (item->data(0) == "relation") { if (item->opacity() == 1) ++lit; else ++dim; }
    if (!require(lit == 1 && dim == 3, "only incident edges are highlighted")) return false;
    layoutScene->clearSelection();
    QGraphicsItem *selectedEdge = nullptr;
    for (auto *item : layoutScene->items()) {
        if (!require(item->opacity() == 1, "clearing selection restores the overview")) return false;
        if (item->data(0) == "relation" && item->data(2) == "ZData") selectedEdge = item;
    }
    if (!require(selectedEdge, "edge selection fixture")) return false;
    selectedEdge->setSelected(true);
    if (!require(scriptNodes["ZData"]->opacity() == 1 && scriptNodes["MMiddle"]->opacity() == 1 && scriptNodes["ACaller"]->opacity() < 0.5 && selectedEdge->opacity() == 1,
        "edge selection highlights its displayed endpoints")) return false;
    layoutScene->clearSelection();
    const QString screenshot = qEnvironmentVariable("VIVIDCODE_LAYOUT_SCREENSHOT");
    if (!screenshot.isEmpty()) {
        layoutWidget.fitGraph(); QApplication::processEvents();
        if (!require(layoutWidget.grab().save(screenshot), "save layout review screenshot")) return false;
    }
    QJsonArray mixedSymbols, mixedRelations;
    for (const auto &language : QStringList{"c#", "python"}) {
        for (const auto &suffix : QStringList{"P", "Q"}) {
            auto s = classSymbol(language + suffix, "ClassDecl"); s["language"] = language; mixedSymbols.append(s);
        }
        auto a = relation(language + "P", language + "Q", "calls", 1); a["language"] = language; mixedRelations.append(a);
        auto b = relation(language + "Q", language + "P", "calls", 2); b["language"] = language; mixedRelations.append(b);
    }
    layoutWidget.setData(QJsonObject{{"file", "mixed-layout"}, {"symbols", mixedSymbols}, {"relations", mixedRelations}}, {}, {}, "mixed layout");
    layoutWidget.setZoomLevel(0.95);
    if (!require(showGraphDetails(&layoutWidget), "mixed partitions expand through context menus")) return false;
    QList<QRectF> partitions;
    for (auto *item : layoutScene->items()) if (item->data(0) == "languagePartition") partitions.append(item->sceneBoundingRect());
    if (!require(partitions.size() == 2 && !partitions[0].intersects(partitions[1]), "language partitions include expanded groups and cycle routing margins")) return false;
    auto *panView = layoutWidget.findChild<QGraphicsView *>("relationGraphView");
    auto *legendOverlay = layoutWidget.findChild<QWidget *>("graphLegendCard");
    layoutWidget.resize(900, 700); layoutWidget.show(); QApplication::processEvents();
    panView->setSceneRect(-3000, -3000, 6000, 6000);
    panView->horizontalScrollBar()->setValue(0); panView->verticalScrollBar()->setValue(0);
    const QPoint legendBefore = legendOverlay->pos();
    int menus = 0;
    QObject::connect(panView, &QWidget::customContextMenuRequested, &layoutWidget, [&](const QPoint &) { ++menus; });
    const QPoint start(140, 140), finish(230, 190);
    auto sendMouse = [&](QEvent::Type type, QPoint point, Qt::MouseButton button, Qt::MouseButtons buttons) {
        QMouseEvent event(type, QPointF(point), QPointF(panView->viewport()->mapToGlobal(point)), button, buttons, Qt::NoModifier);
        QApplication::sendEvent(panView->viewport(), &event);
    };
    const int beforePan = panView->horizontalScrollBar()->value();
    sendMouse(QEvent::MouseButtonPress, start, Qt::RightButton, Qt::RightButton);
    sendMouse(QEvent::MouseMove, finish, Qt::NoButton, Qt::RightButton);
    sendMouse(QEvent::MouseButtonRelease, finish, Qt::RightButton, Qt::NoButton);
    QApplication::processEvents();
    if (!require(panView->horizontalScrollBar()->value() != beforePan && menus == 0, "right drag pans without opening menu")) return false;
    if (!require(legendOverlay->pos() == legendBefore, "legend stays anchored during pan")) return false;
    layoutWidget.setZoomLevel(1.2); layoutWidget.resize(1000, 750); QApplication::processEvents();
    const QRect viewportArea = panView->viewport()->geometry();
    if (!require(qAbs(legendOverlay->geometry().right() - (viewportArea.right() - 12)) <= 1 && legendOverlay->y() == viewportArea.top() + 12,
        "legend anchors to resized viewport after zoom")) return false;
    sendMouse(QEvent::MouseButtonPress, start, Qt::RightButton, Qt::RightButton);
    sendMouse(QEvent::MouseButtonRelease, start, Qt::RightButton, Qt::NoButton);
    if (!require(menus == 1, "right click still opens menu")) return false;
    for (auto *menu : layoutWidget.findChildren<QMenu *>()) menu->close();
    return true;
}
