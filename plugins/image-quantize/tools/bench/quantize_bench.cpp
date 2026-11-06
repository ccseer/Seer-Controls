#include "imageio.h"
#include "sha256.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <string>
#include <vector>

#include "seer_quantize.h"

#include <windows.h>
#include <psapi.h>

namespace {

// 17 columns: six stage_ms slots (the sixth is the engine's own total), one
// wall-clock total, then the process and output fields. Keep this in step with
// row_csv or every appended row shifts by one column.
const char* kCsvHeader =
    "scenario,iterations,stage_scan_ms,stage_split_ms,stage_refine_ms,"
    "stage_palette_ms,stage_map_ms,engine_total_ms,wall_total_ms,"
    "peak_workingset_bytes,peak_pagefile_bytes,workspace_bytes,path,"
    "output_sha256,resolved_threads,output_png_bytes,status\n";

struct Options {
    bool selfcheck = false;
    std::string input;
    std::string csv;
    std::string helper;
    std::string cancel_at;
    std::uint32_t cancel_repeats = 9u;
    std::string output_png;
    std::uint32_t colors = 256u;
    std::uint32_t threads = 0u;
    std::uint32_t iterations = 5u;
    std::string dither = "none";
    float dither_strength = 1.0f;
    std::string space = "srgb";
    std::string pipeline = "auto";
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
        std::string value;
        if (key == "--selfcheck") {
            options.selfcheck = true;
        } else if (key == "--input") {
            if (!next(options.input)) return false;
        } else if (key == "--csv") {
            if (!next(options.csv)) return false;
        } else if (key == "--helper") {
            if (!next(options.helper)) return false;
        } else if (key == "--cancel-at") {
            if (!next(options.cancel_at)) return false;
        } else if (key == "--cancel-repeats") {
            if (!next(value)) return false;
            options.cancel_repeats =
                static_cast<std::uint32_t>(std::stoul(value));
        } else if (key == "--output") {
            if (!next(options.output_png)) return false;
        } else if (key == "--colors") {
            if (!next(value)) return false;
            options.colors = static_cast<std::uint32_t>(std::stoul(value));
        } else if (key == "--threads") {
            if (!next(value)) return false;
            options.threads = static_cast<std::uint32_t>(std::stoul(value));
        } else if (key == "--iterations") {
            if (!next(value)) return false;
            options.iterations = static_cast<std::uint32_t>(std::stoul(value));
        } else if (key == "--dither") {
            if (!next(options.dither)) return false;
        } else if (key == "--dither-strength") {
            if (!next(value)) return false;
            options.dither_strength = std::stof(value);
        } else if (key == "--space") {
            if (!next(options.space)) return false;
        } else if (key == "--pipeline") {
            if (!next(options.pipeline)) return false;
        } else {
            error = "unknown option " + key;
            return false;
        }
    }
    return true;
}

struct Row {
    std::string scenario = "engine-selfcheck";
    std::uint32_t iterations = 1u;
    double stage[6] = {};
    double total_ms = 0.0;
    std::uint64_t peak_workingset = 0u;
    std::uint64_t peak_pagefile = 0u;
    std::size_t workspace_bytes = 0u;
    std::uint32_t path = 0u;
    std::string output_sha256;
    std::uint32_t resolved_threads = 0u;
    std::size_t output_png_bytes = 0u;
    std::string status = "ok";
};

void read_process_peaks(Row& row) {
    PROCESS_MEMORY_COUNTERS counters{};
    counters.cb = sizeof(counters);
    if (GetProcessMemoryInfo(GetCurrentProcess(), &counters, sizeof(counters))) {
        row.peak_workingset = static_cast<std::uint64_t>(counters.PeakWorkingSetSize);
        row.peak_pagefile = static_cast<std::uint64_t>(counters.PeakPagefileUsage);
    }
}

std::string row_csv(const Row& row) {
    std::ostringstream out;
    out << std::fixed << std::setprecision(4);
    out << row.scenario << ',' << row.iterations;
    for (std::uint32_t i = 0; i < 6u; ++i) {
        out << ',' << row.stage[i];
    }
    out << ',' << row.total_ms
        << ',' << row.peak_workingset
        << ',' << row.peak_pagefile
        << ',' << row.workspace_bytes
        << ',' << row.path
        << ',' << row.output_sha256
        << ',' << row.resolved_threads
        << ',' << row.output_png_bytes
        << ',' << row.status << '\n';
    return out.str();
}

struct Invocation {
    std::vector<std::uint8_t> pixels;
    std::vector<std::uint8_t> indices;
    sq_image_in image{};
    sq_result result{};
    sq_stats stats{};

    explicit Invocation(std::uint32_t width, std::uint32_t height) {
        const std::size_t count = static_cast<std::size_t>(width) * height;
        pixels.assign(count * 4u, 0u);
        indices.assign(count, 0u);
        for (std::size_t i = 0; i < count; ++i) {
            pixels[i * 4u + 0u] = static_cast<std::uint8_t>(i % 256u);
            pixels[i * 4u + 1u] = 64u;
            pixels[i * 4u + 2u] = 128u;
            pixels[i * 4u + 3u] = 255u;
        }
        image.struct_size = sizeof(sq_image_in);
        image.version = SQ_ABI_VERSION;
        image.pixels = pixels.data();
        image.width = width;
        image.height = height;
        image.stride = static_cast<std::size_t>(width) * 4u;
        result.struct_size = sizeof(sq_result);
        result.version = SQ_ABI_VERSION;
        result.indices = indices.data();
        result.indices_capacity = count;
        stats.struct_size = sizeof(sq_stats);
        stats.version = SQ_ABI_VERSION;
    }
};

std::string output_hash(const Invocation& invocation) {
    std::vector<std::uint8_t> bytes;
    const std::size_t palette_bytes =
        static_cast<std::size_t>(invocation.result.palette_size) * 4u;
    bytes.insert(bytes.end(), invocation.result.palette_rgba,
                 invocation.result.palette_rgba + palette_bytes);
    // The transparent index is part of the output contract: a drift between
    // "index 0 is transparent" and "no transparent index" with identical
    // palette bytes and indices would otherwise stay invisible to the hash.
    const std::uint32_t transparent = invocation.result.transparent_index;
    bytes.push_back(static_cast<std::uint8_t>(transparent & 0xFFu));
    bytes.push_back(static_cast<std::uint8_t>((transparent >> 8) & 0xFFu));
    bytes.insert(bytes.end(), invocation.indices.begin(), invocation.indices.end());
    return sqtools::sha256_hex(bytes);
}

bool run_once(const Options& options, Invocation& invocation, Row& row,
              std::string& error) {
    sq_params params{};
    if (sq_params_init(&params) != SQ_OK) {
        error = "sq_params_init failed";
        return false;
    }
    params.max_colors = options.colors;
    params.thread_count = options.threads;
    params.dither_mode = options.dither == "fs" ? SQ_DITHER_FLOYD_STEINBERG : SQ_DITHER_NONE;
    params.dither_strength = options.dither_strength;
    params.color_space = options.space == "oklab" ? SQ_SPACE_OKLAB : SQ_SPACE_SRGB;
    params.pipeline = options.pipeline == "srgb" ? SQ_PIPELINE_SRGB
                    : options.pipeline == "a"    ? SQ_PIPELINE_OKLAB_A
                    : options.pipeline == "b"    ? SQ_PIPELINE_OKLAB_B
                                                : SQ_PIPELINE_AUTO;

    const auto start = std::chrono::steady_clock::now();
    const sq_status status = sq_quantize(&invocation.image, &params, nullptr, nullptr,
                                         &invocation.result, &invocation.stats);
    const auto finish = std::chrono::steady_clock::now();
    if (status != SQ_OK) {
        error = std::string("sq_quantize failed: ") + sq_status_message(status);
        return false;
    }

    std::vector<std::uint8_t> png;
    if (!sqtools::encode_indexed_png(invocation.indices,
                                     invocation.image.width, invocation.image.height,
                                     invocation.result.palette_rgba,
                                     invocation.result.palette_size, png, &error)) {
        return false;
    }
    if (!options.output_png.empty() &&
        !sqtools::write_file(options.output_png, png, &error)) {
        return false;
    }
    for (std::uint32_t i = 0; i < 6u; ++i) {
        row.stage[i] = invocation.stats.stage_ms[i];
    }
    row.total_ms = std::chrono::duration<double, std::milli>(finish - start).count();
    row.workspace_bytes = invocation.stats.workspace_bytes;
    row.path = invocation.stats.path;
    row.resolved_threads = invocation.stats.thread_count_used;
    row.output_png_bytes = png.size();
    row.output_sha256 = output_hash(invocation);
    read_process_peaks(row);
    return true;
}

void write_csv(const Options& options, const std::vector<Row>& rows) {
    std::string text;
    if (!options.csv.empty()) {
        std::string error;
        std::vector<std::uint8_t> existing;
        const bool have_file = sqtools::read_file(options.csv, existing, &error)
                            && !existing.empty();
        if (!have_file) {
            text += kCsvHeader;
        }
        for (const Row& row : rows) {
            text += row_csv(row);
        }
        std::ofstream stream(options.csv, std::ios::binary | std::ios::app);
        stream.write(text.data(), static_cast<std::streamsize>(text.size()));
    } else {
        text += kCsvHeader;
        for (const Row& row : rows) {
            text += row_csv(row);
        }
        std::fputs(text.c_str(), stdout);
    }
}

// The sweep needs an image that actually reaches the grid path: the 64x64
// bench pattern carries only 256 distinct colours and would take the exact
// path, which finishes without ever polling for cancellation.
void fill_sweep_pattern(Invocation& invocation) {
    std::uint32_t state = 0x9e3779b9u;
    for (std::size_t i = 0; i < invocation.indices.size(); ++i) {
        state = state * 1664525u + 1013904223u;
        const std::uint32_t value = state ^ (state >> 16);
        invocation.pixels[i * 4u + 0u] = static_cast<std::uint8_t>(value);
        invocation.pixels[i * 4u + 1u] = static_cast<std::uint8_t>(value >> 8);
        invocation.pixels[i * 4u + 2u] = static_cast<std::uint8_t>(value >> 16);
        invocation.pixels[i * 4u + 3u] = 255u;
    }
}

// 8.6 measures the cancellation latency from the moment the callback returns
// "cancel" to the moment sq_quantize returns, not the runtime of the whole
// call. The callback stamps QPC when it decides, and the caller closes the
// interval afterwards.
struct CancelProbe {
    std::uint32_t cancel_at = 0u;
    bool fired = false;
    bool returned = false;
    bool late_callback = false;
    std::uint32_t fired_percent = 0u;
    LARGE_INTEGER stamp{};
};

int cancel_callback(std::uint32_t percent, void* user) {
    auto* probe = static_cast<CancelProbe*>(user);
    if (probe->returned) {
        probe->late_callback = true;
        return 1;
    }
    if (!probe->fired && percent >= probe->cancel_at) {
        probe->fired = true;
        probe->fired_percent = percent;
        QueryPerformanceCounter(&probe->stamp);
        return 1;
    }
    return 0;
}

struct CancelSample {
    double latency_ms = 0.0;
    std::uint32_t percent = 0u;
    bool ok = false;
};

bool cancel_once(const char* label, Invocation& invocation, sq_params& params,
                 std::uint32_t percent, CancelSample& sample) {
    CancelProbe probe;
    probe.cancel_at = percent;
    invocation.result.palette_size = 0u;
    invocation.result.transparent_index = SQ_NO_TRANSPARENT_INDEX;
    const sq_status status = sq_quantize(&invocation.image, &params, cancel_callback,
                                         &probe, &invocation.result, &invocation.stats);
    LARGE_INTEGER end{};
    QueryPerformanceCounter(&end);
    probe.returned = true;

    LARGE_INTEGER frequency{};
    QueryPerformanceFrequency(&frequency);
    sample.percent = percent;
    sample.latency_ms = probe.fired
        ? static_cast<double>(end.QuadPart - probe.stamp.QuadPart) * 1000.0
                     / static_cast<double>(frequency.QuadPart)
        : 0.0;
    sample.ok = status == SQ_ERR_CANCELLED
             && invocation.result.palette_size == 0u
             && invocation.result.transparent_index == SQ_NO_TRANSPARENT_INDEX
             && !probe.late_callback;
    std::printf("CANCEL %s %u%%: status=%s fired_at=%u latency=%.4f ms%s\n",
                label, percent, sq_status_message(status), probe.fired_percent,
                sample.latency_ms,
                sample.ok ? "" : " ASSERT_FAIL");
    return sample.ok;
}

bool run_cancellation_sweep(const std::string& argument, sq_params& params,
                            std::uint32_t repeats,
                            const sqtools::Image* external) {
    const std::uint32_t percents[] = {1u, 10u, 25u, 50u, 75u, 90u, 99u};
    struct Class {
        const char* label;
        std::uint32_t width;
        std::uint32_t height;
    };
    std::vector<Class> classes;
    if (argument == "normal") {
        classes = {{"normal", 64u, 64u}};
    } else if (argument == "strip") {
        classes = {{"1x32768", 32768u, 1u}, {"32768x1", 1u, 32768u}};
    } else if (argument == "wide") {
        classes = {{"1x65535", 65535u, 1u}};
    } else if (argument == "all") {
        classes = {{"normal", 64u, 64u},
                   {"1x32768", 32768u, 1u},
                   {"32768x1", 1u, 32768u},
                   {"1x65535", 65535u, 1u}};
    } else {
        std::fprintf(stderr, "quantize_bench: unknown sweep class %s\n",
                     argument.c_str());
        return false;
    }

    bool all_ok = true;
    for (const Class& current : classes) {
        const bool use_external =
            external != nullptr && std::strcmp(current.label, "normal") == 0;
        const std::uint32_t width = use_external ? external->width : current.width;
        const std::uint32_t height =
            use_external ? external->height : current.height;
        Invocation invocation(width, height);
        if (use_external) {
            std::copy(external->rgba.begin(), external->rgba.end(),
                      invocation.pixels.begin());
        } else {
            fill_sweep_pattern(invocation);
        }
        std::vector<CancelSample> samples;
        samples.reserve(static_cast<std::size_t>(repeats) * 7u);
        for (std::uint32_t repeat = 0u; repeat < repeats; ++repeat) {
            for (std::uint32_t percent : percents) {
                CancelSample sample;
                if (!cancel_once(current.label, invocation, params, percent, sample)) {
                    all_ok = false;
                }
                samples.push_back(sample);
            }
        }
        std::vector<double> latencies;
        latencies.reserve(samples.size());
        for (const CancelSample& sample : samples) {
            latencies.push_back(sample.latency_ms);
        }
        std::sort(latencies.begin(), latencies.end());
        const std::size_t p99_index =
            latencies.empty() ? 0u
                              : (latencies.size() * 99u + 99u) / 100u - 1u;
        const double p99 = latencies.empty() ? 0.0 : latencies[p99_index];
        double worst = 0.0;
        std::uint32_t worst_percent = 0u;
        for (const CancelSample& sample : samples) {
            if (sample.latency_ms >= worst) {
                worst = sample.latency_ms;
                worst_percent = sample.percent;
            }
        }
        const bool within = p99 <= 8.0;
        all_ok = all_ok && within;
        std::printf("CANCEL_SUMMARY %s samples=%zu p99=%.4f ms worst=%.4f ms "
                    "at=%u%% ac12_p99_8ms=%s\n",
                    current.label, samples.size(), p99, worst, worst_percent,
                    within ? "PASS" : "FAIL");
    }
    std::printf("CANCEL_RESULT: %s\n", all_ok ? "PASS" : "FAIL");
    return all_ok;
}

bool run_helper(const std::string& helper, const std::string& input, double& milliseconds) {
    std::string command = "\"" + helper + "\" --input \"" + input + "\"";
    const int wide_size = MultiByteToWideChar(CP_UTF8, 0u, command.c_str(), -1, nullptr, 0);
    std::wstring wide(static_cast<std::size_t>(wide_size), L'\0');
    MultiByteToWideChar(CP_UTF8, 0u, command.c_str(), -1, &wide[0], wide_size);
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    const auto start = std::chrono::steady_clock::now();
    const BOOL created = CreateProcessW(nullptr, &wide[0], nullptr, nullptr, FALSE,
                                        CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process);
    if (!created) {
        return false;
    }
    WaitForSingleObject(process.hProcess, INFINITE);
    CloseHandle(process.hProcess);
    CloseHandle(process.hThread);
    const auto finish = std::chrono::steady_clock::now();
    milliseconds = std::chrono::duration<double, std::milli>(finish - start).count();
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    Options options;
    std::string error;
    if (!parse_options(argc, argv, options, error)) {
        std::fprintf(stderr, "quantize_bench: %s\n", error.c_str());
        return 2;
    }

    if (options.selfcheck) {
        Invocation invocation(4u, 4u);
        Row row;
        row.scenario = "engine-selfcheck";
        if (!run_once(options, invocation, row, error)) {
            std::fprintf(stderr, "SELFCHECK FAIL: %s\n", error.c_str());
            return 1;
        }
        sq_params check_params{};
        sq_params_init(&check_params);
        check_params.max_colors = options.colors;
        check_params.thread_count = options.threads;
        const std::size_t expected_workspace =
            sq_workspace_bytes(&check_params, 4u, 4u);
        const bool complete = row.output_png_bytes > 0u
                           && row.output_sha256.size() == 64u
                           && row.resolved_threads > 0u
                           && row.workspace_bytes == expected_workspace;
        std::printf("%s%s", kCsvHeader, row_csv(row).c_str());
        std::printf("SELFCHECK: %s (workspace equals the 10.2 formula)\n",
                    complete ? "PASS" : "FAIL");
        return complete ? 0 : 1;
    }

    sq_params params{};
    if (sq_params_init(&params) != SQ_OK) {
        return 1;
    }
    // The strip classes synthesize their own images; the normal class takes
    // --input when one is supplied so the sweep also covers a real fixture.
    if (!options.cancel_at.empty()) {
        sqtools::Image external;
        const sqtools::Image* external_ptr = nullptr;
        if (!options.input.empty()) {
            if (!sqtools::load_rgba(options.input, external, &error)) {
                std::fprintf(stderr, "quantize_bench: %s\n", error.c_str());
                return 1;
            }
            external_ptr = &external;
        }
        const bool ok = run_cancellation_sweep(options.cancel_at, params,
                                               options.cancel_repeats, external_ptr);
        return ok ? 0 : 1;
    }

    if (options.input.empty()) {
        std::fprintf(stderr,
                     "usage: quantize_bench --input <path> [--csv <path>] "
                     "[--colors 256] [--dither none|fs] "
                     "[--dither-strength <0..1>] [--space srgb|oklab] "
                     "[--pipeline auto|srgb|a|b] [--threads <n>] [--iterations <n>] "
                     "[--cancel-at normal|strip|wide|all] [--cancel-repeats <n>] "
                     "[--helper <path>] | --selfcheck\n");
        return 2;
    }
    if (!options.helper.empty()) {
        double milliseconds = 0.0;
        if (!run_helper(options.helper, options.input, milliseconds)) {
            std::fprintf(stderr, "HELPER_BLOCKED: cannot launch helper\n");
            return 1;
        }
        std::printf("helper end-to-end: %.4f ms\n", milliseconds);
        return 0;
    }

    sqtools::Image image;
    if (!sqtools::load_rgba(options.input, image, &error)) {
        std::fprintf(stderr, "quantize_bench: %s\n", error.c_str());
        return 1;
    }

    Invocation invocation(image.width, image.height);
    std::copy(image.rgba.begin(), image.rgba.end(), invocation.pixels.begin());
    Row warmup;
    if (!run_once(options, invocation, warmup, error)) {
        std::fprintf(stderr, "quantize_bench: %s\n", error.c_str());
        return 1;
    }
    // The CSV is the append target for every phase report, so each row must
    // identify its fixture. A constant label makes appended rows
    // indistinguishable once more than one input is measured.
    const std::size_t separator = options.input.find_last_of("/\\");
    const std::string scenario = separator == std::string::npos
                                     ? options.input
                                     : options.input.substr(separator + 1u);
    std::vector<Row> rows;
    const std::uint32_t iterations = options.iterations == 0u ? 1u : options.iterations;
    for (std::uint32_t i = 0; i < iterations; ++i) {
        Row row;
        if (!run_once(options, invocation, row, error)) {
            std::fprintf(stderr, "quantize_bench: %s\n", error.c_str());
            return 1;
        }
        row.scenario = scenario;
        row.iterations = iterations;
        rows.push_back(row);
    }
    write_csv(options, rows);
    return 0;
}
