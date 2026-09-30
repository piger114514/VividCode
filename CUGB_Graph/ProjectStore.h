#pragma once
#include <QString>

namespace VividCode {
struct Result {
    bool ok;
    QString message;
};

// 所有路径以传入项目根目录为边界；命令行始终传入当前工作目录。
Result generate(const QString &projectRoot, const QString &runtimeDirectory);
Result removeGenerated(const QString &projectRoot);
Result cleanHistory(const QString &projectRoot);
Result recordIfInitialized(const QString &projectRoot);
}
