#pragma once

#include <windows.h>

#include <functional>

#include <cstdint>

#include "atomicfilewriter.h"

namespace hexviewer {

struct HexDumpRequest {
    std::wstring inputPath;   // as received; normalized internally
    std::wstring txtPath;     // publish target, plain form
    std::wstring tmpPath;     // unique staging path for this invocation
    long long maxKb = 64;     // already clamped
    bool upper = true;
};

// Read seam: fills destination with up to bytesToRead bytes and returns false
// with errorCode set on failure. The production path wraps ReadFile on the
// input handle; tests inject deterministic failures.
using ReadProcedure = std::function<bool(uint8_t *destination,
                                         size_t bytesToRead, size_t *bytesRead,
                                         DWORD *errorCode)>;

// Renders the dump — or the diagnostic block for cloud-offline, unopenable or
// locked input — into the staging file and atomically publishes it as txtPath.
// A diagnostic block is a successful result: the helper's exit code stays 0 so
// the waiting viewer shows the text. Only staging, write or rename failures
// return ok == false.
AtomicWriteResult generate(const HexDumpRequest &request,
                           const ReadProcedure &readProcedure = nullptr);

// Win32 error code -> the fixed short reason used by the diagnostic blocks,
// with the system message text as the fallback.
std::wstring describeIoError(DWORD errorCode);

}  // namespace hexviewer
