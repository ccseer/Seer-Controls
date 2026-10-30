#pragma once

#include <windows.h>

#include <string>

namespace hexviewer {

struct AtomicWriteResult {
    bool ok = false;
    std::wstring errorMessage;
};

// Publishes content through a uniquely named temporary file: create (must not
// already exist), stream, flush to disk, then rename over the target with
// bounded retries. The writer never shows UI and never exits; the caller owns
// both, which keeps the failure branches directly testable. Every path handed
// to Win32 goes through the extended-length prefix, because the tmp name is
// longer than the target and the target may already sit close to the
// 240-character budget.
//
// An instance owns an open handle and the staging path, so it is neither
// copyable (a copy would close the same handle twice) nor allowed to rely on
// every caller reaching finish() or abandon(): the destructor releases both.
class AtomicFileWriter {
public:
    AtomicFileWriter() = default;
    ~AtomicFileWriter();

    AtomicFileWriter(const AtomicFileWriter &) = delete;
    AtomicFileWriter &operator=(const AtomicFileWriter &) = delete;

    bool begin(const std::wstring &tmpPath, std::wstring *error);
    bool write(const void *data, size_t size, std::wstring *error);
    bool finish(const std::wstring &txtPath, std::wstring *error);
    void abandon();

    // One-shot helper: begin + write + finish, with the tmp removed on any
    // failure. ok == false always leaves no staging file behind.
    static AtomicWriteResult commit(const std::wstring &tmpPath,
                                    const std::wstring &txtPath,
                                    const std::string &content);

    // Last-resort direct write (CREATE_ALWAYS, no staging round trip) used
    // only when the atomic publish itself failed. A read-only target has its
    // read-only bit cleared first. Returns the outcome without any UI side
    // effect.
    static AtomicWriteResult writeDirect(const std::wstring &path,
                                         const std::string &content);

private:
    HANDLE m_handle = INVALID_HANDLE_VALUE;
    std::wstring m_tmpPath;
};

}  // namespace hexviewer
