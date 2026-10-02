#include "SemanticPanel.h"
#include "ProjectStore.h"
#include "UnifiedGraphStore.h"
#include "RelationGraph.h"
#include <QtWidgets>

namespace {
QString location(const QJsonObject &s) {
    const QString prefix = s.value("def_file").toString().isEmpty() ? "decl_" : "def_";
    return s.value(prefix + "file").toString() + ':' + QString::number(s.value(prefix + "line").toInt());
}
QString languageName(const QString &language) { return language == "c#" ? "C#" : "C++"; }
QJsonObject stateAt(const QString &root, const QString &language) {
    QFile file(VividCode::semanticDirectory(root) + (language == "c#" ? "/csharp/config.json" : "/config.json"));
    return file.open(QIODevice::ReadOnly) ? QJsonDocument::fromJson(file.readAll()).object() : QJsonObject();
}
}

SemanticPanel::SemanticPanel(QWidget *parent) : QWidget(parent) {
    auto *layout = new QVBoxLayout(this); layout->setContentsMargins(0, 0, 0, 0);
    controls = new QWidget(this); controls->setObjectName("analysisControls");
    auto *row = new QHBoxLayout(controls); row->setContentsMargins(0, 0, 0, 0);
    relationType = new QComboBox; relationType->setObjectName("semanticRelationType"); relationType->addItem(QStringLiteral("全部关系类型"), "");
    for (const QString &type : {"calls", "possible_call", "inherits", "implements", "member_ref", "references", "overrides"}) relationType->addItem(relationDisplayName(type), type);
    row->addWidget(relationType);
    buildConfiguration = new QComboBox; buildConfiguration->setObjectName("csharpConfiguration"); buildConfiguration->addItems({"Debug", "Release"}); row->addWidget(buildConfiguration);
    targetFramework = new QLineEdit; targetFramework->setObjectName("csharpFramework"); targetFramework->setPlaceholderText(QStringLiteral("目标框架（留空使用工程默认值）")); row->addWidget(targetFramework, 1);
    status = new QLabel(QStringLiteral("打开项目后自动识别工程；右键可刷新或导入配置。")); status->setObjectName("semanticStatus"); status->setWordWrap(true); status->setTextFormat(Qt::PlainText); layout->addWidget(status);
    units = new QComboBox; units->setObjectName("semanticUnits"); layout->addWidget(units);
    graph = new RelationGraph; graph->setObjectName("realRelationGraph"); layout->addWidget(graph, 1);
    diagnostics = new QPlainTextEdit; diagnostics->setObjectName("semanticDiagnostics"); diagnostics->setReadOnly(true); diagnostics->setMaximumHeight(100); diagnostics->hide(); layout->addWidget(diagnostics);
    structure = new QWidget(this); structure->setObjectName("structureRelationsView");
    auto *sl = new QVBoxLayout(structure); sl->setContentsMargins(0, 0, 0, 0);
    auto *all = new QPushButton(QStringLiteral("显示全部工程关系")); sl->addWidget(all);
    auto *splitter = new QSplitter(Qt::Vertical); sl->addWidget(splitter, 1);
    symbols = new QTreeWidget; symbols->setObjectName("semanticSymbols"); symbols->setHeaderLabels({QStringLiteral("语言 / 类 / 成员"), QStringLiteral("类型"), QStringLiteral("源码位置")}); symbols->setColumnWidth(0, 250); splitter->addWidget(symbols);
    relations = new QTreeWidget; relations->setObjectName("semanticRelations"); relations->setHeaderLabels({QStringLiteral("起点"), QStringLiteral("关系"), QStringLiteral("终点"), QStringLiteral("发生位置")}); splitter->addWidget(relations); splitter->setSizes({450, 230});
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
        if (!item) return; selectedSymbol = item->data(0, Qt::UserRole).toJsonObject().value("id").toString();
        { const QSignalBlocker blocker(units); units->setCurrentIndex(0); } renderRelations();
    });
    for (auto *tree : {symbols, relations}) {
        tree->setContextMenuPolicy(Qt::CustomContextMenu);
        connect(tree, &QTreeWidget::customContextMenuRequested, this, [this, tree](const QPoint &point) {
            auto *menu = new QMenu(tree); menu->setObjectName("structureContextMenu"); menu->setAttribute(Qt::WA_DeleteOnClose);
            if (auto *item = tree->itemAt(point)) {
                const auto value = item->data(0, Qt::UserRole).toJsonObject();
                const QString prefix = tree == relations ? QString() : value.value("def_file").toString().isEmpty() ? "decl_" : "def_";
                if (!value.value(prefix + "file").toString().isEmpty()) {
                    auto *source = menu->addAction(QStringLiteral("查看源代码"));
                    source->setObjectName("structureViewSourceAction");
                    connect(source, &QAction::triggered, this, [this, value, prefix] { emit sourceRequested(value.value(prefix + "file").toString(), value.value(prefix + "line").toInt(1), value.value(prefix + "col").toInt(1)); });
                }
            }
            auto *refresh = menu->addAction(QStringLiteral("刷新文件和索引")); connect(refresh, &QAction::triggered, this, &SemanticPanel::refreshRequested);
            addContextActions(menu); menu->popup(tree->viewport()->mapToGlobal(point));
        });
    }
    connect(&process, &QProcess::readyReadStandardOutput, this, [this] {
        output += process.readAllStandardOutput();
        if (output.size() > 128 * 1024 * 1024) { diagnostics->appendPlainText(QStringLiteral("结果超过 128 MiB 展示上限。")); process.kill(); }
        if (command == "index") while (output.contains('\n')) {
            const auto line = output.left(output.indexOf('\n')); output.remove(0, line.size() + 1);
            const auto obj = QJsonDocument::fromJson(line).object();
            if (obj.value("type") == "progress") status->setText(QStringLiteral("%1：分析 %2/%3 · %4").arg(languageName(workerLanguage)).arg(obj.value("current").toInt()).arg(obj.value("total").toInt()).arg(obj.value("file").toString()));
            if (obj.contains("message")) diagnostics->appendPlainText(obj.value("message").toString());
        }
    });
    connect(&process, &QProcess::readyReadStandardError, this, [this] { diagnostics->appendPlainText(QString::fromUtf8(process.readAllStandardError())); });
    connect(&process, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (stopping || error != QProcess::FailedToStart) return;
        diagnostics->appendPlainText(languageName(workerLanguage) + QStringLiteral(" 分析器无法启动：") + process.errorString()); beginNextLanguage();
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
                    if (!store.saveLanguage(workerLanguage, doc.object())) diagnostics->appendPlainText(QStringLiteral("共享图数据库保存失败：") + store.error);
                    else languageSnapshots[workerLanguage] = store.load().value(workerLanguage);
                }
                render();
            } else diagnostics->appendPlainText(QStringLiteral("索引结果无效：") + error.errorString());
        } else diagnostics->appendPlainText(QStringLiteral("读取索引失败：") + QString::fromUtf8(output));
        if (refreshAfterLoad && !cancelled) { refreshAfterLoad = false; refreshIndex(); return; }
        refreshAfterLoad = false;
        if (cancelled) { pendingLanguages.clear(); status->setText(QStringLiteral("已取消，保留上次完整结果。")); cancelled = false; }
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
    QDialog dialog(this); dialog.setObjectName("analysisProjectDialog"); dialog.setWindowTitle(QStringLiteral("vividcode · 选择 %1 工程").arg(languageName(language))); dialog.resize(600, 320);
    auto *layout = new QVBoxLayout(&dialog); layout->addWidget(new QLabel(QStringLiteral("发现多个 %1 工程入口，请点击要分析的工程：").arg(languageName(language))));
    auto *list = new QListWidget; list->setObjectName("analysisProjectList");
    for (int index = 0; index < entries.size(); ++index) { auto *item = new QListWidgetItem(entries[index].label, list); item->setData(Qt::UserRole, index); item->setToolTip(entries[index].path); }
    layout->addWidget(list); auto *buttons = new QDialogButtonBox(QDialogButtonBox::Cancel); buttons->button(QDialogButtonBox::Cancel)->setText(QStringLiteral("取消")); layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject); connect(list, &QListWidget::itemClicked, &dialog, &QDialog::accept); connect(list, &QListWidget::itemActivated, &dialog, &QDialog::accept);
    if (dialog.exec() != QDialog::Accepted || !list->currentItem()) return {};
    return entries[list->currentItem()->data(Qt::UserRole).toInt()];
}
void SemanticPanel::setProject(const QString &path, const QStringList &sources) {
    const bool changed = root != path; stop(); root = path; compilation.clear(); selectedSymbol.clear(); diagnostics->clear();
    if (changed || !sources.isEmpty()) knownSources = sources;
    if (changed) { selectedEntries.clear(); languageSnapshots.clear(); }
    if (root.isEmpty()) { selectedEntries.clear(); languageSnapshots.clear(); render(); updateControls(false); return; }
    if (changed) {
        UnifiedGraphStore store(root); languageSnapshots = store.load(); const auto cs = stateAt(root, "c#");
        const QSignalBlocker cb(buildConfiguration), fb(targetFramework); buildConfiguration->setCurrentText(cs.value("configuration").toString("Debug")); targetFramework->setText(cs.value("framework").toString());
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
        status->setText(cancelledSelection ? QStringLiteral("已取消选择工程。右键刷新可重新选择。") : grouped.isEmpty() ? QStringLiteral("当前语言仅支持文件浏览。右键文件可查看源码。") : QStringLiteral("已自动识别语言；尚未找到可分析的工程配置，请右键导入 .sln / .csproj 或 compile_commands.json。")); emit settled(); return;
    }
    working = true; beginNextLanguage();
}
void SemanticPanel::beginNextLanguage() {
    if (pendingLanguages.isEmpty()) { working = false; updateControls(false); emit settled(); return; }
    working = true; workerLanguage = pendingLanguages.takeFirst(); compilation = selectedEntries.value(workerLanguage).path;
    const auto state = stateAt(root, workerLanguage); const QString previous = state.value(isCSharp() ? "project_file" : "compile_commands").toString();
    if (!previous.isEmpty() && QFileInfo(previous).absoluteFilePath() == compilation) { loadCache(true); return; } refreshIndex();
}
void SemanticPanel::refreshIndex() {
    if (compilation.isEmpty()) { beginNextLanguage(); return; }
    if (!QFileInfo::exists(root + "/.vividcode/project.json")) { const auto result = VividCode::generate(root); if (!result.ok) { diagnostics->appendPlainText(result.message); beginNextLanguage(); return; } }
    cancelled = false; launch("index", compilation);
}
void SemanticPanel::importDatabase(const QString &path) {
    if (root.isEmpty()) return; stop(); selectedEntries["c++"] = {"c++", QFileInfo(path).absoluteFilePath(), QStringLiteral("C++ 工程")}; workerLanguage = "c++"; compilation = selectedEntries.value(workerLanguage).path; working = true; updateControls(true); refreshIndex();
}
void SemanticPanel::importCSharpProject(const QString &path, const QString &configuration, const QString &framework) {
    if (root.isEmpty()) return; stop(); selectedEntries["c#"] = {"c#", QFileInfo(path).absoluteFilePath(), QStringLiteral("C# 工程")};
    { const QSignalBlocker cb(buildConfiguration), fb(targetFramework); buildConfiguration->setCurrentText(configuration); targetFramework->setText(framework); }
    workerLanguage = "c#"; compilation = selectedEntries.value(workerLanguage).path; working = true; updateControls(true); refreshIndex();
}
void SemanticPanel::chooseConfiguration() {
    const QString path = QFileDialog::getOpenFileName(this, QStringLiteral("选择工程配置"), root, "工程配置 (*.sln *.csproj compile_commands.json);;所有文件 (*)"); if (path.isEmpty()) return;
    const QString suffix = QFileInfo(path).suffix().toLower();
    if (suffix == "sln" || suffix == "csproj") importCSharpProject(path, buildConfiguration->currentText(), targetFramework->text().trimmed()); else importDatabase(path);
}
void SemanticPanel::addContextActions(QMenu *menu) {
    menu->addSeparator(); auto *configure = menu->addAction(QStringLiteral("导入工程配置…")); configure->setEnabled(!root.isEmpty() && !isBusy()); connect(configure, &QAction::triggered, this, &SemanticPanel::chooseConfiguration);
    auto *choose = menu->addAction(QStringLiteral("重新选择分析工程…")); choose->setEnabled(!root.isEmpty() && !isBusy()); connect(choose, &QAction::triggered, this, [this] { selectedEntries.clear(); emit refreshRequested(); });
    auto *cancel = menu->addAction(QStringLiteral("取消分析")); cancel->setObjectName("cancelAnalysisAction"); cancel->setEnabled(isBusy()); connect(cancel, &QAction::triggered, this, [this] { cancelled = true; pendingLanguages.clear(); refreshAfterLoad = false; process.kill(); });
}
void SemanticPanel::loadCache(bool refresh) { refreshAfterLoad = refresh; launch("export"); }
void SemanticPanel::launch(const QString &action, const QString &database) {
    if (process.state() != QProcess::NotRunning) return; command = action; output.clear(); QStringList args;
    if (isCSharp()) {
        args = {action, "--project", root}; if (!database.isEmpty()) args << "--project-file" << database << "--configuration" << buildConfiguration->currentText();
        if (!database.isEmpty() && !targetFramework->text().trimmed().isEmpty()) args << "--framework" << targetFramework->text().trimmed(); process.setProgram(VividCode::sharedRuntime() + "/csharp-analyzer/vividcode-csharp.exe");
    } else { const QString base = VividCode::sharedRuntime() + "/analyzer"; args = {base + "/analyzer.py", action, "--project", root}; if (!database.isEmpty()) args << "--compile-commands" << database; process.setProgram(base + "/python/python.exe"); }
    process.setArguments(args); auto env = QProcessEnvironment::systemEnvironment(); env.remove("VIVIDCODE_LIBCLANG"); env.remove("LIBCLANG_PATH"); process.setProcessEnvironment(env);
    updateControls(true); status->setText(languageName(workerLanguage) + (action == "index" ? QStringLiteral("：后台检查工程和依赖变化…") : QStringLiteral("：读取已有结果，随后自动检查修改…"))); process.start();
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
void SemanticPanel::render() {
    QJsonArray values; QStringList states;
    for (auto it = languageSnapshots.begin(); it != languageSnapshots.end(); ++it) {
        const QString language = it.key(), prefix = language + ':'; const auto state = it.value().value("state").toObject(); states.append(languageName(language) + ':' + state.value("last_status").toString(QStringLiteral("缓存")));
        for (const auto &value : it.value().value("units").toArray()) {
            auto unit = value.toObject(); if (unit.value("config_id") == "csharp-solution") continue; unit["language"] = language; QJsonArray sv, rv;
            for (const auto &item : unit.value("symbols").toArray()) { auto s = item.toObject(); s["language"] = language; s["original_id"] = s.value("id"); s["id"] = prefix + s.value("id").toString(); if (!s.value("parent_id").toString().isEmpty()) s["parent_id"] = prefix + s.value("parent_id").toString(); sv.append(s); }
            for (const auto &item : unit.value("relations").toArray()) { auto r = item.toObject(); r["language"] = language; r["source_id"] = prefix + r.value("source_id").toString(); r["target_id"] = prefix + r.value("target_id").toString(); rv.append(r); }
            unit["symbols"] = sv; unit["relations"] = rv; values.append(unit);
        }
    }
    snapshot = {{"units", values}};
    { const QSignalBlocker blocker(units); units->clear(); units->addItem(QStringLiteral("全部工程 · 各语言在同一画布分区显示"), -1);
        for (int index = 0; index < values.size(); ++index) { const auto u = values[index].toObject(); units->addItem(languageName(u.value("language").toString()) + " · " + u.value("scope_name").toString(QDir(root).relativeFilePath(u.value("file").toString())), index); }
    }
    status->setText(states.isEmpty() ? QStringLiteral("尚无语义结果。右键可导入工程配置或刷新。") : states.join(QStringLiteral(" · ")) + QStringLiteral(" · 单击选中，右键查看源代码。")); renderUnit();
}
void SemanticPanel::renderUnit() {
    const QSignalBlocker blocker(symbols); symbols->clear(); selectedSymbol.clear(); const auto unit = combinedUnit(); QMap<QString,QTreeWidgetItem *> items, languageRoots;
    for (const auto &value : unit.value("symbols").toArray()) {
        const auto s = value.toObject(); if (s.value("is_external").toBool()) continue; const QString language = s.value("language").toString();
        if (!languageRoots.contains(language)) languageRoots[language] = new QTreeWidgetItem(symbols, {languageName(language)});
        QString label = s.value("qualified_name").toString(s.value("name").toString()); if (!s.value("signature").toString().isEmpty()) label += " · " + s.value("signature").toString();
        auto *item = new QTreeWidgetItem({label, s.value("kind").toString(), location(s)}); item->setData(0, Qt::UserRole, s); items[s.value("id").toString()] = item;
    }
    for (auto it = items.begin(); it != items.end(); ++it) { const auto s = it.value()->data(0, Qt::UserRole).toJsonObject(); auto *parent = items.value(s.value("parent_id").toString()); if (parent && parent != it.value()) parent->addChild(it.value()); else languageRoots.value(s.value("language").toString())->addChild(it.value()); }
    symbols->expandToDepth(1); renderRelations();
}
void SemanticPanel::renderRelations() {
    relations->clear(); const auto unit = combinedUnit(); QMap<QString,QString> names;
    for (const auto &value : unit.value("symbols").toArray()) { const auto s = value.toObject(); names[s.value("id").toString()] = s.value("qualified_name").toString(s.value("name").toString()); }
    graph->setData(unit, selectedSymbol, relationType->currentData().toString(), units->currentText());
    for (const auto &value : unit.value("relations").toArray()) {
        const auto r = value.toObject(); const QString a = r.value("source_id").toString(), b = r.value("target_id").toString(), type = r.value("type").toString();
        if (!selectedSymbol.isEmpty() && a != selectedSymbol && b != selectedSymbol) continue; if (!relationType->currentData().toString().isEmpty() && type != relationType->currentData().toString()) continue;
        auto *item = new QTreeWidgetItem(relations, {names.value(a, a), relationDisplayName(type), names.value(b, b), r.value("file").toString() + ':' + QString::number(r.value("line").toInt())}); item->setData(0, Qt::UserRole, r);
    }
}
void SemanticPanel::focusSymbol(const QString &id) {
    selectedSymbol = id; { const QSignalBlocker blocker(units); units->setCurrentIndex(0); }
    { const QSignalBlocker blocker(symbols); QTreeWidgetItem *match = nullptr; for (QTreeWidgetItemIterator it(symbols); *it; ++it) if ((*it)->data(0, Qt::UserRole).toJsonObject().value("id").toString() == id) { match = *it; break; } symbols->setCurrentItem(match); if (match) symbols->scrollToItem(match); }
    renderRelations();
}
