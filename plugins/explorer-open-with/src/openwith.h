#pragma once

#include <functional>
#include <string>
#include <vector>

#include <windows.h>
#include <winerror.h>

// The manifest keeps success_exit_codes [0] for schema completeness; the host
// never reads a Control helper's exit code. The two values below stay distinct
// so a log or a test can tell a rejected request from one the shell refused.
inline constexpr int kArgumentErrorExitCode = 2;
inline constexpr int kShellFailureExitCode = 1;

struct ParsedInput {
    bool valid = false;
    std::wstring path;
};

// The sink is where a refusal goes: the helper's own entry point points it at
// the error dialog and the diagnostic stderr. That makes the sink part of the
// contract rather than an implementation detail: only the helper process may
// present the failure, because a test that exercised the rejection paths
// in-process would otherwise raise a message box next to its own result.
using ErrorSink = std::function<void(const std::wstring&)>;

ParsedInput parseArguments(const std::vector<std::wstring>& arguments, std::wstring* error = nullptr);
HRESULT invokeOpenWith(const std::wstring& path, HWND owner);
using OpenWithInvoker = std::function<HRESULT(const std::wstring&, HWND)>;
int run(const std::vector<std::wstring>& arguments, const OpenWithInvoker& invoker,
        const ErrorSink& report);
int run(const std::vector<std::wstring>& arguments);
