#pragma once
#include "PiAgent.h"
#include "AiExplanation.h"
#include "VividCode.h"
#include "RelationGraph.h"
#include "UnifiedGraphStore.h"
#include <QTcpServer>
#include <QTcpSocket>
#include <functional>

inline int runPiAgentSmokeTest(QApplication &app) {
    using namespace VividCode;
    QFontDatabase::addApplicationFont(qEnvironmentVariable("WINDIR", "C:/Windows") + "/Fonts/msyh.ttc");
    app.setFont(QFont("Microsoft YaHei", 10));
    QTemporaryDir temporary; if (!temporary.isValid()) return 1;
    const auto previous = qgetenv("VIVIDCODE_HOME");
    struct Restore { QByteArray value; ~Restore() { if (value.isNull()) qunsetenv("VIVIDCODE_HOME"); else qputenv("VIVIDCODE_HOME", value); } } restore{previous};
    qputenv("VIVIDCODE_HOME", (temporary.path() + "/home").toUtf8());
    auto check = [](bool ok, const char *message) { if (!ok) throw std::runtime_error(message); };
    auto waitFor = [&](const std::function<bool()> &done, int timeout = 10000) {
        QElapsedTimer timer; timer.start(); while (!done() && timer.elapsed() < timeout) { app.processEvents(QEventLoop::AllEvents, 20); QThread::msleep(1); } check(done(), "Pi asynchronous test timeout");
    };
    auto write = [&](const QString &path, const QByteArray &bytes) { QDir().mkpath(QFileInfo(path).dir().absolutePath()); QFile file(path); check(file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size(), "write Pi fixture"); };
    const QString root = temporary.path() + QStringLiteral("/项目 with spaces"); QDir().mkpath(root);
    const QString script = temporary.path() + "/fake pi.cjs";
    try {
        write(root + "/a.cpp", "int value = 1;\n"); write(root + "/unchanged.cpp", "int unchanged = 9;\n");
        write(script, R"JS(const fs = require('node:fs');
let buffer = '', timer;
fs.mkdirSync('.sessions',{recursive:true});
const path=require('node:path'); let sessionFile=path.resolve('.sessions/current.jsonl');
for(const name of ['current','archived','other'])fs.writeFileSync('.sessions/'+name+'.jsonl',JSON.stringify({type:'session',cwd:name==='other'?path.resolve('other'):process.cwd()})+'\n');
for(const message of [{role:'assistant',content:'ignore assistant'},{role:'user',content:[{type:'image',data:'ignored'},{type:'text',text:'历史问题\n  '+ '🙂'.repeat(60)}]},{role:'user',content:'later question'}])fs.appendFileSync('.sessions/archived.jsonl',JSON.stringify({type:'message',message})+'\n');
const emit = o => process.stdout.write(JSON.stringify(o) + '\r\n');
process.stdin.on('data', chunk => {
 buffer += chunk.toString('utf8'); let i;
 while ((i = buffer.indexOf('\n')) >= 0) {
  const c = JSON.parse(buffer.slice(0, i)); buffer = buffer.slice(i+1);
  fs.appendFileSync('commands.log', JSON.stringify(c)+'\n');
  const response = data => emit({type:'response', command:c.type, id:c.id, success:true, data});
  if (c.type === 'get_state') response({model:{id:'fixture-model'}, isStreaming:false,sessionFile});
  else if (c.type === 'abort') { clearTimeout(timer); response({}); emit({type:'agent_settled', aborted:true}); }
  else if(c.type==='get_messages')response({messages:sessionFile.endsWith('archived.jsonl')?[{role:'user',content:'old question'},{role:'assistant',content:[{type:'thinking',thinking:'HIDDEN_THOUGHT'},{type:'text',text:'Restored **answer** $x^2$'}],stopReason:'stop'},{role:'toolResult',content:[{type:'text',text:'HIDDEN_TOOL'}]}]:[]});
  else if(c.type==='switch_session'){sessionFile=c.sessionPath;response({cancelled:false});}
  else if(c.type==='new_session'){sessionFile=path.resolve('.sessions/new.jsonl');fs.writeFileSync(sessionFile,JSON.stringify({type:'session',cwd:process.cwd()})+'\n');response({cancelled:false});}
  else if (c.type === 'clear_queue'  || c.type === 'extension_ui_response') response({});
  else if (c.type === 'steer') { response({disposition:'queued'}); emit({type:'message_update',assistantMessageEvent:{type:'text_delta',delta:'STEER_ACCEPTED'}}); }
  else if (c.type === 'prompt') {
   if (c.message === 'handled') { response({disposition:'handled'}); continue; }
   if (c.message === 'reject') { emit({type:'response', command:c.type, id:c.id, success:false, error:'fixture rejected'}); continue; }
   fs.appendFileSync(sessionFile,JSON.stringify({type:'message',message:{role:'user',content:c.message}})+'\n');
   response({disposition:'started'}); emit({type:'agent_start'});
   emit({type:'message_start', message:{role:'assistant'}});
   const packet = Buffer.from(JSON.stringify({type:'message_update',assistantMessageEvent:{type:'text_delta', delta:'流式回复\u2028下一段'}})+'\n');
   if(c.message==='stop')process.stdout.write(packet);
   else {process.stdout.write(packet.subarray(0, packet.length-8));setTimeout(()=>process.stdout.write(packet.subarray(packet.length-8)),15);}
   timer = setTimeout(() => {
    fs.writeFileSync('a.cpp', 'int value = ' + (c.message === 'closing' ? 43 : 42) + ';\n');
    if (c.message === 'crash') process.exit(7);
    if (c.message === 'bad') { process.stdout.write('not JSON\n'); return; }
    if (c.message === 'ui') emit({type:'extension_ui_request',id:'fixture-confirm',method:'confirm',title:'fixture confirmation',message:'Continue fixture?',timeout:5000});
    emit({type:'message_end', message:{role:'assistant',content:[{type:'text',text:'流式回复\u2028下一段'}]}});
    emit({type:'tool_execution_start',toolName:'bash',args:{command:'indirect source edit'}});
    emit({type:'tool_execution_end',toolName:'bash',result:{content:[{type:'text',text:'done'}]},isError:false});
    emit({type:'agent_end',willRetry:false,messages:[]});
    timer=setTimeout(()=> {
      fs.writeFileSync('added.py','answer = 42\n');
      emit({type:'agent_settled',aborted:false});
    }, 300);
   }, c.message === 'stop' ? 5000 : 120);
  }
 }
});
process.stdin.on('end',()=>process.exit(0));
)JS");
        const QString node = QStandardPaths::findExecutable("node"); check(!node.isEmpty(), "Node available for subprocess fixture");
        auto config = [&](bool automatic, const QString &path = QString()) {
            write(sharedHome() + "/pi-agent.json", QJsonDocument(QJsonObject{{"schema",1},{"piPath",path.isEmpty()?script:path},{"nodePath",node},{"autoRefresh",automatic}}).toJson());
        };
        config(false);
        {
            PiAgentPanel panel; panel.setProject(root); panel.resize(1000, 300); panel.show();
            auto *connect = panel.findChild<QPushButton *>("piConnect"); auto *send = panel.findChild<QPushButton *>("piSend");
            auto *stop = panel.findChild<QPushButton *>("piStop"); auto *input = panel.findChild<QPlainTextEdit *>("piInput");
            auto *text = panel.findChild<QTextBrowser *>("piConversation"); auto *automatic = panel.findChild<QCheckBox *>("piAutoRefresh");
            int refreshes = 0; QStringList changed;
            QObject::connect(&panel, &PiAgentPanel::refreshRequested, &panel, [&](const QStringList &files) { ++refreshes; changed = files; panel.finishRefresh("fixture refresh done"); });
            waitFor([&] { return send->isEnabled(); });
            check(text->toPlainText().isEmpty(), "auto connect keeps status out of answer view");
            input->setPlainText("normal");
            QKeyEvent newline(QEvent::KeyPress, Qt::Key_Return, Qt::ShiftModifier); QApplication::sendEvent(input, &newline);
            check(input->toPlainText().contains('\n'), "Shift Enter inserts newline"); input->setPlainText("normal");
            QKeyEvent enter(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier); QApplication::sendEvent(input, &enter);
            waitFor([&] { return text->toPlainText().contains(QStringLiteral("流式回复")); });
            check(panel.isBusy() && send->isEnabled() && !QFileInfo::exists(root + "/added.py"), "agent_end is not task completion");
            input->setPlainText("change direction"); send->click();
            waitFor([&] { QFile log(root+"/commands.log");return log.open(QIODevice::ReadOnly)&&log.readAll().contains("steer"); });
            check(panel.isBusy() && input->toPlainText().isEmpty(), "steering is sent during the active run");
            waitFor([&] { return !panel.isBusy(); });
            check(refreshes == 0 && QFileInfo::exists(root+"/added.py") && !text->toPlainText().contains("[工具"), "disabled update and indirect edits detected");
            check(text->toPlainText().contains(QStringLiteral("流式回复")) && text->toPlainText().count(QStringLiteral("流式回复")) == 1, "fragmented UTF8 JSONL streaming without duplicate final text");
            automatic->setChecked(true); write(root + "/a.cpp", "int value = 0;\n");
            input->setPlainText("normal"); send->click(); waitFor([&] { return refreshes == 1 && !panel.isBusy(); });
            check(changed == QStringList{"a.cpp"}, "exact changed-file list and one update per settled run");

            input->setPlainText("stop"); send->click(); waitFor([&] { return stop->isEnabled(); }); stop->click(); waitFor([&] { return !panel.isBusy(); });
            check(refreshes == 2 && send->isEnabled(), "abort recovers input and updates once");
            input->setPlainText("reject"); send->click(); waitFor([&] { return !panel.isBusy() && panel.findChild<QLabel *>("piStatus")->text().contains("fixture rejected"); });
            check(refreshes == 2, "rejected prompt does not trigger update");
            input->setPlainText("handled"); send->click(); waitFor([&] { return !panel.isBusy() && refreshes == 3; });
            check(send->isEnabled(), "handled command completes without agent events");
            input->setPlainText("ui"); send->click();
            waitFor([&] { for (auto *dialog : panel.findChildren<QDialog *>()) if (dialog->isVisible()) return true; return false; });
            for (auto *dialog : panel.findChildren<QDialog *>()) if (dialog->isVisible()) dialog->accept();
            waitFor([&] { return !panel.isBusy(); });
            QFile commands(root + "/commands.log"); check(commands.open(QIODevice::ReadOnly) && commands.readAll().contains("fixture-confirm"), "extension confirmation returned over RPC"); commands.close();
            auto *history=panel.findChild<QComboBox *>("piHistory");
            check(history->count()==2,"history includes only current project sessions");
            check(history->currentText()=="normal","history keeps the first user prompt after subsequent turns");
            int archived=-1;for(int i=0;i<history->count();++i)if(history->itemData(i).toString().endsWith("archived.jsonl"))archived=i;
            check(archived>=0,"archived project session discovered");
            check(history->itemText(archived)==QStringLiteral("历史问题 ")+QString::fromUtf8("🙂").repeated(43)+QStringLiteral("…"),"history collapses whitespace and truncates at 48 graphemes without splitting emoji");
            check(history->itemData(archived,Qt::ToolTipRole).toString().contains("archived"),"history tooltip retains session identity");
            history->setCurrentIndex(archived);emit history->activated(archived);
            waitFor([&]{return !panel.isBusy()&&text->toPlainText().contains("Restored");});
            check(!text->toPlainText().contains("HIDDEN_"),"history filters thought and tool content");
            waitFor([&]{return !text->document()->resource(QTextDocument::ImageResource,QUrl("math:0")).value<QImage>().isNull();});
            check(!text->document()->resource(QTextDocument::ImageResource,QUrl("https://example.com/image.png")).isValid(),"remote image loads blocked");
            const auto mathShot=qEnvironmentVariable("VIVIDCODE_PI_SCREENSHOT");if(!mathShot.isEmpty()){panel.outputView()->resize(900,600);panel.outputView()->show();app.processEvents();panel.outputView()->grab().save(mathShot+".math.png");}
            panel.findChild<QPushButton *>("piNewSession")->click();waitFor([&]{return !panel.isBusy()&&text->toPlainText().isEmpty();});
            input->setPlainText("stop"); send->click(); waitFor([&] { return stop->isEnabled(); });
            const QString second = temporary.path() + "/second"; QDir().mkpath(second); panel.setProject(second);
            waitFor([&] { return connect->isEnabled(); }); check(!panel.isBusy() && text->toPlainText().isEmpty(), "project switch terminates old session and clears conversation");
        }
        for (const auto &mode : {"bad", "crash"}) {
            write(root + "/a.cpp", "int value = 0;\n"); config(true); PiAgentPanel panel; panel.setProject(root);
            auto *connect = panel.findChild<QPushButton *>("piConnect"); auto *send = panel.findChild<QPushButton *>("piSend");
            int refreshes = 0; QObject::connect(&panel, &PiAgentPanel::refreshRequested, &panel, [&](const QStringList &paths) { check(paths.contains("a.cpp"), "partial edits survive process failure"); ++refreshes; panel.finishRefresh("failure checked"); });
            waitFor([&] { return send->isEnabled(); }); panel.findChild<QPlainTextEdit *>("piInput")->setPlainText(mode); send->click();
            waitFor([&] { return refreshes == 1 && !panel.isBusy() && connect->isEnabled(); });
            check(!send->isEnabled() && !panel.isBusy(), "malformed protocol or crash disconnects cleanly and reports partial changes");
        }
        qInfo() << "PASS: real subprocess JSONL, Unicode, project isolation, stream, tool events, completion, toggle, abort and rejection";
        qInfo() << "PASS: handled commands, extension confirmation, malformed protocol, process crash and partial-edit refresh";
        // Exercise the complete window -> graph settled -> scoped AI request chain.
        config(true); write(root + "/a.cpp", "int value = 0;\n"); QFile::remove(root + "/added.py");
        write(root + "/.vividcode/project.json", QJsonDocument(QJsonObject{{"app","vividcode"},{"schema",1}}).toJson());
        QDir().mkpath(sharedRuntime() + "/analyzer");
        for (const auto &name : {"analyzer.py", "python_indexer.py"})
            check(QFile::copy(QCoreApplication::applicationDirPath() + "/analyzer/" + name, sharedRuntime() + "/analyzer/" + name), "copy Python analysis fixture runtime");
        QTcpServer server; check(server.listen(QHostAddress::LocalHost), "fake explanation server"); QStringList explained; bool pauseExplanations = false;
        QObject::connect(&server, &QTcpServer::newConnection, &server, [&] {
            while (auto *socket = server.nextPendingConnection()) {
                QObject::connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
                QObject::connect(socket, &QTcpSocket::readyRead, socket, [&, socket] {
                    QByteArray buffer = socket->property("body").toByteArray() + socket->readAll(); socket->setProperty("body", buffer);
                    const auto split = buffer.indexOf("\r\n\r\n"); if (split < 0 || socket->property("handled").toBool()) return;
                    int length = -1; for (auto line : buffer.left(split).split('\n')) if (line.toLower().startsWith("content-length:")) length = line.mid(15).trimmed().toInt();
                    if (length < 0 || buffer.size() < split + 4 + length) return; socket->setProperty("handled", true);
                    const auto req = QJsonDocument::fromJson(buffer.mid(split+4,length)).object();
                    const auto content = req.value("messages").toArray().at(1).toObject().value("content").toString();
                    explained.append(QJsonDocument::fromJson(content.toUtf8()).object().value("path").toString());
                    if (pauseExplanations) return;
                    const QString answer = QString::fromUtf8(QJsonDocument(QJsonObject{{"explanation",QStringLiteral("文件负责示例计算。")},{"refresh",QJsonArray{}}}).toJson(QJsonDocument::Compact));
                    const auto body = QJsonDocument(QJsonObject{{"choices",QJsonArray{QJsonObject{{"message",QJsonObject{{"content",answer}}}}}}}).toJson(QJsonDocument::Compact);
                    socket->write("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nConnection: close\r\nContent-Length: " + QByteArray::number(body.size()) + "\r\n\r\n" + body); socket->disconnectFromHost();
                });
            }
        });
        AiSettings ai; ai.baseUrl = QString("http://127.0.0.1:%1/v1").arg(server.serverPort()); ai.model = "fixture"; ai.apiKey = "local-fixture"; ai.save();
        {
            VividCodeWindow window; check(window.openProject(root), "open integration project"); window.show();
            auto *panel = window.findChild<PiAgentPanel *>(); auto *tabs = window.findChild<QTabWidget *>("workspaceTabs");
            check(tabs && tabs->count() == 2 && tabs->tabText(0) == UiLanguage::text("Relations") && tabs->tabText(1) == QStringLiteral("pi"), "bottom switchable panels");
            tabs->setCurrentWidget(panel->outputView()); check(window.findChild<QCheckBox *>("piAutoRefresh")->isChecked(), "update preference persisted");
            auto *send = panel->findChild<QPushButton *>("piSend"); waitFor([&] { return send->isEnabled(); });
            panel->findChild<QPlainTextEdit *>("piInput")->setPlainText("normal"); send->click();
            waitFor([&] { return !panel->isBusy() && explained.size() == 2; }); explained.sort();
            check(explained == QStringList{"a.cpp","added.py"}, "scoped explanation updates exclude unmodified uncached sources");
            const auto units = UnifiedGraphStore(root).load().value("python").value("units").toArray();
            if (units.size() != 1) qCritical().noquote() << window.findChild<QPlainTextEdit *>("semanticDiagnostics")->toPlainText();
            check(units.size() == 1 && units.first().toObject().value("file") == "added.py", "Pi-created source reaches actual semantic graph database before explanation completes");
            const auto target = qEnvironmentVariable("VIVIDCODE_PI_SCREENSHOT"); if (!target.isEmpty()) { window.resize(1320,900); app.processEvents(); check(window.grab().save(target), "save bottom panel screenshot"); tabs->setCurrentIndex(0); app.processEvents(); check(window.grab().save(target + ".info.png"), "save info screenshot"); }
        }
        qInfo() << "PASS: bottom tabs, persisted option, graph refresh and scoped AI pipeline with local HTTP fixture";
        {
            write(root + "/a.cpp", "int value = 0;\n"); pauseExplanations = true; const int priorRequests = int(explained.size());
            auto *window = new VividCodeWindow; check(window->openProject(root), "open close-during-refresh fixture");
            auto *panel = window->findChild<PiAgentPanel *>(); auto *send = panel->findChild<QPushButton *>("piSend");
            waitFor([&] { return send->isEnabled(); });
            panel->findChild<QPlainTextEdit *>("piInput")->setPlainText("closing"); send->click();
            waitFor([&] { return explained.size() > priorRequests; }); check(panel->isBusy(), "AI refresh still in progress");
            delete window; app.processEvents(); pauseExplanations = false;
            qInfo() << "PASS: window closes safely during automatic AI request and shuts down Pi";
        }
        // Optional installed-Pi handshake: no prompt, no model request or source edits.
        const auto realPi = qEnvironmentVariable("VIVIDCODE_TEST_PI");
        if (!realPi.isEmpty()) {
            config(false, realPi); PiAgentPanel panel; panel.setProject(root);
            waitFor([&] { return panel.findChild<QPushButton *>("piSend")->isEnabled(); }, 30000);
            qInfo() << "PASS: installed Pi RPC handshake without model invocation";
        }
        return 0;
    } catch (const std::exception &e) { qCritical() << "FAIL:" << e.what(); return 1; }
}
