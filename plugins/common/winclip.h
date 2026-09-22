#pragma once

#include <windows.h>

#include <string>

// Unicode clipboard text with explicit ownership transfer.
//
// The clipboard is a shared, single-owner resource, so a bounded retry is
// required: another application can hold it open for a short interval without
// anything being wrong. The replacement allocation is prepared before the
// clipboard is emptied, so a failure to allocate never destroys the previous
// contents. Once SetClipboardData succeeds the system owns the handle and the
// helper must not free it.
namespace WinClip {

inline bool openClipboard(const HWND owner, const DWORD retryMs)
{
    const ULONGLONG deadline = GetTickCount64() + retryMs;
    for (;;) {
        if (OpenClipboard(owner)) {
            return true;
        }
        if (GetTickCount64() >= deadline) {
            return false;
        }
        Sleep(20);
    }
}

inline bool setText(const std::wstring &text,
                    const HWND owner,
                    const DWORD retryMs,
                    std::wstring *error)
{
    const size_t bytes = (text.size() + 1) * sizeof(wchar_t);
    HGLOBAL memory     = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (!memory) {
        if (error) {
            *error = L"cannot allocate clipboard memory: "
                     + std::to_wstring(GetLastError());
        }
        return false;
    }
    auto source = static_cast<const wchar_t *>(text.c_str());
    void *target = GlobalLock(memory);
    if (!target) {
        GlobalFree(memory);
        if (error) {
            *error = L"cannot lock clipboard memory: "
                     + std::to_wstring(GetLastError());
        }
        return false;
    }
    CopyMemory(target, source, bytes);
    GlobalUnlock(memory);

    if (!openClipboard(owner, retryMs)) {
        GlobalFree(memory);
        if (error) {
            *error = L"the clipboard was busy for "
                     + std::to_wstring(retryMs) + L" ms";
        }
        return false;
    }
    bool transferred = false;
    if (EmptyClipboard()) {
        if (SetClipboardData(CF_UNICODETEXT, memory)) {
            transferred = true;
        }
        else if (error) {
            *error = L"the clipboard rejected the text: "
                     + std::to_wstring(GetLastError());
        }
    }
    else if (error) {
        *error = L"cannot empty the clipboard: "
                 + std::to_wstring(GetLastError());
    }
    CloseClipboard();
    if (!transferred) {
        GlobalFree(memory);
        return false;
    }
    return true;
}

// Whether overwriting the clipboard would destroy something that cannot be put
// back. An empty getText() only means "no text", not "nothing worth keeping":
// an image or a file list is invisible to getText() and would be lost for good.
// A clipboard that cannot be opened is treated as holding content, because the
// safe answer to "may I overwrite this?" is the one that destroys nothing.
inline bool holdsNonTextContent(const HWND owner)
{
    if (!openClipboard(owner, 1000)) {
        return true;
    }
    bool nonText = false;
    for (UINT format = 0; (format = EnumClipboardFormats(format)) != 0;) {
        // CF_LOCALE rides along with CF_UNICODETEXT: it is metadata, not
        // content of its own.
        if (format != CF_UNICODETEXT && format != CF_TEXT
            && format != CF_OEMTEXT && format != CF_LOCALE) {
            nonText = true;
            break;
        }
    }
    CloseClipboard();
    return nonText;
}

inline std::wstring getText(const HWND owner)
{
    if (!openClipboard(owner, 1000)) {
        return std::wstring();
    }
    std::wstring result;
    if (HANDLE handle = GetClipboardData(CF_UNICODETEXT)) {
        if (const auto text = static_cast<const wchar_t *>(GlobalLock(handle))) {
            result = text;
            GlobalUnlock(handle);
        }
    }
    CloseClipboard();
    return result;
}

}  // namespace WinClip
