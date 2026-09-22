// The worker is spawned detached with no handle inheritance, so the host can
// only ever read the LAUNCHER's stderr. These checks prove the reason channel the
// handoff depends on: the launcher owns a buffer the worker can write into, the
// launcher reads back exactly what was published and bounded, and a refusal is
// explained on whichever channel the half that raised it can reach.
//
// usage: shelluiworker_test

#include <windows.h>

#include <optional>
#include <string>
#include <vector>

#include "shelluiworker.h"
#include "stderrharness.h"
#include "testharness.h"

using TestHarness::check;

namespace {
constexpr auto kHandoffFlag = L"--ui-ready-event";

// A refusal raised by validation, in the shape the handoff expects: the reason
// comes back to the caller instead of being reported by the validator.
std::optional<std::wstring> refuseWith(const wchar_t *reason)
{
    return std::wstring(reason);
}
}  // namespace

int main()
{
    const std::wstring eventName = L"Local\\SeerPluginUi-reason-test";

    // Without an owner-created channel the worker has nowhere to write, which is
    // the exact situation the launcher must avoid.
    check(!ShellUiWorker::publishReason(L"Local\\SeerPluginUi-absent-test",
                                        L"ignored"),
          "publishing without a channel fails instead of inventing one");

    const auto channel = ShellUiWorker::createReasonChannel(eventName);
    check(static_cast<bool>(channel), "the launcher can create the reason channel");
    check(ShellUiWorker::readReason(channel).empty(),
          "a fresh channel reads back empty");

    const std::wstring message = L"Properties could not be opened.";
    check(ShellUiWorker::publishReason(eventName, message),
          "the worker can publish through the launcher's channel");
    check(ShellUiWorker::readReason(channel) == message,
          "the launcher reads back the published reason");

    // Reasoning text is spliced into a fixed-size buffer, so an oversized message
    // must be truncated instead of overrunning it.
    const auto oversized
        = std::wstring(ShellUiWorker::kMaxReasonChars + 200, L'x');
    check(ShellUiWorker::publishReason(eventName, oversized),
          "an oversized reason still publishes");
    const auto readBack = ShellUiWorker::readReason(channel);
    check(readBack.size() == ShellUiWorker::kMaxReasonChars,
          "an oversized reason is truncated to the channel capacity");
    check(!readBack.empty() && readBack.front() == L'x',
          "the truncated reason keeps its leading text");

    // An empty message must leave the channel empty, so the launcher does not
    // emit a blank line the host would render as an empty toast.
    check(ShellUiWorker::publishReason(eventName, std::wstring()),
          "an empty reason publishes successfully");
    check(ShellUiWorker::readReason(channel).empty(),
          "an empty reason leaves the channel empty");

    // A refusal raised by the worker's own validation has to reach the launcher's
    // channel: this process is detached with no inherited stderr, so that buffer
    // is the only thing the host can read back. The launcher's half of the
    // handoff is stood up here, because the worker refuses to run without it.
    {
        const std::wstring workerEvent = L"Local\\SeerPluginUi-worker-test";
        ShellUiWorker::Handle ready(
            CreateEventW(nullptr, TRUE, FALSE, workerEvent.c_str()));
        const auto workerChannel
            = ShellUiWorker::createReasonChannel(workerEvent);
        check(static_cast<bool>(ready) && static_cast<bool>(workerChannel),
              "the worker half of the handoff can be stood up");

        // Captured so the copy the worker also writes to its own stderr, which a
        // detached process would not even have, stays out of ctest output.
        StderrHarness::Capture capture;
        const int refused = ShellUiWorker::run(
            {L"shelluiworker_test.exe", L"--input", L"C:\\path.txt",
             kHandoffFlag, workerEvent},
            [](const std::vector<std::wstring> &) {
                return refuseWith(L"the worker refused the request");
            },
            [](const std::vector<std::wstring> &, const auto &,
               const auto &) { return 0; });
        check(refused == 2,
              "a worker-side validation refusal keeps the argument exit code");
        check(ShellUiWorker::readReason(workerChannel)
                  == L"the worker refused the request",
              "the worker publishes a reason for its own validation refusal");
        capture.drain();
    }

    // The launcher runs where the host reads stderr directly and has no reason
    // channel yet, so that is where its refusal belongs. Handing the reason back
    // to the caller, rather than letting the validator report it, is what keeps
    // it from being dropped on one side or the other.
    {
        StderrHarness::Capture capture;
        const int refused = ShellUiWorker::run(
            {L"shelluiworker_test.exe", L"--input", L"C:\\path.txt"},
            [](const std::vector<std::wstring> &) {
                return refuseWith(L"the launcher refused the request");
            },
            [](const std::vector<std::wstring> &, const auto &,
               const auto &) { return 0; });
        check(refused == 2,
              "a launcher-side validation refusal keeps the argument exit code");
        const auto written = capture.drain();
        check(written.find("the launcher refused the request")
                  != std::string::npos,
              "the launcher explains its refusal on the stderr the host reads");
    }

    return TestHarness::summarize("shelluiworker_test");
}
