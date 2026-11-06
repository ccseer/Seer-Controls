#include "imageio.h"
#include "metrics.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <windows.h>

namespace {

struct Options {
    bool selfcheck = false;
    std::string input;
    std::string output;
    std::string baseline;
    std::string out;
    std::string backgrounds = "white,black,checker";
    std::uint32_t colors = 256u;
    bool nofs = true;
    int repeat = 1;
};

bool parse_options(int argc, char** argv, Options& options, std::string& error) {
    for (int i = 1; i < argc; ++i) {
        const std::string key = argv[i];
        auto next = [&](std::string& destination) -> bool {
            if (i + 1 >= argc) {
                error = "missing value for " + key;
                return false;
            }
            destination = argv[++i];
            return true;
        };
        if (key == "--selfcheck") {
            options.selfcheck = true;
        } else if (key == "--input") {
            if (!next(options.input)) return false;
        } else if (key == "--output") {
            if (!next(options.output)) return false;
        } else if (key == "--baseline") {
            if (!next(options.baseline)) return false;
        } else if (key == "--out") {
            if (!next(options.out)) return false;
        } else if (key == "--backgrounds") {
            if (!next(options.backgrounds)) return false;
        } else if (key == "--colors") {
            std::string value;
            if (!next(value)) return false;
            options.colors = static_cast<std::uint32_t>(std::stoul(value));
        } else if (key == "--dither") {
            std::string value;
            if (!next(value)) return false;
            options.nofs = value == "none";
        } else if (key == "--repeat") {
            std::string value;
            if (!next(value)) return false;
            options.repeat = std::stoi(value);
        } else {
            error = "unknown option " + key;
            return false;
        }
    }
    return true;
}

std::wstring widen(const std::string& text) {
    const int size = MultiByteToWideChar(CP_UTF8, 0u, text.c_str(), -1, nullptr, 0);
    if (size <= 0) {
        return std::wstring();
    }
    std::wstring result(static_cast<std::size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, 0u, text.c_str(), -1, &result[0], size);
    result.pop_back();
    return result;
}

std::string quote(const std::string& path) {
    return "\"" + path + "\"";
}

// 6.4: colour count is positional; there is no --colors option.
bool run_baseline_tool(const std::string& executable, const std::string& input,
                  const std::string& output, std::uint32_t colors, bool nofs,
                  std::string& error) {
    if (GetFileAttributesW(widen(executable).c_str()) == INVALID_FILE_ATTRIBUTES) {
        error = "BASELINE_BLOCKED: baseline binary not available at " + executable;
        return false;
    }
    std::string command = quote(executable)
        + " --force --quality=0-100 --speed 1"
        + (nofs ? " --nofs" : "")
        + " --output " + quote(output) + " " + std::to_string(colors)
        + " -- " + quote(input);
    std::wstring wide = widen(command);
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    const BOOL created = CreateProcessW(nullptr, &wide[0], nullptr, nullptr, FALSE,
                                        CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process);
    if (!created) {
        error = "baseline tool launch failed";
        return false;
    }
    WaitForSingleObject(process.hProcess, INFINITE);
    DWORD code = 1u;
    GetExitCodeProcess(process.hProcess, &code);
    CloseHandle(process.hProcess);
    CloseHandle(process.hThread);
    if (code != 0u) {
        error = "baseline tool exited with code " + std::to_string(code);
        return false;
    }
    return true;
}

void emit(const Options& options, const std::string& text) {
    if (options.out.empty()) {
        std::fputs(text.c_str(), stdout);
        return;
    }
    std::string error;
    if (!sqtools::write_file(options.out,
                             std::vector<std::uint8_t>(text.begin(), text.end()), &error)) {
        std::fprintf(stderr, "%s\n", error.c_str());
    }
}

}  // namespace

int main(int argc, char** argv) {
    Options options;
    std::string error;
    if (!parse_options(argc, argv, options, error)) {
        std::fprintf(stderr, "quantize_metrics: %s\n", error.c_str());
        return 2;
    }
    if (options.selfcheck) {
        std::string report;
        const bool ok = sqtools::metrics_selfcheck(report);
        std::fputs(report.c_str(), stdout);
        std::printf("SELFCHECK: %s\n", ok ? "PASS" : "FAIL");
        return ok ? 0 : 1;
    }
    if (options.input.empty() || options.output.empty()) {
        std::fprintf(stderr,
                     "usage: quantize_metrics --input <src> --output <png> "
                     "[--baseline <path>] [--out <report.md>] "
                     "[--colors <n>] [--dither none|fs] [--repeat <n>] | --selfcheck\n");
        return 2;
    }

    sqtools::Image source, output;
    if (!sqtools::load_rgba(options.input, source, &error)
        || !sqtools::load_rgba(options.output, output, &error)) {
        std::fprintf(stderr, "quantize_metrics: %s\n", error.c_str());
        return 1;
    }
    sqtools::Quality quality = sqtools::compute_quality(source, output);
    std::vector<std::uint8_t> source_bytes, output_bytes;
    if (sqtools::read_file(options.input, source_bytes, &error)) {
        quality.source_bytes = static_cast<double>(source_bytes.size());
    }
    if (sqtools::read_file(options.output, output_bytes, &error)) {
        quality.output_bytes = static_cast<double>(output_bytes.size());
    }
    quality.size_ratio = quality.source_bytes > 0.0
                             ? quality.output_bytes / quality.source_bytes : 0.0;
    quality.banding_defined = sqtools::banding_step(output, quality.banding_step);

    std::string baseline_note;
    if (!options.baseline.empty()) {
        const std::string baseline_output = options.output + ".baseline.png";
        if (run_baseline_tool(options.baseline, options.input, baseline_output,
                         options.colors, options.nofs, error)) {
            std::vector<std::uint8_t> baseline_bytes;
            if (sqtools::read_file(baseline_output, baseline_bytes, &error)) {
                quality.baseline_bytes = static_cast<double>(baseline_bytes.size());
                quality.size_vs_baseline = quality.baseline_bytes > 0.0
                    ? quality.output_bytes / quality.baseline_bytes : 0.0;
            }
            baseline_note = "BASELINE: measured\n";
        } else {
            baseline_note = error + "\n";
        }
    } else {
        baseline_note = "BASELINE_BLOCKED: no --baseline supplied\n";
    }

    std::string text = baseline_note
        + "| fixture | dE00 mean | dE00 p95 | CIE76 mean | PSNR dB | SSIM |\n"
        + "| :--- | ---: | ---: | ---: | ---: | ---: |\n"
        + sqtools::quality_markdown_row(options.input, quality) + "\n\n"
        + sqtools::quality_json_row(options.input, quality) + "\n"
        + "NOTE: Phase 0 reports harness validation only; no engine quality number is claimed.\n";
    emit(options, text);
    return 0;
}
