#pragma once
#include "AiExplanation.h"
#include "RelationGraph.h"
#include "VividCode.h"
#include "ProjectStore.h"
#include "UiTestHelpers.h"
#include <QtWidgets>
#include <functional>

inline int runAiSmokeTest(QApplication &app) {
    using namespace VividCode;
    const QString fonts = qEnvironmentVariable("WINDIR", "C:/Windows") + "/Fonts/";
    QFontDatabase::addApplicationFont(fonts + "msyh.ttc"); QFontDatabase::addApplicationFont(fonts + "consola.ttf");
    app.setFont(QFont("Microsoft YaHei", 10));
    QTemporaryDir temporary;
    if (!temporary.isValid()) return 1;
    const auto priorHome = qgetenv("VIVIDCODE_HOME");
    struct Restore { QByteArray previous; ~Restore() { if (previous.isNull()) qunsetenv("VIVIDCODE_HOME"); else qputenv("VIVIDCODE_HOME", previous); } } restore{priorHome};
    qputenv("VIVIDCODE_HOME", (temporary.path() + "/home").toUtf8());
    const QString root = temporary.path() + "/project"; QDir().mkpath(root);
    const auto check = [](bool ok, const char *what) { if (!ok) throw std::runtime_error(what); };
    const auto write = [&](QString path, QByteArray bytes) {
        QFile file(root + '/' + path); check(file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size(), "write fixture");
    };
    const auto waitFor = [&](const std::function<bool()> &done) {
        QElapsedTimer timer; timer.start();
        while (!done() && timer.elapsed() < 10000) { app.processEvents(QEventLoop::AllEvents, 20); QThread::msleep(1); }
        check(done(), "asynchronous operation timeout");
    };
    try {
        check(AiSettings{}.resolvedLanguage() == "en", "English interface is the default AI language");
        write("a.cpp", "#include \"b.hpp\"\nint main() { return value; }\n"); write("b.hpp", "constexpr int value = 1;\n");
        QTcpServer server; check(server.listen(QHostAddress::LocalHost), "listen local fake API");
        AiSettings settings; settings.baseUrl = QString("http://127.0.0.1:%1/v1/").arg(server.serverPort());
        settings.model = "vividcode-test"; settings.apiKey = "vividcode-local-test-key"; settings.language = "zh";
        {
            AiExplanation unconfigured; unconfigured.setProject(root, {"a.cpp", "b.hpp"}, true);
            check(unconfigured.tooltip("a.cpp").contains("Configure AI Settings") && unconfigured.tooltip("outside.cpp").isEmpty(), "unconfigured hover explains how to configure AI");
        }
        check(settings.endpoint().path() == "/v1/chat/completions", "endpoint normalization");
        const auto chineseProfile = settings.profile(); settings.language = "en"; check(settings.profile() != chineseProfile, "language cache identity"); settings.language = "zh";
        AiSettings invalid = settings; invalid.baseUrl = "http://remote.example/v1";
        bool rejected = false; try { invalid.endpoint(); } catch (...) { rejected = true; } check(rejected, "reject nonlocal cleartext HTTP");
        invalid.baseUrl = "https://provider.example/v1?key=secret"; rejected = false; try { invalid.endpoint(); } catch (...) { rejected = true; } check(rejected, "reject credential-bearing URL");
        settings.save(); const auto loaded = AiSettings::load(); check(loaded.apiKey == settings.apiKey && loaded.profile() == settings.profile(), "DPAPI round trip");
        QFile config(sharedHome() + "/ai-settings.json"); check(config.open(QIODevice::ReadOnly) && !config.readAll().contains(settings.apiKey.toUtf8()), "no plaintext key at rest");
        config.close();
        QFile legacyInput(sharedHome() + "/ai-settings.json"); check(legacyInput.open(QIODevice::ReadOnly), "read legacy settings fixture");
        auto legacy = QJsonDocument::fromJson(legacyInput.readAll()).object(); legacyInput.close(); legacy.remove("audience");
        QSaveFile legacyOutput(sharedHome() + "/ai-settings.json"); check(legacyOutput.open(QIODevice::WriteOnly), "write legacy settings fixture");
        legacyOutput.write(QJsonDocument(legacy).toJson()); check(legacyOutput.commit(), "commit legacy settings fixture");
        check(AiSettings::load().audience == "intermediate" && AiSettings::load().profile() == chineseProfile, "legacy settings default to intermediate without invalidating prior cache identity");
        qInfo() << "PASS: encrypted settings, endpoint validation, profile identity";

        QString mode = "normal"; int version = 1; QList<QJsonObject> requests; QStringList prompts;
        QObject::connect(&server, &QTcpServer::newConnection, &server, [&] {
            while (auto *socket = server.nextPendingConnection()) {
                QObject::connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
                QObject::connect(socket, &QTcpSocket::readyRead, socket, [&, socket] {
                    if (socket->property("handled").toBool()) { socket->readAll(); return; }
                    QByteArray buffer = socket->property("buffer").toByteArray() + socket->readAll(); socket->setProperty("buffer", buffer);
                    const auto split = buffer.indexOf("\r\n\r\n"); if (split < 0) return;
                    const auto headers = buffer.left(split); int length = -1;
                    for (auto line : headers.split('\n')) if (line.toLower().startsWith("content-length:")) length = line.mid(15).trimmed().toInt();
                    if (length < 0 || buffer.size() < split + 4 + length) return;
                    socket->setProperty("handled", true);
                    const auto req = QJsonDocument::fromJson(buffer.mid(split + 4, length)).object();
                    const auto messages = req.value("messages").toArray();
                    prompts.append(messages.at(0).toObject().value("content").toString());
                    const auto input = QJsonDocument::fromJson(messages.at(1).toObject().value("content").toString().toUtf8()).object(); requests.append(input);
                    if (!headers.startsWith("POST /v1/chat/completions HTTP/1.1") || !headers.contains("Bearer " + settings.apiKey.toUtf8())) { socket->abort(); return; }
                    if (mode == "pause") return;
                    if (mode == "mutate") write(input.value("path").toString(), "// changed while request was in flight\n");
                    int code = mode == "error" ? 429 : mode == "redirect" ? 302 : 200;
                    QJsonArray refresh;
                    if ((mode == "nominate" || input.value("deleted").toBool()) && input.value("path") == "a.cpp") {
                        refresh.append(QJsonObject{{"path", "b.hpp"}, {"reason", "its documented collaboration has changed"}});
                        refresh.append(QJsonObject{{"path", "../outside.cpp"}, {"reason", "must be rejected"}});
                        refresh.append(QJsonObject{{"path", "b.hpp"}, {"reason", "duplicate nomination"}});
                    }
                    // Cycles must not cause repeated API calls within a run.
                    if (mode == "nominate" && input.value("path") == "b.hpp") refresh.append(QJsonObject{{"path", "a.cpp"}, {"reason", "cycle"}});
                    const bool english = messages.at(0).toObject().value("content").toString().contains("Use English");
                    const QString explanation = (english ? QString("%1 explanation v%2 <untrusted-tag>. Defines shared values used by the caller; the calling file provides the program entry point.")
                        : QStringLiteral("%1 的文件作用 v%2 <untrusted-tag>。定义供调用方使用的共享常量；调用方文件负责程序入口。依赖变化后，此解释可由 AI 判断并更新。"))
                        .arg(input.value("path").toString()).arg(version);
                    const auto answer = QJsonDocument(QJsonObject{{"explanation", explanation}, {"refresh", refresh}}).toJson(QJsonDocument::Compact);
                    auto body = mode == "error" ? settings.apiKey.toUtf8() : QJsonDocument(QJsonObject{{"choices", QJsonArray{
                        QJsonObject{{"finish_reason", "stop"}, {"message", QJsonObject{{"content", mode == "bad" ? "invalid result" : QString::fromUtf8(answer)}}}}}}}).toJson(QJsonDocument::Compact);
                    if (mode == "oversize") body = QByteArray(1024 * 1024 + 1, 'x');
                    socket->write("HTTP/1.1 " + QByteArray::number(code) + " Test\r\nContent-Type: application/json\r\nConnection: close\r\nContent-Length: " + QByteArray::number(body.size()) + "\r\n" + (code == 302 ? QByteArray("Location: http://127.0.0.1:1/stolen\r\n") : QByteArray()) + "\r\n" + body);
                    socket->disconnectFromHost();
                });
            }
        });
        AiExplanation ai;
        const QStringList files{"a.cpp", "b.hpp"}; const QMap<QString, QStringList> dependencies{{"a.cpp", {"b.hpp"}}, {"b.hpp", {"a.cpp"}}};
        ai.setProject(root, files, true);
        check(ai.tooltip("a.cpp").contains(QStringLiteral("尚无此文件解释")), "missing annotation shows generation guidance");
        const auto run = [&] { ai.start(dependencies); waitFor([&] { return !ai.isBusy(); }); };
        run(); check(requests.size() == 2 && ai.tooltip("a.cpp").contains("v1") && ai.tooltip("a.cpp").contains("&lt;untrusted-tag&gt;"), "initial explanations and HTML escaping");
        run(); check(requests.size() == 2, "unchanged hashes cause zero requests");
        write("a.cpp", "#include \"b.hpp\"\nint main() { return value + 2; }\n"); mode = "nominate"; version = 2; run();
        check(requests.size() == 4 && requests.at(2).value("path") == "a.cpp" && requests.at(3).value("path") == "b.hpp", "hash change and AI nominated unchanged file");
        check(requests.at(3).value("trigger_context").toString().contains("a.cpp") && requests.at(3).value("trigger_context").toString().contains("v2"), "dependency refresh receives changed explanation context");
        check(ai.tooltip("b.hpp").contains("v2"), "unchanged dependent annotation updated");
        check(ai.tooltip("a.cpp").contains(QStringLiteral("待更新")), "cycle recommendation persisted for next run");
        mode = "normal"; run(); check(requests.size() == 5, "deferred dependency recommendation can complete without looping");
        ai.setProject(root, files, true); run(); check(requests.size() == 5, "persistent cache survives project reopening");
        qInfo() << "PASS: hashes, reuse, AI dependency propagation, cycle/path/duplicate guards, cache reopening";

        write("a.cpp", "// revision 3\n"); mode = "error"; run(); check(ai.tooltip("a.cpp").contains("v2") && ai.tooltip("a.cpp").contains(QStringLiteral("待更新")), "HTTP error preserves old annotation and marks stale");
        check(!ai.report().contains(settings.apiKey), "server error cannot disclose key");
        mode = "bad"; run(); check(ai.tooltip("a.cpp").contains("v2"), "malformed model JSON preserves old annotation");
        mode = "redirect"; const int redirectsBefore = requests.size(); run(); check(requests.size() == redirectsBefore + 1 && ai.tooltip("a.cpp").contains("v2"), "redirect rejected without following key");
        mode = "oversize"; run(); check(ai.tooltip("a.cpp").contains("v2"), "oversized response preserves old annotation");
        mode = "mutate"; run(); check(ai.tooltip("a.cpp").contains("v2"), "changed-during-request response discarded");
        mode = "normal"; version = 3; run(); check(ai.tooltip("a.cpp").contains("v3") && !ai.tooltip("a.cpp").contains(QStringLiteral("待更新")), "failed file retried successfully");
        write("a.cpp", "// revision 4\n"); mode = "pause"; const int beforeCancel = requests.size(); ai.start(dependencies);
        waitFor([&] { return requests.size() > beforeCancel; }); ai.cancel(); check(!ai.isBusy() && ai.tooltip("a.cpp").contains("v3"), "cancel active request preserves previous annotation");
        mode = "normal"; version = 4; run(); check(ai.tooltip("a.cpp").contains("v4"), "cancelled request retried");
        const int beforeLevels = requests.size();
        settings.audience = "beginner"; settings.save(); run();
        check(requests.size() == beforeLevels + 2 && prompts.last().contains("Audience: beginner") && prompts.last().contains("6-10"), "beginner detail changes prompt and cache identity");
        settings.audience = "professional"; settings.save(); run();
        check(requests.size() == beforeLevels + 4 && prompts.last().contains("Audience: professional") && prompts.last().contains("1-3"), "professional concise prompt uses separate cache");
        settings.audience = "intermediate"; settings.save(); run();
        check(requests.size() == beforeLevels + 4, "intermediate baseline cache remains reusable");
        qInfo() << "PASS: HTTP/malformed/redirect/oversize failures, source races, redaction, cancellation and retry";

        settings.language = "en"; settings.save(); run(); check(ai.tooltip("a.cpp").contains("AI Analysis"), "English profile and tooltip language");
        settings.language = "auto"; qApp->setProperty("vividcode.uiLanguage", "en-US"); check(settings.resolvedLanguage() == "en", "auto follows English UI");
        qApp->setProperty("vividcode.uiLanguage", "en"); settings.language = "zh"; settings.save();
        ai.setProject(root, files, true); run();
        QFile::remove(root + "/a.cpp"); ai.setProject(root, {"b.hpp"}, true); const int beforeDelete = requests.size(); run();
        check(requests.size() == beforeDelete + 2 && requests.at(beforeDelete).value("deleted").toBool() && ai.tooltip("a.cpp").isEmpty(), "deletion removes annotation and AI refreshes affected file");
        write("large.cpp", QByteArray(300 * 1024, 'x')); ai.setProject(root, {"b.hpp", "large.cpp"}, true); const int beforeLarge = requests.size(); run();
        check(requests.size() == beforeLarge && ai.report().contains(QStringLiteral("exceeds the AI source limit")), "large source never silently truncated");
        ai.setProject(root, {"b.hpp"}, false); const int beforeTruncated = requests.size(); run(); check(requests.size() == beforeTruncated, "incomplete project scan blocks AI");
        qInfo() << "PASS: language profiles, removed-file impact, upload limit and incomplete scan";

        // Real pointer events: hover is metadata only; a click opens a stable card.
        RelationGraph graph; graph.resize(900, 650); graph.show();
        const QJsonObject symbol{{"id", "test-file"}, {"name", "b.hpp"}, {"kind", "RelatedFile"}, {"def_file", "b.hpp"}, {"language", "c++"}};
        int providerReads = 0;
        graph.setFileExplanationProvider([&](const QString &path) { ++providerReads; return ai.tooltip(QDir::isAbsolutePath(path) ? QDir(root).relativeFilePath(path) : path); });
        graph.setData({{"symbols", QJsonArray{symbol}}, {"relations", QJsonArray{}}}, {}, {}, "test"); app.processEvents();
        auto *scene = graph.findChild<QGraphicsScene *>(); auto *view = graph.findChild<QGraphicsView *>(); QGraphicsItem *node = nullptr;
        for (auto *item : scene->items()) if (item->data(0) == "symbol") node = item;
        check(node && view, "canvas fixture");
        const QPoint position = view->mapFromScene(node->sceneBoundingRect().center());
        auto *card = graph.findChild<QWidget *>("aiAnalysisPopup"); auto *cardText = graph.findChild<QTextBrowser *>("aiAnalysisContent");
        check(card && cardText, "analysis card widgets");
        const auto clickAt = [&](const QPoint &point) {
            QMouseEvent press(QEvent::MouseButtonPress, QPointF(point), QPointF(view->viewport()->mapToGlobal(point)), Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
            QApplication::sendEvent(view->viewport(), &press);
            QMouseEvent release(QEvent::MouseButtonRelease, QPointF(point), QPointF(view->viewport()->mapToGlobal(point)), Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
            QApplication::sendEvent(view->viewport(), &release); app.processEvents();
        };
        QHelpEvent hover(QEvent::ToolTip, position, view->viewport()->mapToGlobal(position)); QApplication::sendEvent(view->viewport(), &hover);
        check(!card->isVisible() && providerReads == 0, "hover does not open AI analysis or read cache");
        clickAt(position);
        check(card->isVisible() && cardText->toPlainText().contains("v4"), "file click opens cached AI analysis");
        QHelpEvent blockedHover(QEvent::ToolTip, position, view->viewport()->mapToGlobal(position));
        blockedHover.ignore(); QApplication::sendEvent(view->viewport(), &blockedHover);
        check(blockedHover.isAccepted() && QToolTip::text().isEmpty(), "AI card suppresses subsequent metadata tooltip events");
        clickAt(QPoint(2, 2));
        QHelpEvent restoredHover(QEvent::ToolTip, position, view->viewport()->mapToGlobal(position));
        QApplication::sendEvent(view->viewport(), &restoredHover);
        check(!card->isVisible() && !QToolTip::text().isEmpty(), "metadata hover resumes after AI card closes");
        clickAt(position);
        cardText->selectAll(); check(cardText->textCursor().selectedText().contains("v4"), "analysis card text is selectable");
        const QString screenshot = qEnvironmentVariable("VIVIDCODE_AI_SCREENSHOT");
        if (!screenshot.isEmpty()) {
            graph.grab().save(screenshot);
            card->grab().save(screenshot + ".card.png");
        }
        QToolTip::hideText();
        card->hide(); const int beforeHover = requests.size();
        const QJsonArray classSymbols{
            QJsonObject{{"id", "catalog"}, {"name", "ExpeditionEventCatalog"}, {"kind", "ClassDecl"}, {"def_file", root + "/b.hpp"}, {"language", "c#"}},
            QJsonObject{{"id", "method"}, {"name", "GetEvents"}, {"kind", "CXXMethod"}, {"parent_id", "catalog"}, {"def_file", root + "/b.hpp"}, {"language", "c#"}},
            QJsonObject{{"id", "local"}, {"name", "events"}, {"kind", "VarDecl"}, {"parent_id", "method"}, {"decl_file", "b.hpp"}, {"language", "c#"}}
        };
        graph.setData({{"symbols", classSymbols}, {"relations", QJsonArray{}}, {"config_id", "class-hover"}}, {}, {}, "class hover"); app.processEvents();
        const auto hoverItem = [&](const QString &role, const QString &id, bool frame = false) {
            QGraphicsItem *target = nullptr;
            for (auto *item : scene->items()) if (item->data(0) == role && item->data(1).toJsonObject().value("id") == id) { target = item; break; }
            check(target != nullptr, "hover fixture item exists");
            view->centerOn(target); app.processEvents(); QToolTip::hideText();
            const QPointF scenePoint = frame ? target->mapToScene(target->boundingRect().topLeft() + QPointF(10, 10)) : target->sceneBoundingRect().center();
            const QPoint at = view->mapFromScene(scenePoint); clickAt(at);
            check(card->isVisible() && cardText->toPlainText().contains(QStringLiteral("AI 解析")) && cardText->toPlainText().contains("v4"), "symbol or expanded class click displays cached file analysis");
        };
        hoverItem("symbol", "catalog");
        check(!cardText->toPlainText().contains("ExpeditionEventCatalog") && !cardText->toPlainText().contains(root) && !cardText->toPlainText().contains(QStringLiteral("右键切换")) && !cardText->toPlainText().contains(QStringLiteral("相关符号")), "click card contains analysis only, without hover metadata");
        if (!screenshot.isEmpty()) {
            graph.grab().save(screenshot + ".class.png");
            card->grab().save(screenshot + ".class-card.png");
        }
        check(showGraphDetails(&graph), "expand class through real context action");
        hoverItem("symbol", "method"); hoverItem("symbol", "local"); hoverItem("container", "catalog", true);
        QKeyEvent escape(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier); QApplication::sendEvent(view->viewport(), &escape);
        check(card->isVisible(), "Escape does not dismiss analysis card");
        check(!graph.findChild<QToolButton *>("aiAnalysisClose"), "analysis card has no close button");
        hoverItem("symbol", "local"); clickAt(QPoint(2, 2)); check(!card->isVisible(), "blank click dismisses analysis card");
        QGraphicsItem *dragTarget = nullptr;
        for (auto *item : scene->items()) if (item->data(0) == "symbol" && item->data(1).toJsonObject().value("id") == "method") dragTarget = item;
        check(dragTarget, "drag fixture exists"); view->centerOn(dragTarget); app.processEvents();
        const QPoint dragStart = view->mapFromScene(dragTarget->sceneBoundingRect().center()), dragEnd = dragStart + QPoint(40, 0);
        QMouseEvent dragPress(QEvent::MouseButtonPress, QPointF(dragStart), QPointF(view->viewport()->mapToGlobal(dragStart)), Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(view->viewport(), &dragPress);
        for (const auto &point : {dragEnd, dragStart}) {
            QMouseEvent move(QEvent::MouseMove, QPointF(point), QPointF(view->viewport()->mapToGlobal(point)), Qt::NoButton, Qt::LeftButton, Qt::NoModifier); QApplication::sendEvent(view->viewport(), &move);
        }
        QMouseEvent dragRelease(QEvent::MouseButtonRelease, QPointF(dragStart), QPointF(view->viewport()->mapToGlobal(dragStart)), Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
        QApplication::sendEvent(view->viewport(), &dragRelease); check(!card->isVisible(), "drag out and back is not treated as a click");
        int focuses = 0; QObject::connect(&graph, &RelationGraph::focusRequested, &graph, [&](const QString &) { ++focuses; });
        clickAt(dragStart);
        QMouseEvent doubleClick(QEvent::MouseButtonDblClick, QPointF(dragStart), QPointF(view->viewport()->mapToGlobal(dragStart)), Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(view->viewport(), &doubleClick); QApplication::sendEvent(view->viewport(), &dragRelease); app.processEvents();
        check(!card->isVisible() && focuses > 0, "double-click retains graph focus behavior without leaving a card");
        hoverItem("symbol", "method"); graph.setData({}, {}, {}, {}); check(!card->isVisible(), "graph refresh clears stale card");
        check(requests.size() == beforeHover, "all canvas click routes reuse cache without HTTP requests");
        QToolTip::hideText();
        qInfo() << "PASS: file/class/method/variable/frame click, metadata-only hover, selectable card, dismissals and no-click-HTTP";
        check(!removeGenerated(root).ok, "active AI cache blocks project deletion before mutation");
        check(!ai.tooltip("b.hpp").isEmpty(), "blocked deletion preserves AI cache");
        ai.setProject({}, {}, false);
        VividCodeWindow window; check(window.openProject(root), "main window AI integration");
        check(window.findChild<QAction *>("aiExplainAction") && window.findChild<QAction *>("aiSettingsAction") && window.findChild<QAction *>("aiCancelAction"), "AI toolbar controls");
        window.show(); app.processEvents();
        if (!screenshot.isEmpty()) window.grab().save(screenshot + ".window.png");
        QTimer::singleShot(0, &window, [&] {
            auto *dialog = window.findChild<QDialog *>("aiSettingsDialog");
            if (!dialog) return;
            auto *audience = dialog->findChild<QComboBox *>("aiAudience");
            check(audience && audience->count() == 3 && audience->currentData() == "intermediate", "three audience choices default to intermediate");
            check(audience->itemText(0) == QStringLiteral("Beginner") && audience->itemText(1) == QStringLiteral("Intermediate") && audience->itemText(2) == QStringLiteral("Professional"), "audience choices show names only");
            if (!screenshot.isEmpty()) dialog->grab().save(screenshot + ".settings.png");
            dialog->reject();
        });
        window.findChild<QAction *>("aiSettingsAction")->trigger();
        auto *windowAi = window.findChild<AiExplanation *>();
        mode = "normal"; version = 5; write("b.hpp", "constexpr int value = 5;\n"); const int beforeClick = requests.size();
        window.findChild<QAction *>("aiExplainAction")->trigger(); waitFor([&] { return !windowAi->isBusy(); });
        check(requests.size() == beforeClick + 1 && windowAi->tooltip("b.hpp").contains("v5"), "toolbar click performs incremental API request");
        const QString cache = sharedHome() + "/ai/" + projectCacheKey(root);
        // Close the main window's cache before exercising CLI deletion.
        windowAi->setProject({}, {}, false);
        check(removeGenerated(root).ok && !QFileInfo::exists(cache) && QFileInfo::exists(sharedHome() + "/ai-settings.json"), "project deletion removes AI cache and preserves global settings");
        qInfo() << "PASS: canvas click analysis, audience settings, toolbar and project AI cache cleanup";
        return 0;
    } catch (const std::exception &error) { qCritical() << "FAIL: AI Analysis:" << error.what(); return 1; }
}
