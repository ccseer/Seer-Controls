#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

#include "manifestassert.h"
#include "share.h"
#include "shelluiworker.h"

using ManifestAssert::check;
using ManifestAssert::isExactStringArray;
using ManifestAssert::member;

int main(int argc, char *argv[])
{
    std::filesystem::path stagingRoot;
    if (argc >= 2) {
        stagingRoot = std::filesystem::weakly_canonical(argv[1]);
    }
    else if (std::filesystem::exists("plugin.json")) {
        stagingRoot = std::filesystem::weakly_canonical(".");
    }
    else if (std::filesystem::exists("manifest-stage/plugin.json")) {
        stagingRoot = std::filesystem::weakly_canonical("manifest-stage");
    }
    else {
        std::cerr << "usage: share_manifest_test <staging-root>\n";
        return 2;
    }

    ManifestAssert::Package package;
    if (!ManifestAssert::load(stagingRoot, &package, "explorer-share")) {
        return 1;
    }

    check(package.id == "io.1218.seer.explorer-share",
          "manifest selects io.1218.seer.explorer-share");
    check(package.name == "Share", "manifest name is Share");
    check(package.command == "seer_share.exe", "command is seer_share.exe");
    check(package.version == "1.1.0",
          "the release version matches the packaged archive name");
    check(package.appMinVersion == "4.5.10",
          "the declared minimum host version is unchanged");
    check(isExactStringArray(member(package.root, "extensions"),
                             "${type_file}"),
          "extensions contains the file type token ${type_file}");
    const std::vector<std::string> expectedArguments{"--input",
                                                     "${input_file}"};
    check(package.arguments == expectedArguments,
          "arguments are --input ${input_file}");
    // The launcher and the worker run concurrently, so the worker's payload wait
    // is spent inside the launcher's readiness window rather than added to it.
    // Two things have to hold: the wait has to fit inside that window, or
    // readiness can never arrive in time and a healthy handoff fails; and the
    // host's timeout has to outlast the launcher, which is the only process it
    // bounds.
    const auto options = handoffOptions();
    check(kSharePayloadWaitMs <= options.readyTimeoutMs,
          "the payload wait fits inside the readiness window the launcher "
          "allows");
    const auto handoffBudget = static_cast<long long>(
        ShellUiWorker::totalHandoffBudgetMs(options));
    check(package.timeoutMs > handoffBudget,
          "the timeout budget outlasts the handoff the launcher runs");
    check(!package.description.empty(), "description is present");

    return TestHarness::summarize("share_manifest_test");
}
