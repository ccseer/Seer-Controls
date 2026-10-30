#include <windows.h>

#include <string>
#include <vector>

#include "atomicfilewriter.h"
#include "controlentry.h"
#include "hexdump.h"
#include "hexlauncher.h"
#include "hexoptions.h"
#include "hexpaths.h"
#include "hexsweep.h"
#include "winexit.h"
#include "winpath.h"
#include "wintext.h"

namespace hexviewer {
namespace {

constexpr wchar_t kErrorTitle[] = L"Seer Hex View Error";
constexpr wchar_t kPackageName[] = L"hex-viewer";

// CI and CTest runs set SEER_TEST_NO_UI=1 so every refusal lands on stderr
// (ACP encoded, like the shared WinText writer) instead of blocking on a
// modal message box. Package-internal test hook, not a user option.
bool uiSuppressed()
{
    wchar_t value[8] = {};
    const DWORD read = GetEnvironmentVariableW(L"SEER_TEST_NO_UI", value, 8);
    return read > 0 && read < 8 && std::wstring(value) == L"1";
}

void showErrorDialog(const std::wstring &message)
{
    if (uiSuppressed()) {
        WinText::writeStandardError(std::wstring(kErrorTitle) + L": " + message);
        return;
    }
    MessageBoxW(nullptr, message.c_str(), kErrorTitle, MB_ICONERROR | MB_OK);
}

std::wstring executableDirectory()
{
    std::vector<wchar_t> buffer(32768, L'\0');
    const DWORD length = GetModuleFileNameW(nullptr, buffer.data(),
                                            static_cast<DWORD>(buffer.size()));
    return WinPath::parentDirectory(std::wstring(buffer.data(), length));
}

// Removes a dump published by an earlier invocation. A missing file is
// success. Any other failure means the target is still held by somebody else,
// and neither the atomic rename nor the fallback direct write can replace it
// either, so the caller must refuse instead of launching a window that would
// show the previous dump.
bool removePublishedDump(const std::wstring &path, DWORD *error)
{
    const DWORD attributes = WinPath::attributesOf(path);
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        return true;
    }
    if ((attributes & FILE_ATTRIBUTE_READONLY) != 0) {
        SetFileAttributesW(WinPath::addExtendedPrefix(path).c_str(),
                           attributes & ~FILE_ATTRIBUTE_READONLY);
    }
    if (DeleteFileW(WinPath::addExtendedPrefix(path).c_str())) {
        return true;
    }
    const DWORD failure = GetLastError();
    if (failure == ERROR_FILE_NOT_FOUND || failure == ERROR_PATH_NOT_FOUND) {
        return true;
    }
    if (error) {
        *error = failure;
    }
    return false;
}

int runHelper(const std::vector<std::wstring> &arguments)
{
    // 1. Parse and clamp; every refusal exits with the usage code.
    const HexOptions options = parseCommandLine(arguments);
    if (!options.valid) {
        showErrorDialog(L"Invalid arguments: " + options.error);
        return WinExit::kUsage;
    }

    // 2. The host expands ${seerviewer_exe} without an existence check, so the
    //    helper validates it no matter what.
    if (!WinPath::isRegularFile(options.viewerPath)) {
        showErrorDialog(L"SeerViewer executable not found at specified path.");
        return WinExit::kFailure;
    }

    // 3. Resolve where the dump lives: the package directory when writable,
    //    the namespaced temp root otherwise, and a hard failure naming both
    //    candidates when neither is usable.
    const std::wstring exeDirectory = executableDirectory();
    const std::wstring fallbackTempRoot = WinPath::join(
        WinPath::join(WinPath::systemTempDirectory(), L"SeerControlPlugins"),
        kPackageName);
    const WinPath::OwnedRoot ownedRoot
        = WinPath::resolveOwnedRoot(exeDirectory, kPackageName);
    if (ownedRoot.path.empty()) {
        std::wstring message = L"Cannot find a writable location for hex dumps.";
        message += L"\nTried: '" + exeDirectory + L"'";
        message += L"\nTried: '" + fallbackTempRoot + L"'";
        if (!ownedRoot.note.empty()) {
            message += L"\n" + ownedRoot.note;
        }
        showErrorDialog(message);
        return WinExit::kFailure;
    }

    const SafeOutputPath output = safeOutputPath(ownedRoot.path,
                                                 fallbackTempRoot,
                                                 options.inputPath);
    if (!output.ok) {
        showErrorDialog(L"Cannot build a safe output path for the hex dump.");
        return WinExit::kFailure;
    }

    std::wstring writableReason;
    if (!WinPath::isWritableDirectory(output.targetDir, &writableReason)) {
        showErrorDialog(L"Cannot write to the hex dump directory: "
                        + output.targetDir + L" (" + writableReason + L")");
        return WinExit::kFailure;
    }

    // 4. Remove a dump of the same name: the waiting window accepts a file
    //    whose mtime is within 3 seconds, so a stale result could otherwise be
    //    served as fresh on a rapid second trigger. A target that cannot be
    //    removed cannot be replaced afterwards either, so stop here rather
    //    than open a window onto the previous dump.
    DWORD removeError = 0;
    if (!removePublishedDump(output.txtPath, &removeError)) {
        showErrorDialog(L"Cannot replace the previous hex dump: "
                        + output.txtPath + L" ("
                        + WinText::systemErrorMessage(removeError) + L")");
        return WinExit::kFailure;
    }

    // 5. Launch first so the independent window shows its pending state at
    //    once; everything after this point reports through the dump file.
    if (!launchSeerViewerWaiting(options.viewerPath, output.txtPath,
                                 options.waitTimeoutMs)) {
        showErrorDialog(L"Failed to launch SeerViewer.");
        return WinExit::kFailure;
    }

    // 6. Generate and atomically publish. When even the rename fails, the
    //    direct write is the last resort that still hands the waiting window a
    //    diagnostic line; if that fails too, the window shows its own timeout
    //    page and the message box is the only signal left.
    HexDumpRequest request;
    request.inputPath = options.inputPath;
    request.txtPath = output.txtPath;
    request.tmpPath = tmpFileName(
        output.txtPath, GetCurrentProcessId(),
        static_cast<unsigned long>(GetTickCount64() & 0xFFFFFFFFUL));
    request.maxKb = options.maxKb;
    request.upper = options.upper;

    const AtomicWriteResult result = generate(request);
    if (!result.ok) {
        const std::string diagnostic
            = "[Seer Hex Dump Error] Cannot write dump file ("
              + WinText::toUtf8(result.errorMessage) + ").\r\n";
        const AtomicWriteResult direct
            = AtomicFileWriter::writeDirect(output.txtPath, diagnostic);
        if (!direct.ok) {
            showErrorDialog(L"Cannot write the hex dump file: "
                            + result.errorMessage);
            return WinExit::kFailure;
        }
    }

    // 7. Sweep last so it never delays the first frame.
    sweepOldDumps(output.targetDir);

    return WinExit::kOk;
}

}  // namespace

}  // namespace hexviewer

int WINAPI wWinMain(HINSTANCE, HINSTANCE, LPWSTR, int)
{
    return ControlEntry::run([](const std::vector<std::wstring> &arguments) {
        return hexviewer::runHelper(arguments);
    });
}
