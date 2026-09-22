#include <objbase.h>
#include <shellapi.h>
#include <windows.h>

#include <optional>
#include <string>
#include <vector>

#include "shellverb.h"
#include "shelluiworker.h"
#include "wintext.h"

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int)
{
    const HRESULT comResult = CoInitializeEx(
        nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    if (FAILED(comResult))
        return shellverb::kShellFailureExitCode;

    int argc        = 0;
    const auto argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv) {
        CoUninitialize();
        return shellverb::kArgumentErrorExitCode;
    }

    std::vector<std::wstring> arguments;
    arguments.reserve(static_cast<size_t>(argc));
    for (int index = 0; index < argc; ++index)
        arguments.emplace_back(argv[index]);

    // The request is parsed once per process and the result is handed to the
    // showing half, so validation and showing can never disagree about which
    // file was asked for, and re-validation cannot cost a second opportunity for
    // the file to disappear within this process. The handoff still validates
    // again in the worker, so a file removed between the two processes is caught
    // there instead of here.
    std::wstring error;
    std::optional<shellverb::ParsedInput> requested;
    const auto result = ShellUiWorker::run(
        arguments,
        [&](const std::vector<std::wstring> &args)
            -> std::optional<std::wstring> {
            // The refusal travels back to the handoff, which knows the channel
            // that reaches the host from whichever process it is running in.
            requested = shellverb::parseArguments(args, &error);
            if (requested) {
                return std::nullopt;
            }
            return error;
        },
        [&](const std::vector<std::wstring> &, const auto &ready,
            const auto &failureReport) {
            if (!requested) {
                return shellverb::kArgumentErrorExitCode;
            }
            return shellverb::invokeProperties(requested->inputPath, ready,
                                               failureReport);
        });
    LocalFree(argv);
    CoUninitialize();
    return result;
}
