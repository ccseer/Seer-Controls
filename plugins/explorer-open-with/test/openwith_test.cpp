#include "openwith.h"

#include <windows.h>

#include <iostream>
#include <string>
#include <vector>

#include "testharness.h"
#include "wincmd.h"
#include "wintext.h"

using TestHarness::check;

namespace {

std::wstring tempFile()
{
    wchar_t temp[MAX_PATH]{};
    GetTempPathW(MAX_PATH, temp);
    std::wstring path = std::wstring(temp) + L"Seer Open With 测试 file.txt";
    HANDLE handle = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle != INVALID_HANDLE_VALUE)
        CloseHandle(handle);
    return path;
}

struct HelperRun {
    bool started = false;
    DWORD exitCode = 0;
    std::wstring standardError;
};

// stderr is the only failure channel the host reads, so it cannot be checked
// from inside the process: a redirected pipe is the only way to observe what
// the helper actually wrote.
HelperRun runHelper(const std::wstring &helper,
                    const std::vector<std::wstring> &arguments)
{
    HelperRun run;
    SECURITY_ATTRIBUTES inheritable{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    HANDLE stderrRead  = nullptr;
    HANDLE stderrWrite = nullptr;
    if (!CreatePipe(&stderrRead, &stderrWrite, &inheritable, 0)
        || !SetHandleInformation(stderrRead, HANDLE_FLAG_INHERIT, 0)) {
        return run;
    }

    std::wstring command = WinCmd::quoteArgument(helper);
    for (const auto &argument : arguments) {
        command += L" " + WinCmd::quoteArgument(argument);
    }

    STARTUPINFOW startup{sizeof(startup)};
    startup.dwFlags    = STARTF_USESTDHANDLES;
    startup.hStdError  = stderrWrite;
    PROCESS_INFORMATION process{};
    const BOOL started = CreateProcessW(nullptr, command.data(), nullptr,
                                        nullptr, TRUE, 0, nullptr, nullptr,
                                        &startup, &process);
    CloseHandle(stderrWrite);
    if (!started) {
        CloseHandle(stderrRead);
        return run;
    }
    run.started = true;
    CloseHandle(process.hThread);

    if (WaitForSingleObject(process.hProcess, 15000) == WAIT_TIMEOUT) {
        TerminateProcess(process.hProcess, 1);
        // Termination is asynchronous; without this wait the exit code can still
        // read STILL_ACTIVE and mask the real one.
        WaitForSingleObject(process.hProcess, 5000);
    }
    GetExitCodeProcess(process.hProcess, &run.exitCode);
    CloseHandle(process.hProcess);

    char buffer[4096]{};
    DWORD read = 0;
    while (ReadFile(stderrRead, buffer, sizeof(buffer), &read, nullptr)
           && read > 0) {
        const auto wideLength
            = MultiByteToWideChar(CP_ACP, 0, buffer, static_cast<int>(read),
                                  nullptr, 0);
        std::wstring wide(static_cast<size_t>(wideLength), L'\0');
        MultiByteToWideChar(CP_ACP, 0, buffer, static_cast<int>(read),
                            wide.data(), wideLength);
        run.standardError += wide;
    }
    CloseHandle(stderrRead);
    return run;
}

void testHelperProcess(const std::wstring &helper)
{
    if (helper.empty()) {
        std::cout << "  [skip] the helper path was not provided\n";
        return;
    }

    // A rejected request must explain itself on stderr, because the host renders
    // stderr and nothing else for a failed Control action.
    const auto rejected = runHelper(helper, {});
    check(rejected.started, "the helper starts");
    check(rejected.exitCode == kArgumentErrorExitCode,
          "a helper without arguments exits with the argument-error code");
    check(rejected.standardError.find(L"Open With could not start")
              != std::wstring::npos,
          "the argument refusal names the action on stderr: "
              + WinText::toUtf8(rejected.standardError));

    // A path that does not exist takes the same branch: the reason has to say
    // why rather than exiting silently.
    const auto missing = runHelper(
        helper, {L"--input", L"Z:\\definitely-missing-openwith-target.txt"});
    check(missing.exitCode == kArgumentErrorExitCode,
          "a missing input exits with the argument-error code");
    check(missing.standardError.find(L"Open With could not start")
              != std::wstring::npos,
          "the missing-input refusal carries a reason on stderr");
}

}  // namespace

int main(int argc, char *argv[])
{
    const auto path = tempFile();
    const std::vector<std::wstring> valid{L"shellopenwith.exe", L"--input", path};
    auto parsed = parseArguments(valid);
    check(parsed.valid && parsed.path == path, "valid regular file");
    check(!parseArguments({L"shellopenwith.exe", L"--input"}).valid, "missing value");
    check(!parseArguments({L"shellopenwith.exe", L"--input", path, L"--input", path}).valid, "duplicate input");
    check(!parseArguments({L"shellopenwith.exe", L"--input", path, L"extra"}).valid, "extra arguments");
    check(!parseArguments({L"shellopenwith.exe", L"--input", std::wstring(path).append(L"-missing")}).valid, "missing path");

    wchar_t temp[MAX_PATH]{};
    GetTempPathW(MAX_PATH, temp);
    check(!parseArguments({L"shellopenwith.exe", L"--input", temp}).valid, "directory");

    int calls = 0;
    OpenWithInvoker success = [&](const std::wstring& received, HWND owner) {
        ++calls;
        check(received == path && owner == nullptr, "invoker arguments");
        return S_OK;
    };
    // The in-process paths must not touch the test's real stderr, which the host
    // would read as a failure of this very run; capture the reason instead so
    // the message is still asserted.
    std::wstring reported;
    const ErrorSink capture = [&](const std::wstring& message) {
        reported = message;
    };
    check(run(valid, success, capture) == 0 && calls == 1,
          "successful invoker returns zero");
    check(reported.empty(), "a successful run reports nothing");

    calls = 0;
    OpenWithInvoker failure = [&](const std::wstring&, HWND) {
        ++calls;
        return E_FAIL;
    };
    check(run(valid, failure, capture) == kShellFailureExitCode && calls == 1,
          "failed HRESULT returns the shell-failure code");
    check(reported.find(L"could not be shown") != std::wstring::npos,
          "a shell failure reports a reason naming the action");

    calls = 0;
    reported.clear();
    check(run({L"shellopenwith.exe", L"--input"}, success, capture)
              == kArgumentErrorExitCode
              && calls == 0,
          "invalid input never invokes and returns the argument-error code");
    check(reported.find(L"Open With could not start") != std::wstring::npos,
          "a rejected argument list reports a reason");

    calls = 0;
    reported.clear();
    check(run({L"shellopenwith.exe", L"--input", temp}, success, capture)
              == kArgumentErrorExitCode
              && calls == 0,
          "directory input never invokes");
    check(reported.find(L"Open With could not start") != std::wstring::npos,
          "a directory input reports a reason");

    const auto missingPath = path + L"-missing";
    calls = 0;
    reported.clear();
    check(run({L"shellopenwith.exe", L"--input", missingPath}, success, capture)
              == kArgumentErrorExitCode
              && calls == 0,
          "missing file input never invokes");
    check(reported.find(L"Open With could not start") != std::wstring::npos,
          "a missing file input reports a reason");

    DeleteFileW(path.c_str());

    // argv[1] is the helper built by CMake; without it the process-level check
    // is skipped rather than silently passing.
    testHelperProcess(argc > 1 ? WinText::fromUtf8(argv[1]) : std::wstring());

    return TestHarness::summarize("shellopenwith_test");
}
