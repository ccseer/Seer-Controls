#include <windows.h>
#include <objbase.h>

#include <string>
#include <vector>

#include "common_tests.h"
#include "fileprobe.h"
#include "folderlisting.h"
#include "wincmd.h"
#include "winclip.h"
#include "winexit.h"
#include "winpath.h"
#include "wintext.h"
#include "winui.h"

#include "testharness.h"

using TestHarness::check;

namespace {

std::wstring tempRoot()
{
    return FileProbe::tempRoot(L"SeerFolderListingTests");
}

bool makeDirectory(const std::wstring &path)
{
    return FileProbe::makeDirectoryTree(path);
}

std::vector<std::wstring> makeArgv(std::initializer_list<const wchar_t *> parts)
{
    std::vector<std::wstring> result;
    for (const auto *part : parts) {
        result.emplace_back(part);
    }
    return result;
}

std::vector<std::wstring> lines(const std::wstring &text)
{
    return WinText::splitLines(text);
}

bool containsLine(const std::wstring &text, const std::wstring &needle)
{
    for (const auto &line : lines(text)) {
        if (line.find(needle) != std::wstring::npos) {
            return true;
        }
    }
    return false;
}

std::wstring buildFixture(const std::wstring &root)
{
    const std::wstring source = root + L"\\tree \u6d4b\u8bd5";
    makeDirectory(source + L"\\zeta");
    makeDirectory(source + L"\\Alpha");
    makeDirectory(source + L"\\Alpha\\deep\\deeper\\deepest");
    makeDirectory(source + L"\\\u4e2d\u6587 \u6587\u4ef6\u5939");
    FileProbe::writeTextFile(source + L"\\b.txt", "b");
    FileProbe::writeTextFile(source + L"\\Alpha\\a.txt", "a");
    FileProbe::writeTextFile(source + L"\\zeta\\z.txt", "z");
    FileProbe::writeTextFile(source + L"\\Alpha\\deep\\d.txt", "d");
    FileProbe::writeTextFile(source + L"\\Alpha\\deep\\deeper\\e.txt", "e");
    FileProbe::writeTextFile(source + L"\\Alpha\\deep\\deeper\\deepest\\f.txt",
                             "f");
    FileProbe::writeTextFile(source + L"\\hidden.txt", "h");
    SetFileAttributesW(
        WinPath::addExtendedPrefix(source + L"\\hidden.txt").c_str(),
        FILE_ATTRIBUTE_HIDDEN);
    FileProbe::writeTextFile(source + L"\\tick`name.txt", "tick");
    FileProbe::writeTextFile(source + L"\\tick```triple.txt", "triple");
    FileProbe::writeTextFile(source + L"\\interactive;name.txt", "semi");
    return source;
}

void testPatterns()
{
    check(folderlisting::matchesPattern(L"node_modules", L"node_modules"),
          "an exact pattern matches");
    check(folderlisting::matchesPattern(L"NODE_MODULES", L"node_modules"),
          "pattern matching ignores case");
    check(folderlisting::matchesPattern(L"build.out", L"*.out"),
          "a leading star matches a prefix");
    check(folderlisting::matchesPattern(L"out", L"out*"), "a trailing star matches");
    check(folderlisting::matchesPattern(L"cfg", L"c?g"),
          "a question mark matches one character");
    check(!folderlisting::matchesPattern(L"cggg", L"c?g"),
          "a question mark matches exactly one character");
    check(folderlisting::matchesPattern(L"anything", L"*"),
          "a bare star matches everything");
    check(!folderlisting::matchesPattern(L"build", L"dist"),
          "an unrelated pattern does not match");
    check(folderlisting::matchesPattern(L"aXbXc", L"a*b*c"),
          "multiple stars match in order");
    check(!folderlisting::matchesPattern(L"abc", L"a*c*d"),
          "a pattern that runs out does not match");
}

void testArgumentParsing(const std::wstring &source)
{
    std::wstring error;
    const auto defaults = folderlisting::parseArguments(
        makeArgv({L"copy_folder_listing.exe", L"--input", source.c_str()}),
        &error);
    check(defaults.has_value(), "a plain request parses: "
                                    + WinText::toUtf8(error));
    if (defaults) {
        check(defaults->maxEntries == 5000,
              "the entry limit defaults to 5000");
        check(defaults->maxBytes == 1024 * 1024,
              "the byte limit defaults to 1 MiB");
        check(!defaults->includeHidden, "hidden entries are skipped by default");
        check(defaults->excludes.empty(),
              "no default exclusion pattern is applied");
    }

    const auto explicitRequest = folderlisting::parseArguments(
        makeArgv({L"x.exe", L"--input", source.c_str(),
                  L"--include-hidden", L"--exclude", L"*.tmp",
                  L"--exclude", L"cache", L"--max-entries", L"10",
                  L"--max-bytes", L"2048"}),
        &error);
    check(explicitRequest.has_value(), "every option parses");
    if (explicitRequest) {
        check(explicitRequest->includeHidden, "include-hidden is read");
        check(explicitRequest->excludes.size() == 2,
              "exclude can be repeated");
        check(explicitRequest->maxEntries == 10
                  && explicitRequest->maxBytes == 2048,
              "the limits are read");
    }

    check(!folderlisting::parseArguments(
              makeArgv({L"x.exe", L"--input", source.c_str(), L"--format",
                        L"text"}))
               .has_value(),
          "the removed --format option is rejected");
    check(!folderlisting::parseArguments(
              makeArgv({L"x.exe", L"--input", source.c_str(), L"--depth",
                        L"1"}))
               .has_value(),
          "the removed --depth option is rejected");
    check(!folderlisting::parseArguments(
              makeArgv({L"x.exe", L"--input", source.c_str(), L"--max-entries",
                        L"0"}))
               .has_value(),
          "a zero entry limit is rejected");
    check(!folderlisting::parseArguments(
              makeArgv({L"x.exe", L"--input", source.c_str(), L"--max-bytes",
                        L"-5"}))
               .has_value(),
          "a negative byte limit is rejected");
    check(!folderlisting::parseArguments(
              makeArgv({L"x.exe", L"--input", source.c_str(), L"--max-bytes",
                        L"99999999999999999999"}))
               .has_value(),
          "a byte limit beyond the 64-bit range is rejected");
    check(!folderlisting::parseArguments(
              makeArgv({L"x.exe", L"--input", source.c_str(), L"--input",
                        source.c_str()}))
               .has_value(),
          "a duplicated input is rejected");
    check(!folderlisting::parseArguments(
              makeArgv({L"x.exe", L"--input", source.c_str(), L"--exclude",
                        L""}))
               .has_value(),
          "an empty exclude pattern is rejected");
    check(!folderlisting::parseArguments(
              makeArgv({L"x.exe", L"--max-entries", L"5"}))
               .has_value(),
          "a missing input is rejected");
}

void testShapeAndOrdering(const std::wstring &source)
{
    folderlisting::Request request;
    request.root = source;

    const auto result = folderlisting::buildListing(request);
    check(result.ok, "a listing is built: " + WinText::toUtf8(result.message));
    const auto all = lines(result.listing);
    check(all.size() == 7,
          "only the first level is listed: " + WinText::toUtf8(result.listing));

    const auto indexOf = [&](const std::wstring &needle) {
        for (size_t index = 0; index < all.size(); ++index) {
            if (all[index].find(needle) != std::wstring::npos) {
                return index;
            }
        }
        return all.size();
    };
    check(indexOf(L"Alpha/") < indexOf(L"zeta/"),
          "folders are ordered case-insensitively");
    check(indexOf(L"zeta/") < indexOf(L"b.txt"),
          "folders come before files");
    check(!containsLine(result.listing, L"tree \u6d4b\u8bd5/"),
          "the selected folder itself is not part of its listing");
    check(!containsLine(result.listing, L"a.txt"),
          "the content of subfolders is never reached");
    check(!containsLine(result.listing, L"deep/"),
          "deeper levels are not listed");
    check(!containsLine(result.listing, L"hidden.txt"),
          "hidden entries are omitted by default");

    folderlisting::Request hidden = request;
    hidden.includeHidden     = true;
    const auto withHidden    = folderlisting::buildListing(hidden);
    check(containsLine(withHidden.listing, L"hidden.txt"),
          "include-hidden shows hidden entries");
    check(lines(withHidden.listing).size() == 8,
          "include-hidden adds exactly the hidden entries");

    folderlisting::Request excluded = request;
    excluded.excludes.push_back(L"tick*");
    const auto withoutTicks = folderlisting::buildListing(excluded);
    check(!containsLine(withoutTicks.listing, L"tick`name.txt"),
          "an exclude pattern removes matching entries");
    check(lines(withoutTicks.listing).size() == 5,
          "an exclude pattern removes exactly the matching entries");

    folderlisting::Request empty = request;
    empty.root              = source + L"\\empty-folder";
    if (makeDirectory(empty.root)) {
        const auto nothing = folderlisting::buildListing(empty);
        check(nothing.ok && nothing.listing.empty(),
              "an empty folder yields an empty listing and succeeds");
    }
}

void testLimits(const std::wstring &source)
{
    folderlisting::Request request;
    request.root       = source;
    request.maxEntries = 3;
    const auto limited = folderlisting::buildListing(request);
    check(limited.ok && limited.truncated,
          "the entry limit stops the walk and marks it truncated");
    check(lines(limited.listing).size() == 4,
          "the entry limit is respected exactly (three entries plus the note)");
    check(containsLine(limited.listing, L"[truncated:"),
          "the truncation is marked in the output");
    check(limited.truncationNote.find(L"entry limit") != std::wstring::npos,
          "the truncation note names the entry limit");

    folderlisting::Request tiny = request;
    tiny.maxEntries        = 5000;
    tiny.maxBytes          = 80;
    const auto byteLimited = folderlisting::buildListing(tiny);
    check(byteLimited.ok && byteLimited.truncated,
          "the byte limit stops the walk and marks it truncated");
    check(byteLimited.listing.size() < 300,
          "the byte limit keeps the output small");
    check(byteLimited.truncationNote.find(L"byte limit") != std::wstring::npos,
          "the truncation note names the byte limit");
    check(byteLimited.truncationNote.find(L"UTF-8") != std::wstring::npos,
          "the truncation note states the unit of the byte limit");

    // A byte limit below the first entry renders nothing but still writes the
    // always-visible truncation note; that must not be misreported as "every
    // entry was skipped".
    folderlisting::Request firstLine = request;
    firstLine.maxEntries        = 5000;
    firstLine.maxBytes          = 1;
    const auto tinyLimit        = folderlisting::buildListing(firstLine);
    check(tinyLimit.ok && tinyLimit.truncated,
          "a byte limit below the first entry still reports the truncation");
    check(containsLine(tinyLimit.listing,
                       L"[truncated: the output byte limit"),
          "the truncation note is the only content when the limit is tiny");
    check(!containsLine(tinyLimit.listing, L"skipped"),
          "the byte limit is not misreported as an all-filtered folder");

    // The limit is a UTF-8 byte limit, not a UTF-16 unit limit: a name with
    // Chinese characters must cost more than its character count.
    folderlisting::Request generous = request;
    generous.maxEntries        = folderlisting::kDefaultMaxEntries;
    generous.maxBytes          = folderlisting::kDefaultMaxBytes;
    const auto measured        = folderlisting::buildListing(generous);
    check(!measured.truncated, "the generous limit is not hit by the fixture");
    const auto measuredBytes = WinText::toUtf8(measured.listing).size();
    check(measuredBytes > measured.listing.size(),
          "a listing containing multi-byte names costs more UTF-8 bytes than it "
          "has UTF-16 units");
    check(measuredBytes <= folderlisting::kDefaultMaxBytes,
          "the generated text stays within the default byte limit");

    // Determinism: the same request must produce the same text.
    const auto again = folderlisting::buildListing(request);
    check(again.listing == limited.listing,
          "the same request produces the same listing");
}

void testUnpairedSurrogateName(const std::wstring &root)
{
    // NTFS accepts names that are not well-formed Unicode. The byte limit is a
    // UTF-8 limit, so such a name still has to be charged - the conversion
    // substitutes one U+FFFD per bad unit - rather than measuring as nothing
    // and letting an arbitrarily long name through.
    const std::wstring source = root + L"\\surrogate";
    makeDirectory(source);
    const std::wstring name = std::wstring(L"lone") + wchar_t(0xD800)
                              + std::wstring(L"name.txt");
    FileProbe::writeTextFile(source + L"\\" + name, "x");

    folderlisting::Request request;
    request.root      = source;
    request.maxBytes  = 8;
    const auto limited = folderlisting::buildListing(request);
    check(limited.ok && limited.truncated,
          "a name with an unpaired surrogate is charged against the byte "
          "limit: " + WinText::toUtf8(limited.truncationNote));
}

void testReparseEntries()
{
    wchar_t buffer[32768]{};
    const DWORD written
        = GetEnvironmentVariableW(L"LOCALAPPDATA", buffer, 32768);
    if (written == 0 || written >= 32768) {
        std::cout << "  [skip] LOCALAPPDATA is unavailable\n";
        return;
    }
    const std::wstring aliases
        = WinPath::join(std::wstring(buffer), L"Microsoft\\WindowsApps");
    std::vector<WinPath::DirectoryEntry> entries;
    if (!WinPath::enumerateDirectory(aliases, &entries, nullptr)) {
        std::cout << "  [skip] the execution-alias folder is unavailable\n";
        return;
    }
    size_t links = 0;
    for (const auto &entry : entries) {
        if (entry.isReparsePoint
            && (entry.attributes & FILE_ATTRIBUTE_HIDDEN) == 0) {
            ++links;
        }
    }
    if (links == 0) {
        std::cout << "  [skip] this machine exposes no reparse point to test "
                     "against\n";
        return;
    }

    folderlisting::Request request;
    request.root       = aliases;
    request.maxEntries = 5000;
    const auto result  = folderlisting::buildListing(request);
    check(result.ok, "a folder holding links is rendered");
    check(containsLine(result.listing, L"[link, not followed]"),
          "links are marked and not followed");
    size_t marked = 0;
    for (const auto &line : lines(result.listing)) {
        if (line.find(L"[link, not followed]") != std::wstring::npos) {
            ++marked;
        }
    }
    check(marked == links,
          "every link is marked exactly once and no other entry is");

    if (links >= 2) {
        // A link line must consume the entry budget like any other rendered
        // line; a small limit has to stop the output even when every visible
        // entry is a reparse point.
        folderlisting::Request cappedRequest = request;
        cappedRequest.maxEntries = links / 2;
        const auto capped = folderlisting::buildListing(cappedRequest);
        check(capped.ok && capped.truncated
                  && lines(capped.listing).size()
                         == cappedRequest.maxEntries + 1,
              "the entry limit counts reparse-point lines too");
    }
}

void testClipboardPaths(const std::wstring &source)
{
    folderlisting::Request request;
    request.root = source;

    std::wstring captured;
    int calls = 0;
    const auto succeed = [&](const std::wstring &text, std::wstring *) {
        ++calls;
        captured = text;
        return true;
    };
    auto result = folderlisting::run(request, succeed);
    check(result.ok && result.exitCode == WinExit::kOk,
          "a successful copy reports success");
    check(calls == 1 && captured == result.listing,
          "the plain listing is what reaches the clipboard");

    captured.clear();
    calls = 0;
    const auto fail = [&](const std::wstring &text, std::wstring *error) {
        ++calls;
        captured = text;
        *error   = L"the clipboard was busy";
        return false;
    };
    result = folderlisting::run(request, fail);
    check(!result.ok && result.exitCode == WinExit::kFailure,
          "a failed copy reports a failure");
    check(calls == 1, "the writer is called exactly once");
    check(!captured.empty() && !result.listing.empty(),
          "the generated result is preserved after a failed copy");
    check(result.message.find(L"clipboard") != std::wstring::npos,
          "the failure message explains what happened");
    check(result.message.find(L"--max-entries") != std::wstring::npos,
          "the failure message names the options that shrink the result");
    check(result.message.find(L"selected") == std::wstring::npos,
          "the failure message does not promise text the helper never shows");
    check(result.message.size() <= 200,
          "the failure message fits the stderr budget the host actually reads");

    // A non-empty folder whose every entry is filtered out must not silently
    // succeed with a stale clipboard: the writer must never be reached and the
    // refusal must be observable.
    folderlisting::Request excludeAll = request;
    excludeAll.excludes.push_back(L"*");
    const auto filtered = folderlisting::run(excludeAll, succeed);
    check(!filtered.ok && filtered.exitCode == WinExit::kFailure && calls == 1,
          "a fully filtered folder fails without touching the clipboard");
    check(filtered.message.find(L"skipped") != std::wstring::npos,
          "the filtered-out failure names what happened");
}

void testFailurePaths(const std::wstring &root)
{
    // parseArguments no longer probes the file system: existence and type are
    // owned by buildListing so the documented exit codes stay reachable.
    const std::wstring missing = root + L"\\does-not-exist";
    check(folderlisting::parseArguments(
              makeArgv({L"x.exe", L"--input", missing.c_str()}))
              .has_value(),
          "a missing folder still parses");

    folderlisting::Request missingRequest;
    missingRequest.root = missing;
    const auto missingResult = folderlisting::buildListing(missingRequest);
    check(!missingResult.ok
              && missingResult.exitCode == WinExit::kNotFound,
          "a missing folder fails with the not-found exit code");

    folderlisting::Request empty = missingRequest;
    empty.root = root + L"\\empty-folder";
    if (makeDirectory(empty.root)) {
        int calls = 0;
        const auto writer = [&](const std::wstring &, std::wstring *) {
            ++calls;
            return true;
        };
        const auto nothing = folderlisting::run(empty, writer);
        check(nothing.ok && calls == 0,
              "an empty folder succeeds without touching the clipboard");
    }
}

struct HelperRun {
    DWORD exitCode = static_cast<DWORD>(-1);
    std::wstring standardOutput;
    std::wstring standardError;
};

std::wstring fromAcp(const char *data, DWORD size)
{
    if (size == 0) {
        return std::wstring();
    }
    const int wideLength
        = MultiByteToWideChar(CP_ACP, 0, data, static_cast<int>(size),
                              nullptr, 0);
    if (wideLength <= 0) {
        return std::wstring();
    }
    std::wstring wide(static_cast<size_t>(wideLength), L'\0');
    MultiByteToWideChar(CP_ACP, 0, data, static_cast<int>(size),
                        wide.data(), wideLength);
    return wide;
}

HelperRun runHelper(const std::wstring &helper,
                    const std::vector<std::wstring> &arguments)
{
    HelperRun run;
    SECURITY_ATTRIBUTES inheritable{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    HANDLE stdoutRead   = nullptr;
    HANDLE stdoutWrite  = nullptr;
    HANDLE stderrRead   = nullptr;
    HANDLE stderrWrite  = nullptr;
    if (!CreatePipe(&stdoutRead, &stdoutWrite, &inheritable, 0)
        || !SetHandleInformation(stdoutRead, HANDLE_FLAG_INHERIT, 0)
        || !CreatePipe(&stderrRead, &stderrWrite, &inheritable, 0)
        || !SetHandleInformation(stderrRead, HANDLE_FLAG_INHERIT, 0)) {
        return run;
    }

    std::wstring command = WinCmd::quoteArgument(helper);
    for (const auto &argument : arguments) {
        command += L" " + WinCmd::quoteArgument(argument);
    }

    STARTUPINFOW startup{sizeof(startup)};
    startup.dwFlags    = STARTF_USESTDHANDLES;
    startup.hStdOutput = stdoutWrite;
    startup.hStdError  = stderrWrite;
    PROCESS_INFORMATION process{};
    const BOOL started = CreateProcessW(nullptr, command.data(), nullptr,
                                        nullptr, TRUE, 0, nullptr, nullptr,
                                        &startup, &process);
    CloseHandle(stdoutWrite);
    CloseHandle(stderrWrite);
    if (!started) {
        CloseHandle(stdoutRead);
        CloseHandle(stderrRead);
        return run;
    }
    CloseHandle(process.hThread);

    if (WaitForSingleObject(process.hProcess, 15000) == WAIT_TIMEOUT) {
        TerminateProcess(process.hProcess, 1);
    }
    DWORD code = 0;
    GetExitCodeProcess(process.hProcess, &code);
    run.exitCode = code;
    CloseHandle(process.hProcess);

    // The helper either writes one short refusal or nothing at all, so
    // draining the pipes sequentially cannot deadlock.
    char buffer[4096]{};
    DWORD read_ = 0;
    while (ReadFile(stdoutRead, buffer, sizeof(buffer), &read_, nullptr)
           && read_ > 0) {
        run.standardOutput += fromAcp(buffer, read_);
    }
    while (ReadFile(stderrRead, buffer, sizeof(buffer), &read_, nullptr)
           && read_ > 0) {
        run.standardError += fromAcp(buffer, read_);
    }
    CloseHandle(stdoutRead);
    CloseHandle(stderrRead);
    return run;
}

void testHelperProcess(const std::wstring &helper)
{
    if (helper.empty()) {
        std::cout << "  [skip] the helper path was not provided\n";
        return;
    }
    // stderr is the only failure channel the host reads, so the usage refusal
    // must land there while stdout stays empty even on a failed start.
    const auto run = runHelper(helper, {});
    check(run.exitCode == WinExit::kUsage,
          "a helper without arguments exits with the usage code");
    check(run.standardError.find(L"--input") != std::wstring::npos,
          "the usage refusal names the required option on stderr: "
              + WinText::toUtf8(run.standardError));
    check(run.standardOutput.empty(), "stdout stays empty");
}

void testRealClipboard(const std::wstring &source)
{
    // This section takes over the real clipboard, so it runs only when nothing
    // would be lost: whatever was there before is read back first and restored,
    // and that only works for text.
    if (WinClip::holdsNonTextContent(nullptr)) {
        std::cout << "  [skip] the clipboard holds non-text content that "
                     "cannot be restored\n";
        return;
    }
    const std::wstring previous = WinClip::getText(nullptr);

    folderlisting::Request request;
    request.root = source;
    const auto result
        = folderlisting::run(request, folderlisting::systemClipboardWriter());
    check(result.ok, "the real clipboard accepts the listing: "
                         + WinText::toUtf8(result.message));

    const auto readBack = WinClip::getText(nullptr);
    if (!readBack.empty()) {
        check(readBack.find(L"Alpha/") != std::wstring::npos,
              "the clipboard holds the generated listing after the helper exits");
    }
    else {
        // Another process may have replaced the clipboard between the write and
        // the read; the write itself already succeeded.
        std::cout << "  [info] the clipboard was replaced before it could be "
                     "read back\n";
    }

    if (!previous.empty()) {
        WinClip::setText(previous, nullptr, 2000, nullptr);
    }
}

}  // namespace

int main(int argc, char *argv[])
{
    const HRESULT comResult = CoInitializeEx(
        nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);

    const auto arguments = WinCmd::commandLineArguments();
    if (UiHandoff::isMessageInvocation(arguments)) {
        const int result = UiHandoff::runMessageWorker(arguments);
        if (SUCCEEDED(comResult)) {
            CoUninitialize();
        }
        return result;
    }

    std::wstring testExecutable;
    std::wstring probeChild;
    std::wstring helperExecutable;
    if (argc > 1) {
        testExecutable = WinText::fromUtf8(argv[1]);
    }
    if (argc > 2) {
        probeChild = WinText::fromUtf8(argv[2]);
    }
    if (argc > 3) {
        helperExecutable = WinText::fromUtf8(argv[3]);
    }

    const std::wstring root   = tempRoot();
    const std::wstring source = buildFixture(root);

    TestHarness::section("copy-folder-listing: patterns");
    testPatterns();
    TestHarness::section("copy-folder-listing: argument parsing");
    testArgumentParsing(source);
    TestHarness::section("copy-folder-listing: shape and ordering");
    testShapeAndOrdering(source);
    TestHarness::section("copy-folder-listing: limits");
    testLimits(source);
    TestHarness::section("copy-folder-listing: unpaired surrogate names");
    testUnpairedSurrogateName(root);
    TestHarness::section("copy-folder-listing: reparse entries");
    testReparseEntries();
    TestHarness::section("copy-folder-listing: failure paths");
    testFailurePaths(root);
    TestHarness::section("copy-folder-listing: clipboard outcomes");
    testClipboardPaths(source);
    TestHarness::section("copy-folder-listing: real clipboard");
    testRealClipboard(source);
    TestHarness::section("copy-folder-listing: helper process");
    testHelperProcess(helperExecutable);

    CommonTests::run(testExecutable, probeChild);

    if (SUCCEEDED(comResult)) {
        CoUninitialize();
    }
    return TestHarness::summarize("folderlisting_test");
}
