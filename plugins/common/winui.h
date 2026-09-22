#pragma once

#include <windows.h>

#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "shelluiworker.h"
#include "wincmd.h"
#include "winpath.h"
#include "winproc.h"
#include "wintext.h"

// UI readiness handoff and plugin-owned visible diagnostics.
//
// The host's Control contract reports completion through the helper's exit code
// and never renders the helper's standard output, so a helper that opens a
// window must report success only once that window exists, and must report
// failures in a window the user can actually see.
//
// The pattern is a two-process handoff:
//   * the launcher validates the request, starts itself again with a named
//     event, and returns as soon as the worker signals readiness;
//   * the worker owns the window for its whole lifetime, so the host timeout
//     never bounds how long the user may interact with it.
//
// This is the same contract ShellUiWorker implements for the controls that
// shipped before it. Both share the launcher-owned reason channel: a worker is
// spawned detached with no handle inheritance, so a refusal it raises reaches
// the host only when the launcher re-emits it on its own stderr. ShellUiWorker
// keeps its own entry point because its worker layout is a fixed
// `--input <path>` triple; the mode flags below accept arbitrary option lists.
namespace UiHandoff {

// How a helper explains a refusal. The showing half receives one, and packages
// that run the same code in the launcher and in the worker declare their APIs
// in terms of this alias rather than spelling out their own identical
// std::function type.
using ErrorReport = ShellUiWorker::ErrorReport;

inline constexpr const wchar_t *kReadyFlag = L"--ui-ready-event";
// The mode marker is value-less; the event name travels in its own option so the
// two can never be confused with each other.
inline constexpr const wchar_t *kMessageFlag = L"--ui-message";
inline constexpr const wchar_t *kMessageEventFlag = L"--ui-message-event";
inline constexpr const wchar_t *kTitleFlag = L"--ui-message-title";
inline constexpr const wchar_t *kBodyFlag = L"--ui-message-body";
inline constexpr const wchar_t *kHeadingFlag = L"--ui-message-heading";
inline constexpr const wchar_t *kStatusFlag = L"--ui-message-status";
inline constexpr const wchar_t *kAutoCloseFlag = L"--ui-message-autoclose";

inline constexpr size_t kMaxMessageCharacters = 8000;

inline std::wstring newEventName()
{
    GUID guid{};
    wchar_t text[40]{};
    if (FAILED(CoCreateGuid(&guid)) || !StringFromGUID2(guid, text, 40)) {
        return std::wstring();
    }
    return std::wstring(L"Local\\SeerControlUi-") + text;
}

inline int scaleForDpi(const HWND window, const int value)
{
    UINT dpi = 96;
    if (window) {
        dpi = GetDpiForWindow(window);
    }
    if (dpi == 0) {
        dpi = 96;
    }
    return MulDiv(value, static_cast<int>(dpi), 96);
}

inline HFONT messageFont()
{
    NONCLIENTMETRICSW metrics{};
    metrics.cbSize = sizeof(metrics);
    if (SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics,
                              0)) {
        return CreateFontIndirectW(&metrics.lfMessageFont);
    }
    return static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
}

struct ButtonSpec {
    int id = 0;
    std::wstring label;
    bool closesWindow = false;
};

struct MessageSpec {
    std::wstring title;
    std::wstring heading;
    std::wstring body;
    std::wstring status;
    std::vector<ButtonSpec> buttons;
    // Non-zero makes the window close itself, which keeps a lightweight success
    // note from becoming a window the user has to dismiss.
    DWORD autoCloseMs = 0;
    std::function<bool(int id, std::wstring *body, std::wstring *status)>
        onAction;
};

namespace Detail {

inline constexpr const wchar_t *kWindowClass = L"SeerControlMessageWindow";

inline void layoutChildren(const HWND window, MessageSpec *spec)
{
    RECT client{};
    GetClientRect(window, &client);
    const int margin  = scaleForDpi(window, 12);
    const int spacing = scaleForDpi(window, 8);
    const int buttonH = scaleForDpi(window, 26);
    const int headingH = spec->heading.empty() ? 0 : scaleForDpi(window, 22);
    const int statusH = scaleForDpi(window, 18);

    const int innerWidth = client.right - margin * 2;
    int top              = margin;

    HWND heading = GetDlgItem(window, 1001);
    HWND body    = GetDlgItem(window, 1002);
    HWND status  = GetDlgItem(window, 1003);

    if (heading && headingH > 0) {
        SetWindowPos(heading, nullptr, margin, top, innerWidth, headingH,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        top += headingH + spacing;
    }
    const int buttonsHeight = spec->buttons.empty()
                                  ? 0
                                  : buttonH + spacing * 2;
    const int bodyHeight
        = client.bottom - top - statusH - margin - buttonsHeight - spacing;
    if (body) {
        SetWindowPos(body, nullptr, margin, top, innerWidth,
                     bodyHeight > 40 ? bodyHeight : 40,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        top += (bodyHeight > 40 ? bodyHeight : 40) + spacing;
    }
    if (status) {
        SetWindowPos(status, nullptr, margin, top, innerWidth, statusH,
                     SWP_NOZORDER | SWP_NOACTIVATE);
    }

    int totalWidth = 0;
    std::vector<int> widths;
    widths.reserve(spec->buttons.size());
    for (const auto &button : spec->buttons) {
        HWND handle = GetDlgItem(window, button.id);
        SIZE size{};
        if (handle) {
            HDC dc = GetDC(window);
            if (dc) {
                HFONT font = reinterpret_cast<HFONT>(
                    SendMessageW(handle, WM_GETFONT, 0, 0));
                HGDIOBJ previous = SelectObject(dc, font);
                GetTextExtentPoint32W(dc, button.label.c_str(),
                                      static_cast<int>(button.label.size()),
                                      &size);
                SelectObject(dc, previous);
                ReleaseDC(window, dc);
            }
        }
        const int width = (size.cx > 0 ? size.cx : scaleForDpi(window, 90))
                          + scaleForDpi(window, 28);
        widths.push_back(width);
        totalWidth += width;
    }
    if (!widths.empty()) {
        totalWidth += spacing * static_cast<int>(widths.size() - 1);
    }
    // Buttons read left to right in the order they were declared, and the row is
    // anchored to the right edge as Windows dialogs do.
    int buttonLeft    = client.right - margin - totalWidth;
    const int buttonY = client.bottom - margin - buttonH;
    for (size_t index = 0; index < spec->buttons.size(); ++index) {
        if (HWND handle = GetDlgItem(window, spec->buttons[index].id)) {
            SetWindowPos(handle, nullptr, buttonLeft, buttonY, widths[index],
                         buttonH, SWP_NOZORDER | SWP_NOACTIVATE);
        }
        buttonLeft += widths[index] + spacing;
    }
}

inline LRESULT CALLBACK windowProcedure(HWND window, UINT message, WPARAM wparam,
                                        LPARAM lparam)
{
    auto *spec = reinterpret_cast<MessageSpec *>(GetWindowLongPtrW(
        window, GWLP_USERDATA));

    switch (message) {
    case WM_CREATE: {
        const auto *create = reinterpret_cast<CREATESTRUCTW *>(lparam);
        spec = static_cast<MessageSpec *>(create->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA,
                          reinterpret_cast<LONG_PTR>(spec));

        HFONT font = messageFont();

        HWND heading = CreateWindowExW(
            0, L"STATIC", spec->heading.c_str(),
            WS_CHILD | WS_VISIBLE | SS_LEFT, 0, 0, 0, 0, window,
            reinterpret_cast<HMENU>(1001), create->hInstance, nullptr);
        HWND body = CreateWindowExW(
            WS_EX_CLIENTEDGE, L"EDIT", spec->body.c_str(),
            WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_LEFT | ES_MULTILINE
                | ES_AUTOVSCROLL | ES_READONLY,
            0, 0, 0, 0, window, reinterpret_cast<HMENU>(1002),
            create->hInstance, nullptr);
        HWND status = CreateWindowExW(
            0, L"STATIC", spec->status.c_str(), WS_CHILD | WS_VISIBLE | SS_LEFT,
            0, 0, 0, 0, window, reinterpret_cast<HMENU>(1003),
            create->hInstance, nullptr);

        for (const auto &button : spec->buttons) {
            CreateWindowExW(
                0, L"BUTTON", button.label.c_str(),
                WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | WS_TABSTOP, 0, 0, 0, 0,
                window, reinterpret_cast<HMENU>(
                            static_cast<INT_PTR>(button.id)),
                create->hInstance, nullptr);
        }
        for (const auto id : {1001, 1002, 1003}) {
            if (HWND child = GetDlgItem(window, id)) {
                SendMessageW(child, WM_SETFONT, reinterpret_cast<WPARAM>(font),
                             TRUE);
            }
        }
        for (const auto &button : spec->buttons) {
            if (HWND child = GetDlgItem(window, button.id)) {
                SendMessageW(child, WM_SETFONT, reinterpret_cast<WPARAM>(font),
                             TRUE);
            }
        }
        if (!heading || !body || !status) {
            return -1;
        }
        return 0;
    }
    case WM_SIZE:
        if (spec) {
            layoutChildren(window, spec);
        }
        return 0;
    case WM_GETMINMAXINFO: {
        auto *info = reinterpret_cast<MINMAXINFO *>(lparam);
        info->ptMinTrackSize.x = scaleForDpi(window, 420);
        info->ptMinTrackSize.y = scaleForDpi(window, 240);
        return 0;
    }
    case WM_COMMAND: {
        if (!spec) {
            return 0;
        }
        const int id = LOWORD(wparam);
        if (HIWORD(wparam) != BN_CLICKED) {
            return 0;
        }
        for (const auto &button : spec->buttons) {
            if (button.id != id) {
                continue;
            }
            if (button.closesWindow) {
                PostMessageW(window, WM_CLOSE, 0, 0);
                return 0;
            }
            std::wstring body   = spec->body;
            std::wstring status = std::wstring();
            if (spec->onAction && spec->onAction(id, &body, &status)) {
                spec->body = body;
                if (HWND edit = GetDlgItem(window, 1002)) {
                    SetWindowTextW(edit, body.c_str());
                    SendMessageW(edit, EM_SETSEL, 0, 0);
                    SendMessageW(edit, EM_SCROLLCARET, 0, 0);
                }
                if (HWND label = GetDlgItem(window, 1003)) {
                    SetWindowTextW(label, status.c_str());
                }
            }
            return 0;
        }
        return 0;
    }
    case WM_TIMER:
        if (wparam == 1) {
            PostMessageW(window, WM_CLOSE, 0, 0);
        }
        return 0;
    case WM_CLOSE:
        DestroyWindow(window);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    default:
        break;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}

inline bool ensureWindowClass(const HINSTANCE instance)
{
    WNDCLASSEXW existing{};
    existing.cbSize = sizeof(existing);
    if (GetClassInfoExW(instance, kWindowClass, &existing)) {
        return true;
    }
    WNDCLASSEXW definition{};
    definition.cbSize        = sizeof(definition);
    definition.style         = CS_HREDRAW | CS_VREDRAW;
    definition.lpfnWndProc   = Detail::windowProcedure;
    definition.hInstance     = instance;
    definition.hCursor = LoadCursorW(nullptr, reinterpret_cast<LPCWSTR>(IDC_ARROW));
    definition.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    definition.lpszClassName = kWindowClass;
    if (RegisterClassExW(&definition)) {
        return true;
    }
    return GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
}

inline void centerOnWorkArea(const HWND window)
{
    RECT bounds{};
    if (!SystemParametersInfoW(SPI_GETWORKAREA, 0, &bounds, 0)) {
        return;
    }
    RECT size{};
    GetWindowRect(window, &size);
    const int width  = size.right - size.left;
    const int height = size.bottom - size.top;
    const int x      = bounds.left + ((bounds.right - bounds.left) - width) / 2;
    const int y      = bounds.top + ((bounds.bottom - bounds.top) - height) / 2;
    SetWindowPos(window, HWND_TOP, x, y, 0, 0,
                 SWP_NOSIZE | SWP_NOACTIVATE);
}

}  // namespace Detail

// Shows a window that owns its own message loop. `onReady` fires once the window
// is created, laid out and visible, so the launcher's success report and the
// user's ability to interact with the window stay in the right order.
inline int showMessageWindow(const HINSTANCE instance,
                             const MessageSpec &spec,
                             const std::function<void()> &onReady)
{
    if (!Detail::ensureWindowClass(instance)) {
        return 1;
    }
    MessageSpec mutableSpec = spec;
    const int width  = scaleForDpi(nullptr, 720);
    const int height = scaleForDpi(nullptr, 460);
    HWND window = CreateWindowExW(
        WS_EX_APPWINDOW, Detail::kWindowClass,
        spec.title.empty() ? L"Seer Control" : spec.title.c_str(),
        WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, width, height,
        nullptr, nullptr, instance, &mutableSpec);
    if (!window) {
        return 1;
    }
    Detail::centerOnWorkArea(window);
    ShowWindow(window, SW_SHOWNORMAL);
    UpdateWindow(window);
    SetForegroundWindow(window);
    if (spec.autoCloseMs > 0) {
        SetTimer(window, 1, spec.autoCloseMs, nullptr);
    }
    if (onReady) {
        onReady();
    }

    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        if (!IsDialogMessageW(window, &message)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    }
    return 0;
}

// Worker mode of the shared plugin-owned diagnostic window.
inline bool isMessageInvocation(const std::vector<std::wstring> &arguments)
{
    for (const auto &argument : arguments) {
        if (argument == kMessageFlag) {
            return true;
        }
    }
    return false;
}

inline int runMessageWorker(const std::vector<std::wstring> &arguments)
{
    const auto scan = WinCmd::scanOptions(
        arguments, 1, {kMessageFlag},
        {kMessageEventFlag, kTitleFlag, kBodyFlag, kHeadingFlag, kStatusFlag,
         kAutoCloseFlag});
    std::wstring eventName;
    if (!WinCmd::optionValue(scan, L"ui-message-event", &eventName)
        || eventName.empty()) {
        return 2;
    }
    std::wstring title;
    std::wstring body;
    std::wstring heading;
    std::wstring status;
    WinCmd::optionValue(scan, L"ui-message-title", &title);
    WinCmd::optionValue(scan, L"ui-message-body", &body);
    WinCmd::optionValue(scan, L"ui-message-heading", &heading);
    WinCmd::optionValue(scan, L"ui-message-status", &status);
    std::wstring autoClose;
    const DWORD autoCloseMs
        = WinCmd::optionValue(scan, L"ui-message-autoclose", &autoClose)
              ? static_cast<DWORD>(_wtoi(autoClose.c_str()))
              : 0;

    HANDLE ready = OpenEventW(EVENT_MODIFY_STATE, FALSE, eventName.c_str());
    if (!ready) {
        return 1;
    }

    MessageSpec spec;
    spec.title       = title.empty() ? L"Seer" : title;
    spec.heading     = heading;
    spec.body        = WinText::bound(body, kMaxMessageCharacters);
    spec.status      = status;
    spec.autoCloseMs = autoCloseMs;
    spec.buttons.push_back({1, L"Close", true});

    const int result = showMessageWindow(
        GetModuleHandleW(nullptr), spec, [&] { SetEvent(ready); });
    CloseHandle(ready);
    return result;
}

// Launches the plugin-owned diagnostic window in its own process and returns as
// soon as it is visible, so a failure report never blocks the managed operation.
inline void spawnMessage(const std::wstring &title,
                         const std::wstring &heading,
                         const std::wstring &body,
                         const DWORD readyTimeoutMs = 4000,
                         const std::wstring &status = std::wstring(),
                         const DWORD autoCloseMs = 0)
{
    const std::wstring executable = WinProc::modulePath();
    if (executable.empty()) {
        return;
    }
    const std::wstring eventName = newEventName();
    if (eventName.empty()) {
        return;
    }
    HANDLE ready = CreateEventW(nullptr, TRUE, FALSE, eventName.c_str());
    if (!ready) {
        return;
    }

    std::vector<std::wstring> arguments;
    arguments.push_back(kMessageFlag);
    arguments.push_back(kMessageEventFlag);
    arguments.push_back(eventName);
    arguments.push_back(kTitleFlag);
    arguments.push_back(title);
    arguments.push_back(kHeadingFlag);
    arguments.push_back(heading);
    arguments.push_back(kBodyFlag);
    arguments.push_back(WinText::bound(body, kMaxMessageCharacters));
    if (!status.empty()) {
        arguments.push_back(kStatusFlag);
        arguments.push_back(status);
    }
    if (autoCloseMs > 0) {
        arguments.push_back(kAutoCloseFlag);
        arguments.push_back(std::to_wstring(autoCloseMs));
    }

    WinProc::LaunchOptions options;
    options.detached             = true;
    options.suspended            = true;
    options.tryBreakawayFromJob  = true;

    WinProc::Child child;
    std::wstring error;
    if (!WinProc::createChild(executable, arguments, options, nullptr, &child,
                              &error)) {
        CloseHandle(ready);
        return;
    }
    if (!WinProc::resumeChild(&child, &error)) {
        TerminateProcess(child.process, 1);
        child.close();
        CloseHandle(ready);
        return;
    }

    const ULONGLONG deadline = GetTickCount64() + readyTimeoutMs;
    for (;;) {
        HANDLE waits[]{ready, child.process};
        const DWORD wait = WaitForMultipleObjects(2, waits, FALSE, 50);
        if (wait == WAIT_OBJECT_0) {
            break;
        }
        if (wait == WAIT_OBJECT_0 + 1) {
            break;
        }
        if (GetTickCount64() >= deadline) {
            break;
        }
    }
    CloseHandle(ready);
    child.close();
}

// Writes a bounded failure reason to this process's stderr, so the host can
// present it: Seer captures the stderr of a failed Control action and shows it
// in a completion toast. The host decodes stderr with the ANSI codepage, and
// every message this package produces is ASCII, so CP_ACP is lossless here.
// The write is best-effort: a process without a stderr handle (for example a
// detached elevation child) simply reports nothing, and failures to write are
// never surfaced.
inline void reportFailure(const std::wstring &message)
{
    WinText::writeStandardError(message);
}

// True when the process owns at least one visible unowned top-level window. Used
// as the fallback readiness signal: the readiness event is the primary one, but a
// cold start can take longer than any fixed budget, and killing a window that is
// already on screen would turn a working handoff into a failure.
inline bool ownsVisibleWindow(const DWORD pid)
{
    return WinProc::ownsVisibleWindow(pid);
}

struct HandoffOptions {
    // A UI process can take several seconds to appear on a cold start, and the
    // readiness event only fires once the window is actually created. The budget
    // is generous; the window check below makes the exact value less critical.
    DWORD readyTimeoutMs = 15000;
};

// Launcher mode of the two-process UI handoff.
//
// validate() runs in both processes so an invalid request fails before any
// window exists, and it returns the refusal as text instead of reporting it:
// the half that refuses may be the detached worker, whose stderr the host can
// never read, so the handoff picks the channel that reaches it (the launcher's
// own stderr, or the launcher's reason buffer). show() runs only in the worker,
// must call its `ready` callback once the intended UI is on screen, and
// receives an ErrorReport for anything it fails at afterwards; it then keeps
// running for as long as the UI lives.
inline int run(const std::vector<std::wstring> &arguments,
               const std::function<std::optional<std::wstring>(
                   const std::vector<std::wstring> &)> &validate,
               const std::function<int(const std::vector<std::wstring> &,
                                       const std::function<void()> &,
                                       const ErrorReport &)> &show,
               const HandoffOptions &options = {})
{
    if (arguments.empty()) {
        reportFailure(L"no command line was provided.");
        return 2;
    }

    if (arguments.size() >= 3
        && arguments[arguments.size() - 2] == kReadyFlag) {
        const std::wstring eventName = arguments.back();
        std::vector<std::wstring> worker(arguments.begin(),
                                         arguments.end() - 2);
        // This process is detached with no inherited handles, so nothing it
        // writes to stderr is readable by the host. The named channel the
        // launcher owns is the only way a refusal raised here can be explained.
        const auto report = [&](const std::wstring &message) {
            ShellUiWorker::publishReason(eventName, message);
            WinText::writeStandardError(message);
        };
        HANDLE ready = OpenEventW(EVENT_MODIFY_STATE, FALSE, eventName.c_str());
        if (!ready) {
            report(L"the UI worker could not open the handoff event.");
            return 1;
        }
        if (const auto refusal = validate(worker)) {
            report(*refusal);
            CloseHandle(ready);
            return 2;
        }
        const int result = show(worker, [&] { SetEvent(ready); }, report);
        CloseHandle(ready);
        return result;
    }

    // The launcher runs where the host reads stderr directly, and this happens
    // before the reason channel exists, so its own channel is the only one
    // available here.
    if (const auto refusal = validate(arguments)) {
        WinText::writeStandardError(*refusal);
        return 2;
    }

    const std::wstring eventName = newEventName();
    if (eventName.empty()) {
        return 1;
    }
    HANDLE ready = CreateEventW(nullptr, TRUE, FALSE, eventName.c_str());
    if (!ready) {
        return 1;
    }
    // Created before the worker starts and held for the launcher's whole
    // lifetime, so the worker's write has somewhere to land.
    const auto reasonChannel = ShellUiWorker::createReasonChannel(eventName);
    if (!reasonChannel) {
        CloseHandle(ready);
        return 1;
    }

    std::vector<std::wstring> childArguments(arguments.begin() + 1,
                                             arguments.end());
    childArguments.push_back(kReadyFlag);
    childArguments.push_back(eventName);

    WinProc::LaunchOptions launch;
    launch.detached            = true;
    launch.suspended           = true;
    launch.tryBreakawayFromJob = true;

    WinProc::Child child;
    std::wstring error;
    if (!WinProc::createChild(WinProc::modulePath(), childArguments, launch,
                              nullptr, &child, &error)) {
        CloseHandle(ready);
        // The failure text is already built by createChild; dropping it here
        // would leave the host with an exit code and no explanation at all.
        reportFailure(error.empty()
                          ? std::wstring(L"the UI worker process could not be "
                                         L"started.")
                          : error);
        return 1;
    }
    AllowSetForegroundWindow(child.pid);
    if (!WinProc::resumeChild(&child, &error)) {
        TerminateProcess(child.process, 1);
        child.close();
        CloseHandle(ready);
        reportFailure(error.empty()
                          ? std::wstring(L"the UI worker process could not be "
                                         L"started.")
                          : error);
        return 1;
    }

    // Re-emits whatever the worker published before it died; without this the
    // host would see an exit code with no reason at all.
    const auto reportPublishedReason = [&] {
        const auto reason = ShellUiWorker::readReason(reasonChannel);
        if (!reason.empty()) {
            reportFailure(reason);
        }
    };

    const ULONGLONG deadline = GetTickCount64() + options.readyTimeoutMs;
    int result               = 1;
    for (;;) {
        HANDLE waits[]{ready, child.process};
        const DWORD wait = WaitForMultipleObjects(2, waits, FALSE, 50);
        if (wait == WAIT_OBJECT_0) {
            result = 0;
            break;
        }
        if (wait == WAIT_OBJECT_0 + 1) {
            DWORD code = 1;
            GetExitCodeProcess(child.process, &code);
            // An exit before readiness is a failure even when it is exit 0:
            // the intended UI never appeared.
            result = code == 0 ? 1 : static_cast<int>(code);
            reportPublishedReason();
            break;
        }
        if (GetTickCount64() >= deadline) {
            // The readiness event has not fired, but if the worker already owns
            // a visible window the UI is genuinely on screen: report success
            // and let it live. Only a worker with no window at all is a
            // failure.
            if (ownsVisibleWindow(child.pid)) {
                result = 0;
                break;
            }
            TerminateProcess(child.process, 1);
            WaitForSingleObject(child.process, 1000);
            // The worker may have published a reason before it was terminated;
            // if it did not, the timeout itself is the only explanation left.
            const auto reason = ShellUiWorker::readReason(reasonChannel);
            WinText::writeStandardError(
                reason.empty()
                    ? std::wstring(L"the UI worker did not become ready "
                                   L"before the handoff timed out.")
                    : reason);
            result = 1;
            break;
        }
        // A visible window is also accepted while waiting, so a slow event never
        // costs the user an extra poll interval.
        if (ownsVisibleWindow(child.pid)) {
            result = 0;
            break;
        }
    }

    CloseHandle(ready);
    child.close();
    return result;
}

}  // namespace UiHandoff
