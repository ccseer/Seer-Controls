#include <cstdint>
#include <string>
#include <vector>

#include "hexformat.h"
#include "hexoptions.h"
#include "testharness.h"

using TestHarness::check;

namespace {

const std::vector<uint8_t> kMzBytes = {
    0x4D, 0x5A, 0x90, 0x00, 0x03, 0x00, 0x00, 0x00,
    0x04, 0x00, 0x00, 0x00, 0xFF, 0xFF, 0x00, 0x00};

// The spec sample row for these bytes, verbatim.
const char kFullRow[] =
    "00000000     4D 5A 90 00 03 00 00 00  04 00 00 00 FF FF 00 00    |MZ..............|";

std::string rowOf(const std::vector<uint8_t> &bytes, uint64_t offset, bool upper)
{
    std::string buffer;
    hexviewer::appendLine(buffer, offset, bytes.data(), bytes.size(), upper);
    return buffer;
}

void testAppendLine()
{
    TestHarness::section("appendLine");

    const std::string row = rowOf(kMzBytes, 0, true);
    check(row == std::string(kFullRow) + "\r\n",
          "a full 16-byte row matches the spec layout byte for byte");
    check(row.size() == 85, "each row is 83 visible characters plus CRLF");

    struct PaddingCase {
        size_t count;
        std::string head;
        std::string ascii;
    };
    const std::vector<PaddingCase> cases = {
        {1, "00000000     4D", "M"},
        {7, "00000000     4D 5A 90 00 03 00 00", "MZ....."},
        {8, "00000000     4D 5A 90 00 03 00 00 00", "MZ......"},
        {15, "00000000     4D 5A 90 00 03 00 00 00  04 00 00 00 FF FF 00",
         "MZ............."},
    };
    for (const auto &item : cases) {
        const std::vector<uint8_t> bytes(kMzBytes.begin(),
                                         kMzBytes.begin() + static_cast<size_t>(item.count));
        const std::string line = rowOf(bytes, 0, true).substr(0, 83);
        check(line.size() == 83, "padded rows stay 83 characters wide");
        check(line.substr(0, item.head.size()) == item.head,
              "hex head of " + std::to_string(item.count) + " bytes");
        bool middleBlank = true;
        for (size_t index = item.head.size(); index < 65u; ++index) {
            middleBlank = middleBlank && line[index] == ' ';
        }
        check(middleBlank, "padding fills the hex area up to column 65");
        check(line[65] == '|' && line[82] == '|',
              "the decoded block stays at columns 65..82");
        check(line.substr(66, 16)
                      == item.ascii + std::string(16 - item.ascii.size(), ' '),
              "ascii of " + std::to_string(item.count)
                  + " bytes is padded to 16 inside the pipes");
    }

    // The double gap between byte positions 7 and 8 sits at fixed columns even
    // when both bytes are padding.
    const std::string single = rowOf({0x41}, 0, true).substr(0, 83);
    check(single[34] == ' ' && single[35] == ' ' && single[36] == ' '
              && single[37] == ' ' && single[38] == ' ' && single[39] == ' ',
          "the 7/8 gap columns stay blank on padded rows");

    const std::vector<uint8_t> mixed = {
        0x00, 0x01, 0x09, 0x0A, 0x0D, 0x1F, 0x20, 0x41,
        0x7E, 0x7F, 0x80, 0xFF, 0x41, 0x41, 0x41, 0x41};
    const std::string mixedLine = rowOf(mixed, 0, true).substr(0, 83);
    check(mixedLine.substr(66, 16) == "...... A~...AAAA",
          "bytes below 32 or above 126 render as dots, printables survive");
    check(mixedLine.find_first_of(std::string("\x00\x01\x09\x0A\x0D", 5))
              == std::string::npos,
          "control bytes never reach the output");

    const std::string upper = rowOf(kMzBytes, 0x0000FFF0, true).substr(0, 83);
    check(upper.substr(0, 8) == "0000FFF0",
          "the offset renders as 8 uppercase hex digits");
    const std::string lower = rowOf(kMzBytes, 0x0000FFF0, false).substr(0, 83);
    check(lower.substr(0, 8) == "0000fff0",
          "the offset case follows the dump case");
    check(lower.substr(13, 23) == "4d 5a 90 00 03 00 00 00",
          "the byte case follows the dump case");
}

void testColumnHeader()
{
    TestHarness::section("formatColumnHeader");

    const std::string upper = hexviewer::formatColumnHeader(true);
    check(upper
              == "Offset(h)    00 01 02 03 04 05 06 07  08 09 0A 0B 0C 0D 0E 0F    "
                 "Decoded Text\r\n",
          "the upper column header matches the spec line");
    check(upper.size() == 79, "the column header is 77 characters plus CRLF");
    check(upper.find("Decoded Text") == 65, "Decoded Text starts at column 65");

    const std::string lower = hexviewer::formatColumnHeader(false);
    check(lower
              == "Offset(h)    00 01 02 03 04 05 06 07  08 09 0a 0b 0c 0d 0e 0f    "
                 "Decoded Text\r\n",
          "the lower column header matches the data case");
}

void testHeader()
{
    TestHarness::section("formatHeader");

    const std::string rule(83, '=');
    const std::string truncated = hexviewer::formatHeader(
        L"D:\\workspace\\firmware\\bootloader.bin", 2457600, 65536, 64, true);
    check(truncated
              == rule + "\r\n[Seer Hex Dump]\r\n"
                        "File:      D:\\workspace\\firmware\\bootloader.bin\r\n"
                        "Size:      2,457,600 bytes (2.34 MB)\r\n"
                        "Range:     Showing first 64 KB (0x00000000 - 0x0000FFFF)\r\n"
                  + rule + "\r\n",
          "the truncated header matches the spec sample");

    const std::string full = hexviewer::formatHeader(L"C:\\a.bin", 65536, 65536, 64, true);
    check(full.find("Size:      65,536 bytes (64.00 KB)\r\n") != std::string::npos,
          "a file exactly at the cap reports its real size");
    check(full.find("Range:     Showing all 65,536 bytes (0x00000000 - 0x0000FFFF)\r\n")
              != std::string::npos,
          "a file exactly at the cap is 'Showing all'");

    const std::string smallFile = hexviewer::formatHeader(L"C:\\a.bin", 512, 512, 64, true);
    check(smallFile.find("Range:     Showing all 512 bytes (0x00000000 - 0x000001FF)\r\n")
              != std::string::npos,
          "a small file derives its range from its real size");

    const std::string empty = hexviewer::formatHeader(L"C:\\a.bin", 0, 0, 64, true);
    check(empty.find("Size:      0 bytes\r\n") != std::string::npos,
          "an empty file reports 0 bytes without a unit suffix");
    check(empty.find("Range:     Showing 0 bytes\r\n") != std::string::npos,
          "an empty file avoids the 0 - 1 underflow");

    const std::string lower = hexviewer::formatHeader(L"C:\\a.bin", 65537, 65536, 64, false);
    check(lower.find("Range:     Showing first 64 KB (0x00000000 - 0x0000ffff)\r\n")
              != std::string::npos,
          "the range case follows the dump case");

    const std::string kb128 = hexviewer::formatHeader(L"C:\\a.bin", 131073, 131072, 128, true);
    check(kb128.find("Range:     Showing first 128 KB (0x00000000 - 0x0001FFFF)\r\n")
              != std::string::npos,
          "the range text follows the effective --max-kb");
}

void testSizeFormatting()
{
    TestHarness::section("size formatting");

    using hexviewer::formatGroupedBytes;
    using hexviewer::formatSize;
    using hexviewer::formatUnitSize;

    check(formatGroupedBytes(0) == "0 bytes", "grouped zero");
    check(formatGroupedBytes(1024) == "1,024 bytes", "grouped thousands");
    check(formatGroupedBytes(2457600) == "2,457,600 bytes", "grouped millions");
    check(formatGroupedBytes(21233664ULL) == "21,233,664 bytes",
          "an eight-digit count groups every three digits");

    check(formatUnitSize(0) == "0 bytes", "the ladder stops at bytes for 0");
    check(formatUnitSize(512) == "512 bytes", "the ladder stops at bytes below 1024");
    check(formatUnitSize(1023) == "1,023 bytes",
          "1023 is still the bytes stage, returned grouped");
    check(formatUnitSize(1024) == "1.00 KB", "1024 enters the KB stage");
    check(formatUnitSize(2457600) == "2.34 MB", "megabytes keep two decimals");
    check(formatUnitSize(1073741824ULL) == "1.00 GB", "gigabytes are reachable");
    check(formatUnitSize(1099511627776ULL) == "1.00 TB", "terabytes are reachable");

    check(formatSize(0) == "0 bytes", "the bytes stage omits the parenthesized unit");
    check(formatSize(512) == "512 bytes", "the bytes stage omits the parenthesized unit");
    check(formatSize(1024) == "1,024 bytes (1.00 KB)", "the unit stages add parentheses");
    check(formatSize(2457600) == "2,457,600 bytes (2.34 MB)",
          "the combined form matches the spec sample");
}

void testFooters()
{
    TestHarness::section("footer decision and text");

    using hexviewer::decideFooter;
    using hexviewer::FooterKind;
    using hexviewer::formatFooter;

    check(decideFooter(0, 65536) == FooterKind::Empty, "zero bytes is the empty case");
    check(decideFooter(65535, 65536) == FooterKind::FullyDisplayed,
          "one byte below the cap is fully displayed");
    check(decideFooter(65536, 65536) == FooterKind::FullyDisplayed,
          "exactly at the cap is fully displayed");
    check(decideFooter(65537, 65536) == FooterKind::Truncated,
          "one byte above the cap is truncated");

    const std::string rule(83, '-');
    check(formatFooter(FooterKind::Truncated, 2457600, 64)
                  == rule + "\r\n[Truncated] Display capped at 64 KB to ensure instant "
                            "loading. (Total: 2.34 MB)\r\n"
                  + rule + "\r\n",
          "the truncated footer matches the spec sample");
    check(formatFooter(FooterKind::Truncated, 131073, 128)
                  .find("Display capped at 128 KB")
              != std::string::npos,
          "the cap text follows the effective --max-kb");
    check(formatFooter(FooterKind::FullyDisplayed, 1024, 64)
                  == rule + "\r\n[End of File: Fully displayed] (Total: 1,024 bytes)\r\n"
                  + rule + "\r\n",
          "the fully-displayed footer uses grouped bytes");
    check(formatFooter(FooterKind::Empty, 0, 64) == "\r\n[Empty File: 0 Bytes]\r\n",
          "the empty footer is one blank line plus the marker, without rules");
}

void testErrorBlocks()
{
    TestHarness::section("error blocks");

    const std::string rule(83, '=');
    check(hexviewer::formatOpenError(L"D:\\locked\\system.dat", L"access denied")
                  == rule + "\r\n[Seer Hex Dump Error]\r\n"
                            "File:      D:\\locked\\system.dat\r\n"
                            "Error:     Cannot open file for reading (access denied)\r\n"
                  + rule + "\r\n",
          "the open-error block matches the spec grammar");

    check(hexviewer::formatCloudError(L"D:\\OneDrive\\large_archive.iso")
                  == rule + "\r\n[Seer Hex Dump Error]\r\n"
                            "File:      D:\\OneDrive\\large_archive.iso\r\n"
                            "Error:     Online-only cloud file (Recall on data access).\r\n"
                            "Hint:      Please download the file locally before viewing hex "
                            "dump.\r\n"
                  + rule + "\r\n",
          "the cloud-error block carries the error and hint lines");

    check(hexviewer::formatReadError(0x0000FFF0, L"access denied", true)
              == "[Read Error] Stopped at 0x0000FFF0: access denied\r\n",
          "the read-error footer states the stop offset and reason");
    check(hexviewer::formatReadError(0x0000FFF0, L"access denied", false)
              == "[Read Error] Stopped at 0x0000fff0: access denied\r\n",
          "the read-error offset follows the dump digit case");
}

void testCloudPredicate()
{
    TestHarness::section("isCloudRecallRequired");

    using hexviewer::isCloudRecallRequired;
    check(isCloudRecallRequired(FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS),
          "a recall-on-data-access file is an online-only placeholder");
    check(!isCloudRecallRequired(FILE_ATTRIBUTE_DIRECTORY
                                 | FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS),
          "directories are never treated as cloud placeholders");
    check(!isCloudRecallRequired(0), "plain local attributes are not cloud");
    check(!isCloudRecallRequired(INVALID_FILE_ATTRIBUTES),
          "a failed attribute probe is not cloud");
    check(!isCloudRecallRequired(FILE_ATTRIBUTE_DIRECTORY),
          "a directory attribute alone is not cloud");
}

void testOptions()
{
    TestHarness::section("hexoptions");

    using hexviewer::parseCommandLine;

    const auto defaults = parseCommandLine(
        {L"seer_hex.exe", L"C:\\in.bin", L"C:\\apps\\SeerViewer.exe"});
    check(defaults.valid, "two positionals parse");
    check(defaults.maxKb == 64 && defaults.upper && defaults.waitTimeoutMs == 30000,
          "defaults match the shipped manifest values");

    const auto separate = parseCommandLine(
        {L"seer_hex.exe", L"--max-kb", L"128", L"--wait-timeout", L"40000", L"in",
         L"viewer"});
    check(separate.valid && separate.maxKb == 128 && separate.waitTimeoutMs == 40000,
          "the --key value spelling works");

    const auto inlineForm = parseCommandLine(
        {L"seer_hex.exe", L"in", L"viewer", L"--max-kb=128", L"--case=lower"});
    check(inlineForm.valid && inlineForm.maxKb == 128 && !inlineForm.upper,
          "the --key=value spelling works and options may trail positionals");

    const auto leading = parseCommandLine(
        {L"seer_hex.exe", L"--case", L"lower", L"in", L"viewer"});
    check(leading.valid && !leading.upper && leading.inputPath == L"in"
              && leading.viewerPath == L"viewer",
          "a separate-form value is consumed as the option value, never as a positional");

    const auto clampedLow = parseCommandLine({L"seer_hex.exe", L"--max-kb=0", L"in", L"viewer"});
    check(clampedLow.valid && clampedLow.maxKb == 1, "--max-kb=0 clamps to 1");
    const auto clampedHigh = parseCommandLine(
        {L"seer_hex.exe", L"--max-kb=999999", L"in", L"viewer"});
    check(clampedHigh.valid && clampedHigh.maxKb == 999999,
          "--max-kb=999999 is inside the 1 GiB ceiling and kept as is");
    const auto clampedCeiling = parseCommandLine(
        {L"seer_hex.exe", L"--max-kb=2097152", L"in", L"viewer"});
    check(clampedCeiling.valid && clampedCeiling.maxKb == 1048576,
          "--max-kb above 1 GiB clamps to 1048576");
    const auto timeoutLow = parseCommandLine(
        {L"seer_hex.exe", L"--wait-timeout=100", L"in", L"viewer"});
    check(timeoutLow.valid && timeoutLow.waitTimeoutMs == 5000,
          "--wait-timeout=100 clamps to 5000");
    const auto timeoutHigh = parseCommandLine(
        {L"seer_hex.exe", L"--wait-timeout=700000", L"in", L"viewer"});
    check(timeoutHigh.valid && timeoutHigh.waitTimeoutMs == 600000,
          "--wait-timeout=700000 clamps to 600000");

    const auto lowercase = parseCommandLine({L"seer_hex.exe", L"--lowercase", L"in", L"viewer"});
    check(lowercase.valid && !lowercase.upper, "--lowercase selects lower");
    const auto uppercase = parseCommandLine({L"seer_hex.exe", L"--uppercase", L"in", L"viewer"});
    check(uppercase.valid && uppercase.upper, "--uppercase selects upper");

    const auto mixedCase = parseCommandLine({L"seer_hex.exe", L"--CASE=lower", L"in", L"viewer"});
    check(mixedCase.valid && !mixedCase.upper, "option names are case-insensitive");

    // "--" ends the option list: a positional that starts with '-' is a legal
    // Win32 file name and must reach the input slot instead of being scanned as
    // an option.
    const auto terminated = parseCommandLine(
        {L"seer_hex.exe", L"--max-kb=128", L"--", L"-sample.bin", L"viewer"});
    check(terminated.valid && terminated.inputPath == L"-sample.bin"
              && terminated.maxKb == 128,
          "-- ends the option list and keeps the earlier options");
    const auto terminatedOption = parseCommandLine(
        {L"seer_hex.exe", L"--", L"-lowercase", L"viewer"});
    check(terminatedOption.valid
              && terminatedOption.inputPath == L"-lowercase"
              && terminatedOption.upper,
          "an option-looking token after -- is a positional, not a flag");
    const auto terminatedSlash = parseCommandLine(
        {L"seer_hex.exe", L"--", L"/dir/file", L"viewer"});
    check(terminatedSlash.valid && terminatedSlash.inputPath == L"/dir/file",
          "a slash-prefixed path is a positional");

    // Options are '-' only, matching SeerViewer: '/'-prefixed tokens are paths,
    // so the old '/max-kb' spelling now fails as an extra positional instead of
    // being read as an option.
    const auto slashOption = parseCommandLine(
        {L"seer_hex.exe", L"/max-kb=128", L"in", L"viewer"});
    check(!slashOption.valid,
          "a slash-prefixed option is no longer accepted as an option");

    struct InvalidCase {
        std::vector<std::wstring> args;
        std::string what;
    };
    const std::vector<InvalidCase> invalid = {
        {{L"seer_hex.exe", L"--case=upper", L"--lowercase", L"in", L"viewer"},
         "--case combined with --lowercase"},
        {{L"seer_hex.exe", L"--case=lower", L"--uppercase", L"in", L"viewer"},
         "--case combined with --uppercase"},
        {{L"seer_hex.exe", L"--uppercase", L"--lowercase", L"in", L"viewer"},
         "--uppercase combined with --lowercase"},
        {{L"seer_hex.exe", L"--max-kb=1", L"--max-kb=1", L"in", L"viewer"},
         "duplicate option with the same value"},
        {{L"seer_hex.exe", L"--max-kb=1", L"--max-kb=2", L"in", L"viewer"},
         "duplicate option with different values"},
        {{L"seer_hex.exe", L"--bogus", L"in", L"viewer"}, "unknown option"},
        {{L"seer_hex.exe", L"--max-kb=abc", L"in", L"viewer"}, "non-numeric value"},
        {{L"seer_hex.exe", L"--max-kb=", L"in", L"viewer"}, "empty value"},
        {{L"seer_hex.exe", L"--max-kb", L"in", L"viewer"}, "missing option value"},
        {{L"seer_hex.exe", L"--max-kb=99999999999999999999", L"in", L"viewer"},
         "overflowing value"},
        {{L"seer_hex.exe", L"--uppercase=1", L"in", L"viewer"}, "flag given a value"},
        {{L"seer_hex.exe", L"in"}, "a single positional"},
        {{L"seer_hex.exe", L"in", L"viewer", L"extra"}, "three positionals"},
    };
    for (const auto &item : invalid) {
        check(!parseCommandLine(item.args).valid, "invalid parse rejected: " + item.what);
    }
}

}  // namespace

int main()
{
    testAppendLine();
    testColumnHeader();
    testHeader();
    testSizeFormatting();
    testFooters();
    testErrorBlocks();
    testCloudPredicate();
    testOptions();
    return TestHarness::summarize("hexformat_test");
}
