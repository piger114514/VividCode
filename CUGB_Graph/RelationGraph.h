#pragma once
#include <QWidget>
#include <QJsonObject>
#include <QJsonArray>
#include <QMap>
#include <QList>

class QGraphicsScene;
class QGraphicsView;
class QLabel;
class QComboBox;

struct RelationEdge {
    QString source, target, type, confidence;
    QJsonArray sites;
    bool aggregated = false;
};

struct RelationNeighborhood {
    QMap<QString, QJsonObject> symbols;
    QList<RelationEdge> edges;
    int totalSites = 0;
    int displayedSites = 0;
    int hiddenInternalSites = 0;
    int totalNodes = 0;
    QString focusId;
    QMap<QString, QString> groups;
    QMap<QString, QJsonObject> containers;
};

// 同一 TU/配置的成员层邻域；类焦点覆盖类成员，同类连线合并，证据保留。
RelationNeighborhood buildRelationNeighborhood(const QJsonObject &unit, const QString &focus,
    const QString &type = {}, int maxNodes = 81, int maxEdges = 160);
QString relationDisplayName(const QString &type);
RelationNeighborhood buildRelationMap(const QJsonObject &unit, const QString &focus,
    const QString &type, bool expanded, int maxNodes = 81, int maxEdges = 160);

class RelationGraph : public QWidget {
    Q_OBJECT
public:
    explicit RelationGraph(QWidget *parent = nullptr);
    void setData(const QJsonObject &unit, const QString &focus, const QString &type, const QString &scope);
    void zoomBy(qreal factor);
    void setZoomLevel(qreal level);
    void fitGraph();
signals:
    void sourceRequested(const QString &path, int line, int column);
    void focusRequested(const QString &id);
private:
    void inspectSelection();
    void renderMap();
    void zoomChanged(qreal level);
    void showSite(const QJsonObject &site);
    QGraphicsScene *scene;
    QGraphicsView *view;
    QLabel *summary, *details;
    QComboBox *sites;
    QJsonObject currentUnit;
    QString currentFocus, currentType, currentScope;
    bool expanded = false;
    qreal zoomLevel = 0.65;
};
