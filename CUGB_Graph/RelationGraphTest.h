#pragma once
#include "RelationGraph.h"
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
    QJsonObject unit{{"symbols", QJsonArray{symbol("focus", "()"), symbol("int", "(int)"), symbol("double", "(double)")}},
        {"relations", QJsonArray{call, call, relation("focus", "int", "calls", 20),
            relation("double", "focus", "calls", 30), relation("focus", "focus", "calls", 40),
            relation("focus", "unknown", "possible_call", 50), relation("int", "double", "calls", 60)}}};
    const auto graph = buildRelationNeighborhood(unit, "focus");
    if (!require(graph.totalSites == 5 && graph.displayedSites == 5 && graph.edges.size() == 4 && graph.symbols.size() == 4,
        "one hop, duplicate evidence, aggregated sites, incoming and self-call")) return false;
    if (!require(graph.symbols.value("int").value("signature") != graph.symbols.value("double").value("signature"), "overload identity")) return false;
    if (!require(graph.symbols.value("unknown").value("unresolved").toBool(), "missing endpoint remains explicit")) return false;
    const auto possible = buildRelationNeighborhood(unit, "focus", "possible_call");
    if (!require(possible.edges.size() == 1 && possible.edges.first().confidence == "possible" && possible.symbols.size() == 2, "uncertainty and type filtering")) return false;
    const auto capped = buildRelationNeighborhood(unit, "focus", {}, 2, 2);
    if (!require(capped.symbols.size() <= 2 && capped.edges.size() <= 2 && capped.displayedSites < capped.totalSites && capped.totalSites == 5, "honest display limits")) return false;
    if (!require(buildRelationNeighborhood(unit, {}).symbols.isEmpty(), "no fabricated overview when no focus")) return false;
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
    QList<int> requestedLines;
    QObject::connect(&widget, &RelationGraph::sourceRequested, &widget, [&](const QString &, int line, int) { requestedLines.append(line); });
    QGraphicsItem *repeated = nullptr;
    for (auto *item : scene->items())
        if (item->data(0).toString() == "relation" && item->data(1).toJsonArray().size() == 2) repeated = item;
    if (!require(repeated, "repeated call evidence")) return false;
    repeated->setSelected(true);
    widget.findChild<QComboBox *>("relationGraphSites")->setCurrentIndex(1);
    if (!require(requestedLines == QList<int>{10, 20}, "every aggregated source site remains navigable")) return false;
    unit["relations"] = QJsonArray{call, relation("int", "focus", "calls", 11)};
    widget.setData(unit, "focus", {}, "test.cpp");
    QList<QPainterPath> curves;
    for (auto *item : scene->items())
        if (item->data(0).toString() == "relation") curves.append(static_cast<QGraphicsPathItem *>(item)->path());
    if (!require(curves.size() == 2 && curves[0] != curves[1].toReversed(), "reciprocal arrows have separate curves")) return false;
    widget.setData(classes, "C", {}, "classes.cpp");
    const auto hasMethod = [&] {
        for (auto *item : scene->items()) if (item->data(0).toString() == "symbol" && item->data(1).toJsonObject().value("id") == "C.f") return true;
        return false;
    };
    if (!require(!hasMethod(), "overview hides internal symbols")) return false;
    widget.setZoomLevel(0.95);
    if (!require(hasMethod(), "zoom expands internal symbols")) return false;
    widget.setZoomLevel(0.8);
    if (!require(hasMethod(), "zoom hysteresis avoids level flicker")) return false;
    widget.setZoomLevel(0.7);
    if (!require(!hasMethod(), "zoom out collapses members")) return false;
    return true;
}
