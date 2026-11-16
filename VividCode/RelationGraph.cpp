#include "UiLanguage.h"
#include "RelationGraph.h"
#include "RelationLayout.h"
#include <QtWidgets>
#include <cmath>
#include <functional>

QString relationDisplayName(const QString &type) {
    const QMap<QString, QString> names{
        {"calls", UiLanguage::text("Direct call")}, {"possible_call", UiLanguage::text("Possible call")},
        {"inherits", UiLanguage::text("Inheritance")}, {"implements", UiLanguage::text("Implementation")}, {"member_ref", UiLanguage::text("Member reference")},
        {"references", UiLanguage::text("Reference")}, {"overrides", UiLanguage::text("Override")}};
    return names.value(type, type.isEmpty() ? UiLanguage::text("Unclassified relation") : type);
}

namespace {
struct FileLanguageStyle { QString name; QColor color; };
const QMap<QString, FileLanguageStyle> &fileLanguagePalette() {
    static const QMap<QString, FileLanguageStyle> palette{
        {"c++", {"C/C++", QColor("#6fa8ff")}}, {"c#", {"C#", QColor("#bb91f5")}},
        {"python", {"Python", QColor("#72c99a")}}, {"javascript", {"JavaScript", QColor("#e3c76b")}},
        {"typescript", {"TypeScript", QColor("#61cadd")}}, {"java", {"Java", QColor("#f0a16a")}}};
    return palette;
}
FileLanguageStyle sourceLanguageStyle(const QJsonObject &symbol) {
    QString file = symbol.value("scope_file").toString();
    if (file.isEmpty()) file = symbol.value("def_file").toString();
    if (file.isEmpty()) file = symbol.value("decl_file").toString();
    const QString extension = QFileInfo(file).suffix().toLower();
    QString language = symbol.value("language").toString();
    if (QSet<QString>{"ts", "tsx", "mts", "cts"}.contains(extension)) language = "typescript";
    else if (QSet<QString>{"js", "jsx", "mjs", "cjs"}.contains(extension)) language = "javascript";
    else if (language.isEmpty()) {
        if (extension == "cs") language = "c#";
        else if (extension == "java") language = "java";
        else if (QSet<QString>{"py", "pyw", "pyi"}.contains(extension)) language = "python";
        else if (QSet<QString>{"c", "cc", "cpp", "cxx", "h", "hh", "hpp", "hxx", "inl", "ipp"}.contains(extension)) language = "c++";
    }
    return fileLanguagePalette().value(language);
}
FileLanguageStyle fileLanguageStyle(const QJsonObject &symbol) {
    if (symbol.value("kind") != "FileScope" && symbol.value("kind") != "RelatedFile") return {};
    return sourceLanguageStyle(symbol);
}
QColor fileBackground(const QColor &accent, bool focused = false) {
    const qreal amount = focused ? 0.28 : 0.15;
    return QColor(qRound(24 * (1 - amount) + accent.red() * amount),
        qRound(24 * (1 - amount) + accent.green() * amount), qRound(24 * (1 - amount) + accent.blue() * amount));
}
QString key(const QJsonArray &parts) { return QString::fromUtf8(QJsonDocument(parts).toJson(QJsonDocument::Compact)); }
QString symbolName(const QJsonObject &symbol) {
    const auto name = symbol.value("qualified_name").toString();
    return name.isEmpty() ? symbol.value("name").toString(symbol.value("id").toString()) : name;
}
QString symbolPosition(const QJsonObject &symbol) {
    const QString prefix = symbol.value("def_file").toString().isEmpty() ? "decl_" : "def_";
    const auto file = symbol.value(prefix + "file").toString();
    return file.isEmpty() ? UiLanguage::text("Source location unavailable") : file + ':' + QString::number(symbol.value(prefix + "line").toInt());
}
QString kindName(const QString &kind) {
    const QMap<QString, QString> names{{"ClassDecl", UiLanguage::text("Class")}, {"StructDecl", UiLanguage::text("Struct")},
        {"FunctionDecl", UiLanguage::text("Function")}, {"CXXMethod", UiLanguage::text("Method")},
        {UiLanguage::text("Constructor"), UiLanguage::text("Constructor")}, {UiLanguage::text("Destructor"), UiLanguage::text("Destructor")},
        {"FieldDecl", UiLanguage::text("Field")}, {"VarDecl", UiLanguage::text("Variable")}, {"ParmDecl", UiLanguage::text("Parameter")}, {"RelatedFile", UiLanguage::text("Related file")},
        {"InterfaceDecl", UiLanguage::text("Interface")}, {"PropertyDecl", UiLanguage::text("Property")},
        {"EventDecl", UiLanguage::text("Event")}, {"EnumDecl", UiLanguage::text("Enum")}, {"DelegateDecl", UiLanguage::text("Delegate")},
        {UiLanguage::text("Namespace"), UiLanguage::text("Namespace")}, {"FunctionTemplate", UiLanguage::text("Function template")},
        {"ModuleDecl", UiLanguage::text("Module")}, {"UnresolvedCall", UiLanguage::text("Unresolved target")}};
    if (kind == "FileScope") return UiLanguage::text("File scope");
    if (kind == "ExternalScope") return UiLanguage::text("External scope");
    return names.value(kind, kind);
}
QColor edgeColor(const QString &type) {
    static const QMap<QString, QColor> colors{{"calls", QColor("#f2ae62")}, {"possible_call", QColor("#f2ae62")},
        {"inherits", QColor("#62a0ff")}, {"implements", QColor("#62a0ff")}, {"member_ref", QColor("#55c5b5")},
        {"references", QColor("#b4b4b4")}, {"overrides", QColor("#c594e8")}};
    return colors.value(type, QColor("#b4b4b4"));
}

class GraphView : public QGraphicsView {
public:
    using QGraphicsView::QGraphicsView;
    std::function<void(qreal)> changed;
    QWidget *legendOverlay = nullptr;
    void positionLegend() {
        if (!legendOverlay) return;
        legendOverlay->adjustSize();
        const QRect area = viewport()->geometry();
        legendOverlay->move(qMax(area.left(), area.right() - legendOverlay->width() - 11), area.top() + 12);
        legendOverlay->raise();
    }
    bool rightPressed = false, rightDragged = false;
    QPoint rightStart, rightLast;

    void zoom(qreal factor) {
        const qreal target = qBound(0.05, transform().m11() * factor, 3.0);
        if (qFuzzyCompare(target, transform().m11())) return;
        scale(target / transform().m11(), target / transform().m11());
        if (changed) changed(target);
    }
protected:
    void resizeEvent(QResizeEvent *event) override {
        QGraphicsView::resizeEvent(event); positionLegend();
    }
    void scrollContentsBy(int dx, int dy) override {
        QGraphicsView::scrollContentsBy(dx, dy); positionLegend();
    }
    void mousePressEvent(QMouseEvent *event) override {
        if (event->button() == Qt::RightButton) {
            rightPressed = true; rightDragged = false;
            rightStart = rightLast = event->position().toPoint(); event->accept(); return;
        }
        QGraphicsView::mousePressEvent(event);
    }
    void mouseMoveEvent(QMouseEvent *event) override {
        if (rightPressed) {
            const QPoint point = event->position().toPoint();
            if (!rightDragged && (point - rightStart).manhattanLength() >= QApplication::startDragDistance()) {
                rightDragged = true; viewport()->setCursor(Qt::ClosedHandCursor);
            }
            if (rightDragged) {
                const QPoint delta = point - rightLast;
                horizontalScrollBar()->setValue(horizontalScrollBar()->value() - delta.x());
                verticalScrollBar()->setValue(verticalScrollBar()->value() - delta.y());
                rightLast = point;
            }
            event->accept(); return;
        }
        QGraphicsView::mouseMoveEvent(event);
    }
    void mouseReleaseEvent(QMouseEvent *event) override {
        if (event->button() == Qt::RightButton && rightPressed) {
            rightPressed = false; viewport()->unsetCursor();
            if (!rightDragged) emit customContextMenuRequested(event->position().toPoint());
            event->accept(); return;
        }
        QGraphicsView::mouseReleaseEvent(event);
    }
    bool viewportEvent(QEvent *event) override {
        if (event->type() == QEvent::ContextMenu && static_cast<QContextMenuEvent *>(event)->reason() == QContextMenuEvent::Mouse) {
            event->accept(); return true;
        }
        return QGraphicsView::viewportEvent(event);
    }
    void wheelEvent(QWheelEvent *event) override {
        if (event->angleDelta().y() != 0) { zoom(event->angleDelta().y() > 0 ? 1.2 : 1 / 1.2); event->accept(); }
        else QGraphicsView::wheelEvent(event);
    }
};

class SymbolItem : public QGraphicsRectItem {
public:
    std::function<void()> moved, focused;
    SymbolItem(const QJsonObject &symbol, bool center, bool cyclic = false) : QGraphicsRectItem(0, 0, 260, 92) {
        setFlags(ItemIsSelectable | ItemIsMovable | ItemSendsGeometryChanges);
        setData(0, "symbol"); setData(1, symbol);
        setBrush(QColor(center ? "#25374a" : "#222222"));
        setPen(QPen(QColor(center ? "#62a0ff" : "#666666"), center ? 2.5 : 1.5));
        const auto fileStyle = fileLanguageStyle(symbol);
        if (fileStyle.color.isValid()) {
            setBrush(fileBackground(fileStyle.color, center));
            setPen(QPen(fileStyle.color, center ? 2.5 : 1.5));
        }
        setZValue(1);
        QString label = kindName(symbol.value("kind").toString());
        if (symbol.value("unresolved").toBool()) label = UiLanguage::text("Unresolved target");
        else if (symbol.value("is_external").toBool()) label += UiLanguage::text(" - external symbol");
        else if (center) label += UiLanguage::text(" - current focus");
        const bool container = symbol.value("is_container").toBool();
        if (container) label = UiLanguage::text("%1 related symbols").arg(symbol.value("member_count").toInt());
        if (fileStyle.color.isValid()) label += " · " + fileStyle.name;
        if (cyclic) label = UiLanguage::text("Dependency cycle - ") + label;
        const QStringList lines{symbolName(symbol), container ? kindName(symbol.value("kind").toString()) : symbol.value("signature").toString(), label};
        int y = 8;
        for (int i = 0; i < lines.size(); ++i) {
            auto *text = new QGraphicsSimpleTextItem(this);
            text->setFont(QApplication::font());
            text->setText(QFontMetrics(text->font()).elidedText(lines[i], Qt::ElideMiddle, 240));
            text->setBrush(i == 0 && fileStyle.color.isValid() ? fileStyle.color : QColor(i == 0 ? "#f0f0f0" : "#bbbbbb"));
            text->setPos(10, y); text->setAcceptedMouseButtons(Qt::NoButton); y += 26;
        }
        setData(3, cyclic);
        setToolTip(lines.join('\n') + '\n' + symbolPosition(symbol) + (cyclic ? UiLanguage::text("\nVisible inter-group dependency cycle: groups share a compact region at the same dependency level; internal relations are retained.") : QString()) + (symbol.value("kind") == "RelatedFile" ? UiLanguage::text("\nRight-click for definitions, declarations and references in this file. Drag to move.") : container ? QString() : UiLanguage::text("\nRight-click for source/display options. Double-click for relations. Drag to move.")));
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
        setData(2, edge.target); setData(3, edge.source);
        const QString name = relationDisplayName(edge.type);
        QStringList locations;
        for (const auto &site : edge.sites) {
            const auto r = site.toObject();
            locations << r.value("file").toString() + ':' + QString::number(r.value("line").toInt()) + ':' + QString::number(r.value("col").toInt());
        }
        setToolTip(name + (uncertain ? UiLanguage::text(" (possible static-analysis target)") : QString()) +
            (edge.aggregated ? UiLanguage::text("\nGrouped by actual owning class/file scope; original endpoints and occurrence locations retained") : QString()) + '\n' + locations.join('\n') + UiLanguage::text("\nClick the edge, then select an occurrence below"));
        updatePath();
    }
    void updatePath() {
        prepareGeometryChange();
        const auto a = from->sceneBoundingRect(), b = to->sceneBoundingRect();
        const QPointF start(a.right(), a.center().y()), end(b.left(), b.center().y());
        QPainterPath curve;
        curve.moveTo(start);
        if (from == to || end.x() - start.x() < 60) {
            // 反向摆放及自调用从上方绕回，仍然右侧出、左侧进。
            const qreal clearance = 70 + qAbs(bend);
            const qreal top = qMin(a.top(), b.top()) - clearance;
            const QPointF middle((start.x() + end.x()) / 2, top);
            curve.cubicTo(start + QPointF(clearance, 0), QPointF(start.x() + clearance, top), middle);
            curve.cubicTo(QPointF(end.x() - clearance, top), end - QPointF(clearance, 0), end);
        } else {
            const qreal length = qMax(30.0, (end.x() - start.x()) / 4);
            const QPointF middle = (start + end) / 2 + QPointF(0, bend);
            curve.cubicTo(start + QPointF(length, 0), middle - QPointF(length, 0), middle);
            curve.cubicTo(middle + QPointF(length, 0), end - QPointF(length, 0), end);
        }
        // 终点切线固定向右，箭头准确指向引用者的左侧。
        arrow = QPolygonF{end, end - QPointF(std::cos(0.5) * 13, -std::sin(0.5) * 13),
            end - QPointF(std::cos(0.5) * 13, std::sin(0.5) * 13)};
        setPath(curve);
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
};
}

QJsonObject buildSymbolFileUnit(const QJsonObject &unit, const QString &symbolId, const QString &root, const QString &type) {
    QJsonObject symbol;
    for (const auto &value : unit.value("symbols").toArray()) if (value.toObject().value("id") == symbolId) { symbol = value.toObject(); break; }
    const auto path = [&root](const QString &file) {
        if (file.isEmpty()) return QString();
        const auto relative = QDir::cleanPath(QDir::isAbsolutePath(file) ? QDir(root).relativeFilePath(file) : file);
        return relative == ".." || relative.startsWith("../") || QDir::isAbsolutePath(relative) ? QString() : relative;
    };
    const QString definition = path(symbol.value("def_file").toString()).isEmpty() ? path(symbol.value("decl_file").toString()) : path(symbol.value("def_file").toString());
    QMap<QString,QJsonArray> files; QJsonArray edges; QSet<QString> seen;
    const auto fileId = [&symbolId](const QString &file) { return "vividcode:related-file:" + symbolId + ':' + file; };
    const auto add = [&](QJsonObject site, const QString &role, bool edge) {
        const QString file = path(site.value("file").toString()); if (file.isEmpty()) return;
        site["file"] = file; site["role"] = role;
        const QString identity = key({file, site.value("line"), site.value("col")});
        if (seen.contains(identity)) return; seen.insert(identity); files[file].append(site);
        if (edge && !definition.isEmpty()) {
            site["source_id"] = fileId(file); site["target_id"] = fileId(definition);
            site["language"] = symbol.value("language"); edges.append(site);
        }
    };
    for (const QString prefix : {QString("def_"), QString("decl_")}) {
        if (symbol.value(prefix + "file").toString().isEmpty()) continue;
        const QString role = prefix == "def_" ? UiLanguage::text("Definition") : UiLanguage::text("Declaration");
        add({{"file", symbol.value(prefix + "file")}, {"line", symbol.value(prefix + "line")}, {"col", symbol.value(prefix + "col")}, {"type", "references"}, {"confidence", "static"}}, role, path(symbol.value(prefix + "file").toString()) != definition);
    }
    for (const auto &value : unit.value("relations").toArray()) {
        const auto relation = value.toObject();
        if (relation.value("target_id") != symbolId || (!type.isEmpty() && relation.value("type") != type)) continue;
        add(relation, relationDisplayName(relation.value("type").toString()), true);
    }
    QJsonArray nodes;
    for (auto it = files.begin(); it != files.end(); ++it) {
        const auto first = it.value().first().toObject();
        nodes.append(QJsonObject{{"id", fileId(it.key())}, {"kind", "RelatedFile"}, {"name", QFileInfo(it.key()).fileName()}, {"qualified_name", it.key()},
            {"language", symbol.value("language")}, {"def_file", it.key()}, {"def_line", first.value("line")}, {"def_col", first.value("col")},
            {"signature", UiLanguage::text("%1 - %2 indexed locations").arg(it.key() == definition ? UiLanguage::text("Definition file") : UiLanguage::text("Related file")).arg(it.value().size())},
            {"occurrence_sites", it.value()}, {"is_external", false}});
    }
    return {{"symbols", nodes}, {"relations", edges}, {"file", root}, {"config_id", "symbol-files:" + symbolId}, {"symbol_files", true}, {"symbol_name", symbolName(symbol)}};
}

RelationNeighborhood buildRelationMap(const QJsonObject &unit, const QString &focus,
    const QString &type, bool expanded, int maxNodes, int maxEdges, const QMap<QString, bool> &groupOverrides) {
    RelationNeighborhood result;
    if (maxNodes < 1 || maxEdges < 0) return result;
    QMap<QString, QJsonObject> symbols;
    for (const auto &value : unit.value("symbols").toArray()) {
        const auto s = value.toObject(); const auto id = s.value("id").toString();
        if (!id.isEmpty()) symbols.insert(id, s);
    }
    const auto ensureSymbol = [&](const QString &id) {
        return symbols.value(id, QJsonObject{{"id", id}, {"name", UiLanguage::text("Unresolved symbol")}, {"unresolved", true}});
    };
    const auto isClass = [](const QJsonObject &s) {
        return QSet<QString>{"ClassDecl", "StructDecl", "UnionDecl", "ClassTemplate", "InterfaceDecl", "EnumDecl", "DelegateDecl"}.contains(s.value("kind").toString());
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
        if (s.value("kind") == "RelatedFile") { result.groups.insert(id, id); return id; }
        QString group = ownerClass(id);
        QJsonObject container;
        if (!group.isEmpty()) container = symbols.value(group);
        else if (s.value("is_external").toBool()) {
            group = "vividcode:external-scope:" + s.value("language").toString();
            container = {{"id", group}, {"name", s.value("language") == "python" || s.value("language") == "javascript" ? UiLanguage::text("External dependencies / unresolved targets") : UiLanguage::text("External dependencies")}, {"kind", "ExternalScope"}, {"abstract", true}, {"is_external", true}, {"language", s.value("language")}};
        } else {
            const QString file = s.value("decl_file").toString().isEmpty() ? s.value("def_file").toString() : s.value("decl_file").toString();
            if (!file.isEmpty()) {
                group = "vividcode:file-scope:" + s.value("language").toString() + ':' + file;
                container = {{"id", group}, {"name", QFileInfo(file).fileName()}, {"kind", "FileScope"}, {"abstract", true}, {"scope_file", file}, {"language", s.value("language")}};
            }
        }
        if (group.isEmpty()) group = id;
        else {
            container["is_container"] = true;
            result.containers.insert(group, container);
        }
        result.groups.insert(id, group); return group;
    };
    const auto isExpanded = [&](const QString &id) { return groupOverrides.value(groupFor(id), expanded); };
    const auto displayId = [&](const QString &id) { return isExpanded(id) ? id : groupFor(id); };
    // 类焦点覆盖属于此类的成员关系；方法焦点保持一层邻域。
    QSet<QString> related;
    if (!focus.isEmpty()) related.insert(focus);
    const auto focusClass = ownerClass(focus);
    if (isClass(ensureSymbol(focus)))
        for (auto i = symbols.begin(); i != symbols.end(); ++i) if (ownerClass(i.key()) == focus) related.insert(i.key());
    QSet<QString> rawNodes;
    if (!focus.isEmpty()) rawNodes.insert(focus);
    else for (auto it = symbols.begin(); it != symbols.end(); ++it)
        if (!it.value().value("is_external").toBool()) { rawNodes.insert(it.key()); related.insert(it.key()); }
    if (isExpanded(focus) && !focusClass.isEmpty()) {
        rawNodes.insert(focusClass);
        for (auto i = symbols.begin(); i != symbols.end(); ++i)
            if (i.value().value("parent_id").toString() == focusClass) rawNodes.insert(i.key());
    }
    result.focusId = focus.isEmpty() ? QString() : displayId(focus);
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
        const auto source = displayId(a), target = displayId(b);
        if (!isExpanded(a) && source == target && result.containers.contains(source)) { ++result.hiddenInternalSites; continue; }
        const auto groupKey = key({source, target, kind, confidence});
        if (!grouped.contains(groupKey)) grouped.insert(groupKey, {source, target, kind, confidence, {}, source != a || target != b});
        grouped[groupKey].sites.append(r);
    }
    QMap<QString, QSet<QString>> members;
    for (const auto &id : rawNodes) members[groupFor(id)].insert(id);
    QSet<QString> displayedNodes;
    for (const auto &id : rawNodes) {
        displayedNodes.insert(displayId(id));
        if (isExpanded(id)) result.expandedGroups.insert(groupFor(id));
    }
    result.totalNodes = displayedNodes.size();
    for (auto i = result.containers.begin(); i != result.containers.end(); ++i) i.value()["member_count"] = members.value(i.key()).size();
    const auto displayedSymbol = [&](const QString &id) {
        return !isExpanded(id) && result.containers.contains(id) ? result.containers.value(id) : ensureSymbol(id);
    };
    if (!result.focusId.isEmpty()) result.symbols.insert(result.focusId, displayedSymbol(result.focusId));
    for (const auto &edge : grouped) {
        QSet<QString> additional;
        if (!result.symbols.contains(edge.source)) additional.insert(edge.source);
        if (!result.symbols.contains(edge.target)) additional.insert(edge.target);
        if (result.symbols.size() + additional.size() > maxNodes || result.edges.size() >= maxEdges) continue;
        result.symbols.insert(edge.source, displayedSymbol(edge.source)); result.symbols.insert(edge.target, displayedSymbol(edge.target));
        result.edges.append(edge); result.displayedSites += edge.sites.size();
    }
    // 展开时即使成员没有当前筛选类型的边，也保留真实声明，展示类内部结构。
    if (isExpanded(focus) && !focusClass.isEmpty()) {
        for (const auto &id : rawNodes) {
            if (result.symbols.size() >= maxNodes) break;
            if (id == focusClass || symbols.value(id).value("parent_id").toString() == focusClass)
                result.symbols.insert(id, ensureSymbol(id));
        }
    }
    if (focus.isEmpty()) {
        for (const auto &id : rawNodes) {
            if (result.symbols.size() >= maxNodes) break;
            const QString visibleId = displayId(id);
            result.symbols.insert(visibleId, displayedSymbol(visibleId));
        }
    }
    return result;
}

// A stable, selectable card; it does not grab the mouse from the graph.
class AnalysisPopup : public QWidget {
public:
    explicit AnalysisPopup(QWidget *parent) : QWidget(parent, Qt::Tool | Qt::FramelessWindowHint) {
        setObjectName("aiAnalysisPopup"); setAttribute(Qt::WA_ShowWithoutActivating);
        auto *layout = new QVBoxLayout(this); layout->setContentsMargins(10, 8, 10, 10);
        body = new QTextBrowser; body->setObjectName("aiAnalysisContent"); body->setOpenLinks(false); body->setOpenExternalLinks(false);
        body->setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard); layout->addWidget(body);
        qApp->installEventFilter(this);
    }
    void showContent(const QString &html, const QPoint &anchor) {
        body->setHtml(html);
        body->verticalScrollBar()->setValue(0);
        const auto *screen = QGuiApplication::screenAt(anchor);
        const QRect area = (screen ? screen : QGuiApplication::primaryScreen())->availableGeometry();
        const int popupWidth = qMin(500, area.width() - 20);
        body->document()->setTextWidth(popupWidth - 40);
        const int popupHeight = qMin(qMin(360, area.height() - 20), qMax(90, qCeil(body->document()->size().height()) + 30));
        resize(popupWidth, popupHeight);
        move(qBound(area.left(), anchor.x(), area.right() - width() + 1), qBound(area.top(), anchor.y(), area.bottom() - height() + 1));
        show(); raise();
    }
protected:
    bool eventFilter(QObject *object, QEvent *event) override {
        if (isVisible()) {
            if (event->type() == QEvent::MouseButtonPress) {
                auto *mouse = static_cast<QMouseEvent *>(event);
                if (!frameGeometry().contains(mouse->globalPosition().toPoint())) hide();
            } else if (event->type() == QEvent::ApplicationDeactivate) hide();
        }
        return QWidget::eventFilter(object, event);
    }
private:
    QTextBrowser *body;
};

RelationGraph::RelationGraph(QWidget *parent) : QWidget(parent) {
    analysisPopup = new AnalysisPopup(this);
    auto *layout = new QVBoxLayout(this); layout->setContentsMargins(0, 0, 0, 0);
    summary = new QLabel; summary->setObjectName("relationGraphSummary"); summary->setWordWrap(true); summary->setTextFormat(Qt::PlainText);
    layout->addWidget(summary);
    scene = new QGraphicsScene(this); scene->setObjectName("relationGraphScene");
    auto *graphView = new GraphView(scene); view = graphView; view->setObjectName("relationGraphView");
    graphView->changed = [this](qreal level) { zoomChanged(level); };
    view->setRenderHint(QPainter::Antialiasing); view->setDragMode(QGraphicsView::NoDrag);
    view->setTransformationAnchor(QGraphicsView::AnchorUnderMouse); view->setBackgroundBrush(palette().brush(QPalette::Window));
    view->setMinimumHeight(180);
    auto *splitter = new QSplitter(Qt::Vertical); splitter->setObjectName("graphBottomSplitter");
    splitter->addWidget(view); layout->addWidget(splitter, 1);
    panels = new QTabWidget; panels->setObjectName("graphBottomPanels");
    auto *information = new QWidget; auto *infoLayout = new QVBoxLayout(information); infoLayout->setContentsMargins(6, 4, 6, 4);
    panels->addTab(information, UiLanguage::text("Graph Info")); splitter->addWidget(panels);
    splitter->setStretchFactor(0, 3); splitter->setStretchFactor(1, 1); splitter->setSizes({560, 240});
    auto *legendCard = new QWidget(view);
    graphView->legendOverlay = legendCard;
    legendCard->setObjectName("graphLegendCard");
    legendCard->setAttribute(Qt::WA_TransparentForMouseEvents);
    legendCard->setStyleSheet("#graphLegendCard { background: #202020; border: 1px solid #454545; border-radius: 6px; }");
    auto *legendLayout = new QVBoxLayout(legendCard);
    legendLayout->setContentsMargins(10, 8, 10, 8);
    fileLegend = new QLabel; fileLegend->setObjectName("fileLanguageLegend");
    fileLegend->setTextFormat(Qt::RichText); fileLegend->setWordWrap(true);
    legendLayout->addWidget(fileLegend);
    QStringList edgeColors;
    for (const QString &type : {"inherits", "calls", "member_ref", "overrides", "references"})
        edgeColors.append(QStringLiteral("<span style=\"color:%1\">━ %2</span>").arg(edgeColor(type).name(), relationDisplayName(type)));
    auto *legend = new QLabel(UiLanguage::text("Edges<br>") + edgeColors.join(QStringLiteral("<br>")) + UiLanguage::text("<br>Dashed: possible call<br>Referenced → Referrer"));
    legendLayout->addWidget(legend);
    graphView->positionLegend();
    details = new QLabel;
    details->setObjectName("relationGraphDetails"); details->setWordWrap(true); details->setTextFormat(Qt::PlainText); infoLayout->addWidget(details);
    sites = new QComboBox; sites->setObjectName("relationGraphSites"); sites->setEnabled(false); sites->hide(); infoLayout->addWidget(sites); infoLayout->addStretch();
    sites->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon); sites->setMinimumContentsLength(20);
    connect(scene, &QGraphicsScene::selectionChanged, this, &RelationGraph::inspectSelection);
    view->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(view, &QGraphicsView::customContextMenuRequested, this, &RelationGraph::showContextMenu);
    view->viewport()->installEventFilter(this);
    connect(sites, &QComboBox::currentIndexChanged, this, [this](int index) {
        if (index < 0) return;
        const auto site = sites->itemData(index).toJsonObject();
        showSite(site);
    });
    setData({}, {}, {}, {});
}

bool RelationGraph::eventFilter(QObject *object, QEvent *event) {
    if (object == view->viewport()) {
        if (event->type() == QEvent::ToolTip && analysisPopup->isVisible()) {
            QToolTip::hideText();
            event->accept();
            return true;
        }
        if (event->type() == QEvent::MouseButtonPress) {
            auto *mouse = static_cast<QMouseEvent *>(event);
            analysisPressed = mouse->button() == Qt::LeftButton; analysisDoubleClick = false; analysisPress = mouse->position().toPoint();
            hideFileAnalysis();
        } else if (event->type() == QEvent::MouseButtonDblClick) {
            analysisDoubleClick = true; hideFileAnalysis();
        } else if (event->type() == QEvent::MouseMove && analysisPressed) {
            auto *mouse = static_cast<QMouseEvent *>(event);
            if ((mouse->position().toPoint() - analysisPress).manhattanLength() >= QApplication::startDragDistance()) analysisPressed = false;
        } else if (event->type() == QEvent::MouseButtonRelease) {
            auto *mouse = static_cast<QMouseEvent *>(event);
            const QPoint point = mouse->position().toPoint();
            const bool click = analysisPressed && !analysisDoubleClick && mouse->button() == Qt::LeftButton
                && (point - analysisPress).manhattanLength() < QApplication::startDragDistance();
            analysisPressed = false;
            if (click) showFileAnalysis(point);
        } else if (event->type() == QEvent::Wheel || event->type() == QEvent::Resize) { analysisPressed = false; hideFileAnalysis(); }
    }
    return QWidget::eventFilter(object, event);
}

void RelationGraph::hideFileAnalysis() { analysisPopup->hide(); }

void RelationGraph::showFileAnalysis(const QPoint &point) {
    QToolTip::hideText();
    if (fileExplanation) {
        auto *item = view->itemAt(point);
        while (item && item->data(0) != "symbol" && item->data(0) != "container") item = item->parentItem();
        if (item) {
            const auto symbol = item->data(1).toJsonObject();
            if (!symbol.value("is_external").toBool() && !symbol.value("unresolved").toBool()) {
                QString path = symbol.value("scope_file").toString();
                if (path.isEmpty()) path = symbol.value("def_file").toString();
                if (path.isEmpty()) path = symbol.value("decl_file").toString();
                const QString explanation = path.isEmpty() ? QString() : fileExplanation(path);
                if (!explanation.isEmpty()) {
                    analysisPopup->showContent(explanation,
                        view->viewport()->mapToGlobal(point + QPoint(18, 18)));
                    return;
                }
            }
        }
    }
    hideFileAnalysis();
}

void RelationGraph::setData(const QJsonObject &unit, const QString &focus, const QString &type, const QString &scope) {
    const bool changed = currentUnit.value("config_id") != unit.value("config_id") || currentUnit.value("file") != unit.value("file");
    const bool firstResults = currentUnit.value("symbols").toArray().isEmpty();
    const bool focusChanged = currentFocus != focus;
    const bool newResults = currentUnit != unit;
    if (changed) {
        zoomLevel = 0.65; groupOverrides.clear(); viewAdjusted = false;
    }
    const bool fit = changed || (!viewAdjusted && (firstResults || (newResults && focus.isEmpty())));
    if (focusChanged && !focus.isEmpty()) viewAdjusted = true;
    currentUnit = unit; currentFocus = focus; currentType = type; currentScope = scope;
    renderMap(fit, focusChanged);
}

void RelationGraph::setZoomLevel(qreal level) {
    hideFileAnalysis();
    level = qBound(0.05, level, 3.0);
    view->resetTransform(); view->scale(level, level); zoomChanged(level);
}

void RelationGraph::zoomChanged(qreal level) {
    viewAdjusted = true;
    zoomLevel = level;
}

void RelationGraph::fitGraph() {
    if (scene->items().isEmpty()) return;
    // 只适配当前结构，保留各文件/类的详情或缩略选择。
    viewAdjusted = true; zoomLevel = 0.65; renderMap(true);
}

bool RelationGraph::focusFile(const QString &path, const QString &root) {
    const auto normalized = [&root](const QString &file) {
        return file.isEmpty() ? QString() : QDir::cleanPath(QDir::fromNativeSeparators(QDir(root).absoluteFilePath(file))).toCaseFolded();
    };
    const QString target = normalized(path);
    const auto matches = [&](const QJsonObject &symbol) {
        if (symbol.value("is_external").toBool()) return false;
        for (const QString field : {"scope_file", "def_file", "decl_file"})
            if (normalized(symbol.value(field).toString()) == target) return true;
        return false;
    };
    const auto locate = [&]() {
        QList<QGraphicsItem *> found;
        for (auto *item : scene->items())
            if ((item->data(0) == "symbol" || item->data(0) == "container") && matches(item->data(1).toJsonObject())) found.append(item);
        if (found.isEmpty()) return false;
        hideFileAnalysis(); viewAdjusted = true;
        { const QSignalBlocker blocker(scene);
            scene->clearSelection();
            for (auto *item : found) {
                if (item->flags().testFlag(QGraphicsItem::ItemIsSelectable)) item->setSelected(true);
            }
        }
        inspectSelection();
        return true;
    };
    return locate();
}

void RelationGraph::simplifyAll() {
    viewAdjusted = true;
    // 无焦点、无筛选收集全部已加载分组，包含当前画布显示上限之外的分组。
    const auto overview = buildRelationMap(currentUnit, {}, {}, false);
    for (auto it = groupOverrides.begin(); it != groupOverrides.end(); ++it) it.value() = false;
    for (const auto &group : overview.groups) groupOverrides[group] = false;
    renderMap();
}

void RelationGraph::renderMap(bool fit, bool centerFocus) {
    hideFileAnalysis(); analysisPressed = false;
    const QPointF previousCenter = view->mapToScene(view->viewport()->rect().center());
    const QSignalBlocker blocker(scene), siteBlocker(sites);
    scene->clear(); sites->clear(); sites->setEnabled(false); sites->hide();
    details->setText(UiLanguage::text("Select a node or edge"));
    QMap<QString,QJsonObject> languageUnits;
    QString focusLanguage;
    for (const auto &value : currentUnit.value("symbols").toArray()) {
        const auto symbol = value.toObject(); const QString language = symbol.value("language").toString();
        auto unit = languageUnits.value(language); auto values = unit.value("symbols").toArray(); values.append(symbol); unit["symbols"] = values; languageUnits[language] = unit;
        if (symbol.value("id").toString() == currentFocus) focusLanguage = language;
    }
    for (const auto &value : currentUnit.value("relations").toArray()) {
        const auto relation = value.toObject(); const QString language = relation.value("language").toString();
        auto unit = languageUnits.value(language); auto values = unit.value("relations").toArray(); values.append(relation); unit["relations"] = values; languageUnits[language] = unit;
    }
    RelationNeighborhood neighborhood;
    for (auto it = languageUnits.begin(); it != languageUnits.end(); ++it) {
        const auto part = buildRelationMap(it.value(), it.key() == focusLanguage ? currentFocus : QString(), currentType, false, 81, 160, groupOverrides);
        neighborhood.symbols.insert(part.symbols); neighborhood.groups.insert(part.groups); neighborhood.containers.insert(part.containers); neighborhood.edges.append(part.edges);
        neighborhood.expandedGroups.unite(part.expandedGroups);
        neighborhood.totalSites += part.totalSites; neighborhood.displayedSites += part.displayedSites; neighborhood.hiddenInternalSites += part.hiddenInternalSites; neighborhood.totalNodes += part.totalNodes;
        if (!part.focusId.isEmpty()) neighborhood.focusId = part.focusId;
    }
    summary->setText(UiLanguage::text("%1 nodes - %2 edges")
        .arg(neighborhood.symbols.size()).arg(neighborhood.edges.size()));
    QSet<QString> visibleLanguages;
    const auto collectLanguage = [&visibleLanguages](const QJsonObject &symbol) {
        // Classes and enums also carry source language, even without a file node.
        const auto style = sourceLanguageStyle(symbol);
        if (style.color.isValid()) visibleLanguages.insert(style.name);
    };
    for (auto it = neighborhood.symbols.begin(); it != neighborhood.symbols.end(); ++it) {
        collectLanguage(it.value());
        const auto group = neighborhood.groups.value(it.key(), it.key());
        if (neighborhood.expandedGroups.contains(group)) collectLanguage(neighborhood.containers.value(group));
    }
    QStringList fileColors;
    for (const auto &style : fileLanguagePalette())
        if (visibleLanguages.contains(style.name))
            fileColors.append(QStringLiteral("<span style=\"color:%1\">■ %2</span>").arg(style.color.name(), style.name));
    fileLegend->setText(UiLanguage::text("Files<br>") + (fileColors.isEmpty()
        ? UiLanguage::text("No recognized source language in this view") : fileColors.join(QStringLiteral("<br>"))));
    static_cast<GraphView *>(view)->positionLegend();
    if (neighborhood.symbols.isEmpty()) return;
    const auto focus = neighborhood.focusId;
    QMap<QString, SymbolItem *> nodes;
    QMap<QString, QList<SymbolItem *>> groupNodes;
    QMap<QString, QGraphicsRectItem *> frames;
    QMap<QString, QGraphicsSimpleTextItem *> headings;
    QMap<QString, QStringList> membersByGroup;
    QMap<QString, QMap<QString, QSizeF>> sizesByLanguage;
    for (auto it = neighborhood.symbols.begin(); it != neighborhood.symbols.end(); ++it)
        membersByGroup[neighborhood.groups.value(it.key(), it.key())].append(it.key());
    for (auto it = membersByGroup.begin(); it != membersByGroup.end(); ++it) {
        const auto language = neighborhood.symbols.value(it.value().first()).value("language").toString();
        const bool framed = neighborhood.expandedGroups.contains(it.key()) && neighborhood.containers.contains(it.key());
        sizesByLanguage[language][it.key()] = framed ? QSizeF(565, 70 + std::ceil(it.value().size() / 2.0) * 120) : QSizeF(260, 92);
    }
    QList<QPair<QString, QString>> links;
    for (const auto &edge : neighborhood.edges)
        links.append({neighborhood.groups.value(edge.target, edge.target), neighborhood.groups.value(edge.source, edge.source)});
    QMap<QString, QPointF> groupPositions;
    QMap<QString, QRectF> languageBounds;
    QSet<QString> cyclicGroups;
    qreal languageX = 0;
    for (auto it = sizesByLanguage.begin(); it != sizesByLanguage.end(); ++it) {
        const auto layout = layoutRelationGroups(it.value(), links);
        for (auto p = layout.positions.begin(); p != layout.positions.end(); ++p) {
            groupPositions[p.key()] = p.value() + QPointF(languageX, 60);
        }
        cyclicGroups.unite(layout.cyclicGroups);
        languageBounds[it.key()] = QRectF(QPointF(languageX, 60), layout.size);
        languageX += layout.size.width() + 180;
    }
    for (auto i = neighborhood.symbols.begin(); i != neighborhood.symbols.end(); ++i) {
        auto *node = new SymbolItem(i.value(), i.key() == focus, cyclicGroups.contains(neighborhood.groups.value(i.key(), i.key()))); scene->addItem(node);
        node->setData(2, neighborhood.groups.value(i.key(), i.key()));
        const auto id = i.key(); const bool container = i.value().value("is_container").toBool();
        const bool abstract = i.value().value("abstract").toBool();
        node->focused = [this, id, container, abstract] { if (currentUnit.value("symbol_files").toBool()) return; QTimer::singleShot(0, this, [this, id, container, abstract] {
            if (!container) { emit focusRequested(id); return; }
            viewAdjusted = true; groupOverrides[id] = true; renderMap();
            if (!abstract) emit focusRequested(id);
            for (auto *item : scene->items())
                if (item->data(0).toString() == "container" && item->data(1).toJsonObject().value("id").toString() == id) { view->centerOn(item); break; }
        }); };
        nodes.insert(id, node);
        groupNodes[neighborhood.groups.value(id, id)].append(node);
    }
    QMap<QString, QList<SymbolItem *>> languageNodes;
    for (auto *node : nodes) languageNodes[node->data(1).toJsonObject().value("language").toString()].append(node);
    const bool partitioned = languageNodes.size() > 1 || !languageNodes.firstKey().isEmpty();
    for (auto it = groupNodes.begin(); it != groupNodes.end(); ++it) {
        const auto position = groupPositions.value(it.key());
        const bool framed = neighborhood.expandedGroups.contains(it.key()) && neighborhood.containers.contains(it.key());
        int index = 0;
        for (auto *node : it.value()) {
            node->setPos(position + (framed ? QPointF(20 + (index % 2) * 285, 60 + (index / 2) * 120) : QPointF()));
            ++index;
        }
        if (!framed) continue;
        const bool focal = it.key() == neighborhood.groups.value(focus, focus);
        const auto language = it.value().first()->data(1).toJsonObject().value("language").toString();
        auto *frame = scene->addRect(QRectF(position, sizesByLanguage.value(language).value(it.key())),
            QPen(QColor(focal ? "#527ca6" : "#484848"), 1.5), QBrush(QColor("#151b21")));
        const auto fileStyle = fileLanguageStyle(neighborhood.containers.value(it.key()));
        if (fileStyle.color.isValid()) {
            frame->setPen(QPen(fileStyle.color, focal ? 2.5 : 1.5)); frame->setBrush(fileBackground(fileStyle.color, focal));
        }
        frame->setZValue(-3); frame->setAcceptedMouseButtons(Qt::NoButton);
        frame->setData(0, "container"); frame->setData(1, neighborhood.containers.value(it.key()));
        const QString title = symbolName(neighborhood.containers.value(it.key())) +
            (cyclicGroups.contains(it.key()) ? UiLanguage::text(" - dependency cycle") : UiLanguage::text(" - internal and related symbols"));
        auto *heading = new QGraphicsSimpleTextItem(QFontMetrics(QApplication::font()).elidedText(title, Qt::ElideMiddle, 525), frame);
        heading->setFont(QApplication::font()); heading->setBrush(QColor("#bbbbbb")); heading->setPos(position + QPointF(20, 15));
        if (fileStyle.color.isValid()) heading->setBrush(fileStyle.color);
        heading->setAcceptedMouseButtons(Qt::NoButton); frame->setToolTip(title);
        frames.insert(it.key(), frame); headings.insert(it.key(), heading);
    }
    // 各语言共享同一个 scene；分区只表达语言归属，不添加跨语言关系。
    if (partitioned) {
        for (auto it = languageNodes.begin(); it != languageNodes.end(); ++it) {
            QRectF bounds = languageBounds.value(it.key());
            for (auto *node : it.value()) bounds = bounds.united(node->sceneBoundingRect());
            auto *partition = scene->addRect(bounds.adjusted(-30, -55, 30, 30), QPen(QColor("#404a56")), QBrush(QColor("#11141a")));
            partition->setZValue(-6); partition->setAcceptedMouseButtons(Qt::NoButton); partition->setData(0, "languagePartition"); partition->setData(1, it.key());
            auto *label = new QGraphicsSimpleTextItem(it.key() == "java" ? "Java" : it.key() == "javascript" ? "JavaScript/TypeScript" : it.key() == "python" ? "Python" : it.key() == "c#" ? "C#" : "C++", partition);
            label->setBrush(QColor("#9ac4ff")); label->setFont(QApplication::font()); label->setPos(bounds.topLeft() + QPointF(0, -40)); label->setAcceptedMouseButtons(Qt::NoButton);
        }
    }
    QList<EdgeItem *> edges;
    QMap<QString, int> counts, indices;
    for (const auto &edge : neighborhood.edges) ++counts[key({edge.source, edge.target})];
    for (const auto &edge : neighborhood.edges) {
        const auto pair = key({edge.source, edge.target});
        qreal bend = (indices[pair]++ - (counts[pair] - 1) / 2.0) * 60;
        if (edge.source != edge.target && counts.contains(key({edge.target, edge.source}))) bend += edge.source < edge.target ? 45 : -45;
        // 分析数据保留 source=引用者、target=被引用者；只反转画布上的箭头。
        auto *item = new EdgeItem(nodes.value(edge.target), nodes.value(edge.source), edge, bend);
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
    view->resetTransform(); view->scale(zoomLevel, zoomLevel);
    if (fit) {
        view->fitInView(scene->itemsBoundingRect().adjusted(-30, -30, 30, 30), Qt::KeepAspectRatio);
        zoomLevel = qBound(0.05, qMin(zoomLevel, view->transform().m11()), 3.0);
        view->resetTransform(); view->scale(zoomLevel, zoomLevel);
        view->centerOn(scene->sceneRect().center());
    } else if (centerFocus && nodes.contains(focus)) view->centerOn(nodes.value(focus));
    else view->centerOn(previousCenter);
}

void RelationGraph::inspectSelection() {
    { const QSignalBlocker blocker(sites); sites->clear(); sites->setEnabled(false); sites->hide(); }
    const auto selected = scene->selectedItems();
    QSet<QString> selectedNodes, litNodes, litGroups;
    QSet<QGraphicsItem *> litEdges;
    for (auto *item : selected)
        if (item->data(0) == "symbol") selectedNodes.insert(item->data(1).toJsonObject().value("id").toString());
    litNodes = selectedNodes;
    for (auto *item : scene->items()) if (item->data(0) == "relation") {
        const auto a = item->data(2).toString(), b = item->data(3).toString();
        if (item->isSelected() || selectedNodes.contains(a) || selectedNodes.contains(b)) {
            litEdges.insert(item); litNodes.insert(a); litNodes.insert(b);
        }
    }
    for (auto *item : scene->items()) if (item->data(0) == "symbol") {
        const bool lit = selected.isEmpty() || litNodes.contains(item->data(1).toJsonObject().value("id").toString());
        item->setOpacity(lit ? 1 : 0.22);
        if (lit) litGroups.insert(item->data(2).toString());
    }
    for (auto *item : scene->items()) {
        if (item->data(0) == "relation") {
            const bool lit = selected.isEmpty() || litEdges.contains(item);
            item->setOpacity(lit ? 1 : 0.12); item->setZValue(lit && !selected.isEmpty() ? -0.5 : -1);
        } else if (item->data(0) == "container") item->setOpacity(selected.isEmpty() || litGroups.contains(item->data(1).toJsonObject().value("id").toString()) ? 1 : 0.25);
    }
    if (selected.isEmpty()) {
        details->setText(UiLanguage::text("Select a node or edge"));
        return;
    }
    auto *item = selected.first();
    if (item->data(0).toString() == "symbol") {
        const auto s = item->data(1).toJsonObject();
        details->setText(symbolName(s) + " · " + symbolPosition(s));
        if (s.value("abstract").toBool()) {
            details->setText(symbolName(s) + UiLanguage::text(" - scope grouped by source location"));
            return;
        }
        if (s.value("is_external").toBool() || s.value("unresolved").toBool()) {
            details->setText(details->text() + UiLanguage::text(" - external or unresolved symbol; project source cannot be located.")); return;
        }
    } else if (item->data(0).toString() == "relation") {
        const auto positions = item->data(1).toJsonArray();
        if (positions.isEmpty()) return;
        const auto first = positions.first().toObject();
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
    QString a = UiLanguage::text("Unresolved symbol"), b = UiLanguage::text("Unresolved symbol");
    for (const auto &value : currentUnit.value("symbols").toArray()) {
        const auto s = value.toObject();
        if (s.value("id") == site.value("source_id")) a = symbolName(s);
        if (s.value("id") == site.value("target_id")) b = symbolName(s);
    }
    const QString explanation = b + " → " + a + UiLanguage::text(" - Referenced → Referrer - ") + relationDisplayName(site.value("type").toString()) +
        UiLanguage::text(" - %1 locations available").arg(sites->count());
    details->setToolTip(explanation);
    details->setText(QFontMetrics(details->font()).elidedText(explanation, Qt::ElideMiddle, qMax(300, view->viewport()->width() - 12)));
}

void RelationGraph::showContextMenu(const QPoint &point) {
    auto *menu = new QMenu(this); menu->setObjectName("graphContextMenu"); menu->setAttribute(Qt::WA_DeleteOnClose);
    auto *item = view->itemAt(point);
    while (item && item->data(0).toString().isEmpty()) item = item->parentItem();
    QJsonArray positions;
    if (item && (item->data(0) == "symbol" || item->data(0) == "container")) {
        const auto symbol = item->data(1).toJsonObject();
        const QString group = item->data(0) == "container" ? symbol.value("id").toString() : item->data(2).toString();
        if (!group.isEmpty() && symbol.value("kind") != "RelatedFile") {
            auto *display = menu->addMenu(UiLanguage::text("Display"));
            auto *choices = new QActionGroup(display); choices->setExclusive(true);
            const QStringList labels{UiLanguage::text("Show Details"), UiLanguage::text("Show Summary")};
            const QStringList names{"graphShowDetailsAction", "graphShowThumbnailAction"};
            for (int index = 0; index < labels.size(); ++index) {
                auto *action = display->addAction(labels[index]); action->setObjectName(names[index]);
                choices->addAction(action); action->setCheckable(true);
                action->setChecked(groupOverrides.value(group, false) == (index == 0));
                connect(action, &QAction::triggered, this, [this, group, index] {
                    viewAdjusted = true;
                    groupOverrides[group] = index == 0;
                    renderMap();
                });
            }
        }
        if (!symbol.value("is_external").toBool() && !symbol.value("unresolved").toBool()) {
            const QString prefix = symbol.value("def_file").toString().isEmpty() ? "decl_" : "def_";
            QString file = symbol.value(prefix + "file").toString(); int line = symbol.value(prefix + "line").toInt(1);
            if (file.isEmpty()) { file = symbol.value("scope_file").toString(); line = 1; }
            if (!file.isEmpty()) positions.append(QJsonObject{{"file", file}, {"line", line}, {"col", symbol.value(prefix + "col").toInt(1)}});
        }
        if (symbol.value("kind") == "RelatedFile") positions = symbol.value("occurrence_sites").toArray();
        else {
            auto *focus = menu->addAction(UiLanguage::text("Show Related Relations"));
            connect(focus, &QAction::triggered, this, [this, symbol] { emit focusRequested(symbol.value("id").toString()); });
        }
    } else if (item && item->data(0) == "relation") positions = item->data(1).toJsonArray();
    if (!positions.isEmpty()) {
        QMenu *sourceMenu = positions.size() > 1 ? menu->addMenu(UiLanguage::text("View Source")) : menu;
        for (const auto &value : positions) {
            const auto site = value.toObject();
            const QString at = site.value("file").toString() + ':' + QString::number(site.value("line").toInt()) + ':' + QString::number(site.value("col").toInt(1));
            auto *source = sourceMenu->addAction(positions.size() == 1 ? UiLanguage::text("View Source") : at + (site.value("role").toString().isEmpty() ? QString() : " · " + site.value("role").toString()));
            source->setObjectName("graphViewSourceAction");
            source->setData(site);
            connect(source, &QAction::triggered, this, [this, site] { emit sourceRequested(site.value("file").toString(), site.value("line").toInt(1), site.value("col").toInt(1)); });
        }
    }
    menu->addSeparator(); auto *all = menu->addAction(UiLanguage::text("Show All Relations")); connect(all, &QAction::triggered, this, [this] { emit focusRequested({}); });
    auto *simplify = menu->addAction(UiLanguage::text("Simplify All")); simplify->setObjectName("graphSimplifyAllAction");
    connect(simplify, &QAction::triggered, this, &RelationGraph::simplifyAll);
    auto *fit = menu->addAction(UiLanguage::text("Fit to Canvas")); connect(fit, &QAction::triggered, this, &RelationGraph::fitGraph);
    auto *refresh = menu->addAction(UiLanguage::text("Refresh Files and Index")); refresh->setObjectName("graphRefreshAction"); connect(refresh, &QAction::triggered, this, &RelationGraph::refreshRequested);
    emit contextMenuRequested(menu); menu->popup(view->viewport()->mapToGlobal(point));
}
