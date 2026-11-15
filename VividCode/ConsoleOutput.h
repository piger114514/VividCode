#pragma once
#include <QtCore>
#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace VividCode {
// Keep command-line wording separate from the shared store's UI messages.
inline QString englishConsoleMessage(const QString &message) {
    struct Translation { const char *source; const char *english; };
    static const Translation translations[] = {
        {"已停止生成：本次临时目录已清除，没有留下 .vividcode 或记录。", "Cancelled."},
        {"已停止生成：原 .vividcode、历史记录及语义索引未修改，本次记录已丢弃。", "Cancelled."},
        {"目录嵌套过深，已停止操作：", "Directory nesting limit exceeded: "},
        {"拒绝操作符号链接或目录联接：", "Linked path rejected: "},
        {"无法读取目录：", "Cannot read directory: "},
        {"项目目录不可用：", "Project directory is unavailable: "},
        {"请在项目根目录执行命令，不要在 .vividcode 内执行。", "Run from the project root, not inside .vividcode."},
        {"拒绝覆盖链接：", "Cannot overwrite linked path: "},
        {"锁文件不能是链接：", "Lock file cannot be a link: "},
        {"另一个 vividcode 操作正在进行，或项目目录不可写。", "Project is busy or not writable."},
        {"语义分析正在运行，请取消分析后重试。", "Analysis is running. Cancel it and retry."},
        {".vividcode 已存在但不是文件夹。", ".vividcode is not a directory."},
        {"写入失败：", "Write failed: "},
        {"无法读取记录：", "Cannot read record: "},
        {"记录 JSON 已损坏：", "Invalid record JSON: "},
        {".vividcode 不是受支持的 vividcode 项目。", "Unsupported .vividcode project."},
        {"历史记录 ID 无效或重复，未执行清理。", "Invalid or duplicate history IDs. Nothing cleared."},
        {"最新记录指针无效。", "Invalid latest record reference."},
        {"记录 ID 无效。", "Invalid record ID."},
        {"记录内容无效：", "Invalid record: "},
        {"扫描达到上限。可用 vividcode -c 更改全局文件数量上限，或谨慎使用 vividcode -sg。", "File limit reached. Use vividcode -c or -sg."},
        {"源码文件在扫描后变成链接：", "Source became a link: "},
        {"源码读取失败或扫描期间发生变化，请重试：", "Source changed or became unreadable. Retry: "},
        {"源码读取失败：", "Cannot read source: "},
        {"无法创建记录目录。", "Cannot create record directory."},
        {"拒绝复制链接：", "Cannot copy linked path: "},
        {"无法创建运行目录：", "Cannot create runtime directory: "},
        {"无法复制运行文件：", "Cannot copy runtime file: "},
        {"运行文件复制失败：", "Runtime copy failed: "},
        {"无法更新运行文件，请先关闭查看器：", "Close the viewer before updating: "},
        {"无法保留文件修改时间：", "Cannot preserve modification time: "},
        {"无法保留文件属性：", "Cannot preserve file attributes: "},
        {"拒绝通过链接访问共享目录：", "Linked shared path rejected: "},
        {"共享索引归属不匹配，未修改：", "Index ownership mismatch. Nothing changed: "},
        {"无法创建共享索引目录：", "Cannot create index directory: "},
        {"无法校验迁移文件：", "Cannot verify migration file: "},
        {"旧语义目录含未知文件，已保留：", "Unknown legacy index file preserved: "},
        {"旧索引与共享索引不同，已保留两份数据，请先解决冲突。", "Index conflict. Both copies preserved."},
        {"索引迁移校验失败，旧文件已保留。", "Migration verification failed. Old files preserved."},
        {"共享索引已校验，但旧索引清理失败，请重试。", "Index verified; legacy cleanup failed. Retry."},
        {"无法清理旧运行目录：", "Cannot clear legacy runtime directory: "},
        {"无法清理旧运行依赖：", "Cannot clear legacy dependency: "},
        {"无法清理旧文档：", "Cannot clear legacy document: "},
        {"临时数据清理失败，原项目仍保留：", "Temporary cleanup failed; data preserved: "},
        {"无法暂存原目录，原数据未修改：", "Cannot stage original directory; data unchanged: "},
        {"无法提交生成结果：", "Cannot publish refresh: "},
        {"恢复原目录失败，原数据保存在：", "Restore failed. Original data is at: "},
        {"请先关闭此项目的 vividcode 查看器，再生成或更新程序文件。", "Close this project's viewer and retry."},
        {"共享运行环境未安装或正在更新，请先运行 install-command.ps1。", "Runtime unavailable. Run install-command.ps1."},
        {"共享运行包不完整，请重新运行 install-command.ps1：", "Incomplete runtime. Reinstall: "},
        {"共享安装信息无效，请重新安装。", "Invalid runtime installation. Reinstall."},
        {"无法创建生成临时目录。", "Cannot create refresh staging directory."},
        {"无法准备索引迁移。", "Cannot prepare index migration."},
        {"请输入 1 到 2147483647 之间的源码文件数量。", "Enter an integer from 1 to 2147483647."},
        {"无法创建全局设置目录。", "Cannot create settings directory."},
        {"另一个源码范围设置正在保存。", "File limit settings are busy. Retry."},
        {"请先关闭此项目的 vividcode 查看器。", "Close this project's viewer and retry."},
        {"请先关闭此项目的 AI 查看器，再清理缓存。", "Close this project's viewer and retry."},
        {"AI 缓存归属不匹配，未删除：", "AI cache ownership mismatch. Nothing cleared: "},
        {"AI 解释缓存正在使用或不可写，请关闭查看器后重试。", "AI cache is busy or not writable. Close the viewer and retry."},
        {"共享索引清理失败，请关闭分析进程后重试。", "Cannot clear index. Close analysis processes and retry."},
        {"删除未完成：文件可能被占用或不可写，请关闭查看器后重试。", "Project cleanup incomplete. Close the viewer and check permissions."},
        {"当前目录尚未初始化，请先运行 vividcode -g。", "No project data. Run vividcode -g first."},
        {"没有完整的最新记录，请先运行 vividcode -g。", "No complete latest record. Run vividcode -g first."},
        {"历史清理未完成，可重试：", "History cleanup incomplete. Retry: "},
        {"无法读取源码范围设置。", "Cannot read file limit settings."},
        {"源码范围设置 JSON 已损坏。", "Invalid file limit settings JSON."},
        {"源码文件数量上限无效，请用 vividcode -c 设置正整数。", "Invalid file limit. Use vividcode -c."}
    };
    QStringList result;
    for (const auto &line : message.split('\n')) {
        QString translated = line;
        for (const auto &entry : translations) {
            const QString prefix = QString::fromUtf8(entry.source);
            if (!line.startsWith(prefix)) continue;
            translated = QString::fromLatin1(entry.english) + line.mid(prefix.size());
            // QFile's localized OS detail is not part of a path. Keep the path,
            // use an English fallback when Windows returns a non-English reason.
            const int detail = translated.indexOf(" / ");
            if (detail >= 0 && translated.mid(detail + 3).contains(QRegularExpression("[\\x{3400}-\\x{9fff}]")))
                translated = translated.left(detail) + " / File system error.";
            break;
        }
        result.append(translated);
    }
    return result.join('\n');
}

class ConsoleProgress {
    QTextStream &output;
    QElapsedTimer timer;
    bool terminal = false, active = false;
    QString previous;
public:
    explicit ConsoleProgress(QTextStream &stream) : output(stream) {
#ifdef Q_OS_WIN
        DWORD mode = 0; terminal = GetConsoleMode(GetStdHandle(STD_OUTPUT_HANDLE), &mode);
#endif
        timer.start();
    }
    void update(qint64 completed, qint64 total) {
        const bool first = !active;
        const bool final = total >= 0 && completed == total;
        if (!first && !final && (!terminal || timer.elapsed() < 100)) return;
        const QString line = total < 0 ? QString("Generating: %1").arg(completed) : QString("Generating: %1/%2").arg(completed).arg(total);
        if (line == previous) return;
        if (terminal) output << '\r' << line << QString(qMax(0, previous.size() - line.size()), ' ');
        else output << line << '\n';
        output.flush(); previous = line; active = true; timer.restart();
    }
    void finish() {
        if (active && terminal) { output << '\n'; output.flush(); }
        active = false;
    }
};
}
