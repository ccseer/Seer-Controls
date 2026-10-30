#pragma once

#include <cstdint>
#include <string>

namespace hexviewer {

// 64-bit FNV-1a over the UTF-16 code units of the normalized absolute path.
// The input is normalized to native separators, stripped of any extended
// prefix, and lexically expanded, so the same file hashes the same however the
// host spells the path. Case is preserved: on NTFS "A.bin" and "a.bin" are
// distinct names and must never share one dump file.
uint64_t fnv1a64(const std::wstring &rawPath);

// Middle-ellipsis capping. Keeps (maxChars-3)/2 leading code units and the
// rest trailing; a surrogate pair straddling either boundary shrinks that side
// by one code unit, so pairs are never split and the result is never longer
// than maxChars. A cap below 4 has no room for the ellipsis: it returns the
// leading units instead and drops a high surrogate left dangling by the cut.
std::wstring capBaseName(const std::wstring &name, size_t maxChars = 64);

// "<hash16> - <capped>.hextxt"; at most 16 + 3 + maxChars + 7 characters.
// The .hextxt suffix is what Seer's text viewer claims through the syntax
// contribution mechanism (HexDump definition shipped in syntax/hextxt.xml),
// so the dump opens highlighted instead of falling to the unknown viewer.
// The embedded base name is sanitized first: a Win32-reserved character (':' in
// particular, which would turn the dump into an NTFS stream) and an unpaired
// UTF-16 surrogate (which the percent-encoded pending handoff to SeerViewer
// would turn into U+FFFD) become '_'. A drive root has no base name, so the
// result carries an empty one.
std::wstring outputFileName(const std::wstring &path, size_t maxChars = 64);

struct SafeOutputPath {
    std::wstring txtPath;   // plain form, no extended prefix; goes to SeerViewer
    std::wstring targetDir; // the dumps directory that will hold it
    bool ok = false;
};

// Deterministic output placement: prefer the owned root's dumps\ directory,
// shrink the embedded name to 32 characters once the full path would reach 240
// characters (MAX_PATH with margin; the tmp file is created through the
// extended prefix and is not bound by this budget), then fall back to the
// namespaced temp root, and fail (ok == false) when nothing fits. An empty
// preferred root fails immediately.
SafeOutputPath safeOutputPath(const std::wstring &preferredOwnedRoot,
                              const std::wstring &fallbackTempRoot,
                              const std::wstring &path);

// "<txt>.<pid>_<nonce>.tmp": unique per invocation, so two rapid triggers on
// the same file never share a staging file.
std::wstring tmpFileName(const std::wstring &txtPath, unsigned long pid,
                         unsigned long nonce);

}  // namespace hexviewer
