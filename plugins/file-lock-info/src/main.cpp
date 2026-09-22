#include "controlentry.h"
#include "filelock.h"

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int)
{
    return ControlEntry::run([](const std::vector<std::wstring> &arguments) {
        return filelock::run(arguments);
    });
}
