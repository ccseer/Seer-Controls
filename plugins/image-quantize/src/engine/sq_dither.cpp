#include "sq_dither.h"
#include "sq_color.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <utility>

namespace sq {
namespace {

std::int32_t coerce_srgb(double value) {
    const std::int32_t rounded =
        static_cast<std::int32_t>(std::floor(value + 0.5));
    return rounded < 0 ? 0 : (rounded > 255 ? 255 : rounded);
}

struct Targets {
    double L[kMaxPaletteEntries]{};
    double a[kMaxPaletteEntries]{};
    double b[kMaxPaletteEntries]{};
};

}  // namespace

void distribute_error(std::int32_t e_scaled, std::int32_t& p7,
                       std::int32_t& p3, std::int32_t& p5,
                       std::int32_t& p1) {
    p7 = (e_scaled * 7) / 16;
    p3 = (e_scaled * 3) / 16;
    p5 = (e_scaled * 5) / 16;
    p1 = e_scaled - p7 - p3 - p5;
}

bool map_dither(const sq_image_in& input, const sq_params& params,
                const Palette& palette, std::uint8_t* indices,
                Workspace& workspace, Progress& progress,
                std::uint32_t band_start, std::uint32_t band_width,
                bool oklab) {
    const std::uint32_t width = input.width;
    const std::uint32_t height = input.height;
    const std::size_t row_slots = static_cast<std::size_t>(width) * 3u;
    double* const buffer = workspace.dither_rows();
    std::memset(buffer, 0, row_slots * 2u * sizeof(double));
    double* current = buffer;
    double* next = buffer + row_slots;

    const std::uint32_t first =
        palette.transparent_index == SQ_NO_TRANSPARENT_INDEX ? 0u : 1u;
    const std::uint8_t transparent =
        static_cast<std::uint8_t>(palette.transparent_index);
    const std::int32_t k = static_cast<std::int32_t>(std::floor(
        static_cast<double>(params.dither_strength) * 1024.0 + 0.5));
    const double strength = static_cast<double>(params.dither_strength);
    const std::uint64_t total =
        static_cast<std::uint64_t>(width) * height;

    Targets targets;
    if (oklab) {
        for (std::uint32_t i = first; i < palette.size; ++i) {
            const sq_color::Oklab colour = sq_color::srgb8_to_oklab(
                palette.r[i], palette.g[i], palette.b[i]);
            targets.L[i] = colour.L;
            targets.a[i] = colour.a;
            targets.b[i] = colour.b;
        }
    }

    std::uint64_t units = 0u;
    for (std::uint32_t row = 0u; row < height; ++row) {
        const bool forward = (row % 2u) == 0u;
        const std::int32_t dir = forward ? 1 : -1;
        const std::uint8_t* source = input.pixels + row * input.stride;
        std::uint8_t* destination =
            indices + static_cast<std::size_t>(row) * width;
        const std::uint8_t* below =
            row + 1u < height
                ? input.pixels + (row + 1u) * input.stride
                : nullptr;

        for (std::uint32_t step = 0u; step < width; ++step) {
            const std::uint32_t column =
                forward ? step : width - 1u - step;
            const std::int32_t column_signed =
                static_cast<std::int32_t>(column);
            const std::uint8_t* pixel =
                source + static_cast<std::size_t>(column) * 4u;
            std::uint8_t r = 0u;
            std::uint8_t g = 0u;
            std::uint8_t b = 0u;
            std::uint8_t alpha = 0u;
            canonical_pixel(pixel, params.alpha_mode, r, g, b, alpha);
            ++units;
            if ((units & 1023u) == 0u) {
                const std::uint32_t percent =
                    band_start + static_cast<std::uint32_t>(
                                     band_width * units / total);
                progress.publish(percent);
                if (progress.cancelled()) {
                    return false;
                }
            }
            if (alpha == 0u) {
                destination[column] = transparent;
                continue;
            }

            double* incoming = current +
                               static_cast<std::size_t>(column) * 3u;
            const std::int32_t next_column = column_signed + dir;
            const std::int32_t behind_column = column_signed - dir;
            const std::int32_t width_signed =
                static_cast<std::int32_t>(width);
            const bool has_next =
                next_column >= 0 && next_column < width_signed;
            const bool has_behind =
                behind_column >= 0 && behind_column < width_signed;
            const bool next_opaque =
                has_next &&
                source[static_cast<std::size_t>(next_column) * 4u + 3u] != 0u;
            const bool below_behind_opaque =
                below != nullptr && has_behind &&
                below[static_cast<std::size_t>(behind_column) * 4u + 3u] != 0u;
            const bool below_opaque =
                below != nullptr &&
                below[static_cast<std::size_t>(column) * 4u + 3u] != 0u;
            const bool below_next_opaque =
                below != nullptr && has_next &&
                below[static_cast<std::size_t>(next_column) * 4u + 3u] != 0u;

            const auto spread = [&](std::uint32_t channel, double p7,
                                    double p3, double p5, double p1) {
                if (next_opaque) {
                    current[static_cast<std::size_t>(next_column) * 3u +
                            channel] += p7;
                }
                if (below == nullptr) {
                    return;
                }
                if (below_behind_opaque) {
                    next[static_cast<std::size_t>(behind_column) * 3u +
                         channel] += p3;
                }
                if (below_opaque) {
                    next[static_cast<std::size_t>(column) * 3u + channel] +=
                        p5;
                }
                if (below_next_opaque) {
                    next[static_cast<std::size_t>(next_column) * 3u +
                         channel] += p1;
                }
            };

            if (!oklab) {
                const std::int32_t cr = coerce_srgb(
                    static_cast<double>(r) + incoming[0]);
                const std::int32_t cg = coerce_srgb(
                    static_cast<double>(g) + incoming[1]);
                const std::int32_t cb = coerce_srgb(
                    static_cast<double>(b) + incoming[2]);
                const std::uint32_t best = nearest_index(
                    palette, static_cast<std::uint8_t>(cr),
                    static_cast<std::uint8_t>(cg),
                    static_cast<std::uint8_t>(cb));
                destination[column] = static_cast<std::uint8_t>(best);
                const std::int32_t errors[3] = {
                    cr - static_cast<std::int32_t>(palette.r[best]),
                    cg - static_cast<std::int32_t>(palette.g[best]),
                    cb - static_cast<std::int32_t>(palette.b[best])};
                for (std::uint32_t channel = 0u; channel < 3u; ++channel) {
                    const std::int32_t scaled = (errors[channel] * k) / 1024;
                    std::int32_t p7 = 0;
                    std::int32_t p3 = 0;
                    std::int32_t p5 = 0;
                    std::int32_t p1 = 0;
                    distribute_error(scaled, p7, p3, p5, p1);
                    spread(channel, static_cast<double>(p7),
                           static_cast<double>(p3),
                           static_cast<double>(p5),
                           static_cast<double>(p1));
                }
            } else {
                const sq_color::Oklab colour =
                    sq_color::srgb8_to_oklab(r, g, b);
                const double cl = std::clamp(colour.L + incoming[0], 0.0, 1.0);
                const double ca =
                    std::clamp(colour.a + incoming[1], -0.4, 0.4);
                const double cb =
                    std::clamp(colour.b + incoming[2], -0.4, 0.4);
                std::uint32_t best = first;
                double best_distance =
                    std::numeric_limits<double>::infinity();
                for (std::uint32_t i = first; i < palette.size; ++i) {
                    const double dl = cl - targets.L[i];
                    const double da = ca - targets.a[i];
                    const double db = cb - targets.b[i];
                    const double distance = (dl * dl + da * da) + db * db;
                    if (distance < best_distance) {
                        best_distance = distance;
                        best = i;
                    }
                }
                destination[column] = static_cast<std::uint8_t>(best);
                const double errors[3] = {
                    cl - targets.L[best], ca - targets.a[best],
                    cb - targets.b[best]};
                for (std::uint32_t channel = 0u; channel < 3u; ++channel) {
                    const double scaled = errors[channel] * strength;
                    spread(channel, scaled * 7.0 / 16.0,
                           scaled * 3.0 / 16.0, scaled * 5.0 / 16.0,
                           scaled / 16.0);
                }
            }
        }
        // The outgoing row holds the shares aimed at row + 1. Promote it to
        // the incoming row, then clear the retired buffer before it is reused.
        std::swap(current, next);
        std::memset(next, 0, row_slots * sizeof(double));
        const std::uint32_t percent =
            band_start +
            static_cast<std::uint32_t>(band_width * units / total);
        progress.publish(percent);
        if (progress.cancelled()) {
            return false;
        }
    }
    progress.publish(band_start + band_width);
    return !progress.cancelled();
}

}  // namespace sq
