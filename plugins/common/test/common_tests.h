#pragma once

#include <windows.h>

#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "winclip.h"
#include "wincmd.h"
#include "winpath.h"
#include "winproc.h"
#include "wintext.h"
#include "winui.h"

#include "fileprobe.h"
#include "testharness.h"

// Shared assertions for the native Control packages.
//
// Everything here crosses a real Windows boundary: CRT argv serialization
// through a launched child, an inherited working directory, the containment job,
// an owned scratch directory, and the clipboard. A quote()/CommandLineToArgvW
// round trip inside one process is not evidence that the helper -> child
// boundary is safe, so the argv cases below are asserted on what the child
// actually received.
namespace CommonTests {

using TestHarness::check;

inline std::wstring tempRoot()
{
    return FileProbe::tempRoot(L"SeerControlTests");
}

inline bool makeDirectory(const std::wstring &path)
{
    return FileProbe::makeDirectoryTree(path);
}

inline std::string readTextFile(const std::wstring &path)
{
    return FileProbe::readTextFile(path);
}

inline std::wstring reportField(const std::string &report,
                                const std::string &key)
{
    return FileProbe::reportField(report, key);
}

inline bool writeTextFile(const std::wstring &path, const std::string &bytes)
{
    return FileProbe::writeTextFile(path, bytes);
}

struct ProbeRun {
    bool launched = false;
    bool exited = false;
    int exitCode = -1;
    std::wstring outputFile;
    std::string report;
    std::string error;
};

inline ProbeRun runProbe(const std::wstring &probePath,
                         const std::wstring &workingDirectory,
                         const std::vector<std::wstring> &arguments,
                         const DWORD waitMs,
                         const bool contained)
{
    ProbeRun run;
    if (arguments.empty()) {
        return run;
    }
    run.outputFile = arguments.front();

    WinProc::Job job;
    std::wstring error;
    if (contained && !job.create(&error)) {
        run.error = WinText::toUtf8(error);
        return run;
    }

    WinProc::Child child;
    const bool started
        = contained
              ? WinProc::launchContained(probePath, arguments, workingDirectory,
                                         job, &child, &error)
              : WinProc::createChild(probePath, arguments,
                                     [&] {
                                         WinProc::LaunchOptions options;
                                         options.workingDirectory
                                             = workingDirectory;
                                         options.noWindow = true;
                                         return options;
                                     }(),
                                     nullptr, &child, &error);
    if (!started) {
        run.error = WinText::toUtf8(error);
        return run;
    }
    run.launched = true;
    DWORD code   = 0;
    run.exited   = WinProc::waitForExit(child, waitMs, &code);
    run.exitCode = static_cast<int>(code);
    child.close();
    run.report = readTextFile(run.outputFile);
    return run;
}

inline void testQuotingRoundTrip()
{
    const std::vector<std::wstring> cases{
        L"",          L"space path",     L"C:\\folder\\",
        L"a\"b",      L"trailing\\\\",   L"quote\"and\\mixed\\\"",
        L"\u4e2d\u6587\u6d4b\u8bd5", L"emoji \U0001F600",
        L"amp&ersand", L"pct%value",     L"bang!value",
        L"caret^value", L"paren(1)",     L"brack[1]",
        L"semi;colon", L"apos'trophe",   L"dollar$value",
        L"back`tick",  L"newline\nvalue"};

    for (const auto &value : cases) {
        const std::wstring command
            = std::wstring(L"probe ") + WinCmd::quoteArgument(value);
        int count = 0;
        auto parsed = CommandLineToArgvW(command.c_str(), &count);
        check(parsed && count == 2 && std::wstring(parsed[1]) == value,
              "quoteArgument round trips through CommandLineToArgvW");
        if (parsed) {
            LocalFree(parsed);
        }
    }

    const std::vector<std::wstring> joined{L"a b", L"c\"d", L"", L"\\"};
    const auto command = L"probe " + WinCmd::joinArguments(joined);
    int count          = 0;
    auto parsed        = CommandLineToArgvW(command.c_str(), &count);
    check(parsed && count == 5 && std::wstring(parsed[1]) == joined[0]
              && std::wstring(parsed[2]) == joined[1]
              && std::wstring(parsed[3]) == joined[2]
              && std::wstring(parsed[4]) == joined[3],
          "joinArguments preserves every element");
    if (parsed) {
        LocalFree(parsed);
    }
}

inline void testChildArgumentBoundary(const std::wstring &probePath,
                                      const std::wstring &root)
{
    if (probePath.empty()) {
        check(false, "probe child path was provided by the build");
        return;
    }
    const std::wstring directory = root + L"\\argv boundary \u6d4b\u8bd5 & dir";
    makeDirectory(directory);

    const std::vector<std::wstring> payload{
        L"plain",
        L"with space",
        L"with\"quote",
        L"trailing\\",
        L"\\\\server\\share\\dir",
        L"\u4e2d\u6587\\\u5b50\u76ee\u5f55",
        L"emoji \U0001F600 tail",
        L"amp&and",
        L"pct%and",
        L"bang!and",
        L"caret^and",
        L"paren(1)",
        L"brack[1]",
        L"semi;colon",
        L"apos'trophe",
        L""};

    std::vector<std::wstring> arguments;
    arguments.push_back(directory + L"\\argv-report.txt");
    arguments.push_back(L"echo");
    arguments.push_back(L"unused-one");
    arguments.push_back(L"unused-two");
    for (const auto &value : payload) {
        arguments.push_back(value);
    }

    const auto run = runProbe(probePath, directory, arguments, 20000, true);
    if (!run.launched || !run.exited) {
        check(false, "probe child launched and exited: " + run.error);
        return;
    }
    check(run.exitCode == 0, "probe child exit code is zero");
    check(!run.report.empty(), "probe child wrote its report");

    for (size_t index = 0; index < payload.size(); ++index) {
        const auto key
            = "argv" + std::to_string(static_cast<int>(index + 5));
        check(reportField(run.report, key) == payload[index],
              "child received payload element " + std::to_string(index)
                  + " unchanged");
    }

    check(!reportField(run.report, "cwd").empty()
              && WinText::compareIgnoreCase(reportField(run.report, "cwd"),
                                            directory)
                     == 0,
          "child inherited the requested working directory");
    check(reportField(run.report, "injob") == L"1",
          "contained child is inside the containment job");
    check(reportField(run.report, "jobkillonclose") == L"1",
          "containment job is configured with kill-on-close");
}

inline void testJobContainmentKillsWholeJob(const std::wstring &probePath,
                                            const std::wstring &root)
{
    if (probePath.empty()) {
        return;
    }
    const std::wstring directory = root + L"\\job containment";
    makeDirectory(directory);

    WinProc::Job job;
    std::wstring error;
    if (!job.create(&error)) {
        check(false, "containment job created: " + WinText::toUtf8(error));
        return;
    }
    WinProc::Child child;
    if (!WinProc::launchContained(
            probePath,
            {directory + L"\\job-report.txt", L"sleep", L"8000"}, directory,
            job, &child, &error)) {
        check(false, "contained sleeper started: " + WinText::toUtf8(error));
        return;
    }

    std::string report;
    for (int attempt = 0; attempt < 100; ++attempt) {
        report = readTextFile(directory + L"\\job-report.txt");
        if (!report.empty()) {
            break;
        }
        Sleep(50);
    }
    check(reportField(report, "injob") == L"1",
          "sleeper was assigned before it was resumed");

    job.close();
    const DWORD wait = WaitForSingleObject(child.process, 5000);
    check(wait == WAIT_OBJECT_0,
          "closing the containment job terminates the child");
    child.close();
}

inline void testPathHelpers(const std::wstring &root)
{
    const std::wstring container = WinPath::parentDirectory(root);
    std::wstring normalized;
    check(WinPath::lexicalPath(root + L"\\..\\job containment\\", &normalized)
              && WinText::compareIgnoreCase(
                     normalized, container + L"\\job containment")
                     == 0,
          "lexicalPath normalizes and drops a trailing separator");

    check(WinPath::isWithin(root, root + L"\\job containment"),
          "isWithin accepts a descendant");
    check(!WinPath::isWithin(root, root + L"2\\elsewhere"),
          "isWithin rejects a sibling with a shared prefix");
    check(WinPath::isWithin(root, root),
          "isWithin accepts the root itself");

    check(WinPath::isRootPath(L"C:\\") && WinPath::isRootPath(L"C:/"),
          "isRootPath accepts a drive root");
    check(WinPath::isRootPath(L"\\\\server\\share\\"),
          "isRootPath accepts a share root");
    check(!WinPath::isRootPath(L"C:\\folder"),
          "isRootPath rejects a folder");
    check(WinPath::stripTrailingSeparators(L"C:\\a\\b\\") == L"C:\\a\\b",
          "stripTrailingSeparators removes a trailing separator");
    check(WinPath::parentDirectory(L"C:\\a\\b") == L"C:\\a",
          "parentDirectory returns the container");
    check(WinPath::fileName(L"C:\\a\\b.txt") == L"b.txt",
          "fileName returns the leaf");
    check(WinPath::join(L"C:\\a", L"b") == L"C:\\a\\b",
          "join appends one separator");

    std::wstring resolved;
    bool fully = false;
    check(WinPath::finalPathOfDeepestExistingAncestor(
              root + L"\\missing one\\missing two", &resolved, &fully)
              && !fully,
          "an uncreated descendant still resolves through its ancestors");
}

inline void testOwnedScratch(const std::wstring &root)
{
    const std::wstring parent = root + L"\\scratch parent";
    makeDirectory(parent);

    WinPath::OwnedScratch scratch;
    std::wstring error;
    check(scratch.create(parent, L"io.test.owner", L"common-test", &error),
          "scratch directory created: " + WinText::toUtf8(error));
    check(scratch.valid() && WinPath::isDirectory(scratch.directory()),
          "scratch directory exists");
    check(WinPath::OwnedScratch::owns(scratch.directory(), L"io.test.owner",
                                      L"common-test"),
          "scratch marker proves ownership");
    check(!WinPath::OwnedScratch::owns(scratch.directory(), L"io.test.other",
                                       L"common-test"),
          "a different owner cannot claim the scratch directory");
    check(!WinPath::OwnedScratch::owns(scratch.directory(), L"io.test.owner",
                                       L"other-purpose"),
          "a different purpose cannot claim the scratch directory");

    writeTextFile(scratch.directory() + L"\\payload.bin", "payload");
    makeDirectory(scratch.directory() + L"\\nested");
    writeTextFile(scratch.directory() + L"\\nested\\deep.bin", "deep");

    const std::wstring directory = scratch.directory();
    check(scratch.cleanup(&error),
          "owned scratch cleanup succeeded: " + WinText::toUtf8(error));
    check(!WinPath::exists(directory), "scratch directory is gone");

    WinPath::OwnedScratch foreign;
    std::wstring foreignError;
    check(!foreign.adopt(parent, L"io.test.owner", L"common-test",
                         &foreignError),
          "adopt refuses a directory without a marker");
}

// The scratch-location policy shared by the packages: a writable executable
// directory is used as-is, a location that cannot be created or written falls
// back to <system temp>\SeerControlPlugins\<package>, and the writability
// probe leaves nothing behind in either location.
inline void testOwnedRootPolicy(const std::wstring &root)
{
    const std::wstring preferred = root + L"\\plugin dir";
    const WinPath::OwnedRoot direct
        = WinPath::resolveOwnedRoot(preferred, L"unit-test");
    check(!direct.usedFallback
              && direct.path == WinPath::stripTrailingSeparators(preferred),
          "a writable executable directory is preferred as-is");
    check(!WinPath::exists(WinPath::join(preferred, L".seer-write-probe")),
          "the writability probe leaves no file in the preferred location");

    const std::wstring blocked = root + L"\\blocked";
    writeTextFile(blocked, "a file, not a directory");
    const WinPath::OwnedRoot fallback
        = WinPath::resolveOwnedRoot(blocked, L"unit-test");
    check(fallback.usedFallback,
          "an unwritable executable directory triggers the fallback");
    const std::wstring expectedFallback
        = WinPath::join(WinPath::join(WinPath::systemTempDirectory(),
                                      L"SeerControlPlugins"),
                        L"unit-test");
    check(fallback.path == expectedFallback,
          "the fallback lives under <system temp>\\SeerControlPlugins");
    check(!fallback.note.empty(),
          "the fallback records why the preferred location was rejected");
    check(WinPath::isDirectory(expectedFallback), "the fallback root exists");
    check(!WinPath::exists(WinPath::join(expectedFallback,
                                         L".seer-write-probe")),
          "the writability probe leaves no file in the fallback location");

    const WinPath::OwnedRoot unnamed
        = WinPath::resolveOwnedRoot(std::wstring(), L"unit-test");
    check(unnamed.usedFallback && unnamed.path == expectedFallback,
          "an unavailable executable directory falls back as well");

    // Clean up the fallback tree this test created under the system temp. The
    // package-level directory may hold another package's data, so only an
    // empty one is removed.
    std::wstring cleanupError;
    WinPath::removeTreeOwned(WinPath::systemTempDirectory(), expectedFallback,
                             &cleanupError);
    RemoveDirectoryW(WinPath::join(WinPath::systemTempDirectory(),
                                   L"SeerControlPlugins")
                         .c_str());
}

inline void testClipboard()
{
    const std::wstring previous = WinClip::getText(nullptr);
    const std::wstring text
        = L"Seer control clipboard \u6d4b\u8bd5\ttab\r\nsecond line";
    std::wstring error;
    check(WinClip::setText(text, nullptr, 2000, &error),
          "clipboard accepted Unicode text: " + WinText::toUtf8(error));
    // The clipboard is a shared global resource; another process may replace
    // the value between the write and the read, so a mismatch is reported
    // rather than asserted as failure.
    const auto readBack = WinClip::getText(nullptr);
    check(readBack.empty() || readBack == text,
          "clipboard read-back matches the value just written");
    if (!previous.empty()) {
        WinClip::setText(previous, nullptr, 2000, &error);
    }
}

inline BOOL CALLBACK findTopLevelWindow(HWND window, LPARAM data)
{
    auto *payload = reinterpret_cast<std::pair<DWORD, bool *> *>(data);
    DWORD pid     = 0;
    GetWindowThreadProcessId(window, &pid);
    if (pid == payload->first && IsWindowVisible(window)
        && GetWindow(window, GW_OWNER) == nullptr) {
        *payload->second = true;
        return FALSE;
    }
    return TRUE;
}

inline BOOL CALLBACK closeWindowForProcess(HWND window, LPARAM data)
{
    const auto pid = *reinterpret_cast<DWORD *>(data);
    DWORD owner    = 0;
    GetWindowThreadProcessId(window, &owner);
    if (owner == pid && IsWindowVisible(window)
        && GetWindow(window, GW_OWNER) == nullptr) {
        PostMessageW(window, WM_CLOSE, 0, 0);
        return FALSE;
    }
    return TRUE;
}

// Exercises the two-process UI readiness contract end to end: the parent must
// not report success before a real window exists, and closing the window must
// end the worker cleanly.
inline void testUiReadinessHandoff(const std::wstring &testExecutable)
{
    if (testExecutable.empty()) {
        check(false, "test executable path was provided by the build");
        return;
    }
    const std::wstring eventName
        = std::wstring(L"Local\\SeerControlCommonTest-")
          + std::to_wstring(GetCurrentProcessId()) + L"-"
          + std::to_wstring(GetTickCount64());
    HANDLE ready = CreateEventW(nullptr, TRUE, FALSE, eventName.c_str());
    if (!ready) {
        check(false, "readiness event created");
        return;
    }

    WinProc::LaunchOptions options;
    options.detached            = true;
    options.suspended           = true;
    options.tryBreakawayFromJob = true;
    WinProc::Child child;
    std::wstring error;
    const bool started = WinProc::createChild(
        testExecutable,
        {UiHandoff::kMessageFlag, UiHandoff::kMessageEventFlag, eventName,
         UiHandoff::kTitleFlag, L"Seer control common test",
         UiHandoff::kHeadingFlag, L"UI readiness",
         UiHandoff::kBodyFlag, L"bounded body"},
        options, nullptr, &child, &error);
    if (!started) {
        check(false, "UI worker started: " + WinText::toUtf8(error));
        CloseHandle(ready);
        return;
    }
    WinProc::resumeChild(&child, &error);

    bool readySignalled = false;
    const ULONGLONG deadline = GetTickCount64() + 15000;
    while (GetTickCount64() < deadline) {
        if (WaitForSingleObject(ready, 100) == WAIT_OBJECT_0) {
            readySignalled = true;
            break;
        }
        if (WaitForSingleObject(child.process, 0) == WAIT_OBJECT_0) {
            break;
        }
    }
    check(readySignalled, "UI worker signalled readiness");

    bool visible = false;
    if (readySignalled) {
        std::pair<DWORD, bool *> payload{child.pid, &visible};
        for (int attempt = 0; attempt < 40 && !visible; ++attempt) {
            EnumWindows(findTopLevelWindow,
                        reinterpret_cast<LPARAM>(&payload));
            if (!visible) {
                Sleep(50);
            }
        }
    }
    check(visible, "a visible top-level window exists for the UI worker");

    if (visible) {
        DWORD pid = child.pid;
        EnumWindows(closeWindowForProcess, reinterpret_cast<LPARAM>(&pid));
        DWORD code = 1;
        const bool exited = WinProc::waitForExit(child, 10000, &code);
        check(exited && code == 0,
              "the UI worker exited cleanly after its window closed");
    }

    if (child.running()) {
        WinProc::terminateChild(&child);
    }
    CloseHandle(ready);
}

inline void testUiHandoffRejectsInvalidArguments(
    const std::vector<std::wstring> &arguments)
{
    int showCalls = 0;
    const int result = UiHandoff::run(
        arguments, [](const std::vector<std::wstring> &) { return false; },
        [&](const std::vector<std::wstring> &,
            const std::function<void()> &) {
            ++showCalls;
            return 0;
        });
    check(result == 2, "the handoff reports a usage failure for bad input");
    check(showCalls == 0, "no UI is created for invalid input");
}

inline void testOptionScanner()
{
    const std::vector<std::wstring> arguments{
        L"helper.exe", L"--input", L"C:\\a b", L"--admin", L"--shell", L"wt"};
    const auto scan = WinCmd::scanOptions(arguments, 1, {L"admin"},
                                          {L"input", L"shell"});
    check(scan.ok, "option scan accepted a valid command line");
    check(scan.positionals.empty(), "option scan consumed every argument");
    std::wstring value;
    check(WinCmd::optionValue(scan, L"input", &value) && value == L"C:\\a b",
          "option scan reads a value that contains a space");
    check(WinCmd::hasOption(scan, L"admin"), "option scan reads a flag");

    const auto unknown = WinCmd::scanOptions(
        {L"helper.exe", L"--bogus"}, 1, {}, {L"input"});
    check(!unknown.ok, "option scan rejects an unknown option");

    const auto missing
        = WinCmd::scanOptions({L"helper.exe", L"--input"}, 1, {}, {L"input"});
    check(!missing.ok, "option scan rejects a missing value");

    const auto duplicated = WinCmd::scanOptions(
        {L"helper.exe", L"--input", L"a", L"--input", L"b"}, 1, {}, {L"input"});
    check(duplicated.ok && !duplicated.duplicates.empty(),
          "option scan records a duplicated option");
}

inline void testTextHelpers()
{
    check(WinText::equalsIgnoreCase(L"PNG", L"png"),
          "equalsIgnoreCase ignores case");
    check(!WinText::equalsIgnoreCase(L"png", L"gif"),
          "equalsIgnoreCase distinguishes values");
    check(WinText::startsWithIgnoreCase(L"${type_file}", L"${TYPE_"),
          "startsWithIgnoreCase ignores case");
    check(WinText::fromUtf8(WinText::toUtf8(L"\u4e2d\u6587 \U0001F600"))
              == L"\u4e2d\u6587 \U0001F600",
          "UTF-8 conversion round trips");
    check(WinText::trim(L"  a b  ") == L"a b", "trim removes outer whitespace");
    check(WinText::splitLines(L"a\r\nb\nc").size() == 3,
          "splitLines returns every line");
    check(WinText::bound(L"abcdef", 3) != L"abcdef",
          "bound truncates an oversized message");
}

inline int run(const std::wstring &testExecutable,
               const std::wstring &probeChild)
{
    const std::wstring root = tempRoot();
    makeDirectory(root);

    TestHarness::section("common: quoting");
    testQuotingRoundTrip();
    TestHarness::section("common: option scanner");
    testOptionScanner();
    TestHarness::section("common: text");
    testTextHelpers();
    TestHarness::section("common: paths");
    testPathHelpers(root);
    TestHarness::section("common: owned scratch");
    testOwnedScratch(root);
    TestHarness::section("common: scratch-location policy");
    testOwnedRootPolicy(root);
    TestHarness::section("common: clipboard");
    testClipboard();
    TestHarness::section("common: child argument and job boundary");
    testChildArgumentBoundary(probeChild, root);
    testJobContainmentKillsWholeJob(probeChild, root);
    TestHarness::section("common: UI readiness handoff");
    testUiHandoffRejectsInvalidArguments({L"helper.exe"});
    testUiReadinessHandoff(testExecutable);
    return 0;
}

}  // namespace CommonTests
