#pragma once
#include "VividCode.h"
#include "SemanticPanel.h"
#include "UnifiedGraphStore.h"
#include "UiTestHelpers.h"
#include <QtWidgets>

inline int runJavaScriptSmokeTest(QApplication &app) {
    const auto fail = [](int line) { qCritical() << "FAIL: JavaScript UI at JavaScriptSmokeTest.h" << line; return 1; };
    QFontDatabase::addApplicationFont(qEnvironmentVariable("WINDIR", "C:/Windows") + "/Fonts/msyh.ttc");
    app.setFont(QFont("Microsoft YaHei", 10));
    QTemporaryDir directory(QDir::tempPath() + QStringLiteral("/vividcode JS 中文-XXXXXX"));
    if (!directory.isValid()) return fail(__LINE__);
    const auto write = [&](const QString &name, const QByteArray &bytes) {
        QFile file(directory.filePath(name)); return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
    };
    if (!write("worker.ts", "export function helper(x:number){return x*2}\nexport class Worker { run(){return helper(2)} }\n")) return fail(__LINE__);
    if (!write("use.ts", "import {helper} from './worker';\nexport function caller(){\n  return helper(3);\n}\n")) return fail(__LINE__);
    if (!write("view.tsx", "import {helper} from './worker';\nexport const App=()=> <div>{helper(4)}</div>;\n")) return fail(__LINE__);
    VividCodeWindow window; window.show();
    auto *panel = window.findChild<SemanticPanel *>();
    if (!panel) return fail(__LINE__);
    const auto wait = [&] {
        if (!panel->isBusy()) return true;
        QEventLoop loop; QTimer timer; timer.setSingleShot(true);
        QObject::connect(&timer, &QTimer::timeout, &loop, &QEventLoop::quit);
        QObject::connect(panel, &SemanticPanel::settled, &loop, &QEventLoop::quit);
        timer.start(45000); loop.exec(); return timer.isActive() && !panel->isBusy();
    };
    const auto state = [&] {
        QFile file(VividCode::semanticDirectory(directory.path()) + "/javascript/config.json");
        return file.open(QIODevice::ReadOnly) ? QJsonDocument::fromJson(file.readAll()).object() : QJsonObject();
    };
    if (!window.openProject(directory.path()) || !wait() || state().value("last_status") != "ok") {
        qCritical().noquote() << window.findChild<QPlainTextEdit *>("semanticDiagnostics")->toPlainText(); return fail(__LINE__);
    }
    if (!panel->analysisLanguages().contains("javascript") || window.findChild<QComboBox *>("csharpConfiguration")->isVisible()) return fail(__LINE__);
    auto *symbols = window.findChild<QTreeWidget *>("semanticSymbols");
    auto *graph = window.findChild<RelationGraph *>();
    auto *scene = graph->findChild<QGraphicsScene *>("relationGraphScene");
    auto *source = window.findChild<QPlainTextEdit *>("sourceViewer");
    const auto symbolId = [&](const QString &name) {
        for (QTreeWidgetItemIterator it(symbols); *it; ++it) {
            const auto s = (*it)->data(0, Qt::UserRole).toJsonObject();
            if (s.value("qualified_name") == name) return s.value("id").toString();
        }
        return QString();
    };
    const QString caller = symbolId("caller"), helper = symbolId("helper");
    if (caller.isEmpty() || helper.isEmpty() || symbolId("Worker.run").isEmpty() || symbolId("App").isEmpty()) return fail(__LINE__);
    panel->focusSymbol(caller); graph->setZoomLevel(1.0); if (!showGraphDetails(graph)) return fail(__LINE__);
    QGraphicsItem *edge = nullptr, *definition = nullptr;
    for (auto *item : scene->items()) {
        if (item->data(0).toString() == "symbol" && item->data(1).toJsonObject().value("id") == helper) definition = item;
        if (item->data(0).toString() == "relation") for (const auto &value : item->data(1).toJsonArray()) {
            const auto r = value.toObject();
            if (r.value("source_id") == caller && r.value("target_id") == helper && r.value("type") == "calls") edge = item;
        }
    }
    if (!edge || !definition || !triggerGraphSource(graph, edge)) return fail(__LINE__);
    if (source->textCursor().blockNumber() != 2 || !source->textCursor().block().text().contains("helper(3)")) return fail(__LINE__);
    if (!triggerGraphSource(graph, definition) || source->textCursor().blockNumber() != 0 || !source->toPlainText().startsWith("export function helper")) return fail(__LINE__);
    clickOutsidePopup(window); app.processEvents();
    if (!window.openProject(directory.path()) || !wait() || state().value("reused").toInt() != 3) return fail(__LINE__);
    UnifiedGraphStore store(directory.path()); const auto original = store.load().value("javascript").value("units");
    if (!write("use.ts", "export function broken( {")) return fail(__LINE__);
    if (!window.openProject(directory.path()) || !wait() || state().value("last_status") != "failed") return fail(__LINE__);
    if (symbolId("caller").isEmpty() || store.load().value("javascript").value("units") != original) return fail(__LINE__);
    if (!write("use.ts", "import {helper} from './worker'; export const changed=()=>helper(5);")) return fail(__LINE__);
    if (!write("other.py", "def helperPython():\n    return 1\ndef callerPython():\n    return helperPython()\n")) return fail(__LINE__);
    if (!window.openProject(directory.path()) || !wait() || symbolId("changed").isEmpty() || !symbolId("caller").isEmpty()) return fail(__LINE__);
    if (store.load().size() != 2 || !panel->analysisLanguages().contains("python")) return fail(__LINE__);
    bool jsLabel = false, pythonLabel = false;
    for (auto *item : scene->items()) if (auto *label = dynamic_cast<QGraphicsSimpleTextItem *>(item)) {
        jsLabel |= label->text() == "JavaScript/TypeScript"; pythonLabel |= label->text() == "Python";
    }
    if (!jsLabel || !pythonLabel) return fail(__LINE__);
    panel->focusSymbol({}); graph->setZoomLevel(1.0); if (!showGraphDetails(graph)) return fail(__LINE__); app.processEvents();
    auto *view = graph->findChild<QGraphicsView *>("relationGraphView");
    view->fitInView(scene->itemsBoundingRect().adjusted(-30,-30,30,30), Qt::KeepAspectRatio);
    if (!window.grab().save(QCoreApplication::applicationDirPath() + "/javascript-relation-preview.png")) return fail(__LINE__);
    const QString encoded = QStringLiteral("export function 中文(){ return 1; }\n");
    QByteArray bytes("\xff\xfe", 2); bytes.append(reinterpret_cast<const char *>(encoded.utf16()), encoded.size() * 2);
    if (!write("encoded.mts", bytes) || !window.openProject(directory.path()) || !wait()) return fail(__LINE__);
    auto *files = window.findChild<QTreeWidget *>("fileTree");
    const auto matches = files->findItems("encoded.mts", Qt::MatchExactly | Qt::MatchRecursive);
    if (matches.isEmpty() || !triggerFileSource(files, matches.first()) || !source->toPlainText().contains(QStringLiteral("中文"))) return fail(__LINE__);
    for (const auto &name : {"worker.ts", "use.ts", "view.tsx", "encoded.mts"}) if (!QFile::remove(directory.filePath(name))) return fail(__LINE__);
    if (!window.openProject(directory.path()) || !wait() || state().value("total").toInt(-1) != 0) return fail(__LINE__);
    if (!store.load().value("javascript").value("units").toArray().isEmpty() || store.load().value("python").value("units").toArray().isEmpty()) return fail(__LINE__);
    qInfo() << "PASS: JS/TS detection, class/arrow/TSX graph, cross-file definition/call navigation, cache reuse, failure preservation, Python mixed canvas, UTF16 source, final-file deletion";
    return 0;
}
