#include <filesystem>
#include <iostream>
#include <string>

#include "manifestassert.h"

using ManifestAssert::check;
using ManifestAssert::hasString;
using ManifestAssert::member;

int main(int argc, char *argv[])
{
    if (argc != 2) {
        std::cerr << "usage: ocr_manifest_test <package-root>\n";
        return 2;
    }

    ManifestAssert::Package package;
    if (!ManifestAssert::load(argv[1], &package, "ocr")) {
        return 1;
    }

    check(package.id == "io.1218.seer.ocr",
          "manifest selects the fixed OCR package");
    check(package.name == "OCR Text", "manifest name is OCR Text");
    check(package.command == "SeerOcrWorker.exe",
          "command is the fixed OCR helper");
    check(package.version == "1.0.0", "the package version is fixed");

    // ${screenshot} and SeerViewer --wait-file both arrived in 4.8.0, so an
    // older host can neither expand the argument nor open the pending window.
    check(package.appMinVersion == "4.8.0",
          "the declared minimum host version is 4.8.0");

    // The only multi-type package in the repository: both matchers, in order.
    check(package.extensions.size() == 2
              && package.extensions[0] == "${type_pdf}"
              && package.extensions[1] == "${type_image}",
          "extensions lists the pdf and image tokens in order");
    check(hasString(member(package.root, "extensions"), "${type_pdf}"),
          "the pdf token survived manifest loading");
    check(hasString(member(package.root, "extensions"), "${type_image}"),
          "the image token survived manifest loading");

    check(package.arguments.size() == 7
              && package.arguments[0] == "${screenshot}"
              && package.arguments[1] == "${seerviewer_exe}"
              && package.arguments[2] == "${input_file}"
              && package.arguments[3] == "--ep=auto"
              && package.arguments[4] == "--cls=1"
              && package.arguments[5] == "--barcode=1"
              && package.arguments[6] == "--wait-timeout=300000",
          "arguments pass screenshot, seerviewer, input file and the fixed "
          "ep/cls/barcode/wait-timeout defaults");
    check(package.timeoutMs >= 120000,
          "the timeout budget covers model cold start");
    check(!package.description.empty(), "description is present");

    // A package-relative command must resolve inside the staged root and the
    // staged file must be the helper that the control bar would launch.
    check(package.commandPath.filename().string() == "SeerOcrWorker.exe",
          "the staged command file is the OCR helper");

    return TestHarness::summarize("ocr_manifest_test");
}
