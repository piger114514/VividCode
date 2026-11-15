#pragma once
#include "VividCode.h"
#include "SemanticPanel.h"
#include "UnifiedGraphStore.h"
#include "AnalysisEnvironment.h"
#include "UiTestHelpers.h"
#include <QtWidgets>

inline int runEnvironmentSmokeTest(QApplication &app) {
    const auto fail = [](int line) { qCritical() << "FAIL: local environment at EnvironmentSmokeTest.h" << line; return 1; };
    const bool javascript = app.arguments().contains("--javascript-only");
    const bool python = app.arguments().contains("--python-only");
    QTemporaryDir directory(QDir::tempPath() + QStringLiteral("/vividcode local env 中文-XXXXXX")); if (!directory.isValid()) return fail(__LINE__);
    const auto write = [&](const QString &name, const QByteArray &bytes) { QFile file(directory.filePath(name)); return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size(); };
    if (!write("lib.ts","export function helper(){return 2}") || !write("use.ts","import {helper} from './lib'; export function caller(){return helper()}") || !write("other.py","def pyCaller():\n    return 1\n")) return fail(__LINE__);
    if (!python) qputenv("VIVIDCODE_PYTHON", "C:/vividcode-missing/python.exe");
    else qputenv("VIVIDCODE_NODE", "C:/vividcode-missing/node.exe");
    qputenv("VIVIDCODE_DOTNET", "C:/vividcode-missing/dotnet.exe"); qputenv("VIVIDCODE_LIBCLANG", "C:/vividcode-missing/libclang.dll");
    VividCodeWindow window; window.show(); auto *panel = window.findChild<SemanticPanel *>(); if (!panel) return fail(__LINE__);
    const auto wait = [&] {
        if (!panel->isBusy()) return true; QEventLoop loop; QTimer timer; timer.setSingleShot(true);
        QObject::connect(&timer,&QTimer::timeout,&loop,&QEventLoop::quit); QObject::connect(panel,&SemanticPanel::settled,&loop,&QEventLoop::quit);
        timer.start(30000); loop.exec(); return timer.isActive() && !panel->isBusy();
    };
    if (!window.openProject(directory.path()) || !wait()) return fail(__LINE__);
    auto *files = window.findChild<QTreeWidget *>("fileTree"); const auto matches = files->findItems("lib.ts",Qt::MatchExactly|Qt::MatchRecursive);
    if (matches.isEmpty() || !triggerFileSource(files,matches.first()) || !window.findChild<QPlainTextEdit *>("sourceViewer")->toPlainText().contains("helper")) return fail(__LINE__);
    clickOutsidePopup(window); app.processEvents();
    const auto pythonFiles = files->findItems("other.py",Qt::MatchExactly|Qt::MatchRecursive);
    if (pythonFiles.isEmpty() || !triggerFileSource(files,pythonFiles.first()) || !window.findChild<QPlainTextEdit *>("sourceViewer")->toPlainText().contains("pyCaller")) return fail(__LINE__);
    clickOutsidePopup(window); app.processEvents();
    if (!QFileInfo::exists(directory.filePath(".vividcode/project.json"))) return fail(__LINE__);
    auto *messages = window.findChild<QPlainTextEdit *>("semanticDiagnostics");
    if (!python && !messages->toPlainText().contains(QStringLiteral("Python 暂不可分析"))) return fail(__LINE__);
    QMenu menu; panel->addContextActions(&menu);
    if (!menu.findChild<QAction *>("analysisEnvironmentAction")) return fail(__LINE__);
    if (python) {
        QFile config(VividCode::semanticDirectory(directory.path()) + "/python/config.json");
        if (!config.open(QIODevice::ReadOnly) || QJsonDocument::fromJson(config.readAll()).object().value("last_status") != "ok") { qCritical().noquote() << messages->toPlainText(); return fail(__LINE__); }
        UnifiedGraphStore store(directory.path());
        if (store.load().value("python").value("units").toArray().size() != 1) return fail(__LINE__);
        qInfo() << "PASS: Python-only component uses local Python without Node/.NET/LLVM or C++ helper files"; return 0;
    }
    if (!javascript) {
        if (!messages->toPlainText().contains(QStringLiteral("尚未安装 JavaScript/TypeScript"))) return fail(__LINE__);
        qInfo() << "PASS: core-only browses files and creates launcher without language environments; missing components isolated"; return 0;
    }
    const auto state = [&] { QFile file(VividCode::semanticDirectory(directory.path()) + "/javascript/config.json"); return file.open(QIODevice::ReadOnly) ? QJsonDocument::fromJson(file.readAll()).object() : QJsonObject(); };
    if (state().value("last_status") != "ok") { qCritical().noquote() << messages->toPlainText(); return fail(__LINE__); }
    UnifiedGraphStore store(directory.path()); const auto original = store.load().value("javascript").value("units"); if (original.toArray().size() != 2) return fail(__LINE__);
    const QString node = VividCode::localNode(); QString error;
    if (!VividCode::saveAnalysisEnvironment({{"node",node}},&error)) return fail(__LINE__);
    qunsetenv("VIVIDCODE_NODE");
    if (!window.openProject(directory.path()) || !wait() || state().value("reused").toInt() != 2 || state().value("node").toString() != node) return fail(__LINE__);
    if (!VividCode::saveAnalysisEnvironment({{"node","C:/vividcode-missing/node.exe"}},&error)) return fail(__LINE__);
    if (!window.openProject(directory.path()) || !wait() || state().value("last_status") != "failed" || store.load().value("javascript").value("units") != original) return fail(__LINE__);
    if (!VividCode::saveAnalysisEnvironment({{"node",node}},&error)) return fail(__LINE__);
    if (!window.openProject(directory.path()) || !wait() || state().value("last_status") != "ok") return fail(__LINE__);
    qInfo() << "PASS: JS component uses only local Node, no Python/.NET/LLVM; saved path, missing Node preserves graph, recovery";
    return 0;
}
