// Shared test fixture for the native Control packages.
//
// It is deliberately a separate process so the tests can observe the real
// Seer -> helper -> child boundary: the argument vector that survived CRT
// serialization, the working directory the child actually inherited, and
// whether the child landed inside the containment job.
//
// usage: probe_child.exe <outfile> <mode> [mode-arguments] [payload...]
//   modes: echo | sleep <ms> | hold-file <path> <ms>
// Payload arguments are echoed back so an argv round trip can be asserted on
// arbitrary bytes, including quotes, backslashes and non-ASCII text.

#include <windows.h>

#include <string>
#include <vector>

namespace {

std::string utf8(const std::wstring &value)
{
    if (value.empty()) {
        return std::string();
    }
    const int size = WideCharToMultiByte(CP_UTF8, 0, value.data(),
                                         static_cast<int>(value.size()),
                                         nullptr, 0, nullptr, nullptr);
    std::string result(static_cast<size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
                        result.data(), size, nullptr, nullptr);
    return result;
}

void append(std::string *sink, const std::string &line)
{
    sink->append(line);
    sink->append("\r\n");
}

}  // namespace

int main()
{
    int count  = 0;
    auto argv  = CommandLineToArgvW(GetCommandLineW(), &count);
    if (!argv) {
        return 3;
    }
    std::vector<std::wstring> arguments;
    for (int index = 0; index < count; ++index) {
        arguments.emplace_back(argv[index]);
    }
    LocalFree(argv);

    if (arguments.size() < 3) {
        return 2;
    }
    const std::wstring outFile   = arguments[1];
    const std::wstring mode      = arguments[2];
    const std::wstring extraOne  = arguments.size() > 3 ? arguments[3]
                                                       : std::wstring();
    const std::wstring extraTwo  = arguments.size() > 4 ? arguments[4]
                                                       : std::wstring();

    std::string report;
    append(&report, "argc=" + std::to_string(static_cast<int>(
                              arguments.size())));
    for (size_t index = 0; index < arguments.size(); ++index) {
        append(&report,
               "argv" + std::to_string(index) + "=" + utf8(arguments[index]));
    }

    wchar_t directory[32768]{};
    const DWORD length = GetCurrentDirectoryW(32768, directory);
    append(&report,
           "cwd=" + utf8(std::wstring(directory, length)));

    BOOL inJob = FALSE;
    IsProcessInJob(GetCurrentProcess(), nullptr, &inJob);
    append(&report, std::string("injob=") + (inJob ? "1" : "0"));
    if (inJob) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        if (QueryInformationJobObject(nullptr, JobObjectExtendedLimitInformation,
                                     &limits, sizeof(limits), nullptr)) {
            const bool killOnClose
                = (limits.BasicLimitInformation.LimitFlags
                   & JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE)
                  != 0;
            append(&report,
                   std::string("jobkillonclose=") + (killOnClose ? "1" : "0"));
        }
    }

    DWORD sleepMs = 0;
    HANDLE held   = INVALID_HANDLE_VALUE;
    if (mode == L"sleep") {
        sleepMs = static_cast<DWORD>(_wtoi(extraOne.c_str()));
    }
    else if (mode == L"hold-file") {
        held = CreateFileW(extraOne.c_str(), GENERIC_READ | GENERIC_WRITE, 0,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL,
                           nullptr);
        append(&report, std::string("held=") + (held != INVALID_HANDLE_VALUE
                                                    ? "1"
                                                    : "0"));
        sleepMs = static_cast<DWORD>(_wtoi(extraTwo.c_str()));
    }

    HANDLE output = CreateFileW(outFile.c_str(), GENERIC_WRITE, 0, nullptr,
                                CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (output != INVALID_HANDLE_VALUE) {
        DWORD written = 0;
        WriteFile(output, report.data(), static_cast<DWORD>(report.size()),
                  &written, nullptr);
        CloseHandle(output);
    }

    if (sleepMs > 0) {
        Sleep(sleepMs);
    }
    if (held != INVALID_HANDLE_VALUE) {
        CloseHandle(held);
    }
    return 0;
}
