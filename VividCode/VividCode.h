#pragma once

#include <QtWidgets/QMainWindow>
#include "ui_VividCode.h"
#include <QStringList>

class QTreeWidget;
class QPlainTextEdit;
class QLineEdit;
class QLabel;
class SemanticPanel;

class VividCodeWindow : public QMainWindow
{
    Q_OBJECT

public:
    VividCodeWindow(QWidget *parent = nullptr);
    ~VividCodeWindow();
    bool openProject(const QString &directory);

private:
    Ui::VividCodeWindow ui;
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

