#pragma once
#include "UiLanguage.h"
#include "VividCode.h"
#include "AiExplanation.h"
#include "SourcePopup.h"
#include "RelationGraph.h"
#include <QtWidgets>

inline int verifyUiLanguage(QApplication &app, const QString &expected) {
    VividCodeWindow window;
    const auto *open = window.findChild<QAction *>("openProjectAction");
    const auto *selector = window.findChild<QComboBox *>("interfaceLanguage");
    return app.property("vividcode.uiLanguage").toString() == expected && open && selector &&
        selector->currentData().toString() == expected && open->text() ==
        (expected == "zh" ? QStringLiteral("打开项目…") : QStringLiteral("Open Project...")) ? 0 : 1;
}

inline int runUiLanguageTest(QApplication &app) {
    const auto check = [](bool value, const char *message) { if (!value) throw std::runtime_error(message); };
    QTemporaryDir temporary;
    const auto oldHome = qgetenv("VIVIDCODE_HOME");
    struct Restore { QByteArray home; ~Restore() { qputenv("VIVIDCODE_HOME", home); } } restore{oldHome};
    qputenv("VIVIDCODE_HOME", temporary.path().toUtf8());
    try {
        check(temporary.isValid(), "temporary language settings");
        check(UiLanguage::preference() == "en", "default remains English");
        check(!UiLanguage::save("invalid").isEmpty(), "invalid language rejected");
        for (const QString &language : {QString("zh"), QString("en")}) {
            check(UiLanguage::save(language).isEmpty(), "save language");
            check(UiLanguage::preference() == language, "language persistence");
            UiLanguage::initialize(app, language);
            check(verifyUiLanguage(app, language) == 0, "main window uses selected language");
            check(relationDisplayName("calls") == (language == "zh" ? QStringLiteral("直接调用") : QStringLiteral("Direct call")), "graph legend uses active language");
            VividCode::AiSettings settings;
            check(settings.resolvedLanguage() == language, "automatic AI follows active interface");
            settings.language = language == "zh" ? "en" : "zh";
            check(settings.resolvedLanguage() == settings.language, "explicit AI language stays independent");
            VividCodeWindow window; window.show(); app.processEvents();
            auto *selector = window.findChild<QComboBox *>("interfaceLanguage");
            const QString next = language == "zh" ? "en" : "zh";
            selector->setCurrentIndex(selector->findData(next));
            check(UiLanguage::preference() == next, "toolbar saves next-start preference");
            check(app.property("vividcode.uiLanguage").toString() == language, "active tasks retain current language until restart");
            QProcess child;
            auto environment = QProcessEnvironment::systemEnvironment(); environment.remove("VIVIDCODE_UI_LANGUAGE");
            child.setProcessEnvironment(environment);
            child.start(QCoreApplication::applicationFilePath(), {"--verify-ui-language", next, "-platform", "offscreen"});
            check(child.waitForFinished(15000) && child.exitStatus() == QProcess::NormalExit && child.exitCode() == 0, "fresh process restores selected interface language");
            selector->setCurrentIndex(selector->findData(language));
            SourcePopup popup(&window);
            const QString source = QStringLiteral("// 用户源码 keeps its original text\nint value = 42;");
            popup.showCode(source, 2, 1, QPoint(40, 80), "test.cpp");
            check(popup.viewer()->toPlainText() == source, "translations never modify source content");
            check(popup.findChild<QComboBox *>("sourceHighlightMode")->itemText(0) ==
                (language == "zh" ? QStringLiteral("语义高亮") : QStringLiteral("Semantic highlighting")), "source mode translated");
            popup.close();
            bool inspected = false;
            QTimer::singleShot(0, &window, [&] {
                auto *dialog = window.findChild<QDialog *>("aiSettingsDialog");
                if (!dialog) return;
                auto *choice = dialog->findChild<QComboBox *>("aiLanguage");
                auto *buttons = dialog->findChild<QDialogButtonBox *>();
                inspected = choice && choice->itemText(0) == (language == "zh" ? QStringLiteral("跟随界面语言") : QStringLiteral("Follow interface")) &&
                    buttons->button(QDialogButtonBox::Cancel)->text().remove('&') == (language == "zh" ? QStringLiteral("取消") : QStringLiteral("Cancel"));
                dialog->reject();
            });
            VividCode::editAiSettings(&window); check(inspected, "AI settings and standard buttons translated");
            auto *graph = window.findChild<RelationGraph *>("realRelationGraph");
            QJsonArray examples;
            for (const QString &extension : {"cpp", "cs", "py", "js", "ts", "java"}) {
                const QString file = "example." + extension;
                const QString sourceLanguage = extension == "cs" ? "c#" : extension == "py" ? "python" : extension == "java" ? "java" :
                    (extension == "js" || extension == "ts") ? "javascript" : "c++";
                examples.append(QJsonObject{{"id", file}, {"name", file}, {"kind", "RelatedFile"}, {"language", sourceLanguage}, {"def_file", file}, {"def_line", 1}});
            }
            graph->setData({{"symbols", examples}, {"relations", QJsonArray()}}, {}, {}, {});
            const auto *fileLegend = graph->findChild<QLabel *>("fileLanguageLegend");
            check(fileLegend && fileLegend->text().startsWith(language == "zh" ? QStringLiteral("文件<br>") : QStringLiteral("Files<br>")), "file color legend heading translated");
            for (const auto &name : {"C/C++", "C#", "Python", "JavaScript", "TypeScript", "Java"})
                check(fileLegend->text().contains(name), "all six language colors present in legend");
            for (const QString &kind : {"ClassDecl", "EnumDecl", "CXXMethod"}) {
                graph->setData({{"symbols", QJsonArray{QJsonObject{{"id", "sample"}, {"name", "Sample"}, {"kind", kind}, {"language", "c#"}, {"def_file", "Sample.cs"}}}}}, {}, {}, {});
                check(fileLegend->text().contains("C#"), "class enum and method views retain source language legend");
            }
            graph->setData({{"symbols", QJsonArray{QJsonObject{{"id", "fallback"}, {"name", "Sample"}, {"kind", "ClassDecl"}, {"def_file", "Sample.cs"}}}}}, {}, {}, {});
            check(fileLegend->text().contains("C#"), "source extension fallback without language metadata");
            graph->setData({}, {}, {}, {});
            check(!fileLegend->text().contains("C#") && fileLegend->text().contains(UiLanguage::text("No recognized source language in this view")), "empty view clears previous legend without implying missing components");
            graph->setData({{"symbols", examples}, {"relations", QJsonArray()}}, {}, {}, {});
            auto *legendCard = graph->findChild<QWidget *>("graphLegendCard");
            QString legendText; for (auto *label : legendCard->findChildren<QLabel *>()) legendText += label->text();
            check(language == "zh" ? legendText.contains(QStringLiteral("直接调用")) :
                (!legendText.contains(QRegularExpression(QStringLiteral("[\\x{4e00}-\\x{9fff}]"))) && legendText.contains("Direct call") && legendText.contains("Referenced → Referrer")), "full populated legend has no mixed-language text");
            app.processEvents();
            const auto preview = qEnvironmentVariable("VIVIDCODE_LANGUAGE_SCREENSHOTS");
            if (!preview.isEmpty()) {
                check(window.grab().save(preview + "/interface-" + language + ".png"), "language screenshot");
                check(legendCard->grab().save(preview + "/legend-" + language + ".png"), "populated legend screenshot");
            }
        }
        QFile corrupt(UiLanguage::settingsPath()); check(corrupt.open(QIODevice::WriteOnly), "corrupt fixture"); corrupt.write("invalid JSON"); corrupt.close();
        check(UiLanguage::preference() == "en", "damaged preference falls back safely");
        qInfo() << "PASS: Chinese/English UI, toolbar, persistence across fresh processes, AI language, source preservation, dialogs, corrupt-settings fallback";
        return 0;
    } catch (const std::exception &error) { qCritical() << "FAIL:" << error.what(); return 1; }
}
