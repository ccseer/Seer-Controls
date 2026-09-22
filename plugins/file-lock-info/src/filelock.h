#pragma once

#include <windows.h>

#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace filelock {

// Bounded growth for the Restart Manager list: a hostile or very busy machine
// cannot make the helper allocate without limit.
inline constexpr UINT kInitialEntries = 16;
inline constexpr UINT kMaximumEntries = 4096;
inline constexpr int kMaximumAttempts = 6;

struct LockUser {
    std::wstring applicationType;
    std::wstring name;
    DWORD pid = 0;
    bool startTimeKnown = false;
    unsigned long long startTime = 0;
    std::wstring executablePath;
    bool executableAccessible = false;
    bool identityChanged = false;
};

struct LockReport {
    bool ok = false;
    int exitCode = 1;
    std::wstring message;
    std::wstring filePath;
    std::vector<LockUser> users;
    bool truncated = false;
    unsigned long long queriedAt = 0;
    bool elevated = false;
};

// Injected so the query can be tested without the Restart Manager.
using LockQuery = std::function<LockReport(const std::wstring &filePath)>;

struct Request {
    std::wstring filePath;
    bool admin = false;
};

std::optional<Request> parseArguments(const std::vector<std::wstring> &arguments,
                                      std::wstring *error = nullptr);

bool processIsElevated();

// Restart Manager backend: start a session, register the exact file, retrieve
// the affected applications and services, and end the session on every path.
LockReport queryRestartManager(const std::wstring &filePath);

LockQuery systemLockQuery();

std::wstring formatReportText(const LockReport &report);

// Runs the query and shows the report in a system-owned modal dialog. A query
// that failed is not a report: it opens no dialog and its reason goes to the
// host's failure channel instead. The helper exits once the user dismisses the
// dialog, so the presentation stays inside the manifest timeout.
int run(const std::vector<std::wstring> &arguments, const LockQuery &query);
int run(const std::vector<std::wstring> &arguments);

}  // namespace filelock
