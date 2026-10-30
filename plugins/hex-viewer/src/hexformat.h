#pragma once

#include <windows.h>

#include <cstdint>
#include <string>

namespace hexviewer {

// Footer selection for a finished dump. totalBytes is the size measured before
// reading; capBytes is the effective --max-kb limit in bytes.
enum class FooterKind {
    Truncated,
    FullyDisplayed,
    Empty
};

// Formats one row into a stack buffer and appends 83 visible characters plus
// CRLF, so the hot path never allocates per row. Columns, all zero-based:
// offset at 0..7, a 5-space gap, first byte at 13, a double gap between bytes 7
// and 8, a 4-space gap, and the "|ascii|" block at columns 65..82. Bytes below
// 32 or above 126 render as dots.
void appendLine(std::string &buffer, uint64_t offset, const uint8_t *bytes,
                size_t count, bool upper);

// "Offset(h)    00 01 ...  Decoded Text": four spaces after the label, labels
// start at column 13 and keep the same double gap; Decoded Text starts at
// column 65, over the data rows' opening '|'. Label case follows the dump case.
std::string formatColumnHeader(bool upper);

// The "====" block at the top of every dump, including diagnostics. expected
// bytes is min(total, cap) as predicted before reading; the Range upper bound
// is expectedBytes - 1. expectedBytes == 0 (the empty file) renders
// "Showing 0 bytes" instead of a range. The path is converted to UTF-8.
std::string formatHeader(const std::wstring &path, uint64_t totalBytes,
                         uint64_t expectedBytes, long long maxKb, bool upper);

// Thousands grouping with the unit suffix: "1,024 bytes", "2,457,600 bytes".
std::string formatGroupedBytes(uint64_t bytes);

// The bytes -> KB -> MB -> GB -> TB ladder (1024-based). A value of 1024 or
// more advances one step; TB is the last step. Below 1024 the value stays in
// bytes and the result is formatGroupedBytes(bytes).
std::string formatUnitSize(uint64_t bytes);

// Header "Size:" value: grouped bytes, plus the unit form in parentheses once
// the ladder leaves the bytes stage, e.g. "2,457,600 bytes (2.34 MB)".
std::string formatSize(uint64_t bytes);

// totalBytes == 0 is Empty; totalBytes > capBytes is Truncated; anything else
// (including exactly capBytes) is FullyDisplayed.
FooterKind decideFooter(uint64_t totalBytes, uint64_t capBytes);

// Truncated / FullyDisplayed render their own rule lines; Empty starts with
// one blank line, matching the empty-file sample. All lines end with CRLF.
std::string formatFooter(FooterKind kind, uint64_t totalBytes, long long maxKb);

// The "====" diagnostic block for an input that cannot be opened for reading.
std::string formatOpenError(const std::wstring &path, const std::wstring &reason);

// The diagnostic block for an online-only cloud placeholder; never reads.
std::string formatCloudError(const std::wstring &path);

// Footer appended after the rows that were read before a read failure. The
// offset follows the dump's digit case, like every other offset in the file.
std::string formatReadError(uint64_t offset, const std::wstring &reason,
                            bool upper);

// Mirrors the host's IsFileOnCloud test: only a regular (non-directory) file
// whose bytes are not local counts as an online-only placeholder.
bool isCloudRecallRequired(DWORD attributes);

}  // namespace hexviewer
