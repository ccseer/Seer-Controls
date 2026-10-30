#include "hexdump.h"

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

#include "hexformat.h"
#include "winpath.h"
#include "wintext.h"

namespace hexviewer {
namespace {

constexpr size_t kReadBlockBytes = 64 * 1024;
// The staging buffer is handed to the tmp file once it grows past this, so
// memory stays bounded regardless of the effective cap.
constexpr size_t kContentFlushBytes = 1024 * 1024;

struct HandleCloser {
    void operator()(void *handle) const
    {
        CloseHandle(handle);
    }
};

// The input handle is released by one owner instead of at each of the five
// exit paths, so a future early return cannot leak it.
using UniqueHandle = std::unique_ptr<void, HandleCloser>;

bool readFromHandle(HANDLE handle, uint8_t *destination, size_t bytesToRead,
                    size_t *bytesRead, DWORD *errorCode)
{
    DWORD read = 0;
    if (!ReadFile(handle, destination, static_cast<DWORD>(bytesToRead), &read,
                  nullptr)) {
        *errorCode = GetLastError();
        return false;
    }
    *bytesRead = read;
    return true;
}

}  // namespace

std::wstring describeIoError(DWORD errorCode)
{
    switch (errorCode) {
    case ERROR_FILE_NOT_FOUND:
    case ERROR_PATH_NOT_FOUND:
        return L"file not found";
    case ERROR_ACCESS_DENIED:
        return L"access denied";
    case ERROR_SHARING_VIOLATION:
    case ERROR_LOCK_VIOLATION:
        return L"file is locked by another process";
    default:
        return WinText::systemErrorMessage(errorCode);
    }
}

AtomicWriteResult generate(const HexDumpRequest &request,
                           const ReadProcedure &readProcedure)
{
    // One normalization up front: display uses backslashes without any
    // extended prefix (also what the File: line shows), and every Win32 call
    // goes through the extended-prefix form of it. The host may hand over a
    // Qt-style forward-slash path, which the extended prefix alone would not
    // fix.
    std::wstring displayPath;
    const std::wstring native
        = WinPath::removeExtendedPrefix(WinPath::nativeSeparators(request.inputPath));
    if (!WinPath::lexicalPath(native, &displayPath)) {
        displayPath = WinPath::nativeSeparators(native);
    }
    const std::wstring openPath = WinPath::addExtendedPrefix(displayPath);
    const uint64_t capBytes = static_cast<uint64_t>(request.maxKb) * 1024ULL;

    // Order is fixed: attributes -> cloud intercept -> open, and GetLastError
    // is read immediately after the failing call.
    const DWORD attributes = GetFileAttributesW(openPath.c_str());
    if (isCloudRecallRequired(attributes)) {
        return AtomicFileWriter::commit(request.tmpPath, request.txtPath,
                                        formatCloudError(displayPath));
    }

    const HANDLE rawInput = CreateFileW(
        openPath.c_str(), GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
        OPEN_EXISTING,
        FILE_FLAG_SEQUENTIAL_SCAN | FILE_FLAG_OPEN_NO_RECALL, nullptr);
    if (rawInput == INVALID_HANDLE_VALUE) {
        const DWORD openError = GetLastError();
        return AtomicFileWriter::commit(
            request.tmpPath, request.txtPath,
            formatOpenError(displayPath, describeIoError(openError)));
    }
    const UniqueHandle input(rawInput);

    LARGE_INTEGER size{};
    if (!GetFileSizeEx(input.get(), &size)) {
        const DWORD sizeError = GetLastError();
        return AtomicFileWriter::commit(
            request.tmpPath, request.txtPath,
            formatOpenError(displayPath, describeIoError(sizeError)));
    }
    const uint64_t totalBytes = static_cast<uint64_t>(size.QuadPart);
    const uint64_t expectedBytes = std::min<uint64_t>(totalBytes, capBytes);

    std::wstring writerError;
    AtomicFileWriter writer;
    if (!writer.begin(request.tmpPath, &writerError)) {
        return {false, writerError};
    }

    std::string content;
    auto flush = [&]() -> bool {
        if (content.empty()) {
            return true;
        }
        if (!writer.write(content.data(), content.size(), &writerError)) {
            return false;
        }
        content.clear();
        return true;
    };
    auto abortWrite = [&]() {
        writer.abandon();
        return AtomicWriteResult{false, writerError};
    };

    content += formatHeader(displayPath, totalBytes, expectedBytes,
                            request.maxKb, request.upper);

    if (totalBytes == 0) {
        content += formatFooter(FooterKind::Empty, totalBytes, request.maxKb);
    }
    else {
        content += "\r\n";
        content += formatColumnHeader(request.upper);

        std::vector<uint8_t> block(kReadBlockBytes);
        uint64_t produced = 0;
        bool readFailed = false;
        std::wstring readReason;
        uint64_t failedAt = 0;

        while (produced < expectedBytes) {
            const size_t want = static_cast<size_t>(std::min<uint64_t>(
                block.size(), expectedBytes - produced));
            size_t got = 0;
            DWORD errorCode = 0;
            const bool readOk
                = readProcedure
                      ? readProcedure(block.data(), want, &got, &errorCode)
                      : readFromHandle(input.get(), block.data(), want, &got,
                                       &errorCode);
            if (!readOk) {
                readFailed = true;
                failedAt = produced;
                readReason = describeIoError(errorCode);
                break;
            }
            if (got == 0) {
                break;  // EOF, e.g. the file shrank while it was being read
            }
            for (size_t offset = 0; offset < got; offset += 16) {
                const size_t chunk = std::min<size_t>(16, got - offset);
                appendLine(content, produced + offset, block.data() + offset,
                           chunk, request.upper);
            }
            produced += got;
            if (content.size() >= kContentFlushBytes && !flush()) {
                return abortWrite();
            }
        }

        if (!flush()) {
            return abortWrite();
        }
        if (readFailed) {
            content += "\r\n";
            content += formatReadError(failedAt, readReason, request.upper);
        }
        else {
            content += "\r\n";
            content += formatFooter(decideFooter(totalBytes, capBytes),
                                    totalBytes, request.maxKb);
        }
    }

    if (!flush()) {
        return abortWrite();
    }
    if (!writer.finish(request.txtPath, &writerError)) {
        return {false, writerError};
    }
    return {true, std::wstring()};
}

}  // namespace hexviewer
