#include <windows.h>
#include <shellapi.h>
#include <algorithm>
#include "shelluiworker.h"

#include <iostream>
#include <optional>

#include "shellverb.h"
#include "stderrharness.h"
#include "wincmd.h"
#include "testharness.h"

using TestHarness::check;

namespace {

std::wstring tempDirectory()
{
    wchar_t path[MAX_PATH]{};
    GetTempPathW(MAX_PATH, path);
    std::wstring directory = path;
    directory += L"Seer Shell Verb ";
    directory += std::to_wstring(GetCurrentProcessId());
    CreateDirectoryW(directory.c_str(), nullptr);
    return directory;
}

std::wstring createFile(const std::wstring &directory, const std::wstring &name)
{
    const auto path = directory + L"\\" + name;
    const auto handle
        = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                      FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle != INVALID_HANDLE_VALUE)
        CloseHandle(handle);
    return path;
}
// stderr is the test-observable half of the failure contract; the helper also
// presents the reason in its own dialog. Capturing the pipe is the only way to
// assert the text without a modal window, and the capture is shared with the
// other packages that assert it.

}  // namespace

int main()
{
    const auto testWindow = CreateWindowExW(WS_EX_TOOLWINDOW, L"STATIC",
                                            L"SeerPropertiesTestWindow",
                                            WS_POPUP, 0, 0, 1, 1, nullptr,
                                            nullptr, GetModuleHandleW(nullptr),
                                            nullptr);
    check(testWindow && !shellverb::hasVisibleWindowForProcess(
                               GetCurrentProcessId()),
          "ignores a hidden window");
    ShowWindow(testWindow, SW_SHOW);
    check(testWindow && shellverb::hasVisibleWindowForProcess(
                              GetCurrentProcessId()),
          "finds a visible window owned by the requested process");
    check(!shellverb::hasVisibleWindowForProcess(0),
          "does not match windows belonging to another process");
    DestroyWindow(testWindow);
    const auto directory   = tempDirectory();
    const auto spacedPath  = createFile(directory, L"regular file.txt");
    const auto unicodePath = createFile(directory, L"Unicode \u6d4b\u8bd5.txt");
    const std::vector<std::wstring> valid{L"shellverb_properties.exe",
                                          L"--input", spacedPath};

    {
        bool ready = false;
        std::wstring error;
        const auto result = shellverb::invokeProperties(
            spacedPath, [&] { ready = true; },
            [&](const std::wstring &message) { error = message; },
            [&](SHELLEXECUTEINFOW *info) -> BOOL {
                check(std::wstring(info->lpVerb) == L"properties"
                          && std::wstring(info->lpFile) == spacedPath
                          && (info->fMask & SEE_MASK_NOASYNC) != 0,
                      "requests a synchronous Properties handoff");
                info->hProcess = nullptr;
                return TRUE;
            });
        check(result == shellverb::kShellFailureExitCode && !ready
                  && error.find(L"no dialog window appeared")
                         != std::wstring::npos,
              "a null process handle without a window is a failure, not a silent "
              "success");
    }
    {
        const auto window = CreateWindowExW(
            WS_EX_TOOLWINDOW, L"STATIC", L"SeerPropertiesLocalTest", WS_POPUP,
            0, 0, 1, 1, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        ShowWindow(window, SW_SHOW);
        bool ready = false;
        const auto result = shellverb::invokeProperties(
            spacedPath, [&] {
                ready = true;
                DestroyWindow(window);
            }, {}, [](SHELLEXECUTEINFOW *info) -> BOOL {
                info->hProcess = nullptr;
                return TRUE;
            });
        check(window && result == 0 && ready && !IsWindow(window),
              "keeps an in-process Properties window alive until readiness");
    }
    {
        const auto window = CreateWindowExW(
            WS_EX_TOOLWINDOW, L"STATIC", L"SeerPropertiesProcessTest", WS_POPUP,
            0, 0, 1, 1, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        ShowWindow(window, SW_SHOW);
        bool ready = false;
        const auto result = shellverb::invokeProperties(
            spacedPath, [&] {
                ready = true;
                DestroyWindow(window);
            }, {}, [](SHELLEXECUTEINFOW *info) -> BOOL {
                info->hProcess = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION,
                                             FALSE, GetCurrentProcessId());
                check(info->hProcess != nullptr,
                      "creates a queryable process handle");
                return TRUE;
            });
        check(window && result == 0 && ready,
              "reads the returned process identity before closing its handle");
    }
    {
        bool ready = false;
        std::wstring error;
        const auto result = shellverb::invokeProperties(
            spacedPath, [&] { ready = true; },
            [&](const std::wstring &message) { error = message; },
            [](SHELLEXECUTEINFOW *) -> BOOL {
                SetLastError(ERROR_ACCESS_DENIED);
                return FALSE;
            });
        check(result == shellverb::kShellFailureExitCode && !ready
                  && error.find(L"shell refused") != std::wstring::npos,
              "reports a refused Shell request without signaling readiness");
    }

    const auto parsed = shellverb::parseArguments(valid);
    check(parsed && parsed->inputPath == spacedPath,
          "accepts an existing file with spaces");
    const auto unicode = shellverb::parseArguments(
        {L"shellverb_properties.exe", L"--input", unicodePath});
    check(unicode && unicode->inputPath == unicodePath,
          "accepts a Unicode path");
    check(!shellverb::parseArguments({L"shellverb_properties.exe", L"--input"}),
          "rejects a missing value");
    check(!shellverb::parseArguments({L"shellverb_properties.exe", L"--input",
                                      spacedPath, L"--input", unicodePath}),
          "rejects duplicate input");
    check(!shellverb::parseArguments(
              {L"shellverb_properties.exe", L"--input", spacedPath, L"extra"}),
          "rejects extra arguments");
    check(!shellverb::parseArguments(
              {L"shellverb_properties.exe", L"--input", directory}),
          "rejects a directory");
    check(!shellverb::parseArguments({L"shellverb_properties.exe", L"--input",
                                      spacedPath + L"-missing"}),
          "rejects a missing path");

    auto forwardPath = spacedPath;
    std::replace(forwardPath.begin(), forwardPath.end(), L'\\', L'/');
    const auto normalized = shellverb::parseArguments(
        {L"shellverb_properties.exe", L"--input", forwardPath});
    check(normalized && normalized->inputPath == spacedPath,
          "normalizes host paths before invoking the shell");
    for (const auto value : {L"", L"space path", L"C:\\folder\\", L"a\"b"}) {
        const auto command
            = std::wstring(L"helper ") + WinCmd::quoteArgument(value);
        int count = 0;
        const auto args = CommandLineToArgvW(command.c_str(), &count);
        check(args && count == 2 && std::wstring(args[1]) == value,
              "worker argument quoting round trips through Windows parsing");
        if (args)
            LocalFree(args);
    }

    int calls = 0;
    const shellverb::PropertiesInvoker succeeds
        = [&](const std::wstring &inputPath) {
              ++calls;
              check(inputPath == spacedPath,
                    "passes the accepted path to the invoker");
              return 0;
          };
    check(shellverb::run(valid, succeeds) == 0 && calls == 1,
          "returns zero for a successful invocation");

    calls                                    = 0;
    const shellverb::PropertiesInvoker fails = [&](const std::wstring &) {
        ++calls;
        return shellverb::kShellFailureExitCode;
    };
    check(shellverb::run(valid, fails) == shellverb::kShellFailureExitCode
              && calls == 1,
          "returns a distinct shell failure code");
    calls = 0;
    std::string refused;
    {
        // Captured, because an uncaptured run would print the refusal next to
        // this test's own FAIL: lines.
        StderrHarness::Capture capture;
        check(shellverb::run({L"shellverb_properties.exe", L"--input"}, succeeds)
                      == shellverb::kArgumentErrorExitCode
                  && calls == 0,
              "returns a distinct argument failure code without invoking");
        refused = capture.drain();
    }
    check(refused.find("Properties could not start") != std::string::npos,
          "explains an argument refusal on stderr, not just the exit code");

    // A refusal comes back as text for the caller to present, whichever half of
    // the handoff raised it, so the wording is part of that contract.
    {
        std::wstring error;
        check(!shellverb::parseArguments(
                  {L"shellverb_properties.exe", L"--input",
                   spacedPath + L"-missing"},
                  &error)
                  && error.find(L"Properties could not start")
                         != std::wstring::npos,
              "a validation refusal comes back worded for the user");
    }

    const auto handoffShow = [](const std::vector<std::wstring> &, const auto &,
                                const auto &) { return 0; };
    // The worker half of a handoff that never left the process: the launcher's
    // event and channel are created here first, because the worker refuses to
    // show anything without them.
    {
        const std::wstring handoffEvent = L"Local\\SeerShellVerbReportTest";
        ShellUiWorker::Handle ready(
            CreateEventW(nullptr, TRUE, FALSE, handoffEvent.c_str()));
        const auto handoffChannel
            = ShellUiWorker::createReasonChannel(handoffEvent);
        check(static_cast<bool>(ready) && static_cast<bool>(handoffChannel),
              "the handoff event and reason channel can be created");

        // Captured so the copy the worker also writes to its own stderr, which a
        // detached process would not even have, stays out of ctest output.
        StderrHarness::Capture capture;
        const auto handoff = ShellUiWorker::run(
            {L"shellverb_properties.exe", L"--input", spacedPath,
             L"--ui-ready-event", handoffEvent},
            [](const std::vector<std::wstring> &)
                -> std::optional<std::wstring> {
                return std::wstring(L"Properties could not start: refused");
            },
            handoffShow);
        check(handoff == shellverb::kArgumentErrorExitCode,
              "a worker-side validation refusal keeps the argument failure code");
        // The channel is what matters: a detached worker's stderr write goes
        // nowhere, so this asserts the reason landed where the launcher can read
        // it rather than only where it happens to also be written here.
        check(ShellUiWorker::readReason(handoffChannel)
                  == L"Properties could not start: refused",
              "the refusal reached the launcher's reason channel");
        capture.drain();
    }

    // A launcher-side refusal happens before any reason channel exists, so it has
    // to stay observable in tests. The validator here is the real one, so
    // the wording and the channel are both covered by production code rather than
    // by a stub that could not fail this way.
    {
        std::string launcherRefusal;
        {
            StderrHarness::Capture capture;
            std::wstring error;
            const auto handoff = ShellUiWorker::run(
                {L"shellverb_properties.exe", L"--input",
                 directory + L"\\does-not-exist.txt"},
                [&](const std::vector<std::wstring> &args)
                    -> std::optional<std::wstring> {
                    if (shellverb::parseArguments(args, &error)) {
                        return std::nullopt;
                    }
                    return error;
                },
                handoffShow);
            check(handoff == shellverb::kArgumentErrorExitCode,
                  "a launcher-side validation refusal exits with the argument "
                  "code");
            launcherRefusal = capture.drain();
        }
        check(launcherRefusal.find("Properties could not start")
                  != std::string::npos,
              "a launcher-side refusal explains itself on its own stderr");
    }

    DeleteFileW(spacedPath.c_str());
    DeleteFileW(unicodePath.c_str());
    RemoveDirectoryW(directory.c_str());
    return TestHarness::summarize("shellverb_properties_test");
}
