#pragma once

#include <windows.h>
#include <objbase.h>
#include <shellapi.h>
#include <shlobj.h>

#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "wincmd.h"
#include "winpath.h"
#include "wintext.h"

// Native process launching shared by the Control helpers.
//
// Two lifetimes are deliberately kept apart:
//   * Data-processing children (archivers, image optimizers) are created
//     suspended, assigned to a kill-on-close Job Object and only then resumed,
//     so host cancellation cannot leave uncontained work behind.
//   * UI children (terminals, report windows) must outlive the launcher and are
//     never placed in that job.
namespace WinProc {

inline bool hasExecutableSuffix(const std::wstring &path)
{
    const auto dot = path.find_last_of(L'.');
    if (dot == std::wstring::npos) {
        return false;
    }
    const auto suffix = WinText::toLower(path.substr(dot + 1));
    return suffix == L"exe" || suffix == L"com" || suffix == L"bat"
           || suffix == L"cmd";
}

inline bool isExecutableFile(const std::wstring &path)
{
    return WinPath::isRegularFile(path) && hasExecutableSuffix(path);
}

inline std::wstring modulePath()
{
    std::vector<wchar_t> buffer(32768, L'\0');
    const DWORD written = GetModuleFileNameW(
        nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (written == 0 || written >= buffer.size()) {
        return std::wstring();
    }
    return std::wstring(buffer.data(), written);
}

inline std::wstring readRegistryValue(HKEY root,
                                      const std::wstring &subKey,
                                      const std::wstring &valueName)
{
    HKEY key = nullptr;
    if (RegOpenKeyExW(root, subKey.c_str(), 0, KEY_QUERY_VALUE, &key)
        != ERROR_SUCCESS) {
        return std::wstring();
    }
    DWORD type = 0;
    DWORD size = 0;
    if (RegQueryValueExW(key, valueName.c_str(), nullptr, &type, nullptr, &size)
            != ERROR_SUCCESS
        || (type != REG_SZ && type != REG_EXPAND_SZ) || size < sizeof(wchar_t)) {
        RegCloseKey(key);
        return std::wstring();
    }
    std::vector<wchar_t> buffer(size / sizeof(wchar_t) + 2, L'\0');
    const auto status
        = RegQueryValueExW(key, valueName.c_str(), nullptr, &type,
                           reinterpret_cast<LPBYTE>(buffer.data()), &size);
    RegCloseKey(key);
    if (status != ERROR_SUCCESS) {
        return std::wstring();
    }
    std::wstring value = WinText::trim(buffer.data());
    if (type == REG_EXPAND_SZ && !value.empty()) {
        std::vector<wchar_t> expanded(32768, L'\0');
        const DWORD written = ExpandEnvironmentStringsW(
            value.c_str(), expanded.data(),
            static_cast<DWORD>(expanded.size()));
        if (written > 0 && written <= expanded.size()) {
            value = std::wstring(expanded.data());
        }
    }
    return value;
}

inline std::wstring expandEnvironment(const std::wstring &value)
{
    if (value.empty() || value.find(L'%') == std::wstring::npos) {
        return value;
    }
    std::vector<wchar_t> expanded(32768, L'\0');
    const DWORD written = ExpandEnvironmentStringsW(
        value.c_str(), expanded.data(),
        static_cast<DWORD>(expanded.size()));
    if (written == 0 || written > expanded.size()) {
        return value;
    }
    return std::wstring(expanded.data());
}

// The documented per-executable override used by installers and by the AppX
// execution aliases in %LOCALAPPDATA%\Microsoft\WindowsApps.
inline std::wstring lookupAppPaths(const std::wstring &executableName)
{
    const std::wstring key
        = L"Software\\Microsoft\\Windows\\CurrentVersion\\App Paths\\"
          + executableName;
    for (const auto root : {HKEY_CURRENT_USER, HKEY_LOCAL_MACHINE}) {
        const auto candidate = readRegistryValue(root, key, L"");
        if (!candidate.empty()) {
            return candidate;
        }
    }
    return std::wstring();
}

inline std::vector<std::wstring> pathDirectories()
{
    std::vector<std::wstring> directories;
    std::vector<wchar_t> buffer(32768, L'\0');
    const DWORD written = GetEnvironmentVariableW(
        L"PATH", buffer.data(), static_cast<DWORD>(buffer.size()));
    if (written == 0 || written >= buffer.size()) {
        return directories;
    }
    const std::wstring raw(buffer.data(), written);
    size_t begin = 0;
    for (;;) {
        const auto position = raw.find(L';', begin);
        const auto end = position == std::wstring::npos ? raw.size() : position;
        std::wstring entry = WinText::trim(raw.substr(begin, end - begin));
        if (entry.size() >= 2 && entry.front() == L'"' && entry.back() == L'"') {
            entry = entry.substr(1, entry.size() - 2);
        }
        entry = expandEnvironment(entry);
        if (!entry.empty()) {
            directories.push_back(entry);
        }
        if (position == std::wstring::npos) {
            break;
        }
        begin = position + 1;
    }
    return directories;
}

inline std::wstring locateOnPath(const std::wstring &executableName)
{
    const auto directories = pathDirectories();
    for (const auto &directory : directories) {
        const auto candidate = WinPath::join(directory, executableName);
        if (isExecutableFile(candidate)) {
            return candidate;
        }
    }
    return std::wstring();
}

// Resolves an absolute path, or a bare name through App Paths and PATH. The
// selected directory is never searched, so a downloaded executable inside the
// previewed folder cannot hijack a launch.
inline std::wstring resolveExecutable(const std::wstring &nameOrPath)
{
    if (nameOrPath.find(L'\\') != std::wstring::npos
        || nameOrPath.find(L'/') != std::wstring::npos) {
        std::wstring absolute;
        if (!WinPath::lexicalPath(nameOrPath, &absolute)) {
            return std::wstring();
        }
        return isExecutableFile(absolute) ? absolute : std::wstring();
    }
    const auto appPath = lookupAppPaths(nameOrPath);
    if (!appPath.empty() && isExecutableFile(appPath)) {
        return appPath;
    }
    return locateOnPath(nameOrPath);
}

// kill-on-job-close, non-inheritable, owned by the helper that created it.
class Job {
public:
    Job() = default;
    Job(const Job &) = delete;
    Job &operator=(const Job &) = delete;

    ~Job() { close(); }

    bool create(std::wstring *error)
    {
        close();
        m_handle = CreateJobObjectW(nullptr, nullptr);
        if (!m_handle) {
            if (error) {
                *error = L"cannot create the containment job: "
                         + std::to_wstring(GetLastError());
            }
            return false;
        }
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags
            = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (!SetInformationJobObject(m_handle, JobObjectExtendedLimitInformation,
                                     &limits, sizeof(limits))) {
            if (error) {
                *error = L"cannot configure the containment job: "
                         + std::to_wstring(GetLastError());
            }
            close();
            return false;
        }
        return true;
    }

    bool assign(HANDLE process, std::wstring *error) const
    {
        if (!m_handle) {
            if (error) {
                *error = L"containment job is not created";
            }
            return false;
        }
        if (!AssignProcessToJobObject(m_handle, process)) {
            if (error) {
                *error = L"cannot assign the child to the containment job: "
                         + std::to_wstring(GetLastError());
            }
            return false;
        }
        return true;
    }

    HANDLE handle() const { return m_handle; }

    void close()
    {
        if (m_handle) {
            CloseHandle(m_handle);
            m_handle = nullptr;
        }
    }

private:
    HANDLE m_handle = nullptr;
};

struct Child {
    HANDLE process = nullptr;
    HANDLE thread = nullptr;
    DWORD pid = 0;
    // Set when CREATE_BREAKAWAY_FROM_JOB was refused and the launch was retried
    // without it, which means the child may inherit the host's job.
    bool breakawayDenied = false;

    bool running() const { return process != nullptr; }

    void closeThread()
    {
        if (thread) {
            CloseHandle(thread);
            thread = nullptr;
        }
    }

    void close()
    {
        closeThread();
        if (process) {
            CloseHandle(process);
            process = nullptr;
            pid    = 0;
        }
    }
};

struct LaunchOptions {
    std::wstring workingDirectory;
    bool newConsole = false;
    bool detached = false;
    bool tryBreakawayFromJob = false;
    bool suspended = false;
    bool inheritHandles = false;
    bool noWindow = false;
};

inline bool createChild(const std::wstring &executablePath,
                        const std::vector<std::wstring> &arguments,
                        const LaunchOptions &options,
                        const STARTUPINFOW *startupOverride,
                        Child *child,
                        std::wstring *error)
{
    std::wstring commandLine = WinCmd::quoteArgument(executablePath);
    for (const auto &argument : arguments) {
        commandLine += L' ';
        commandLine += WinCmd::quoteArgument(argument);
    }
    std::vector<wchar_t> mutableCommand(commandLine.begin(), commandLine.end());
    mutableCommand.push_back(L'\0');

    DWORD flags = CREATE_UNICODE_ENVIRONMENT;
    if (options.newConsole) {
        flags |= CREATE_NEW_CONSOLE;
    }
    if (options.detached) {
        flags |= DETACHED_PROCESS | CREATE_NEW_PROCESS_GROUP;
    }
    if (options.suspended) {
        flags |= CREATE_SUSPENDED;
    }
    if (options.noWindow) {
        flags |= CREATE_NO_WINDOW;
    }
    if (options.tryBreakawayFromJob) {
        flags |= CREATE_BREAKAWAY_FROM_JOB;
    }

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    if (startupOverride) {
        startup = *startupOverride;
        startup.cb = sizeof(startup);
    }

    const std::wstring application = WinPath::addExtendedPrefix(
        WinPath::nativeSeparators(executablePath));
    const DWORD requestedFlags = flags;

    auto attempt = [&](const DWORD attemptFlags) {
        PROCESS_INFORMATION process{};
        const BOOL started = CreateProcessW(
            application.c_str(), mutableCommand.data(), nullptr, nullptr,
            options.inheritHandles ? TRUE : FALSE, attemptFlags, nullptr,
            options.workingDirectory.empty() ? nullptr
                                             : options.workingDirectory.c_str(),
            &startup, &process);
        if (started) {
            child->process = process.hProcess;
            child->thread  = process.hThread;
            child->pid     = process.dwProcessId;
        }
        return started != FALSE;
    };

    if (attempt(requestedFlags)) {
        return true;
    }
    const DWORD failure = GetLastError();
    if ((requestedFlags & CREATE_BREAKAWAY_FROM_JOB) != 0
        && failure == ERROR_ACCESS_DENIED) {
        // The host may own a job that forbids breakaway; a UI child is still
        // better off launched than not launched. The downgrade is recorded so the
        // caller can say so instead of silently launching a child that will die
        // with the job that owns it.
        if (attempt(requestedFlags & ~CREATE_BREAKAWAY_FROM_JOB)) {
            child->breakawayDenied = true;
            return true;
        }
    }
    if (error) {
        *error = L"cannot start '" + executablePath
                 + L"': " + std::to_wstring(GetLastError());
    }
    return false;
}

inline bool resumeChild(Child *child, std::wstring *error)
{
    if (!child->thread) {
        return true;
    }
    if (ResumeThread(child->thread) == static_cast<DWORD>(-1)) {
        if (error) {
            *error = L"cannot resume the child process: "
                     + std::to_wstring(GetLastError());
        }
        return false;
    }
    child->closeThread();
    return true;
}

// Data-processing child: suspended -> job -> resume. If the assignment fails the
// still-suspended child is terminated, so no uncontained work is ever started.
inline bool launchContained(const std::wstring &executablePath,
                            const std::vector<std::wstring> &arguments,
                            const std::wstring &workingDirectory,
                            const Job &job,
                            Child *child,
                            std::wstring *error)
{
    LaunchOptions options;
    options.workingDirectory = workingDirectory;
    options.suspended        = true;
    options.noWindow         = true;

    if (!createChild(executablePath, arguments, options, nullptr, child,
                     error)) {
        return false;
    }
    if (!job.assign(child->process, error)) {
        TerminateProcess(child->process, 1);
        child->close();
        return false;
    }
    if (!resumeChild(child, error)) {
        TerminateProcess(child->process, 1);
        child->close();
        return false;
    }
    return true;
}

inline bool waitForExit(const Child &child,
                        const DWORD timeoutMs,
                        DWORD *exitCode)
{
    if (!child.running()) {
        return false;
    }
    if (WaitForSingleObject(child.process, timeoutMs) != WAIT_OBJECT_0) {
        return false;
    }
    if (exitCode) {
        GetExitCodeProcess(child.process, exitCode);
    }
    return true;
}

inline bool terminateChild(Child *child)
{
    if (!child->running()) {
        return true;
    }
    TerminateProcess(child->process, 1);
    WaitForSingleObject(child->process, 5000);
    child->close();
    return true;
}

struct CaptureResult {
    int exitCode = -1;
    std::string standardOutput;
    std::string standardError;
    bool timedOut = false;
};

// Non-blocking drain: PeekNamedPipe reports what is available, so a reader that
// alternates between two pipes can never deadlock on a full pipe buffer.
inline void drainPipe(HANDLE pipe, std::string *sink, const size_t limit)
{
    char buffer[4096];
    for (;;) {
        DWORD available = 0;
        if (!PeekNamedPipe(pipe, nullptr, 0, nullptr, &available, nullptr)
            || available == 0) {
            return;
        }
        DWORD read = 0;
        if (!ReadFile(pipe, buffer, sizeof(buffer), &read, nullptr)
            || read == 0) {
            return;
        }
        if (sink->size() < limit) {
            sink->append(buffer, read);
        }
    }
}

// Runs a contained child to completion while draining both pipes on the calling
// thread. PeekNamedPipe keeps the drain non-blocking, so a child that fills one
// pipe buffer cannot deadlock a reader that is waiting on the other.
inline bool runContained(const std::wstring &executablePath,
                         const std::vector<std::wstring> &arguments,
                         const std::wstring &workingDirectory,
                         const Job *job,
                         const DWORD timeoutMs,
                         const size_t captureLimit,
                         CaptureResult *capture,
                         std::wstring *error)
{
    SECURITY_ATTRIBUTES security{};
    security.nLength        = sizeof(security);
    security.bInheritHandle = TRUE;

    HANDLE stdoutRead  = nullptr;
    HANDLE stdoutWrite = nullptr;
    HANDLE stderrRead  = nullptr;
    HANDLE stderrWrite = nullptr;
    if (!CreatePipe(&stdoutRead, &stdoutWrite, &security, 0)
        || !CreatePipe(&stderrRead, &stderrWrite, &security, 0)) {
        if (error) {
            *error = L"cannot create capture pipes: "
                     + std::to_wstring(GetLastError());
        }
        if (stdoutRead) {
            CloseHandle(stdoutRead);
        }
        if (stdoutWrite) {
            CloseHandle(stdoutWrite);
        }
        if (stderrRead) {
            CloseHandle(stderrRead);
        }
        return false;
    }
    SetHandleInformation(stdoutRead, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(stderrRead, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW startup{};
    startup.cb         = sizeof(startup);
    startup.dwFlags    = STARTF_USESTDHANDLES;
    startup.hStdInput  = nullptr;
    startup.hStdOutput = stdoutWrite;
    startup.hStdError  = stderrWrite;

    struct PipeCloser {
        HANDLE handle = nullptr;
        ~PipeCloser()
        {
            if (handle) {
                CloseHandle(handle);
            }
        }
    } stdoutWriteGuard{stdoutWrite}, stderrWriteGuard{stderrWrite},
        stdoutReadGuard{stdoutRead}, stderrReadGuard{stderrRead};

    LaunchOptions options;
    options.workingDirectory = workingDirectory;
    options.suspended        = true;
    options.noWindow         = true;
    options.inheritHandles   = true;

    Child child;
    if (!createChild(executablePath, arguments, options, &startup, &child,
                     error)) {
        return false;
    }
    CloseHandle(stdoutWrite);
    stdoutWriteGuard.handle = nullptr;
    CloseHandle(stderrWrite);
    stderrWriteGuard.handle = nullptr;

    if (job && !job->assign(child.process, error)) {
        TerminateProcess(child.process, 1);
        child.close();
        return false;
    }
    if (!resumeChild(&child, error)) {
        TerminateProcess(child.process, 1);
        child.close();
        return false;
    }

    const ULONGLONG deadline = GetTickCount64() + timeoutMs;
    capture->exitCode        = -1;
    bool exited              = false;
    for (;;) {
        drainPipe(stdoutRead, &capture->standardOutput, captureLimit);
        drainPipe(stderrRead, &capture->standardError, captureLimit);
        if (WaitForSingleObject(child.process, 25) == WAIT_OBJECT_0) {
            exited = true;
            break;
        }
        if (GetTickCount64() >= deadline) {
            capture->timedOut = true;
            break;
        }
    }

    if (!exited) {
        TerminateProcess(child.process, 1);
        WaitForSingleObject(child.process, 5000);
        if (error) {
            *error = L"'" + WinPath::fileName(executablePath)
                     + L"' did not finish within "
                     + std::to_wstring(timeoutMs / 1000) + L" s";
        }
    }
    else {
        DWORD code = 1;
        GetExitCodeProcess(child.process, &code);
        capture->exitCode = static_cast<int>(code);
    }

    // A grandchild can inherit the write end and keep the pipe open after the
    // launched process exits, so the final drain is bounded instead of blocking
    // until every writer closes.
    const ULONGLONG drainDeadline = GetTickCount64() + 400;
    for (;;) {
        drainPipe(stdoutRead, &capture->standardOutput, captureLimit);
        drainPipe(stderrRead, &capture->standardError, captureLimit);
        if (GetTickCount64() >= drainDeadline) {
            break;
        }
        Sleep(20);
    }
    child.close();
    return exited;
}

struct ElevationOutcome {
    bool completed = false;
    bool started = false;
    bool cancelled = false;
    DWORD pid = 0;
    std::wstring error;
};

// Documented runas/UAC elevation. ShellExecuteExW blocks until the user answers
// the consent prompt, so it runs on a worker thread and the caller waits for a
// bounded interval instead of the whole manifest timeout.
inline ElevationOutcome runElevated(const std::wstring &executablePath,
                                    const std::vector<std::wstring> &arguments,
                                    const std::wstring &workingDirectory,
                                    const DWORD timeoutMs)
{
    auto outcome = std::make_shared<ElevationOutcome>();
    // Shared ownership, because the worker signals this event from whatever
    // thread it is still running on. On the timeout path the thread is detached
    // and the handle has to stay valid until that thread's SetEvent returns;
    // closing it here would leave the worker signalling a handle the process
    // may already have recycled.
    const auto done = std::shared_ptr<void>(
        CreateEventW(nullptr, TRUE, FALSE, nullptr), [](void *handle) {
            CloseHandle(handle);
        });
    if (!done) {
        outcome->error = L"cannot create the elevation handshake";
        return *outcome;
    }

    std::thread worker([outcome, done, executablePath, arguments,
                        workingDirectory] {
        const HRESULT hr = CoInitializeEx(
            nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
        const std::wstring parameters = WinCmd::joinArguments(arguments);
        SHELLEXECUTEINFOW info{};
        info.cbSize = sizeof(info);
        info.fMask  = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC
                     | SEE_MASK_FLAG_NO_UI;
        info.lpVerb       = L"runas";
        info.lpFile       = executablePath.c_str();
        info.lpParameters = parameters.empty() ? nullptr : parameters.c_str();
        info.lpDirectory  = workingDirectory.empty()
                                ? nullptr
                                : workingDirectory.c_str();
        info.nShow        = SW_SHOWNORMAL;
        SetLastError(ERROR_SUCCESS);
        if (!ShellExecuteExW(&info)) {
            const DWORD failure = GetLastError();
            outcome->cancelled = failure == ERROR_CANCELLED;
            outcome->error = outcome->cancelled
                                 ? L"elevation was cancelled"
                                 : L"cannot start elevated process: "
                                       + std::to_wstring(failure);
        }
        else {
            outcome->started = true;
            if (info.hProcess) {
                outcome->pid = GetProcessId(info.hProcess);
                CloseHandle(info.hProcess);
            }
        }
        outcome->completed = true;
        SetEvent(done.get());
        if (SUCCEEDED(hr)) {
            CoUninitialize();
        }
    });

    const DWORD wait = WaitForSingleObject(done.get(), timeoutMs);
    if (wait == WAIT_OBJECT_0) {
        worker.join();
        return *outcome;
    }

    // The consent prompt is still up. Abandon the thread; the process is about
    // to exit, which closes the prompt and the ShellExecuteExW call with it.
    // The event handle goes with the thread and closes when it finishes.
    worker.detach();
    ElevationOutcome timedOut;
    timedOut.error = L"elevation request did not complete within "
                     + std::to_wstring(timeoutMs / 1000) + L" s";
    return timedOut;
}


// True when the process owns at least one visible unowned top-level window.
//
// Both UI handoffs use it as their fallback readiness signal: the readiness
// event is the primary one, but a cold start can outlast any fixed budget, and
// killing a child whose window is already on screen would turn a working
// handoff into a failure. It lives here because the launcher side of that check
// is a query about a process id, and because both handoff implementations
// already include this header.
namespace Detail {

struct WindowSearch {
    DWORD pid   = 0;
    bool found  = false;
};

inline BOOL CALLBACK findOwnedWindow(HWND window, LPARAM data)
{
    auto *search = reinterpret_cast<WindowSearch *>(data);
    DWORD owner  = 0;
    GetWindowThreadProcessId(window, &owner);
    if (owner == search->pid && IsWindowVisible(window)
        && GetWindow(window, GW_OWNER) == nullptr) {
        search->found = true;
        return FALSE;
    }
    return TRUE;
}

}  // namespace Detail

inline bool ownsVisibleWindow(const DWORD pid)
{
    Detail::WindowSearch search{pid, false};
    EnumWindows(Detail::findOwnedWindow, reinterpret_cast<LPARAM>(&search));
    return search.found;
}

}  // namespace WinProc
