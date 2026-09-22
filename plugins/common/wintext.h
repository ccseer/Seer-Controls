#pragma once

#include <windows.h>

#include <cwctype>
#include <string>
#include <vector>

// Text helpers used by the native Control helpers.
//
// Case-insensitive comparison goes through CompareStringOrdinal instead of
// towlower so ordering and equality stay identical to the Windows ordinal
// semantics the host uses for extensions, and so the result does not depend on
// the C locale of the helper process.
namespace WinText {

inline std::wstring toLower(const std::wstring &value)
{
    std::wstring result(value);
    for (auto &character : result) {
        if (character >= L'A' && character <= L'Z') {
            character = static_cast<wchar_t>(character - L'A' + L'a');
        }
    }
    return result;
}

inline int compareIgnoreCase(const std::wstring &left, const std::wstring &right)
{
    const int result = CompareStringOrdinal(left.c_str(),
                                            static_cast<int>(left.size()),
                                            right.c_str(),
                                            static_cast<int>(right.size()),
                                            TRUE);
    if (result == 0) {
        return left.compare(right);
    }
    return result - CSTR_EQUAL;
}

inline bool equalsIgnoreCase(const std::wstring &left, const std::wstring &right)
{
    return left.size() == right.size() && compareIgnoreCase(left, right) == 0;
}

inline bool startsWith(const std::wstring &value, const std::wstring &prefix)
{
    return value.size() >= prefix.size()
           && value.compare(0, prefix.size(), prefix) == 0;
}

inline bool startsWithIgnoreCase(const std::wstring &value,
                                 const std::wstring &prefix)
{
    return value.size() >= prefix.size()
           && compareIgnoreCase(value.substr(0, prefix.size()), prefix) == 0;
}

inline bool endsWithIgnoreCase(const std::wstring &value,
                               const std::wstring &suffix)
{
    return value.size() >= suffix.size()
           && compareIgnoreCase(value.substr(value.size() - suffix.size()),
                                suffix)
                  == 0;
}

inline bool isSpace(const wchar_t character)
{
    return character == L' ' || character == L'\t' || character == L'\r'
           || character == L'\n' || character == L'\f' || character == L'\v';
}

inline std::wstring trim(const std::wstring &value)
{
    size_t begin = 0;
    size_t end   = value.size();
    while (begin < end && isSpace(value[begin])) {
        ++begin;
    }
    while (end > begin && isSpace(value[end - 1])) {
        --end;
    }
    return value.substr(begin, end - begin);
}

// Win32 error codes reach the user as part of a Control failure toast, so they
// are rendered as text with the code kept as a suffix: a bare number is only
// diagnosable by whoever wrote the call.
inline std::wstring systemErrorMessage(const DWORD error)
{
    wchar_t *buffer = nullptr;
    const DWORD length
        = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER
                             | FORMAT_MESSAGE_FROM_SYSTEM
                             | FORMAT_MESSAGE_IGNORE_INSERTS,
                         nullptr, error, 0, reinterpret_cast<LPWSTR>(&buffer),
                         0, nullptr);
    std::wstring message;
    if (length > 0 && buffer != nullptr) {
        message.assign(buffer, static_cast<size_t>(length));
    }
    if (buffer != nullptr) {
        LocalFree(buffer);
    }
    // The system text ends with a CRLF.
    message = trim(message);
    if (message.empty()) {
        return std::to_wstring(error);
    }
    return message + L" (" + std::to_wstring(error) + L")";
}

inline std::string toUtf8(const std::wstring &value)
{
    if (value.empty()) {
        return std::string();
    }
    const int size = WideCharToMultiByte(CP_UTF8, 0, value.data(),
                                        static_cast<int>(value.size()),
                                        nullptr, 0, nullptr, nullptr);
    if (size <= 0) {
        return std::string();
    }
    std::string result(static_cast<size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
                        result.data(), size, nullptr, nullptr);
    return result;
}

inline std::wstring fromUtf8(const std::string &value)
{
    if (value.empty()) {
        return std::wstring();
    }
    const int size = MultiByteToWideChar(CP_UTF8, 0, value.data(),
                                         static_cast<int>(value.size()),
                                         nullptr, 0);
    if (size <= 0) {
        return std::wstring();
    }
    std::wstring result(static_cast<size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
                        result.data(), size);
    return result;
}

inline std::wstring widen(const std::string &value)
{
    return fromUtf8(value);
}

inline std::wstring join(const std::vector<std::wstring> &parts,
                         const std::wstring &separator)
{
    std::wstring result;
    for (size_t index = 0; index < parts.size(); ++index) {
        if (index != 0) {
            result += separator;
        }
        result += parts[index];
    }
    return result;
}

inline std::vector<std::wstring> splitLines(const std::wstring &value)
{
    std::vector<std::wstring> lines;
    std::wstring current;
    for (const auto character : value) {
        if (character == L'\n') {
            if (!current.empty() && current.back() == L'\r') {
                current.pop_back();
            }
            lines.push_back(current);
            current.clear();
            continue;
        }
        current += character;
    }
    if (!current.empty()) {
        lines.push_back(current);
    }
    return lines;
}

inline std::wstring replaceAll(const std::wstring &value,
                               const std::wstring &needle,
                               const std::wstring &replacement)
{
    if (needle.empty()) {
        return value;
    }
    std::wstring result;
    size_t position = 0;
    for (;;) {
        const auto found = value.find(needle, position);
        if (found == std::wstring::npos) {
            result += value.substr(position);
            return result;
        }
        result += value.substr(position, found - position);
        result += replacement;
        position = found + needle.size();
    }
}

// Bounds a diagnostic before it reaches a message box or a report window so a
// multi-megabyte stderr capture cannot create an unusable UI.
inline std::wstring bound(const std::wstring &value, const size_t limit)
{
    if (value.size() <= limit) {
        return value;
    }
    return value.substr(0, limit) + L"\n[... truncated, "
           + std::to_wstring(value.size() - limit) + L" characters omitted]";
}

// The host reads a failed Control action's stderr and shows it to the user, so
// this is the only channel a helper has for explaining a refusal it exits on.
// The text is bounded before it is encoded, because the host truncates anyway
// and an unbounded capture would only waste the write.
inline void writeStandardError(const std::wstring &message,
                               const size_t limit = 256)
{
    const std::wstring bounded = bound(message, limit);
    if (bounded.empty()) {
        return;
    }
    const int bytesNeeded
        = WideCharToMultiByte(CP_ACP, 0, bounded.c_str(),
                              static_cast<int>(bounded.size()), nullptr, 0,
                              nullptr, nullptr);
    if (bytesNeeded <= 0) {
        return;
    }
    std::string encoded(static_cast<size_t>(bytesNeeded), '\0');
    WideCharToMultiByte(CP_ACP, 0, bounded.c_str(),
                        static_cast<int>(bounded.size()), encoded.data(),
                        bytesNeeded, nullptr, nullptr);
    encoded += "\r\n";
    const HANDLE stderrHandle = GetStdHandle(STD_ERROR_HANDLE);
    if (stderrHandle == nullptr || stderrHandle == INVALID_HANDLE_VALUE) {
        return;
    }
    DWORD written = 0;
    WriteFile(stderrHandle, encoded.data(),
              static_cast<DWORD>(encoded.size()), &written, nullptr);
}

inline std::wstring fromCodePoint(const unsigned long codePoint)
{
    if (codePoint <= 0xFFFFUL) {
        return std::wstring(1, static_cast<wchar_t>(codePoint));
    }
    const unsigned long adjusted = codePoint - 0x10000UL;
    std::wstring result;
    result += static_cast<wchar_t>(0xD800UL + (adjusted >> 10));
    result += static_cast<wchar_t>(0xDC00UL + (adjusted & 0x3FFUL));
    return result;
}

}  // namespace WinText
