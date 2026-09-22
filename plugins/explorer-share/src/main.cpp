#include <windows.h>
#include <shellapi.h>

#include <functional>
#include <optional>

#include "share.h"
#include "shelluiworker.h"

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int)
{
    int argc        = 0;
    const auto argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv)
        return 2;

    std::vector<std::wstring> arguments;
    arguments.reserve(static_cast<size_t>(argc));
    for (int i = 0; i < argc; ++i) {
        arguments.emplace_back(argv[i]);
    }
    const int result = ShellUiWorker::run(arguments,
        [](const auto &args) -> std::optional<std::wstring> {
            // The refusal travels back to the handoff, which knows the channel
            // that reaches the host from whichever process it is running in.
            std::wstring error;
            if (parseArguments(args, &error).valid) {
                return std::nullopt;
            }
            return error;
        },
        [](const auto &args, const auto &ready,
           const std::function<void(const std::wstring &)> &report) {
            WindowsSharePlatform platform;
            return ::run(args, platform, ready, report);
        },
        // Without this the launcher gives up after the default readiness wait,
        // which is shorter than the payload wait this worker performs before it
        // can signal -- the handoff would fail on a worker that is still
        // healthy, and the worker's own timeout could never be read back.
        handoffOptions());
    LocalFree(argv);
    return result;
}
