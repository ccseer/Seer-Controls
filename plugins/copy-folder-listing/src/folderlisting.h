#pragma once

#include <cstddef>

#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace folderlisting {

inline constexpr size_t kDefaultMaxEntries = 5000;
inline constexpr size_t kDefaultMaxBytes = 1024 * 1024;

struct Request {
    std::wstring root;
    bool includeHidden = false;
    std::vector<std::wstring> excludes;
    size_t maxEntries = kDefaultMaxEntries;
    size_t maxBytes = kDefaultMaxBytes;
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

// Builds the whole bounded result in memory. No clipboard is touched here.
// The selected folder must exist and must not itself be a link; a folder that
// exists but cannot be enumerated is a failure, never an empty listing.
ListingResult buildListing(const Request &request);

bool matchesPattern(const std::wstring &name, const std::wstring &pattern);

ClipboardWriter systemClipboardWriter();

ListingResult run(const Request &request, const ClipboardWriter &writer);
ListingResult run(const std::vector<std::wstring> &arguments);

}  // namespace folderlisting
