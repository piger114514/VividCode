#pragma once
#include "SourcePopup.h"
#include "SemanticPanel.h"
#include "UnifiedGraphStore.h"
#include <stdexcept>

inline int runSourceHighlightTest(QApplication &app) {
    using H = SourceHighlighter;
    const auto check = [](bool ok, const char *message) { if (!ok) throw std::runtime_error(message); };
    const auto mark = [](int line, int col, const QString &name, const QString &kind, const QString &language = "c#", bool declaration = true) {
        return QJsonObject{{"line", line}, {"col", col}, {"name", name}, {"kind", kind}, {"language", language}, {"declaration", declaration}};
    };
    const auto foreground = [](QTextDocument *doc, int line, const QString &word, int from = 0) {
        const auto block = doc->findBlockByNumber(line - 1); const int at = block.text().indexOf(word, from);
        if (at < 0) return QColor();
        for (const auto &range : block.layout()->formats()) if (at >= range.start && at < range.start + range.length) return range.format.foreground().color();
        return QColor();
    };
    try {
        QTextDocument doc; H highlighter(&doc);
        const auto load = [&](const QString &path, const QString &text, const QJsonArray &marks = {}) {
            highlighter.configure(path, marks); doc.setPlainText(text); highlighter.rehighlight();
            check(doc.toPlainText() == text, "highlighting must not modify source");
        };
        load("Sample.cs", "class Worker {\n  int value;\n  int Run(int value) { return this.value + value; }\n  string text = \"value // class\"; // value\n}", {
            mark(2, 7, "value", "FieldDecl"), mark(3, 15, "value", "ParmDecl"),
            mark(3, 31, "value", "FieldDecl", "c#", false), mark(3, 44, "value", "ParmDecl", "c#", false),
            mark(4, 18, "value", "FieldDecl")});
        check(foreground(&doc, 1, "class") == H::color(H::Keyword) && foreground(&doc, 1, "Worker") == H::color(H::Type), "class syntax colors");
        check(foreground(&doc, 3, "Run") == H::color(H::Function), "function syntax color");
        check(foreground(&doc, 2, "value") == H::color(H::Member), "indexed member declaration");
        check(foreground(&doc, 3, "value") == H::color(H::Parameter), "same-name parameter differs from member");
        check(foreground(&doc, 3, "value", 25) == H::color(H::Member), "indexed receiver member occurrence");
        check(foreground(&doc, 3, "value", 40) == H::color(H::Parameter), "indexed parameter reference");
        check(foreground(&doc, 4, "value") == H::color(H::String) && foreground(&doc, 4, "value", 35) == H::color(H::Comment), "semantic marks cannot color strings or comments");
        highlighter.setMode(H::Syntax);
        check(foreground(&doc, 2, "value") == H::color(H::Variable), "syntax mode does not apply semantic marks");
        highlighter.setMode(H::Disabled);
        check(doc.firstBlock().layout()->formats().isEmpty(), "plain mode clears all highlighting");
        highlighter.setMode(H::Semantic);

        load("a.cpp", "/* class A\nvalue */ int count = 42;\nauto raw = R\"tag(a // b\nclass fake)tag\"; int after;\nint value;", {mark(5, 5, "different", "FieldDecl", "c++")});
        check(foreground(&doc, 2, "value") == H::color(H::Comment) && foreground(&doc, 2, "42") == H::color(H::Number), "multiline comment ends before code");
        check(foreground(&doc, 4, "class") == H::color(H::String) && foreground(&doc, 4, "after") == H::color(H::Variable), "C++ raw string delimiter survives blocks");
        check(foreground(&doc, 5, "value") == H::color(H::Variable), "stale mismatched symbol names are ignored");
        const QString unicode = QString::fromUtf8("auto 中文 = 1; int value;");
        load("unicode.cpp", unicode, {mark(1, int(unicode.left(unicode.indexOf("value")).toUtf8().size()) + 1, "value", "FieldDecl", "c++")});
        check(foreground(&doc, 1, "value") == H::color(H::Member), "C++ UTF-8 byte columns converted to UTF-16");
        load("sample.py", "class Sample:\n    def run(self, value):\n        \"\"\"class fake\n        # value\"\"\"\n        return value # comment", {mark(2, 5, "run", "CXXMethod", "python"), mark(2, 19, "value", "ParmDecl", "python")});
        check(foreground(&doc, 2, "value") == H::color(H::Parameter), "Python parameter UTF-16 location");
        check(foreground(&doc, 4, "value") == H::color(H::String) && foreground(&doc, 5, "return") == H::color(H::Control), "Python triple strings preserve comment precedence");
        load("sample.ts", "const pattern = /[/*]class/gi;\nconst text = `class fake\n// inside template`; function run(value) { return value / 2; }");
        check(foreground(&doc, 1, "class") == H::color(H::String), "JS regex is not a block comment");
        check(foreground(&doc, 3, "inside") == H::color(H::String) && foreground(&doc, 3, "run") == H::color(H::Function), "JS template spans and closing delimiter");
        load("sample.cs", "var text = @\"one \"\"two\nclass fake\"; int count;\nvar raw = \"\"\"class\n/* value */\"\"\"; int after;");
        check(foreground(&doc, 2, "class") == H::color(H::String) && foreground(&doc, 2, "count") == H::color(H::Variable), "C# verbatim multiline and escaped quotes");
        check(foreground(&doc, 4, "value") == H::color(H::String) && foreground(&doc, 4, "after") == H::color(H::Variable), "C# raw multiline string");
        load("Sample.java", "class Sample {\n  String text = \"\"\"\n  // class fake\n  \"\"\";\n  int value;\n}", {mark(5, 3, "value", "FieldDecl", "java")});
        check(foreground(&doc, 3, "class") == H::color(H::String) && foreground(&doc, 5, "value") == H::color(H::Member), "Java text blocks and declaration-start offsets");
        load("other.txt", "class Sample; /* value");
        check(doc.firstBlock().layout()->formats().isEmpty(), "unknown language and prior block state reset");
        qInfo() << "PASS: five-language syntax, semantic locations, shadowing, Unicode, multiline strings/comments, stale marks and modes";

        QTemporaryDir temporary; check(temporary.isValid(), "temporary fixture");
        const auto oldHome = qgetenv("VIVIDCODE_HOME");
        struct Restore { QByteArray home; ~Restore() { if (home.isNull()) qunsetenv("VIVIDCODE_HOME"); else qputenv("VIVIDCODE_HOME", home); } } restore{oldHome};
        qputenv("VIVIDCODE_HOME", (temporary.path() + "/home").toUtf8());
        const QString root = temporary.path() + "/project"; QDir().mkpath(root);
        const QString cache = VividCode::semanticDirectory(root); QDir().mkpath(cache);
        QFile owner(cache + "/project.json"); check(owner.open(QIODevice::WriteOnly), "fixture ownership file");
        owner.write(QJsonDocument(QJsonObject{{"app", "vividcode"}, {"schema", 1}, {"projectRoot", root}}).toJson()); owner.close();
        const QJsonObject symbol{{"id", "field"}, {"name", "value"}, {"kind", "FieldDecl"}, {"def_file", "sample.cpp"}, {"def_line", 2}, {"def_col", 7}};
        const QJsonObject relation{{"source_id", "caller"}, {"target_id", "field"}, {"type", "member_ref"}, {"file", root + "/sample.cpp"}, {"line", 3}, {"col", 5}};
        { UnifiedGraphStore store(root); check(store.saveLanguage("c++", {{"state", QJsonObject{{"last_status", "ok"}}}, {"units", QJsonArray{QJsonObject{{"symbols", QJsonArray{symbol}}, {"relations", QJsonArray{relation}}}}}}), "seed semantic graph"); }
        SemanticPanel panel; panel.setProject(root, {}, false);
        check(panel.sourceHighlights("sample.cpp").size() == 2 && panel.sourceHighlights("other.cpp").isEmpty(), "project snapshot connects declarations and references only for requested file");
        panel.findChild<QComboBox *>("semanticRelationType")->setCurrentIndex(1);
        check(panel.sourceHighlights("sample.cpp").size() == 2, "graph relation filters do not remove source colors");

        QWidget parent; parent.resize(1000, 700); parent.show(); SourcePopup popup(&parent);
        const QString preview = "// Source preview - semantic highlighting\nclass Worker {\n    int count = 42;\n    int Run(int amount) {\n        return count + amount;\n    }\n};\n";
        const QJsonArray positions{mark(3, 9, "count", "FieldDecl", "c++"), mark(4, 17, "amount", "ParmDecl", "c++"),
            mark(5, 16, "count", "FieldDecl", "c++", false), mark(5, 24, "amount", "ParmDecl", "c++", false)};
        popup.showCode(preview, 4, 9, QPoint(30, 30), "sample.cpp", positions); app.processEvents();
        auto *selector = popup.findChild<QComboBox *>("sourceHighlightMode");
        check(selector && popup.viewer()->isReadOnly() && popup.viewer()->toPlainText() == preview, "popup preserves read-only source");
        selector->showPopup(); app.processEvents();
        check(popup.isVisible() && selector->view()->isVisible(), "opening mode menu keeps source popup visible");
        selector->hidePopup(); app.processEvents();
        check(popup.isVisible(), "closing mode menu keeps source popup visible");
        selector->setCurrentIndex(2); check(popup.viewer()->document()->firstBlock().layout()->formats().isEmpty(), "popup plain mode");
        selector->setCurrentIndex(0); check(foreground(popup.viewer()->document(), 3, "count") == H::color(H::Member), "popup semantic mode");
        const QString screenshot = qEnvironmentVariable("VIVIDCODE_HIGHLIGHT_SCREENSHOT");
        if (!screenshot.isEmpty()) check(popup.grab().save(screenshot), "save highlighted preview");
        popup.close(); popup.showCode("int value;", 1, 1, QPoint(30, 30), "other.cpp");
        check(foreground(popup.viewer()->document(), 1, "value") == H::color(H::Variable), "reopened popup clears previous symbols");
        qInfo() << "PASS: semantic snapshot wiring, popup modes, plain-text preservation and reopen reset";
        return 0;
    } catch (const std::exception &error) { qCritical() << "FAIL:" << error.what(); return 1; }
}
