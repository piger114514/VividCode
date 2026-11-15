#pragma once
#include <QWidget>
#include <QJsonObject>
#include <QJsonArray>
#include <QMap>
#include <QList>
#include <QSet>
#include <functional>

class QGraphicsScene;
class QGraphicsView;
class QLabel;
class QComboBox;
class QMenu;
class AnalysisPopup;
class QTabWidget;

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
    QSet<QString> expandedGroups;
};

QString relationDisplayName(const QString &type);
QJsonObject buildSymbolFileUnit(const QJsonObject &unit, const QString &symbolId, const QString &root, const QString &type = {});
// 类/文件概览与成员层共用模型；汇总连线保留真实端点和源码证据。
RelationNeighborhood buildRelationMap(const QJsonObject &unit, const QString &focus,
    const QString &type, bool expanded, int maxNodes = 81, int maxEdges = 160,
    const QMap<QString, bool> &groupOverrides = {});

class RelationGraph : public QWidget {
    Q_OBJECT
public:
    explicit RelationGraph(QWidget *parent = nullptr);
    void setData(const QJsonObject &unit, const QString &focus, const QString &type, const QString &scope);
    void setZoomLevel(qreal level);
    void fitGraph();
    bool focusFile(const QString &path, const QString &root);
    void showContextMenu(const QPoint &point);
    void setFileExplanationProvider(std::function<QString(const QString &)> provider) { fileExplanation = std::move(provider); }
    QTabWidget *bottomPanels() const { return panels; }
signals:
    void sourceRequested(const QString &path, int line, int column);
    void focusRequested(const QString &id);
    void refreshRequested();
    void contextMenuRequested(QMenu *menu);
private:
    bool eventFilter(QObject *object, QEvent *event) override;
    void showFileAnalysis(const QPoint &point);
    void hideFileAnalysis();
    AnalysisPopup *analysisPopup;
    QPoint analysisPress;
    bool analysisPressed = false, analysisDoubleClick = false;
    std::function<QString(const QString &)> fileExplanation;
    void simplifyAll();
    void inspectSelection();
    void renderMap(bool fit = false, bool centerFocus = false);
    void zoomChanged(qreal level);
    void showSite(const QJsonObject &site);
    QGraphicsScene *scene;
    QGraphicsView *view;
    QLabel *summary, *details, *fileLegend;
    QComboBox *sites;
    QTabWidget *panels;
    QJsonObject currentUnit;
    QString currentFocus, currentType, currentScope;
    bool viewAdjusted = false;
    QMap<QString, bool> groupOverrides;
    qreal zoomLevel = 0.65;
};
