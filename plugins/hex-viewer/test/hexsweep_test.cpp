#include <windows.h>

#include <string>

#include "hexsweep.h"
#include "testharness.h"
#include "winpath.h"

using TestHarness::check;

namespace {

constexpr unsigned long long kFileTimeUnitsPerDay = 864000000000ULL;

bool writeFile(const std::wstring &path, const char *content = "dump")
{
    HANDLE handle = CreateFileW(
        WinPath::addExtendedPrefix(path).c_str(), GENERIC_WRITE, 0, nullptr,
        CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        return false;
    }
    const DWORD bytes = static_cast<DWORD>(::strlen(content));
    DWORD written = 0;
    const bool ok = WriteFile(handle, content, bytes, &written, nullptr)
                    && written == bytes;
    CloseHandle(handle);
    return ok;
}

bool touchDaysAgo(const std::wstring &path, unsigned days)
{
    HANDLE handle = CreateFileW(WinPath::addExtendedPrefix(path).c_str(),
                                FILE_WRITE_ATTRIBUTES, 0, nullptr,
                                OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        return false;
    }
    FILETIME now{};
    GetSystemTimeAsFileTime(&now);
    ULARGE_INTEGER adjusted{};
    adjusted.LowPart  = now.dwLowDateTime;
    adjusted.HighPart = now.dwHighDateTime;
    adjusted.QuadPart
        -= static_cast<unsigned long long>(days) * kFileTimeUnitsPerDay;
    const FILETIME stamp{adjusted.LowPart, adjusted.HighPart};
    const bool ok = SetFileTime(handle, nullptr, nullptr, &stamp) != 0;
    CloseHandle(handle);
    return ok;
}

bool makeDirectory(const std::wstring &path)
{
    return CreateDirectoryW(WinPath::addExtendedPrefix(path).c_str(), nullptr)
           != 0;
}

void testSweepAgeAndPatterns(const std::wstring &dumps)
{
    TestHarness::section("sweep removes expired dumps");

    check(writeFile(WinPath::join(dumps, L"old.hextxt"))
              && touchDaysAgo(WinPath::join(dumps, L"old.hextxt"), 8),
          "the expired .hextxt fixture is in place");
    check(writeFile(WinPath::join(dumps, L"fresh.hextxt")),
          "the fresh .hextxt fixture is in place");
    check(writeFile(WinPath::join(dumps, L"stage.gen.tmp"))
              && touchDaysAgo(WinPath::join(dumps, L"stage.gen.tmp"), 8),
          "the expired orphan .tmp fixture is in place");
    check(writeFile(WinPath::join(dumps, L"unrelated.dat"))
              && touchDaysAgo(WinPath::join(dumps, L"unrelated.dat"), 30),
          "an old file with an unrelated extension stays out of the sweep");

    hexviewer::sweepOldDumps(dumps);

    check(!WinPath::isRegularFile(WinPath::join(dumps, L"old.hextxt")),
          "an expired .hextxt dump is removed");
    check(WinPath::isRegularFile(WinPath::join(dumps, L"fresh.hextxt")),
          "a fresh .hextxt dump survives");
    check(!WinPath::isRegularFile(WinPath::join(dumps, L"stage.gen.tmp")),
          "an expired orphan .tmp file is removed");
    check(WinPath::isRegularFile(WinPath::join(dumps, L"unrelated.dat")),
          "a file with an unrelated extension is never touched");
}

void testSweepClearedReadonlyBit(const std::wstring &dumps)
{
    TestHarness::section("sweep clears the read-only bit before deleting");

    const std::wstring locked = WinPath::join(dumps, L"locked.hextxt");
    check(writeFile(locked) && touchDaysAgo(locked, 8),
          "the expired read-only fixture is in place");
    DWORD attributes = GetFileAttributesW(
        WinPath::addExtendedPrefix(locked).c_str());
    check(attributes != INVALID_FILE_ATTRIBUTES
              && SetFileAttributesW(
                  WinPath::addExtendedPrefix(locked).c_str(),
                  attributes | FILE_ATTRIBUTE_READONLY),
          "the fixture carries the read-only attribute");

    hexviewer::sweepOldDumps(dumps);

    check(!WinPath::isRegularFile(locked),
          "a read-only expired dump is still removed");
}

void testSweepLeavesDirectoriesAndHandlesEdges(const std::wstring &dumps)
{
    TestHarness::section("sweep leaves directories and tolerates edges");

    const std::wstring sub = WinPath::join(dumps, L"nested");
    check(makeDirectory(sub) && writeFile(WinPath::join(sub, L"old.hextxt"))
              && touchDaysAgo(WinPath::join(sub, L"old.hextxt"), 30),
          "a directory holding an expired dump is in place");

    hexviewer::sweepOldDumps(dumps);

    check(WinPath::isRegularFile(WinPath::join(sub, L"old.hextxt")),
          "the sweep is not recursive: deeper dumps stay");

    hexviewer::sweepOldDumps(L"");
    hexviewer::sweepOldDumps(WinPath::join(dumps, L"does-not-exist"));
    check(true, "empty and missing directories are tolerated");
}

}  // namespace

int main()
{
    // A throwaway directory under the temp root; the sweep only ever touches
    // direct children, so one level keeps the case local.
    const std::wstring dumps = WinPath::join(
        WinPath::systemTempDirectory(),
        L"hexsweep_test_" + std::to_wstring(GetTickCount()));
    if (!WinPath::makeDirectoryTree(dumps)) {
        return 2;
    }

    testSweepAgeAndPatterns(dumps);
    testSweepClearedReadonlyBit(dumps);
    testSweepLeavesDirectoriesAndHandlesEdges(dumps);

    std::wstring error;
    WinPath::removeTreeOwned(WinPath::systemTempDirectory(), dumps, &error);
    return TestHarness::summarize("hexsweep_test");
}
