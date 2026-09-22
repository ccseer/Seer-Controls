#include "openwith.h"

#include <shlobj.h>

#include <algorithm>

#include "wintext.h"

ParsedInput parseArguments(const std::vector<std::wstring> &arguments,
                           std::wstring *error)
{
    auto fail = [&](const wchar_t *message) {
        if (error)
            *error = message;
        return ParsedInput{};
    };

    if (arguments.size() != 3 || arguments[1] != L"--input")
        return fail(L"expected exactly --input <path>");

    auto path = arguments[2];
    std::replace(path.begin(), path.end(), L'/', L'\\');
    if (path.empty())
        return fail(L"input path is empty");

    const auto attributes = GetFileAttributesW(path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES
        || (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0)
        return fail(L"input must be an existing regular file");

    return ParsedInput{true, path};
}

HRESULT invokeOpenWith(const std::wstring &path, HWND owner)
{
    OPENASINFO info{};
    info.pcszFile    = path.c_str();
    info.pcszClass   = nullptr;
    info.oaifInFlags = OAIF_ALLOW_REGISTRATION | OAIF_EXEC;
    return SHOpenWithDialog(owner, &info);
}

int run(const std::vector<std::wstring> &arguments,
        const OpenWithInvoker &invoker, const ErrorSink &report)
{
    std::wstring error;
    const auto parsed = parseArguments(arguments, &error);
    if (!parsed.valid) {
        report(L"Open With could not start: " + error);
        return kArgumentErrorExitCode;
    }

    const auto result = invoker(parsed.path, nullptr);
    if (FAILED(result)) {
        report(L"Open With could not be shown for the selected file.");
        return kShellFailureExitCode;
    }
    return 0;
}

int run(const std::vector<std::wstring> &arguments)
{
    // Only this overload is reached from wWinMain, so it owns the one sink that
    // writes to the process's real stderr.
    return run(arguments, invokeOpenWith,
               [](const std::wstring &message) {
                   WinText::writeStandardError(message);
               });
}
