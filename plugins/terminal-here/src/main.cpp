#include "controlentry.h"
#include "terminalhere.h"

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int)
{
    return ControlEntry::run([](const std::vector<std::wstring> &arguments) {
        return terminalhere::run(arguments);
    });
}
