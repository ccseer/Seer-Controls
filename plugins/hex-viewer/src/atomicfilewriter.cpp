#include "atomicfilewriter.h"

#include "winpath.h"
#include "wintext.h"

namespace hexviewer {
namespace {

constexpr int kRenameAttempts = 3;
constexpr DWORD kRenameBackoffMs = 50;

// A read-only target blocks both the replace and a direct write; the bit is
// cleared best-effort before either.
void clearReadOnly(const std::wstring &path)
{
    const DWORD attributes
        = GetFileAttributesW(WinPath::addExtendedPrefix(path).c_str());
    if (attributes != INVALID_FILE_ATTRIBUTES
        && (attributes & FILE_ATTRIBUTE_READONLY) != 0) {
        SetFileAttributesW(WinPath::addExtendedPrefix(path).c_str(),
                           attributes & ~FILE_ATTRIBUTE_READONLY);
    }
}

}  // namespace

AtomicFileWriter::~AtomicFileWriter()
{
    abandon();
}

bool AtomicFileWriter::begin(const std::wstring &tmpPath, std::wstring *error)
{
    m_tmpPath = tmpPath;
    m_handle = CreateFileW(WinPath::addExtendedPrefix(tmpPath).c_str(),
                           GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (m_handle == INVALID_HANDLE_VALUE) {
        if (error) {
            *error = L"cannot create temporary file '" + tmpPath
                     + L"': " + WinText::systemErrorMessage(GetLastError());
        }
        // Nothing was created, so no later scope exit may delete this path: the
        // file that made CREATE_NEW fail belongs to somebody else.
        m_tmpPath.clear();
        return false;
    }
    return true;
}

bool AtomicFileWriter::write(const void *data, size_t size, std::wstring *error)
{
    if (m_handle == INVALID_HANDLE_VALUE) {
        if (error) {
            *error = L"the temporary file is not open";
        }
        return false;
    }
    if (size == 0) {
        return true;
    }
    DWORD written = 0;
    if (!WriteFile(m_handle, data, static_cast<DWORD>(size), &written, nullptr)
        || written != size) {
        if (error) {
            *error = L"cannot write temporary file '" + m_tmpPath
                     + L"': " + WinText::systemErrorMessage(GetLastError());
        }
        return false;
    }
    return true;
}

bool AtomicFileWriter::finish(const std::wstring &txtPath, std::wstring *error)
{
    if (m_handle == INVALID_HANDLE_VALUE) {
        if (error) {
            *error = L"the temporary file is not open";
        }
        return false;
    }
    if (!FlushFileBuffers(m_handle)) {
        const DWORD flushError = GetLastError();
        if (error) {
            *error = L"cannot flush temporary file '" + m_tmpPath
                     + L"': " + WinText::systemErrorMessage(flushError);
        }
        abandon();
        return false;
    }
    CloseHandle(m_handle);
    m_handle = INVALID_HANDLE_VALUE;

    clearReadOnly(txtPath);
    DWORD renameError = 0;
    for (int attempt = 0; attempt < kRenameAttempts; ++attempt) {
        if (MoveFileExW(WinPath::addExtendedPrefix(m_tmpPath).c_str(),
                        WinPath::addExtendedPrefix(txtPath).c_str(),
                        MOVEFILE_REPLACE_EXISTING)) {
            // The staging path no longer exists; clearing it keeps the
            // destructor from deleting a name that was already published.
            m_tmpPath.clear();
            return true;
        }
        renameError = GetLastError();
        if (attempt + 1 < kRenameAttempts) {
            Sleep(kRenameBackoffMs);
        }
    }
    if (error) {
        *error = L"MoveFileExW failed: "
                 + WinText::systemErrorMessage(renameError);
    }
    abandon();
    return false;
}

void AtomicFileWriter::abandon()
{
    if (m_handle != INVALID_HANDLE_VALUE) {
        CloseHandle(m_handle);
        m_handle = INVALID_HANDLE_VALUE;
    }
    if (!m_tmpPath.empty()) {
        DeleteFileW(WinPath::addExtendedPrefix(m_tmpPath).c_str());
        m_tmpPath.clear();
    }
}

AtomicWriteResult AtomicFileWriter::commit(const std::wstring &tmpPath,
                                           const std::wstring &txtPath,
                                           const std::string &content)
{
    AtomicWriteResult result;
    AtomicFileWriter writer;
    std::wstring error;
    if (!writer.begin(tmpPath, &error)) {
        result.errorMessage = error;
        return result;
    }
    if (!writer.write(content.data(), content.size(), &error)) {
        writer.abandon();
        result.errorMessage = error;
        return result;
    }
    if (!writer.finish(txtPath, &error)) {
        result.errorMessage = error;
        return result;
    }
    result.ok = true;
    return result;
}

AtomicWriteResult AtomicFileWriter::writeDirect(const std::wstring &path,
                                                const std::string &content)
{
    AtomicWriteResult result;
    clearReadOnly(path);
    const HANDLE handle = CreateFileW(
        WinPath::addExtendedPrefix(path).c_str(), GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
        CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        result.errorMessage = L"cannot open '" + path
                              + L"' for a direct write: "
                              + WinText::systemErrorMessage(GetLastError());
        return result;
    }
    DWORD written = 0;
    if (!WriteFile(handle, content.data(),
                   static_cast<DWORD>(content.size()), &written, nullptr)
        || written != content.size()) {
        result.errorMessage = L"cannot write '" + path + L"': "
                              + WinText::systemErrorMessage(GetLastError());
    }
    else {
        result.ok = true;
    }
    CloseHandle(handle);
    return result;
}

}  // namespace hexviewer
