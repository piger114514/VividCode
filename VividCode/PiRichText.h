#pragma once
#include "UiLanguage.h"
#include <QtWidgets>
#include <QProcess>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
class PiRichText : public QTextBrowser {
public:
    explicit PiRichText(QWidget *parent=nullptr) : QTextBrowser(parent) {
        setOpenLinks(false); setOpenExternalLinks(false);
        connect(this,&QTextBrowser::anchorClicked,this,[](const QUrl &url){if(url.scheme()=="https"||url.scheme()=="http")QDesktopServices::openUrl(url);});
        delay.setSingleShot(true); connect(&delay,&QTimer::timeout,this,[this]{render();});
        timeout.setSingleShot(true); connect(&timeout,&QTimer::timeout,this,[this]{worker.kill();});
        connect(&worker,qOverload<int,QProcess::ExitStatus>(&QProcess::finished),this,[this](int code,QProcess::ExitStatus){
            timeout.stop(); const auto bytes=worker.readAllStandardOutput();
            if(activeVersion==version && code==0){const auto obj=QJsonDocument::fromJson(bytes).object();if(obj.contains("html")){
                resources.clear(); const auto images=obj.value("images").toObject();
                for(auto it=images.begin();it!=images.end();++it){QImage image=QImage::fromData(it.value().toString().toUtf8(),"SVG");if(!image.isNull()){if(image.width()>1000)image=image.scaledToWidth(1000,Qt::SmoothTransformation);resources.insert(it.key(),image);}}
                const bool bottom=verticalScrollBar()->value()>=verticalScrollBar()->maximum()-4;const int scroll=verticalScrollBar()->value();
                setHtml(obj.value("html").toString());verticalScrollBar()->setValue(bottom?verticalScrollBar()->maximum():scroll);
            }}
            if(activeVersion!=version)delay.start(0);
        });
    }
    ~PiRichText() override { worker.disconnect(this);worker.kill();worker.waitForFinished(1000); }
    void setNode(const QString &path){node=path;}
    void clear(){waiting=false;messages=QJsonArray{};source.clear();++version; resources.clear(); QTextBrowser::clear();}
    void appendMessage(const QString &role,const QString &text){
        waiting=false; messages.append(QJsonObject{{"role",role},{"text",text.left(1024*1024)}});
        refreshMessages();
    }
    void setThinking(bool value){waiting=value;refreshMessages();}
    void refreshMessages(){
        auto display=messages;if(waiting)display.append(QJsonObject{{"role","assistant"},{"text",UiLanguage::text("Thinking...")}});
        auto encode=[&display]{return QString::fromUtf8(QJsonDocument(QJsonObject{{"messages",display}}).toJson(QJsonDocument::Compact));};
        source=encode();while(source.toUtf8().size()>1500*1024 && messages.size()>1){messages.removeAt(0);display.removeAt(0);source=encode();}
        ++version;
        QString html="<table width='100%' cellspacing='0' cellpadding='12' border='0'>";
        for(int i=0;i<display.size();++i){const auto message=display[i].toObject();
            const QString name=message.value("role")=="user"?UiLanguage::text("You"):QStringLiteral("pi");
            const QString color=message.value("role")=="user"?"#2d2d2d":"#181818";
            html+=QString("<tr bgcolor='%1'><td width='52' valign='top'><span style='font-size:14pt;font-weight:700;color:#ffffff'>%2</span></td><td valign='top'>%3</td></tr>").arg(color,name,message.value("text").toString().toHtmlEscaped().replace("\n","<br>"));
        }
        const bool bottom=verticalScrollBar()->value()>=verticalScrollBar()->maximum()-4; const int scroll=verticalScrollBar()->value();
        setHtml(html+"</table>");verticalScrollBar()->setValue(bottom?verticalScrollBar()->maximum():scroll);delay.start(80);
    }
protected:
    QVariant loadResource(int, const QUrl &url) override {return resources.value(url.toString());}
private:
    void render(){
        if(worker.state()!=QProcess::NotRunning)return;
        QString executable=node;if(executable.isEmpty())executable=QStandardPaths::findExecutable("node");
        const QString script=QCoreApplication::applicationDirPath()+"/pi-renderer/render.cjs";
        if(executable.isEmpty()||!QFileInfo::exists(script))return;
        activeVersion=version;auto env=QProcessEnvironment::systemEnvironment();env.remove("NODE_OPTIONS");env.remove("NODE_PATH");worker.setProcessEnvironment(env);
#ifdef Q_OS_WIN
        worker.setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments *args){args->flags|=0x08000000;});
#endif
        worker.start(executable,{script});if(worker.waitForStarted(1000)){worker.write(source.toUtf8());worker.closeWriteChannel();timeout.start(8000);}
    }
    bool waiting=false;
    QJsonArray messages;QProcess worker;QTimer delay,timeout;QString source,node;quint64 version=0,activeVersion=0;QMap<QString,QVariant> resources;
};
