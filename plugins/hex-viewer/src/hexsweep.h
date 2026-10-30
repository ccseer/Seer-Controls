#pragma once

#include <string>

namespace hexviewer {

// Best-effort reclaim of dumps older than seven days (by last write time):
// every *.hextxt and orphan *.tmp directly inside dumpsDirectory.
// Directories, reparse points and anything else are left alone; individual
// failures are ignored.
void sweepOldDumps(const std::wstring &dumpsDirectory);

}  // namespace hexviewer
