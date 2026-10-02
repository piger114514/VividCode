#include "ProjectStore.h"
#include "SharedRuntime.h"
#include <memory>
#include <QCoreApplication>
#include <QDir>
#include <QTextStream>
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
        if (!runtimeLock->valid()) { error << QStringLiteral("共享程序正在更新，请稍后重试。\n"); return 1; }
    }
    const QString help = QStringLiteral(
        "vividcode — 代码项目浏览器\n"
        "在目标项目目录执行：\n"
        "  vividcode -g    生成/更新 .vividcode 启动器与 JSON 记录\n"
        "  vividcode -d    删除当前 .vividcode 及本项目共享索引\n"
        "  vividcode -c    清理旧记录\n"
        "  vividcode -help 显示帮助\n");
    if (args.size() == 1 || (args.size() == 2 && command == "-help")) { out << help; return 0; }
    if (args.size() != 2 || !QStringList{"-g", "-d", "-c"}.contains(command)) {
        error << QStringLiteral("参数错误。\n") << help;
        return 2;
    }
    const QString root = QDir::currentPath();
    VividCode::Result result;
    if (command == "-g") result = VividCode::generate(root);
    else if (command == "-d") result = VividCode::removeGenerated(root);
    else result = VividCode::cleanHistory(root);
    (result.ok ? out : error) << result.message << Qt::endl;
    return result.ok ? 0 : 1;
}
