#pragma once

#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace terminalhere {

enum class ShellChoice { Auto, WindowsTerminal, PowerShell7, WindowsPowerShell, CommandPrompt };

enum class ShellKind { WindowsTerminal, PowerShell7, WindowsPowerShell, CommandPrompt };

// Returns the absolute path of an installed shell, or an empty string.
using ShellLocator = std::function<std::wstring(ShellKind)>;

struct Request {
    std::wstring directory;
    ShellChoice choice = ShellChoice::Auto;
    bool elevated = false;
};

struct LaunchPlan {
    ShellKind shell = ShellKind::CommandPrompt;
    std::wstring executable;
    std::vector<std::wstring> arguments;
    std::wstring workingDirectory;
    bool elevated = false;
};

struct PlanResult {
    bool ok = false;
    int exitCode = 1;
    std::wstring message;
    LaunchPlan plan;
};

// Windows Terminal uses ';' as its command delimiter and re-parses the argument
// itself, so a directory containing ';' is not fixed by CRT quoting. Windows
// Terminal 1.24 accepts a backslash-escaped ';' and starts in the requested
// directory, including when the ';' directly follows a path separator.
std::wstring escapeForWindowsTerminal(const std::wstring &value);

std::optional<Request> parseArguments(const std::vector<std::wstring> &arguments,
                                      std::wstring *error = nullptr);

ShellLocator systemShellLocator();

PlanResult plan(const Request &request, const ShellLocator &locator);

// Returns a WinExit code. Fills `message` with an actionable explanation for
// every failure so the caller can present it in a window.
int execute(const LaunchPlan &plan, std::wstring *message);

int run(const std::vector<std::wstring> &arguments, const ShellLocator &locator);
int run(const std::vector<std::wstring> &arguments);

}  // namespace terminalhere
