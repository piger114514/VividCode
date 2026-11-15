#pragma once
#include <QtCore>
#include <atomic>
#include <mutex>
#include <thread>
#include <stdexcept>
#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace VividCode {
struct GenerationCancelled : std::runtime_error { GenerationCancelled() : std::runtime_error("Cancelled") {} };
class GenerationControl {
    std::thread input;
    std::atomic<bool> closing{false}, cancelled{false};
    std::mutex mutex;
    bool accepting = true;
public:
    explicit GenerationControl(bool consoleInput) {
#ifdef Q_OS_WIN
        if (consoleInput) {
            input = std::thread([this] {
                const HANDLE stream = GetStdHandle(STD_INPUT_HANDLE);
                QByteArray line; char buffer[256]; DWORD count = 0;
                while (!closing && ReadFile(stream, buffer, sizeof(buffer), &count, nullptr) && count) {
                    for (DWORD i = 0; i < count; ++i) {
                        if (buffer[i] == '\n') {
                            if (line.trimmed().toLower() == "stop") {
                                const std::lock_guard<std::mutex> lock(mutex);
                                if (accepting) cancelled = true;
                                return;
                            }
                            line.clear();
                        } else if (line.size() < 256) line.append(buffer[i]);
                    }
                }
            });
        }
#else
        Q_UNUSED(consoleInput);
#endif
    }
    void checkpoint() const { if (cancelled) throw GenerationCancelled(); }
    // Stops accepted before publication discard staging. The input thread and
    // the publisher synchronize this boundary within the current process only.
    void seal() { const std::lock_guard<std::mutex> lock(mutex); checkpoint(); accepting = false; }
    ~GenerationControl() {
        closing = true;
#ifdef Q_OS_WIN
        if (input.joinable()) {
            while (WaitForSingleObject(input.native_handle(), 20) != WAIT_OBJECT_0) CancelSynchronousIo(input.native_handle());
            input.join();
        }
#endif
    }
};
}
