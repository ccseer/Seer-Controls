#pragma once

#include <cstddef>

#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace folderlisting {

inline constexpr size_t kDefaultMaxEntries = 5000;
inline constexpr size_t kDefaultMaxBytes = 1024 * 1024;

// Written after every rendered entry, including the last one, so the result
// always ends the same way and a consumer that splits on the separator never
// sees a final entry without one.
inline constexpr wchar_t kDefaultSeparator[] = L"\r\n";
inline constexpr size_t kMaxSeparatorChars = 16;

struct Request {
    std::wstring root;
    bool includeHidden = false;
    std::vector<std::wstring> excludes;
    size_t maxEntries = kDefaultMaxEntries;
    size_t maxBytes = kDefaultMaxBytes;
    std::wstring separator = kDefaultSeparator;
};

struct ListingResult {
    bool ok = false;
    int exitCode = 1;
    std::wstring message;
    std::wstring listing;
    bool truncated = false;
    std::wstring truncationNote;
};

// The clipboard is a shared global resource, so it is injected: a test can prove
// that a failed copy still preserves the generated result.
using ClipboardWriter = std::function<bool(const std::wstring &text,
                                           std::wstring *error)>;

std::optional<Request> parseArguments(const std::vector<std::wstring> &arguments,
                                      std::wstring *error = nullptr);

// Expands a --separator value. Named forms (crlf, lf, cr, tab, space, comma,
// semicolon, pipe, none) exist because the host passes arguments as JSON
// strings, where a literal escape would need its backslash doubled; anything
// else is taken literally after \n, \r, \t and \\ are expanded.
bool parseSeparator(const std::wstring &value, std::wstring *separator,
                    std::wstring *error);

// Builds the whole bounded result in memory. No clipboard is touched here.
// The selected folder must exist and must not itself be a link; a folder that
// exists but cannot be enumerated is a failure, never an empty listing.
ListingResult buildListing(const Request &request);

bool matchesPattern(const std::wstring &name, const std::wstring &pattern);

ClipboardWriter systemClipboardWriter();

ListingResult run(const Request &request, const ClipboardWriter &writer);
ListingResult run(const std::vector<std::wstring> &arguments);

}  // namespace folderlisting
