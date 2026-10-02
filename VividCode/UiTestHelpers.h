#pragma once
#include "RelationGraph.h"
#include "VividCode.h"
#include "SourcePopup.h"
#include <QtWidgets>

inline bool triggerGraphSource(RelationGraph *graph, QGraphicsItem *item, int requestedLine = 0) {
    auto *view = graph->findChild<QGraphicsView *>("relationGraphView");
    if (!view || !item) return false;
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
        if (!found) return false;
    }
    graph->showContextMenu(view->mapFromScene(scenePoint));
    auto *menu = graph->findChild<QMenu *>("graphContextMenu"); if (!menu) return false;
    QAction *action = nullptr;
    for (auto *candidate : menu->findChildren<QAction *>())
        if (candidate->objectName() == "graphViewSourceAction" && (!requestedLine || candidate->data().toJsonObject().value("line").toInt() == requestedLine)) { action = candidate; break; }
    if (!action) { menu->close(); return false; }
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
