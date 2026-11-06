#include "shellverb.h"
#include "shelluiworker.h"
#include <algorithm>

#include "wintext.h"

#include <windows.h>
#include <shellapi.h>

namespace {

struct WindowSearch {
    DWORD processId;
    bool found = false;
};

}  // namespace

namespace shellverb {

bool hasVisibleWindowForProcess(const DWORD processId)
{
    WindowSearch search{processId};
    EnumWindows(
        [](HWND hwnd, LPARAM data) -> BOOL {
            auto search = reinterpret_cast<WindowSearch *>(data);
            DWORD owner = 0;
            GetWindowThreadProcessId(hwnd, &owner);
            if (owner == search->processId
                && IsWindowVisible(hwnd)) {
                search->found = true;
                return FALSE;
            }
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&search));
    return search.found;
}

std::optional<ParsedInput> parseArguments(const std::vector<std::wstring> &argv)
{
    return parseArguments(argv, nullptr);
}

std::optional<ParsedInput> parseArguments(const std::vector<std::wstring> &argv,
                                          std::wstring *error)
{
    // The reported text is what the user sees for a refused action, so it says
    // what went wrong in the user's terms rather than naming a parameter. The
    // caller only has to decide which channel carries it.
    const auto fail
        = [error](const wchar_t *message) -> std::optional<ParsedInput> {
        if (error)
            *error = std::wstring(L"Properties could not start: ") + message;
        return std::nullopt;
    };

    if (argv.size() != 3 || argv[1] != L"--input")
        return fail(L"expected exactly --input <path>");

    auto inputPath = argv[2];
    std::replace(inputPath.begin(), inputPath.end(), L'/', L'\\');
    if (inputPath.empty())
        return fail(L"input path is empty");

    const auto attributes = GetFileAttributesW(inputPath.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES
        || (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0)
        return fail(L"input must be an existing regular file");

    return ParsedInput{inputPath};
}

int invokeProperties(const std::wstring &inputPath,
                     const std::function<void()> &onReady,
                     const ErrorReport &report,
                     const ShellExecutor &execute)
{
    // A reason raised here only reaches the user when it goes through the report
    // channel: the worker's own output goes nowhere. The fallback covers the runs
    // that have no channel at all -- the public default entry points, and any
    // caller that only has this half -- where the helper's own error dialog is
    // still available, and returning a failure code with no reason is the
    // failure mode this whole path exists to avoid.
    const auto announce = [&](const std::wstring &message) {
        if (report) {
            report(message);
            return;
        }
        WinText::writeStandardError(message);
    };

    SHELLEXECUTEINFOW info{};
    info.cbSize = sizeof(info);
    info.fMask
        = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_INVOKEIDLIST | SEE_MASK_NOASYNC;
    info.lpVerb = L"properties";
    info.lpFile = inputPath.c_str();
    info.nShow  = SW_SHOWNORMAL;
    if (!execute(&info)) {
        const auto failure = GetLastError();
        announce(L"Properties could not be opened: the shell refused the "
                 L"request ("
                 + WinText::systemErrorMessage(failure) + L").");
        return kShellFailureExitCode;
    }

    const auto processHandle = info.hProcess;
    const DWORD processId = processHandle ? GetProcessId(processHandle)
                                         : GetCurrentProcessId();
    if (processHandle)
        CloseHandle(processHandle);
    // The properties verb can create the sheet inside this process and still
    // leave hProcess null. Treating that as "handled by another Shell process"
    // reported readiness before the window existed, and the worker exited and
    // tore the half-built sheet down with it, so the user saw nothing at all.
    // The visible window is therefore the only success signal, whether or not a
    // process handle came back; a verb that really is handled elsewhere has to
    // be confirmed by that window too.
    const auto hasWindow = [processId] {
        return hasVisibleWindowForProcess(processId);
    };
    // Waits for the dialog this half is responsible for before it can report
    // readiness, which is exactly the pre-readiness work the launcher's shared
    // budget accounts for; the constant is shared so the two cannot drift.
    const auto deadline = GetTickCount64() + ShellUiWorker::kPreReadinessUiWaitMs;
    while (!hasWindow()) {
        if (GetTickCount64() >= deadline) {
            // An exit code alone would be a failure the user has no way to
            // interpret, because the host reports nothing after the launch
            // succeeded; announce picks the channel that reaches the user.
            announce(
                L"Properties did not open: no dialog window appeared for this "
                L"file. Another program may own the Properties dialog for it; "
                L"try opening Properties from Explorer instead.");
            return kShellFailureExitCode;
        }
        if (!ShellUiWorker::pumpMessages())
            return kShellFailureExitCode;
    }
    if (onReady)
        onReady();
    while (hasWindow()) {
        if (!ShellUiWorker::pumpMessages())
            break;
    }
    return 0;
}

int run(const std::vector<std::wstring> &argv, const PropertiesInvoker &invoker)
{
    std::wstring error;
    const auto parsed = parseArguments(argv, &error);
    if (!parsed) {
        // The helper shows its own failure dialog, so the reason still has to
        // reach stderr for tests; an exit code alone would be unexplained.
        // parseArguments already worded the reason for the user.
        WinText::writeStandardError(error);
        return kArgumentErrorExitCode;
    }

    return invoker(parsed->inputPath);
}

int run(const std::vector<std::wstring> &argv)
{
    return run(argv, [](const std::wstring &path) { return invokeProperties(path); });
}

}  // namespace shellverb
