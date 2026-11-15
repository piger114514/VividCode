#pragma once
#include "VividCode.h"
#include "SemanticPanel.h"
#include "UnifiedGraphStore.h"
#include "UiTestHelpers.h"
#include <QtWidgets>

inline int runJavaSmokeTest(QApplication &app) {
    const auto fail=[](int line){qCritical()<<"FAIL: Java UI at JavaSmokeTest.h"<<line;return 1;};
    QFontDatabase::addApplicationFont(qEnvironmentVariable("WINDIR","C:/Windows")+"/Fonts/msyh.ttc");
    app.setFont(QFont("Microsoft YaHei",10));
    QTemporaryDir directory(QDir::tempPath()+QStringLiteral("/vividcode Java 中文-XXXXXX"));
    const auto write=[&](const QString &name,const QByteArray &bytes){QFile file(directory.filePath(name));return file.open(QIODevice::WriteOnly)&&file.write(bytes)==bytes.size();};
    if(!directory.isValid()||!write("Helper.java","class Helper {\n static int work(int x){return x*2;}\n}\n")||
        !write("Use.java","class Use {\n static int caller(){\n  return Helper.work(3);\n }\n}\n")) return fail(__LINE__);
    VividCodeWindow window;window.show();auto *panel=window.findChild<SemanticPanel *>();if(!panel)return fail(__LINE__);
    const auto wait=[&]{if(!panel->isBusy())return true;QEventLoop loop;QTimer timer;timer.setSingleShot(true);QObject::connect(&timer,&QTimer::timeout,&loop,&QEventLoop::quit);QObject::connect(panel,&SemanticPanel::settled,&loop,&QEventLoop::quit);timer.start(45000);loop.exec();return timer.isActive()&&!panel->isBusy();};
    const auto state=[&]{QFile file(VividCode::semanticDirectory(directory.path())+"/java/config.json");return file.open(QIODevice::ReadOnly)?QJsonDocument::fromJson(file.readAll()).object():QJsonObject();};
    if(!window.openProject(directory.path())||!wait()||state().value("last_status")!="ok") {
        qCritical().noquote()<<window.findChild<QPlainTextEdit *>("semanticDiagnostics")->toPlainText();return fail(__LINE__);
    }
    auto *symbols=window.findChild<QTreeWidget *>("semanticSymbols");auto *graph=window.findChild<RelationGraph *>();
    auto *scene=graph->findChild<QGraphicsScene *>("relationGraphScene");auto *source=window.findChild<QPlainTextEdit *>("sourceViewer");
    const auto symbolId=[&](const QString &name){for(QTreeWidgetItemIterator it(symbols);*it;++it){auto s=(*it)->data(0,Qt::UserRole).toJsonObject();if(s.value("qualified_name")==name)return s.value("id").toString();}return QString();};
    const QString caller=symbolId("Use.caller"),helper=symbolId("Helper.work");
    if(caller.isEmpty()||helper.isEmpty()||!panel->analysisLanguages().contains("java"))return fail(__LINE__);
    auto *filter=window.findChild<QComboBox *>("semanticRelationType");filter->setCurrentIndex(filter->findData("calls"));
    panel->focusSymbol(caller);graph->setZoomLevel(1.0); if (!showGraphDetails(graph)) return fail(__LINE__);QGraphicsItem *edge=nullptr,*definition=nullptr;
    for(auto *item:scene->items()) {
        if(item->data(0)=="symbol"&&item->data(1).toJsonObject().value("id")==helper)definition=item;
        if(item->data(0)=="relation")for(const auto &v:item->data(1).toJsonArray()){auto r=v.toObject();if(r.value("source_id")==caller&&r.value("target_id")==helper&&r.value("type")=="calls")edge=item;}
    }
    if(!edge||!definition||!triggerGraphSource(graph,edge)||source->textCursor().blockNumber()!=2||!source->textCursor().block().text().contains("Helper.work(3)"))return fail(__LINE__);
    if(!triggerGraphSource(graph,definition)||source->textCursor().blockNumber()!=1)return fail(__LINE__);
    clickOutsidePopup(window);app.processEvents();
    if(!window.openProject(directory.path())||!wait()||state().value("reused").toInt()!=2)return fail(__LINE__);
    UnifiedGraphStore store(directory.path());auto original=store.load().value("java").value("units");
    if(!write("Use.java","class Use { broken( {")||!window.openProject(directory.path())||!wait()||state().value("last_status")!="failed")return fail(__LINE__);
    if(symbolId("Use.caller").isEmpty()||store.load().value("java").value("units")!=original)return fail(__LINE__);
    if(!write("Use.java","class Use { static int changed(){return Helper.work(5);} }")||
        !write("other.py","def helperPython():\n    return 1\ndef callerPython():\n    return helperPython()\n")||!window.openProject(directory.path())||!wait())return fail(__LINE__);
    if(symbolId("Use.changed").isEmpty()||store.load().size()!=2)return fail(__LINE__);
    bool javaLabel=false,pythonLabel=false;
    for(auto *item:scene->items())if(auto *label=dynamic_cast<QGraphicsSimpleTextItem *>(item)){javaLabel|=label->text()=="Java";pythonLabel|=label->text()=="Python";}
    if(!javaLabel||!pythonLabel)return fail(__LINE__);
    const auto before=store.load().value("java").value("units");const auto saved=qgetenv("VIVIDCODE_JAVA");
    qputenv("VIVIDCODE_JAVA",directory.filePath("missing.exe").toUtf8());
    if(!window.openProject(directory.path())||!wait()||state().value("last_status")!="failed"||store.load().value("java").value("units")!=before)return fail(__LINE__);
    if(saved.isNull())qunsetenv("VIVIDCODE_JAVA");else qputenv("VIVIDCODE_JAVA",saved);
    if(!window.openProject(directory.path())||!wait()||state().value("last_status")!="ok")return fail(__LINE__);
    filter->setCurrentIndex(0);panel->focusSymbol({});graph->setZoomLevel(1.0); if (!showGraphDetails(graph)) return fail(__LINE__);app.processEvents();
    auto *view=graph->findChild<QGraphicsView *>("relationGraphView");view->fitInView(scene->itemsBoundingRect().adjusted(-30,-30,30,30),Qt::KeepAspectRatio);
    if(!window.grab().save(QCoreApplication::applicationDirPath()+"/java-relation-preview.png"))return fail(__LINE__);
    if(!QFile::remove(directory.filePath("Use.java"))||!QFile::remove(directory.filePath("Helper.java"))||!window.openProject(directory.path())||!wait())return fail(__LINE__);
    if(state().value("total").toInt(-1)!=0||!store.load().value("java").value("units").toArray().isEmpty()||store.load().value("python").value("units").toArray().isEmpty())return fail(__LINE__);
    qInfo()<<"PASS: Java shared graph, cross-file definition/call navigation, cache, failure and missing-JDK preservation/recovery, Python partition, deletion";
    return 0;
}
