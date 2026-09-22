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
// The host renders a failed Control action's stderr and nothing else, so a
// non-zero exit without a written reason is a failure the user cannot act on.
// Capturing the pipe is the only way to assert that contract; the capture itself
// is shared with the other packages that assert it.

}  // namespace

int main()
{
    const auto directory   = tempDirectory();
    const auto spacedPath  = createFile(directory, L"regular file.txt");
    const auto unicodePath = createFile(directory, L"Unicode \u6d4b\u8bd5.txt");
    const std::vector<std::wstring> valid{L"shellverb_properties.exe",
                                          L"--input", spacedPath};

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
        // Captured, because an uncaptured run would print the host-facing
        // refusal next to this test's own FAIL: lines.
        StderrHarness::Capture capture;
        check(shellverb::run({L"shellverb_properties.exe", L"--input"}, succeeds)
                      == shellverb::kArgumentErrorExitCode
                  && calls == 0,
              "returns a distinct argument failure code without invoking");
        refused = capture.drain();
    }
    check(refused.find("Properties could not start") != std::string::npos,
          "explains an argument refusal on stderr, not just the exit code");

    // A refusal comes back as text for the host to render, whichever half of the
    // handoff raised it, so the wording is part of that contract.
    {
        std::wstring error;
        check(!shellverb::parseArguments(
                  {L"shellverb_properties.exe", L"--input",
                   spacedPath + L"-missing"},
                  &error)
                  && error.find(L"Properties could not start")
                         != std::wstring::npos,
              "a validation refusal comes back worded for the host");
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
    // to reach the host through stderr. The validator here is the real one, so
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
              "a launcher-side refusal explains itself on the stderr the host "
              "reads");
    }

    DeleteFileW(spacedPath.c_str());
    DeleteFileW(unicodePath.c_str());
    RemoveDirectoryW(directory.c_str());
    return TestHarness::summarize("shellverb_properties_test");
}
