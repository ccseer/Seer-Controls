#pragma once

#include <windows.h>

#include <string>
#include <vector>

#include "winpath.h"
#include "wintext.h"

// Filesystem helpers for the native Control test targets.
namespace FileProbe {

// Creates every missing component, because CreateDirectoryW does not.
inline bool makeDirectoryTree(const std::wstring &path)
{
    const std::wstring normalized = WinPath::stripTrailingSeparators(
        WinPath::nativeSeparators(path));
    size_t start = 0;
    if (WinPath::isUncPath(normalized)) {
        size_t separators = 0;
        size_t position   = 2;
        while (position < normalized.size() && separators < 2) {
            if (normalized[position] == WinPath::kSeparator) {
                ++separators;
            }
            ++position;
        }
        start = position;
    }
    else if (normalized.size() >= 2 && normalized[1] == L':') {
        start = 3;
    }
    for (;;) {
        const auto position
            = normalized.find(WinPath::kSeparator, start);
        const auto end
            = position == std::wstring::npos ? normalized.size() : position;
        const std::wstring partial = normalized.substr(0, end);
        if (!partial.empty()) {
            CreateDirectoryW(WinPath::addExtendedPrefix(partial).c_str(),
                             nullptr);
        }
        if (position == std::wstring::npos) {
            break;
        }
        start = position + 1;
    }
    return WinPath::isDirectory(normalized);
}

inline bool writeTextFile(const std::wstring &path, const std::string &bytes)
{
    HANDLE handle = CreateFileW(WinPath::addExtendedPrefix(path).c_str(),
                                GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        return false;
    }
    DWORD written = 0;
    const bool ok = WriteFile(handle, bytes.data(),
                              static_cast<DWORD>(bytes.size()), &written,
                              nullptr)
                    && written == bytes.size();
    CloseHandle(handle);
    return ok;
}

inline std::string readTextFile(const std::wstring &path)
{
    HANDLE handle = CreateFileW(WinPath::addExtendedPrefix(path).c_str(),
                                GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL,
                                nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        return std::string();
    }
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(handle, &size) || size.QuadPart <= 0
        || size.QuadPart > 64LL * 1024 * 1024) {
        CloseHandle(handle);
        return std::string();
    }
    std::string bytes(static_cast<size_t>(size.QuadPart), '\0');
    DWORD read = 0;
    const bool ok
        = ReadFile(handle, bytes.data(), static_cast<DWORD>(bytes.size()), &read,
                   nullptr)
          && read == bytes.size();
    CloseHandle(handle);
    return ok ? bytes : std::string();
}

inline std::wstring readTextFileWide(const std::wstring &path)
{
    return WinText::fromUtf8(readTextFile(path));
}

// Reads a `key=value` line from a probe report.
inline std::wstring reportField(const std::string &report,
                                const std::string &key)
{
    const std::string needle = key + "=";
    size_t position          = 0;
    for (;;) {
        const auto found = report.find(needle, position);
        if (found == std::string::npos) {
            return std::wstring();
        }
        if (found == 0 || report[found - 1] == '\n'
            || report[found - 1] == '\r') {
            const auto newline = report.find("\r\n", found);
            const auto stop
                = newline == std::string::npos ? report.size() : newline;
            return WinText::fromUtf8(report.substr(
                found + needle.size(), stop - found - needle.size()));
        }
        position = found + 1;
    }
}

inline std::wstring tempRoot(const wchar_t *label)
{
    std::vector<wchar_t> buffer(32768, L'\0');
    const DWORD written = GetTempPathW(static_cast<DWORD>(buffer.size()),
                                       buffer.data());
    std::wstring root = written ? std::wstring(buffer.data(), written)
                                : std::wstring(L".\\");
    const std::wstring result
        = WinPath::join(root, std::wstring(label) + L"_"
                                  + std::to_wstring(GetCurrentProcessId()));
    makeDirectoryTree(result);
    return result;
}

}  // namespace FileProbe
