#include <filesystem>
#include <iostream>
#include <string>

#include "manifestassert.h"
#include "wincmd.h"

using ManifestAssert::check;
using ManifestAssert::member;

// The package root comes from CTest, so it can contain characters outside the
// active code page: read the wide command line instead of decoding argv.
int main()
{
    const std::vector<std::wstring> arguments = WinCmd::commandLineArguments();
    if (arguments.size() != 2) {
        std::cerr << "usage: hexviewer_manifest_test <package-root>\n";
        return 2;
    }

    ManifestAssert::Package package;
    if (!ManifestAssert::load(arguments[1], &package, "hex-viewer")) {
        return 1;
    }

    check(package.id == "io.1218.seer.hex-viewer",
          "manifest selects the fixed hex-viewer package");
    check(package.name == "Hex View", "manifest name is Hex View");
    check(package.command == "seer_hex.exe",
          "command is the fixed hex helper");
    check(package.version == "1.0.0", "the package version is fixed");

    // The 4.7.11 floor matches the .hextxt syntax contribution: an older host
    // installs the package but its text viewer does not claim *.hextxt, which
    // only affects the runtime behavior of a very old install, not package
    // loading.
    check(package.appMinVersion == "4.7.11",
          "the declared minimum host version is 4.7.11");

    // The whole-file matcher: any file qualifies, no folder matcher.
    check(package.extensions.size() == 1
              && package.extensions[0] == "${type_file}",
          "extensions lists exactly the whole-file token");

    check(package.arguments.size() == 4
              && package.arguments[0] == "${input_file}"
              && package.arguments[1] == "${seerviewer_exe}"
              && package.arguments[2] == "--max-kb=64"
              && package.arguments[3] == "--wait-timeout=30000",
          "arguments pass input file, seerviewer and the fixed max-kb/"
          "wait-timeout defaults");
    check(package.timeoutMs == 60000,
          "the manifest timeout is 60000 as declared");
    check(package.successExitCodes.size() == 1
              && package.successExitCodes.front() == 0,
          "success_exit_codes is [0]");

    // No icon_path on purpose: the control bar takes the button icon from the
    // helper executable's embedded icon resource (GetFileIcon), exactly like
    // the OCR package. res/hex.png stays in the repository as the design
    // source for res/hex.ico and is not shipped.
    check(member(package.root, "icon_path") == nullptr,
          "the manifest declares no icon_path");

    // Host semantics: the launch succeeds, the main preview stays open.
    const auto *closeAfterSuccess = member(package.root, "close_after_success");
    check(closeAfterSuccess
                  && closeAfterSuccess->kind
                         == ManifestAssert::JsonValue::Kind::Boolean
                  && closeAfterSuccess->stringValue == "false",
          "close_after_success is false");

    check(package.commandPath.filename().string() == "seer_hex.exe",
          "the staged command file is the hex helper");

    // The syntax contribution: Seer's text viewer loads <package-root>/syntax/
    // *.xml, so the staged tree must carry the HexDump definition and keep the
    // attributes the host contract enforces (name, extensions, positive
    // priority, loadable kateversion). Losing the file or the attributes
    // silently turns .hextxt dumps back into plain text, or makes the host
    // refuse the install outright.
    const auto definitionPath = package.packageRoot / "syntax" / "hextxt.xml";
    std::string definition;
    check(std::filesystem::exists(definitionPath),
          "the staged package ships syntax/hextxt.xml");
    if (ManifestAssert::readWholeFile(definitionPath, &definition)) {
        check(definition.find("name=\"HexDump\"") != std::string::npos,
              "the definition claims the HexDump language name");
        check(definition.find("extensions=\"*.hextxt\"") != std::string::npos,
              "the definition claims *.hextxt");
        check(definition.find("version=\"2\"") != std::string::npos,
              "the definition declares version 2");
        check(definition.find("priority=\"100\"") != std::string::npos,
              "the definition declares priority 100");
        // The host rejects a package whose definition has no loadable
        // kateversion, because the syntax library would discard it without a
        // trace and the extension would never be claimed.
        check(definition.find("kateversion=\"5.0\"") != std::string::npos,
              "the definition declares kateversion 5.0");
    }
    else {
        check(false, "syntax/hextxt.xml is readable");
    }

    return TestHarness::summarize("hexviewer_manifest_test");
}
