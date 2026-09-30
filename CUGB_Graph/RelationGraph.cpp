#include "RelationGraph.h"
#include <QtWidgets>
#include <cmath>
#include <functional>

QString relationDisplayName(const QString &type) {
    static const QMap<QString, QString> names{
        {"calls", QStringLiteral("直接调用")}, {"possible_call", QStringLiteral("可能调用")},
        {"inherits", QStringLiteral("继承")}, {"member_ref", QStringLiteral("成员引用")},
        {"references", QStringLiteral("引用")}, {"overrides", QStringLiteral("覆写")}};
    return names.value(type, type.isEmpty() ? QStringLiteral("未分类关系") : type);
}

namespace {
QString key(const QJsonArray &parts) { return QString::fromUtf8(QJsonDocument(parts).toJson(QJsonDocument::Compact)); }
QString symbolName(const QJsonObject &symbol) {
    const auto name = symbol.value("qualified_name").toString();
    return name.isEmpty() ? symbol.value("name").toString(symbol.value("id").toString()) : name;
}
QString symbolPosition(const QJsonObject &symbol) {
    const QString prefix = symbol.value("def_file").toString().isEmpty() ? "decl_" : "def_";
    const auto file = symbol.value(prefix + "file").toString();
    return file.isEmpty() ? QStringLiteral("源码位置不可用") : file + ':' + QString::number(symbol.value(prefix + "line").toInt());
}
QString kindName(const QString &kind) {
    static const QMap<QString, QString> names{{"ClassDecl", QStringLiteral("类")}, {"StructDecl", QStringLiteral("结构体")},
        {"FunctionDecl", QStringLiteral("函数")}, {"CXXMethod", QStringLiteral("方法")},
        {"Constructor", QStringLiteral("构造函数")}, {"Destructor", QStringLiteral("析构函数")},
        {"FieldDecl", QStringLiteral("成员变量")}, {"VarDecl", QStringLiteral("变量")},
        {"Namespace", QStringLiteral("命名空间")}, {"FunctionTemplate", QStringLiteral("函数模板")}};
    if (kind == "FileScope") return QStringLiteral("文件范围");
    if (kind == "ExternalScope") return QStringLiteral("外部符号分组");
    return names.value(kind, kind);
}
QColor edgeColor(const QString &type) {
    static const QMap<QString, QColor> colors{{"calls", QColor("#f2ae62")}, {"possible_call", QColor("#f2ae62")},
        {"inherits", QColor("#62a0ff")}, {"member_ref", QColor("#55c5b5")},
        {"references", QColor("#b4b4b4")}, {"overrides", QColor("#c594e8")}};
    return colors.value(type, QColor("#b4b4b4"));
}

class GraphView : public QGraphicsView {
public:
    using QGraphicsView::QGraphicsView;
    std::function<void(qreal)> changed;
    void zoom(qreal factor) {
        const qreal target = qBound(0.05, transform().m11() * factor, 3.0);
        scale(target / transform().m11(), target / transform().m11());
        if (changed) changed(target);
    }
protected:
    void wheelEvent(QWheelEvent *event) override {
        if (event->angleDelta().y() != 0) { zoom(event->angleDelta().y() > 0 ? 1.2 : 1 / 1.2); event->accept(); }
        else QGraphicsView::wheelEvent(event);
    }
};

class SymbolItem : public QGraphicsRectItem {
public:
    std::function<void()> moved, focused;
    SymbolItem(const QJsonObject &symbol, bool center) : QGraphicsRectItem(0, 0, 260, 92) {
        setFlags(ItemIsSelectable | ItemIsMovable | ItemSendsGeometryChanges);
        setData(0, "symbol"); setData(1, symbol);
        setBrush(QColor(center ? "#25374a" : "#222222"));
        setPen(QPen(QColor(center ? "#62a0ff" : "#666666"), center ? 2.5 : 1.5));
        setZValue(1);
        QString label = kindName(symbol.value("kind").toString());
        if (symbol.value("unresolved").toBool()) label = QStringLiteral("端点信息缺失");
        else if (symbol.value("is_external").toBool()) label += QStringLiteral(" · 外部符号");
        else if (center) label += QStringLiteral(" · 当前焦点");
        const bool container = symbol.value("is_container").toBool();
        if (container) label = QStringLiteral("%1 个相关符号 · 放大展开").arg(symbol.value("member_count").toInt());
        const QStringList lines{symbolName(symbol), container ? kindName(symbol.value("kind").toString()) : symbol.value("signature").toString(), label};
        int y = 8;
        for (int i = 0; i < lines.size(); ++i) {
            auto *text = new QGraphicsSimpleTextItem(this);
            text->setFont(QApplication::font());
            text->setText(QFontMetrics(text->font()).elidedText(lines[i], Qt::ElideMiddle, 240));
            text->setBrush(QColor(i == 0 ? "#f0f0f0" : "#bbbbbb"));
            text->setPos(10, y); text->setAcceptedMouseButtons(Qt::NoButton); y += 26;
        }
        setToolTip(lines.join('\n') + '\n' + symbolPosition(symbol) + (container ? QStringLiteral("\n双击或放大，展开内部符号和关系") : QStringLiteral("\n单击定位源码 · 双击查看此符号的关系 · 可拖动")));
    }
protected:
    QVariant itemChange(GraphicsItemChange change, const QVariant &value) override {
        const auto result = QGraphicsRectItem::itemChange(change, value);
        if (change == ItemPositionHasChanged && moved) moved();
        return result;
    }
    void mouseDoubleClickEvent(QGraphicsSceneMouseEvent *event) override {
        event->accept(); if (focused) focused();
    }
};

class EdgeItem : public QGraphicsPathItem {
public:
    EdgeItem(SymbolItem *source, SymbolItem *target, const RelationEdge &edge, qreal offset)
        : from(source), to(target), bend(offset), color(edgeColor(edge.type)) {
        const bool uncertain = edge.type == "possible_call" || edge.confidence == "possible";
        setPen(QPen(color, 2, uncertain ? Qt::DashLine : Qt::SolidLine));
        setFlags(ItemIsSelectable); setZValue(-1);
        setData(0, "relation"); setData(1, edge.sites);
        const QString name = relationDisplayName(edge.type) + (edge.aggregated ? QStringLiteral("（汇总）") : QString());
        label = new QGraphicsSimpleTextItem(name + (edge.sites.size() > 1 ? QStringLiteral(" ×%1").arg(edge.sites.size()) : QString()), this);
        label->setFont(QApplication::font()); label->setBrush(color); label->setAcceptedMouseButtons(Qt::NoButton);
        QStringList locations;
        for (const auto &site : edge.sites) {
            const auto r = site.toObject();
            locations << r.value("file").toString() + ':' + QString::number(r.value("line").toInt()) + ':' + QString::number(r.value("col").toInt());
        }
        setToolTip(name + (uncertain ? QStringLiteral("（静态分析的可能目标）") : QString()) +
            (edge.aggregated ? QStringLiteral("\n按真实所属类/文件范围汇总；原始端点及发生位置保留") : QString()) + '\n' + locations.join('\n') + QStringLiteral("\n点击连线，在下方选择发生位置"));
        updatePath();
    }
    void updatePath() {
        prepareGeometryChange();
        const auto a = from->sceneBoundingRect(), b = to->sceneBoundingRect();
        QPainterPath curve;
        if (from == to) {
            curve.moveTo(a.topRight() - QPointF(45, 0));
            curve.cubicTo(a.topRight() + QPointF(45, -85 - bend), a.topLeft() + QPointF(-45, -85 - bend), a.topLeft() + QPointF(45, 0));
        } else {
            const qreal direction = b.center().x() >= a.center().x() ? 1 : -1;
            const QPointF start(direction > 0 ? a.right() : a.left(), a.center().y());
            const QPointF end(direction > 0 ? b.left() : b.right(), b.center().y());
            const qreal length = qMax(60.0, qAbs(end.x() - start.x()) / 2);
            curve.moveTo(start); curve.cubicTo(start + QPointF(direction * length, bend), end - QPointF(direction * length, -bend), end);
        }
        const QPointF tip = curve.pointAtPercent(1), before = curve.pointAtPercent(0.97);
        const qreal angle = std::atan2(tip.y() - before.y(), tip.x() - before.x());
        arrow = QPolygonF{tip, tip - QPointF(std::cos(angle - 0.5) * 13, std::sin(angle - 0.5) * 13),
            tip - QPointF(std::cos(angle + 0.5) * 13, std::sin(angle + 0.5) * 13)};
        setPath(curve);
        const auto middle = curve.pointAtPercent(0.5);
        label->setPos(middle - QPointF(label->boundingRect().width() / 2, label->boundingRect().height() + 4));
    }
    QRectF boundingRect() const override { return QGraphicsPathItem::boundingRect().united(arrow.boundingRect()).adjusted(-5, -5, 5, 5); }
    QPainterPath shape() const override {
        QPainterPathStroker stroker; stroker.setWidth(14);
        auto result = stroker.createStroke(path()); result.addPolygon(arrow); return result;
    }
    void paint(QPainter *painter, const QStyleOptionGraphicsItem *, QWidget *) override {
        QPen stroke = pen(); if (isSelected()) { stroke.setColor(Qt::white); stroke.setWidthF(3); }
        painter->setPen(stroke); painter->setBrush(Qt::NoBrush); painter->drawPath(path());
        painter->setPen(Qt::NoPen); painter->setBrush(isSelected() ? QColor(Qt::white) : color); painter->drawPolygon(arrow);
    }
private:
    SymbolItem *from, *to;
    qreal bend;
    QColor color;
    QPolygonF arrow;
    QGraphicsSimpleTextItem *label;
};
}

RelationNeighborhood buildRelationNeighborhood(const QJsonObject &unit, const QString &focus,
    const QString &type, int maxNodes, int maxEdges) {
    return buildRelationMap(unit, focus, type, true, maxNodes, maxEdges);
}

RelationNeighborhood buildRelationMap(const QJsonObject &unit, const QString &focus,
    const QString &type, bool expanded, int maxNodes, int maxEdges) {
    RelationNeighborhood result;
    if (focus.isEmpty() || maxNodes < 1 || maxEdges < 0) return result;
    QMap<QString, QJsonObject> symbols;
    for (const auto &value : unit.value("symbols").toArray()) {
        const auto s = value.toObject(); const auto id = s.value("id").toString();
        if (!id.isEmpty()) symbols.insert(id, s);
    }
    const auto ensureSymbol = [&](const QString &id) {
        return symbols.value(id, QJsonObject{{"id", id}, {"name", QStringLiteral("未解析符号")}, {"unresolved", true}});
    };
    const auto isClass = [](const QJsonObject &s) {
        return QSet<QString>{"ClassDecl", "StructDecl", "UnionDecl", "ClassTemplate"}.contains(s.value("kind").toString());
    };
    QMap<QString, QString> owners;
    const auto ownerClass = [&](const QString &id) {
        if (owners.contains(id)) return owners.value(id);
        QSet<QString> visited;
        QString cursor = id, owner;
        while (!cursor.isEmpty() && !visited.contains(cursor) && symbols.contains(cursor)) {
            visited.insert(cursor);
            if (isClass(symbols.value(cursor))) { owner = cursor; break; }
            cursor = symbols.value(cursor).value("parent_id").toString();
        }
        owners.insert(id, owner); return owner;
    };
    const auto groupFor = [&](const QString &id) {
        if (result.groups.contains(id)) return result.groups.value(id);
        const auto s = ensureSymbol(id);
        QString group = ownerClass(id);
        QJsonObject container;
        if (!group.isEmpty()) container = symbols.value(group);
        else if (s.value("is_external").toBool()) {
            group = "vividcode:external-scope";
            container = {{"id", group}, {"name", QStringLiteral("项目外依赖")}, {"kind", "ExternalScope"}, {"abstract", true}, {"is_external", true}};
        } else {
            const QString file = s.value("decl_file").toString().isEmpty() ? s.value("def_file").toString() : s.value("decl_file").toString();
            if (!file.isEmpty()) {
                group = "vividcode:file-scope:" + file;
                container = {{"id", group}, {"name", QFileInfo(file).fileName()}, {"kind", "FileScope"}, {"abstract", true}, {"scope_file", file}};
            }
        }
        if (group.isEmpty()) group = id;
        else {
            container["is_container"] = true;
            result.containers.insert(group, container);
        }
        result.groups.insert(id, group); return group;
    };
    // 类焦点覆盖属于此类的成员关系；方法焦点保持一层邻域。
    QSet<QString> related{focus};
    const auto focusClass = ownerClass(focus);
    if (isClass(ensureSymbol(focus)))
        for (auto i = symbols.begin(); i != symbols.end(); ++i) if (ownerClass(i.key()) == focus) related.insert(i.key());
    QSet<QString> rawNodes{focus};
    if (expanded && !focusClass.isEmpty()) {
        rawNodes.insert(focusClass);
        for (auto i = symbols.begin(); i != symbols.end(); ++i)
            if (i.value().value("parent_id").toString() == focusClass) rawNodes.insert(i.key());
    }
    result.focusId = expanded ? focus : groupFor(focus);
    QMap<QString, RelationEdge> grouped;
    QSet<QString> evidence;
    for (const auto &value : unit.value("relations").toArray()) {
        const auto r = value.toObject(); const auto a = r.value("source_id").toString(), b = r.value("target_id").toString();
        const auto kind = r.value("type").toString(), confidence = r.value("confidence").toString();
        if (a.isEmpty() || b.isEmpty() || (!related.contains(a) && !related.contains(b)) || (!type.isEmpty() && type != kind)) continue;
        const auto siteKey = key({a, b, kind, confidence, r.value("file"), r.value("line"), r.value("col")});
        if (evidence.contains(siteKey)) continue;
        evidence.insert(siteKey); ++result.totalSites;
        rawNodes.insert(a); rawNodes.insert(b);
        const auto source = expanded ? a : groupFor(a), target = expanded ? b : groupFor(b);
        if (!expanded && source == target && result.containers.contains(source)) { ++result.hiddenInternalSites; continue; }
        const auto groupKey = key({source, target, kind, confidence});
        if (!grouped.contains(groupKey)) grouped.insert(groupKey, {source, target, kind, confidence, {}, source != a || target != b});
        grouped[groupKey].sites.append(r);
    }
    QMap<QString, QSet<QString>> members;
    for (const auto &id : rawNodes) members[groupFor(id)].insert(id);
    result.totalNodes = expanded ? rawNodes.size() : members.size();
    for (auto i = result.containers.begin(); i != result.containers.end(); ++i) i.value()["member_count"] = members.value(i.key()).size();
    const auto displayedSymbol = [&](const QString &id) {
        return !expanded && result.containers.contains(id) ? result.containers.value(id) : ensureSymbol(id);
    };
    result.symbols.insert(result.focusId, displayedSymbol(result.focusId));
    for (const auto &edge : grouped) {
        QSet<QString> additional;
        if (!result.symbols.contains(edge.source)) additional.insert(edge.source);
        if (!result.symbols.contains(edge.target)) additional.insert(edge.target);
        if (result.symbols.size() + additional.size() > maxNodes || result.edges.size() >= maxEdges) continue;
        result.symbols.insert(edge.source, displayedSymbol(edge.source)); result.symbols.insert(edge.target, displayedSymbol(edge.target));
        result.edges.append(edge); result.displayedSites += edge.sites.size();
    }
    // 展开时即使成员没有当前筛选类型的边，也保留真实声明，展示类内部结构。
    if (expanded && !focusClass.isEmpty()) {
        for (const auto &id : rawNodes) {
            if (result.symbols.size() >= maxNodes) break;
            if (id == focusClass || symbols.value(id).value("parent_id").toString() == focusClass)
                result.symbols.insert(id, ensureSymbol(id));
        }
    }
    return result;
}

RelationGraph::RelationGraph(QWidget *parent) : QWidget(parent) {
    auto *layout = new QVBoxLayout(this); layout->setContentsMargins(0, 0, 0, 0);
    summary = new QLabel; summary->setObjectName("relationGraphSummary"); summary->setWordWrap(true); summary->setTextFormat(Qt::PlainText);
    layout->addWidget(summary);
    scene = new QGraphicsScene(this); scene->setObjectName("relationGraphScene");
    auto *graphView = new GraphView(scene); view = graphView; view->setObjectName("relationGraphView");
    graphView->changed = [this](qreal level) { zoomChanged(level); };
    view->setRenderHint(QPainter::Antialiasing); view->setDragMode(QGraphicsView::ScrollHandDrag);
    view->setTransformationAnchor(QGraphicsView::AnchorUnderMouse); view->setBackgroundBrush(palette().brush(QPalette::Window));
    view->setMinimumHeight(180);
    layout->addWidget(view, 1);
    auto *legend = new QLabel(QStringLiteral("箭头：起点 → 终点 · 蓝：继承 · 橙：调用（虚线为可能调用） · 绿：成员引用 · 紫：覆写 · 灰：引用"));
    legend->setWordWrap(true); layout->addWidget(legend);
    details = new QLabel(QStringLiteral("滚轮缩放：缩小时看类，放大后看内部成员；单击定位源码，拖动调整布局。"));
    details->setObjectName("relationGraphDetails"); details->setWordWrap(true); details->setTextFormat(Qt::PlainText); layout->addWidget(details);
    sites = new QComboBox; sites->setObjectName("relationGraphSites"); sites->setEnabled(false); sites->hide(); layout->addWidget(sites);
    sites->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon); sites->setMinimumContentsLength(20);
    connect(scene, &QGraphicsScene::selectionChanged, this, &RelationGraph::inspectSelection);
    connect(sites, &QComboBox::currentIndexChanged, this, [this](int index) {
        if (index < 0) return;
        const auto site = sites->itemData(index).toJsonObject();
        showSite(site);
    });
    setData({}, {}, {}, {});
}

void RelationGraph::setData(const QJsonObject &unit, const QString &focus, const QString &type, const QString &scope) {
    if (focus.isEmpty() || currentUnit.value("config_id") != unit.value("config_id") || currentUnit.value("file") != unit.value("file")) {
        expanded = false; zoomLevel = 0.65;
    }
    currentUnit = unit; currentFocus = focus; currentType = type; currentScope = scope;
    renderMap();
}

void RelationGraph::zoomBy(qreal factor) { static_cast<GraphView *>(view)->zoom(factor); }

void RelationGraph::setZoomLevel(qreal level) {
    level = qBound(0.05, level, 3.0);
    view->resetTransform(); view->scale(level, level); zoomChanged(level);
}

void RelationGraph::zoomChanged(qreal level) {
    zoomLevel = level;
    const bool next = expanded ? level >= 0.75 : level >= 0.9;
    if (next != expanded) { expanded = next; renderMap(); }
}

void RelationGraph::fitGraph() {
    if (scene->items().isEmpty()) return;
    // 适应视图回到概览，避免把几十个方法压成不可读的小字。
    expanded = false; renderMap();
    view->fitInView(scene->itemsBoundingRect().adjusted(-30,-30,30,30), Qt::KeepAspectRatio);
    zoomLevel = qMin(0.7, view->transform().m11());
    view->resetTransform(); view->scale(zoomLevel, zoomLevel);
}

void RelationGraph::renderMap() {
    const QSignalBlocker blocker(scene), siteBlocker(sites);
    scene->clear(); sites->clear(); sites->setEnabled(false); sites->hide();
    details->setText(QStringLiteral("滚轮缩放 · 概览看类，放大看内部成员 · 单击定位源码 · 双击展开/切换焦点 · 拖动调整布局"));
    if (currentFocus.isEmpty()) {
        summary->setText(QStringLiteral("请在上方选择类、函数或成员，查看当前编译单元/配置中的真实关系。")); return;
    }
    const auto neighborhood = buildRelationMap(currentUnit, currentFocus, currentType, expanded);
    const auto focus = neighborhood.focusId;
    summary->setText(QStringLiteral("范围：%1 · 当前 TU/配置 · %2\n%3/%4 个节点，%5 条连线，显示 %6/%7 处关系；概览收起类内 %8 处%9")
        .arg(currentScope, expanded ? QStringLiteral("成员层：方法焦点显示邻域，类焦点显示其成员关系") : QStringLiteral("概览：类/文件范围；放大至 90% 展开内部"))
        .arg(neighborhood.symbols.size()).arg(neighborhood.totalNodes).arg(neighborhood.edges.size()).arg(neighborhood.displayedSites).arg(neighborhood.totalSites).arg(neighborhood.hiddenInternalSites)
        .arg(neighborhood.displayedSites + neighborhood.hiddenInternalSites < neighborhood.totalSites || neighborhood.symbols.size() < neighborhood.totalNodes
            ? QStringLiteral(" · 已达显示上限，可筛选；完整记录在列表中") : QString()));
    if (neighborhood.totalSites == 0) details->setText(QStringLiteral("此符号在当前编译单元/配置和关系筛选下没有索引关系；这不表示全项目没有关系。"));
    QSet<QString> incoming;
    for (const auto &edge : neighborhood.edges) if (edge.target == focus && edge.source != focus) incoming.insert(edge.source);
    QMap<QString, SymbolItem *> nodes;
    QMap<QString, QList<SymbolItem *>> groupNodes;
    QMap<QString, QGraphicsRectItem *> frames;
    QMap<QString, QGraphicsSimpleTextItem *> headings;
    int left = 0, right = 0;
    for (auto i = neighborhood.symbols.begin(); i != neighborhood.symbols.end(); ++i) {
        auto *node = new SymbolItem(i.value(), i.key() == focus); scene->addItem(node);
        if (i.key() == focus) node->setPos(380, 0);
        else if (incoming.contains(i.key())) node->setPos(0, left++ * 135);
        else node->setPos(760, right++ * 135);
        const auto id = i.key(); const bool container = i.value().value("is_container").toBool();
        const bool abstract = i.value().value("abstract").toBool();
        node->focused = [this, id, container, abstract] { QTimer::singleShot(0, this, [this, id, container, abstract] {
            if (!container) { emit focusRequested(id); return; }
            setZoomLevel(1.0);
            if (!abstract) emit focusRequested(id);
            for (auto *item : scene->items())
                if (item->data(0).toString() == "container" && item->data(1).toJsonObject().value("id").toString() == id) { view->centerOn(item); break; }
        }); };
        nodes.insert(id, node);
        groupNodes[neighborhood.groups.value(id, id)].append(node);
    }
    nodes.value(focus)->setPos(380, qMax(0, qMax(left, right) - 1) * 135 / 2.0);
    if (expanded) {
        // 容器只表达索引的真实归属；没有人为添加“调用/继承”线。
        const QString focalGroup = neighborhood.groups.value(focus, focus);
        QSet<QString> incomingGroups;
        for (const auto &edge : neighborhood.edges) {
            const auto a = neighborhood.groups.value(edge.source, edge.source), b = neighborhood.groups.value(edge.target, edge.target);
            if (b == focalGroup && a != focalGroup) incomingGroups.insert(a);
        }
        qreal leftY = 0, rightY = 0;
        for (auto i = groupNodes.begin(); i != groupNodes.end(); ++i) {
            const qreal height = 70 + std::ceil(i.value().size() / 2.0) * 120;
            const bool focal = i.key() == focalGroup;
            const bool in = incomingGroups.contains(i.key());
            const QPointF position(focal ? 700 : (in ? 0 : 1400), focal ? 0 : (in ? leftY : rightY));
            if (!focal) { if (in) leftY += height + 70; else rightY += height + 70; }
            int index = 0;
            for (auto *node : i.value()) { node->setPos(position + QPointF(20 + (index % 2) * 285, 60 + (index / 2) * 120)); ++index; }
            if (neighborhood.containers.contains(i.key())) {
                auto *frame = scene->addRect(QRectF(position, QSizeF(565, height)), QPen(QColor(focal ? "#527ca6" : "#484848"), 1.5), QBrush(QColor("#151b21")));
                frame->setZValue(-3); frame->setAcceptedMouseButtons(Qt::NoButton);
                frame->setData(0, "container"); frame->setData(1, neighborhood.containers.value(i.key()));
                auto *heading = new QGraphicsSimpleTextItem(symbolName(neighborhood.containers.value(i.key())) + QStringLiteral(" · 内部及相关符号"), frame);
                heading->setFont(QApplication::font()); heading->setBrush(QColor("#bbbbbb")); heading->setPos(position + QPointF(20, 15));
                heading->setAcceptedMouseButtons(Qt::NoButton);
                frames.insert(i.key(), frame); headings.insert(i.key(), heading);
            }
        }
    }
    QList<EdgeItem *> edges;
    QMap<QString, int> counts, indices;
    for (const auto &edge : neighborhood.edges) ++counts[key({edge.source, edge.target})];
    for (const auto &edge : neighborhood.edges) {
        const auto pair = key({edge.source, edge.target});
        qreal bend = (indices[pair]++ - (counts[pair] - 1) / 2.0) * 60;
        if (edge.source != edge.target && counts.contains(key({edge.target, edge.source}))) bend += edge.source < edge.target ? 45 : -45;
        auto *item = new EdgeItem(nodes.value(edge.source), nodes.value(edge.target), edge, bend);
        scene->addItem(item); edges.append(item);
    }
    for (auto *node : nodes) node->moved = [edges, frames, headings, groupNodes, this] {
        for (auto *edge : edges) edge->updatePath();
        for (auto i = frames.begin(); i != frames.end(); ++i) {
            QRectF bounds;
            for (auto *member : groupNodes.value(i.key())) bounds = bounds.isNull() ? member->sceneBoundingRect() : bounds.united(member->sceneBoundingRect());
            i.value()->setRect(bounds.adjusted(-20, -45, 20, 20));
            headings.value(i.key())->setPos(i.value()->rect().topLeft() + QPointF(20, 15));
        }
        scene->setSceneRect(scene->itemsBoundingRect().adjusted(-100, -100, 100, 100));
    };
    scene->setSceneRect(scene->itemsBoundingRect().adjusted(-100, -100, 100, 100));
    view->resetTransform(); view->scale(zoomLevel, zoomLevel); view->centerOn(nodes.value(focus));
}

void RelationGraph::inspectSelection() {
    { const QSignalBlocker blocker(sites); sites->clear(); sites->setEnabled(false); sites->hide(); }
    const auto selected = scene->selectedItems(); if (selected.isEmpty()) return;
    auto *item = selected.first();
    if (item->data(0).toString() == "symbol") {
        const auto s = item->data(1).toJsonObject();
        details->setText(symbolName(s) + " · " + symbolPosition(s));
        if (s.value("abstract").toBool()) {
            details->setText(symbolName(s) + QStringLiteral(" · 按真实位置归并的范围，放大或双击查看内部符号。"));
            if (!s.value("scope_file").toString().isEmpty()) emit sourceRequested(s.value("scope_file").toString(), 1, 1);
            return;
        }
        if (s.value("is_external").toBool() || s.value("unresolved").toBool()) {
            details->setText(details->text() + QStringLiteral(" · 外部或未解析符号，当前不能定位项目内源码。")); return;
        }
        const QString prefix = s.value("def_file").toString().isEmpty() ? "decl_" : "def_";
        if (s.value(prefix + "line").toInt() > 0)
            emit sourceRequested(s.value(prefix + "file").toString(), s.value(prefix + "line").toInt(), s.value(prefix + "col").toInt(1));
    } else if (item->data(0).toString() == "relation") {
        const auto positions = item->data(1).toJsonArray();
        if (positions.isEmpty()) return;
        const auto first = positions.first().toObject();
        const QString type = first.value("type").toString();
        static const QMap<QString, QString> explanations{{"calls", QStringLiteral("起点调用终点")}, {"possible_call", QStringLiteral("起点可能调用终点；运行时目标尚未确定")},
            {"inherits", QStringLiteral("起点继承终点")}, {"member_ref", QStringLiteral("起点引用终点成员；暂不区分读写")},
            {"references", QStringLiteral("起点引用终点")}, {"overrides", QStringLiteral("起点覆写终点方法")}};
        details->setText(explanations.value(type, relationDisplayName(type)) + QStringLiteral(" · %1 处源码位置，可在下方逐项选择。").arg(positions.size()));
        { const QSignalBlocker blocker(sites);
            for (const auto &position : positions) {
                const auto r = position.toObject();
                sites->addItem(r.value("file").toString() + ':' + QString::number(r.value("line").toInt()) + ':' + QString::number(r.value("col").toInt()), r);
            }
            sites->setEnabled(true); sites->show();
        }
        showSite(first);
    }
}

void RelationGraph::showSite(const QJsonObject &site) {
    QString a = QStringLiteral("未解析符号"), b = QStringLiteral("未解析符号");
    for (const auto &value : currentUnit.value("symbols").toArray()) {
        const auto s = value.toObject();
        if (s.value("id") == site.value("source_id")) a = symbolName(s);
        if (s.value("id") == site.value("target_id")) b = symbolName(s);
    }
    const QString explanation = a + " → " + b + " · " + relationDisplayName(site.value("type").toString()) +
        QStringLiteral(" · %1 处位置可选").arg(sites->count());
    details->setToolTip(explanation);
    details->setText(QFontMetrics(details->font()).elidedText(explanation, Qt::ElideMiddle, qMax(300, view->viewport()->width() - 12)));
    if (!site.value("file").toString().isEmpty() && site.value("line").toInt() > 0)
        emit sourceRequested(site.value("file").toString(), site.value("line").toInt(), site.value("col").toInt(1));
}
