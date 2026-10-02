#include "SemanticPanel.h"
#include "ProjectStore.h"
#include "SharedRuntime.h"
#include "RelationGraph.h"
#include <QtWidgets>

namespace {
QString relationName(const QString &type) {
    return relationDisplayName(type);
}
QString location(const QJsonObject &s, bool definition) {
    const QString prefix = definition && !s.value("def_file").toString().isEmpty() ? "def_" : "decl_";
    return s.value(prefix + "file").toString() + ':' + QString::number(s.value(prefix + "line").toInt());
}
}

SemanticPanel::SemanticPanel(QWidget *parent) : QWidget(parent) {
    auto *layout = new QVBoxLayout(this);
    auto *buttons = new QHBoxLayout;
    import = new QPushButton(QStringLiteral("导入编译数据库…"));
    update = new QPushButton(QStringLiteral("更新 C++ 索引"));
    cancel = new QPushButton(QStringLiteral("取消")); cancel->setEnabled(false);
    buttons->addWidget(import); buttons->addWidget(update); buttons->addWidget(cancel); buttons->addStretch();
    layout->addLayout(buttons);
    status = new QLabel(QStringLiteral("导入 compile_commands.json 后分析 C++。该文件由项目构建系统导出，记录头文件路径、宏和语言标准。"));
    status->setWordWrap(true); status->setTextFormat(Qt::PlainText); status->setObjectName("semanticStatus");
    layout->addWidget(status);
    units = new QComboBox; units->setObjectName("semanticUnits"); layout->addWidget(units);
    auto *selectors = new QHBoxLayout;
    focusSymbols = new QComboBox; focusSymbols->setObjectName("semanticFocus");
    focusSymbols->setEditable(true); focusSymbols->setInsertPolicy(QComboBox::NoInsert);
    focusSymbols->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    focusSymbols->setMinimumContentsLength(20);
    focusSymbols->completer()->setFilterMode(Qt::MatchContains);
    focusSymbols->completer()->setCaseSensitivity(Qt::CaseInsensitive);
    focusSymbols->completer()->setCompletionMode(QCompleter::PopupCompletion);
    selectors->addWidget(new QLabel(QStringLiteral("选择符号："))); selectors->addWidget(focusSymbols, 1);
    relationType = new QComboBox; relationType->addItem(QStringLiteral("全部关系类型"), "");
    relationType->setObjectName("semanticRelationType");
    for (const QString &type : {"calls", "possible_call", "inherits", "member_ref", "references", "overrides"}) relationType->addItem(relationName(type), type);
    selectors->addWidget(relationType); layout->addLayout(selectors);
    tabs = new QTabWidget; tabs->setObjectName("semanticViews");
    graph = new RelationGraph; graph->setObjectName("realRelationGraph");
    tabs->addTab(graph, QStringLiteral("真实关系图"));
    auto *splitter = new QSplitter;
    symbols = new QTreeWidget; symbols->setObjectName("semanticSymbols");
    symbols->setHeaderLabels({QStringLiteral("类 / 成员 / 函数"), QStringLiteral("类型 / 访问"), QStringLiteral("定义 / 声明位置")});
    symbols->setColumnWidth(0, 260); symbols->setColumnWidth(1, 140);
    splitter->addWidget(symbols);
    auto *relationBox = new QWidget; auto *rl = new QVBoxLayout(relationBox); rl->setContentsMargins(0,0,0,0);
    auto *all = new QPushButton(QStringLiteral("查看本编译单元全部关系"));
    rl->addWidget(all);
    relations = new QTreeWidget; relations->setObjectName("semanticRelations");
    relations->setHeaderLabels({QStringLiteral("起点"), QStringLiteral("关系"), QStringLiteral("终点"), QStringLiteral("发生位置")});
    relations->setColumnWidth(0,180); relations->setColumnWidth(2,180); rl->addWidget(relations);
    splitter->addWidget(relationBox); tabs->addTab(splitter, QStringLiteral("C++ 结构与关系列表"));
    layout->addWidget(tabs, 1);
    diagnostics = new QPlainTextEdit; diagnostics->setReadOnly(true); diagnostics->setMaximumHeight(100);
    diagnostics->setPlaceholderText(QStringLiteral("编译诊断显示在这里。出错时保留上次完整索引；成员引用暂不区分读写。"));
    layout->addWidget(diagnostics);
    diagnostics->hide();
    connect(diagnostics, &QPlainTextEdit::textChanged, this, [this] { diagnostics->setVisible(!diagnostics->toPlainText().isEmpty()); });
    connect(import, &QPushButton::clicked, this, [this] {
        const QString path = QFileDialog::getOpenFileName(this, QStringLiteral("选择 compile_commands.json"), root, "JSON (*.json)");
        if (!path.isEmpty()) importDatabase(path);
    });
    connect(update, &QPushButton::clicked, this, &SemanticPanel::refreshIndex);
    connect(cancel, &QPushButton::clicked, this, [this] { cancelled = true; refreshAfterLoad = false; process.kill(); });
    connect(units, &QComboBox::currentIndexChanged, this, &SemanticPanel::renderUnit);
    connect(relationType, &QComboBox::currentIndexChanged, this, &SemanticPanel::renderRelations);
    connect(all, &QPushButton::clicked, this, [this] {
        { const QSignalBlocker blocker(focusSymbols); focusSymbols->setCurrentIndex(0); }
        selectedSymbol.clear(); renderRelations(); tabs->setCurrentIndex(1);
    });
    connect(focusSymbols, &QComboBox::currentIndexChanged, this, [this](int index) {
        if (index >= 0) focusSymbol(focusSymbols->itemData(index).toString());
    });
    connect(graph, &RelationGraph::sourceRequested, this, &SemanticPanel::sourceRequested);
    connect(graph, &RelationGraph::focusRequested, this, &SemanticPanel::focusSymbol);
    connect(symbols, &QTreeWidget::currentItemChanged, this, [this](QTreeWidgetItem *item) {
        if (!item) return;
        const auto s = item->data(0, Qt::UserRole).toJsonObject();
        selectedSymbol = s.value("id").toString();
        { const QSignalBlocker blocker(focusSymbols); focusSymbols->setCurrentIndex(focusSymbols->findData(selectedSymbol)); }
        renderRelations();
        const QString prefix = s.value("def_file").toString().isEmpty() ? "decl_" : "def_";
        emit sourceRequested(s.value(prefix + "file").toString(), s.value(prefix + "line").toInt(), s.value(prefix + "col").toInt());
    });
    symbols->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(symbols, &QTreeWidget::customContextMenuRequested, this, [this](const QPoint &point) {
        auto *item = symbols->itemAt(point); if (!item) return;
        const auto s = item->data(0, Qt::UserRole).toJsonObject();
        QMenu menu; auto *decl = menu.addAction(QStringLiteral("定位声明"));
        if (menu.exec(symbols->viewport()->mapToGlobal(point)) == decl)
            emit sourceRequested(s.value("decl_file").toString(), s.value("decl_line").toInt(), s.value("decl_col").toInt());
    });
    connect(relations, &QTreeWidget::currentItemChanged, this, [this](QTreeWidgetItem *item) {
        if (!item) return;
        const auto r = item->data(0, Qt::UserRole).toJsonObject();
        emit sourceRequested(r.value("file").toString(), r.value("line").toInt(), r.value("col").toInt());
    });
    connect(&process, &QProcess::readyReadStandardOutput, this, [this] {
        output += process.readAllStandardOutput();
        if (output.size() > 128 * 1024 * 1024) { diagnostics->appendPlainText(QStringLiteral("结果超过 128 MiB 展示上限。")); process.kill(); }
        if (command == "index") {
            while (output.contains('\n')) {
                const auto line = output.left(output.indexOf('\n')); output.remove(0, line.size() + 1);
                const auto obj = QJsonDocument::fromJson(line).object();
                if (obj.value("type") == "progress") status->setText(QStringLiteral("分析 %1/%2：%3").arg(obj.value("current").toInt()).arg(obj.value("total").toInt()).arg(obj.value("file").toString()));
                if (obj.contains("message")) diagnostics->appendPlainText(obj.value("message").toString());
            }
        }
    });
    connect(&process, &QProcess::readyReadStandardError, this, [this] { diagnostics->appendPlainText(QString::fromUtf8(process.readAllStandardError())); });
    connect(&process, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (stopping || error != QProcess::FailedToStart) return;
        status->setText(QStringLiteral("分析器无法启动：") + process.errorString());
        import->setEnabled(true); update->setEnabled(true); cancel->setEnabled(false); emit settled();
    });
    connect(&process, qOverload<int,QProcess::ExitStatus>(&QProcess::finished), this, [this](int exitCode, QProcess::ExitStatus exitStatus) {
        if (stopping) return;
        import->setEnabled(true); update->setEnabled(true); cancel->setEnabled(false);
        if (command == "index") { loadCache(false); return; }
        if (exitCode == 0 && exitStatus == QProcess::NormalExit) {
            QJsonParseError error; auto doc = QJsonDocument::fromJson(output, &error);
            if (error.error == QJsonParseError::NoError && doc.object().value("ok").toBool()) { snapshot = doc.object(); render(); }
            else status->setText(QStringLiteral("无法读取索引结果：") + error.errorString());
        } else status->setText(QStringLiteral("读取索引失败：") + QString::fromUtf8(output));
        if (cancelled) { status->setText(QStringLiteral("已取消，显示上次完整索引；结果尚未更新。")); cancelled = false; }
        if (refreshAfterLoad) { refreshAfterLoad = false; refreshIndex(); }
        else emit settled();
    });
}

SemanticPanel::~SemanticPanel() { stop(); }
bool SemanticPanel::isBusy() const { return process.state() != QProcess::NotRunning; }
void SemanticPanel::stop() {
    stopping = true; refreshAfterLoad = false;
    if (isBusy()) { process.kill(); process.waitForFinished(1500); }
    stopping = false;
}
void SemanticPanel::setProject(const QString &path) {
    stop(); root = path; compilation.clear(); selectedSymbol.clear(); snapshot = {}; output.clear();
    symbols->clear(); relations->clear(); units->clear(); diagnostics->clear();
    { const QSignalBlocker blocker(focusSymbols); focusSymbols->clear(); }
    graph->setData({}, {}, {}, {});
    if (!root.isEmpty() && QFileInfo::exists(VividCode::semanticDirectory(root) + "/config.json")) loadCache(true);
    else status->setText(QStringLiteral("尚未配置 C++ 分析。请导入构建系统生成的 compile_commands.json；其他语言目前仅支持文件浏览。"));
}
void SemanticPanel::importDatabase(const QString &path) {
    if (root.isEmpty() || isBusy()) return;
    if (!QFileInfo::exists(root + "/.vividcode/project.json")) {
        const auto result = VividCode::generate(root);
        if (!result.ok) { status->setText(result.message); return; }
    }
    compilation = path; refreshIndex();
}
void SemanticPanel::refreshIndex() {
    if (compilation.isEmpty()) { status->setText(QStringLiteral("请先导入 compile_commands.json。")); emit settled(); return; }
    cancelled = false; diagnostics->clear(); launch("index", compilation);
}
void SemanticPanel::loadCache(bool refresh) { refreshAfterLoad = refresh; launch("export"); }
void SemanticPanel::launch(const QString &action, const QString &database) {
    if (isBusy()) return;
    command = action; output.clear();
    const QString base = VividCode::sharedRuntime() + "/analyzer";
    QStringList args{base + "/analyzer.py", action, "--project", root};
    if (!database.isEmpty()) args << "--compile-commands" << database;
    process.setProgram(base + "/python/python.exe"); process.setArguments(args);
    auto environment = QProcessEnvironment::systemEnvironment(); environment.remove("VIVIDCODE_LIBCLANG"); environment.remove("LIBCLANG_PATH");
    process.setProcessEnvironment(environment);
    import->setEnabled(false); update->setEnabled(false); cancel->setEnabled(true);
    status->setText(action == "index" ? QStringLiteral("后台检查编译参数和依赖变化…") : QStringLiteral("读取已有索引（尚未检查源码变化）…"));
    process.start();
}
void SemanticPanel::render() {
    const auto state = snapshot.value("state").toObject(); compilation = state.value("compile_commands").toString();
    units->blockSignals(true); units->clear();
    const auto values = snapshot.value("units").toArray();
    for (const auto &value : values) {
        auto u = value.toObject(); units->addItem(QDir(root).relativeFilePath(u.value("file").toString()) + " · " + u.value("config_id").toString().left(8));
    }
    units->blockSignals(false); renderUnit();
    const QString last = state.value("last_status").toString();
    status->setText(last == "ok" ? QStringLiteral("%1 个编译单元 · 新解析 %2，复用 %3。请选择编译单元查看结构，点击关系定位发生位置。").arg(values.size()).arg(state.value("indexed").toInt()).arg(state.value("reused").toInt())
        : QStringLiteral("索引状态：%1。显示上次完整结果，可能已过时。").arg(last));
    for (const auto &e : state.value("errors").toArray()) diagnostics->appendPlainText(e.toObject().value("message").toString());
}
void SemanticPanel::renderUnit() {
    symbols->clear(); selectedSymbol.clear();
    const QSignalBlocker focusBlocker(focusSymbols);
    focusSymbols->clear(); focusSymbols->addItem(QStringLiteral("选择类、函数或成员；可输入名称搜索…"), "");
    const auto values = snapshot.value("units").toArray();
    if (units->currentIndex() < 0 || units->currentIndex() >= values.size()) { relations->clear(); graph->setData({}, {}, {}, {}); return; }
    auto unitData = values.at(units->currentIndex()).toObject();
    QMap<QString,QTreeWidgetItem *> items; QMap<QString,QString> parents;
    for (const auto &value : unitData.value("symbols").toArray()) {
        auto s = value.toObject(); if (s.value("is_external").toBool()) continue;
        QString label = s.value("qualified_name").toString();
        if (!s.value("signature").toString().isEmpty()) label += " · " + s.value("signature").toString();
        focusSymbols->addItem(label, s.value("id").toString());
        auto *item = new QTreeWidgetItem({label, s.value("kind").toString() + ' ' + s.value("access").toString(), location(s, true)});
        item->setData(0,Qt::UserRole,s); items.insert(s.value("id").toString(),item); parents.insert(s.value("id").toString(),s.value("parent_id").toString());
    }
    for (auto i = items.begin(); i != items.end(); ++i) {
        auto *parent = items.value(parents.value(i.key()));
        if (parent && parent != i.value()) parent->addChild(i.value()); else symbols->addTopLevelItem(i.value());
    }
    symbols->expandToDepth(0); renderRelations();
}
void SemanticPanel::renderRelations() {
    relations->clear(); const auto values = snapshot.value("units").toArray();
    if (units->currentIndex() < 0 || units->currentIndex() >= values.size()) { graph->setData({}, {}, {}, {}); return; }
    auto unitData = values.at(units->currentIndex()).toObject(); QMap<QString,QString> names;
    for (const auto &value : unitData.value("symbols").toArray()) { auto s = value.toObject(); names[s.value("id").toString()] = s.value("qualified_name").toString() + " " + s.value("signature").toString(); }
    graph->setData(unitData, selectedSymbol, relationType->currentData().toString(), units->currentText());
    for (const auto &value : unitData.value("relations").toArray()) {
        auto r = value.toObject(); const QString src = r.value("source_id").toString(), dst = r.value("target_id").toString(), type = r.value("type").toString();
        if (!selectedSymbol.isEmpty() && src != selectedSymbol && dst != selectedSymbol) continue;
        if (!relationType->currentData().toString().isEmpty() && type != relationType->currentData().toString()) continue;
        auto *item = new QTreeWidgetItem(relations, {names.value(src,src), relationName(type), names.value(dst,dst), r.value("file").toString() + ':' + QString::number(r.value("line").toInt())});
        item->setData(0,Qt::UserRole,r);
    }
}

void SemanticPanel::focusSymbol(const QString &id) {
    selectedSymbol = id;
    {
        const QSignalBlocker blocker(symbols);
        QTreeWidgetItem *match = nullptr;
        for (QTreeWidgetItemIterator it(symbols); *it; ++it)
            if ((*it)->data(0, Qt::UserRole).toJsonObject().value("id").toString() == id) { match = *it; break; }
        symbols->setCurrentItem(match);
        if (match) symbols->scrollToItem(match);
    }
    {
        const QSignalBlocker blocker(focusSymbols);
        int index = focusSymbols->findData(id);
        if (index < 0 && !id.isEmpty()) {
            const auto values = snapshot.value("units").toArray();
            if (units->currentIndex() >= 0 && units->currentIndex() < values.size()) {
                for (const auto &value : values.at(units->currentIndex()).toObject().value("symbols").toArray()) {
                    const auto s = value.toObject();
                    if (s.value("id").toString() == id) {
                        focusSymbols->addItem(s.value("qualified_name").toString() + " · " + s.value("signature").toString() + QStringLiteral(" · 外部符号"), id);
                        index = focusSymbols->count() - 1; break;
                    }
                }
            }
        }
        focusSymbols->setCurrentIndex(index < 0 ? 0 : index);
    }
    renderRelations();
}
