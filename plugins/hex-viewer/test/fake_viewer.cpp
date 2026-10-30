#include <windows.h>
#include <shellapi.h>

#include <string>
#include <vector>

// A stand-in SeerViewer used by hexdump_test. It records the argument list it
// was invoked with, one argument per line, into a capture file derived from
// the dump path it was asked to wait for, so the test needs no extra side
// channel and the helper's launch contract is asserted end to end.
int WINAPI wWinMain(HINSTANCE, HINSTANCE, LPWSTR, int)
{
    int count = 0;
    wchar_t **raw = CommandLineToArgvW(GetCommandLineW(), &count);
    if (!raw) {
        return 1;
    }
    std::vector<std::wstring> arguments;
    arguments.reserve(static_cast<size_t>(count));
    for (int index = 0; index < count; ++index) {
        arguments.emplace_back(raw[index]);
    }
    LocalFree(raw);

    std::wstring target;
    for (size_t index = 0; index + 1 < arguments.size(); ++index) {
        if (arguments[index] == L"--") {
            target = arguments[index + 1];
            break;
        }
    }
    if (target.empty()) {
        return 2;
    }

    std::wstring content;
    for (size_t index = 1; index < arguments.size(); ++index) {
        content += arguments[index];
        content += L"\n";
    }

    const std::wstring capturePath = target + L".fakeviewercapture.txt";
    HANDLE handle = CreateFileW(capturePath.c_str(), GENERIC_WRITE, 0,
                                nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL,
                                nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        return 3;
    }
    const int bytesNeeded = WideCharToMultiByte(
        CP_UTF8, 0, content.c_str(), static_cast<int>(content.size()), nullptr,
        0, nullptr, nullptr);
    if (bytesNeeded > 0) {
        std::string encoded(static_cast<size_t>(bytesNeeded), '\0');
        WideCharToMultiByte(CP_UTF8, 0, content.c_str(),
                            static_cast<int>(content.size()), encoded.data(),
                            bytesNeeded, nullptr, nullptr);
        DWORD written = 0;
        WriteFile(handle, encoded.data(),
                  static_cast<DWORD>(encoded.size()), &written, nullptr);
    }
    CloseHandle(handle);
    return 0;
}
