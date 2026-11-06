#pragma once

#include <windows.h>

#include <string>

// A detached Control helper has no channel back to the host: once the
// launch succeeds the host never observes the process again. The helper
// therefore presents every refusal itself. SEER_TEST_NO_UI=1 suppresses
// the dialog so CTest never blocks on a modal window and can keep
// asserting stderr.
namespace WinUserError {

inline constexpr wchar_t kTitle[] = L"Seer Control Error";

inline bool uiSuppressed()
{
    wchar_t value[8] = {};
    const DWORD read = GetEnvironmentVariableW(L"SEER_TEST_NO_UI",
                                               value, 8);
    return read > 0 && read < 8 && std::wstring(value) == L"1";
}

inline void showErrorDialog(const std::wstring &message)
{
    if (uiSuppressed()) {
        return;
    }
    MessageBoxW(nullptr, message.c_str(), kTitle,
                MB_ICONERROR | MB_OK | MB_SETFOREGROUND);
}

}  // namespace WinUserError
