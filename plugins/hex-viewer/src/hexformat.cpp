#include "hexformat.h"

#include <cstdio>
#include <string>

#include "wintext.h"

namespace hexviewer {
namespace {

const char kHexDigitsUpper[] = "0123456789ABCDEF";
const char kHexDigitsLower[] = "0123456789abcdef";

// One row is 83 visible characters: 8 offset digits, a 5-space gap, the 16-byte
// grid with its 7/8 double gap, a 4-space gap, then |ascii|. The two block gaps
// are deliberately wider than one space: the dump is read next to editor text,
// and a single space reads as a stray column at small font sizes.
constexpr size_t kOffsetChars = 8;
constexpr size_t kOffsetGap = 5;
constexpr size_t kAsciiGap = 4;
constexpr size_t kRowColumns = 83;

const char *digitTable(bool upper)
{
    return upper ? kHexDigitsUpper : kHexDigitsLower;
}

// At least minDigits lowercase/uppercase hex digits, more when the value needs
// them (the read-error offset can outgrow 32 bits in principle).
std::string hexWithDigits(uint64_t value, bool upper, int minDigits)
{
    const char *digits = digitTable(upper);
    int nibbles = 1;
    for (uint64_t rest = value; rest >= 16 && nibbles < 16; rest >>= 4) {
        ++nibbles;
    }
    if (nibbles < minDigits) {
        nibbles = minDigits;
    }
    std::string out;
    for (int shift = (nibbles - 1) * 4; shift >= 0; shift -= 4) {
        out += digits[(value >> shift) & 0xF];
    }
    return out;
}

}  // namespace

void appendLine(std::string &buffer, uint64_t offset, const uint8_t *bytes,
                size_t count, bool upper)
{
    if (count > 16) {
        count = 16;
    }
    const char *digits = digitTable(upper);
    char line[kRowColumns + 2];
    size_t position = 0;
    for (int shift = 28; shift >= 0; shift -= 4) {
        line[position++] = digits[(offset >> shift) & 0xF];
    }
    for (size_t index = 0; index < kOffsetGap; ++index) {
        line[position++] = ' ';
    }
    for (size_t index = 0; index < 16; ++index) {
        if (index < count) {
            const uint8_t value = bytes[index];
            line[position++] = digits[(value >> 4) & 0xF];
            line[position++] = digits[value & 0xF];
        }
        else {
            line[position++] = ' ';
            line[position++] = ' ';
        }
        if (index < 15) {
            line[position++] = ' ';
            if (index == 7) {
                line[position++] = ' ';  // the 7/8 double gap
            }
        }
    }
    for (size_t index = 0; index < kAsciiGap; ++index) {
        line[position++] = ' ';
    }
    line[position++] = '|';
    for (size_t index = 0; index < 16; ++index) {
        if (index < count) {
            const uint8_t value = bytes[index];
            line[position++] = (value >= 32 && value <= 126)
                                   ? static_cast<char>(value)
                                   : '.';
        }
        else {
            line[position++] = ' ';
        }
    }
    line[position++] = '|';
    buffer.append(line, position);
    buffer += "\r\n";
}

std::string formatColumnHeader(bool upper)
{
    const char *digits = digitTable(upper);
    // "Offset(h)" is one character wider than the offset field, so padding to
    // the first byte column leaves four spaces after the label.
    std::string out = "Offset(h)";
    out.append(kOffsetChars + kOffsetGap - out.size(), ' ');
    for (int index = 0; index < 16; ++index) {
        out += digits[(index >> 4) & 0xF];
        out += digits[index & 0xF];
        if (index < 15) {
            out += ' ';
            if (index == 7) {
                out += ' ';
            }
        }
        else {
            out.append(kAsciiGap, ' ');
            out += "Decoded Text\r\n";
        }
    }
    return out;
}

std::string formatHeader(const std::wstring &path, uint64_t totalBytes,
                         uint64_t expectedBytes, long long maxKb, bool upper)
{
    const uint64_t capBytes = static_cast<uint64_t>(maxKb) * 1024ULL;
    std::string range;
    if (expectedBytes == 0) {
        range = "Showing 0 bytes";
    }
    else if (totalBytes > capBytes) {
        range = "Showing first " + std::to_string(maxKb) + " KB (0x00000000 - 0x"
                + hexWithDigits(expectedBytes - 1, upper, 8) + ")";
    }
    else {
        range = "Showing all " + formatGroupedBytes(totalBytes)
                + " (0x00000000 - 0x" + hexWithDigits(expectedBytes - 1, upper, 8)
                + ")";
    }

    const std::string rule(kRowColumns, '=');
    std::string out;
    out += rule + "\r\n";
    out += "[Seer Hex Dump]\r\n";
    out += "File:      " + WinText::toUtf8(path) + "\r\n";
    out += "Size:      " + formatSize(totalBytes) + "\r\n";
    out += "Range:     " + range + "\r\n";
    out += rule + "\r\n";
    return out;
}

std::string formatGroupedBytes(uint64_t bytes)
{
    const std::string digits = std::to_string(bytes);
    std::string grouped;
    const size_t count = digits.size();
    for (size_t index = 0; index < count; ++index) {
        grouped += digits[index];
        const size_t remaining = count - index - 1;
        if (remaining > 0 && remaining % 3 == 0) {
            grouped += ',';
        }
    }
    return grouped + " bytes";
}

std::string formatUnitSize(uint64_t bytes)
{
    if (bytes < 1024) {
        return formatGroupedBytes(bytes);
    }
    static const char *kUnits[] = {"KB", "MB", "GB", "TB"};
    double value = static_cast<double>(bytes);
    int stage = 0;
    while (value >= 1024.0 && stage < 4) {
        value /= 1024.0;
        ++stage;
    }
    char text[32];
    std::snprintf(text, sizeof(text), "%.2f %s", value, kUnits[stage - 1]);
    return std::string(text);
}

std::string formatSize(uint64_t bytes)
{
    if (bytes < 1024) {
        return formatGroupedBytes(bytes);
    }
    return formatGroupedBytes(bytes) + " (" + formatUnitSize(bytes) + ")";
}

FooterKind decideFooter(uint64_t totalBytes, uint64_t capBytes)
{
    if (totalBytes == 0) {
        return FooterKind::Empty;
    }
    return totalBytes > capBytes ? FooterKind::Truncated
                                 : FooterKind::FullyDisplayed;
}

std::string formatFooter(FooterKind kind, uint64_t totalBytes, long long maxKb)
{
    const std::string rule(kRowColumns, '-');
    switch (kind) {
    case FooterKind::Truncated:
        return rule + "\r\n[Truncated] Display capped at " + std::to_string(maxKb)
               + " KB to ensure instant loading. (Total: "
               + formatUnitSize(totalBytes) + ")\r\n" + rule + "\r\n";
    case FooterKind::FullyDisplayed:
        return rule + "\r\n[End of File: Fully displayed] (Total: "
               + formatGroupedBytes(totalBytes) + ")\r\n" + rule + "\r\n";
    case FooterKind::Empty:
    default:
        return "\r\n[Empty File: 0 Bytes]\r\n";
    }
}

std::string formatOpenError(const std::wstring &path, const std::wstring &reason)
{
    const std::string rule(kRowColumns, '=');
    std::string out;
    out += rule + "\r\n";
    out += "[Seer Hex Dump Error]\r\n";
    out += "File:      " + WinText::toUtf8(path) + "\r\n";
    out += "Error:     Cannot open file for reading (" + WinText::toUtf8(reason)
           + ")\r\n";
    out += rule + "\r\n";
    return out;
}

std::string formatCloudError(const std::wstring &path)
{
    const std::string rule(kRowColumns, '=');
    std::string out;
    out += rule + "\r\n";
    out += "[Seer Hex Dump Error]\r\n";
    out += "File:      " + WinText::toUtf8(path) + "\r\n";
    out += "Error:     Online-only cloud file (Recall on data access).\r\n";
    out += "Hint:      Please download the file locally before viewing hex dump.\r\n";
    out += rule + "\r\n";
    return out;
}

std::string formatReadError(uint64_t offset, const std::wstring &reason,
                           bool upper)
{
    return "[Read Error] Stopped at 0x" + hexWithDigits(offset, upper, 8) + ": "
           + WinText::toUtf8(reason) + "\r\n";
}

bool isCloudRecallRequired(DWORD attributes)
{
    return attributes != INVALID_FILE_ATTRIBUTES
           && (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0
           && (attributes & FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS) != 0;
}

}  // namespace hexviewer
