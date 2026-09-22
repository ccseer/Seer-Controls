#pragma once

#include <windows.h>
#include <objbase.h>
#include <algorithm>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "wincmd.h"
#include "winproc.h"
#include "wintext.h"

// Launcher side of the two-process UI handoff for the controls whose worker
// layout is a fixed `--input <path>` triple. Helpers with arbitrary option lists
// use the shared launcher instead.
namespace ShellUiWorker {
// How a helper explains a refusal. Both handoff callers -- the validator and the
// showing half -- receive one, and packages that run the same code in the
// launcher and in the worker declare their APIs in terms of this alias rather
// than each spelling out their own identical std::function type.
using ErrorReport = std::function<void(const std::wstring &)>;

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

// The launcher and the worker run concurrently, so their waits overlap rather
// than adding up, and the two numbers bound different things:
//
//   * A worker that has to wait for its own UI before it can signal readiness
//     spends that time inside the launcher's readiness window. Its wait must
//     therefore be no longer than Options::readyTimeoutMs, or readiness can
//     never arrive in time and the handoff fails on a worker that is healthy.
//   * kLauncherOverheadMs is what the launcher itself costs on top of that
//     window: process creation, its own polling, and teardown. The host's
//     manifest timeout bounds the launcher, so it has to exceed
//     readyTimeoutMs plus this.
inline constexpr DWORD kLauncherOverheadMs = 3000;

// How long a worker waits for the UI it is responsible for before giving up and
// reporting the failure. Exported so the worker that waits and the manifest
// assertion that checks the wait fits inside the readiness window cannot drift
// apart, which is what keeps a handoff from timing out on a healthy worker.
inline constexpr DWORD kPreReadinessUiWaitMs = 3000;

inline DWORD totalHandoffBudgetMs(const Options &options = Options())
{
    return options.readyTimeoutMs + kLauncherOverheadMs;
}

// The worker is spawned detached with no handle inheritance, so it inherits no
// stderr and the host can never read a reason it writes there. A refusal that
// happens in the worker only reaches the host as the launcher's exit code, which
// is why the worker reports its message over a named channel the launcher owns
// and the launcher re-emits it on its own stderr.
//
// The mapping is created by the launcher BEFORE the worker starts, for the same
// reason the readiness event is: a mapping created by the writer alone would be
// destroyed as soon as that writer released its last handle, and the launcher
// would then open nothing. Ownership therefore stays with the launcher.
inline constexpr DWORD kMaxReasonChars = 256;

inline std::wstring reasonChannelName(const std::wstring &eventName)
{
    return eventName + L"-reason";
}

// Creates the shared reason buffer. Returns an owning handle, or nullptr.
inline Handle createReasonChannel(const std::wstring &eventName)
{
    return Handle(CreateFileMappingW(
        INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
        (kMaxReasonChars + 1) * sizeof(wchar_t),
        reasonChannelName(eventName).c_str()));
}

// Owner side: reads back whatever the worker published, or an empty string when
// nothing was written.
inline std::wstring readReason(const Handle &channel)
{
    if (!channel)
        return std::wstring();
    const auto *view = static_cast<const wchar_t *>(
        MapViewOfFile(channel.get(), FILE_MAP_READ, 0, 0, 0));
    if (!view)
        return std::wstring();
    const std::wstring reason(view, wcsnlen(view, kMaxReasonChars));
    UnmapViewOfFile(view);
    return reason;
}

// Worker side: opens the launcher's channel and writes one bounded message.
inline bool publishReason(const std::wstring &eventName,
                          const std::wstring &message)
{
    Handle mapping(OpenFileMappingW(FILE_MAP_WRITE, FALSE,
                                    reasonChannelName(eventName).c_str()));
    if (!mapping)
        return false;
    auto *view = static_cast<wchar_t *>(
        MapViewOfFile(mapping.get(), FILE_MAP_WRITE, 0, 0, 0));
    if (!view)
        return false;
    auto bounded = message;
    if (bounded.size() > kMaxReasonChars)
        bounded.resize(kMaxReasonChars);
    std::copy(bounded.begin(), bounded.end(), view);
    view[bounded.size()] = L'\0';
    UnmapViewOfFile(view);
    return true;
}

// `validate` returns the reason it refused, or an empty optional to accept. It
// does not report the refusal itself, because the two halves of this handoff
// need it on different channels -- the worker can only reach the host through
// the launcher's reason buffer, the launcher only through its own stderr -- and
// a reason handed back to the caller is one that cannot be dropped on either
// side. `show` receives the report to use for anything it fails at afterwards.
template<typename Validate, typename Show>
int run(std::vector<std::wstring> arguments, Validate validate, Show show,
        const Options &options = Options())
{
    constexpr auto workerFlag = L"--ui-ready-event";
    if (arguments.size() == 5 && arguments[3] == workerFlag) {
        const auto readyName = arguments[4];
        Handle ready(OpenEventW(EVENT_MODIFY_STATE, FALSE, readyName.c_str()));
        arguments.resize(3);
        // This process is detached with no inherited handles, so nothing it
        // writes to stderr is readable by the host. The named channel the
        // launcher owns is the only way a refusal raised here can be explained.
        const auto report = [&](const std::wstring &message) {
            publishReason(readyName, message);
            WinText::writeStandardError(message);
        };
        if (!ready) {
            report(L"The UI worker could not open the handoff event.");
            return 2;
        }
        if (const auto refusal = validate(arguments)) {
            report(*refusal);
            return 2;
        }
        return show(arguments, [&] { SetEvent(ready.get()); }, report);
    }
    // The launcher runs where the host reads stderr directly, and this happens
    // before the reason channel exists, so its own channel is the only one
    // available here.
    if (const auto refusal = validate(arguments)) {
        WinText::writeStandardError(*refusal);
        return 2;
    }

    GUID guid{};
    wchar_t guidText[40]{};
    if (FAILED(CoCreateGuid(&guid)) || !StringFromGUID2(guid, guidText, 40))
        return 1;
    const auto eventName = std::wstring(L"Local\\SeerPluginUi-") + guidText;
    Handle ready(CreateEventW(nullptr, TRUE, FALSE, eventName.c_str()));
    if (!ready)
        return 1;
    // Created before the worker starts and held for the launcher's whole
    // lifetime, so the worker's write has somewhere to land.
    const auto reasonChannel = createReasonChannel(eventName);
    if (!reasonChannel)
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
            // Re-emit whatever the worker reported before it died; without this
            // the host would see an exit code with no reason at all.
            const auto reason = readReason(reasonChannel);
            if (!reason.empty())
                WinText::writeStandardError(reason);
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
            const auto reason = readReason(reasonChannel);
            if (!reason.empty())
                WinText::writeStandardError(reason);
            result = 1;
            break;
        }
    }
    return result;
}
} // namespace ShellUiWorker
