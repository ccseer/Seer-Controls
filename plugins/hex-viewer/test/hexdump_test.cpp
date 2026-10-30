#include <windows.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "atomicfilewriter.h"
#include "hexdump.h"
#include "hexpaths.h"
#include "testharness.h"
#include "wincmd.h"
#include "winexit.h"
#include "winpath.h"
#include "wintext.h"

using TestHarness::check;

namespace {

namespace fs = std::filesystem;

constexpr char kMaskedFile[] = "<MASKED>";

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
    const int wideLength = MultiByteToWideChar(CP_ACP, 0, data,
                                               static_cast<int>(size), nullptr,
                                               0);
    if (wideLength <= 0) {
        return std::wstring();
    }
    std::wstring wide(static_cast<size_t>(wideLength), L'\0');
    MultiByteToWideChar(CP_ACP, 0, data, static_cast<int>(size), wide.data(),
                        wideLength);
    return wide;
}

// Same shape as the shared folderlisting test launcher: double pipe, inherit,
// bounded wait, sequential drain. The helper writes at most one short refusal,
// so draining after the wait cannot deadlock.
HelperRun runHelper(const std::wstring &helper,
                    const std::vector<std::wstring> &arguments)
{
    HelperRun run;
    SECURITY_ATTRIBUTES inheritable{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    HANDLE stdoutRead = nullptr;
    HANDLE stdoutWrite = nullptr;
    HANDLE stderrRead = nullptr;
    HANDLE stderrWrite = nullptr;
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
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdOutput = stdoutWrite;
    startup.hStdError = stderrWrite;
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

    if (WaitForSingleObject(process.hProcess, 30000) == WAIT_TIMEOUT) {
        TerminateProcess(process.hProcess, 1);
    }
    DWORD code = 0;
    GetExitCodeProcess(process.hProcess, &code);
    run.exitCode = code;
    CloseHandle(process.hProcess);

    char buffer[4096]{};
    DWORD read = 0;
    while (ReadFile(stdoutRead, buffer, sizeof(buffer), &read, nullptr)
           && read > 0) {
        run.standardOutput += fromAcp(buffer, read);
    }
    while (ReadFile(stderrRead, buffer, sizeof(buffer), &read, nullptr)
           && read > 0) {
        run.standardError += fromAcp(buffer, read);
    }
    CloseHandle(stdoutRead);
    CloseHandle(stderrRead);
    return run;
}

fs::path sandboxDir()
{
    return fs::current_path() / "test-sandbox";
}

std::vector<uint8_t> patternBytes(size_t count, uint8_t seed)
{
    std::vector<uint8_t> bytes(count);
    for (size_t index = 0; index < count; ++index) {
        bytes[index] = static_cast<uint8_t>((seed + index) & 0xFF);
    }
    return bytes;
}

void writeBinary(const fs::path &path, const std::vector<uint8_t> &bytes)
{
    std::ofstream out(path, std::ios::binary);
    out.write(reinterpret_cast<const char *>(bytes.data()),
              static_cast<std::streamsize>(bytes.size()));
}

std::string readFileBytes(const fs::path &path)
{
    std::ifstream in(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)),
                       std::istreambuf_iterator<char>());
}

void deleteFileQuietly(const std::wstring &path)
{
    const DWORD attributes = WinPath::attributesOf(path);
    if (attributes != INVALID_FILE_ATTRIBUTES
        && (attributes & FILE_ATTRIBUTE_READONLY) != 0) {
        SetFileAttributesW(WinPath::addExtendedPrefix(path).c_str(),
                           attributes & ~FILE_ATTRIBUTE_READONLY);
    }
    DeleteFileW(WinPath::addExtendedPrefix(path).c_str());
}

// Independent test oracle for one dump row, written straight from the spec's
// column contract: offset at 0..7, first byte at 13, the 7/8 double gap, and
// the decoded block at columns 65..82.
std::string oracleRow(uint64_t offset, const uint8_t *bytes, size_t count,
                      bool upper)
{
    static const char kUp[] = "0123456789ABCDEF";
    static const char kDown[] = "0123456789abcdef";
    const char *digits = upper ? kUp : kDown;
    std::string line;
    for (int shift = 28; shift >= 0; shift -= 4) {
        line += digits[(offset >> shift) & 0xF];
    }
    line += "     ";
    for (size_t index = 0; index < 16; ++index) {
        if (index < count) {
            const uint8_t value = bytes[index];
            line += digits[(value >> 4) & 0xF];
            line += digits[value & 0xF];
        }
        else {
            line += "  ";
        }
        if (index < 15) {
            line += ' ';
            if (index == 7) {
                line += ' ';
            }
        }
    }
    line += "    ";
    line += '|';
    for (size_t index = 0; index < 16; ++index) {
        if (index < count) {
            const uint8_t value = bytes[index];
            line += (value >= 32 && value <= 126) ? static_cast<char>(value)
                                                  : '.';
        }
        else {
            line += ' ';
        }
    }
    line += "|";
    line += "\r\n";
    return line;
}

std::string goldenColumnHeader(bool upper)
{
    return upper
               ? "Offset(h)    00 01 02 03 04 05 06 07  08 09 0A 0B 0C 0D 0E 0F    "
                 "Decoded Text\r\n"
               : "Offset(h)    00 01 02 03 04 05 06 07  08 09 0a 0b 0c 0d 0e 0f    "
                 "Decoded Text\r\n";
}

std::string goldenFooterFullyDisplayed(const std::string &totalText)
{
    const std::string dashes(83, '-');
    return dashes + "\r\n[End of File: Fully displayed] (Total: " + totalText
           + ")\r\n" + dashes + "\r\n";
}

std::string goldenFooterTruncated(long long maxKb, const std::string &totalText)
{
    const std::string dashes(83, '-');
    return dashes + "\r\n[Truncated] Display capped at " + std::to_string(maxKb)
           + " KB to ensure instant loading. (Total: " + totalText + ")\r\n"
           + dashes + "\r\n";
}

// Assembles the expected dump text from the hardcoded header/footer literals
// of the test case plus the oracle rows. The File: value is the mask.
std::string goldenFor(const std::string &sizeLine, const std::string &rangeLine,
                      const std::string &footerText,
                      const std::vector<uint8_t> &dumpedBytes, bool upper)
{
    const std::string rule(83, '=');
    std::string out = rule + "\r\n[Seer Hex Dump]\r\nFile:      "
                      + std::string(kMaskedFile) + "\r\n";
    out += "Size:      " + sizeLine + "\r\n";
    out += "Range:     " + rangeLine + "\r\n";
    out += rule + "\r\n";
    if (!dumpedBytes.empty()) {
        out += "\r\n";
        out += goldenColumnHeader(upper);
        for (size_t offset = 0; offset < dumpedBytes.size(); offset += 16) {
            out += oracleRow(offset, dumpedBytes.data() + offset,
                             std::min<size_t>(16, dumpedBytes.size() - offset),
                             upper);
        }
        out += "\r\n";
    }
    out += footerText;
    return out;
}

std::string maskFileLine(const std::string &text)
{
    const std::string marker = "File:      ";
    const size_t begin = text.find(marker);
    if (begin == std::string::npos) {
        return text;
    }
    const size_t end = text.find("\r\n", begin);
    if (end == std::string::npos) {
        return text;
    }
    return text.substr(0, begin) + marker + std::string(kMaskedFile)
           + text.substr(end);
}

void checkNoBomAndCrlf(const std::string &content)
{
    check(!(content.size() >= 3 && static_cast<unsigned char>(content[0]) == 0xEF
            && static_cast<unsigned char>(content[1]) == 0xBB
            && static_cast<unsigned char>(content[2]) == 0xBF),
          "the output carries no BOM");
    for (size_t index = 0; index < content.size(); ++index) {
        if (content[index] == '\n') {
            if (index == 0 || content[index - 1] != '\r') {
                check(false, "every LF is preceded by CR");
                return;
            }
        }
    }
    check(true, "every LF is preceded by CR");
}

struct GenerateOutcome {
    bool ok = false;
    bool published = false;
    std::string content;
    std::wstring error;
};

GenerateOutcome runGenerate(const fs::path &input, const fs::path &txt,
                            long long maxKb, bool upper,
                            const hexviewer::ReadProcedure &reader = nullptr)
{
    hexviewer::HexDumpRequest request;
    request.inputPath = input.wstring();
    request.txtPath = txt.wstring();
    request.tmpPath = txt.wstring() + L".gen.tmp";
    request.maxKb = maxKb;
    request.upper = upper;
    const auto result = hexviewer::generate(request, reader);
    GenerateOutcome outcome;
    outcome.ok = result.ok;
    outcome.error = result.errorMessage;
    outcome.published = fs::exists(txt);
    if (outcome.published) {
        outcome.content = readFileBytes(txt);
    }
    return outcome;
}

void testDirectGolden(const fs::path &sandbox)
{
    TestHarness::section("direct generate vs golden text");

    // 512 bytes: fully displayed, range derived from the real size.
    {
        const fs::path input = sandbox / "small.bin";
        const auto data = patternBytes(512, 0);
        writeBinary(input, data);
        for (const bool upper : {true, false}) {
            const fs::path txt = sandbox / (upper ? "small.hextxt"
                                                  : "small-lower.hextxt");
            const auto outcome = runGenerate(input, txt, 64, upper);
            check(outcome.ok, "the 512-byte dump generates (upper="
                                  + std::to_string(upper) + ")");
            check(outcome.published, "the 512-byte dump is published");
            const std::string masked = maskFileLine(outcome.content);
            const std::string footer = goldenFooterFullyDisplayed("512 bytes");
            const std::string range = upper
                ? "Showing all 512 bytes (0x00000000 - 0x000001FF)"
                : "Showing all 512 bytes (0x00000000 - 0x000001ff)";
            check(masked == goldenFor("512 bytes", range, footer, data, upper),
                  "the 512-byte dump matches the golden text (upper="
                      + std::to_string(upper) + ")");
            checkNoBomAndCrlf(outcome.content);
        }
    }

    // Exactly 64 KB: fully displayed, not truncated.
    {
        const fs::path input = sandbox / "exact64k.bin";
        const auto data = patternBytes(65536, 2);
        writeBinary(input, data);
        const fs::path txt = sandbox / "exact64k.hextxt";
        const auto outcome = runGenerate(input, txt, 64, true);
        check(outcome.ok && outcome.published, "the 64 KB dump generates");
        const std::string masked = maskFileLine(outcome.content);
        check(masked
                  == goldenFor("65,536 bytes (64.00 KB)",
                               "Showing all 65,536 bytes (0x00000000 - 0x0000FFFF)",
                               goldenFooterFullyDisplayed("65,536 bytes"), data,
                               true),
              "a file exactly at the cap matches the golden text");
        check(masked.find("[Truncated]") == std::string::npos,
              "a file exactly at the cap is not marked truncated");
    }

    // 64 KB + 1: truncated to the cap.
    {
        const fs::path input = sandbox / "over64k.bin";
        const auto data = patternBytes(65537, 4);
        writeBinary(input, data);
        const fs::path txt = sandbox / "over64k.hextxt";
        const auto outcome = runGenerate(input, txt, 64, true);
        check(outcome.ok && outcome.published, "the 64 KB + 1 dump generates");
        const std::string masked = maskFileLine(outcome.content);
        const std::vector<uint8_t> dumped(data.begin(), data.begin() + 65536);
        check(masked
                  == goldenFor("65,537 bytes (64.00 KB)",
                               "Showing first 64 KB (0x00000000 - 0x0000FFFF)",
                               goldenFooterTruncated(64, "64.00 KB"), dumped,
                               true),
              "a file above the cap matches the truncated golden text");
        check(masked.find("00010000   ") == std::string::npos,
              "no row is emitted past the cap");
    }

    // Empty file: header plus the empty marker, no column header.
    {
        const fs::path input = sandbox / "empty.bin";
        writeBinary(input, {});
        const fs::path txt = sandbox / "empty.hextxt";
        const auto outcome = runGenerate(input, txt, 64, true);
        check(outcome.ok && outcome.published, "the empty dump generates");
        const std::string masked = maskFileLine(outcome.content);
        const std::string rule(83, '=');
        const std::string expected = rule
                                     + "\r\n[Seer Hex Dump]\r\nFile:      "
                                     + std::string(kMaskedFile) + "\r\n"
                                     + "Size:      0 bytes\r\n"
                                     + "Range:     Showing 0 bytes\r\n" + rule
                                     + "\r\n\r\n[Empty File: 0 Bytes]\r\n";
        check(masked == expected, "the empty dump matches the spec sample");
        check(outcome.content.find("Offset(h)") == std::string::npos,
              "the empty dump has no column header");
    }

    // Unicode and space-laden paths open and display correctly.
    {
        const fs::path dir = sandbox / L"中文 dir" / L"日本語 フォルダ";
        fs::create_directories(dir);
        const fs::path input = dir / L"файл.bin";
        const auto data = patternBytes(48, 11);
        writeBinary(input, data);
        const fs::path txt = sandbox / "unicode.hextxt";
        const auto outcome = runGenerate(input, txt, 64, true);
        check(outcome.ok && outcome.published,
              "a unicode path generates its dump");
        check(outcome.content.find(WinText::toUtf8(input.wstring()))
                  != std::string::npos,
              "the File line shows the unicode path");
        check(outcome.content.find(goldenFooterFullyDisplayed("48 bytes"))
                  != std::string::npos,
              "the unicode dump ends with the fully-displayed footer");
        checkNoBomAndCrlf(outcome.content);
    }
}

void testInjectedReadFailure(const fs::path &sandbox)
{
    TestHarness::section("injected mid-read failure");

    const fs::path input = sandbox / "readfail.bin";
    const auto data = patternBytes(8192, 1);
    writeBinary(input, data);
    const fs::path txt = sandbox / "readfail.hextxt";

    int calls = 0;
    hexviewer::ReadProcedure failing
        = [&data, &calls](uint8_t *destination, size_t bytesToRead,
                          size_t *bytesRead, DWORD *errorCode) -> bool {
        ++calls;
        if (calls == 1) {
            const size_t give = std::min<size_t>(bytesToRead, 2048);
            std::memcpy(destination, data.data(), give);
            *bytesRead = give;
            return true;
        }
        *errorCode = ERROR_ACCESS_DENIED;
        return false;
    };

    const auto outcome = runGenerate(input, txt, 4, true, failing);
    check(calls == 2, "the reader was consulted twice");
    check(outcome.ok, "a mid-read failure still publishes the dump");
    check(outcome.published, "the partial dump is published");
    check(outcome.content.find("[Read Error] Stopped at 0x00000800: access "
                               "denied\r\n")
              != std::string::npos,
          "the read-error footer states the stop offset and reason");
    check(outcome.content.find("[Truncated]") == std::string::npos
              && outcome.content.find("[End of File") == std::string::npos,
          "the read error replaces the normal footer");
    check(outcome.content.find("000007F0     ") != std::string::npos
              && outcome.content.find("00000800     ") == std::string::npos,
          "rows read before the failure are kept, later rows are absent");
    check(outcome.content.find("Showing first 4 KB (0x00000000 - 0x00000FFF)\r\n")
              != std::string::npos,
          "the header still shows the pre-read range prediction");
    checkNoBomAndCrlf(outcome.content);
}

void testRenameFailure(const fs::path &sandbox)
{
    TestHarness::section("held target blocks the atomic publish");

    const fs::path input = sandbox / "rename.bin";
    writeBinary(input, patternBytes(32, 5));
    const fs::path txt = sandbox / "rename.hextxt";
    HANDLE held = CreateFileW(
        WinPath::addExtendedPrefix(txt.wstring()).c_str(),
        GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL, nullptr);
    check(held != INVALID_HANDLE_VALUE, "the target fixture is held");

    const auto outcome = runGenerate(input, txt, 1, true);
    check(!outcome.ok, "a held target fails the atomic publish");
    check(outcome.error.find(L"MoveFileExW failed") != std::wstring::npos,
          "the error names the failed rename");
    check(!fs::exists(fs::path(txt.wstring() + L".gen.tmp")),
          "no staging file remains after the failure");
    CloseHandle(held);
    deleteFileQuietly(txt.wstring());
}

void testWriteDirect(const fs::path &sandbox)
{
    TestHarness::section("direct write fallback");

    const fs::path txt = sandbox / "direct.hextxt";
    writeBinary(txt, {'o', 'l', 'd'});
    const std::string diagnostic =
        "[Seer Hex Dump Error] Cannot write dump file (test reason).\r\n";

    // Write access is shared even when DELETE is not, so the direct write
    // succeeds where the rename could not.
    HANDLE held = CreateFileW(
        WinPath::addExtendedPrefix(txt.wstring()).c_str(),
        GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL, nullptr);
    check(held != INVALID_HANDLE_VALUE, "the target is held without share-delete");
    const auto direct = hexviewer::AtomicFileWriter::writeDirect(
        txt.wstring(), diagnostic);
    check(direct.ok, "the direct write succeeds while only the rename is blocked");
    check(readFileBytes(txt) == diagnostic, "the diagnostic line replaced the content");
    CloseHandle(held);

    // An exclusive hold blocks the direct write too, reported without UI.
    HANDLE exclusive = CreateFileW(
        WinPath::addExtendedPrefix(txt.wstring()).c_str(),
        GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL, nullptr);
    check(exclusive != INVALID_HANDLE_VALUE, "the target is held exclusively");
    const auto blocked = hexviewer::AtomicFileWriter::writeDirect(
        txt.wstring(), diagnostic);
    check(!blocked.ok && !blocked.errorMessage.empty(),
          "an exclusive hold fails the direct write with a message");
    CloseHandle(exclusive);
    deleteFileQuietly(txt.wstring());
}

void testInjectedReadFailureLowercase(const fs::path &sandbox)
{
    TestHarness::section("injected mid-read failure, lowercase");

    const fs::path input = sandbox / "readfail-lower.bin";
    const auto data = patternBytes(65536, 2);
    writeBinary(input, data);
    const fs::path txt = sandbox / "readfail-lower.hextxt";

    // 0xABC0 bytes are handed over before the failure, so the stop offset
    // contains hex letters and the digit case is observable.
    int calls = 0;
    hexviewer::ReadProcedure failing
        = [&data, &calls](uint8_t *destination, size_t bytesToRead,
                          size_t *bytesRead, DWORD *errorCode) -> bool {
        ++calls;
        if (calls == 1) {
            const size_t give = std::min<size_t>(bytesToRead, 0xABC0);
            std::memcpy(destination, data.data(), give);
            *bytesRead = give;
            return true;
        }
        *errorCode = ERROR_ACCESS_DENIED;
        return false;
    };

    const auto outcome = runGenerate(input, txt, 64, false, failing);
    check(outcome.ok, "a mid-read failure still publishes the lowercase dump");
    check(outcome.content.find(
              "[Read Error] Stopped at 0x0000abc0: access denied\r\n")
              != std::string::npos,
          "the read-error offset follows the dump digit case");
    check(outcome.content.find("0x0000ffff") != std::string::npos,
          "the header range is lowercase as well");
    checkNoBomAndCrlf(outcome.content);
}

void testWriterScopeExit(const fs::path &sandbox)
{
    TestHarness::section("AtomicFileWriter scope exit");

    const fs::path tmp = sandbox / "scope.tmp";
    deleteFileQuietly(tmp.wstring());
    std::wstring error;
    {
        hexviewer::AtomicFileWriter writer;
        check(writer.begin(tmp.wstring(), &error), "the staging file opens");
        check(writer.write("abc", 3, &error), "the staging file accepts bytes");
    }
    // The handle is opened without sharing, so the delete only succeeds when the
    // destructor closed it first; a leaked handle fails this assertion.
    check(!WinPath::isRegularFile(tmp.wstring()),
          "leaving the scope closes the handle and removes the staging file");
}

void testWriterBeginRefusal(const fs::path &sandbox)
{
    TestHarness::section("AtomicFileWriter refuses an occupied staging path");

    const fs::path tmp = sandbox / "occupied.tmp";
    writeBinary(tmp, {'k', 'e', 'e', 'p'});
    std::wstring error;
    {
        hexviewer::AtomicFileWriter writer;
        check(!writer.begin(tmp.wstring(), &error),
              "CREATE_NEW fails when the staging path already exists");
        check(!error.empty(), "the refusal carries a message");
    }
    // The failure did not create the file, so the scope exit must not delete it.
    check(readFileBytes(tmp) == "keep",
          "a failed begin leaves the existing file untouched");
}

std::wstring expectedDumpPath(const std::wstring &helper, const fs::path &input)
{
    const std::wstring exeDirectory = WinPath::parentDirectory(helper);
    return WinPath::join(WinPath::join(exeDirectory, L"dumps"),
                         hexviewer::outputFileName(input.wstring(), 64));
}

// The helper launches the viewer detached and exits without waiting for it, so
// the capture file appears asynchronously; poll briefly instead of asserting
// immediately.
bool waitForFile(const std::wstring &path, DWORD timeoutMs)
{
    const DWORD deadline = GetTickCount() + timeoutMs;
    while (GetTickCount() < deadline) {
        if (WinPath::isRegularFile(path)) {
            return true;
        }
        Sleep(25);
    }
    return WinPath::isRegularFile(path);
}

void cleanDumps(const std::wstring &helper)
{
    const std::wstring exeDirectory = WinPath::parentDirectory(helper);
    std::error_code code;
    fs::remove_all(WinPath::join(exeDirectory, L"dumps"), code);
}

void testEndToEnd(const std::wstring &helper, const std::wstring &fakeViewer,
                  const fs::path &sandbox)
{
    TestHarness::section("end-to-end run with the fake viewer");

    cleanDumps(helper);
    const fs::path input = sandbox / "e2e.bin";
    writeBinary(input, patternBytes(300, 7));
    const std::wstring expectedTxt = expectedDumpPath(helper, input);

    const auto run = runHelper(helper, {input.wstring(), fakeViewer});
    check(run.exitCode == WinExit::kOk,
          "the helper exits 0 on the happy path (stderr: "
              + WinText::toUtf8(run.standardError) + ")");
    check(WinPath::isRegularFile(expectedTxt),
          "the dump file appeared at the derived location");

    const std::wstring capture = expectedTxt + L".fakeviewercapture.txt";
    check(waitForFile(capture, 4000), "the fake viewer recorded its argv");
    const auto lines = WinText::splitLines(
        WinText::fromUtf8(readFileBytes(fs::path(capture))));
    check(lines.size() == 4 && lines[0] == L"--wait-file"
              && lines[1] == L"--wait-timeout=30000" && lines[2] == L"--"
              && lines[3] == expectedTxt,
          "the viewer receives the pending contract arguments");

    std::vector<WinPath::DirectoryEntry> entries;
    WinPath::enumerateDirectory(WinPath::parentDirectory(expectedTxt), &entries,
                                nullptr);
    bool leftovers = false;
    for (const auto &entry : entries) {
        leftovers = leftovers
                    || WinText::toLower(entry.name).size() > 4
                           && WinText::toLower(entry.name).compare(
                                  WinText::toLower(entry.name).size() - 4, 4,
                                  L".tmp")
                                  == 0;
    }
    check(!leftovers, "no staging file remains after the run");

    deleteFileQuietly(expectedTxt);
    deleteFileQuietly(capture);
}

void testLockedInputEndToEnd(const std::wstring &helper,
                             const std::wstring &fakeViewer,
                             const fs::path &sandbox)
{
    TestHarness::section("locked input end-to-end");

    cleanDumps(helper);
    const fs::path input = sandbox / "locked.bin";
    writeBinary(input, patternBytes(64, 3));
    HANDLE held = CreateFileW(
        WinPath::addExtendedPrefix(input.wstring()).c_str(), GENERIC_READ, 0,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    check(held != INVALID_HANDLE_VALUE, "the fixture is locked exclusively");

    const std::wstring expectedTxt = expectedDumpPath(helper, input);
    const auto run = runHelper(helper, {input.wstring(), fakeViewer});
    check(run.exitCode == WinExit::kOk,
          "a locked input still exits 0 with a diagnostic dump");
    check(WinPath::isRegularFile(expectedTxt),
          "the diagnostic dump is published for the waiting viewer");
    const std::string content = readFileBytes(fs::path(expectedTxt));
    check(content.find("[Seer Hex Dump Error]") != std::string::npos,
          "the diagnostic dump carries the error block");
    check(content.find("Cannot open file for reading (file is locked by "
                       "another process)\r\n")
              != std::string::npos,
          "the lock maps to the fixed reason text");

    CloseHandle(held);
    deleteFileQuietly(expectedTxt);
}

void testLongNameEndToEnd(const std::wstring &helper,
                          const std::wstring &fakeViewer,
                          const fs::path &sandbox)
{
    TestHarness::section("255-character file name end-to-end");

    cleanDumps(helper);
    const fs::path dir = sandbox / "longname";
    fs::create_directories(dir);
    std::wstring longName;
    for (int index = 0; index < 255; ++index) {
        longName += static_cast<wchar_t>(L'f' + (index % 20));
    }
    const std::wstring plainPath = (dir / fs::path(longName)).wstring();
    // A leftover from an interrupted run would fail CREATE_NEW, and
    // std::filesystem's own removal cannot reach a path this long, so the
    // fixture is cleared through the extended prefix first.
    deleteFileQuietly(plainPath);
    HANDLE created = CreateFileW(
        WinPath::addExtendedPrefix(plainPath).c_str(), GENERIC_WRITE, 0,
        nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    check(created != INVALID_HANDLE_VALUE,
          "the 255-character fixture is created through the extended prefix");
    if (created != INVALID_HANDLE_VALUE) {
        const char payload[] = "long name payload";
        DWORD written = 0;
        WriteFile(created, payload, sizeof(payload) - 1, &written, nullptr);
        CloseHandle(created);
    }

    const std::wstring expectedTxt = expectedDumpPath(helper, plainPath);
    const auto run = runHelper(helper, {plainPath, fakeViewer});
    check(run.exitCode == WinExit::kOk,
          "the long-name run exits 0 (stderr: "
              + WinText::toUtf8(run.standardError) + ")");
    check(WinPath::isRegularFile(expectedTxt),
          "the long-name input produces its dump at the capped name");
    const std::string content = readFileBytes(fs::path(expectedTxt));
    check(content.find(WinText::toUtf8(longName)) != std::string::npos,
          "the File line keeps the full long name");
    check(content.find(goldenFooterFullyDisplayed("17 bytes")) != std::string::npos,
          "the long-name dump carries the fully-displayed footer");

    const std::wstring capture = expectedTxt + L".fakeviewercapture.txt";
    check(waitForFile(capture, 4000), "the fake viewer recorded its argv");
    const auto lines = WinText::splitLines(
        WinText::fromUtf8(readFileBytes(fs::path(capture))));
    check(lines.size() == 4 && lines[3] == expectedTxt,
          "the viewer is pointed at the same capped path");

    deleteFileQuietly(expectedTxt);
    deleteFileQuietly(capture);
    // std::filesystem cannot remove a path this long; clear it explicitly so
    // the end-of-run sandbox removal can succeed.
    deleteFileQuietly(plainPath);
}

void testHeldTargetRefused(const std::wstring &helper,
                           const std::wstring &fakeViewer,
                           const fs::path &sandbox)
{
    TestHarness::section("previous dump held by another process");

    cleanDumps(helper);
    const fs::path input = sandbox / "heldtarget.bin";
    writeBinary(input, patternBytes(64, 5));
    const std::wstring expectedTxt = expectedDumpPath(helper, input);
    std::error_code code;
    fs::create_directories(fs::path(expectedTxt).parent_path(), code);
    writeBinary(fs::path(expectedTxt), patternBytes(32, 6));

    // Held without FILE_SHARE_DELETE: neither DeleteFileW nor the later
    // MoveFileExW/direct write can replace it, so the helper must refuse
    // before it opens a window onto the previous dump.
    HANDLE held = CreateFileW(
        WinPath::addExtendedPrefix(expectedTxt).c_str(), GENERIC_READ, 0,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    check(held != INVALID_HANDLE_VALUE,
          "the previous dump is held without delete sharing");

    const auto run = runHelper(helper, {input.wstring(), fakeViewer});
    check(run.exitCode == WinExit::kFailure,
          "an unremovable previous dump exits with the failure code");
    check(run.standardError.find(L"Cannot replace the previous hex dump")
              != std::wstring::npos,
          "the refusal names the held dump on stderr");
    check(!WinPath::exists(expectedTxt + L".fakeviewercapture.txt"),
          "the viewer is never launched onto the previous dump");

    CloseHandle(held);
    deleteFileQuietly(expectedTxt);
}

void testNoUiRefusals(const std::wstring &helper, const fs::path &sandbox)
{
    TestHarness::section("SEER_TEST_NO_UI refusals");

    // The variable was set for this process at startup, so every child inherits
    // it and all refusals land on stderr instead of a message box.
    const fs::path input = sandbox / "noui.bin";
    writeBinary(input, patternBytes(16, 9));

    const auto missingViewer = runHelper(
        helper, {input.wstring(), L"C:\\definitely\\missing\\SeerViewer.exe"});
    check(missingViewer.exitCode == WinExit::kFailure,
          "a missing viewer exits with the failure code");
    check(missingViewer.standardError.find(
              L"SeerViewer executable not found at specified path.")
              != std::wstring::npos,
          "the missing-viewer refusal explains itself on stderr");

    const auto badOption = runHelper(helper, {input.wstring(), L"viewer", L"--bogus"});
    check(badOption.exitCode == WinExit::kUsage,
          "an unknown option exits with the usage code");
    check(badOption.standardOutput.empty(), "stdout stays empty");
    check(badOption.standardError.find(L"Invalid arguments") != std::wstring::npos,
          "the usage refusal lands on stderr");

    const auto badNumber
        = runHelper(helper, {L"--max-kb=abc", input.wstring(), L"viewer"});
    check(badNumber.exitCode == WinExit::kUsage, "a non-numeric value exits 2");
    check(!badNumber.standardError.empty(), "the non-numeric refusal lands on stderr");
}

}  // namespace

int main()
{
    // Wide argv straight from GetCommandLineW: decoding through the active code
    // page would corrupt a build path that contains characters outside it. The
    // ACP helper above stays, because the helper's own stderr is ACP-encoded.
    const std::vector<std::wstring> arguments = WinCmd::commandLineArguments();
    if (arguments.size() != 3) {
        std::cerr << "usage: hexdump_test <seer_hex.exe> <fake_viewer.exe>\n";
        return 2;
    }
    const std::wstring &helper = arguments[1];
    const std::wstring &fakeViewer = arguments[2];

    // Every refusal in this run must reach stderr instead of a modal box.
    SetEnvironmentVariableW(L"SEER_TEST_NO_UI", L"1");

    const fs::path sandbox = sandboxDir();
    std::error_code code;
    fs::remove_all(sandbox, code);
    fs::create_directories(sandbox);

    testDirectGolden(sandbox);
    testInjectedReadFailure(sandbox);
    testInjectedReadFailureLowercase(sandbox);
    testRenameFailure(sandbox);
    testWriteDirect(sandbox);
    testWriterScopeExit(sandbox);
    testWriterBeginRefusal(sandbox);
    testEndToEnd(helper, fakeViewer, sandbox);
    testLockedInputEndToEnd(helper, fakeViewer, sandbox);
    testHeldTargetRefused(helper, fakeViewer, sandbox);
    testLongNameEndToEnd(helper, fakeViewer, sandbox);
    testNoUiRefusals(helper, sandbox);

    fs::remove_all(sandbox, code);
    cleanDumps(helper);
    return TestHarness::summarize("hexdump_test");
}
