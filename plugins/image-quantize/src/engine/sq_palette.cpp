#include "sq_internal.h"
#include "sq_color.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <limits>
#include <thread>
#include <vector>

namespace sq {
namespace {

template <typename MapPixel>
bool map_rows(const sq_image_in& input, const sq_params& params,
              Workspace& workspace, Progress& progress, std::uint8_t* indices,
              std::uint32_t band_start, std::uint32_t band_width,
              MapPixel map_pixel) {
    const std::uint32_t threads = std::min(workspace.threads(), 8u);
    std::atomic<std::uint64_t> pixels_done{0u};
    std::atomic<std::uint32_t> finished{0u};
    const std::uint64_t total_pixels =
        static_cast<std::uint64_t>(input.width) * input.height;

    std::vector<std::thread> workers;
    workers.reserve(threads);
    try {
        for (std::uint32_t worker = 0u; worker < threads; ++worker) {
            const std::uint32_t begin =
                static_cast<std::uint32_t>(
                    static_cast<std::uint64_t>(input.height) * worker / threads);
            const std::uint32_t end =
                static_cast<std::uint32_t>(
                    static_cast<std::uint64_t>(input.height) * (worker + 1u) /
                    threads);
            workers.emplace_back([&, begin, end]() {
                std::uint64_t local = 0u;
                std::uint64_t unflushed = 0u;
                for (std::uint32_t row = begin; row < end; ++row) {
                    if (progress.cancelled()) {
                        break;
                    }
                    const std::uint8_t* source =
                        input.pixels + row * input.stride;
                    std::uint8_t* destination =
                        indices + static_cast<std::size_t>(row) * input.width;
                    for (std::uint32_t column = 0u; column < input.width;
                         ++column) {
                        std::uint8_t r = 0u;
                        std::uint8_t g = 0u;
                        std::uint8_t b = 0u;
                        std::uint8_t a = 0u;
                        canonical_pixel(
                            source + static_cast<std::size_t>(column) * 4u,
                            params.alpha_mode, r, g, b, a);
                        destination[column] = map_pixel(r, g, b, a);
                        ++local;
                        ++unflushed;
                        if ((local & 4095u) == 0u) {
                            pixels_done.fetch_add(unflushed,
                                                  std::memory_order_relaxed);
                            unflushed = 0u;
                            if (progress.cancelled()) {
                                break;
                            }
                        }
                    }
                    if (progress.cancelled()) {
                        break;
                    }
                }
                if (unflushed != 0u) {
                    pixels_done.fetch_add(unflushed,
                                          std::memory_order_relaxed);
                }
                finished.fetch_add(1u, std::memory_order_relaxed);
            });
        }
    } catch (...) {
        progress.cancel.store(true, std::memory_order_relaxed);
        for (std::thread& thread : workers) {
            thread.join();
        }
        return false;
    }

    if (progress.fn != nullptr) {
        while (finished.load(std::memory_order_relaxed) < threads) {
            const std::uint64_t done =
                pixels_done.load(std::memory_order_relaxed);
            const std::uint64_t bounded =
                done < total_pixels ? done : total_pixels;
            const std::uint64_t scaled = band_width * bounded;
            const std::uint32_t percent = total_pixels == 0u
                ? band_start + band_width
                : static_cast<std::uint32_t>(
                      band_start + scaled / total_pixels);
            progress.publish(percent);
            if (progress.cancelled()) {
                // A one-millisecond sleep is only a hint: the Windows timer
                // granularity can stretch it to several milliseconds, and that
                // stretch lands in the measured cancellation latency. Once a
                // cancel is pending, yield instead of sleeping.
                std::this_thread::yield();
            } else {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        }
    }

    for (std::thread& thread : workers) {
        thread.join();
    }
    if (progress.cancelled()) {
        return false;
    }
    progress.publish(band_start + band_width);
    return true;
}

}  // namespace

std::uint32_t collect_points(const ScanResult& scan, Workspace& workspace,
                             Point* points, bool from_histogram) {
    std::uint32_t count = 0u;
    if (!from_histogram && scan.probe_state == 0u) {
        for (std::uint32_t i = 0u; i < scan.exact_used; ++i) {
            const std::uint32_t key = scan.exact[i].key;
            const std::uint32_t alpha = key & 0xFFu;
            if (alpha == 0u) {
                continue;
            }
            points[count].r = static_cast<double>((key >> 24) & 0xFFu);
            points[count].g = static_cast<double>((key >> 16) & 0xFFu);
            points[count].b = static_cast<double>((key >> 8) & 0xFFu);
            points[count].w = static_cast<double>(scan.exact[i].count) * alpha;
            points[count].n = scan.exact[i].count;
            points[count].nearest = 0u;
            ++count;
        }
        return count;
    }

    const std::uint64_t* hist = workspace.histogram(0u);
    for (std::size_t cell = 0u; cell < kCellCount; ++cell) {
        const std::uint64_t n = hist[kHistN * kCellCount + cell];
        if (n == 0u) {
            continue;
        }
        const std::uint64_t w = hist[kHistW * kCellCount + cell];
        if (w == 0u) {
            continue;
        }
        points[count].r =
            static_cast<double>(hist[kHistR * kCellCount + cell]) /
            static_cast<double>(w);
        points[count].g =
            static_cast<double>(hist[kHistG * kCellCount + cell]) /
            static_cast<double>(w);
        points[count].b =
            static_cast<double>(hist[kHistB * kCellCount + cell]) /
            static_cast<double>(w);
        points[count].w = static_cast<double>(w);
        points[count].n = static_cast<std::uint32_t>(n);
        points[count].nearest = 0u;
        ++count;
    }
    return count;
}

void convert_points_to_oklab(Point* points, std::uint32_t point_count) {
    for (std::uint32_t i = 0u; i < point_count; ++i) {
        const sq_color::Oklab colour = sq_color::srgb_value_to_oklab(
            points[i].r, points[i].g, points[i].b);
        points[i].r = colour.L;
        points[i].g = colour.a;
        points[i].b = colour.b;
    }
}

bool build_exact_palette(const ScanResult& scan, Palette& palette) {
    std::memset(&palette, 0, sizeof(palette));
    std::uint32_t index = 0u;
    if (scan.has_transparent) {
        palette.rgba[0] = 0u;
        palette.r[0] = 0u;
        palette.g[0] = 0u;
        palette.b[0] = 0u;
        palette.transparent_index = 0u;
        index = 1u;
    } else {
        palette.transparent_index =
            static_cast<std::uint16_t>(SQ_NO_TRANSPARENT_INDEX);
    }
    for (std::uint32_t i = 0u; i < scan.exact_used; ++i) {
        if (index >= kMaxPaletteEntries) {
            return false;
        }
        const std::uint32_t key = scan.exact[i].key;
        palette.rgba[index] = key;
        palette.r[index] = static_cast<std::uint8_t>((key >> 24) & 0xFFu);
        palette.g[index] = static_cast<std::uint8_t>((key >> 16) & 0xFFu);
        palette.b[index] = static_cast<std::uint8_t>((key >> 8) & 0xFFu);
        ++index;
    }
    palette.size = static_cast<std::uint16_t>(index);
    return palette.size > 0u;
}

bool map_exact(const sq_image_in& input, const sq_params& params,
               const ScanResult& scan, const Palette& palette,
               std::uint8_t* indices, Workspace& workspace, Progress& progress,
               std::uint32_t band_start, std::uint32_t band_width) {
    const Entry* begin = scan.exact;
    const Entry* end = scan.exact + scan.exact_used;
    const std::uint8_t transparent =
        static_cast<std::uint8_t>(palette.transparent_index);
    const std::uint32_t offset =
        palette.transparent_index == SQ_NO_TRANSPARENT_INDEX ? 0u : 1u;
    return map_rows(
        input, params, workspace, progress, indices, band_start, band_width,
        [begin, end, transparent, offset](std::uint8_t r, std::uint8_t g,
                                          std::uint8_t b, std::uint8_t a) {
            if (a == 0u) {
                return transparent;
            }
            const std::uint32_t key = pack_key(r, g, b, a);
            const Entry* found = std::lower_bound(
                begin, end, key, [](const Entry& entry, std::uint32_t value) {
                    return entry.key < value;
                });
            if (found != end && found->key == key) {
                return static_cast<std::uint8_t>((found - begin) + offset);
            }
            return static_cast<std::uint8_t>(0u);
        });
}

bool map_nearest(const sq_image_in& input, const sq_params& params,
                 const Palette& palette, std::uint8_t* indices,
                 Workspace& workspace, Progress& progress,
                 std::uint32_t band_start, std::uint32_t band_width,
                 bool oklab) {
    const std::uint8_t transparent =
        static_cast<std::uint8_t>(palette.transparent_index);
    if (!oklab) {
        return map_rows(
            input, params, workspace, progress, indices, band_start,
            band_width,
            [&palette, transparent](std::uint8_t r, std::uint8_t g,
                                    std::uint8_t b, std::uint8_t a) {
                if (a == 0u) {
                    return transparent;
                }
                return nearest_index(palette, r, g, b);
            });
    }

    // Index generation searches the emitted eight-bit palette converted back
    // into the working space, never the pre-rounding float centroid.
    struct OklabEntry {
        double L;
        double a;
        double b;
    };
    OklabEntry entries[kMaxPaletteEntries]{};
    const std::uint32_t first =
        palette.transparent_index == SQ_NO_TRANSPARENT_INDEX ? 0u : 1u;
    for (std::uint32_t i = first; i < palette.size; ++i) {
        const sq_color::Oklab colour = sq_color::srgb8_to_oklab(
            palette.r[i], palette.g[i], palette.b[i]);
        entries[i].L = colour.L;
        entries[i].a = colour.a;
        entries[i].b = colour.b;
    }
    return map_rows(
        input, params, workspace, progress, indices, band_start, band_width,
        [&palette, &entries, transparent, first](
            std::uint8_t r, std::uint8_t g, std::uint8_t b, std::uint8_t a) {
            if (a == 0u) {
                return transparent;
            }
            const sq_color::Oklab pixel = sq_color::srgb8_to_oklab(r, g, b);
            std::uint32_t best = 0u;
            double best_distance = std::numeric_limits<double>::infinity();
            for (std::uint32_t i = first; i < palette.size; ++i) {
                const double dL = pixel.L - entries[i].L;
                const double da = pixel.a - entries[i].a;
                const double db = pixel.b - entries[i].b;
                const double distance = (dL * dL + da * da) + db * db;
                if (distance < best_distance) {
                    best_distance = distance;
                    best = i;
                }
            }
            return static_cast<std::uint8_t>(best);
        });
}

std::uint8_t nearest_index(const Palette& palette, std::uint8_t r,
                           std::uint8_t g, std::uint8_t b) {
    const std::uint32_t first =
        palette.transparent_index == SQ_NO_TRANSPARENT_INDEX ? 0u : 1u;
    const std::uint32_t size = palette.size;
    if (first >= size) {
        return 0u;
    }
    std::uint32_t best = first;
    std::uint32_t best_distance = 0xFFFFFFFFu;
    for (std::uint32_t i = first; i < size; ++i) {
        const int dr = static_cast<int>(r) - palette.r[i];
        const int dg = static_cast<int>(g) - palette.g[i];
        const int db = static_cast<int>(b) - palette.b[i];
        const std::uint32_t distance =
            static_cast<std::uint32_t>(dr * dr + dg * dg + db * db);
        if (distance < best_distance) {
            best_distance = distance;
            best = i;
        }
    }
    return static_cast<std::uint8_t>(best);
}

}  // namespace sq
