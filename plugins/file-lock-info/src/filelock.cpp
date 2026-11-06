#include "filelock.h"

#include <windows.h>
#include <restartmanager.h>

#include <algorithm>
#include <cstdio>

#include "wincmd.h"
#include "winexit.h"
#include "winpath.h"
#include "winproc.h"
#include "wintext.h"

namespace filelock {

namespace {


std::wstring stampOf(const unsigned long long fileTime)
{
    if (fileTime == 0) {
        return std::wstring();
    }
    FILETIME utc{};
    utc.dwLowDateTime  = static_cast<DWORD>(fileTime & 0xFFFFFFFFULL);
    utc.dwHighDateTime = static_cast<DWORD>(fileTime >> 32);
    FILETIME localFileTime{};
    if (!FileTimeToLocalFileTime(&utc, &localFileTime)) {
        return std::wstring();
    }
    SYSTEMTIME local{};
    if (!FileTimeToSystemTime(&localFileTime, &local)) {
        return std::wstring();
    }
    wchar_t text[64]{};
    swprintf_s(text, L"%04u-%02u-%02u %02u:%02u:%02u", local.wYear, local.wMonth,
               local.wDay, local.wHour, local.wMinute, local.wSecond);
    return text;
}

unsigned long long startTimeOfProcess(const DWORD pid, bool *known)
{
    if (known) {
        *known = false;
    }
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process) {
        return 0;
    }
    FILETIME creation{};
    FILETIME exit{};
    FILETIME kernel{};
    FILETIME user{};
    const bool queried
        = GetProcessTimes(process, &creation, &exit, &kernel, &user) != FALSE;
    CloseHandle(process);
    if (!queried) {
        return 0;
    }
    if (known) {
        *known = true;
    }
    return (static_cast<unsigned long long>(creation.dwHighDateTime) << 32)
           | creation.dwLowDateTime;
}

std::wstring executablePathOf(const DWORD pid, bool *accessible)
{
    if (accessible) {
        *accessible = false;
    }
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process) {
        return std::wstring();
    }
    std::vector<wchar_t> path(32768, L'\0');
    DWORD size = static_cast<DWORD>(path.size());
    const bool queried = QueryFullProcessImageNameW(process, 0, path.data(),
                                                    &size) != FALSE;
    CloseHandle(process);
    if (!queried || size == 0) {
        return std::wstring();
    }
    if (accessible) {
        *accessible = true;
    }
    return std::wstring(path.data(), size);
}

}  // namespace

bool processIsElevated()
{
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
        return false;
    }
    TOKEN_ELEVATION elevation{};
    DWORD returned = 0;
    const bool queried
        = GetTokenInformation(token, TokenElevation, &elevation,
                              sizeof(elevation), &returned)
          != FALSE;
    CloseHandle(token);
    return queried && elevation.TokenIsElevated != 0;
}

std::optional<Request> parseArguments(const std::vector<std::wstring> &arguments,
                                      std::wstring *error)
{
    const auto fail = [error](const std::wstring &message)
        -> std::optional<Request> {
        if (error) {
            *error = message;
        }
        return std::nullopt;
    };

    if (arguments.empty()) {
        return fail(L"the helper was started without a command line");
    }
    const auto scan = WinCmd::scanOptions(arguments, 1, {L"admin"}, {L"input"});
    if (!scan.ok) {
        return fail(scan.error);
    }
    std::wstring duplicate;
    if (!WinCmd::rejectDuplicates(scan, &duplicate)) {
        return fail(duplicate);
    }
    if (!scan.positionals.empty()) {
        return fail(L"unexpected argument: " + scan.positionals.front());
    }

    Request request;
    std::wstring rawInput;
    if (!WinCmd::optionValue(scan, L"input", &rawInput) || rawInput.empty()) {
        return fail(L"--input <file> is required");
    }
    if (!WinPath::lexicalPath(rawInput, &request.filePath)) {
        return fail(L"the selected file cannot be resolved: " + rawInput);
    }
    request.admin = WinCmd::hasOption(scan, L"admin");
    return request;
}

LockReport queryRestartManager(const std::wstring &filePath)
{
    LockReport report;
    report.filePath  = filePath;
    report.queriedAt = GetTickCount64();
    report.elevated  = processIsElevated();

    if (!WinPath::isRegularFile(filePath)) {
        report.exitCode = WinExit::kNotFound;
        report.message  = L"the selection is not an existing regular file: "
                         + filePath;
        return report;
    }

    DWORD session  = 0;
    wchar_t key[CCH_RM_SESSION_KEY + 1]{};
    DWORD status = RmStartSession(&session, 0, key);
    if (status != ERROR_SUCCESS) {
        report.exitCode = WinExit::kUnsupported;
        report.message
            = L"the Restart Manager is not available on this system (error "
              + std::to_wstring(status) + L").";
        return report;
    }
    struct SessionGuard {
        DWORD session;
        ~SessionGuard() { RmEndSession(session); }
    } guard{session};

    const wchar_t *files[]{filePath.c_str()};
    status = RmRegisterResources(session, 1, files, 0, nullptr, 0, nullptr);
    if (status != ERROR_SUCCESS) {
        report.exitCode = WinExit::kFailure;
        report.message
            = L"the file could not be registered with the Restart Manager "
              L"(error " + std::to_wstring(status) + L").";
        return report;
    }

    std::vector<RM_PROCESS_INFO> buffer(kInitialEntries);
    // RmGetList takes the array capacity in both pnProcInfoNeeded and pnProcInfo
    // on the way in, and reports the filled count and the required count on the
    // way out; passing zero for the capacity always answers "more data".
    UINT capacity       = static_cast<UINT>(buffer.size());
    UINT needed         = capacity;
    UINT count          = capacity;
    DWORD rebootReasons = 0;
    bool listed         = false;

    for (int attempt = 0; attempt < kMaximumAttempts; ++attempt) {
        needed = capacity;
        count  = capacity;
        status = RmGetList(session, &needed, &count, buffer.data(),
                           &rebootReasons);
        if (status == ERROR_SUCCESS) {
            listed = true;
            break;
        }
        if (status != ERROR_MORE_DATA) {
            report.exitCode = WinExit::kFailure;
            report.message
                = L"the Restart Manager could not return the list (error "
                  + std::to_wstring(status) + L").";
            return report;
        }
        if (needed == 0) {
            listed = true;
            count  = 0;
            break;
        }
        UINT target = needed;
        if (target > kMaximumEntries) {
            target           = kMaximumEntries;
            report.truncated = true;
        }
        if (target < kInitialEntries) {
            target = kInitialEntries;
        }
        if (target > capacity) {
            buffer.assign(target, RM_PROCESS_INFO{});
            capacity = target;
            continue;
        }
        // The buffer is already large enough, yet the Restart Manager still
        // reports more entries: the list is genuinely changing under us.
    }

    if (!listed) {
        if (report.truncated) {
            // The bound was reached. The entries that fit were written, so a
            // partial report is more useful than no report, and it says so.
            listed = true;
            count  = capacity;
        }
        else {
            report.exitCode = WinExit::kFailure;
            report.message
                = L"the list of users kept changing while it was being read and "
                  L"could not be captured in "
                  + std::to_wstring(kMaximumAttempts)
                  + L" attempts. Run the control again to retry.";
            return report;
        }
    }
    if (count > capacity) {
        // Never trust a reported count that exceeds what was allocated.
        count = capacity;
    }

    for (UINT index = 0; index < count; ++index) {
        const auto &info = buffer[index];
        LockUser user;
        user.pid = info.Process.dwProcessId;
        switch (info.ApplicationType) {
        case RmCritical:
            user.applicationType = L"Critical system process";
            break;
        case RmExplorer:
            user.applicationType = L"Windows Explorer";
            break;
        case RmConsole:
            user.applicationType = L"Console application";
            break;
        case RmService:
            user.applicationType = L"Service";
            break;
        default:
            user.applicationType = L"Application";
            break;
        }
        user.name = WinText::trim(info.strAppName);
        if (user.name.empty()) {
            user.name = L"(unnamed)";
        }

        const unsigned long long reported
            = (static_cast<unsigned long long>(
                   info.Process.ProcessStartTime.dwHighDateTime)
               << 32)
              | info.Process.ProcessStartTime.dwLowDateTime;
        bool observedKnown = false;
        const unsigned long long observed
            = startTimeOfProcess(user.pid, &observedKnown);
        user.startTimeKnown = observedKnown;
        user.startTime      = observedKnown ? observed : 0;
        // A pending identifier can be reused; when the start time no longer
        // matches the one the Restart Manager saw, the details would describe a
        // different process, so they are withheld instead.
        if (reported != 0 && observedKnown && reported != observed) {
            user.identityChanged = true;
        }
        else {
            user.executablePath = executablePathOf(user.pid,
                                                   &user.executableAccessible);
        }
        report.users.push_back(user);
    }

    report.ok       = true;
    report.exitCode = WinExit::kOk;
    return report;
}

LockQuery systemLockQuery()
{
    return [](const std::wstring &filePath) {
        return queryRestartManager(filePath);
    };
}

std::wstring formatReportText(const LockReport &report)
{
    std::wstring text = L"File: " + report.filePath + L"\r\n\r\n";

    if (!report.ok) {
        text += report.message;
        return text;
    }

    bool unreadablePath = false;

    if (report.users.empty()) {
        // Only shown when there is nothing to list. A user who cannot delete a
        // file needs a reason to trust an empty result, and this is the one
        // place where the caveat is actionable instead of noise.
        text += L"No application reported.\r\n";
        text += L"A driver, a protected process or another user session may "
                L"still be using this file.\r\n";
    }
    else {
        text += std::to_wstring(static_cast<long long>(report.users.size()))
                + L" application(s) using this file:\r\n\r\n";
        const size_t displayCount
            = (std::min)(report.users.size(), kMaxDisplayUsers);
        for (size_t index = 0; index < displayCount; ++index) {
            const auto &user = report.users[index];
            text += std::to_wstring(static_cast<long long>(index + 1)) + L". "
                    + user.applicationType + L"\r\n";
            text += L"   Name: " + user.name + L"\r\n";
            text += L"   PID:  " + std::to_wstring(user.pid) + L"\r\n";
            if (user.identityChanged) {
                text += L"   Executable: not shown - the process identifier "
                        L"changed while the list was read. Run the control "
                        L"again.\r\n";
            }
            else if (user.executableAccessible && !user.executablePath.empty()) {
                text += L"   Executable: " + user.executablePath + L"\r\n";
            }
            else {
                unreadablePath = true;
                text += L"   Executable: not accessible\r\n";
            }
            if (user.startTimeKnown) {
                text += L"   Started: " + stampOf(user.startTime) + L"\r\n";
            }
            text += L"\r\n";
        }

        if (report.users.size() > displayCount) {
            const size_t remaining = report.users.size() - displayCount;
            text += L"... and "
                    + std::to_wstring(static_cast<long long>(remaining))
                    + L" more application(s) using this file ("
                    + std::to_wstring(static_cast<long long>(displayCount))
                    + L" of "
                    + std::to_wstring(static_cast<long long>(report.users.size()))
                    + L" shown).\r\n\r\n";
        }
    }

    if (report.truncated) {
        text += L"Only the first "
                + std::to_wstring(static_cast<long long>(kMaximumEntries))
                + L" applications are listed.\r\n\r\n";
    }

    if (unreadablePath) {
        text += report.elevated
                    ? L"Some details are not readable even with administrator "
                      L"rights.\r\n"
                    : L"Some details need administrator rights.\r\n";
    }
    return text;
}

int run(const std::vector<std::wstring> &arguments, const LockQuery &query)
{
    std::wstring error;
    const auto request = parseArguments(arguments, &error);
    if (!request) {
        // The host does not read a Control helper's stderr, so the failure has to
        // reach the user from this process; writeStandardError raises the dialog.
        WinText::writeStandardError(error);
        return WinExit::kUsage;
    }
    if (!WinPath::isRegularFile(request->filePath)) {
        WinText::writeStandardError(L"the selection is not an existing regular "
                                 L"file: "
                                 + request->filePath);
        return WinExit::kNotFound;
    }

    if (request->admin && !processIsElevated()) {
        // The user asked for an elevated query through the configured argument,
        // so consent is requested explicitly and nothing is elevated silently.
        const auto outcome = WinProc::runElevated(
            WinProc::modulePath(), {L"--input", request->filePath, L"--admin"},
            std::wstring(), 90000);
        if (!outcome.completed) {
            WinText::writeStandardError(outcome.error);
            return WinExit::kTimeout;
        }
        if (outcome.cancelled) {
            WinText::writeStandardError(
                L"No elevated query was performed, so no report was shown and "
                L"nothing was queried.");
            return WinExit::kCancelled;
        }
        if (!outcome.started) {
            WinText::writeStandardError(outcome.error);
            return WinExit::kFailure;
        }
        // ShellExecuteExW returns only after the elevated process exists; that
        // is the strongest signal available across the elevation boundary. The
        // elevated process runs this same code and shows its own report
        // dialog.
        return WinExit::kOk;
    }

    const auto report = query(request->filePath);
    if (!report.ok) {
        // A failed query is not a report: presenting it in the result dialog
        // would read like a result, so it goes to the helper's error dialog
        // instead and the result dialog never opens.
        WinText::writeStandardError(report.message);
        return report.exitCode == WinExit::kOk ? WinExit::kFailure
                                               : report.exitCode;
    }

    // The report is presented in a system-owned modal dialog: one MessageBoxW,
    // no window drawn by the plugin itself. The helper exits once the user
    // dismisses it, so nothing lingers in the background, and nothing is copied
    // to the clipboard.
    MessageBoxW(nullptr, formatReportText(report).c_str(),
                L"Who Is Locking This File",
                MB_OK | MB_ICONINFORMATION | MB_SETFOREGROUND);
    return WinExit::kOk;
}


int run(const std::vector<std::wstring> &arguments)
{
    return run(arguments, systemLockQuery());
}

}  // namespace filelock
