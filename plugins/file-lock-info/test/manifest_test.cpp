#include <iostream>
#include <string>

#include "manifestassert.h"

using ManifestAssert::check;
using ManifestAssert::isExactStringArray;
using ManifestAssert::member;

int main(int argc, char *argv[])
{
    if (argc != 2) {
        std::cerr << "usage: filelock_manifest_test <package-root>\n";
        return 2;
    }

    ManifestAssert::Package package;
    if (!ManifestAssert::load(argv[1], &package, "file-lock-info")) {
        return 1;
    }

    check(package.id == "io.1218.seer.file-lock-info",
          "manifest selects the fixed Who Is Locking This File package");
    check(package.command == "file_lock_info.exe",
          "command is the fixed file-lock helper");
    check(package.version == "1.0.0", "the package version is fixed");
    check(package.appMinVersion == "4.5.10",
          "the declared minimum host version is unchanged");
    check(isExactStringArray(member(package.root, "extensions"),
                             "${type_file}"),
          "extensions contains the file type token, not the folder token");

    const std::vector<std::string> expectedArguments{"--input", "${input_file}"};
    check(package.arguments == expectedArguments,
          "arguments pass the selected file as a separate array element");
    check(package.timeoutMs >= 120000,
          "the timeout budget covers an optional consent prompt");
    check(package.timeoutMs <= 600000,
          "the timeout budget stays finite and bounded");
    check(!package.description.empty(), "description is present");

    // The admin option is a helper argument a user may add in Seer's Arguments
    // editor; it must not be baked into the shipped default list, and there must
    // be no admin property in the manifest.
    for (const auto &argument : package.arguments) {
        check(argument != "--admin",
              "admin is not enabled by default in the shipped arguments");
    }
    check(member(package.root, "admin") == nullptr,
          "no admin property was invented in the manifest");

    return TestHarness::summarize("filelock_manifest_test");
}
