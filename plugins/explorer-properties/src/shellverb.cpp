#include "shellverb.h"
#include "shelluiworker.h"
#include <algorithm>

#include "wintext.h"

#include <windows.h>
#include <shellapi.h>

namespace shellverb {

std::optional<ParsedInput> parseArguments(const std::vector<std::wstring> &argv)
{
    return parseArguments(argv, nullptr);
}

std::optional<ParsedInput> parseArguments(const std::vector<std::wstring> &argv,
                                          std::wstring *error)
{
    // The reported text is what the host renders for a refused action, so it
    // says what went wrong in the user's terms rather than naming a parameter.
    // The caller only has to decide which channel carries it.
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
                     const ErrorReport &report)
{
    // The host reads the launcher's stderr, so a reason raised here only reaches
    // it when it goes through the report channel. The fallback covers the runs
    // that have no channel at all -- the public default entry points, and any
    // caller that only has this half -- where writing stderr is still the one
    // thing the host can read, and returning a failure code with no reason is
    // the failure mode this whole path exists to avoid.
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
    if (!ShellExecuteExW(&info)) {
        const auto failure = GetLastError();
        announce(L"Properties could not be opened: the shell refused the "
                 L"request ("
                 + WinText::systemErrorMessage(failure) + L").");
        return kShellFailureExitCode;
    }

    if (info.hProcess)
        CloseHandle(info.hProcess);
    const auto hasWindow = [] {
        bool found = false;
        EnumWindows([](HWND hwnd, LPARAM data) -> BOOL {
            DWORD pid = 0;
            GetWindowThreadProcessId(hwnd, &pid);
            if (pid == GetCurrentProcessId() && IsWindowVisible(hwnd)) {
                *reinterpret_cast<bool *>(data) = true;
                return FALSE;
            }
            return TRUE;
        }, reinterpret_cast<LPARAM>(&found));
        return found;
    };
    // Waits for the dialog this half is responsible for before it can report
    // readiness, which is exactly the pre-readiness work the launcher's shared
    // budget accounts for; the constant is shared so the two cannot drift.
    const auto deadline = GetTickCount64() + ShellUiWorker::kPreReadinessUiWaitMs;
    while (!hasWindow()) {
        if (GetTickCount64() >= deadline) {
            // The host renders a failed Control action's stderr and nothing
            // else, so an exit code alone would be a failure the user has no
            // way to interpret; announce picks the channel that reaches it.
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
        // The host shows a failed Control action's stderr, so the reason has to
        // be written there; an exit code alone would be an unexplained failure.
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
