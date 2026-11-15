#pragma once
#include "VividCode.h"
#include "SemanticPanel.h"
#include "RelationGraph.h"
#include "ProjectStore.h"
#include "SharedRuntime.h"
#include "UnifiedGraphStore.h"
#include "UiTestHelpers.h"
#include "ProjectScanner.h"
#include <QtWidgets>

inline int runCSharpSmokeTest(QApplication &app) {
    const auto fail = [](int line) { qCritical() << "FAIL: C# UI check at CSharpSmokeTest.h line" << line; return 1; };
    QFontDatabase::addApplicationFont(qEnvironmentVariable("WINDIR", "C:/Windows") + "/Fonts/msyh.ttc");
    app.setFont(QFont("Microsoft YaHei", 10));
    QTemporaryDir dir(QDir::tempPath() + QStringLiteral("/vividcode C# 中文-XXXXXX"));
    if (!dir.isValid()) return fail(__LINE__);
    const auto write = [&](const QString &name, const QByteArray &bytes) {
        QFile file(dir.filePath(name)); return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
    };
    if (!write("Sample.csproj", "<Project Sdk=\"Microsoft.NET.Sdk\"><PropertyGroup><TargetFramework>net9.0</TargetFramework></PropertyGroup></Project>")) return fail(__LINE__);
    if (!write("Worker.cs", "namespace Sample;\npublic class Worker {\n    public int Count;\n    public int Pick(int x) => Count + x;\n}\n")) return fail(__LINE__);
    if (!write("Use.cs", "namespace Sample;\npublic class Use {\n    public int Run() { var w = new Worker(); return w.Pick(2); }\n}\n")) return fail(__LINE__);
    const QByteArray solution =
        "Microsoft Visual Studio Solution File, Format Version 12.00\n"
        "# Visual Studio Version 17\n"
        "Project(\"{9A19103F-16F7-4668-BE54-9A1E7A4F7556}\") = \"Sample\", \"Sample.csproj\", \"{29A420CF-5888-4DCA-B924-562D84B6B04C}\"\nEndProject\n"
        "Global\n\tGlobalSection(SolutionConfigurationPlatforms) = preSolution\n\t\tDebug|Any CPU = Debug|Any CPU\n\tEndGlobalSection\n"
        "\tGlobalSection(ProjectConfigurationPlatforms) = postSolution\n"
        "\t\t{29A420CF-5888-4DCA-B924-562D84B6B04C}.Debug|Any CPU.ActiveCfg = Debug|Any CPU\n"
        "\t\t{29A420CF-5888-4DCA-B924-562D84B6B04C}.Debug|Any CPU.Build.0 = Debug|Any CPU\n\tEndGlobalSection\nEndGlobal\n";
    if (!write("Sample.SLN", solution)) return fail(__LINE__);
    QDir(dir.path()).mkpath("nested");
    if (!write("nested/Ignored.sln", solution)) return fail(__LINE__);
    QProcess restore; restore.setProgram("dotnet"); restore.setArguments({"restore", dir.filePath("Sample.csproj"), "--ignore-failed-sources"}); restore.start();
    if (!restore.waitForFinished(45000) || restore.exitCode() != 0) { qCritical().noquote() << restore.readAllStandardOutput() << restore.readAllStandardError(); return fail(__LINE__); }
    VividCodeWindow window; window.show();
    auto *panel = window.findChild<SemanticPanel *>(); if (!panel) return fail(__LINE__);
    const auto wait = [&] {
        QEventLoop loop; QTimer timer; timer.setSingleShot(true);
        QObject::connect(&timer, &QTimer::timeout, &loop, &QEventLoop::quit);
        QObject::connect(panel, &SemanticPanel::settled, &loop, &QEventLoop::quit);
        timer.start(45000); loop.exec(); return timer.isActive() && !panel->isBusy();
    };
    if (!window.openProject(dir.path()) || !wait()) return fail(__LINE__);
    if (!panel->analysisLanguages().contains("c#") || window.findChild<QComboBox *>("semanticLanguage") || window.findChild<QComboBox *>("semanticFocus")) return fail(__LINE__);
    const auto configPath = VividCode::semanticDirectory(dir.path()) + "/csharp/config.json";
    const auto selectedProject = [&] {
        QFile file(configPath); if (!file.open(QIODevice::ReadOnly)) return QString();
        return QDir::fromNativeSeparators(QJsonDocument::fromJson(file.readAll()).object().value("project_file").toString());
    };
    if (selectedProject() != dir.filePath("Sample.SLN")) return fail(__LINE__);
    if (!window.findChild<QToolBar *>("analysisSettingsBar")->isVisible()) return fail(__LINE__);
    auto *symbols = window.findChild<QTreeWidget *>("semanticSymbols");
    auto *graph = window.findChild<RelationGraph *>();
    auto *scene = graph->findChild<QGraphicsScene *>("relationGraphScene");
    auto *source = window.findChild<QPlainTextEdit *>("sourceViewer");
    QString runId;
    for (QTreeWidgetItemIterator it(symbols); *it; ++it) {
        const auto s = (*it)->data(0, Qt::UserRole).toJsonObject();
        if (s.value("qualified_name") == "Sample.Use.Run") runId = s.value("id").toString();
    }
    if (runId.isEmpty()) { qCritical().noquote() << window.findChild<QLabel *>("semanticStatus")->text(); return fail(__LINE__); }
    panel->focusSymbol(runId); graph->setZoomLevel(1.0); if (!showGraphDetails(graph)) return fail(__LINE__);
    QGraphicsItem *edge = nullptr, *method = nullptr;
    for (auto *item : scene->items()) {
        if (item->data(0).toString() == "symbol" && item->data(1).toJsonObject().value("qualified_name") == "Sample.Worker.Pick") method = item;
    }
    if (!method) return fail(__LINE__);
    const auto target = method->data(1).toJsonObject().value("id");
    for (auto *item : scene->items()) {
        if (item->data(0).toString() != "relation") continue;
        const auto r = item->data(1).toJsonArray().first().toObject();
        if (r.value("type") == "calls" && r.value("target_id") == target) edge = item;
    }
    if (!edge) return fail(__LINE__);
    edge->setSelected(true);
    if (dynamic_cast<SourcePopup *>(window.findChild<QWidget *>("sourcePopup"))->isVisible()) return fail(__LINE__);
    if (!triggerGraphSource(graph, edge)) return fail(__LINE__);
    if (source->textCursor().blockNumber() != 2 || !source->textCursor().block().text().contains("w.Pick(2)")) { qCritical() << "source after menu" << source->toPlainText() << source->textCursor().blockNumber(); return fail(__LINE__); }
    scene->clearSelection(); method->setSelected(true);
    if (!triggerGraphSource(graph, method)) return fail(__LINE__);
    if (source->textCursor().blockNumber() != 3 || !source->textCursor().block().text().contains("int Pick(int x)")) return fail(__LINE__);
    app.processEvents();
    if (!dynamic_cast<SourcePopup *>(window.findChild<QWidget *>("sourcePopup"))->isVisible() || source->toPlainText().isEmpty()) return fail(__LINE__);
    if (!dynamic_cast<SourcePopup *>(window.findChild<QWidget *>("sourcePopup"))->grab().save(QCoreApplication::applicationDirPath() + "/source-popup-preview.png")) return fail(__LINE__);
    clickOutsidePopup(window);
    if (dynamic_cast<SourcePopup *>(window.findChild<QWidget *>("sourcePopup"))->isVisible()) return fail(__LINE__);
    if (!window.grab().save(QCoreApplication::applicationDirPath() + "/csharp-relation-preview.png")) return fail(__LINE__);
    const auto cachedPath = VividCode::semanticDirectory(dir.path()) + "/csharp/index.json";
    QFile index(cachedPath); if (!index.open(QIODevice::ReadOnly)) return fail(__LINE__);
    index.close();
    // With a cache for Sample.SLN, a second solution still requires a choice.
    if (!write("Alternative.sln", solution)) return fail(__LINE__);
    bool choseSolution = false;
    const auto chooseSolution = [&] {
        auto *dialog = window.findChild<QDialog *>("analysisProjectDialog");
        auto *list = dialog ? dialog->findChild<QListWidget *>("analysisProjectList") : nullptr;
        if (!list || list->count() != 2) { if (dialog) dialog->reject(); return; }
        dialog->grab().save(QCoreApplication::applicationDirPath() + "/csharp-solution-preview.png");
        auto *item = list->item(0);
        if (!item->text().endsWith("Alternative.sln")) { dialog->reject(); return; }
        const QPoint pos = list->visualItemRect(item).center();
        QMouseEvent press(QEvent::MouseButtonPress, QPointF(pos), QPointF(list->viewport()->mapToGlobal(pos)), Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        QMouseEvent release(QEvent::MouseButtonRelease, QPointF(pos), QPointF(list->viewport()->mapToGlobal(pos)), Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
        QApplication::sendEvent(list->viewport(), &press); QApplication::sendEvent(list->viewport(), &release);
        choseSolution = dialog->result() == QDialog::Accepted;
    };
    panel->setProject({});
    QTimer::singleShot(0, panel, chooseSolution);
    if (!window.openProject(dir.path()) || !choseSolution || !wait() || selectedProject() != dir.filePath("Alternative.sln")) return fail(__LINE__);
    // Same-project F5 refresh uses the chosen solution without another popup.
    bool unexpectedDialog = false;
    QTimer::singleShot(0, panel, [&] {
        if (auto *dialog = window.findChild<QDialog *>("analysisProjectDialog")) { unexpectedDialog = true; dialog->reject(); }
    });
    auto *refresh = window.findChild<QAction *>("refreshProjectAction");
    if (!refresh || refresh->shortcut() != QKeySequence::Refresh) return fail(__LINE__);
    refresh->trigger();
    if (!wait() || unexpectedDialog || selectedProject() != dir.filePath("Alternative.sln")) return fail(__LINE__);
    panel->setProject({});
    QTimer::singleShot(0, panel, [&] {
        if (auto *dialog = window.findChild<QDialog *>("analysisProjectDialog")) dialog->reject();
    });
    window.openProject(dir.path());
    if (panel->isBusy() || !window.findChild<QLabel *>("semanticStatus")->text().contains(QStringLiteral("Project selection cancelled")) || selectedProject() != dir.filePath("Alternative.sln")) return fail(__LINE__);
    choseSolution = false;
    QTimer::singleShot(0, panel, chooseSolution);
    refresh->trigger();
    if (!choseSolution || !wait()) return fail(__LINE__);
    // A real C++ compilation database and C# solution are analyzed in one open.
    if (!write("native.cpp", "struct Native { int value; int Read() { return value; } };\nint runNative() { Native n{}; return n.Read(); }\n")) return fail(__LINE__);
    const QJsonArray commands{QJsonObject{{"directory", dir.path()}, {"file", "native.cpp"}, {"arguments", QJsonArray{"clang++", "-std=c++17", "-c", "native.cpp"}}}};
    if (!write("compile_commands.json", QJsonDocument(commands).toJson()) || !window.openProject(dir.path()) || !wait()) return fail(__LINE__);
    if (panel->analysisLanguages().size() != 2) return fail(__LINE__);
    {
        UnifiedGraphStore store(dir.path()); const auto snapshots = store.load();
        if (snapshots.size() != 2 || snapshots.value("c#").value("units").toArray().isEmpty() || snapshots.value("c++").value("units").toArray().isEmpty()) return fail(__LINE__);
    }
    QMap<QString,QRectF> partitions;
    for (auto *item : scene->items()) if (item->data(0) == "languagePartition") partitions[item->data(1).toString()] = item->sceneBoundingRect();
    if (partitions.size() != 2 || partitions.value("c#").intersects(partitions.value("c++"))) return fail(__LINE__);
    if (!window.grab().save(QCoreApplication::applicationDirPath() + "/mixed-language-preview.png")) return fail(__LINE__);
    // Source files from both languages share the sidebar.
    QSet<QString> sidebarFiles;
    for (int i = 0; i < symbols->topLevelItemCount(); ++i) sidebarFiles.insert(symbols->topLevelItem(i)->text(0));
    if (!sidebarFiles.contains("Worker.cs") || !sidebarFiles.contains("Use.cs") || !sidebarFiles.contains("native.cpp")) return fail(__LINE__);
    window.findChild<QComboBox *>("csharpConfiguration")->setCurrentText("Release");
    if (!wait()) return fail(__LINE__);
    {
        UnifiedGraphStore store(dir.path());
        if (store.load().value("c#").value("state").toObject().value("configuration") != "Release") return fail(__LINE__);
    }
    // Compare against the latest complete index after the solution switch.
    if (!index.open(QIODevice::ReadOnly)) return fail(__LINE__);
    const auto latest = index.readAll(); index.close();
    write("Use.cs", "invalid C#;"); panel->setProject(dir.path()); if (!wait()) return fail(__LINE__);
    const auto failureStatus = window.findChild<QLabel *>("semanticStatus")->text();
    if (!failureStatus.contains("failed")) { qCritical().noquote() << "Expected failure status:" << failureStatus << window.findChild<QPlainTextEdit *>("semanticDiagnostics")->toPlainText(); return fail(__LINE__); }
    if (!index.open(QIODevice::ReadOnly) || index.readAll() != latest) { qCritical() << "Failed refresh changed the previous complete C# index"; return fail(__LINE__); }
    index.close();
    // A failed C# refresh must not discard the successful C++ result or shared C# snapshot.
    { UnifiedGraphStore store(dir.path()); if (store.load().size() != 2) return fail(__LINE__); }
    QTemporaryDir empty; window.openProject(empty.path()); app.processEvents();
    if (panel->isBusy() || symbols->topLevelItemCount() || !scene->items().isEmpty()) return fail(__LINE__);
    if (!VividCode::removeGenerated(dir.path()).ok || QFileInfo::exists(cachedPath) || QFileInfo::exists(VividCode::semanticDirectory(dir.path()) + "/graph.db")) return fail(__LINE__);
    qInfo() << "PASS: automatic root solution discovery, multiple-entry mouse selection, cancel/retry, refresh, context source popup/outside dismissal, mixed C++ and C# real analysis, shared SQLite snapshots, separate graph partitions, auto configuration update, failure preservation and cache lifecycle";
    return 0;
}

// Explicit real-project acceptance entry; caller supplies a Unity root.
inline int runUnitySmokeTest(QApplication &app, const QString &root) {
    const auto fail = [](int line) { qCritical() << "FAIL: Unity UI check at CSharpSmokeTest.h line" << line; return 1; };
    QFontDatabase::addApplicationFont(qEnvironmentVariable("WINDIR", "C:/Windows") + "/Fonts/msyh.ttc");
    app.setFont(QFont("Microsoft YaHei", 10));
    const auto scan = scanProject(root);
    if (scan.truncated || scan.files.isEmpty()) return fail(__LINE__);
    for (const auto &file : scan.files) if (file.startsWith("Library/", Qt::CaseInsensitive) || file.startsWith("Temp/", Qt::CaseInsensitive)) return fail(__LINE__);
    VividCodeWindow window; window.show();
    auto *panel = window.findChild<SemanticPanel *>();
    QEventLoop loop; QTimer timer; timer.setSingleShot(true);
    QObject::connect(&timer, &QTimer::timeout, &loop, &QEventLoop::quit);
    QObject::connect(panel, &SemanticPanel::settled, &loop, &QEventLoop::quit);
    if (!window.openProject(root)) return fail(__LINE__);
    timer.start(90000); loop.exec();
    if (!timer.isActive() || panel->isBusy()) return fail(__LINE__);
    UnifiedGraphStore store(root); const auto snapshot = store.load().value("c#");
    if (snapshot.value("state").toObject().value("last_status") != "ok" || snapshot.value("state").toObject().value("project_kind") != "unity") {
        qCritical().noquote() << window.findChild<QPlainTextEdit *>("semanticDiagnostics")->toPlainText(); return fail(__LINE__);
    }
    auto *symbols = window.findChild<QTreeWidget *>("semanticSymbols");
    QMap<QString,QJsonObject> byId;
    for (QTreeWidgetItemIterator it(symbols); *it; ++it) { const auto s = (*it)->data(0, Qt::UserRole).toJsonObject(); if (!s.value("id").toString().isEmpty()) byId[s.value("id").toString()] = s; }
    QJsonObject chosen;
    auto *relations = window.findChild<QTreeWidget *>("semanticRelations");
    for (QTreeWidgetItemIterator it(relations); *it; ++it) {
        const auto r = (*it)->data(0, Qt::UserRole).toJsonObject();
        if (r.value("type") != "calls" || !byId.contains(r.value("source_id").toString()) || !byId.contains(r.value("target_id").toString())) continue;
        if (chosen.isEmpty() || byId.value(r.value("source_id").toString()).value("qualified_name") == "PlayerController.Update") chosen = r;
    }
    if (chosen.isEmpty()) return fail(__LINE__);
    auto *graph = window.findChild<RelationGraph *>(); auto *scene = graph->findChild<QGraphicsScene *>("relationGraphScene");
    if (!window.grab().save(QCoreApplication::applicationDirPath() + "/unity-overview-preview.png")) return fail(__LINE__);
    panel->focusSymbol(chosen.value("source_id").toString()); graph->setZoomLevel(1.0); if (!showGraphDetails(graph)) return fail(__LINE__);
    QGraphicsItem *edge = nullptr, *method = nullptr;
    for (auto *item : scene->items()) {
        if (item->data(0) == "symbol" && item->data(1).toJsonObject().value("id") == chosen.value("target_id")) method = item;
        if (item->data(0) == "relation") for (const auto &r : item->data(1).toJsonArray()) if (r.toObject() == chosen) edge = item;
    }
    auto *source = window.findChild<QPlainTextEdit *>("sourceViewer");
    if (!edge || !method || !triggerGraphSource(graph, edge) || source->textCursor().blockNumber() != chosen.value("line").toInt() - 1) return fail(__LINE__);
    const auto target = byId.value(chosen.value("target_id").toString()); const auto prefix = target.value("def_file").toString().isEmpty() ? "decl_" : "def_";
    if (!triggerGraphSource(graph, method) || source->textCursor().blockNumber() != target.value(QString(prefix) + "line").toInt() - 1) return fail(__LINE__);
    clickOutsidePopup(window); app.processEvents();
    if (!window.grab().save(QCoreApplication::applicationDirPath() + "/unity-relation-preview.png")) return fail(__LINE__);
    qInfo().noquote() << "PASS: Unity project" << root << "files" << scan.files.size() << "source" << byId.value(chosen.value("source_id").toString()).value("qualified_name").toString()
        << "calls" << target.value("qualified_name").toString() << "at" << chosen.value("file").toString() << chosen.value("line").toInt() << "; graph.db, graph edge and definition/source navigation verified";
    return 0;
}
