#include "terminalhere.h"

#include <windows.h>

#include "wincmd.h"
#include "winexit.h"
#include "winpath.h"
#include "winproc.h"
#include "wintext.h"

namespace terminalhere {

namespace {

// Windows Terminal's wt.exe is a redirector: with -w new it hands the command to
// the terminal process and exits. The exit code is the only documented signal
// available for that handoff, so the launcher waits for it.
constexpr DWORD kHandoffTimeoutMs = 8000;

// A console shell that dies immediately never gave the user a shell; the window
// itself is created by the system before CreateProcessW returns.
constexpr DWORD kConsoleFailureWindowMs = 400;

// The consent prompt is the user's to answer, so the elevation hop gets a
// user-interaction budget rather than the plain launch budget.
constexpr DWORD kElevationTimeoutMs = 90000;

std::wstring environmentValue(const wchar_t *name)
{
    std::vector<wchar_t> buffer(32768, L'\0');
    const DWORD written = GetEnvironmentVariableW(
        name, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (written == 0 || written >= buffer.size()) {
        return std::wstring();
    }
    return std::wstring(buffer.data(), written);
}

std::wstring firstExisting(const std::vector<std::wstring> &candidates)
{
    for (const auto &candidate : candidates) {
        if (WinProc::isExecutableFile(candidate)) {
            return candidate;
        }
    }
    return std::wstring();
}

const wchar_t *shellName(const ShellKind kind)
{
    switch (kind) {
    case ShellKind::WindowsTerminal:
        return L"Windows Terminal";
    case ShellKind::PowerShell7:
        return L"PowerShell 7 (pwsh)";
    case ShellKind::WindowsPowerShell:
        return L"Windows PowerShell";
    case ShellKind::CommandPrompt:
        return L"Command Prompt";
    }
    return L"shell";
}

PlanResult failure(const int exitCode, const std::wstring &message)
{
    PlanResult result;
    result.ok       = false;
    result.exitCode = exitCode;
    result.message  = message;
    return result;
}

}  // namespace

std::wstring escapeForWindowsTerminal(const std::wstring &value)
{
    std::wstring result;
    result.reserve(value.size() + 4);
    for (const auto character : value) {
        if (character == L';') {
            result += L'\\';
        }
        result += character;
    }
    return result;
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
    const auto scan = WinCmd::scanOptions(arguments, 1, {L"admin"},
                                          {L"input", L"shell"});
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
    if (!WinCmd::optionValue(scan, L"input", &rawInput)) {
        return fail(L"--input <directory> is required");
    }
    if (rawInput.empty()) {
        return fail(L"--input cannot be empty");
    }
    if (!WinPath::lexicalPath(rawInput, &request.directory)) {
        return fail(L"the selected path cannot be resolved: " + rawInput);
    }
    if (!WinPath::isDirectory(request.directory)) {
        return fail(L"the selection is not an existing directory: "
                    + request.directory);
    }

    std::wstring shell;
    if (WinCmd::optionValue(scan, L"shell", &shell)) {
        const auto lowered = WinText::toLower(WinText::trim(shell));
        if (lowered == L"auto") {
            request.choice = ShellChoice::Auto;
        }
        else if (lowered == L"wt") {
            request.choice = ShellChoice::WindowsTerminal;
        }
        else if (lowered == L"pwsh") {
            request.choice = ShellChoice::PowerShell7;
        }
        else if (lowered == L"powershell") {
            request.choice = ShellChoice::WindowsPowerShell;
        }
        else if (lowered == L"cmd") {
            request.choice = ShellChoice::CommandPrompt;
        }
        else {
            return fail(L"--shell must be one of auto, wt, pwsh, powershell, "
                        L"cmd; got '" + shell + L"'");
        }
    }
    request.elevated = WinCmd::hasOption(scan, L"admin");
    return request;
}

ShellLocator systemShellLocator()
{
    return [](const ShellKind kind) -> std::wstring {
        switch (kind) {
        case ShellKind::WindowsTerminal: {
            const auto local = environmentValue(L"LOCALAPPDATA");
            if (!local.empty()) {
                const auto alias = firstExisting(
                    {WinPath::join(local,
                                   L"Microsoft\\WindowsApps\\wt.exe")});
                if (!alias.empty()) {
                    return alias;
                }
            }
            return WinProc::resolveExecutable(L"wt.exe");
        }
        case ShellKind::PowerShell7: {
            const auto programFiles = environmentValue(L"ProgramFiles");
            if (!programFiles.empty()) {
                const auto installed = firstExisting(
                    {WinPath::join(programFiles, L"PowerShell\\7\\pwsh.exe")});
                if (!installed.empty()) {
                    return installed;
                }
            }
            return WinProc::resolveExecutable(L"pwsh.exe");
        }
        case ShellKind::WindowsPowerShell: {
            const auto root = environmentValue(L"SystemRoot");
            if (!root.empty()) {
                const auto installed = firstExisting({WinPath::join(
                    root, L"System32\\WindowsPowerShell\\v1.0\\powershell.exe")});
                if (!installed.empty()) {
                    return installed;
                }
            }
            return WinProc::resolveExecutable(L"powershell.exe");
        }
        case ShellKind::CommandPrompt: {
            const auto root = environmentValue(L"SystemRoot");
            if (!root.empty()) {
                const auto installed
                    = firstExisting({WinPath::join(root, L"System32\\cmd.exe")});
                if (!installed.empty()) {
                    return installed;
                }
            }
            return WinProc::resolveExecutable(L"cmd.exe");
        }
        }
        return std::wstring();
    };
}

PlanResult plan(const Request &request, const ShellLocator &locator)
{
    if (request.directory.empty()) {
        return failure(WinExit::kUsage, L"no directory was selected");
    }

    const auto locate = [&](const ShellKind kind) {
        return locator ? locator(kind) : std::wstring();
    };

    bool explicitlyRequested = true;
    ShellKind chosen         = ShellKind::CommandPrompt;
    switch (request.choice) {
    case ShellChoice::WindowsTerminal:
        chosen = ShellKind::WindowsTerminal;
        break;
    case ShellChoice::PowerShell7:
        chosen = ShellKind::PowerShell7;
        break;
    case ShellChoice::WindowsPowerShell:
        chosen = ShellKind::WindowsPowerShell;
        break;
    case ShellChoice::CommandPrompt:
        chosen = ShellKind::CommandPrompt;
        break;
    case ShellChoice::Auto:
        explicitlyRequested = false;
        if (!locate(ShellKind::WindowsTerminal).empty()) {
            chosen = ShellKind::WindowsTerminal;
        }
        else if (!locate(ShellKind::PowerShell7).empty()) {
            chosen = ShellKind::PowerShell7;
        }
        else if (!locate(ShellKind::WindowsPowerShell).empty()) {
            chosen = ShellKind::WindowsPowerShell;
        }
        else if (!locate(ShellKind::CommandPrompt).empty()) {
            chosen = ShellKind::CommandPrompt;
        }
        else {
            return failure(
                WinExit::kMissingDependency,
                L"No supported shell was found. Install Windows Terminal or "
                L"enable Windows PowerShell, or pass --shell with an installed "
                L"shell.");
        }
        break;
    }

    const std::wstring executable = locate(chosen);
    if (executable.empty()) {
        const std::wstring name = shellName(chosen);
        if (explicitlyRequested) {
            return failure(WinExit::kMissingDependency,
                           name + L" was requested with --shell but was not "
                                  L"found on this system. Nothing was started, "
                                  L"because silently substituting another shell "
                                  L"would contradict the request.");
        }
        return failure(WinExit::kMissingDependency,
                       name + L" is not installed.");
    }

    if (chosen == ShellKind::CommandPrompt
        && WinPath::isUncPath(request.directory)) {
        // Checked before the existence test on purpose: the limitation belongs
        // to the shell, so the user is told what they can act on instead of
        // being told the share does not exist.
        return failure(
            WinExit::kUnsupported,
            L"Command Prompt cannot use a UNC path as its starting directory (\\"
                + request.directory
                + L"). Run the action again with --shell wt, --shell pwsh, or "
                  L"--shell powershell, or map the share to a drive letter.");
    }

    if (!WinPath::isDirectory(request.directory)) {
        return failure(WinExit::kNotFound,
                       L"the selection is not an existing directory: "
                           + request.directory);
    }

    LaunchPlan planned;
    planned.shell            = chosen;
    planned.executable       = executable;
    planned.workingDirectory = request.directory;
    planned.elevated         = request.elevated;

    if (chosen == ShellKind::WindowsTerminal) {
        // -w new forces a new window, new-tab carries the starting directory,
        // and the directory is escaped because Windows Terminal re-parses the
        // argument itself.
        planned.arguments.push_back(L"-w");
        planned.arguments.push_back(L"new");
        planned.arguments.push_back(L"new-tab");
        planned.arguments.push_back(L"-d");
        planned.arguments.push_back(
            escapeForWindowsTerminal(request.directory));
    }
    // The console shells receive no target argument at all: the directory is
    // passed as the process working directory, so no quoting rule, cmd parser,
    // or PowerShell parser ever sees it.

    PlanResult result;
    result.ok   = true;
    result.plan = planned;
    return result;
}

int execute(const LaunchPlan &launchPlan, std::wstring *message)
{
    const auto fail = [message](const int code, const std::wstring &text) {
        if (message) {
            *message = text;
        }
        return code;
    };

    if (launchPlan.executable.empty()) {
        return fail(WinExit::kMissingDependency,
                    L"no shell executable was resolved");
    }

    if (launchPlan.elevated) {
        const auto outcome = WinProc::runElevated(
            launchPlan.executable, launchPlan.arguments,
            launchPlan.workingDirectory, kElevationTimeoutMs);
        if (!outcome.completed) {
            return fail(WinExit::kTimeout, outcome.error);
        }
        if (outcome.cancelled) {
            return fail(WinExit::kCancelled,
                        L"Elevation was cancelled at the consent prompt, so no "
                        L"terminal was started.");
        }
        if (!outcome.started) {
            return fail(WinExit::kFailure,
                        outcome.error.empty()
                            ? L"the elevated terminal could not be started"
                            : outcome.error);
        }
        // ShellExecuteExW returns only after the elevated process exists, which
        // is the strongest signal available across the elevation boundary; the
        // elevated starting directory and elevation state remain manual checks.
        return WinExit::kOk;
    }

    if (launchPlan.shell == ShellKind::WindowsTerminal) {
        WinProc::CaptureResult capture;
        std::wstring error;
        if (!WinProc::runContained(launchPlan.executable, launchPlan.arguments,
                                   launchPlan.workingDirectory, nullptr,
                                   kHandoffTimeoutMs, 64 * 1024, &capture,
                                   &error)) {
            return fail(capture.timedOut ? WinExit::kTimeout : WinExit::kFailure,
                        capture.timedOut
                            ? L"Windows Terminal did not confirm the handoff "
                              L"within "
                                  + std::to_wstring(kHandoffTimeoutMs / 1000)
                                  + L" s. The terminal window may still have "
                                    L"opened; check for a new window before "
                                    L"retrying."
                            : (error.empty() ? L"Windows Terminal could not be "
                                               L"started"
                                             : error));
        }
        if (capture.exitCode != 0) {
            std::wstring detail
                = WinText::trim(WinText::fromUtf8(capture.standardError));
            if (detail.empty()) {
                detail = WinText::trim(
                    WinText::fromUtf8(capture.standardOutput));
            }
            std::wstring text = L"Windows Terminal rejected the request (exit "
                                + std::to_wstring(capture.exitCode) + L").";
            if (!detail.empty()) {
                text += L"\n\n" + WinText::bound(detail, 512);
            }
            return fail(WinExit::kFailure, text);
        }
        return WinExit::kOk;
    }

    WinProc::LaunchOptions options;
    options.workingDirectory    = launchPlan.workingDirectory;
    options.newConsole          = true;
    options.tryBreakawayFromJob = true;
    options.suspended           = true;

    WinProc::Child child;
    std::wstring error;
    if (!WinProc::createChild(launchPlan.executable, launchPlan.arguments,
                              options, nullptr, &child, &error)) {
        return fail(WinExit::kFailure,
                    error.empty() ? L"the terminal could not be started" : error);
    }
    if (!WinProc::resumeChild(&child, &error)) {
        WinProc::terminateChild(&child);
        return fail(WinExit::kFailure, error);
    }

    const DWORD wait = WaitForSingleObject(child.process,
                                           kConsoleFailureWindowMs);
    if (wait == WAIT_OBJECT_0) {
        DWORD code = 1;
        GetExitCodeProcess(child.process, &code);
        child.close();
        std::wstring text = std::wstring(shellName(launchPlan.shell))
                            + L" exited immediately (exit "
                            + std::to_wstring(code) + L").";
        if (launchPlan.shell == ShellKind::CommandPrompt) {
            text += L"\n\nCommand Prompt refuses a UNC or unreachable starting "
                    L"directory; use --shell pwsh or map the share to a drive "
                    L"letter.";
        }
        return fail(WinExit::kFailure, text);
    }

    child.close();
    return WinExit::kOk;
}

int run(const std::vector<std::wstring> &arguments, const ShellLocator &locator)
{
    std::wstring error;
    const auto request = parseArguments(arguments, &error);
    if (!request) {
        // The host surfaces the captured stderr of a failed Control action in
        // a toast, so the reason is written there instead of a window.
        WinText::writeStandardError(error);
        return WinExit::kUsage;
    }

    const auto planned = plan(*request, locator);
    if (!planned.ok) {
        WinText::writeStandardError(planned.message);
        return planned.exitCode;
    }

    std::wstring message;
    const int code = execute(planned.plan, &message);
    if (code != WinExit::kOk) {
        WinText::writeStandardError(message);
    }
    return code;
}

int run(const std::vector<std::wstring> &arguments)
{
    return run(arguments, systemShellLocator());
}

}  // namespace terminalhere
