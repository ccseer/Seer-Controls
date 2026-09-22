#include <windows.h>
#include <objbase.h>

#include <string>
#include <vector>

#include "common_tests.h"
#include "terminalhere.h"
#include "wincmd.h"
#include "winexit.h"
#include "winpath.h"
#include "winproc.h"
#include "wintext.h"

#include "testharness.h"

using terminalhere::ShellChoice;
using terminalhere::ShellKind;
using TestHarness::check;

namespace {

std::wstring tempRoot()
{
    return FileProbe::tempRoot(L"SeerTerminalHereTests");
}

bool makeDirectory(const std::wstring &path)
{
    return FileProbe::makeDirectoryTree(path);
}

std::vector<std::wstring> makeArgv(std::initializer_list<const wchar_t *> parts)
{
    std::vector<std::wstring> result;
    result.reserve(parts.size());
    for (const auto *part : parts) {
        result.emplace_back(part);
    }
    return result;
}

terminalhere::ShellLocator fakeLocator(const std::vector<ShellKind> &installed)
{
    // Captured by value as a vector: an initializer_list would only copy the
    // pointer to a temporary backing array.
    return [installed](const ShellKind kind) -> std::wstring {
        for (const auto candidate : installed) {
            if (candidate == kind) {
                switch (kind) {
                case ShellKind::WindowsTerminal:
                    return L"C:\\fake\\wt.exe";
                case ShellKind::PowerShell7:
                    return L"C:\\fake\\pwsh.exe";
                case ShellKind::WindowsPowerShell:
                    return L"C:\\fake\\powershell.exe";
                case ShellKind::CommandPrompt:
                    return L"C:\\fake\\cmd.exe";
                }
            }
        }
        return std::wstring();
    };
}

void testWindowsTerminalEscaping()
{
    check(terminalhere::escapeForWindowsTerminal(L"C:\\plain dir")
              == L"C:\\plain dir",
          "a directory without a semicolon is unchanged");
    check(terminalhere::escapeForWindowsTerminal(L"C:\\a;b") == L"C:\\a\\;b",
          "a semicolon is backslash-escaped for Windows Terminal");
    check(terminalhere::escapeForWindowsTerminal(L"C:\\a;;b")
              == L"C:\\a\\;\\;b",
          "every semicolon is escaped");
    check(terminalhere::escapeForWindowsTerminal(L"C:\\dir\\;b")
              == L"C:\\dir\\\\;b",
          "an existing backslash before a semicolon is preserved");
    check(terminalhere::escapeForWindowsTerminal(L"C:\\a & b % c ! d ^ e")
              == L"C:\\a & b % c ! d ^ e",
          "characters Windows Terminal accepts verbatim are not altered");
    check(terminalhere::escapeForWindowsTerminal(L"") == L"",
          "escaping an empty value stays empty");
}

void testArgumentParsing(const std::wstring &directory)
{
    std::wstring error;
    const auto valid = terminalhere::parseArguments(
        makeArgv({L"terminal_here.exe", L"--input", directory.c_str()}), &error);
    check(valid.has_value(), "a directory is accepted: "
                                 + WinText::toUtf8(error));
    if (valid) {
        check(valid->choice == ShellChoice::Auto,
              "shell defaults to auto");
        check(!valid->elevated, "admin defaults to false");
    }

    const auto withAdmin = terminalhere::parseArguments(
        makeArgv({L"terminal_here.exe", L"--input", directory.c_str(),
                  L"--shell", L"cmd", L"--admin"}));
    check(withAdmin.has_value(), "an explicit shell with admin parses");

    const auto explicitShell = terminalhere::parseArguments(
        makeArgv({L"terminal_here.exe", L"--input", directory.c_str(),
                  L"--shell", L"PWSH", L"--admin"}));
    check(explicitShell && explicitShell->choice == ShellChoice::PowerShell7,
          "shell names are case-insensitive");
    check(explicitShell && explicitShell->elevated,
          "the admin flag is read");

    const auto explicitCommandPrompt = terminalhere::parseArguments(
        makeArgv({L"terminal_here.exe", L"--input", directory.c_str(),
                  L"--shell", L"cmd"}));
    check(explicitCommandPrompt
              && explicitCommandPrompt->choice == ShellChoice::CommandPrompt,
          "cmd is an accepted shell name");

    check(!terminalhere::parseArguments(
              makeArgv({L"terminal_here.exe", L"--shell", L"cmd"}))
               .has_value(),
          "a missing --input is rejected");
    check(!terminalhere::parseArguments(
              makeArgv({L"terminal_here.exe", L"--input", L""}))
               .has_value(),
          "an empty --input is rejected");
    check(!terminalhere::parseArguments(
              makeArgv({L"terminal_here.exe", L"--input", directory.c_str(),
                    L"--shell", L"bash"}))
               .has_value(),
          "an unknown shell is rejected");
    check(!terminalhere::parseArguments(
              makeArgv({L"terminal_here.exe", L"--input", directory.c_str(),
                    L"--shell", L"pwsh", L"--shell", L"cmd"}))
               .has_value(),
          "a duplicated option is rejected");
    check(!terminalhere::parseArguments(
              makeArgv({L"terminal_here.exe", L"--input", directory.c_str(),
                    L"--bogus"}))
               .has_value(),
          "an unknown option is rejected");
    check(!terminalhere::parseArguments(
              makeArgv({L"terminal_here.exe", L"--input", directory.c_str(),
                    directory.c_str()}))
               .has_value(),
          "a stray positional argument is rejected");
    check(!terminalhere::parseArguments(
              makeArgv({L"terminal_here.exe", L"--input",
                    (directory + L"\\missing").c_str()}))
               .has_value(),
          "a non-existent directory is rejected");
}

terminalhere::Request requestFor(const std::wstring &directory,
                                 const ShellChoice choice,
                                 const bool elevated = false)
{
    terminalhere::Request request;
    request.directory = directory;
    request.choice    = choice;
    request.elevated  = elevated;
    return request;
}

void testPlanSelection(const std::wstring &directory)
{
    const auto all = fakeLocator({ShellKind::WindowsTerminal,
                                  ShellKind::PowerShell7,
                                  ShellKind::WindowsPowerShell,
                                  ShellKind::CommandPrompt});
    const auto onlyPowerShell
        = fakeLocator({ShellKind::WindowsPowerShell, ShellKind::CommandPrompt});
    const auto onlyCommandPrompt = fakeLocator({ShellKind::CommandPrompt});
    const auto nothing           = fakeLocator({});

    auto autoPlan = terminalhere::plan(requestFor(directory, ShellChoice::Auto),
                                       all);
    check(autoPlan.ok && autoPlan.plan.shell == ShellKind::WindowsTerminal,
          "auto prefers a verified Windows Terminal installation");

    autoPlan = terminalhere::plan(requestFor(directory, ShellChoice::Auto),
                                  onlyPowerShell);
    check(autoPlan.ok
              && autoPlan.plan.shell == ShellKind::WindowsPowerShell,
          "auto falls back to Windows PowerShell");

    autoPlan = terminalhere::plan(requestFor(directory, ShellChoice::Auto),
                                  onlyCommandPrompt);
    check(autoPlan.ok && autoPlan.plan.shell == ShellKind::CommandPrompt,
          "auto falls back to Command Prompt last");

    autoPlan = terminalhere::plan(requestFor(directory, ShellChoice::Auto),
                                  nothing);
    check(!autoPlan.ok && autoPlan.exitCode == WinExit::kMissingDependency,
          "auto fails with a missing-dependency code when nothing is installed");

    autoPlan = terminalhere::plan(
        requestFor(directory, ShellChoice::WindowsTerminal), onlyPowerShell);
    check(!autoPlan.ok && autoPlan.exitCode == WinExit::kMissingDependency,
          "an explicitly requested shell that is missing is reported");
    check(autoPlan.message.find(L"Windows Terminal") != std::wstring::npos
              && autoPlan.message.find(L"not") != std::wstring::npos,
          "the missing-shell message names the requested shell");
    check(autoPlan.plan.executable.empty(),
          "a missing explicit shell is never substituted");

    autoPlan = terminalhere::plan(
        requestFor(directory, ShellChoice::PowerShell7), onlyPowerShell);
    check(!autoPlan.ok, "an unavailable pwsh is reported, not substituted");
}

void testPlanCommandLine(const std::wstring &directory)
{
    const auto all = fakeLocator({ShellKind::WindowsTerminal,
                                  ShellKind::PowerShell7,
                                  ShellKind::WindowsPowerShell,
                                  ShellKind::CommandPrompt});

    const auto terminal = terminalhere::plan(
        requestFor(directory, ShellChoice::WindowsTerminal), all);
    check(terminal.ok, "a Windows Terminal plan is produced");
    const std::vector<std::wstring> expected{
        L"-w", L"new", L"new-tab", L"-d",
        terminalhere::escapeForWindowsTerminal(directory)};
    check(terminal.plan.arguments == expected,
          "the Windows Terminal command line uses the documented new-window, "
          "new-tab and starting-directory options");
    check(terminal.plan.workingDirectory == directory,
          "the Windows Terminal plan still carries the target directory");

    const auto prompt = terminalhere::plan(
        requestFor(directory, ShellChoice::CommandPrompt), all);
    check(prompt.ok && prompt.plan.arguments.empty(),
          "Command Prompt receives no target argument at all");
    check(prompt.plan.workingDirectory == directory,
          "Command Prompt receives the target as its process working "
          "directory");

    const auto shell = terminalhere::plan(
        requestFor(directory, ShellChoice::PowerShell7), all);
    check(shell.ok && shell.plan.arguments.empty(),
          "PowerShell receives no interpolated path argument");
    check(shell.plan.workingDirectory == directory,
          "PowerShell receives the target as its process working directory");

    const auto elevated = terminalhere::plan(
        requestFor(directory, ShellChoice::CommandPrompt, true), all);
    check(elevated.ok && elevated.plan.elevated,
          "the admin request is carried into the plan");

    // A UNC path is rejected before the existence check on purpose: the
    // limitation belongs to the shell, so the user is told about it instead of
    // being told the share does not exist.
    const auto unc = terminalhere::plan(
        requestFor(L"\\\\server\\share\\folder", ShellChoice::CommandPrompt),
        all);
    check(!unc.ok && unc.exitCode == WinExit::kUnsupported,
          "Command Prompt with a UNC directory is reported as unsupported");
    check(unc.message.find(L"UNC") != std::wstring::npos,
          "the UNC message explains the limitation");
    check(unc.message.find(L"pwsh") != std::wstring::npos,
          "the UNC message offers a working alternative");

    // Windows Terminal has no such limitation, so an unreachable share is only
    // reported as a missing directory rather than as an unsupported shell.
    const auto uncTerminal = terminalhere::plan(
        requestFor(L"\\\\server\\share\\folder", ShellChoice::WindowsTerminal),
        all);
    check(!uncTerminal.ok && uncTerminal.exitCode == WinExit::kNotFound,
          "Windows Terminal is not blocked by the UNC limitation itself");
    check(uncTerminal.message.find(L"UNC") == std::wstring::npos,
          "the Windows Terminal failure is about the directory, not the shell");

    check(terminalhere::escapeForWindowsTerminal(L"\\\\server\\share\\a;b")
              == L"\\\\server\\share\\a\\;b",
          "a UNC path with a semicolon is escaped for Windows Terminal");
}

// A UI child must not end up in a job this helper created. Whether it inherits
// a job from the launching harness is outside the helper's control, so the
// expected outcome is computed from the launcher's own job membership.
struct JobMembership {
    bool inJob = false;
    bool parentAllowsBreakaway = false;
    bool parentKillsOnClose = false;
};

JobMembership currentProcessJob()
{
    JobMembership result;
    BOOL inJob = FALSE;
    IsProcessInJob(GetCurrentProcess(), nullptr, &inJob);
    result.inJob = inJob != FALSE;
    if (result.inJob) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        if (QueryInformationJobObject(nullptr, JobObjectExtendedLimitInformation,
                                      &limits, sizeof(limits), nullptr)) {
            result.parentAllowsBreakaway
                = (limits.BasicLimitInformation.LimitFlags
                   & JOB_OBJECT_LIMIT_BREAKAWAY_OK)
                  != 0;
            result.parentKillsOnClose
                = (limits.BasicLimitInformation.LimitFlags
                   & JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE)
                  != 0;
        }
    }
    return result;
}

// Exercises the console-shell lifetime contract with a real child: the helper
// must keep the working directory it was given, must not place a UI child in the
// data-processing job, and must not report success for a process that died
// immediately.
void testConsoleLaunchContract(const std::wstring &probeChild,
                               const std::wstring &root)
{
    const std::wstring directory = root + L"\\console launch & dir";
    makeDirectory(directory);

    terminalhere::LaunchPlan plan;
    plan.shell            = ShellKind::CommandPrompt;
    plan.executable       = probeChild;
    plan.workingDirectory = directory;
    plan.arguments = {directory + L"\\console-report.txt", L"sleep", L"1500"};

    std::wstring message;
    const int code = terminalhere::execute(plan, &message);
    check(code == WinExit::kOk,
          "a console child that stays alive is reported as started: "
              + WinText::toUtf8(message));

    // execute() returns once the 400 ms aliveness window proves the shell did
    // not die immediately, but a child under a fresh console needs 250-450 ms
    // of process and console start-up before its report lands, so a single
    // read races the write. Poll with a bounded budget, the way the sibling
    // suites do.
    const std::wstring reportPath = directory + L"\\console-report.txt";
    std::string report;
    for (int attempt = 0; attempt < 40; ++attempt) {
        report = CommonTests::readTextFile(reportPath);
        if (!report.empty()) {
            break;
        }
        Sleep(50);
    }
    check(!report.empty(), "the launched child wrote its report");
    check(WinText::compareIgnoreCase(
              CommonTests::reportField(report, "cwd"), directory)
              == 0,
          "the launched shell inherited the requested working directory");

    const auto membership = currentProcessJob();
    const std::wstring killOnClose
        = CommonTests::reportField(report, "jobkillonclose");
    const bool childKillsOnClose = killOnClose == L"1";
    std::cout << "  [info] launcher in job: " << (membership.inJob ? 1 : 0)
              << ", parent kills on close: "
              << (membership.parentKillsOnClose ? 1 : 0)
              << ", terminal child kills on close: "
              << (childKillsOnClose ? 1 : 0) << '\n';
    // The data-processing job this helper creates always sets
    // JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE. A terminal child that merely inherited
    // the launcher's own job state proves no such job was added.
    check(childKillsOnClose
              == (membership.inJob && membership.parentKillsOnClose),
          "the terminal launcher creates no kill-on-close containment job");

    terminalhere::LaunchPlan dying = plan;
    dying.arguments = {directory + L"\\dying-report.txt", L"echo"};
    message.clear();
    const int dyingCode = terminalhere::execute(dying, &message);
    check(dyingCode == WinExit::kFailure,
          "a shell that exits immediately is reported as a failure");
    check(!message.empty(), "the immediate-exit failure explains itself");

    terminalhere::LaunchPlan missing = plan;
    missing.executable = directory + L"\\not-here.exe";
    message.clear();
    check(terminalhere::execute(missing, &message) == WinExit::kFailure,
          "an executable that does not exist fails");
    check(!message.empty(), "the missing-executable failure explains itself");
}

void testRealShellDiscovery()
{
    const auto locator = terminalhere::systemShellLocator();
    const auto cmd     = locator(ShellKind::CommandPrompt);
    check(!cmd.empty() && WinProc::isExecutableFile(cmd),
          "Command Prompt is discovered on this system");
    const auto windowsPowerShell = locator(ShellKind::WindowsPowerShell);
    if (!windowsPowerShell.empty()) {
        check(WinProc::isExecutableFile(windowsPowerShell),
              "the discovered Windows PowerShell path is a real executable");
    }
    const auto terminal = locator(ShellKind::WindowsTerminal);
    if (!terminal.empty()) {
        check(WinProc::isExecutableFile(terminal),
              "the discovered Windows Terminal path is a real executable");
    }
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
    const std::wstring directory = root + L"\\selected folder \u6d4b\u8bd5";
    makeDirectory(directory);

    TestHarness::section("terminal-here: Windows Terminal escaping");
    testWindowsTerminalEscaping();
    TestHarness::section("terminal-here: argument parsing");
    testArgumentParsing(directory);
    TestHarness::section("terminal-here: shell selection");
    testPlanSelection(directory);
    TestHarness::section("terminal-here: launch plan");
    testPlanCommandLine(directory);
    TestHarness::section("terminal-here: shell discovery");
    testRealShellDiscovery();
    TestHarness::section("terminal-here: console launch contract");
    testConsoleLaunchContract(probeChild, root);

    CommonTests::run(probeChild);

    if (SUCCEEDED(comResult)) {
        CoUninitialize();
    }
    return TestHarness::summarize("terminalhere_test");
}
