#pragma once

#include <windows.h>
#include <objbase.h>

#include <algorithm>
#include <string>
#include <vector>

#include "wintext.h"

// Path helpers shared by the native Control helpers.
//
// Two distinct notions of "the path" are kept apart on purpose:
//   * lexicalPath(): GetFullPathNameW only. It normalizes separators and
//     relative segments without touching the file system, so it never follows a
//     reparse point.
//   * finalPath(): GetFinalPathNameByHandleW on an opened handle. It resolves
//     symlinks and junctions, so it is the value used for containment checks.
// Callers that must not be confused by a link need both.
namespace WinPath {

inline constexpr wchar_t kSeparator = L'\\';

inline std::wstring nativeSeparators(const std::wstring &path)
{
    std::wstring result(path);
    for (auto &character : result) {
        if (character == L'/') {
            character = kSeparator;
        }
    }
    return result;
}

inline bool isUncPath(const std::wstring &path)
{
    return path.size() >= 2 && path[0] == kSeparator && path[1] == kSeparator;
}

inline bool isExtendedPath(const std::wstring &path)
{
    return path.size() >= 4 && path.compare(0, 4, L"\\\\?\\") == 0;
}

// A drive root ("C:\") or a UNC share root ("\\server\share\") has no parent
// that can hold a sibling file, which is why it needs its own predicate.
inline bool isRootPath(const std::wstring &rawPath)
{
    std::wstring trimmed = nativeSeparators(rawPath);
    while (trimmed.size() > 1 && trimmed.back() == kSeparator) {
        trimmed.pop_back();
    }
    if (trimmed.size() == 2 && trimmed[1] == L':') {
        return true;
    }
    if (trimmed.size() == 3 && trimmed[1] == L':' && trimmed[2] == kSeparator) {
        return true;
    }
    if (isUncPath(trimmed)) {
        size_t index    = 2;
        int separators  = 0;
        while (index < trimmed.size()) {
            if (trimmed[index] == kSeparator) {
                ++separators;
                if (separators == 2) {
                    return index == trimmed.size() - 1;
                }
            }
            ++index;
        }
        return separators < 2;
    }
    return false;
}

inline std::wstring stripTrailingSeparators(const std::wstring &path)
{
    if (isRootPath(path)) {
        return path;
    }
    std::wstring result(path);
    while (result.size() > 3 && result.back() == kSeparator) {
        result.pop_back();
    }
    if (result.size() == 3 && result[1] == L':' && result[2] == kSeparator) {
        return result;
    }
    if (result.size() == 2 && result[1] == L':') {
        return result;
    }
    return result;
}

inline bool lexicalPath(const std::wstring &path, std::wstring *result)
{
    const std::wstring native = nativeSeparators(path);
    const DWORD required = GetFullPathNameW(native.c_str(), 0, nullptr, nullptr);
    if (required == 0) {
        return false;
    }
    std::vector<wchar_t> buffer(static_cast<size_t>(required) + 2, L'\0');
    const DWORD written
        = GetFullPathNameW(native.c_str(), static_cast<DWORD>(buffer.size()),
                           buffer.data(), nullptr);
    if (written == 0 || written >= buffer.size()) {
        return false;
    }
    *result = stripTrailingSeparators(std::wstring(buffer.data(), written));
    return true;
}

inline std::wstring addExtendedPrefix(const std::wstring &path)
{
    if (path.empty() || isExtendedPath(path)) {
        return path;
    }
    if (isUncPath(path)) {
        return L"\\\\?\\UNC\\" + path.substr(2);
    }
    if (path.size() >= 2 && path[1] == L':') {
        return L"\\\\?\\" + path;
    }
    return path;
}

inline std::wstring removeExtendedPrefix(const std::wstring &path)
{
    if (path.size() >= 8 && path.compare(0, 8, L"\\\\?\\UNC\\") == 0) {
        return L"\\\\" + path.substr(8);
    }
    if (isExtendedPath(path)) {
        return path.substr(4);
    }
    return path;
}

// Resolves symlinks, junctions and 8.3 names for an existing path. Fails when
// the path does not exist, which is the expected outcome while a caller is
// validating a not-yet-created destination.
inline bool finalPath(const std::wstring &path, std::wstring *result)
{
    const std::wstring open = addExtendedPrefix(nativeSeparators(path));
    HANDLE handle = CreateFileW(open.c_str(), 0,
                                FILE_SHARE_READ | FILE_SHARE_WRITE
                                    | FILE_SHARE_DELETE,
                                nullptr, OPEN_EXISTING,
                                FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        return false;
    }
    const DWORD required = GetFinalPathNameByHandleW(
        handle, nullptr, 0, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    if (required == 0) {
        CloseHandle(handle);
        return false;
    }
    std::vector<wchar_t> buffer(static_cast<size_t>(required) + 2, L'\0');
    const DWORD written = GetFinalPathNameByHandleW(
        handle, buffer.data(), static_cast<DWORD>(buffer.size()),
        FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    CloseHandle(handle);
    if (written == 0 || written >= buffer.size()) {
        return false;
    }
    *result = stripTrailingSeparators(
        removeExtendedPrefix(std::wstring(buffer.data(), written)));
    return true;
}

// Resolves the deepest existing ancestor and re-appends the missing components.
// A destination directory usually does not exist yet, so its own path cannot be
// opened; its ancestors still have to be resolved before a containment test is
// meaningful.
inline bool finalPathOfDeepestExistingAncestor(const std::wstring &path,
                                               std::wstring *result,
                                               bool *fullyResolved)
{
    std::wstring candidate;
    if (!lexicalPath(path, &candidate)) {
        return false;
    }
    std::vector<std::wstring> missing;
    for (;;) {
        std::wstring resolved;
        if (finalPath(candidate, &resolved)) {
            for (auto it = missing.rbegin(); it != missing.rend(); ++it) {
                resolved += kSeparator;
                resolved += *it;
            }
            *result = stripTrailingSeparators(resolved);
            if (fullyResolved) {
                *fullyResolved = missing.empty();
            }
            return true;
        }
        if (isRootPath(candidate)) {
            return false;
        }
        const size_t position = candidate.find_last_of(kSeparator);
        if (position == std::wstring::npos) {
            return false;
        }
        missing.push_back(candidate.substr(position + 1));
        candidate = candidate.substr(0, position);
        if (candidate.size() == 2 && candidate[1] == L':') {
            candidate += kSeparator;
        }
        if (candidate.empty()) {
            return false;
        }
    }
}

inline bool isWithin(const std::wstring &root, const std::wstring &candidate)
{
    std::wstring normalizedRoot;
    std::wstring normalizedCandidate;
    if (!lexicalPath(root, &normalizedRoot)
        || !lexicalPath(candidate, &normalizedCandidate)) {
        return false;
    }
    if (WinText::equalsIgnoreCase(normalizedRoot, normalizedCandidate)) {
        return true;
    }
    if (normalizedRoot.empty()) {
        return false;
    }
    std::wstring prefix = normalizedRoot;
    if (prefix.back() != kSeparator) {
        prefix += kSeparator;
    }
    return WinText::startsWithIgnoreCase(normalizedCandidate, prefix);
}

inline std::wstring parentDirectory(const std::wstring &path)
{
    std::wstring trimmed = stripTrailingSeparators(nativeSeparators(path));
    if (isRootPath(trimmed)) {
        return trimmed;
    }
    const size_t position = trimmed.find_last_of(kSeparator);
    if (position == std::wstring::npos) {
        return std::wstring();
    }
    std::wstring parent = trimmed.substr(0, position);
    if (parent.size() == 2 && parent[1] == L':') {
        parent += kSeparator;
    }
    return parent;
}

inline std::wstring fileName(const std::wstring &path)
{
    const std::wstring trimmed = stripTrailingSeparators(nativeSeparators(path));
    const size_t position     = trimmed.find_last_of(kSeparator);
    if (position == std::wstring::npos) {
        return trimmed;
    }
    return trimmed.substr(position + 1);
}

inline std::wstring join(const std::wstring &directory, const std::wstring &name)
{
    if (directory.empty()) {
        return name;
    }
    std::wstring result = stripTrailingSeparators(directory);
    if (!result.empty() && result.back() != kSeparator) {
        result += kSeparator;
    }
    return result + name;
}

inline DWORD attributesOf(const std::wstring &path)
{
    return GetFileAttributesW(addExtendedPrefix(nativeSeparators(path)).c_str());
}

inline bool exists(const std::wstring &path)
{
    return attributesOf(path) != INVALID_FILE_ATTRIBUTES;
}

inline bool isDirectory(const std::wstring &path)
{
    const DWORD attributes = attributesOf(path);
    return attributes != INVALID_FILE_ATTRIBUTES
           && (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

inline bool isRegularFile(const std::wstring &path)
{
    const DWORD attributes = attributesOf(path);
    return attributes != INVALID_FILE_ATTRIBUTES
           && (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

inline bool isReparsePoint(const std::wstring &path)
{
    const DWORD attributes = attributesOf(path);
    return attributes != INVALID_FILE_ATTRIBUTES
           && (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
}


struct DirectoryEntry {
    std::wstring name;
    std::wstring fullPath;
    DWORD attributes = 0;
    unsigned long long size = 0;
    bool isDirectory = false;
    bool isReparsePoint = false;
};

// Creates every missing component of a directory path, which CreateDirectoryW
// does not do on its own.
inline bool makeDirectoryTree(const std::wstring &path)
{
    const std::wstring normalized = stripTrailingSeparators(
        nativeSeparators(path));
    size_t start = 0;
    if (isUncPath(normalized)) {
        size_t separators = 0;
        size_t position   = 2;
        while (position < normalized.size() && separators < 2) {
            if (normalized[position] == kSeparator) {
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
        const auto position = normalized.find(kSeparator, start);
        const auto end = position == std::wstring::npos ? normalized.size()
                                                        : position;
        const std::wstring partial = normalized.substr(0, end);
        if (!partial.empty()) {
            CreateDirectoryW(addExtendedPrefix(partial).c_str(), nullptr);
        }
        if (position == std::wstring::npos) {
            break;
        }
        start = position + 1;
    }
    return isDirectory(normalized);
}


// A directory counts as writable only if it (and any missing parents) can be
// created and a probe file can be created and deleted inside it. The probe is
// what actually detects a read-only location such as "Program Files": a
// directory can exist and still refuse writes. No probe file is left behind.
inline bool isWritableDirectory(const std::wstring &directory,
                                std::wstring *reason = nullptr)
{
    if (!isDirectory(directory) && !makeDirectoryTree(directory)) {
        if (reason && reason->empty()) {
            *reason = L"'" + directory + L"' cannot be created";
        }
        return false;
    }
    const std::wstring probe = join(directory, L".seer-write-probe");
    HANDLE handle = CreateFileW(addExtendedPrefix(probe).c_str(),
                                GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        if (reason && reason->empty()) {
            *reason = L"'" + directory + L"' rejects writes (error "
                      + std::to_wstring(GetLastError()) + L")";
        }
        return false;
    }
    CloseHandle(handle);
    DeleteFileW(addExtendedPrefix(probe).c_str());
    return true;
}

inline std::wstring systemTempDirectory()
{
    std::vector<wchar_t> buffer(32768, L'\0');
    const DWORD written = GetTempPathW(static_cast<DWORD>(buffer.size()),
                                       buffer.data());
    if (written == 0 || written >= static_cast<DWORD>(buffer.size())) {
        return std::wstring();
    }
    return stripTrailingSeparators(std::wstring(buffer.data(), written));
}

// Where a Control helper keeps its working and retained data. The policy is:
//   1. Prefer the plugin's own executable directory, so a portable install
//      keeps everything local to the package.
//   2. Fall back to <system temp>\SeerControlPlugins\<packageName> only when
//      that directory cannot be created or written, which is the usual case
//      for a "Program Files" install. An empty path means neither location is
//      usable and the caller must fail with a clear message.
struct OwnedRoot {
    std::wstring path;
    bool usedFallback = false;
    std::wstring note;  // why the preferred location was rejected
};

inline OwnedRoot resolveOwnedRoot(const std::wstring &executableDirectory,
                                  const std::wstring &packageName)
{
    OwnedRoot result;
    std::wstring reason;
    if (!executableDirectory.empty()
        && isWritableDirectory(executableDirectory, &reason)) {
        result.path
            = stripTrailingSeparators(nativeSeparators(executableDirectory));
        return result;
    }
    result.usedFallback = true;
    result.note         = reason;
    const std::wstring temp = systemTempDirectory();
    if (temp.empty()) {
        if (!result.note.empty()) {
            result.note += L"; the system temporary directory is unavailable";
        }
        return result;
    }
    const std::wstring fallback
        = join(join(temp, L"SeerControlPlugins"), packageName);
    std::wstring fallbackReason;
    if (isWritableDirectory(fallback, &fallbackReason)) {
        result.path = fallback;
        return result;
    }
    if (result.note.empty()) {
        result.note = fallbackReason;
    }
    else if (!fallbackReason.empty()) {
        result.note += L"; " + fallbackReason;
    }
    return result;
}


inline bool enumerateDirectory(const std::wstring &directory,
                               std::vector<DirectoryEntry> *entries,
                               std::wstring *error)
{
    WIN32_FIND_DATAW data{};
    const std::wstring pattern
        = addExtendedPrefix(stripTrailingSeparators(nativeSeparators(directory))
                            + kSeparator + L"*");
    HANDLE handle = FindFirstFileW(pattern.c_str(), &data);
    if (handle == INVALID_HANDLE_VALUE) {
        const DWORD failure = GetLastError();
        if (failure == ERROR_FILE_NOT_FOUND) {
            return true;
        }
        if (error) {
            *error = L"cannot enumerate '" + directory + L"': "
                     + WinText::systemErrorMessage(failure);
        }
        return false;
    }
    do {
        const std::wstring name(data.cFileName);
        if (name == L"." || name == L"..") {
            continue;
        }
        DirectoryEntry entry;
        entry.name = name;
        entry.fullPath
            = stripTrailingSeparators(nativeSeparators(directory)) + kSeparator
              + name;
        entry.attributes    = data.dwFileAttributes;
        entry.isDirectory   = (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
                              != 0;
        entry.isReparsePoint
            = (data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
        entry.size = (static_cast<unsigned long long>(data.nFileSizeHigh) << 32)
                     | data.nFileSizeLow;
        entries->push_back(entry);
    } while (FindNextFileW(handle, &data));
    const DWORD failure = GetLastError();
    FindClose(handle);
    if (failure != ERROR_NO_MORE_FILES) {
        if (error) {
            *error = L"enumeration of '" + directory
                     + L"' ended early: " + WinText::systemErrorMessage(failure);
        }
        return false;
    }
    return true;
}

// Removes a tree without following reparse points: a link is removed as a link,
// never as its target. Refuses to leave the caller-owned root.
inline bool removeTreeOwned(const std::wstring &root,
                            const std::wstring &target,
                            std::wstring *error)
{
    if (!isWithin(root, target)) {
        if (error) {
            *error = L"refusing to remove '" + target
                     + L"' outside owned root '" + root + L"'";
        }
        return false;
    }
    const DWORD attributes = attributesOf(target);
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        return true;
    }
    if ((attributes & FILE_ATTRIBUTE_DIRECTORY) == 0) {
        if ((attributes & FILE_ATTRIBUTE_READONLY) != 0) {
            SetFileAttributesW(addExtendedPrefix(target).c_str(),
                               attributes & ~FILE_ATTRIBUTE_READONLY);
        }
        if (!DeleteFileW(addExtendedPrefix(target).c_str())) {
            if (error) {
                *error = L"cannot delete '" + target + L"': "
                         + std::to_wstring(GetLastError());
            }
            return false;
        }
        return true;
    }
    if ((attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
        if (!RemoveDirectoryW(addExtendedPrefix(target).c_str())) {
            if (error) {
                *error = L"cannot remove link '" + target + L"': "
                         + std::to_wstring(GetLastError());
            }
            return false;
        }
        return true;
    }

    std::vector<DirectoryEntry> entries;
    if (!enumerateDirectory(target, &entries, error)) {
        return false;
    }
    for (const auto &entry : entries) {
        if (!removeTreeOwned(root, entry.fullPath, error)) {
            return false;
        }
    }
    if (!RemoveDirectoryW(addExtendedPrefix(target).c_str())) {
        if (error) {
            *error = L"cannot remove directory '" + target + L"': "
                     + std::to_wstring(GetLastError());
        }
        return false;
    }
    return true;
}

// An exclusively created directory that only its creating invocation may
// delete. Ownership is proven by a marker file that records the creating owner,
// the purpose, a per-invocation token, the directory it was created in, and the
// creating process identity, so a leftover directory from a crashed run can be
// reclaimed safely while a scratch directory still in use by a running
// invocation cannot.
class OwnedScratch {
public:
    static constexpr const wchar_t *kMarkerName = L".seer-control-owner";
    static constexpr const wchar_t *kMarkerMagic = L"seer-control-scratch-v1";
    bool create(const std::wstring &parentDirectory,
                const std::wstring &ownerTag,
                const std::wstring &purposeTag,
                std::wstring *error,
                const std::wstring &namePrefix = std::wstring())
    {
        m_ownerTag   = ownerTag;
        m_purposeTag = purposeTag;
        if (!isDirectory(parentDirectory)) {
            if (error) {
                *error = L"parent directory does not exist: " + parentDirectory;
            }
            return false;
        }
        GUID guid{};
        if (FAILED(CoCreateGuid(&guid))) {
            if (error) {
                *error = L"cannot create a scratch identity";
            }
            return false;
        }
        wchar_t text[40]{};
        if (!StringFromGUID2(guid, text, 40)) {
            if (error) {
                *error = L"cannot create a scratch identity";
            }
            return false;
        }
        m_token = text;
        m_token.erase(std::remove(m_token.begin(), m_token.end(), L'{'),
                      m_token.end());
        m_token.erase(std::remove(m_token.begin(), m_token.end(), L'}'),
                      m_token.end());
        std::replace(m_token.begin(), m_token.end(), L'-', L'_');
        m_namePrefix = namePrefix;

        const std::wstring directory = join(
            stripTrailingSeparators(parentDirectory), m_namePrefix + m_token);
        if (!CreateDirectoryW(addExtendedPrefix(directory).c_str(), nullptr)) {
            if (error) {
                *error = L"cannot create scratch directory '" + directory
                         + L"': " + std::to_wstring(GetLastError());
            }
            return false;
        }
        m_directory = directory;
        if (!writeMarker(error)) {
            RemoveDirectoryW(addExtendedPrefix(m_directory).c_str());
            m_directory.clear();
            return false;
        }
        m_valid = true;
        return true;
    }

    bool adopt(const std::wstring &directory,
               const std::wstring &ownerTag,
               const std::wstring &purposeTag,
               std::wstring *error)
    {
        if (!owns(directory, ownerTag, purposeTag)) {
            if (error) {
                *error = L"directory is not owned by this operation: "
                         + directory;
            }
            return false;
        }
        m_directory  = stripTrailingSeparators(nativeSeparators(directory));
        m_ownerTag   = ownerTag;
        m_purposeTag = purposeTag;
        m_token      = readField(markerPath(), L"token=");
        m_valid      = true;
        return true;
    }

    bool valid() const { return m_valid; }

    const std::wstring &directory() const { return m_directory; }

    const std::wstring &namePrefix() const { return m_namePrefix; }

    std::wstring markerPath() const
    {
        return m_directory.empty() ? std::wstring()
                                   : join(m_directory, kMarkerName);
    }

    static bool owns(const std::wstring &directory,
                     const std::wstring &ownerTag,
                     const std::wstring &purposeTag)
    {
        const std::wstring marker = join(stripTrailingSeparators(directory),
                                         kMarkerName);
        if (readField(marker, L"magic=") != kMarkerMagic) {
            return false;
        }
        if (readField(marker, L"owner=") != ownerTag) {
            return false;
        }
        if (readField(marker, L"purpose=") != purposeTag) {
            return false;
        }
        const std::wstring recorded = readField(marker, L"directory=");
        if (recorded.empty()) {
            return false;
        }
        return WinText::compareIgnoreCase(stripTrailingSeparators(recorded),
                                          stripTrailingSeparators(directory))
               == 0;
    }

    // A recorded process identity that no longer matches means the creating
    // invocation is gone, including when the identifier was recycled.
    static bool ownerIsAlive(const std::wstring &directory)
    {
        const std::wstring marker = join(stripTrailingSeparators(directory),
                                         kMarkerName);
        const std::wstring pidText = readField(marker, L"pid=");
        const std::wstring startText = readField(marker, L"pidstart=");
        if (pidText.empty() || startText.empty()) {
            return false;
        }
        const unsigned long pid = wcstoul(pidText.c_str(), nullptr, 10);
        if (pid == 0) {
            return false;
        }
        const unsigned long long recorded
            = wcstoull(startText.c_str(), nullptr, 10);
        HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE,
                                     static_cast<DWORD>(pid));
        if (!process) {
            return false;
        }
        FILETIME creation{};
        FILETIME exit{};
        FILETIME kernel{};
        FILETIME user{};
        const bool queried
            = GetProcessTimes(process, &creation, &exit, &kernel, &user) != FALSE;
        CloseHandle(process);
        if (!queried) {
            return false;
        }
        const unsigned long long observed
            = (static_cast<unsigned long long>(creation.dwHighDateTime) << 32)
              | creation.dwLowDateTime;
        return observed == recorded;
    }

    static bool isAbandoned(const std::wstring &directory,
                            const std::wstring &ownerTag,
                            const std::wstring &purposeTag)
    {
        return owns(directory, ownerTag, purposeTag)
               && !ownerIsAlive(directory);
    }

    // Removes only directories whose marker proves this owner and purpose and
    // whose creating process is gone. `namePrefix` limits the scan to this
    // helper's own scratch name shape, so no unrelated directory is considered.
    // Returns the number removed.
    static int recoverAbandoned(const std::wstring &parentDirectory,
                                const std::wstring &ownerTag,
                                const std::wstring &purposeTag,
                                const std::wstring &namePrefix,
                                std::wstring *error)
    {
        if (namePrefix.empty()) {
            return 0;
        }
        std::vector<DirectoryEntry> entries;
        if (!enumerateDirectory(parentDirectory, &entries, error)) {
            return 0;
        }
        int removed = 0;
        for (const auto &entry : entries) {
            if (!entry.isDirectory || entry.isReparsePoint) {
                continue;
            }
            if (!WinText::startsWithIgnoreCase(entry.name, namePrefix)) {
                continue;
            }
            if (!isAbandoned(entry.fullPath, ownerTag, purposeTag)) {
                continue;
            }
            std::wstring failure;
            if (!removeTreeOwned(parentDirectory, entry.fullPath, &failure)) {
                if (error && error->empty()) {
                    *error = failure;
                }
                continue;
            }
            ++removed;
        }
        return removed;
    }

    bool cleanup(std::wstring *error = nullptr)
    {
        if (!m_valid || m_directory.empty()) {
            return true;
        }
        if (!owns(m_directory, m_ownerTag, m_purposeTag)) {
            if (error) {
                *error = L"scratch ownership check failed, keeping "
                         + m_directory;
            }
            return false;
        }
        const std::wstring parent = parentDirectory(m_directory);
        std::wstring failure;
        if (!removeTreeOwned(parent, m_directory, &failure)) {
            if (error) {
                *error = failure;
            }
            return false;
        }
        m_valid = false;
        m_directory.clear();
        return true;
    }

    static std::wstring readField(const std::wstring &markerPath,
                                  const std::wstring &key)
    {
        HANDLE handle = CreateFileW(addExtendedPrefix(markerPath).c_str(),
                                    GENERIC_READ,
                                    FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                    OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL,
                                    nullptr);
        if (handle == INVALID_HANDLE_VALUE) {
            return std::wstring();
        }
        LARGE_INTEGER size{};
        if (!GetFileSizeEx(handle, &size) || size.QuadPart <= 0
            || size.QuadPart > 64 * 1024) {
            CloseHandle(handle);
            return std::wstring();
        }
        std::string bytes(static_cast<size_t>(size.QuadPart), '\0');
        DWORD read   = 0;
        const bool ok = ReadFile(handle, bytes.data(),
                                 static_cast<DWORD>(bytes.size()), &read,
                                 nullptr)
                        && read == bytes.size();
        CloseHandle(handle);
        if (!ok) {
            return std::wstring();
        }
        const auto text = WinText::fromUtf8(bytes);
        for (const auto &line : WinText::splitLines(text)) {
            if (WinText::startsWith(line, key)) {
                return WinText::trim(line.substr(key.size()));
            }
        }
        return std::wstring();
    }

private:
    bool writeMarker(std::wstring *error)
    {
        FILETIME creation{};
        FILETIME exit{};
        FILETIME kernel{};
        FILETIME user{};
        unsigned long long start = 0;
        if (GetProcessTimes(GetCurrentProcess(), &creation, &exit, &kernel,
                            &user)) {
            start = (static_cast<unsigned long long>(creation.dwHighDateTime)
                     << 32)
                    | creation.dwLowDateTime;
        }

        std::wstring content = L"magic=";
        content += kMarkerMagic;
        content += L"\nowner=" + m_ownerTag;
        content += L"\npurpose=" + m_purposeTag;
        content += L"\ntoken=" + m_token;
        content += L"\ndirectory=" + m_directory;
        content += L"\npid=" + std::to_wstring(GetCurrentProcessId());
        content += L"\npidstart=" + std::to_wstring(start);
        content += L"\n";

        const std::wstring marker = markerPath();
        HANDLE handle = CreateFileW(addExtendedPrefix(marker).c_str(),
                                    GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                    FILE_ATTRIBUTE_NORMAL, nullptr);
        if (handle == INVALID_HANDLE_VALUE) {
            if (error) {
                *error = L"cannot write the ownership marker: "
                         + std::to_wstring(GetLastError());
            }
            return false;
        }
        const auto bytes = WinText::toUtf8(content);
        DWORD written    = 0;
        const bool ok = WriteFile(handle, bytes.data(),
                                  static_cast<DWORD>(bytes.size()), &written,
                                  nullptr)
                        && written == bytes.size();
        CloseHandle(handle);
        if (!ok && error) {
            *error = L"cannot write the ownership marker";
        }
        return ok;
    }

    std::wstring m_directory;
    std::wstring m_ownerTag;
    std::wstring m_purposeTag;
    std::wstring m_token;
    std::wstring m_namePrefix;
    bool m_valid = false;
};

}  // namespace WinPath
