#include <windows.h>
#include <objbase.h>

#include <string>
#include <vector>

#include "common_tests.h"
#include "filelock.h"
#include "fileprobe.h"
#include "stderrharness.h"
#include "wincmd.h"
#include "winexit.h"
#include "winpath.h"
#include "winproc.h"
#include "wintext.h"

#include "testharness.h"

using TestHarness::check;

namespace {

std::wstring tempRoot()
{
    return FileProbe::tempRoot(L"SeerFileLockTests");
}

bool makeDirectory(const std::wstring &path)
{
    return FileProbe::makeDirectoryTree(path);
}

std::vector<std::wstring> makeArgv(std::initializer_list<const wchar_t *> parts)
{
    std::vector<std::wstring> result;
    for (const auto *part : parts) {
        result.emplace_back(part);
    }
    return result;
}

filelock::LockReport reportWith(const size_t users,
                                const bool truncated = false)
{
    filelock::LockReport report;
    report.ok        = true;
    report.exitCode  = WinExit::kOk;
    report.filePath  = L"C:\\temp\\locked.txt";
    report.truncated = truncated;
    for (size_t index = 0; index < users; ++index) {
        filelock::LockUser user;
        user.applicationType = index == 0 ? L"Application" : L"Service";
        user.name            = L"app" + std::to_wstring(index);
        user.pid             = static_cast<DWORD>(1000 + index);
        user.startTimeKnown  = true;
        user.startTime       = 132000000000000000ULL + index;
        if (index == 1) {
            user.identityChanged = true;
        }
        else {
            user.executableAccessible = true;
            user.executablePath       = L"C:\\apps\\app"
                                        + std::to_wstring(index) + L".exe";
        }
        report.users.push_back(user);
    }
    return report;
}

void testArgumentParsing(const std::wstring &file)
{
    std::wstring error;
    const auto plain = filelock::parseArguments(
        makeArgv({L"file_lock_info.exe", L"--input", file.c_str()}), &error);
    check(plain.has_value(), "a plain request parses: "
                                 + WinText::toUtf8(error));
    check(plain && !plain->admin, "admin defaults to false");

    const auto elevated = filelock::parseArguments(
        makeArgv({L"x.exe", L"--input", file.c_str(), L"--admin"}));
    check(elevated && elevated->admin, "the admin flag is read");

    check(!filelock::parseArguments(makeArgv({L"x.exe"})).has_value(),
          "a missing input is rejected");
    check(!filelock::parseArguments(makeArgv({L"x.exe", L"--input", L""}))
               .has_value(),
          "an empty input is rejected");
    check(!filelock::parseArguments(
              makeArgv({L"x.exe", L"--input", file.c_str(), L"--input",
                        file.c_str()}))
               .has_value(),
          "a duplicated input is rejected");
    check(!filelock::parseArguments(
              makeArgv({L"x.exe", L"--input", file.c_str(), L"--bogus"}))
               .has_value(),
          "an unknown option is rejected");
    check(!filelock::parseArguments(
              makeArgv({L"x.exe", L"--input", file.c_str(), L"extra"}))
               .has_value(),
          "a stray positional argument is rejected");
}

void testReportFormatting()
{
    const auto empty = filelock::formatReportText(reportWith(0));
    check(empty.find(L"No users reported by Restart Manager.")
              != std::wstring::npos,
          "an empty result uses the exact documented wording");
    check(empty.find(L"not a complete list") != std::wstring::npos,
          "the standing limitation note is present");
    check(empty.find(L"--admin argument") != std::wstring::npos,
          "a non-elevated report explains the admin rerun");

    const auto two = filelock::formatReportText(reportWith(2));
    check(two.find(L"2 user(s) reported by Restart Manager") != std::wstring::npos,
          "the number of users is reported");
    check(two.find(L"app0") != std::wstring::npos
              && two.find(L"PID:  1000") != std::wstring::npos,
          "each user reports its name and identifier");
    check(two.find(L"C:\\apps\\app0.exe") != std::wstring::npos,
          "an accessible executable path is shown");
    check(two.find(L"process identifier no longer matches") != std::wstring::npos,
          "a reused identifier withholds the details instead of misreporting");
    check(two.find(L"Started: ") != std::wstring::npos,
          "a known start time is shown");

    const auto truncated = filelock::formatReportText(reportWith(3, true));
    check(truncated.find(L"Only the first") != std::wstring::npos,
          "a truncated list says so");

    auto elevated = reportWith(0);
    elevated.elevated = true;
    check(filelock::formatReportText(elevated).find(L"--admin argument")
              == std::wstring::npos,
          "an elevated report does not offer the admin rerun");

    auto failed = reportWith(0);
    failed.ok       = false;
    failed.exitCode = WinExit::kUnsupported;
    failed.message  = L"the Restart Manager is not available on this system";
    const auto text = filelock::formatReportText(failed);
    check(text.find(L"not available") != std::wstring::npos,
          "a failed query shows its message");
    check(text.find(L"No users reported") == std::wstring::npos,
          "a failed query is not presented as an empty result");
}

// The real backend, against a file that either is or is not held open.
void testRestartManagerBackend(const std::wstring &root,
                               const std::wstring &probeChild)
{
    const std::wstring directory = root + L"\\backend \u6d4b\u8bd5";
    makeDirectory(directory);
    const std::wstring file = directory + L"\\locked file.txt";
    FileProbe::writeTextFile(file, "contents");

    const auto idle = filelock::queryRestartManager(file);
    check(idle.ok, "the Restart Manager query succeeds: "
                       + WinText::toUtf8(idle.message));
    check(idle.users.empty(),
          "a file nobody holds is reported as having no users");
    check(filelock::formatReportText(idle).find(L"No users reported")
              != std::wstring::npos,
          "the idle case uses the documented wording");

    const auto missing = filelock::queryRestartManager(directory + L"\\absent.txt");
    check(!missing.ok && missing.exitCode == WinExit::kNotFound,
          "a missing file is rejected before the session starts");

    if (probeChild.empty()) {
        return;
    }
    // A controlled process holding the file exclusively is what the backend must
    // find; the report is only meaningful if it does.
    FileProbe::writeTextFile(directory + L"\\holder-report.txt", "");
    WinProc::LaunchOptions options;
    options.workingDirectory = directory;
    options.noWindow         = true;
    WinProc::Child holder;
    std::wstring error;
    if (!WinProc::createChild(
            probeChild,
            {directory + L"\\holder-report.txt", L"hold-file", file, L"6000"},
            options, nullptr, &holder, &error)) {
        check(false, "the holding child started: " + WinText::toUtf8(error));
        return;
    }
    std::string holderReport;
    for (int attempt = 0; attempt < 100; ++attempt) {
        holderReport = FileProbe::readTextFile(
            directory + L"\\holder-report.txt");
        if (FileProbe::reportField(holderReport, "held") == L"1") {
            break;
        }
        Sleep(50);
    }
    check(FileProbe::reportField(holderReport, "held") == L"1",
          "the controlled process holds the file exclusively");

    const auto held = filelock::queryRestartManager(file);
    check(held.ok, "the Restart Manager query succeeds while the file is held: "
                       + WinText::toUtf8(held.message));
    std::cout << "  [info] users while held: " << held.users.size() << '\n';
    bool foundHolder = false;
    for (const auto &user : held.users) {
        if (user.pid == holder.pid) {
            foundHolder = true;
        }
    }
    check(foundHolder,
          "the process holding the file is reported by the Restart Manager");
    if (foundHolder) {
        const auto text = filelock::formatReportText(held);
        check(text.find(std::to_wstring(holder.pid)) != std::wstring::npos,
              "the holder's identifier appears in the report");
    }

    WinProc::terminateChild(&holder);
}

// The failed-query contract of the presentation change: a query that failed is
// not a report, so it opens no dialog and its reason goes to the host's
// failure channel instead. The test completing at all is part of the proof: a
// wrongly opened dialog would block this process until ctest kills it. The
// success path ends in the modal system dialog itself and is covered by the
// manual acceptance list in the README, because an in-process call would block
// on it by design.
void testFailedQueryIsNeverPresented(const std::wstring &file)
{
    auto failing     = reportWith(0);
    failing.ok       = false;
    failing.exitCode = WinExit::kUnsupported;
    failing.message  = L"the Restart Manager is not available on this system";
    const filelock::LockQuery broken = [&](const std::wstring &) {
        return failing;
    };

    StderrHarness::Capture capture;
    const int code = filelock::run({L"file_lock_info.exe", L"--input", file},
                                   broken);
    const auto written = capture.drain();
    check(code == WinExit::kUnsupported,
          "a failed query reports the query's own exit code");
    check(written.find("the Restart Manager is not available")
              != std::string::npos,
          "a failed query explains itself on the host's failure channel");

    // A backend refusal that carries no specific exit code still fails the
    // action instead of presenting an error text as a result.
    auto generic     = reportWith(0);
    generic.ok       = false;
    generic.exitCode = WinExit::kOk;
    generic.message  = L"the query could not be completed";
    const filelock::LockQuery genericBroken = [&](const std::wstring &) {
        return generic;
    };
    StderrHarness::Capture genericCapture;
    check(filelock::run({L"file_lock_info.exe", L"--input", file},
                        genericBroken)
          == WinExit::kFailure,
          "a failed query without its own exit code fails generically");
    genericCapture.drain();
}

void testUsageWithInjectedQuery(const std::wstring &file)
{
    // The launcher must reject an unusable request through its own validation
    // instead of showing anything.
    const auto request = filelock::parseArguments(
        {L"file_lock_info.exe", L"--input", file, L"--bogus"});
    check(!request.has_value(), "an unusable request never reaches the query");
}

}  // namespace

int main(int argc, char *argv[])
{
    const HRESULT comResult = CoInitializeEx(
        nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);

    std::wstring probeChild;
    if (argc > 2) {
        probeChild = WinText::fromUtf8(argv[2]);
    }

    const std::wstring root = tempRoot();
    makeDirectory(root);
    const std::wstring file = root + L"\\target file.txt";
    FileProbe::writeTextFile(file, "target");

    TestHarness::section("file-lock-info: argument parsing");
    testArgumentParsing(file);
    TestHarness::section("file-lock-info: report text");
    testReportFormatting();
    TestHarness::section("file-lock-info: restart manager backend");
    testRestartManagerBackend(root, probeChild);
    TestHarness::section("file-lock-info: failed query handling");
    testFailedQueryIsNeverPresented(file);
    TestHarness::section("file-lock-info: usage");
    testUsageWithInjectedQuery(file);

    CommonTests::run(probeChild);

    if (SUCCEEDED(comResult)) {
        CoUninitialize();
    }
    return TestHarness::summarize("filelock_test");
}
