#include "hexpaths.h"

#include "winpath.h"

namespace hexviewer {
namespace {

// MAX_PATH (260) minus a safety margin for the tmp suffix that SeerViewer
// side tooling may append; the tmp file itself is created through the extended
// prefix and is not bound by this budget.
constexpr size_t kPathLengthBudget = 240;
constexpr size_t kShortenedName = 32;

bool isHighSurrogate(wchar_t unit)
{
    return unit >= 0xD800 && unit <= 0xDBFF;
}

bool isLowSurrogate(wchar_t unit)
{
    return unit >= 0xDC00 && unit <= 0xDFFF;
}

// A path component that reaches CreateFileW with one of these cannot name a
// plain file: ':' makes Windows read the rest as an NTFS stream name, which
// publishes the dump into an alternate data stream of a stray zero-length base
// file and breaks the atomic rename, and the remaining characters are illegal
// in a name component outright.
bool isIllegalNameUnit(wchar_t unit)
{
    if (unit < 32) {
        return true;
    }
    switch (unit) {
    case L'<':
    case L'>':
    case L':':
    case L'"':
    case L'/':
    case L'\\':
    case L'|':
    case L'?':
    case L'*':
        return true;
    default:
        return false;
    }
}

std::wstring sanitizeNameComponent(const std::wstring &name)
{
    std::wstring safe;
    safe.reserve(name.size());
    for (size_t index = 0; index < name.size(); ++index) {
        const wchar_t unit = name[index];
        // A well-formed pair is kept for capBaseName to bound safely.
        if (isHighSurrogate(unit) && index + 1 < name.size()
            && isLowSurrogate(name[index + 1])) {
            safe += unit;
            safe += name[index + 1];
            ++index;
            continue;
        }
        // An unpaired surrogate survives NTFS untouched, but the path handed to
        // SeerViewer is percent-encoded through UTF-8, which turns the lone unit
        // into U+FFFD: the viewer would poll for a name that is not on disk.
        if (isHighSurrogate(unit) || isLowSurrogate(unit)) {
            safe += L'_';
            continue;
        }
        safe += isIllegalNameUnit(unit) ? L'_' : unit;
    }
    return safe;
}

}  // namespace

uint64_t fnv1a64(const std::wstring &rawPath)
{
    std::wstring normalized = WinPath::nativeSeparators(rawPath);
    normalized = WinPath::removeExtendedPrefix(normalized);
    std::wstring lexical;
    if (!WinPath::lexicalPath(normalized, &lexical)) {
        lexical = normalized;
    }
    uint64_t hash = 0xcbf29ce484222325ULL;
    for (const wchar_t unit : lexical) {
        hash ^= static_cast<uint16_t>(unit);
        hash *= 0x100000001b3ULL;
    }
    return hash;
}

std::wstring capBaseName(const std::wstring &name, size_t maxChars)
{
    if (maxChars < 4) {
        // Too narrow for the ellipsis form; the cut must still not leave a
        // dangling high surrogate in the result.
        std::wstring head = name.substr(0, maxChars);
        if (!head.empty() && isHighSurrogate(head.back())) {
            head.pop_back();
        }
        return head;
    }
    if (name.size() <= maxChars) {
        return name;
    }
    size_t leftLength = (maxChars - 3) / 2;
    size_t rightLength = maxChars - 3 - leftLength;
    // At maxChars == 4 the leading run is empty, so there is no boundary unit
    // to inspect and indexing name[leftLength - 1] would wrap to SIZE_MAX.
    if (leftLength > 0 && isHighSurrogate(name[leftLength - 1])) {
        --leftLength;
    }
    if (isLowSurrogate(name[name.size() - rightLength])) {
        --rightLength;
    }
    return name.substr(0, leftLength) + L"..."
           + name.substr(name.size() - rightLength);
}

std::wstring outputFileName(const std::wstring &path, size_t maxChars)
{
    static const wchar_t *kDigits = L"0123456789abcdef";
    const uint64_t hash = fnv1a64(path);
    std::wstring hashText;
    for (int shift = 60; shift >= 0; shift -= 4) {
        hashText += kDigits[(hash >> shift) & 0xF];
    }
    const std::wstring base
        = capBaseName(sanitizeNameComponent(WinPath::fileName(path)), maxChars);
    return hashText + L" - " + base + L".hextxt";
}

SafeOutputPath safeOutputPath(const std::wstring &preferredOwnedRoot,
                              const std::wstring &fallbackTempRoot,
                              const std::wstring &path)
{
    SafeOutputPath result;
    if (preferredOwnedRoot.empty()) {
        return result;
    }
    const std::wstring preferredDumps
        = WinPath::join(preferredOwnedRoot, L"dumps");

    const std::wstring full
        = WinPath::join(preferredDumps, outputFileName(path, 64));
    if (full.size() < kPathLengthBudget) {
        result.txtPath = full;
        result.targetDir = preferredDumps;
        result.ok = true;
        return result;
    }

    const std::wstring shortened = WinPath::join(
        preferredDumps, outputFileName(path, kShortenedName));
    if (shortened.size() < kPathLengthBudget) {
        result.txtPath = shortened;
        result.targetDir = preferredDumps;
        result.ok = true;
        return result;
    }

    if (!fallbackTempRoot.empty() && fallbackTempRoot != preferredOwnedRoot) {
        const std::wstring fallbackDumps
            = WinPath::join(fallbackTempRoot, L"dumps");
        const std::wstring fallback = WinPath::join(
            fallbackDumps, outputFileName(path, kShortenedName));
        if (fallback.size() < kPathLengthBudget) {
            result.txtPath = fallback;
            result.targetDir = fallbackDumps;
            result.ok = true;
            return result;
        }
    }
    return result;
}

std::wstring tmpFileName(const std::wstring &txtPath, unsigned long pid,
                         unsigned long nonce)
{
    return txtPath + L"." + std::to_wstring(pid) + L"_"
           + std::to_wstring(nonce) + L".tmp";
}

}  // namespace hexviewer
