// Live Windows Terminal acceptance for the starting-directory contract.
//
// Windows Terminal re-parses its own command line, so CRT quoting alone is not
// enough for a directory containing ';'. This test proves the mechanism instead
// of assuming it: it asks the helper for a real Windows Terminal plan, launches
// the shared probe child inside Windows Terminal with that plan, and reads back
// the working directory the child actually received.
//
// Return code 77 means the check could not run on this machine (Windows Terminal
// is not installed); ctest reports that as skipped rather than passed.

#include <windows.h>
#include <objbase.h>

#include <string>
#include <vector>

#include "fileprobe.h"
#include "terminalhere.h"
#include "winexit.h"
#include "winpath.h"
#include "wintext.h"

#include "testharness.h"

using TestHarness::check;
using terminalhere::ShellChoice;
using terminalhere::ShellKind;

namespace {

constexpr int kSkipExitCode = 77;

std::wstring tempRoot()
{
    return FileProbe::tempRoot(L"SeerTerminalHereLive");
}

bool makeDirectory(const std::wstring &path)
{
    return FileProbe::makeDirectoryTree(path);
}

std::wstring readTextFile(const std::wstring &path)
{
    return FileProbe::readTextFileWide(path);
}

std::wstring fieldOf(const std::wstring &report, const std::wstring &key)
{
    for (const auto &line : WinText::splitLines(report)) {
        const auto prefix = key + L"=";
        if (WinText::startsWith(line, prefix)) {
            return line.substr(prefix.size());
        }
    }
    return std::wstring();
}

// Runs one Windows Terminal starting-directory case.
void runCase(const std::wstring &label,
             const std::wstring &probeChild,
             const std::wstring &reportDirectory,
             const std::wstring &startingDirectory)
{
    TestHarness::section("live windows terminal: " + WinText::toUtf8(label));
    makeDirectory(startingDirectory);

    const std::wstring reportPath
        = reportDirectory + L"\\live-" + label + L".txt";
    DeleteFileW(WinPath::addExtendedPrefix(reportPath).c_str());

    terminalhere::Request request;
    request.directory = startingDirectory;
    request.choice    = ShellChoice::WindowsTerminal;

    const auto planned
        = terminalhere::plan(request, terminalhere::systemShellLocator());
    check(planned.ok, "Windows Terminal plan was produced");
    if (!planned.ok) {
        return;
    }
    check(planned.plan.arguments.size() == 5
              && planned.plan.arguments[0] == L"-w"
              && planned.plan.arguments[2] == L"new-tab"
              && planned.plan.arguments[3] == L"-d",
          "plan uses the documented new-window and starting-directory options");
    if (WinText::equalsIgnoreCase(label, L"semicolon")) {
        check(planned.plan.arguments[4] != startingDirectory
                  && planned.plan.arguments[4].find(L"\\;")
                         != std::wstring::npos,
              "a semicolon directory is escaped before it reaches Windows "
              "Terminal");
    }

    // The probe's own output path must stay out of the test directory: Windows
    // Terminal re-parses the trailing command line too.
    auto live             = planned.plan;
    live.arguments.push_back(probeChild);
    live.arguments.push_back(reportPath);
    live.arguments.push_back(L"echo");

    std::wstring message;
    const int code = terminalhere::execute(live, &message);
    check(code == WinExit::kOk,
          "Windows Terminal accepted the request: " + WinText::toUtf8(message));
    if (code != WinExit::kOk) {
        return;
    }

    std::wstring report;
    for (int attempt = 0; attempt < 120; ++attempt) {
        report = readTextFile(reportPath);
        if (!report.empty()) {
            break;
        }
        Sleep(100);
    }
    check(!report.empty(),
          "the probe child ran inside Windows Terminal and wrote its report");
    if (report.empty()) {
        return;
    }
    check(WinText::compareIgnoreCase(fieldOf(report, L"cwd"), startingDirectory)
              == 0,
          "Windows Terminal started the child in the selected directory");
}

}  // namespace

int main(int argc, char *argv[])
{
    const HRESULT comResult = CoInitializeEx(
        nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);

    std::wstring probeChild;
    if (argc > 1) {
        probeChild = WinText::fromUtf8(argv[1]);
    }
    if (probeChild.empty() || !WinPath::isRegularFile(probeChild)) {
        std::cerr << "FAIL: probe child path was provided by the build\n";
        return 1;
    }

    const auto locator = terminalhere::systemShellLocator();
    const auto terminal = locator(ShellKind::WindowsTerminal);
    if (terminal.empty()) {
        std::cout << "SKIP: Windows Terminal is not installed on this machine, "
                     "so the starting-directory handoff was not exercised\n";
        return kSkipExitCode;
    }
    std::cout << "Windows Terminal: " << WinText::toUtf8(terminal) << '\n';

    const std::wstring root = tempRoot();
    makeDirectory(root);
    const std::wstring reportDirectory = root + L"\\reports";
    makeDirectory(reportDirectory);

    runCase(L"semicolon", probeChild, reportDirectory,
            root + L"\\live ; semicolon \u6d4b\u8bd5");
    runCase(L"punctuation", probeChild, reportDirectory,
            root + L"\\live & amp % pct ! bang ^ caret (1) [2] apos");
    runCase(L"plain", probeChild, reportDirectory, root + L"\\live plain");

    if (SUCCEEDED(comResult)) {
        CoUninitialize();
    }
    return TestHarness::summarize("terminalhere_live_test");
}
