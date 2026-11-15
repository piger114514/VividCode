#pragma once
#include "SharedRuntime.h"
#include <QtCore>

namespace VividCode {
// Runtime executables stay in the user's installation. Only selected analyzer
// components belong to vividcode; explicit choices never silently fall back.
inline QJsonObject analysisEnvironment() {
    QFile file(sharedHome() + "/environments.json");
    if (!file.exists()) return {};
    if (QFileInfo(file).isSymLink() || QFileInfo(file).isJunction() || !file.open(QIODevice::ReadOnly) || file.size() > 65536)
        throw std::runtime_error("无法读取分析环境配置。");
    QJsonParseError error; const auto doc = QJsonDocument::fromJson(file.readAll(), &error);
    if (error.error != QJsonParseError::NoError || !doc.isObject()) throw std::runtime_error("分析环境配置已损坏。");
    return doc.object();
}
inline QString environmentChoice(const QString &name) {
    const QString variable = "VIVIDCODE_" + name.toUpper();
    return qEnvironmentVariable(variable.toUtf8().constData(), analysisEnvironment().value(name).toString()).trimmed();
}
inline QString localExecutable(const QString &name, const QStringList &commands) {
    const QString configured = environmentChoice(name);
    if (!configured.isEmpty()) {
        if (!QFileInfo(configured).isAbsolute() || !QFileInfo(configured).isFile())
            throw std::runtime_error((QStringLiteral("本机 %1 路径无效：%2。请在“分析环境…”中修正。").arg(name, configured)).toStdString());
        return QFileInfo(configured).canonicalFilePath();
    }
    for (const auto &command : commands) {
        const QString found = QStandardPaths::findExecutable(command);
        if (!found.isEmpty() && !QDir::fromNativeSeparators(found).contains("/Microsoft/WindowsApps/", Qt::CaseInsensitive)) return found;
    }
    throw std::runtime_error((QStringLiteral("未找到本机 %1。请自行安装并加入 PATH，或在“分析环境…”指定路径；vividcode 不会自动安装。").arg(name)).toStdString());
}
inline QString probeExecutable(const QString &program, const QStringList &args) {
    QProcess process; auto env = QProcessEnvironment::systemEnvironment();
    env.remove("NODE_OPTIONS"); env.remove("NODE_PATH");
    for (const auto &key : {"JAVA_TOOL_OPTIONS", "_JAVA_OPTIONS", "JDK_JAVA_OPTIONS", "CLASSPATH"}) env.remove(key);
    process.setProcessEnvironment(env);
    process.start(program, args);
    if (!process.waitForFinished(5000)) {
        const QString message = QStringLiteral("本机运行环境检查失败或超时：%1 · %2").arg(program, process.errorString());
        process.kill(); process.waitForFinished(1000); throw std::runtime_error(message.toStdString());
    }
    if (process.exitStatus() != QProcess::NormalExit || process.exitCode())
        throw std::runtime_error((QStringLiteral("运行环境不可用：%1\n%2").arg(program, QString::fromUtf8(process.readAllStandardError()))).toStdString());
    return QString::fromUtf8(process.readAllStandardOutput()).trimmed();
}
struct PythonCommand { QString program; QStringList prefix; };
inline PythonCommand localPython() {
    const QString program = localExecutable("python", {"python.exe", "python3.exe", "py.exe"});
    const QStringList prefix = QFileInfo(program).baseName().compare("py", Qt::CaseInsensitive) == 0 ? QStringList{"-3", "-E", "-s"} : QStringList{"-E", "-s"};
    probeExecutable(program, prefix + QStringList{"-c", "import sys,sqlite3; assert sys.version_info >= (3,12) and sys.maxsize > 2**32, '需要 Python 3.12+ x64'; print(sys.version.split()[0])"});
    return {program, prefix};
}
inline QString localNode() {
    const QString program = localExecutable("node", {"node.exe"});
    const QString version = probeExecutable(program, {"-p", "process.versions.node + ' ' + process.arch"});
    if (!QRegularExpression("^(22|24)\\..* x64$").match(version).hasMatch())
        throw std::runtime_error((QStringLiteral("JS/TS 组件当前支持 Node.js 22/24 x64，本机为 %1。请指定兼容环境。").arg(version)).toStdString());
    return program;
}
inline QString localJava() {
    QString program;
    if (!environmentChoice("java").isEmpty()) program = localExecutable("java", {});
    else {
        const QString javaHome = qEnvironmentVariable("JAVA_HOME");
        if (!javaHome.isEmpty()) program = QDir(javaHome).filePath("bin/java.exe");
        else {
            QSettings registry("HKEY_LOCAL_MACHINE\\SOFTWARE\\JavaSoft\\JDK", QSettings::NativeFormat);
            const QString current = registry.value("CurrentVersion").toString();
            const QString home = registry.value(current + "/JavaHome").toString();
            if (!home.isEmpty()) program = QDir(home).filePath("bin/java.exe");
            else program = localExecutable("java", {"java.exe"});
        }
    }
    if (!QFileInfo(program).isFile()) throw std::runtime_error("JDK java.exe 不存在，请在分析环境中配置完整 JDK 17+。");
    program = QFileInfo(program).canonicalFilePath();
    const QString home = QFileInfo(program).absolutePath() + "/..";
    if (!QFileInfo::exists(home + "/bin/javac.exe") || !QFileInfo::exists(home + "/lib/modules"))
        throw std::runtime_error("Java 分析需要完整 JDK 17+；请选择 JDK/bin/java.exe。");
    const QString version = probeExecutable(home + "/bin/javac.exe", {"-version"});
    const auto match = QRegularExpression("javac ([0-9]+)").match(version);
    if (!match.hasMatch() || match.captured(1).toInt() < 17) throw std::runtime_error("Java 分析需要 JDK 17+。");
    return program;
}
inline QString localLibClang() {
    QString configured = environmentChoice("libclang");
    if (configured.isEmpty()) configured = qEnvironmentVariable("LIBCLANG_PATH");
    if (!configured.isEmpty()) {
        if (QFileInfo(configured).isDir()) configured = QDir(configured).filePath("libclang.dll");
        if (QFileInfo(configured).isFile()) return QFileInfo(configured).canonicalFilePath();
        throw std::runtime_error("指定的本机 libclang.dll 不存在，请修正分析环境。");
    }
    const QString clang = QStandardPaths::findExecutable("clang.exe");
    if (!clang.isEmpty() && QFileInfo::exists(QFileInfo(clang).absolutePath() + "/libclang.dll")) return QFileInfo(clang).absolutePath() + "/libclang.dll";
    throw std::runtime_error("C++ 分析需要本机 LLVM/libclang.dll。请自行安装 LLVM，再在“分析环境…”指定 DLL；只有 MSVC 不保证带 libclang。");
}
inline bool saveAnalysisEnvironment(const QJsonObject &settings, QString *error) {
    const QString home = sharedHome();
    for (QString part = home; !part.isEmpty();) {
        const QFileInfo info(part); if (info.isSymLink() || info.isJunction()) { *error = QStringLiteral("环境配置目录不能是链接。"); return false; }
        const QString parent = info.absolutePath(); if (parent == part) break; part = parent;
    }
    if (!QDir().mkpath(home)) { *error = QStringLiteral("无法创建环境配置目录。"); return false; }
    const QString path = home + "/environments.json";
    if (QFileInfo(path).isSymLink() || QFileInfo(path).isJunction()) { *error = QStringLiteral("环境配置文件不能是链接。"); return false; }
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(QJsonDocument(settings).toJson()) < 0 || !file.commit()) { *error = file.errorString(); return false; }
    return true;
}
}
