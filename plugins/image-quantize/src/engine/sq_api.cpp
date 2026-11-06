#include "include/seer_quantize.h"
#include "sq_internal.h"
#include "sq_kmeans.h"
#include "sq_dither.h"

#include <chrono>
#include <cstring>
#include <limits>

namespace {

template <typename T>
sq_status check_descriptor(const T* descriptor) {
    if (descriptor == nullptr) {
        return SQ_ERR_INVALID_ARG;
    }
    if (descriptor->struct_size < sizeof(T)) {
        return SQ_ERR_UNSUPPORTED_VERSION;
    }
    if (descriptor->version != SQ_ABI_VERSION) {
        return SQ_ERR_UNSUPPORTED_VERSION;
    }
    return SQ_OK;
}

bool all_zero(const std::uint32_t* values, std::size_t count) {
    for (std::size_t i = 0; i < count; ++i) {
        if (values[i] != 0u) {
            return false;
        }
    }
    return true;
}

sq_status cancelled_result(sq_result* result) {
    result->palette_size = 0;
    result->transparent_index =
        static_cast<std::uint16_t>(SQ_NO_TRANSPARENT_INDEX);
    return SQ_ERR_CANCELLED;
}

double elapsed_ms(std::chrono::steady_clock::time_point from,
                  std::chrono::steady_clock::time_point to) {
    return std::chrono::duration<double, std::milli>(to - from).count();
}

}  // namespace

extern "C" std::uint32_t sq_abi_version(void) {
    return SQ_ABI_VERSION;
}

extern "C" sq_status sq_params_init(sq_params* params) {
    if (params == nullptr) {
        return SQ_ERR_INVALID_ARG;
    }
    std::memset(params, 0, sizeof(*params));
    params->struct_size = static_cast<std::uint32_t>(sizeof(sq_params));
    params->version = SQ_ABI_VERSION;
    params->max_colors = 256u;
    params->alpha_mode = SQ_ALPHA_STRAIGHT;
    params->color_space = SQ_SPACE_SRGB;
    params->dither_mode = SQ_DITHER_NONE;
    params->dither_strength = 0.0f;
    params->kmeans_max_iter = 20u;
    params->kmeans_tolerance = 1e-4f;
    params->thread_count = SQ_AUTO_THREADS;
    params->pipeline = SQ_PIPELINE_AUTO;
    return SQ_OK;
}

extern "C" sq_status sq_params_validate(const sq_params* params) {
    const sq_status header = check_descriptor(params);
    if (header != SQ_OK) {
        return header;
    }
    if (params->max_colors < 2u || params->max_colors > SQ_MAX_PALETTE_ENTRIES) {
        return SQ_ERR_INVALID_ARG;
    }
    if (params->alpha_mode != SQ_ALPHA_STRAIGHT &&
        params->alpha_mode != SQ_ALPHA_PREMULTIPLIED) {
        return SQ_ERR_INVALID_ARG;
    }
    if (params->color_space != SQ_SPACE_SRGB &&
        params->color_space != SQ_SPACE_OKLAB) {
        return SQ_ERR_INVALID_ARG;
    }
    if (params->dither_mode != SQ_DITHER_NONE &&
        params->dither_mode != SQ_DITHER_FLOYD_STEINBERG) {
        return SQ_ERR_INVALID_ARG;
    }
    if (params->pipeline > SQ_PIPELINE_OKLAB_B) {
        return SQ_ERR_INVALID_ARG;
    }
    if (params->kmeans_max_iter > 1000u || params->thread_count > 256u) {
        return SQ_ERR_INVALID_ARG;
    }
    if (!all_zero(params->reserved0, 3u)) {
        return SQ_ERR_INVALID_ARG;
    }

    // Written as !(x >= lo && x <= hi) so NaN and infinity fail the test.
    // Comparisons stay in float: 0.1f promoted to double is slightly above
    // 0.1, which would reject the documented inclusive upper bound.
    const float strength = params->dither_strength;
    if (!(strength >= 0.0f && strength <= 1.0f)) {
        return SQ_ERR_INVALID_ARG;
    }
    const float tolerance = params->kmeans_tolerance;
    if (!(tolerance > 0.0f && tolerance <= 0.1f)) {
        return SQ_ERR_INVALID_ARG;
    }
    return SQ_OK;
}

extern "C" std::size_t sq_workspace_bytes(const sq_params* params,
                                          std::uint32_t width,
                                          std::uint32_t height) {
    (void)height;  // the formula is sized from width and the worker count only
    if (params == nullptr) {
        return 0u;
    }
    const std::uint32_t resolved = sq::resolve_thread_count(params->thread_count);
    const std::uint32_t workers = sq::scan_worker_count(resolved);
    return sq::core_bytes(width, workers, 4096u);
}

extern "C" const char* sq_status_message(sq_status status) {
    switch (status) {
        case SQ_OK: return "ok";
        case SQ_ERR_INVALID_ARG: return "invalid argument";
        case SQ_ERR_UNSUPPORTED_VERSION: return "unsupported version";
        case SQ_ERR_BUFFER_TOO_SMALL: return "buffer too small";
        case SQ_ERR_CANCELLED: return "cancelled";
        case SQ_ERR_OUT_OF_MEMORY: return "out of memory";
        case SQ_ERR_INTERNAL: return "internal error";
        default: return "unknown status";
    }
}

extern "C" sq_status sq_quantize(const sq_image_in* input,
                                 const sq_params* params,
                                 sq_progress_fn progress_fn,
                                 void* progress_user,
                                 sq_result* result,
                                 sq_stats* stats) {
    const sq_status params_status = sq_params_validate(params);
    if (params_status != SQ_OK) {
        return params_status;
    }
    const sq_status input_header = check_descriptor(input);
    if (input_header != SQ_OK) {
        return input_header;
    }
    const sq_status result_header = check_descriptor(result);
    if (result_header != SQ_OK) {
        return result_header;
    }
    if (stats != nullptr) {
        const sq_status stats_header = check_descriptor(stats);
        if (stats_header != SQ_OK) {
            return stats_header;
        }
    }
    if (input->pixels == nullptr || result->indices == nullptr) {
        return SQ_ERR_INVALID_ARG;
    }
    if (!all_zero(input->reserved0, 2u)) {
        return SQ_ERR_INVALID_ARG;
    }

    const std::uint32_t width = input->width;
    const std::uint32_t height = input->height;
    if (width == 0u || width > SQ_MAX_DIMENSION || height == 0u ||
        height > SQ_MAX_DIMENSION) {
        return SQ_ERR_INVALID_ARG;
    }
    const std::size_t width_size = static_cast<std::size_t>(width);
    const std::size_t height_size = static_cast<std::size_t>(height);
    if (width_size > (std::numeric_limits<std::size_t>::max)() / 4u) {
        return SQ_ERR_INVALID_ARG;
    }
    if (width_size > static_cast<std::size_t>(SQ_MAX_PIXELS) / height_size) {
        return SQ_ERR_INVALID_ARG;
    }
    const std::size_t pixels = width_size * height_size;
    const std::size_t row_bytes = width_size * 4u;
    if (input->stride < row_bytes) {
        return SQ_ERR_INVALID_ARG;
    }
    if (input->stride >
        (std::numeric_limits<std::size_t>::max)() / height_size) {
        return SQ_ERR_INVALID_ARG;
    }
    if (result->indices_capacity < pixels) {
        return SQ_ERR_BUFFER_TOO_SMALL;
    }

    sq::Workspace workspace;
    if (!workspace.allocate(*params, width, height)) {
        return SQ_ERR_OUT_OF_MEMORY;
    }
    if (workspace.live_bytes() != sq_workspace_bytes(params, width, height)) {
        return SQ_ERR_INTERNAL;
    }

    const auto start = std::chrono::steady_clock::now();
    sq::Progress progress;
    progress.fn = progress_fn;
    progress.user = progress_user;
    progress.publish(0u);

    sq::ScanResult scan;
    if (!sq::scan_image(*input, *params, workspace, progress, scan)) {
        if (progress.cancelled()) {
            return cancelled_result(result);
        }
        return SQ_ERR_OUT_OF_MEMORY;
    }
    const auto after_scan = std::chrono::steady_clock::now();
    const double scan_ms = elapsed_ms(start, after_scan);
    if (progress.cancelled()) {
        return cancelled_result(result);
    }

    const bool oklab = sq::use_oklab(*params);
    const std::uint32_t transparent_budget =
        scan.has_transparent ? 1u : 0u;
    const std::uint32_t opaque_budget =
        params->max_colors - transparent_budget;

    sq_path path = SQ_PATH_GRID;
    if (scan.probe_state == 0u &&
        scan.distinct_colors + transparent_budget <= params->max_colors) {
        path = SQ_PATH_EXACT;
    } else if (scan.probe_state == 0u &&
               scan.occupied_cells < opaque_budget) {
        path = SQ_PATH_DEGENERATE;
    } else if (scan.probe_state == 1u &&
               scan.occupied_cells < opaque_budget) {
        if (!sq::escalate_probe(*input, *params, workspace, progress, scan)) {
            if (progress.cancelled()) {
                return cancelled_result(result);
            }
            return SQ_ERR_OUT_OF_MEMORY;
        }
        scan.exact = workspace.exact_table();
        path = scan.probe_state == 0u ? SQ_PATH_DEGENERATE : SQ_PATH_GRID;
    }

    sq::Palette* palette = reinterpret_cast<sq::Palette*>(workspace.misc());
    double split_ms = 0.0;
    double refine_ms = 0.0;
    double palette_ms = 0.0;
    double map_ms = 0.0;
    std::uint32_t kmeans_iterations = 0u;
    std::uint32_t band_start = 42u;
    std::uint32_t band_width = 57u;

    if (path == SQ_PATH_EXACT) {
        const auto palette_start = std::chrono::steady_clock::now();
        if (!sq::build_exact_palette(scan, *palette)) {
            return SQ_ERR_INTERNAL;
        }
        palette_ms = elapsed_ms(palette_start, std::chrono::steady_clock::now());
        progress.publish(42u);
        if (progress.cancelled()) {
            return cancelled_result(result);
        }
    } else if (path == SQ_PATH_DEGENERATE) {
        progress.publish(40u);
        progress.publish(43u);
        const auto refine_start = std::chrono::steady_clock::now();
        const std::uint32_t point_count =
            sq::collect_points(scan, workspace, workspace.points(), false);
        if (oklab) {
            sq::convert_points_to_oklab(workspace.points(), point_count);
        }
        sq::RefineInfo info;
        if (!sq::build_clustered_palette(
                workspace.points(), point_count, params->max_colors,
                scan.has_transparent, params->kmeans_max_iter,
                params->kmeans_tolerance, workspace.threads(),
                workspace.shards(), progress, *palette, info, oklab)) {
            if (progress.cancelled()) {
                return cancelled_result(result);
            }
            return SQ_ERR_INTERNAL;
        }
        kmeans_iterations = info.iterations;
        refine_ms = elapsed_ms(refine_start, std::chrono::steady_clock::now());
        if (progress.cancelled()) {
            return cancelled_result(result);
        }
        progress.publish(65u);
        progress.publish(67u);
        band_start = 67u;
        band_width = 32u;
    } else {
        progress.publish(40u);
        const auto split_start = std::chrono::steady_clock::now();
        if (!sq::build_wu_palette(
                workspace.histogram(0u), workspace.cumulative(),
                params->max_colors, scan.has_transparent, workspace.misc(),
                *palette, &progress)) {
            if (progress.cancelled()) {
                return cancelled_result(result);
            }
            return SQ_ERR_INTERNAL;
        }
        const sq::Palette initial = *palette;
        split_ms = elapsed_ms(split_start, std::chrono::steady_clock::now());
        if (progress.cancelled()) {
            return cancelled_result(result);
        }
        progress.publish(43u);
        const auto refine_start = std::chrono::steady_clock::now();
        const std::uint32_t point_count =
            sq::collect_points(scan, workspace, workspace.points(), true);
        if (oklab) {
            sq::convert_points_to_oklab(workspace.points(), point_count);
        }
        sq::RefineInfo info;
        if (!sq::refine_palette(
                workspace.points(), point_count, initial,
                params->kmeans_max_iter, params->kmeans_tolerance,
                workspace.threads(), workspace.shards(), progress, *palette,
                info, nullptr, 0u, oklab)) {
            if (progress.cancelled()) {
                return cancelled_result(result);
            }
            return SQ_ERR_INTERNAL;
        }
        kmeans_iterations = info.iterations;
        refine_ms = elapsed_ms(refine_start, std::chrono::steady_clock::now());
        if (progress.cancelled()) {
            return cancelled_result(result);
        }
        progress.publish(65u);
        progress.publish(67u);
        band_start = 67u;
        band_width = 32u;
    }

    // A callback that asked to cancel at the 65 or 67 checkpoint has already
    // returned. Spawning the mapping workers now would only add their start-up
    // cost to the measured cancellation latency.
    if (progress.cancelled()) {
        return cancelled_result(result);
    }

    const auto map_start = std::chrono::steady_clock::now();
    bool mapped = false;
    if (path == SQ_PATH_EXACT) {
        mapped = sq::map_exact(*input, *params, scan, *palette, result->indices,
                               workspace, progress, band_start, band_width);
    } else if (params->dither_mode == SQ_DITHER_FLOYD_STEINBERG &&
               params->dither_strength > 0.0f) {
        mapped = sq::map_dither(*input, *params, *palette, result->indices,
                                workspace, progress, band_start, band_width,
                                oklab);
    } else {
        mapped = sq::map_nearest(*input, *params, *palette, result->indices,
                                 workspace, progress, band_start, band_width,
                                 oklab);
    }
    if (!mapped) {
        if (progress.cancelled()) {
            return cancelled_result(result);
        }
        return SQ_ERR_INTERNAL;
    }
    map_ms = elapsed_ms(map_start, std::chrono::steady_clock::now());

    result->palette_size = palette->size;
    result->transparent_index = palette->transparent_index;
    std::memset(result->palette_rgba, 0, sizeof(result->palette_rgba));
    for (std::uint32_t i = 0u; i < palette->size; ++i) {
        const std::uint32_t value = palette->rgba[i];
        result->palette_rgba[i * 4u + 0u] =
            static_cast<std::uint8_t>((value >> 24) & 0xFFu);
        result->palette_rgba[i * 4u + 1u] =
            static_cast<std::uint8_t>((value >> 16) & 0xFFu);
        result->palette_rgba[i * 4u + 2u] =
            static_cast<std::uint8_t>((value >> 8) & 0xFFu);
        result->palette_rgba[i * 4u + 3u] =
            static_cast<std::uint8_t>(value & 0xFFu);
    }

    if (stats != nullptr) {
        const auto finish = std::chrono::steady_clock::now();
        stats->path = static_cast<std::uint32_t>(path);
        stats->probe_state = scan.probe_state;
        stats->unique_colors = scan.distinct_colors;
        stats->occupied_cells = scan.occupied_cells;
        stats->palette_size = palette->size;
        stats->kmeans_iterations = kmeans_iterations;
        stats->thread_count_used = workspace.threads();
        for (double& value : stats->stage_ms) {
            value = 0.0;
        }
        stats->stage_ms[0] = scan_ms;
        stats->stage_ms[1] = split_ms;
        stats->stage_ms[2] = refine_ms;
        // On the grid and degenerate paths, palette extraction and deduplication
        // are merged into refine_palette or build_clustered_palette; palette_ms
        // is only measured separately on the exact path, so stage_ms[3] is 0.0
        // on non-exact paths by design.
        stats->stage_ms[3] = palette_ms;
        stats->stage_ms[4] = map_ms;
        stats->stage_ms[5] = elapsed_ms(start, finish);
        stats->workspace_bytes = workspace.peak_bytes();
    }

    if (progress.cancelled()) {
        return cancelled_result(result);
    }
    // 4.6.1 / 4.7: publish 100% on normal completion. If the host callback
    // explicitly signals cancellation at 100%, cancellation takes precedence.
    if (!progress.publish(100u)) {
        return cancelled_result(result);
    }
    return SQ_OK;
}
