#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

#include "manifestassert.h"
#include "shelluiworker.h"

using ManifestAssert::check;
using ManifestAssert::isExactStringArray;
using ManifestAssert::member;

int main(int argc, char *argv[])
{
    if (argc != 2) {
        std::cerr << "usage: shellverb_manifest_test <package-root>\n";
        return 2;
    }

    ManifestAssert::Package package;
    if (!ManifestAssert::load(argv[1], &package, "explorer-properties")) {
        return 1;
    }

    check(package.id == "io.1218.seer.explorer-properties",
          "manifest selects the fixed Properties package");
    check(package.name == "Explorer Properties",
          "manifest name is Explorer Properties");
    check(package.command == "shellverb_properties.exe",
          "command is the fixed Properties helper");
    check(package.version == "1.1.0",
          "the release version matches the packaged archive name");
    check(package.appMinVersion == "4.5.10",
          "the declared minimum host version is unchanged");
    check(isExactStringArray(member(package.root, "extensions"),
                             "${type_file}"),
          "extensions contains the file type token");
    const std::vector<std::string> expectedArguments{"--input",
                                                     "${input_file}"};
    check(package.arguments == expectedArguments,
          "arguments pass the file as separate array elements");
    // The launcher and the worker run concurrently, so the dialog wait below is
    // spent inside the launcher's readiness window rather than added to it. The
    // wait has to fit inside that window, or readiness can never arrive in time
    // on a healthy worker, and the host's timeout has to outlast the launcher.
    check(ShellUiWorker::kPreReadinessUiWaitMs
              <= ShellUiWorker::Options().readyTimeoutMs,
          "the dialog wait fits inside the readiness window the launcher "
          "allows");
    const auto handoffBudget
        = static_cast<long long>(ShellUiWorker::totalHandoffBudgetMs());
    check(package.timeoutMs > handoffBudget,
          "the timeout budget outlasts the handoff the launcher runs");
    check(!package.description.empty(), "description is present");

    return TestHarness::summarize("shellverb_manifest_test");
}
