#pragma once
#include "RelationGraph.h"
#include "VividCode.h"
#include "SourcePopup.h"
#include <QtWidgets>

// Source-navigation checks explicitly request details through the real menu;
// zoom is deliberately independent of structure visibility.
inline bool showGraphDetails(RelationGraph *graph, bool details = true) {
    auto *view = graph->findChild<QGraphicsView *>("relationGraphView");
    if (!view) return false;
    QSet<QString> expandedGroups;
    for (;;) {
        QApplication::processEvents();
        QGraphicsItem *target = nullptr;
        for (auto *item : view->scene()->items()) {
            const auto symbol = item->data(1).toJsonObject();
            if (((item->data(0) == "symbol" && symbol.value("is_container").toBool()) || (!details && item->data(0) == "container")) &&
                !expandedGroups.contains(symbol.value("id").toString())) { target = item; break; }
        }
        if (!target) return true;
        expandedGroups.insert(target->data(1).toJsonObject().value("id").toString());
        view->centerOn(target);
        graph->showContextMenu(view->mapFromScene(target->sceneBoundingRect().center()));
        auto *menu = graph->findChild<QMenu *>("graphContextMenu");
        auto *action = menu ? menu->findChild<QAction *>(details ? "graphShowDetailsAction" : "graphShowThumbnailAction") : nullptr;
        if (!action) { if (menu) menu->close(); return false; }
        menu->close(); action->trigger();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    }
}

inline bool triggerGraphSource(RelationGraph *graph, QGraphicsItem *item, int requestedLine = 0) {
    auto *view = graph->findChild<QGraphicsView *>("relationGraphView");
    if (!view || !item) return false;
    // 缩放/摘要文本更新后先完成布局，避免用旧视口尺寸换算右键位置。
    QApplication::processEvents();
    if (!view->scene()->items().contains(item)) return false;
    view->centerOn(item);
    QPointF scenePoint = item->sceneBoundingRect().center();
    if (auto *edge = dynamic_cast<QGraphicsPathItem *>(item)) {
        bool found = false;
        for (int sample = 2; sample < 19; ++sample) {
            const auto candidate = edge->mapToScene(edge->path().pointAtPercent(sample / 20.0));
            auto *hit = view->itemAt(view->mapFromScene(candidate));
            while (hit && hit->data(0).toString().isEmpty()) hit = hit->parentItem();
            if (hit == item) { scenePoint = candidate; found = true; break; }
        }
        // 不同目标的线可能共用一段路径；也可右键该线的文字标签。
        for (auto *child : edge->childItems()) {
            if (found) break;
            const auto bounds = child->boundingRect();
            for (int y = 1; y < 5 && !found; ++y)
                for (int x = 1; x < 10 && !found; ++x) {
                    const auto candidate = child->mapToScene(bounds.topLeft() + QPointF(bounds.width() * x / 10, bounds.height() * y / 5));
                    auto *hit = view->itemAt(view->mapFromScene(candidate));
                    while (hit && hit->data(0).toString().isEmpty()) hit = hit->parentItem();
                    if (hit == item) { scenePoint = candidate; found = true; }
                }
        }
        if (!found) {
            qWarning() << "Graph source test could not hit edge" << edge->sceneBoundingRect()
                       << view->viewport()->rect() << view->transform() << view->sceneRect();
            return false;
        }
    }
    graph->showContextMenu(view->mapFromScene(scenePoint));
    auto *menu = graph->findChild<QMenu *>("graphContextMenu"); if (!menu) return false;
    QAction *action = nullptr;
    for (auto *candidate : menu->findChildren<QAction *>())
        if (candidate->objectName() == "graphViewSourceAction" && (!requestedLine || candidate->data().toJsonObject().value("line").toInt() == requestedLine)) { action = candidate; break; }
    if (!action) { qWarning() << "Graph source test could not find source action"; menu->close(); return false; }
    menu->close(); action->trigger();
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    QApplication::processEvents();
    return true;
}
inline bool triggerFileSource(QTreeWidget *tree, QTreeWidgetItem *item) {
    tree->scrollToItem(item); tree->customContextMenuRequested(tree->visualItemRect(item).center());
    auto *menu = tree->findChild<QMenu *>("fileContextMenu"); if (!menu) return false;
    auto *action = menu->findChild<QAction *>("fileViewSourceAction"); if (!action) { menu->close(); return false; }
    menu->close(); action->trigger(); QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete); QApplication::processEvents(); return true;
}
inline void clickOutsidePopup(VividCodeWindow &window) {
    const QPoint point(10, 10);
    QMouseEvent press(QEvent::MouseButtonPress, QPointF(point), QPointF(window.mapToGlobal(point)), Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(&window, &press);
}
inline bool triggerStructureSource(QTreeWidget *tree, QTreeWidgetItem *item) {
    tree->scrollToItem(item); tree->customContextMenuRequested(tree->visualItemRect(item).center());
    auto *menu = tree->findChild<QMenu *>("structureContextMenu"); if (!menu) return false;
    auto *action = menu->findChild<QAction *>("structureViewSourceAction"); if (!action) { menu->close(); return false; }
    menu->close(); action->trigger(); QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete); QApplication::processEvents(); return true;
}
