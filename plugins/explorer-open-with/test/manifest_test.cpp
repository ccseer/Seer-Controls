#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

#include "manifestassert.h"

using ManifestAssert::check;
using ManifestAssert::isExactStringArray;
using ManifestAssert::member;

int main(int argc, char *argv[])
{
    if (argc != 2) {
        std::cerr << "usage: openwith_manifest_test <package-root>\n";
        return 2;
    }

    ManifestAssert::Package package;
    if (!ManifestAssert::load(argv[1], &package, "explorer-open-with")) {
        return 1;
    }

    check(package.id == "io.1218.seer.explorer-open-with",
          "manifest selects the fixed Open With package");
    check(package.name == "Open With", "manifest name is Open With");
    check(package.command == "shellopenwith.exe",
          "command is shellopenwith.exe");
    check(package.appMinVersion == "4.5.10",
          "the declared minimum host version is unchanged");
    check(isExactStringArray(member(package.root, "extensions"),
                             "${type_file}"),
          "extensions contains the file type token");
    const std::vector<std::string> expectedArguments{"--input", "${input_file}",
                                                     "${use_backslash}"};
    check(package.arguments == expectedArguments,
          "arguments pass the file and the separator token separately");
    // The Open With dialog is modal and owns this process for as long as it is
    // open, so the host timeout bounds how long the user may take to choose.
    check(package.timeoutMs >= 30000,
          "the timeout budget leaves room for a modal dialog");
    check(package.timeoutMs <= 600000,
          "the timeout budget stays finite and bounded");
    check(!package.description.empty(), "description is present");

    return TestHarness::summarize("openwith_manifest_test");
}
