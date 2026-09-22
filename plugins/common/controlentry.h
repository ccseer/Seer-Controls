#pragma once

#include <windows.h>
#include <objbase.h>

#include <string>
#include <vector>

#include "wincmd.h"
#include "winexit.h"
#include "wintext.h"

// The wWinMain every Control helper shares.
//
// COM initialization and the "started with no command line" exit code are
// identical in every package, so only the package's own runner is supplied. The runner turns an argument list into the
// exit code the host compares against the manifest's success_exit_codes.
namespace ControlEntry {

template<typename Invoke>
int run(Invoke invoke)
{
    const HRESULT comResult = CoInitializeEx(
        nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);

    // An empty argument list means CommandLineToArgvW failed, not that the
    // caller passed nothing: the host always passes at least --input. Every
    // nonzero exit has to explain itself on stderr, and this is the one path in
    // the shared entry point that never reaches a package runner.
    const auto arguments = WinCmd::commandLineArguments();
    int result = WinExit::kUsage;
    if (arguments.empty()) {
        WinText::writeStandardError(
            L"cannot read the command line");
    }
    else {
        result = invoke(arguments);
    }
    if (SUCCEEDED(comResult)) {
        CoUninitialize();
    }
    return result;
}

}  // namespace ControlEntry
