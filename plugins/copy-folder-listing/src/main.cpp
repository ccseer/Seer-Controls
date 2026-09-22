#include "controlentry.h"
#include "folderlisting.h"

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int)
{
    return ControlEntry::run([](const std::vector<std::wstring> &arguments) {
        return folderlisting::run(arguments).exitCode;
    });
}
