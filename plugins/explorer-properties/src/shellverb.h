#pragma once

#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "shelluiworker.h"
#include <shellapi.h>

namespace shellverb {

bool hasVisibleWindowForProcess(DWORD processId);

struct ParsedInput {
    std::wstring inputPath;
};

inline constexpr int kArgumentErrorExitCode = 2;
inline constexpr int kShellFailureExitCode = 1;

// A reason raised by the showing half after validation accepted the request; the
// launcher presents it in its own error dialog. The alias is owned by the
// handoff so every user of it agrees on the signature.
using ErrorReport = ShellUiWorker::ErrorReport;
using ShellExecutor = std::function<BOOL(SHELLEXECUTEINFOW *)>;

std::optional<ParsedInput> parseArguments(const std::vector<std::wstring>& argv);
// A refusal comes back as text for the caller to present rather than being
// reported here, because the caller knows which channel carries it: the launcher
// owns the error dialog, the worker can only reach it through the launcher's
// reason channel. Worded for the user, because it is shown verbatim.
std::optional<ParsedInput> parseArguments(const std::vector<std::wstring>& argv,
                                          std::wstring* error);
int invokeProperties(const std::wstring& inputPath,
                     const std::function<void()> &onReady = {},
                     const ErrorReport &report = {},
                     const ShellExecutor &execute = ShellExecuteExW);
using PropertiesInvoker = std::function<int(const std::wstring&)>;
int run(const std::vector<std::wstring>& argv);
int run(const std::vector<std::wstring>& argv, const PropertiesInvoker& invoker);

}
