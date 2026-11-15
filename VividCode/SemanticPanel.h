#pragma once
#include <QWidget>
#include <QJsonObject>
#include <QJsonArray>
#include <QProcess>
#include <QStringList>
#include <QMap>
#include "ProjectDetection.h"
class QTreeWidget;
class QComboBox;
class QLabel;
class QPlainTextEdit;
class RelationGraph;
class QLineEdit;
class QMenu;

class SemanticPanel : public QWidget {
    Q_OBJECT
public:
    explicit SemanticPanel(QWidget *parent = nullptr);
    ~SemanticPanel() override;
    void setProject(const QString &root, const QStringList &sources = {}, bool scanComplete = true);
    void importDatabase(const QString &path);
    void importCSharpProject(const QString &path, const QString &configuration = "Debug", const QString &framework = {});
    bool isBusy() const;
    QWidget *analysisControls() const { return controls; }
    QWidget *structureView() const { return structure; }
    void addContextActions(QMenu *menu);
    void focusSymbol(const QString &id);
    bool focusFile(const QString &path);
    void showSymbolFiles(const QString &id);
    QString focusedSymbol() const { return selectedSymbol; }
    QStringList analysisLanguages() const { return selectedEntries.keys(); }
    QMap<QString, QStringList> fileDependencies() const;
    QJsonArray sourceHighlights(const QString &path) const;
signals:
    void sourceRequested(const QString &path, int line, int column);
    void settled();
    void refreshRequested();
private:
    void setStatus(const QString &text);
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
    void chooseEnvironment();
    QJsonObject combinedUnit() const;
    QProcess process;
    QString root, command, selectedSymbol, compilation, workerLanguage;
    QString pendingFileFocus;
    QStringList knownSources, pendingLanguages;
    QMap<QString,AnalysisEntry> selectedEntries;
    QMap<QString,QJsonObject> languageSnapshots;
    QByteArray output;
    bool refreshAfterLoad = false, stopping = false, cancelled = false;
    bool working = false, symbolFilesMode = false;
    QJsonObject snapshot;
    QComboBox *units, *relationType, *buildConfiguration;
    QLineEdit *targetFramework;
    QTreeWidget *symbols, *relations;
    QLabel *status;
    QPlainTextEdit *diagnostics;
    RelationGraph *graph;
    QWidget *controls, *structure;
};
