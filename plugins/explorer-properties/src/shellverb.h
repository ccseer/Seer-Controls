#pragma once

#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "shelluiworker.h"

namespace shellverb {

struct ParsedInput {
    std::wstring inputPath;
};

inline constexpr int kArgumentErrorExitCode = 2;
inline constexpr int kShellFailureExitCode = 1;

// A reason raised by the showing half after validation accepted the request; the
// launcher re-emits it on the stderr the host actually reads. The alias is owned
// by the handoff so every user of it agrees on the signature.
using ErrorReport = ShellUiWorker::ErrorReport;

std::optional<ParsedInput> parseArguments(const std::vector<std::wstring>& argv);
// A refusal comes back as text for the host to render rather than being written
// here, because the caller knows which channel carries it: the launcher writes
// its stderr, the worker can only reach the user through the launcher's reason
// channel. Worded for the user, since the host renders it verbatim.
std::optional<ParsedInput> parseArguments(const std::vector<std::wstring>& argv,
                                          std::wstring* error);
int invokeProperties(const std::wstring& inputPath,
                     const std::function<void()> &onReady = {},
                     const ErrorReport &report = {});
using PropertiesInvoker = std::function<int(const std::wstring&)>;
int run(const std::vector<std::wstring>& argv);
int run(const std::vector<std::wstring>& argv, const PropertiesInvoker& invoker);

}
