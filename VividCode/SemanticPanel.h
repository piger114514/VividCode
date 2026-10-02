#pragma once
#include <QWidget>
#include <QJsonObject>
#include <QProcess>
#include <QStringList>
#include "ProjectDetection.h"
class QTreeWidget;
class QComboBox;
class QLabel;
class QPushButton;
class QPlainTextEdit;
class RelationGraph;
class QTabWidget;
class QLineEdit;
class QMenu;

class SemanticPanel : public QWidget {
    Q_OBJECT
public:
    explicit SemanticPanel(QWidget *parent = nullptr);
    ~SemanticPanel() override;
    void setProject(const QString &root, const QStringList &sources = {});
    void importDatabase(const QString &path);
    void importCSharpProject(const QString &path, const QString &configuration = "Debug", const QString &framework = {});
    bool isBusy() const;
    QWidget *analysisControls() const { return controls; }
    QWidget *structureView() const { return structure; }
    void addContextActions(QMenu *menu);
    void focusSymbol(const QString &id);
    QString focusedSymbol() const { return selectedSymbol; }
    QStringList analysisLanguages() const { return selectedEntries.keys(); }
signals:
    void sourceRequested(const QString &path, int line, int column);
    void settled();
    void refreshRequested();
private:
    void launch(const QString &command, const QString &database = {});
    void loadCache(bool refresh);
    void refreshIndex();
    void stop();
    void render();
    void renderUnit();
    void renderRelations();
    bool isCSharp() const;
    void updateControls(bool busy);
    AnalysisEntry selectEntry(const QList<AnalysisEntry> &entries, const QString &language);
    void beginNextLanguage();
    void chooseConfiguration();
    QJsonObject combinedUnit() const;
    QProcess process;
    QString root, command, selectedSymbol, compilation, workerLanguage;
    QStringList knownSources, pendingLanguages;
    QMap<QString,AnalysisEntry> selectedEntries;
    QMap<QString,QJsonObject> languageSnapshots;
    QByteArray output;
    bool refreshAfterLoad = false, stopping = false, cancelled = false;
    bool working = false;
    QJsonObject snapshot;
    QComboBox *units, *relationType, *buildConfiguration;
    QLineEdit *targetFramework;
    QTreeWidget *symbols, *relations;
    QLabel *status;
    QPlainTextEdit *diagnostics;
    RelationGraph *graph;
    QWidget *controls, *structure;
};
