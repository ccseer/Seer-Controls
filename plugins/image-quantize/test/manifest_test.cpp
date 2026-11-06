#include <filesystem>
#include <iostream>
#include <set>
#include <string>

#include "manifestassert.h"

using ManifestAssert::check;
using ManifestAssert::member;

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "usage: image_quantize_manifest_test <package-root>\n";
        return 2;
    }

    ManifestAssert::Package package;
    if (!ManifestAssert::load(argv[1], &package, "image-quantize")) {
        return 1;
    }

    check(package.id == "io.1218.seer.image-quantize", "manifest id is fixed");
    check(package.name == "Quantize Image", "manifest name is Quantize Image");
    check(package.version == "1.0.0", "package version is 1.0.0");
    check(package.appMinVersion == "4.7.10",
          "appMinVersion matches the host that ships ${type_image}");
    check(package.backend == "process", "backend is process");
    check(package.command == "image_quantize.exe",
          "command is the helper executable");
    check(package.extensions.size() == 1
              && package.extensions[0] == "${type_image}",
          "extensions lists exactly the image token");
    check(package.arguments.size() == 5
              && package.arguments[0] == "--input"
              && package.arguments[1] == "${input_file}"
              && package.arguments[2] == "--seerviewer"
              && package.arguments[3] == "${seerviewer_exe}"
              && package.arguments[4] == "--wait-timeout=300000",
          "arguments open the waiting viewer on the produced file");
    check(package.timeoutMs == 300000,
          "timeout_ms leaves room for an 8K source at the max tier");
    check(package.successExitCodes.size() == 1
              && package.successExitCodes[0] == 0,
          "only exit code 0 counts as success");
    const ManifestAssert::JsonValue* close =
        member(package.root, "close_after_success");
    check(close != nullptr && close->stringValue == "false",
          "close_after_success stays false");

    const std::set<std::string> expected = {
        "plugin.json", "image_quantize.exe", "README.md"};
    std::set<std::string> actual;
    for (const auto& entry : std::filesystem::directory_iterator(package.packageRoot)) {
        actual.insert(entry.path().filename().string());
    }
    check(actual == expected, "staged tree holds exactly three files");

    return TestHarness::summarize("image_quantize_manifest_test");
}
