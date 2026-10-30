#include "hexlauncher.h"

#include <windows.h>
#include <shellapi.h>

#include "wincmd.h"

namespace hexviewer {

bool launchSeerViewerWaiting(const std::wstring &viewerPath,
                             const std::wstring &txtPath,
                             long long waitTimeoutMs)
{
    if (viewerPath.empty() || txtPath.empty()) {
        return false;
    }
    const std::wstring parameters = WinCmd::joinArguments({
        L"--wait-file",
        L"--wait-timeout=" + std::to_wstring(waitTimeoutMs),
        L"--",
        txtPath,
    });

    std::wstring command = WinCmd::quoteArgument(viewerPath) + L" " + parameters;
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, 0,
                       nullptr, nullptr, &startup, &process)) {
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
        return true;
    }

    SHELLEXECUTEINFOW shell{};
    shell.cbSize = sizeof(shell);
    shell.fMask = SEE_MASK_FLAG_NO_UI;
    shell.lpVerb = L"open";
    shell.lpFile = viewerPath.c_str();
    shell.lpParameters = parameters.c_str();
    shell.nShow = SW_SHOWNORMAL;
    return ShellExecuteExW(&shell) != FALSE;
}

}  // namespace hexviewer
