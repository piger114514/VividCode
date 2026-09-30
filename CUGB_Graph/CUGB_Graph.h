#pragma once

#include <QtWidgets/QMainWindow>
#include "ui_CUGB_Graph.h"
#include <QStringList>

class QTreeWidget;
class QPlainTextEdit;
class QLineEdit;
class QLabel;
class SemanticPanel;

class CUGB_Graph : public QMainWindow
{
    Q_OBJECT

public:
    CUGB_Graph(QWidget *parent = nullptr);
    ~CUGB_Graph();
    bool openProject(const QString &directory);

private:
    Ui::CUGB_GraphClass ui;
    void rebuildViews();
    void showFile(const QString &relativePath);
    QString rootPath;
    QString currentFile;
    QStringList files;
    QTreeWidget *tree;
    QPlainTextEdit *code;
    QLineEdit *filter;
    QLabel *projectLabel;
    QLabel *fileLabel;
    SemanticPanel *semantic;
};

