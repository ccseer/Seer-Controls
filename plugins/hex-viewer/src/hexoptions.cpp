#include "hexoptions.h"

#include <cerrno>
#include <cwchar>
#include <cwctype>

#include "wintext.h"

namespace hexviewer {
namespace {

constexpr long long kMaxKbMinimum = 1;
// A full dump is a supported request: the chunked text view opens large files
// quickly and the host lets the user move the chunk threshold, so the old 4 MB
// ceiling was our own policy, not a viewer limit. One row costs 85 bytes for
// 16 source bytes, so 1 GiB of input is about 5.3 GiB of text; the ceiling is
// the point where the dump is still a single file a user can afford.
constexpr long long kMaxKbMaximum = 1048576;
constexpr long long kWaitTimeoutMinimum = 5000;
constexpr long long kWaitTimeoutMaximum = 600000;

// Strict decimal parse: no leading whitespace, full consumption, no overflow.
// The caller decides whether a parsed value is clamped or rejected.
bool parseInteger(const std::wstring &value, long long *sink)
{
    if (value.empty()) {
        return false;
    }
    const wchar_t head = value[0];
    if (head != L'-' && head != L'+' && !iswdigit(static_cast<wint_t>(head))) {
        return false;
    }
    errno = 0;
    wchar_t *end = nullptr;
    const long long parsed = std::wcstoll(value.c_str(), &end, 10);
    if (errno == ERANGE || end != value.c_str() + value.size()) {
        return false;
    }
    *sink = parsed;
    return true;
}

long long clampTo(long long value, long long minimum, long long maximum)
{
    if (value < minimum) {
        return minimum;
    }
    return value > maximum ? maximum : value;
}

}  // namespace

HexOptions parseCommandLine(const std::vector<std::wstring> &arguments)
{
    HexOptions options;
    std::vector<std::wstring> positionals;
    bool optionsDone = false;
    bool seenMaxKb = false;
    bool seenCase = false;
    bool seenWaitTimeout = false;
    bool seenUppercase = false;
    bool seenLowercase = false;

    for (size_t index = 1; index < arguments.size(); ++index) {
        const std::wstring &token = arguments[index];
        // "--" ends the option list, so a positional that itself starts with
        // '-' still reaches the file/viewer slots. Everything after it is a
        // positional even when it looks like an option.
        if (!optionsDone && token == L"--") {
            optionsDone = true;
            continue;
        }
        if (optionsDone || token.size() < 2 || token[0] != L'-') {
            positionals.push_back(token);
            continue;
        }

        size_t dashes = 0;
        while (dashes < token.size() && token[dashes] == L'-') {
            ++dashes;
        }
        const std::wstring rest = token.substr(dashes);
        std::wstring name;
        std::wstring inlineValue;
        bool hasInlineValue = false;
        const size_t equals = rest.find(L'=');
        if (equals != std::wstring::npos) {
            name = WinText::toLower(rest.substr(0, equals));
            inlineValue = rest.substr(equals + 1);
            hasInlineValue = true;
        }
        else {
            name = WinText::toLower(rest);
        }
        if (name.empty()) {
            options.error = L"malformed option: " + token;
            return options;
        }

        if (name == L"max-kb" || name == L"case" || name == L"wait-timeout") {
            bool &seen = name == L"max-kb" ? seenMaxKb
                         : name == L"case" ? seenCase
                                           : seenWaitTimeout;
            if (seen) {
                options.error = L"option specified more than once: --" + name;
                return options;
            }
            seen = true;
            std::wstring value;
            if (hasInlineValue) {
                value = inlineValue;
            }
            else {
                if (index + 1 >= arguments.size()) {
                    options.error = L"option requires a value: --" + name;
                    return options;
                }
                value = arguments[++index];
            }
            if (value.empty()) {
                options.error = L"option value is empty: --" + name;
                return options;
            }
            if (name == L"max-kb") {
                long long parsed = 0;
                if (!parseInteger(value, &parsed)) {
                    options.error = L"option value is not a number: --max-kb=" + value;
                    return options;
                }
                options.maxKb = clampTo(parsed, kMaxKbMinimum, kMaxKbMaximum);
            }
            else if (name == L"wait-timeout") {
                long long parsed = 0;
                if (!parseInteger(value, &parsed)) {
                    options.error =
                        L"option value is not a number: --wait-timeout=" + value;
                    return options;
                }
                options.waitTimeoutMs = clampTo(parsed, kWaitTimeoutMinimum,
                                                kWaitTimeoutMaximum);
            }
            else if (WinText::equalsIgnoreCase(value, L"upper")) {
                options.upper = true;
            }
            else if (WinText::equalsIgnoreCase(value, L"lower")) {
                options.upper = false;
            }
            else {
                options.error = L"option value must be upper or lower: --case="
                                + value;
                return options;
            }
        }
        else if (name == L"uppercase" || name == L"lowercase") {
            bool &seen = name == L"uppercase" ? seenUppercase : seenLowercase;
            if (seen) {
                options.error = L"option specified more than once: --" + name;
                return options;
            }
            if (hasInlineValue) {
                options.error = L"option does not take a value: --" + name;
                return options;
            }
            seen = true;
            options.upper = name == L"uppercase";
        }
        else {
            options.error = L"unknown option: " + token;
            return options;
        }
    }

    if (seenCase && (seenUppercase || seenLowercase)) {
        options.error = L"--case cannot be combined with --uppercase or --lowercase";
        return options;
    }
    if (seenUppercase && seenLowercase) {
        options.error = L"--uppercase and --lowercase cannot be combined";
        return options;
    }
    if (positionals.size() != 2) {
        options.error =
            L"expected exactly two positional arguments: <input_file> "
            L"<seerviewer_exe>";
        return options;
    }
    options.inputPath = positionals[0];
    options.viewerPath = positionals[1];
    options.valid = true;
    return options;
}

}  // namespace hexviewer
