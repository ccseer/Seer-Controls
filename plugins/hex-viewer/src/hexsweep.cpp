#include "hexsweep.h"

#include <windows.h>

#include <vector>

#include "winpath.h"
#include "wintext.h"

namespace hexviewer {
namespace {

bool endsWith(const std::wstring &value, const wchar_t *suffix)
{
    const std::wstring wide(suffix);
    return value.size() >= wide.size()
           && value.compare(value.size() - wide.size(), wide.size(), wide)
                  == 0;
}

}  // namespace

void sweepOldDumps(const std::wstring &dumpsDirectory)
{
    if (dumpsDirectory.empty()) {
        return;
    }

    FILETIME now{};
    GetSystemTimeAsFileTime(&now);
    ULARGE_INTEGER nowValue{};
    nowValue.LowPart = now.dwLowDateTime;
    nowValue.HighPart = now.dwHighDateTime;
    // Seven days in 100-nanosecond units.
    const unsigned long long kSevenDays = 7ULL * 86400ULL * 10000000ULL;
    if (nowValue.QuadPart <= kSevenDays) {
        return;
    }
    const unsigned long long cutoff = nowValue.QuadPart - kSevenDays;

    std::vector<WinPath::DirectoryEntry> entries;
    if (!WinPath::enumerateDirectory(dumpsDirectory, &entries, nullptr)) {
        return;
    }

    for (const auto &entry : entries) {
        if (entry.isDirectory || entry.isReparsePoint) {
            continue;
        }
        const std::wstring lower = WinText::toLower(entry.name);
        if (!endsWith(lower, L".hextxt") && !endsWith(lower, L".tmp")) {
            continue;
        }
        WIN32_FILE_ATTRIBUTE_DATA data{};
        if (!GetFileAttributesExW(
                WinPath::addExtendedPrefix(entry.fullPath).c_str(),
                GetFileExInfoStandard, &data)) {
            continue;
        }
        ULARGE_INTEGER written{};
        written.LowPart = data.ftLastWriteTime.dwLowDateTime;
        written.HighPart = data.ftLastWriteTime.dwHighDateTime;
        if (written.QuadPart >= cutoff) {
            continue;
        }
        if ((data.dwFileAttributes & FILE_ATTRIBUTE_READONLY) != 0) {
            SetFileAttributesW(
                WinPath::addExtendedPrefix(entry.fullPath).c_str(),
                data.dwFileAttributes & ~FILE_ATTRIBUTE_READONLY);
        }
        DeleteFileW(WinPath::addExtendedPrefix(entry.fullPath).c_str());
    }
}

}  // namespace hexviewer
