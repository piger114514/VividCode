// Small project launcher: Win32 + static CRT only, no Qt/Python DLLs beside it.
#define NOMINMAX
#include <windows.h>
#include <shlobj.h>
#include <shellapi.h>
#include <filesystem>
#include <string>
#include <vector>

namespace {
struct Handle {
    HANDLE value = INVALID_HANDLE_VALUE;
    ~Handle() { if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value); }
};
std::wstring environment(const wchar_t *name) {
    DWORD n = GetEnvironmentVariableW(name, nullptr, 0);
    if (!n) return {};
    std::wstring result(n, L'\0');
    DWORD written = GetEnvironmentVariableW(name, result.data(), n);
    if (!written || written >= n) return {};
    result.resize(written); return result;
}
std::wstring quote(const std::wstring &value) {
    std::wstring result = L"\"";
    size_t slashes = 0;
    for (wchar_t c : value) {
        if (c == L'\\') { ++slashes; continue; }
        result.append(c == L'"' ? slashes * 2 + 1 : slashes, L'\\');
        result += c; slashes = 0;
    }
    result.append(slashes * 2, L'\\'); result += L'"'; return result;
}
bool plainPath(std::filesystem::path path) {
    while (!path.empty()) {
        DWORD attributes = GetFileAttributesW(path.c_str());
        if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_REPARSE_POINT)) return false;
        auto parent = path.parent_path(); if (parent == path) break; path = parent;
    }
    return true;
}
}
int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    int argc = 0;
    LPWSTR *raw = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!raw) return 1;
    std::vector<std::wstring> args(raw, raw + argc); LocalFree(raw);
    bool verify = false;
    for (const auto &arg : args) if (arg == L"--verify-startup") verify = true;
    const auto fail = [verify](const wchar_t *message) {
        if (!verify) MessageBoxW(nullptr, message, L"vividcode", MB_OK | MB_ICONERROR);
        OutputDebugStringW(message); return 1;
    };
    std::wstring executable(32768, L'\0');
    DWORD count = GetModuleFileNameW(nullptr, executable.data(), static_cast<DWORD>(executable.size()));
    if (!count || count >= executable.size()) return fail(L"无法确定项目启动器路径。");
    executable.resize(count);
    std::filesystem::path store = std::filesystem::path(executable).parent_path();
    if (_wcsicmp(store.filename().c_str(), L".vividcode") || !plainPath(store))
        return fail(L"请使用项目 .vividcode 文件夹中的启动器。");
    const auto project = store.parent_path();
    std::wstring home = environment(L"VIVIDCODE_HOME");
    if (home.empty()) {
        PWSTR local = nullptr;
        if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &local)))
            return fail(L"无法确定当前用户的 vividcode 安装位置。");
        home = std::wstring(local) + L"\\vividcode"; CoTaskMemFree(local);
    }
    std::filesystem::path base(home), runtime = base / L"runtime", viewer = runtime / L"vividcode-viewer.exe";
    if (!base.is_absolute() || !plainPath(base) || !plainPath(viewer) ||
        GetFileAttributesW(viewer.c_str()) == INVALID_FILE_ATTRIBUTES)
        return fail(L"本机尚未安装 vividcode 共享程序。请先安装，再打开项目启动器。");
    Handle runtimeLock;
    runtimeLock.value = CreateFileW((base / L"runtime.lock").c_str(), GENERIC_READ, FILE_SHARE_READ,
        nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (runtimeLock.value == INVALID_HANDLE_VALUE) return fail(L"vividcode 共享程序正在更新，请稍后重试。");
    std::wstring command = quote(viewer.native()) + L" --project " + quote(project.native());
    for (size_t i = 1; i < args.size(); ++i) {
        if (args[i] == L"--verify-startup") command += L" --verify-startup";
        else if (args[i] == L"-platform" && i + 1 < args.size()) {
            command += L" -platform " + quote(args[++i]);
        } else return fail(L"启动器参数无效。");
    }
    Handle job;
    job.value = CreateJobObjectW(nullptr, nullptr);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!job.value || !SetInformationJobObject(job.value, JobObjectExtendedLimitInformation, &limits, sizeof(limits)))
        return fail(L"无法建立 vividcode 进程生命周期。");
    STARTUPINFOW startup{}; startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(viewer.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_SUSPENDED,
        nullptr, runtime.c_str(), &startup, &process)) return fail(L"无法启动共享查看器，请重新安装 vividcode。");
    Handle child{process.hProcess}, thread{process.hThread};
    if (!AssignProcessToJobObject(job.value, child.value)) {
        TerminateProcess(child.value, 1); return fail(L"无法管理共享查看器进程。");
    }
    if (ResumeThread(thread.value) == static_cast<DWORD>(-1)) {
        TerminateProcess(child.value, 1); return fail(L"无法运行共享查看器。");
    }
    WaitForSingleObject(child.value, INFINITE);
    DWORD code = 1; GetExitCodeProcess(child.value, &code);
    return static_cast<int>(code);
}
