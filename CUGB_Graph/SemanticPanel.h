#pragma once
#include <QWidget>
#include <QJsonObject>
#include <QProcess>
class QTreeWidget;
class QComboBox;
class QLabel;
class QPushButton;
class QPlainTextEdit;
class RelationGraph;
class QTabWidget;

class SemanticPanel : public QWidget {
    Q_OBJECT
public:
    explicit SemanticPanel(QWidget *parent = nullptr);
    ~SemanticPanel() override;
    void setProject(const QString &root);
    void importDatabase(const QString &path);
    bool isBusy() const;
signals:
    void sourceRequested(const QString &path, int line, int column);
    void settled();
private:
    void launch(const QString &command, const QString &database = {});
    void loadCache(bool refresh);
    void refreshIndex();
    void stop();
    void render();
    void renderUnit();
    void renderRelations();
    void focusSymbol(const QString &id);
    QProcess process;
    QString root, command, selectedSymbol, compilation;
    QByteArray output;
    bool refreshAfterLoad = false, stopping = false, cancelled = false;
    QJsonObject snapshot;
    QComboBox *units, *relationType, *focusSymbols;
    QTreeWidget *symbols, *relations;
    QLabel *status;
    QPlainTextEdit *diagnostics;
    QPushButton *update, *cancel, *import;
    RelationGraph *graph;
    QTabWidget *tabs;
};
