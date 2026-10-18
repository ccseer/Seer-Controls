#include "paths.h"

#include "hash.h"

#include <windows.h>
#include <algorithm>
#include <fstream>
#include <vector>

namespace seer_ocr {

namespace {

// Length of the directory fingerprint prefixed to every output base name.
constexpr size_t kFingerprintLength = 8;

std::string toUtf8(const std::wstring &wstr)
{
    if (wstr.empty()) return {};
    const int size = WideCharToMultiByte(CP_UTF8, 0, wstr.data(), static_cast<int>(wstr.size()),
                                         nullptr, 0, nullptr, nullptr);
    if (size <= 0) {
        return {};
    }
    std::string result(static_cast<size_t>(size), 0);
    WideCharToMultiByte(CP_UTF8, 0, wstr.data(), static_cast<int>(wstr.size()),
                        result.data(), size, nullptr, nullptr);
    return result;
}

// Widens a lowercase hex string to UTF-16; every byte is ASCII by construction.
std::wstring hexToWide(const std::string &hex)
{
    std::wstring out;
    out.reserve(hex.size());
    for (char ch : hex) {
        out.push_back(static_cast<wchar_t>(static_cast<unsigned char>(ch)));
    }
    return out;
}

// Fingerprint of the directory holding the target file. Two files with the same
// name in different folders must not share one output set: the waiting
// SeerViewer polls <base>.ocr.txt by mtime, so a collision lets one document's
// result be delivered as another's.
// Windows paths are case-insensitive and accept both separators, so the bytes
// are normalized first; only ASCII is folded so non-ASCII text stays intact.
std::wstring directoryFingerprint(const std::wstring &input_file)
{
    const size_t last_sep = input_file.find_last_of(L"\\/");
    const std::wstring dir = (last_sep != std::wstring::npos)
                                 ? input_file.substr(0, last_sep)
                                 : std::wstring();

    std::string dir_utf8 = toUtf8(dir);
    for (char &ch : dir_utf8) {
        if (ch == '/') {
            ch = '\\';
        }
        else if (ch >= 'A' && ch <= 'Z') {
            ch = static_cast<char>(ch - 'A' + 'a');
        }
    }

    const std::string hex = md5Hex(dir_utf8);
    if (hex.size() < kFingerprintLength) {
        return std::wstring(kFingerprintLength, L'0');
    }
    return hexToWide(hex.substr(0, kFingerprintLength));
}

bool isReservedDosDeviceName(const std::wstring &name)
{
    static const wchar_t *kReserved[] = {
        L"CON", L"PRN", L"AUX", L"NUL",
        L"COM1", L"COM2", L"COM3", L"COM4", L"COM5", L"COM6", L"COM7", L"COM8", L"COM9",
        L"LPT1", L"LPT2", L"LPT3", L"LPT4", L"LPT5", L"LPT6", L"LPT7", L"LPT8", L"LPT9"
    };

    std::wstring upper = name;
    std::transform(upper.begin(), upper.end(), upper.begin(), ::towupper);

    for (const auto *res : kReserved) {
        if (upper == res) {
            return true;
        }
    }
    return false;
}

}  // namespace

std::wstring sanitizeBaseName(const std::wstring &input_file)
{
    if (input_file.empty()) {
        return L"unnamed";
    }

    // Strip directory components
    size_t last_sep = input_file.find_last_of(L"\\/");
    std::wstring name = (last_sep != std::wstring::npos) ? input_file.substr(last_sep + 1)
                                                         : input_file;

    // Filter invalid characters
    std::wstring sanitized;
    sanitized.reserve(name.size());
    for (wchar_t ch : name) {
        if (ch < 32 || ch == L'<' || ch == L'>' || ch == L':' || ch == L'"'
            || ch == L'/' || ch == L'\\' || ch == L'|' || ch == L'?' || ch == L'*') {
            sanitized += L'_';
        }
        else {
            sanitized += ch;
        }
    }

    // Trim trailing dots and spaces
    while (!sanitized.empty() && (sanitized.back() == L' ' || sanitized.back() == L'.')) {
        sanitized.pop_back();
    }

    if (sanitized.empty()) {
        return L"unnamed";
    }

    // Avoid DOS device names
    if (isReservedDosDeviceName(sanitized)) {
        sanitized = L"_" + sanitized;
    }

    // Truncate to 80 characters
    if (sanitized.size() > 80) {
        sanitized = sanitized.substr(0, 80);
        while (!sanitized.empty() && (sanitized.back() == L' ' || sanitized.back() == L'.')) {
            sanitized.pop_back();
        }
        if (sanitized.empty()) {
            return L"unnamed";
        }
    }

    return sanitized;
}

std::wstring outputBaseName(const std::wstring &input_file)
{
    // The ".ocr" infix is part of the contract: sweepOldOutputs() globs
    // "*.ocr.*" to reclaim aged files, so it has to stay in the name.
    return directoryFingerprint(input_file) + L" - " + sanitizeBaseName(input_file);
}

OcrOutputPaths resolveOutputPaths(const std::wstring &input_file)
{
    OcrOutputPaths paths;

    wchar_t temp_buf[MAX_PATH] = {0};
    DWORD len = GetTempPathW(MAX_PATH, temp_buf);
    std::wstring temp_dir = (len > 0 && len < MAX_PATH) ? std::wstring(temp_buf) : L"C:\\Temp\\";
    if (!temp_dir.empty() && temp_dir.back() == L'\\') {
        temp_dir.pop_back();
    }

    paths.base_dir = temp_dir + L"\\SeerOCR";
    CreateDirectoryW(paths.base_dir.c_str(), nullptr);

    paths.base_name = outputBaseName(input_file);
    const std::wstring prefix = paths.base_dir + L"\\" + paths.base_name + L".ocr";

    paths.txt_path      = prefix + L".txt";
    paths.json_path     = prefix + L".json";
    paths.log_path      = prefix + L".log";
    paths.txt_tmp_path  = prefix + L".txt.tmp";
    paths.json_tmp_path = prefix + L".json.tmp";

    return paths;
}

std::wstring getModuleDirectory()
{
    wchar_t path[MAX_PATH] = {0};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring s(path);
    size_t pos = s.find_last_of(L"\\/");
    if (pos != std::wstring::npos) {
        return s.substr(0, pos);
    }
    return L".";
}

bool atomicWriteFile(const std::wstring &target_path, const std::string &data)
{
    const std::wstring tmp_path = target_path + L".tmp";

    HANDLE file = CreateFileW(
        tmp_path.c_str(),
        GENERIC_WRITE,
        0,
        nullptr,
        CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);

    if (file == INVALID_HANDLE_VALUE) {
        return false;
    }

    DWORD written = 0;
    BOOL ok = WriteFile(file, data.data(), static_cast<DWORD>(data.size()), &written, nullptr);
    CloseHandle(file);

    if (!ok || written != data.size()) {
        DeleteFileW(tmp_path.c_str());
        return false;
    }

    if (!MoveFileExW(tmp_path.c_str(), target_path.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        // Fallback: try copy + delete
        if (!CopyFileW(tmp_path.c_str(), target_path.c_str(), FALSE)) {
            DeleteFileW(tmp_path.c_str());
            return false;
        }
        DeleteFileW(tmp_path.c_str());
    }

    return true;
}

}  // namespace seer_ocr
