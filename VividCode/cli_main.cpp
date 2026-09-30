#include "ProjectStore.h"
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
    QTextStream out(stdout), error(stderr);
    out.setEncoding(QStringConverter::Utf8);
    error.setEncoding(QStringConverter::Utf8);
    const QString help = QStringLiteral(
        "vividcode — 代码项目浏览器\n"
        "在目标项目目录执行：\n"
        "  vividcode --g    生成/更新 .vividcode 运行文件与代码文档记录\n"
        "  vividcode --d    删除当前目录下的 .vividcode\n"
        "  vividcode --c    清理旧记录，仅保留最新成功保存的一份\n"
        "  vividcode --help 显示帮助\n");
    if (args.size() == 1 || (args.size() == 2 && args.at(1) == "--help")) { out << help; return 0; }
    if (args.size() != 2 || !QStringList{"--g", "--d", "--c"}.contains(args.at(1))) {
        error << QStringLiteral("参数错误：一次只能执行一个命令。\n") << help;
        return 2;
    }
    const QString root = QDir::currentPath();
    VividCode::Result result;
    if (args.at(1) == "--g") result = VividCode::generate(root, QCoreApplication::applicationDirPath());
    else if (args.at(1) == "--d") result = VividCode::removeGenerated(root);
    else result = VividCode::cleanHistory(root);
    (result.ok ? out : error) << result.message << Qt::endl;
    return result.ok ? 0 : 1;
}
