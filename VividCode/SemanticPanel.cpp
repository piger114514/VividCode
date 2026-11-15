#include "UiLanguage.h"
#include "SemanticPanel.h"
#include "ProjectStore.h"
#include "UnifiedGraphStore.h"
#include "RelationGraph.h"
#include <QtWidgets>
#include "AnalysisEnvironment.h"

namespace {
QString location(const QJsonObject &s) {
    const QString prefix = s.value("def_file").toString().isEmpty() ? "decl_" : "def_";
    return s.value(prefix + "file").toString() + ':' + QString::number(s.value(prefix + "line").toInt());
}
QString languageName(const QString &language) { return language == "java" ? "Java" : language == "javascript" ? "JavaScript/TypeScript" : language == "python" ? "Python" : language == "c#" ? "C#" : "C++"; }
QJsonObject stateAt(const QString &root, const QString &language) {
    QFile file(VividCode::semanticDirectory(root) + (language == "java" ? "/java/config.json" : language == "javascript" ? "/javascript/config.json" : language == "python" ? "/python/config.json" : language == "c#" ? "/csharp/config.json" : "/config.json"));
    return file.open(QIODevice::ReadOnly) ? QJsonDocument::fromJson(file.readAll()).object() : QJsonObject();
}
}

SemanticPanel::SemanticPanel(QWidget *parent) : QWidget(parent) {
    auto *layout = new QVBoxLayout(this); layout->setContentsMargins(0, 0, 0, 0);
    controls = new QWidget(this); controls->setObjectName("analysisControls");
    auto *row = new QHBoxLayout(controls); row->setContentsMargins(0, 0, 0, 0);
    relationType = new QComboBox; relationType->setObjectName("semanticRelationType"); relationType->addItem(UiLanguage::text("All relation types"), "");
    for (const QString &type : {"calls", "possible_call", "inherits", "implements", "member_ref", "references", "overrides"}) relationType->addItem(relationDisplayName(type), type);
    row->addWidget(relationType);
    buildConfiguration = new QComboBox; buildConfiguration->setObjectName("csharpConfiguration"); buildConfiguration->addItems({"Debug", "Release"}); row->addWidget(buildConfiguration);
    targetFramework = new QLineEdit; targetFramework->setObjectName("csharpFramework"); targetFramework->setPlaceholderText(UiLanguage::text("Target framework (blank for project default)")); row->addWidget(targetFramework, 1);
    status = new QLabel(UiLanguage::text("Open a project to detect its configuration. Right-click to refresh or import configuration.")); status->setObjectName("semanticStatus"); status->setWordWrap(true); status->setTextFormat(Qt::PlainText); layout->addWidget(status);
    units = new QComboBox; units->setObjectName("semanticUnits"); layout->addWidget(units);
    graph = new RelationGraph; graph->setObjectName("realRelationGraph"); layout->addWidget(graph, 1);
    diagnostics = new QPlainTextEdit; diagnostics->setObjectName("semanticDiagnostics"); diagnostics->setReadOnly(true); diagnostics->setMaximumHeight(100); diagnostics->hide(); layout->addWidget(diagnostics);
    structure = new QWidget(this); structure->setObjectName("structureRelationsView");
    auto *sl = new QVBoxLayout(structure); sl->setContentsMargins(0, 0, 0, 0);
    auto *all = new QPushButton(UiLanguage::text("Show All Relations")); sl->addWidget(all);
    auto *splitter = new QSplitter(Qt::Vertical); sl->addWidget(splitter, 1);
    symbols = new QTreeWidget; symbols->setObjectName("semanticSymbols"); symbols->setHeaderLabels({UiLanguage::text("File / Class / Function / Variable"), UiLanguage::text("Type"), UiLanguage::text("Source location")}); symbols->setColumnWidth(0, 250); symbols->setColumnHidden(2, true); splitter->addWidget(symbols);
    symbols->setToolTip(UiLanguage::text("Symbols grouped by file: classes, functions and variables. Double-click a name to show its related files on the graph; right-click to view source."));
    relations = new QTreeWidget; relations->setObjectName("semanticRelations"); relations->setHeaderLabels({UiLanguage::text("Start"), UiLanguage::text("Relation"), UiLanguage::text("End"), UiLanguage::text("Occurrence")}); splitter->addWidget(relations); splitter->setSizes({450, 230});
    connect(diagnostics, &QPlainTextEdit::textChanged, this, [this] { diagnostics->setVisible(!diagnostics->toPlainText().isEmpty()); });
    connect(units, &QComboBox::currentIndexChanged, this, &SemanticPanel::renderUnit);
    connect(relationType, &QComboBox::currentIndexChanged, this, &SemanticPanel::renderRelations);
    connect(buildConfiguration, &QComboBox::currentIndexChanged, this, [this] { if (!root.isEmpty() && !isBusy()) emit refreshRequested(); });
    connect(targetFramework, &QLineEdit::editingFinished, this, [this] {
        if (root.isEmpty() || isBusy() || !selectedEntries.contains("c#")) return;
        if (targetFramework->text().trimmed() != languageSnapshots.value("c#").value("state").toObject().value("framework").toString()) emit refreshRequested();
    });
    connect(all, &QPushButton::clicked, this, [this] { focusSymbol({}); });
    connect(graph, &RelationGraph::sourceRequested, this, &SemanticPanel::sourceRequested);
    connect(graph, &RelationGraph::focusRequested, this, &SemanticPanel::focusSymbol);
    connect(graph, &RelationGraph::refreshRequested, this, &SemanticPanel::refreshRequested);
    connect(graph, &RelationGraph::contextMenuRequested, this, &SemanticPanel::addContextActions);
    connect(symbols, &QTreeWidget::currentItemChanged, this, [this](QTreeWidgetItem *item) {
        if (!item) return; pendingFileFocus.clear(); symbolFilesMode = false; selectedSymbol = item->data(0, Qt::UserRole).toJsonObject().value("id").toString();
        { const QSignalBlocker blocker(units); units->setCurrentIndex(0); } renderRelations();
    });
    connect(symbols, &QTreeWidget::itemDoubleClicked, this, [this](QTreeWidgetItem *item) {
        const auto id = item->data(0, Qt::UserRole).toJsonObject().value("id").toString();
        if (!id.isEmpty()) showSymbolFiles(id);
    });
    for (auto *tree : {symbols, relations}) {
        tree->setContextMenuPolicy(Qt::CustomContextMenu);
        connect(tree, &QTreeWidget::customContextMenuRequested, this, [this, tree](const QPoint &point) {
            auto *menu = new QMenu(tree); menu->setObjectName("structureContextMenu"); menu->setAttribute(Qt::WA_DeleteOnClose);
            if (auto *item = tree->itemAt(point)) {
                const auto value = item->data(0, Qt::UserRole).toJsonObject();
                const QString prefix = tree == relations ? QString() : value.value("def_file").toString().isEmpty() ? "decl_" : "def_";
                if (!value.value(prefix + "file").toString().isEmpty()) {
                    auto *source = menu->addAction(UiLanguage::text("View Source"));
                    source->setObjectName("structureViewSourceAction");
                    connect(source, &QAction::triggered, this, [this, value, prefix] { emit sourceRequested(value.value(prefix + "file").toString(), value.value(prefix + "line").toInt(1), value.value(prefix + "col").toInt(1)); });
                }
            }
            auto *refresh = menu->addAction(UiLanguage::text("Refresh Files and Index")); connect(refresh, &QAction::triggered, this, &SemanticPanel::refreshRequested);
            addContextActions(menu); menu->popup(tree->viewport()->mapToGlobal(point));
        });
    }
    connect(&process, &QProcess::readyReadStandardOutput, this, [this] {
        output += process.readAllStandardOutput();
        if (output.size() > 128 * 1024 * 1024) { diagnostics->appendPlainText(UiLanguage::text("Analysis results exceed the 128 MiB display limit.")); process.kill(); }
        if (command == "index") while (output.contains('\n')) {
            const auto line = output.left(output.indexOf('\n')); output.remove(0, line.size() + 1);
            const auto obj = QJsonDocument::fromJson(line).object();
            if (obj.value("type") == "progress") setStatus(UiLanguage::text("%1: processing %2/%3 - %4").arg(languageName(workerLanguage)).arg(obj.value("current").toInt()).arg(obj.value("total").toInt()).arg(obj.value("file").toString()));
            if (obj.contains("message")) diagnostics->appendPlainText(obj.value("message").toString());
        }
    });
    connect(&process, &QProcess::readyReadStandardError, this, [this] { diagnostics->appendPlainText(QString::fromUtf8(process.readAllStandardError())); });
    connect(&process, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (stopping || error != QProcess::FailedToStart) return;
        diagnostics->appendPlainText(languageName(workerLanguage) + UiLanguage::text(" analyzer could not start: ") + process.errorString()); beginNextLanguage();
    });
    connect(&process, qOverload<int,QProcess::ExitStatus>(&QProcess::finished), this, [this](int code, QProcess::ExitStatus exitStatus) {
        if (stopping) return;
        if (command == "index") { loadCache(false); return; }
        if (code == 0 && exitStatus == QProcess::NormalExit) {
            QJsonParseError error; const auto doc = QJsonDocument::fromJson(output, &error);
            if (error.error == QJsonParseError::NoError && doc.object().value("ok").toBool()) {
                languageSnapshots[workerLanguage] = doc.object();
                if (!refreshAfterLoad && doc.object().value("state").toObject().value("last_status") == "ok") {
                    UnifiedGraphStore store(root);
                    if (!store.saveLanguage(workerLanguage, doc.object())) diagnostics->appendPlainText(UiLanguage::text("Could not save the shared graph database: ") + store.error);
                    else languageSnapshots[workerLanguage] = store.load().value(workerLanguage);
                }
                render();
            } else diagnostics->appendPlainText(UiLanguage::text("Invalid analysis results: ") + error.errorString());
        } else diagnostics->appendPlainText(UiLanguage::text("Could not read analysis results: ") + QString::fromUtf8(output));
        if (refreshAfterLoad && !cancelled) { refreshAfterLoad = false; refreshIndex(); return; }
        refreshAfterLoad = false;
        if (cancelled) { pendingLanguages.clear(); setStatus(UiLanguage::text("Cancelled. Previous analysis results retained.")); cancelled = false; }
        beginNextLanguage();
    });
    updateControls(false);
}
SemanticPanel::~SemanticPanel() { stop(); }
bool SemanticPanel::isBusy() const { return working || process.state() != QProcess::NotRunning; }
void SemanticPanel::stop() {
    stopping = true; refreshAfterLoad = false; pendingLanguages.clear(); working = false;
    if (process.state() != QProcess::NotRunning) { process.kill(); process.waitForFinished(1500); }
    stopping = false;
}
bool SemanticPanel::isCSharp() const { return workerLanguage == "c#"; }
void SemanticPanel::updateControls(bool busy) {
    const bool cs = selectedEntries.contains("c#"); buildConfiguration->setVisible(cs); targetFramework->setVisible(cs); buildConfiguration->setEnabled(!busy); targetFramework->setEnabled(!busy);
}
AnalysisEntry SemanticPanel::selectEntry(const QList<AnalysisEntry> &entries, const QString &language) {
    if (entries.size() == 1) return entries.first();
    QDialog dialog(this); dialog.setObjectName("analysisProjectDialog"); dialog.setWindowTitle(UiLanguage::text("vividcode - Select %1 Project").arg(languageName(language))); dialog.resize(600, 320);
    auto *layout = new QVBoxLayout(&dialog); layout->addWidget(new QLabel(UiLanguage::text("Multiple %1 project entries found. Select the project to analyze:").arg(languageName(language))));
    auto *list = new QListWidget; list->setObjectName("analysisProjectList");
    for (int index = 0; index < entries.size(); ++index) { auto *item = new QListWidgetItem(entries[index].label, list); item->setData(Qt::UserRole, index); item->setToolTip(entries[index].path); }
    layout->addWidget(list); auto *buttons = new QDialogButtonBox(QDialogButtonBox::Cancel); buttons->button(QDialogButtonBox::Cancel)->setText(UiLanguage::text("Cancel")); layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject); connect(list, &QListWidget::itemClicked, &dialog, &QDialog::accept); connect(list, &QListWidget::itemActivated, &dialog, &QDialog::accept);
    if (dialog.exec() != QDialog::Accepted || !list->currentItem()) return {};
    return entries[list->currentItem()->data(Qt::UserRole).toInt()];
}
void SemanticPanel::setProject(const QString &path, const QStringList &sources, bool scanComplete) {
    pendingFileFocus.clear();
    const bool changed = root != path; stop(); root = path; compilation.clear(); selectedSymbol.clear(); symbolFilesMode = false; diagnostics->clear();
    if (changed || !sources.isEmpty()) knownSources = sources;
    if (changed) { selectedEntries.clear(); languageSnapshots.clear(); }
    if (root.isEmpty()) { selectedEntries.clear(); languageSnapshots.clear(); render(); updateControls(false); return; }
    if (changed) {
        UnifiedGraphStore store(root); languageSnapshots = store.load(); const auto cs = stateAt(root, "c#");
        const QSignalBlocker cb(buildConfiguration), fb(targetFramework); buildConfiguration->setCurrentText(cs.value("configuration").toString("Debug")); targetFramework->setText(cs.value("framework").toString());
    }
    if (!scanComplete) {
        render(); updateControls(false);
        setStatus(UiLanguage::text("File scan reached its limit. Analysis skipped; previous successful cache retained. Adjust the global limit with vividcode -c or use -sg with care."));
        emit settled(); return;
    }
    QMap<QString,QList<AnalysisEntry>> grouped;
    for (const auto &entry : detectAnalysisEntries(root, knownSources)) grouped[entry.language].append(entry);
    bool cancelledSelection = false;
    for (auto it = selectedEntries.begin(); it != selectedEntries.end();) {
        const bool discoveredConfiguration = it.value().path.isEmpty() && grouped.contains(it.key()) && !grouped.value(it.key()).first().path.isEmpty();
        if (!grouped.contains(it.key()) || discoveredConfiguration || (!it.value().path.isEmpty() && !QFileInfo::exists(it.value().path))) it = selectedEntries.erase(it); else ++it;
    }
    for (auto it = grouped.begin(); it != grouped.end(); ++it) {
        if (!selectedEntries.contains(it.key())) { const auto chosen = selectEntry(it.value(), it.key()); if (chosen.language.isEmpty()) { cancelledSelection = true; continue; } selectedEntries[it.key()] = chosen; }
        if (!selectedEntries.value(it.key()).path.isEmpty()) pendingLanguages.append(it.key());
    }
    render(); updateControls(!pendingLanguages.isEmpty());
    if (pendingLanguages.isEmpty()) {
        const bool unity = QDir(root + "/Assets").exists() && QFileInfo::exists(root + "/ProjectSettings/ProjectVersion.txt");
        setStatus(cancelledSelection ? UiLanguage::text("Project selection cancelled. Right-click to refresh and select again.") : grouped.isEmpty() ? UiLanguage::text("Only file browsing is supported for this language. Right-click a file to view source.") : unity ? UiLanguage::text("Unity project detected. Regenerate .sln / .csproj in Unity, wait for script compilation, then press F5. You can also right-click to import project configuration.") : UiLanguage::text("Language detected, but no analyzable project configuration found. Right-click to import .sln / .csproj or compile_commands.json.")); emit settled(); return;
    }
    working = true; beginNextLanguage();
}
void SemanticPanel::beginNextLanguage() {
    if (pendingLanguages.isEmpty()) { working = false; updateControls(false); emit settled(); return; }
    working = true; workerLanguage = pendingLanguages.takeFirst(); compilation = selectedEntries.value(workerLanguage).path;
    const auto state = stateAt(root, workerLanguage); const QString previous = state.value(workerLanguage == "java" || workerLanguage == "python" || workerLanguage == "javascript" ? "project_root" : isCSharp() ? "project_file" : "compile_commands").toString();
    if (!previous.isEmpty() && QFileInfo(previous).absoluteFilePath() == compilation) { loadCache(true); return; } refreshIndex();
}
void SemanticPanel::refreshIndex() {
    if (compilation.isEmpty()) { beginNextLanguage(); return; }
    if (!QFileInfo::exists(root + "/.vividcode/project.json")) { const auto result = VividCode::generate(root); if (!result.ok) { diagnostics->appendPlainText(result.message); beginNextLanguage(); return; } }
    cancelled = false; launch("index", compilation);
}
void SemanticPanel::importDatabase(const QString &path) {
    if (root.isEmpty()) return; stop(); selectedEntries["c++"] = {"c++", QFileInfo(path).absoluteFilePath(), UiLanguage::text("C++ project")}; workerLanguage = "c++"; compilation = selectedEntries.value(workerLanguage).path; working = true; updateControls(true); refreshIndex();
}
void SemanticPanel::importCSharpProject(const QString &path, const QString &configuration, const QString &framework) {
    if (root.isEmpty()) return; stop(); selectedEntries["c#"] = {"c#", QFileInfo(path).absoluteFilePath(), UiLanguage::text("C# project")};
    { const QSignalBlocker cb(buildConfiguration), fb(targetFramework); buildConfiguration->setCurrentText(configuration); targetFramework->setText(framework); }
    workerLanguage = "c#"; compilation = selectedEntries.value(workerLanguage).path; working = true; updateControls(true); refreshIndex();
}
void SemanticPanel::chooseConfiguration() {
    const QString path = QFileDialog::getOpenFileName(this, UiLanguage::text("Select Project Configuration"), root, UiLanguage::text("Project configuration (*.sln *.csproj compile_commands.json);;All files (*)")); if (path.isEmpty()) return;
    const QString suffix = QFileInfo(path).suffix().toLower();
    if (suffix == "sln" || suffix == "csproj") importCSharpProject(path, buildConfiguration->currentText(), targetFramework->text().trimmed()); else importDatabase(path);
}
void SemanticPanel::addContextActions(QMenu *menu) {
    auto *environment = menu->addAction(UiLanguage::text("Analysis Environment...")); environment->setObjectName("analysisEnvironmentAction");
    environment->setEnabled(!isBusy()); connect(environment, &QAction::triggered, this, &SemanticPanel::chooseEnvironment);
    menu->addSeparator(); auto *configure = menu->addAction(UiLanguage::text("Import Project Configuration...")); configure->setEnabled(!root.isEmpty() && !isBusy()); connect(configure, &QAction::triggered, this, &SemanticPanel::chooseConfiguration);
    auto *choose = menu->addAction(UiLanguage::text("Select Analysis Project...")); choose->setEnabled(!root.isEmpty() && !isBusy()); connect(choose, &QAction::triggered, this, [this] { selectedEntries.clear(); emit refreshRequested(); });
    auto *cancel = menu->addAction(UiLanguage::text("Cancel Analysis")); cancel->setObjectName("cancelAnalysisAction"); cancel->setEnabled(isBusy()); connect(cancel, &QAction::triggered, this, [this] { cancelled = true; pendingLanguages.clear(); refreshAfterLoad = false; process.kill(); });
}
void SemanticPanel::loadCache(bool refresh) { refreshAfterLoad = refresh; launch("export"); }
void SemanticPanel::launch(const QString &action, const QString &database) {
    if (process.state() != QProcess::NotRunning) return; command = action; output.clear(); QStringList args;
    auto env = QProcessEnvironment::systemEnvironment(); env.remove("NODE_OPTIONS"); env.remove("NODE_PATH");
    try {
    if (workerLanguage == "java") {
        args = {"--java-analyzer", action, "--project", root};
        process.setProgram(VividCode::sharedRuntime() + "/vividcode.exe");
        if (action == "index" && !QFileInfo::exists(VividCode::sharedRuntime() + "/java-analyzer/VividCodeJava.java"))
            throw std::runtime_error("Java analyzer is not installed. Select java during installation and prepare a local JDK 17+.");
    } else if (workerLanguage == "javascript") {
        args = {"--javascript-analyzer", action, "--project", root};
        process.setProgram(VividCode::sharedRuntime() + "/vividcode.exe");
        if (action == "index" && !QFileInfo::exists(VividCode::sharedRuntime() + "/javascript-analyzer/typescript/lib/typescript.js"))
            throw std::runtime_error("JavaScript/TypeScript analyzer is not installed. Select javascript during installation; Python and other language runtimes are not required.");
    } else
    if (isCSharp()) {
        const QString dotnet = VividCode::localExecutable("dotnet", {"dotnet.exe"});
        if (action == "index" && VividCode::probeExecutable(dotnet, {"--list-sdks"}).isEmpty()) throw std::runtime_error("C# analysis requires a local .NET SDK. Install the SDK/workloads required by the project.");
        const QString analyzer = VividCode::sharedRuntime() + "/csharp-analyzer/vividcode-csharp.dll";
        if (!QFileInfo::exists(analyzer)) throw std::runtime_error("C# analyzer is not installed. Select csharp during installation; other language components are not required.");
        args = {analyzer, action, "--project", root}; if (!database.isEmpty()) args << "--project-file" << database << "--configuration" << buildConfiguration->currentText();
        if (!database.isEmpty() && !targetFramework->text().trimmed().isEmpty()) args << "--framework" << targetFramework->text().trimmed(); process.setProgram(dotnet);
        env.insert("DOTNET_HOST_PATH", dotnet); env.insert("DOTNET_ROOT", QFileInfo(dotnet).absolutePath());
    } else {
        const QString base = VividCode::sharedRuntime() + "/analyzer";
        const bool python = workerLanguage == "python";
        const QString script = base + (python ? "/python_indexer.py" : "/analyzer.py");
        if (!QFileInfo::exists(script)) throw std::runtime_error((QStringLiteral("%1 analyzer is not installed. Select %2 during installation.").arg(languageName(workerLanguage), python ? "python" : "cpp")).toStdString());
        const auto py = VividCode::localPython(); args = py.prefix + QStringList{script, action, "--project", root};
        if (!python && !database.isEmpty()) args << "--compile-commands" << database;
        process.setProgram(py.program);
        if (!python && action == "index") {
            env.insert("VIVIDCODE_LIBCLANG", VividCode::localLibClang());
            const QString resource = VividCode::environmentChoice("clang_resource");
            if (!resource.isEmpty()) env.insert("VIVIDCODE_CLANG_RESOURCE", resource);
        }
    }
    } catch (const std::exception &error) {
        const QString message = languageName(workerLanguage) + UiLanguage::text(" is not available for analysis: ") + QString::fromUtf8(error.what()); diagnostics->appendPlainText(message);
        auto languageSnapshot = languageSnapshots.value(workerLanguage); auto state = languageSnapshot.value("state").toObject(); state["last_status"] = "unavailable"; state["message"] = message;
        languageSnapshot["state"] = state; languageSnapshots[workerLanguage] = languageSnapshot; refreshAfterLoad = false; render(); beginNextLanguage(); return;
    }
    process.setArguments(args); process.setProcessEnvironment(env);
    updateControls(true); setStatus(languageName(workerLanguage) + (action == "index" ? UiLanguage::text(": checking project and dependency changes in the background...") : UiLanguage::text(": loading cached results, then checking for changes..."))); process.start();
}
void SemanticPanel::chooseEnvironment() {
    QDialog dialog(this); dialog.setObjectName("analysisEnvironmentDialog"); dialog.setWindowTitle(UiLanguage::text("vividcode - Analysis Environment")); dialog.resize(760, 380);
    auto *layout = new QVBoxLayout(&dialog); auto *hint = new QLabel(UiLanguage::text("Leave blank to detect from PATH. Prepare only the runtimes for the languages you use; vividcode does not download or install runtimes.\nJS/TS: Node 22/24 x64; Python: 3.12+ x64; C++: Python + LLVM/libclang; C#: the project's .NET SDK; Java: full JDK 17+.")); hint->setWordWrap(true); layout->addWidget(hint);
    auto *form = new QFormLayout; QMap<QString,QLineEdit *> fields; QJsonObject settings;
    try { settings = VividCode::analysisEnvironment(); } catch (const std::exception &e) { layout->addWidget(new QLabel(QString::fromUtf8(e.what()))); }
    const QMap<QString,QString> labels{{"java", "Java JDK 17+ bin/java.exe"}, {"node", "Node.js node.exe"}, {"python", "Python python.exe / py.exe"}, {"dotnet", ".NET dotnet.exe"}, {"libclang", "LLVM libclang.dll"}, {"clang_resource", UiLanguage::text("Clang built-in headers (optional)")}};
    for (auto it = labels.begin(); it != labels.end(); ++it) {
        auto *row = new QWidget; auto *line = new QHBoxLayout(row); line->setContentsMargins(0,0,0,0);
        auto *field = new QLineEdit(settings.value(it.key()).toString()); field->setObjectName("environment_" + it.key()); field->setPlaceholderText(UiLanguage::text("Auto-detect / not configured")); fields[it.key()] = field;
        auto *browse = new QPushButton(UiLanguage::text("Browse...")); line->addWidget(field,1); line->addWidget(browse); form->addRow(it.value(),row);
        const QString name = it.key(); connect(browse,&QPushButton::clicked,&dialog,[&,field,name] {
            const QString path = name == "clang_resource" ? QFileDialog::getExistingDirectory(&dialog,UiLanguage::text("Select Clang Resource Directory"),field->text()) : QFileDialog::getOpenFileName(&dialog,UiLanguage::text("Select Local Runtime File"),field->text(),name == "libclang" ? "DLL (*.dll)" : UiLanguage::text("Programs (*.exe)"));
            if (!path.isEmpty()) field->setText(path);
        });
    }
    layout->addLayout(form); auto *buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel); layout->addWidget(buttons);
    connect(buttons,&QDialogButtonBox::rejected,&dialog,&QDialog::reject);
    connect(buttons,&QDialogButtonBox::accepted,&dialog,[&] {
        QJsonObject selected; for (auto it = fields.begin(); it != fields.end(); ++it) {
            const QString path = it.value()->text().trimmed();
            if (!path.isEmpty() && (!QFileInfo(path).isAbsolute() || (it.key() == "clang_resource" ? !QFileInfo(path).isDir() : !QFileInfo(path).isFile()))) { QMessageBox::warning(&dialog, "vividcode", UiLanguage::text("Invalid path: ") + path); return; }
            if (!path.isEmpty()) selected[it.key()] = QDir::fromNativeSeparators(path);
        }
        QString error; if (!VividCode::saveAnalysisEnvironment(selected,&error)) { QMessageBox::warning(&dialog,"vividcode",error); return; } dialog.accept();
    });
    if (dialog.exec() == QDialog::Accepted && !root.isEmpty()) emit refreshRequested();
}
QMap<QString, QStringList> SemanticPanel::fileDependencies() const {
    QMap<QString, QSet<QString>> adjacent;
    const auto fileOf = [this](const QJsonObject &s) {
        QString file = s.value("def_file").toString(); if (file.isEmpty()) file = s.value("decl_file").toString();
        return QDir::isAbsolutePath(file) ? QDir(root).relativeFilePath(file) : file;
    };
    // Always use all successful language snapshots, regardless of canvas filters.
    for (const auto &value : snapshot.value("units").toArray()) {
        const auto unit = value.toObject(); QMap<QString, QString> files;
        for (const auto &item : unit.value("symbols").toArray()) {
            const auto symbol = item.toObject();
            if (!symbol.value("is_external").toBool()) files[symbol.value("id").toString()] = fileOf(symbol);
        }
        for (const auto &item : unit.value("relations").toArray()) {
            const auto relation = item.toObject();
            QString a = relation.value("file").toString(); if (QDir::isAbsolutePath(a)) a = QDir(root).relativeFilePath(a);
            if (!knownSources.contains(a)) a = files.value(relation.value("source_id").toString());
            const QString b = files.value(relation.value("target_id").toString());
            if (knownSources.contains(a) && knownSources.contains(b) && a != b) { adjacent[a].insert(b); adjacent[b].insert(a); }
        }
    }
    QMap<QString, QStringList> result;
    for (auto it = adjacent.begin(); it != adjacent.end(); ++it) { auto paths = it.value().values(); paths.sort(); result[it.key()] = paths; }
    return result;
}

QJsonArray SemanticPanel::sourceHighlights(const QString &path) const {
    const auto normalize = [this](const QString &file) {
        return QDir::cleanPath(QDir::fromNativeSeparators(QDir::isAbsolutePath(file) ? file : QDir(root).filePath(file)));
    };
    const QString wanted = normalize(path);
    QJsonArray result; QSet<QByteArray> seen;
    const auto append = [&](const QJsonObject &symbol, const QJsonObject &site, const QString &prefix, bool declaration, const QString &language) {
        const QString file = site.value(prefix + "file").toString();
        if (file.isEmpty() || normalize(file).compare(wanted, Qt::CaseInsensitive) != 0 || symbol.value("name").toString().isEmpty()) return;
        const QJsonObject mark{{"line", site.value(prefix + "line")}, {"col", site.value(prefix + "col")},
            {"name", symbol.value("name")}, {"kind", symbol.value("kind")}, {"language", language}, {"declaration", declaration}};
        const auto key = QJsonDocument(mark).toJson(QJsonDocument::Compact);
        if (!seen.contains(key)) { seen.insert(key); result.append(mark); }
    };
    // Read every unit, independent of graph focus, relation filters or selected scope.
    for (const auto &value : snapshot.value("units").toArray()) {
        const auto unit = value.toObject(); const QString language = unit.value("language").toString();
        QHash<QString, QJsonObject> symbolsById;
        for (const auto &entry : unit.value("symbols").toArray()) {
            const auto symbol = entry.toObject(); symbolsById.insert(symbol.value("id").toString(), symbol);
            append(symbol, symbol, "decl_", true, language); append(symbol, symbol, "def_", true, language);
        }
        for (const auto &entry : unit.value("relations").toArray()) {
            const auto site = entry.toObject(); const auto symbol = symbolsById.value(site.value("target_id").toString());
            if (symbol.value("kind") != "UnresolvedCall") append(symbol, site, {}, false, language);
        }
    }
    return result;
}

QJsonObject SemanticPanel::combinedUnit() const {
    QMap<QString,QJsonObject> symbolMap, relationMap; const int selectedScope = units->currentData().toInt(); const auto values = snapshot.value("units").toArray();
    for (int index = 0; index < values.size(); ++index) {
        if (selectedScope >= 0 && index != selectedScope) continue; const auto unit = values[index].toObject();
        for (const auto &value : unit.value("symbols").toArray()) { const auto s = value.toObject(); const QString id = s.value("id").toString(); if (!symbolMap.contains(id) || symbolMap.value(id).value("decl_file").toString().isEmpty()) symbolMap[id] = s; }
        for (const auto &value : unit.value("relations").toArray()) { const auto r = value.toObject(); relationMap[QString::fromUtf8(QJsonDocument(r).toJson(QJsonDocument::Compact))] = r; }
    }
    QJsonArray sv, rv; for (const auto &s : symbolMap) sv.append(s); for (const auto &r : relationMap) rv.append(r);
    return {{"symbols", sv}, {"relations", rv}, {"file", root}, {"config_id", "unified-project"}};
}
void SemanticPanel::setStatus(const QString &text) {
    status->setText(text);
    status->setVisible(!text.isEmpty());
}
void SemanticPanel::render() {
    QJsonArray values; QStringList states;
    for (auto it = languageSnapshots.begin(); it != languageSnapshots.end(); ++it) {
        const QString language = it.key(), prefix = language + ':';
        const auto state = it.value().value("state").toObject();
        if (state.value("last_status") != "ok") {
            QString message = state.value("message").toString();
            const auto errors = state.value("errors").toArray();
            if (message.isEmpty() && !errors.isEmpty()) message = errors.first().toObject().value("message").toString();
            if (message.isEmpty()) message = UiLanguage::text("Analysis incomplete. Check the environment and refresh.");
            states.append(languageName(language) + ": " + state.value("last_status").toString("incomplete") + " - " + message);
        }
        for (const auto &value : it.value().value("units").toArray()) {
            auto unit = value.toObject(); if (unit.value("config_id") == "csharp-solution") continue; unit["language"] = language; QJsonArray sv, rv;
            for (const auto &item : unit.value("symbols").toArray()) { auto s = item.toObject(); s["language"] = language; s["id"] = prefix + s.value("id").toString(); if (!s.value("parent_id").toString().isEmpty()) s["parent_id"] = prefix + s.value("parent_id").toString(); sv.append(s); }
            for (const auto &item : unit.value("relations").toArray()) { auto r = item.toObject(); r["language"] = language; r["source_id"] = prefix + r.value("source_id").toString(); r["target_id"] = prefix + r.value("target_id").toString(); rv.append(r); }
            unit["symbols"] = sv; unit["relations"] = rv; values.append(unit);
        }
    }
    snapshot = {{"units", values}};
    { const QSignalBlocker blocker(units); units->clear(); units->addItem(UiLanguage::text("All configurations"), -1);
        for (int index = 0; index < values.size(); ++index) { const auto u = values[index].toObject(); units->addItem(languageName(u.value("language").toString()) + " · " + u.value("scope_name").toString(QDir(root).relativeFilePath(u.value("file").toString())), index); }
    }
    setStatus(languageSnapshots.isEmpty() ? UiLanguage::text("Waiting for analysis. Right-click to import configuration or refresh.") : states.join(QStringLiteral(" · "))); renderUnit();
}
void SemanticPanel::renderUnit() {
    const QSignalBlocker blocker(symbols); symbols->clear(); selectedSymbol.clear(); symbolFilesMode = false; const auto unit = combinedUnit();
    QMap<QString,QTreeWidgetItem *> items, fileRoots; QMap<QString,QJsonObject> records;
    const auto fileOf = [this](const QJsonObject &s) {
        QString file = s.value("def_file").toString(); if (file.isEmpty()) file = s.value("decl_file").toString();
        return QDir::isAbsolutePath(file) ? QDir(root).relativeFilePath(file) : file;
    };
    for (const auto &value : unit.value("symbols").toArray()) {
        const auto s = value.toObject(); if (s.value("is_external").toBool()) continue;
        const QString file = fileOf(s), id = s.value("id").toString(); if (file.isEmpty()) continue;
        if (!fileRoots.contains(file)) {
            auto *entry = new QTreeWidgetItem(symbols, {file, languageName(s.value("language").toString())});
            entry->setData(0, Qt::UserRole, QJsonObject{{"def_file", file}, {"def_line", 1}, {"def_col", 1}}); fileRoots[file] = entry;
        }
        records[id] = s;
        if (s.value("kind") == "ModuleDecl") continue;
        QString label = s.value("name").toString(); if (label.isEmpty()) label = s.value("qualified_name").toString();
        const QString kind = s.value("kind").toString();
        const QString type = kind == "ParmDecl" ? UiLanguage::text("Parameter") : kind == "VarDecl" ? UiLanguage::text("Variable") : kind == "FieldDecl" ? UiLanguage::text("Field") : kind == "FunctionDecl" ? UiLanguage::text("Function") : kind == "CXXMethod" ? UiLanguage::text("Method") : kind;
        auto *item = new QTreeWidgetItem({label, type, location(s)}); item->setToolTip(0, s.value("qualified_name").toString() + '\n' + s.value("signature").toString() + UiLanguage::text("\nDouble-click to view related files"));
        item->setData(0, Qt::UserRole, s); items[id] = item;
    }
    for (auto it = items.begin(); it != items.end(); ++it) {
        const auto s = records.value(it.key()); const auto parentId = s.value("parent_id").toString(); auto *parent = items.value(parentId);
        if (parent && parent != it.value() && fileOf(records.value(parentId)) == fileOf(s)) parent->addChild(it.value());
        else fileRoots.value(fileOf(s))->addChild(it.value());
    }
    symbols->sortItems(0, Qt::AscendingOrder); symbols->expandToDepth(0); renderRelations();
    if (!pendingFileFocus.isEmpty()) focusFile(pendingFileFocus);
}
void SemanticPanel::renderRelations() {
    relations->clear(); const auto unit = combinedUnit(); QMap<QString,QString> names;
    for (const auto &value : unit.value("symbols").toArray()) { const auto s = value.toObject(); names[s.value("id").toString()] = s.value("qualified_name").toString(s.value("name").toString()); }
    if (symbolFilesMode) graph->setData(buildSymbolFileUnit(unit, selectedSymbol, root, relationType->currentData().toString()), {}, {}, UiLanguage::text("Related files - double-click other symbols to switch; use Show All Relations to return."));
    else graph->setData(unit, selectedSymbol, relationType->currentData().toString(), units->currentText());
    for (const auto &value : unit.value("relations").toArray()) {
        const auto r = value.toObject(); const QString a = r.value("source_id").toString(), b = r.value("target_id").toString(), type = r.value("type").toString();
        if (!selectedSymbol.isEmpty() && a != selectedSymbol && b != selectedSymbol) continue; if (!relationType->currentData().toString().isEmpty() && type != relationType->currentData().toString()) continue;
        auto *item = new QTreeWidgetItem(relations, {names.value(a, a), relationDisplayName(type), names.value(b, b), r.value("file").toString() + ':' + QString::number(r.value("line").toInt())}); item->setData(0, Qt::UserRole, r);
    }
}
void SemanticPanel::focusSymbol(const QString &id) {
    pendingFileFocus.clear();
    symbolFilesMode = false;
    selectedSymbol = id; { const QSignalBlocker blocker(units); units->setCurrentIndex(0); }
    { const QSignalBlocker blocker(symbols); QTreeWidgetItem *match = nullptr; for (QTreeWidgetItemIterator it(symbols); *it; ++it) if ((*it)->data(0, Qt::UserRole).toJsonObject().value("id").toString() == id) { match = *it; break; } symbols->setCurrentItem(match); if (match) symbols->scrollToItem(match); }
    renderRelations();
}

bool SemanticPanel::focusFile(const QString &path) {
    pendingFileFocus = path;
    const bool found = graph->focusFile(path, root);
    if (found) pendingFileFocus.clear();
    return found;
}

void SemanticPanel::showSymbolFiles(const QString &id) {
    if (id.isEmpty()) return;
    focusSymbol(id); symbolFilesMode = true;
    { const QSignalBlocker blocker(relationType); relationType->setCurrentIndex(0); }
    renderRelations();
}
