#pragma once
#include <QString>
#include <functional>

namespace VividCode {
struct Result {
    bool ok;
    QString message;
};

struct GenerationFeedback {
    // total < 0 while discovering files; completed counts fully hashed files.
    std::function<void(qint64 completed, qint64 total)> progress;
    std::function<void(const QString &message)> warning;
};

// 所有路径以传入项目根目录为边界；命令行始终传入当前工作目录。
Result generate(const QString &projectRoot, bool force = false, bool consoleInput = false,
    const GenerationFeedback &feedback = {});
Result setFileLimit(qint64 count);
Result removeGenerated(const QString &projectRoot);
Result cleanHistory(const QString &projectRoot);
Result recordIfInitialized(const QString &projectRoot);
}
