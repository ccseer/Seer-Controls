#include <filesystem>
#include <iostream>
#include <string>

#include "manifestassert.h"

using ManifestAssert::check;
using ManifestAssert::isExactStringArray;
using ManifestAssert::member;

int main(int argc, char *argv[])
{
    if (argc != 2) {
        std::cerr << "usage: terminalhere_manifest_test <package-root>\n";
        return 2;
    }

    ManifestAssert::Package package;
    if (!ManifestAssert::load(argv[1], &package, "terminal-here")) {
        return 1;
    }

    check(package.id == "io.1218.seer.terminal-here",
          "manifest selects the fixed Terminal Here package");
    check(package.name == "Terminal Here", "manifest name is Terminal Here");
    check(package.command == "terminal_here.exe",
          "command is the fixed Terminal Here helper");
    check(package.version == "1.0.0", "the package version is fixed");
    check(package.appMinVersion == "4.5.10",
          "the declared minimum host version is unchanged");
    check(isExactStringArray(member(package.root, "extensions"),
                             "${type_folder}"),
          "extensions contains the folder type token");
    check(package.extensions.size() == 1
              && package.extensions.front() == "${type_folder}",
          "the folder token survived manifest loading");
    check(package.arguments.size() == 2
              && package.arguments[0] == "--input"
              && package.arguments[1] == "${input_file}",
          "arguments pass the selected folder as a separate array element");
    check(package.timeoutMs >= 120000,
          "the timeout budget covers a UAC interaction");
    check(package.timeoutMs <= 300000,
          "the timeout budget stays finite and bounded");
    check(!package.description.empty(), "description is present");

    // A package-relative command must resolve inside the staged root and the
    // staged file must be the helper that the control bar would launch.
    check(package.commandPath.filename().string() == "terminal_here.exe",
          "the staged command file is the Terminal Here helper");

    return TestHarness::summarize("terminalhere_manifest_test");
}
