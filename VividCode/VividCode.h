#pragma once

#include <QtWidgets/QMainWindow>
#include <QStringList>

class QTreeWidget;
class QLineEdit;
class QLabel;
class SemanticPanel;
class SourcePopup;
namespace VividCode { class AiExplanation; class PiAgentPanel; }

class VividCodeWindow : public QMainWindow
{
    Q_OBJECT

public:
    VividCodeWindow(QWidget *parent = nullptr);
    ~VividCodeWindow();
    bool openProject(const QString &directory);

private:
    void rebuildViews();
    void showFile(const QString &relativePath, int line = 1, int column = 1);
    void refreshAfterPi();
    QString rootPath;
    QString currentFile;
    QStringList files;
    QTreeWidget *tree;
    SourcePopup *sourcePopup;
    QLineEdit *filter;
    QLabel *projectLabel;
    SemanticPanel *semantic;
    VividCode::AiExplanation *ai;
    VividCode::PiAgentPanel *pi;
    QStringList piChangedFiles;
    QString piRefreshRoot;
    QMap<QString, QStringList> piPreviousDependencies;
    bool piRefreshPending = false, piAwaitingGraph = false, piAwaitingAi = false;
    bool completeFileScan = false;
};

