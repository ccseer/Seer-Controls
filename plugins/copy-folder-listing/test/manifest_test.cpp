#include <iostream>
#include <string>

#include "manifestassert.h"

using ManifestAssert::check;
using ManifestAssert::isExactStringArray;
using ManifestAssert::JsonValue;
using ManifestAssert::member;

int main(int argc, char *argv[])
{
    if (argc != 2) {
        std::cerr << "usage: folderlisting_manifest_test <package-root>\n";
        return 2;
    }

    ManifestAssert::Package package;
    if (!ManifestAssert::load(argv[1], &package, "copy-folder-listing")) {
        return 1;
    }

    check(package.id == "io.1218.seer.copy-folder-listing",
          "manifest selects the fixed Copy Folder Listing package");
    check(package.name == "Copy Folder Listing",
          "manifest name is Copy Folder Listing");
    check(package.command == "copy_folder_listing.exe",
          "command is the fixed Copy Folder Listing helper");
    check(package.appMinVersion == "4.5.11",
          "the declared minimum host version is unchanged");
    check(package.version == "1.0.0",
          "manifest version is 1.0.0");
    check(isExactStringArray(member(package.root, "extensions"),
                             "${type_folder}"),
          "extensions contains the folder type token");

    const std::vector<std::string> expectedArguments{"--input", "${input_file}"};
    check(package.arguments == expectedArguments,
          "arguments pass the selected folder as a separate array element");
    check(package.timeoutMs >= 60000,
          "the timeout budget covers enumerating a large folder");
    check(package.timeoutMs <= 600000,
          "the timeout budget stays finite and bounded");
    check(!package.description.empty(), "description is present");

    const auto *closeAfterSuccess
        = member(package.root, "close_after_success");
    check(closeAfterSuccess != nullptr
              && closeAfterSuccess->kind == JsonValue::Kind::Boolean
              && closeAfterSuccess->stringValue == "false",
          "the manifest declares close_after_success = false");

    // The documented product defaults are the helper's defaults, not host limits,
    // so the manifest must not try to encode them.
    for (const auto &argument : package.arguments) {
        check(argument.find("--depth") == std::string::npos
                  && argument.find("--max-entries") == std::string::npos
                  && argument.find("--max-bytes") == std::string::npos,
              "the manifest does not pin the helper's product defaults");
    }

    return TestHarness::summarize("folderlisting_manifest_test");
}
