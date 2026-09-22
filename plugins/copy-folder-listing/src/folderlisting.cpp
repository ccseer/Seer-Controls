#include "folderlisting.h"

#include <windows.h>

#include <algorithm>
#include <cerrno>
#include <cstdlib>

#include "wincmd.h"
#include "winclip.h"
#include "winexit.h"
#include "winpath.h"
#include "wintext.h"

namespace folderlisting {

namespace {

struct Builder {
    const Request &request;
    std::wstring text;
    // The limit is a UTF-8 byte limit: it measures the text the way a file or a
    // pasted message would be measured, not the way Windows stores clipboard
    // text (CF_UNICODETEXT keeps UTF-16 units). Counting UTF-16 units would
    // silently allow about twice as much text as the option says.
    size_t utf8Bytes = 0;
    size_t listed = 0;
    bool truncated = false;
    std::wstring truncationNote;
};

void setTruncationNote(Builder *builder, const std::wstring &description)
{
    builder->truncated = true;
    if (builder->truncationNote.empty()) {
        builder->truncationNote = description;
    }
}

// Measures a line the way the clipboard hands it out: UTF-8 bytes, plus the
// CRLF that follows it. A name with an unpaired surrogate (legal in NTFS)
// converts to one U+FFFD per bad unit, three bytes each, which is also the
// worst case per unit - so a refusal is charged the same way instead of as
// nothing, and no name can slip past the byte limit by failing to convert.
// Asking for the encoded length only, instead of converting, keeps one heap
// allocation per entry out of a large listing.
size_t lineUtf8Bytes(const std::wstring &line)
{
    const int encoded
        = WideCharToMultiByte(CP_UTF8, 0, line.data(),
                              static_cast<int>(line.size()), nullptr, 0, nullptr,
                              nullptr);
    constexpr size_t kMaxUtf8BytesPerUnit = 3;
    const size_t bytes = encoded > 0 ? static_cast<size_t>(encoded)
                                     : line.size() * kMaxUtf8BytesPerUnit;
    return bytes + 2;  // CRLF
}

bool appendLine(Builder *builder, const std::wstring &line)
{
    const size_t lineBytes = lineUtf8Bytes(line);
    if (builder->utf8Bytes + lineBytes > builder->request.maxBytes) {
        setTruncationNote(
            builder,
            L"the output byte limit ("
                + std::to_wstring(builder->request.maxBytes)
                + L" UTF-8 bytes) was reached");
        return false;
    }
    builder->utf8Bytes += lineBytes;
    builder->text += line;
    builder->text += L"\r\n";
    return true;
}

bool entryLimitReached(const Builder &builder)
{
    return builder.listed >= builder.request.maxEntries;
}

bool isHidden(const DWORD attributes)
{
    return (attributes & FILE_ATTRIBUTE_HIDDEN) != 0;
}

void sortEntries(std::vector<WinPath::DirectoryEntry> *entries)
{
    std::sort(entries->begin(), entries->end(),
              [](const WinPath::DirectoryEntry &left,
                 const WinPath::DirectoryEntry &right) {
                  // Folders first, then files, each group in a stable
                  // case-insensitive ordinal order.
                  if (left.isDirectory != right.isDirectory) {
                      return left.isDirectory;
                  }
                  const int order
                      = WinText::compareIgnoreCase(left.name, right.name);
                  if (order != 0) {
                      return order < 0;
                  }
                  return left.name < right.name;
              });
}

// Renders first-level entries only. Folders are listed but never descended
// into: the request is a listing of one directory, not a walk.
void renderEntries(Builder *builder,
                   const std::vector<WinPath::DirectoryEntry> &entries)
{
    for (const auto &entry : entries) {
        if (entryLimitReached(*builder)) {
            setTruncationNote(
                builder,
                L"the entry limit ("
                    + std::to_wstring(builder->request.maxEntries)
                    + L" entries) was reached");
            return;
        }
        if (!builder->request.includeHidden && isHidden(entry.attributes)) {
            continue;
        }
        bool excluded = false;
        for (const auto &pattern : builder->request.excludes) {
            if (matchesPattern(entry.name, pattern)) {
                excluded = true;
                break;
            }
        }
        if (excluded) {
            continue;
        }

        // Every rendered line counts toward the entry limit, including the
        // reparse-point ones; the limit means "maximum rendered entries".
        ++builder->listed;

        std::wstring line = entry.name;
        if (entry.isDirectory) {
            line += L"/";
        }

        if (entry.isReparsePoint) {
            line += L" -> [link, not followed]";
            if (!appendLine(builder, line)) {
                return;
            }
            continue;
        }
        if (!appendLine(builder, line)) {
            return;
        }
    }
}

}  // namespace

bool matchesPattern(const std::wstring &name, const std::wstring &pattern)
{
    // Iterative wildcard match with backtracking: '*' matches any run of
    // characters, '?' matches exactly one. Comparisons are per UTF-16 code
    // unit, case-insensitive and ordinal, matching how the host compares
    // extensions. CompareStringOrdinal takes explicit lengths, so a comparison
    // never allocates.
    size_t nameIndex    = 0;
    size_t patternIndex = 0;
    size_t starIndex    = std::wstring::npos;
    size_t matchIndex   = 0;

    while (nameIndex < name.size()) {
        bool matched = false;
        if (patternIndex < pattern.size()) {
            if (pattern[patternIndex] == L'?') {
                matched = true;
            }
            else if (pattern[patternIndex] != L'*') {
                matched = CompareStringOrdinal(&pattern[patternIndex], 1,
                                               &name[nameIndex], 1, TRUE)
                          == CSTR_EQUAL;
            }
        }
        if (matched) {
            ++nameIndex;
            ++patternIndex;
            continue;
        }
        if (patternIndex < pattern.size() && pattern[patternIndex] == L'*') {
            starIndex    = patternIndex++;
            matchIndex   = nameIndex;
            continue;
        }
        if (starIndex != std::wstring::npos) {
            patternIndex = starIndex + 1;
            nameIndex    = ++matchIndex;
            continue;
        }
        return false;
    }
    while (patternIndex < pattern.size() && pattern[patternIndex] == L'*') {
        ++patternIndex;
    }
    return patternIndex == pattern.size();
}

std::optional<Request> parseArguments(const std::vector<std::wstring> &arguments,
                                      std::wstring *error)
{
    const auto fail = [error](const std::wstring &message)
        -> std::optional<Request> {
        if (error) {
            *error = message;
        }
        return std::nullopt;
    };

    if (arguments.empty()) {
        return fail(L"the helper was started without a command line");
    }
    const auto scan = WinCmd::scanOptions(
        arguments, 1, {L"include-hidden"},
        {L"input", L"exclude", L"max-entries", L"max-bytes"});
    if (!scan.ok) {
        return fail(scan.error);
    }
    std::wstring duplicate;
    if (!WinCmd::rejectDuplicates(scan, &duplicate, {L"exclude"})) {
        return fail(duplicate);
    }
    if (!scan.positionals.empty()) {
        return fail(L"unexpected argument: " + scan.positionals.front());
    }

    Request request;
    std::wstring rawInput;
    if (!WinCmd::optionValue(scan, L"input", &rawInput) || rawInput.empty()) {
        return fail(L"--input <directory> is required");
    }
    if (!WinPath::lexicalPath(rawInput, &request.root)) {
        return fail(L"the selected folder cannot be resolved: " + rawInput);
    }

    const auto readCount = [&](const wchar_t *name, const size_t fallback,
                               size_t *sink,
                               std::wstring *problem) -> bool {
        std::wstring raw;
        if (!WinCmd::optionValue(scan, name, &raw)) {
            *sink = fallback;
            return true;
        }
        const auto trimmed = WinText::trim(raw);
        if (trimmed.empty()
            || trimmed.find_first_not_of(L"0123456789")
                   != std::wstring::npos) {
            *problem = L"--" + std::wstring(name)
                       + L" must be a whole number greater than zero; got '"
                       + raw + L"'";
            return false;
        }
        errno = 0;
        const long long value = _wcstoi64(trimmed.c_str(), nullptr, 10);
        if (errno == ERANGE || value <= 0) {
            *problem = L"--" + std::wstring(name)
                       + L" is out of range; got '" + raw + L"'";
            return false;
        }
        *sink = static_cast<size_t>(value);
        return true;
    };

    std::wstring problem;
    if (!readCount(L"max-entries", kDefaultMaxEntries, &request.maxEntries,
                   &problem)
        || !readCount(L"max-bytes", kDefaultMaxBytes, &request.maxBytes,
                      &problem)) {
        return fail(problem);
    }

    request.includeHidden = WinCmd::hasOption(scan, L"include-hidden");
    for (const auto &pattern : WinCmd::optionValues(scan, L"exclude")) {
        const auto trimmed = WinText::trim(pattern);
        if (trimmed.empty()) {
            return fail(L"--exclude cannot be empty");
        }
        request.excludes.push_back(trimmed);
    }
    return request;
}

ListingResult buildListing(const Request &request)
{
    ListingResult result;
    if (!WinPath::isDirectory(request.root)) {
        result.exitCode = WinExit::kNotFound;
        result.message  = L"the selection is not an existing folder: "
                         + request.root;
        return result;
    }
    if (WinPath::isReparsePoint(request.root)) {
        result.exitCode = WinExit::kUnsupported;
        result.message
            = L"the selection is itself a link, and links are never followed. "
              L"Select the real folder instead.";
        return result;
    }

    std::vector<WinPath::DirectoryEntry> entries;
    std::wstring enumerateError;
    if (!WinPath::enumerateDirectory(request.root, &entries,
                                     &enumerateError)) {
        // An unreadable root is the whole request failing, not one branch of a
        // tree: the recursion-era "[inaccessible: ...]" marker would otherwise
        // be copied over the clipboard and reported as success.
        result.exitCode = WinExit::kFailure;
        result.message  = L"the folder could not be read: " + enumerateError
                         + L". Nothing was copied.";
        return result;
    }
    sortEntries(&entries);

    Builder builder{request};
    renderEntries(&builder, entries);

    if (builder.truncated) {
        const std::wstring note = L"... [truncated: " + builder.truncationNote
                                  + L"]";
        // The note is small and must always be visible, so it bypasses the byte
        // limit on purpose.
        builder.text += note;
        builder.text += L"\r\n";
        // The note bypasses the limit but is still part of what the user gets,
        // so the reported size accounts for it.
        builder.utf8Bytes += lineUtf8Bytes(note);
    }

    // A non-empty folder that renders nothing means every entry was skipped as
    // hidden or excluded. Silently succeeding with a stale clipboard would be
    // indistinguishable from a working copy, so it is reported as a failure.
    // This must sit after the truncation note: a byte limit below the first
    // entry also leaves the text empty, and its note is the honest diagnosis.
    if (!builder.truncated && builder.text.empty() && !entries.empty()) {
        result.exitCode = WinExit::kFailure;
        result.message
            = L"Nothing was copied: every entry of the folder was skipped as "
              L"hidden or excluded. Relax --include-hidden or --exclude and "
              L"retry.";
        return result;
    }

    result.ok       = true;
    result.exitCode = WinExit::kOk;
    result.listing  = builder.text;
    result.truncated      = builder.truncated;
    result.truncationNote = builder.truncationNote;
    return result;
}

ClipboardWriter systemClipboardWriter()
{
    return [](const std::wstring &text, std::wstring *error) {
        // A real owner window is not required for CF_UNICODETEXT, but the
        // clipboard is retried for a bounded interval because another process
        // holding it open for a moment is normal.
        return WinClip::setText(text, nullptr, 3000, error);
    };
}

ListingResult run(const Request &request, const ClipboardWriter &writer)
{
    auto result = buildListing(request);
    if (!result.ok) {
        return result;
    }

    // An empty folder has nothing to copy: leaving the clipboard untouched is
    // safer than replacing its previous content with an empty string.
    if (result.listing.empty()) {
        return result;
    }

    std::wstring copyError;
    if (!writer(result.listing, &copyError)) {
        result.ok       = false;
        result.exitCode = WinExit::kFailure;
        result.message
            = L"Nothing was copied: the clipboard refused the listing ("
              + copyError + L"). Retry once it is free, or shrink the result "
                             L"with --max-entries or --max-bytes.";
        return result;
    }
    return result;
}

ListingResult run(const std::vector<std::wstring> &arguments)
{
    ListingResult outcome;
    std::wstring error;
    const auto request = parseArguments(arguments, &error);
    if (!request) {
        outcome.exitCode = WinExit::kUsage;
        outcome.message  = error;
        // The host surfaces the captured stderr of a failed Control action in
        // a toast, so the reason is written there instead of a window.
        WinText::writeStandardError(outcome.message);
        return outcome;
    }

    const auto result = run(*request, systemClipboardWriter());
    if (!result.ok) {
        WinText::writeStandardError(result.message);
        return result;
    }

    // Success is reported through the exit code alone: the host toasts the
    // outcome, and this helper opens no window of its own.
    return result;
}

}  // namespace folderlisting
