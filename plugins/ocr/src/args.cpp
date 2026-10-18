#include "args.h"

#include <windows.h>
#include <shellapi.h>
#include <algorithm>

namespace seer_ocr {

namespace {

std::wstring toLower(std::wstring str)
{
    std::transform(str.begin(), str.end(), str.begin(), ::towlower);
    return str;
}

bool startsWith(const std::wstring &str, const std::wstring &prefix)
{
    return str.rfind(prefix, 0) == 0;
}

}  // namespace

OcrCliOptions parseCommandLine(int argc, wchar_t *argv[])
{
    std::vector<std::wstring> args;
    args.reserve(argc);
    for (int i = 0; i < argc; ++i) {
        if (argv[i]) {
            args.emplace_back(argv[i]);
        }
    }
    return parseCommandLine(args);
}

OcrCliOptions parseCommandLine(const std::vector<std::wstring> &args)
{
    OcrCliOptions opts;
    std::vector<std::wstring> positionals;

    for (size_t i = 1; i < args.size(); ++i) {
        const auto &arg = args[i];
        if (arg == L"--console") {
            opts.console = true;
        }
        else if (startsWith(arg, L"--ep=")) {
            const auto val = toLower(arg.substr(5));
            if (val == L"dml") {
                opts.ep = ExecutionProvider::Dml;
            }
            else if (val == L"cpu") {
                opts.ep = ExecutionProvider::Cpu;
            }
            else if (val == L"auto") {
                opts.ep = ExecutionProvider::Auto;
            }
            else {
                opts.error_msg = L"Invalid --ep value: " + val;
                return opts;
            }
        }
        else if (arg == L"--ep" && i + 1 < args.size()) {
            const auto val = toLower(args[++i]);
            if (val == L"dml") {
                opts.ep = ExecutionProvider::Dml;
            }
            else if (val == L"cpu") {
                opts.ep = ExecutionProvider::Cpu;
            }
            else if (val == L"auto") {
                opts.ep = ExecutionProvider::Auto;
            }
            else {
                opts.error_msg = L"Invalid --ep value: " + val;
                return opts;
            }
        }
        else if (startsWith(arg, L"--cls=")) {
            const auto val = arg.substr(6);
            opts.enable_cls = (val != L"0");
        }
        else if (arg == L"--cls" && i + 1 < args.size()) {
            const auto val = args[++i];
            opts.enable_cls = (val != L"0");
        }
        else if (startsWith(arg, L"--max-side=")) {
            try {
                opts.max_side = std::stoi(arg.substr(11));
                if (opts.max_side < 128) {
                    opts.max_side = 128;
                }
            }
            catch (...) {
                opts.error_msg = L"Bad --max-side: " + arg;
                return opts;
            }
        }
        else if (arg == L"--max-side" && i + 1 < args.size()) {
            try {
                opts.max_side = std::stoi(args[++i]);
                if (opts.max_side < 128) {
                    opts.max_side = 128;
                }
            }
            catch (...) {
                opts.error_msg = L"Bad --max-side: " + args[i];
                return opts;
            }
        }
        else if (startsWith(arg, L"--wait-timeout=")) {
            try {
                opts.wait_timeout_ms = std::stoi(arg.substr(15));
                if (opts.wait_timeout_ms < 5000) {
                    opts.wait_timeout_ms = 5000;
                }
                else if (opts.wait_timeout_ms > 600000) {
                    opts.wait_timeout_ms = 600000;
                }
            }
            catch (...) {
                opts.error_msg = L"Bad --wait-timeout: " + arg;
                return opts;
            }
        }
        else if (arg == L"--wait-timeout" && i + 1 < args.size()) {
            try {
                opts.wait_timeout_ms = std::stoi(args[++i]);
                if (opts.wait_timeout_ms < 5000) {
                    opts.wait_timeout_ms = 5000;
                }
                else if (opts.wait_timeout_ms > 600000) {
                    opts.wait_timeout_ms = 600000;
                }
            }
            catch (...) {
                opts.error_msg = L"Bad --wait-timeout: " + args[i];
                return opts;
            }
        }
        else if (startsWith(arg, L"--force-dml-fail=")) {
            const auto val = toLower(arg.substr(17));
            if (val == L"init") {
                opts.force_fail = ForceDmlFail::Init;
            }
            else if (val == L"run") {
                opts.force_fail = ForceDmlFail::Run;
            }
            else {
                opts.error_msg = L"Invalid --force-dml-fail value: " + val;
                return opts;
            }
        }
        else if (arg == L"--force-dml-fail" && i + 1 < args.size()) {
            const auto val = toLower(args[++i]);
            if (val == L"init") {
                opts.force_fail = ForceDmlFail::Init;
            }
            else if (val == L"run") {
                opts.force_fail = ForceDmlFail::Run;
            }
            else {
                opts.error_msg = L"Invalid --force-dml-fail value: " + val;
                return opts;
            }
        }
        else if (startsWith(arg, L"-")) {
            opts.error_msg = L"Unknown flag: " + arg;
            return opts;
        }
        else {
            positionals.push_back(arg);
        }
    }

    if (positionals.size() < 3) {
        opts.error_msg = L"Missing positional arguments (expected screenshot, seerviewer_exe, input_file)";
        if (positionals.size() >= 1) opts.screenshot_path = positionals[0];
        if (positionals.size() >= 2) opts.seerviewer_exe = positionals[1];
        return opts;
    }

    opts.screenshot_path = positionals[0];
    opts.seerviewer_exe  = positionals[1];
    opts.input_file      = positionals[2];
    opts.valid           = true;
    return opts;
}

}  // namespace seer_ocr
