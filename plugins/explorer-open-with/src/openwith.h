#pragma once

#include <functional>
#include <string>
#include <vector>

#include <windows.h>
#include <winerror.h>

// Every manifest lists success_exit_codes [0] only, so any nonzero value is a
// reported failure; the two below are distinct so a log can tell a rejected
// request from one the shell refused.
inline constexpr int kArgumentErrorExitCode = 2;
inline constexpr int kShellFailureExitCode = 1;

struct ParsedInput {
    bool valid = false;
    std::wstring path;
};

// The host renders a failed Control action's stderr and nothing else, so a
// refusal has to be written there. That makes the sink part of the contract
// rather than an implementation detail: only the helper process may write to
// the real stderr, because a test that exercised the rejection paths in-process
// would otherwise print host-facing failure text next to its own result.
using ErrorSink = std::function<void(const std::wstring&)>;

ParsedInput parseArguments(const std::vector<std::wstring>& arguments, std::wstring* error = nullptr);
HRESULT invokeOpenWith(const std::wstring& path, HWND owner);
using OpenWithInvoker = std::function<HRESULT(const std::wstring&, HWND)>;
int run(const std::vector<std::wstring>& arguments, const OpenWithInvoker& invoker,
        const ErrorSink& report);
int run(const std::vector<std::wstring>& arguments);
