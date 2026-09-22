#pragma once

#include <windows.h>
#include <objbase.h>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "wincmd.h"
#include "winproc.h"

// Launcher side of the two-process UI handoff for the controls whose worker
// layout is a fixed `--input <path>` triple. Helpers with arbitrary option lists
// use UiHandoff in winui.h instead.
namespace ShellUiWorker {
struct CloseHandleDeleter {
    void operator()(HANDLE handle) const { CloseHandle(handle); }
};
using Handle = std::unique_ptr<void, CloseHandleDeleter>;

inline bool pumpMessages(DWORD waitMs = 25)
{
    MsgWaitForMultipleObjects(0, nullptr, FALSE, waitMs, QS_ALLINPUT);
    MSG message{};
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
        if (message.message == WM_QUIT)
            return false;
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    return true;
}

struct Options {
    // A UI process can take several seconds to appear on a cold start, and the
    // readiness event only fires once the intended UI exists. The budget is
    // generous because the visible-window check below makes its exact value
    // less critical, but it still has to stay below the package's manifest
    // timeout: the host bounds the launcher even though it never bounds the
    // worker that owns the UI.
    DWORD readyTimeoutMs = 15000;
};

template<typename Validate, typename Show>
int run(std::vector<std::wstring> arguments, Validate validate, Show show,
        const Options &options = Options())
{
    constexpr auto workerFlag = L"--ui-ready-event";
    if (arguments.size() == 5 && arguments[3] == workerFlag) {
        Handle ready(OpenEventW(EVENT_MODIFY_STATE, FALSE, arguments[4].c_str()));
        arguments.resize(3);
        if (!ready || !validate(arguments))
            return 2;
        return show(arguments, [&] { SetEvent(ready.get()); });
    }
    if (!validate(arguments))
        return 2;

    GUID guid{};
    wchar_t guidText[40]{};
    if (FAILED(CoCreateGuid(&guid)) || !StringFromGUID2(guid, guidText, 40))
        return 1;
    const auto eventName = std::wstring(L"Local\\SeerPluginUi-") + guidText;
    Handle ready(CreateEventW(nullptr, TRUE, FALSE, eventName.c_str()));
    if (!ready)
        return 1;

    std::vector<wchar_t> executable(32768);
    const auto length = GetModuleFileNameW(nullptr, executable.data(),
                                           static_cast<DWORD>(executable.size()));
    if (!length || length >= executable.size())
        return 1;
    auto command = WinCmd::quoteArgument(executable.data());
    for (size_t i = 1; i < arguments.size(); ++i)
        command += L" " + WinCmd::quoteArgument(arguments[i]);
    command += L" " + std::wstring(workerFlag) + L" "
               + WinCmd::quoteArgument(eventName);
    STARTUPINFOW startup{sizeof(startup)};
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(executable.data(), command.data(), nullptr, nullptr, FALSE,
                         DETACHED_PROCESS | CREATE_NEW_PROCESS_GROUP | CREATE_SUSPENDED,
                         nullptr, nullptr, &startup, &process))
        return 1;
    Handle child(process.hProcess);
    Handle thread(process.hThread);
    AllowSetForegroundWindow(process.dwProcessId);
    if (ResumeThread(thread.get()) == static_cast<DWORD>(-1)) {
        TerminateProcess(child.get(), 1);
        return 1;
    }
    // The host timeout covers launch, not the user's time in a system dialog.
    const ULONGLONG deadline = GetTickCount64() + options.readyTimeoutMs;
    int result               = 1;
    for (;;) {
        HANDLE waits[]{ready.get(), child.get()};
        const DWORD wait = WaitForMultipleObjects(2, waits, FALSE, 50);
        if (wait == WAIT_OBJECT_0) {
            result = 0;
            break;
        }
        if (wait == WAIT_OBJECT_0 + 1) {
            DWORD code = 1;
            GetExitCodeProcess(child.get(), &code);
            // An exit before readiness is a failure even when it is exit 0:
            // the intended UI never appeared.
            result = code == 0 ? 1 : static_cast<int>(code);
            break;
        }
        // A window that is already on screen counts as readiness even when the
        // event is late, so a slow child is never killed on a healthy handoff.
        if (WinProc::ownsVisibleWindow(process.dwProcessId)) {
            result = 0;
            break;
        }
        if (GetTickCount64() >= deadline) {
            TerminateProcess(child.get(), 1);
            WaitForSingleObject(child.get(), 1000);
            result = 1;
            break;
        }
    }
    return result;
}
} // namespace ShellUiWorker
