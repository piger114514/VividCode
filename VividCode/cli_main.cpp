#include "ProjectStore.h"
#include "SharedRuntime.h"
#include "JavaScriptIndexer.h"
#include "JavaIndexer.h"
#include "ConsoleOutput.h"
#include <memory>
#include <QCoreApplication>
#include <QDir>
#include <QTextStream>
#include <QRegularExpression>
#ifdef Q_OS_WIN
#define NOMINMAX
#include <windows.h>
#endif

int main(int argc, char *argv[])
{
#ifdef Q_OS_WIN
    SetConsoleOutputCP(CP_UTF8);
#endif
    QCoreApplication app(argc, argv);
    if (qEnvironmentVariableIsEmpty("VIVIDCODE_HOME")) qputenv("VIVIDCODE_HOME", VividCode::sharedHome().toUtf8());
    QCoreApplication::setApplicationName("vividcode");
    const QStringList args = app.arguments();
    QString command = args.value(1);
    if (command.startsWith("--")) command.remove(0, 1); // Single-dash commands; accept the previous double-dash spelling.
    QTextStream out(stdout), error(stderr);
    out.setEncoding(QStringConverter::Utf8);
    error.setEncoding(QStringConverter::Utf8);
    std::unique_ptr<VividCode::RuntimeReadLock> runtimeLock;
    if (QDir::cleanPath(app.applicationDirPath()).compare(VividCode::sharedRuntime(), Qt::CaseInsensitive) == 0) {
        runtimeLock = std::make_unique<VividCode::RuntimeReadLock>(VividCode::sharedHome());
        if (!runtimeLock->valid()) { error << "Runtime is updating. Retry shortly.\n"; return 1; }
    }
    const QString help = QStringLiteral(
        "vividcode\n"
        "Usage: vividcode <command>\n"
        "  vividcode -g     Refresh project\n"
        "  vividcode -sg    Refresh without file limits\n"
        "  vividcode -c     Set file limit\n"
        "  vividcode -n     Clear old data\n"
        "  vividcode -d     Clear project\n"
        "  vividcode -help  Show help\n"
        "Type stop and press Enter to cancel generation.\n");
    if (args.value(1) == "--javascript-analyzer") return runJavaScriptIndexer(args);
    if (args.value(1) == "--java-analyzer") return runJavaIndexer(args);
    if (args.size() == 1 || (args.size() == 2 && command == "-help")) { out << help; return 0; }
    if (args.size() != 2 || !QStringList{"-g", "-sg", "-d", "-c", "-n"}.contains(command)) {
        error << "Invalid arguments. Use vividcode -help.\n";
        return 2;
    }
    const QString root = QDir::currentPath();
    VividCode::Result result;
    if (command == "-g" || command == "-sg") {
        VividCode::ConsoleProgress progress(out);
        VividCode::GenerationFeedback feedback;
        feedback.progress = [&progress](qint64 completed, qint64 total) { progress.update(completed, total); };
        feedback.warning = [&progress, &error](const QString &message) { progress.finish(); error << VividCode::englishConsoleMessage(message) << Qt::endl; };
        result = VividCode::generate(root, command == "-sg", true, feedback);
        progress.finish();
    }
    else if (command == "-c") {
        out << "Enter file limit: "; out.flush();
        QTextStream input(stdin); input.setEncoding(QStringConverter::Utf8);
        const QString text = input.readLine().trimmed(); bool valid = false;
        const qint64 count = text.toLongLong(&valid);
        if (!valid || count < 1 || count > 2147483647 || text.contains(QRegularExpression("[^0-9]"))) {
            error << "Enter an integer from 1 to 2147483647.\n"; return 2;
        }
        result = VividCode::setFileLimit(count);
        if (result.ok) { out << "File limit set to " << count << ".\n"; return 0; }
    }
    else if (command == "-d") result = VividCode::removeGenerated(root);
    else result = VividCode::cleanHistory(root);
    if (result.ok) out << (command == "-n" ? "Old data cleared." : command == "-d" ? "Project cleared." : "Refreshed.") << Qt::endl;
    else error << VividCode::englishConsoleMessage(result.message) << Qt::endl;
    return result.ok ? 0 : result.message.startsWith(QStringLiteral("已停止生成")) ? 130 : 1;
}
