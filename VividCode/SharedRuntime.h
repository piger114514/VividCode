#pragma once
#include <QtCore>
#include <stdexcept>
#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace VividCode {
// VIVIDCODE_HOME is a development/test override; production uses the current user.
inline QString sharedHome() {
    const QString configured = qEnvironmentVariable("VIVIDCODE_HOME");
    if (configured.isEmpty() && qEnvironmentVariable("LOCALAPPDATA").isEmpty())
        throw std::runtime_error("LOCALAPPDATA is unavailable");
    const QString home = configured.isEmpty()
        ? qEnvironmentVariable("LOCALAPPDATA") + "/vividcode" : configured;
    if (!QFileInfo(home).isAbsolute()) throw std::runtime_error("vividcode shared home must be absolute");
    return QDir::cleanPath(QDir::fromNativeSeparators(home));
}
inline QString sharedRuntime() { return sharedHome() + "/runtime"; }
inline QString projectCacheKey(const QString &root) {
    const QString canonical = QFileInfo(root).canonicalFilePath();
    if (canonical.isEmpty()) throw std::runtime_error("Project directory unavailable");
    return QString::fromLatin1(QCryptographicHash::hash(
        QDir::fromNativeSeparators(canonical).toLower().toUtf8(), QCryptographicHash::Sha256).toHex());
}
inline QString semanticDirectory(const QString &root) {
    return sharedHome() + "/semantic/" + projectCacheKey(root);
}
// Multiple viewers may read the runtime; the installer requests exclusive access.
class RuntimeReadLock {
#ifdef Q_OS_WIN
    HANDLE handle = INVALID_HANDLE_VALUE;
#endif
public:
    explicit RuntimeReadLock(const QString &home) {
#ifdef Q_OS_WIN
        const QString path = home + "/runtime.lock";
        if (QFileInfo(path).isSymLink() || QFileInfo(path).isJunction()) return;
        handle = CreateFileW(reinterpret_cast<LPCWSTR>(path.utf16()), GENERIC_READ,
            FILE_SHARE_READ, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
#endif
    }
    ~RuntimeReadLock() {
#ifdef Q_OS_WIN
        if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle);
#endif
    }
    RuntimeReadLock(const RuntimeReadLock &) = delete;
    RuntimeReadLock &operator=(const RuntimeReadLock &) = delete;
    bool valid() const {
#ifdef Q_OS_WIN
        return handle != INVALID_HANDLE_VALUE;
#else
        return true;
#endif
    }
};
}
