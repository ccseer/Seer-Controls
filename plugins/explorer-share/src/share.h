#pragma once

#include <functional>
#include <string>
#include <vector>

#include <windows.h>
#include <winerror.h>

#include "shelluiworker.h"

struct ParsedInput {
    bool valid = false;
    std::wstring path;
};

struct ISharePlatform {
    virtual HRESULT show(const std::wstring& path, std::function<void()> onDataRequested = nullptr) = 0;
    virtual ~ISharePlatform() = default;
};

class WindowsSharePlatform : public ISharePlatform {
public:
    HRESULT show(const std::wstring& path, std::function<void()> onDataRequested = nullptr) override;
};

// Mirrors shellverb's ErrorReport: the showing half runs in the detached UI
// worker, so a reason it raises reaches the host only when the launcher
// re-emits it. The shared alias keeps the two handoff callers in step.
using ErrorReport = ShellUiWorker::ErrorReport;

// The worker waits this long for WinRT to hand over the share payload, and it
// cannot signal readiness before that happens. Because the launcher and the
// worker run concurrently, this is a wait inside the launcher's readiness
// window, so it has to fit inside Options::readyTimeoutMs; the manifest test
// asserts that against handoffOptions() below.
inline constexpr DWORD kSharePayloadWaitMs = 30000;

// Start-up margin over the payload wait: process creation, apartment init, and
// creating the window the share UI is anchored to.
inline constexpr DWORD kShareStartupMarginMs = 5000;

// The handoff this package actually runs with. Both main.cpp and the manifest
// test read it, so the timeout the host enforces and the wait the worker
// performs are checked against the same numbers instead of drifting apart.
inline ShellUiWorker::Options handoffOptions()
{
    return ShellUiWorker::Options{kSharePayloadWaitMs + kShareStartupMarginMs};
}

// Human-readable text for a failure the host would otherwise render as a bare
// exit code because this process has no stderr the host can read.
std::wstring describeFailure(HRESULT result);

// A refusal comes back as text for the host to render rather than being written
// here, because the caller knows which channel carries it: the launcher writes
// its stderr, the worker can only reach the user through the launcher's reason
// channel. Worded for the user, since the host renders it verbatim.
ParsedInput parseArguments(const std::vector<std::wstring>& arguments,
                           std::wstring* error = nullptr);
int run(const std::vector<std::wstring>& arguments, ISharePlatform& platform,
        std::function<void()> onReady = {}, const ErrorReport& report = {});
int run(const std::vector<std::wstring>& arguments);
