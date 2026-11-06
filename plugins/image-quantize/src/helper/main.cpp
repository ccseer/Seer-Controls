#include <windows.h>

#include "quantizerunner.h"
#include "winexit.h"

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argv == nullptr) {
        iq::write_error(L"the command line could not be read");
        return WinExit::kUsage;
    }

    const HRESULT hr = CoInitializeEx(
        nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    if (FAILED(hr)) {
        LocalFree(argv);
        iq::write_error(L"COM initialization failed");
        return WinExit::kMissingDependency;
    }

    const int code = iq::run(argc, argv);
    CoUninitialize();
    LocalFree(argv);
    return code;
}
