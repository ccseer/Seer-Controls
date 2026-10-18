#include "launcher.h"

#include <windows.h>
#include <shellapi.h>

namespace seer_ocr {

bool launchSeerViewerWaiting(
    const std::wstring &seerviewer_exe,
    const std::wstring &txt_path,
    int wait_timeout_ms)
{
    if (seerviewer_exe.empty() || txt_path.empty()) {
        return false;
    }

    std::wstring params = L"--wait-file --wait-timeout=" + std::to_wstring(wait_timeout_ms)
                          + L" -- \"" + txt_path + L"\"";

    // First attempt: CreateProcessW
    std::wstring cmd = L"\"" + seerviewer_exe + L"\" " + params;
    STARTUPINFOW si = {sizeof(si)};
    PROCESS_INFORMATION pi = {};
    if (CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        return true;
    }

    // Fallback: ShellExecuteExW
    SHELLEXECUTEINFOW sei = {sizeof(sei)};
    sei.fMask        = SEE_MASK_FLAG_NO_UI;
    sei.lpVerb       = L"open";
    sei.lpFile       = seerviewer_exe.c_str();
    sei.lpParameters = params.c_str();
    sei.nShow        = SW_SHOWNORMAL;

    if (ShellExecuteExW(&sei)) {
        return true;
    }

    return false;
}

}  // namespace seer_ocr
