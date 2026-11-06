#include "sq_kmeans.h"
#include "sq_color.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <thread>
#include <vector>

namespace sq {
namespace {

constexpr std::uint32_t kShardCount = 32u;
constexpr std::uint32_t kShardPoints = 1024u;
constexpr std::uint32_t kClusterCapacity = 256u;

inline double distance_to(const Point& point, double r, double g, double b) {
    const double dr = point.r - r;
    const double dg = point.g - g;
    const double db = point.b - b;
    return (dr * dr + dg * dg) + db * db;
}

inline std::uint8_t emission_channel(double value) {
    const double rounded = std::floor(value + 0.5);
    if (rounded <= 0.0) {
        return 0u;
    }
    if (rounded >= 255.0) {
        return 255u;
    }
    return static_cast<std::uint8_t>(rounded);
}

struct Scratch {
    double centre_r[kClusterCapacity]{};
    double centre_g[kClusterCapacity]{};
    double centre_b[kClusterCapacity]{};
    double total_r[kClusterCapacity]{};
    double total_g[kClusterCapacity]{};
    double total_b[kClusterCapacity]{};
    double total_w[kClusterCapacity]{};
    double shard_objective[kShardCount]{};
};

void accumulate_shards(Point* points, std::uint32_t point_count,
                       std::uint32_t cluster_count, std::uint32_t owner,
                       std::uint32_t stride, Scratch& scratch,
                       Shard* shards, Progress& progress,
                       std::atomic<std::uint64_t>& units_done,
                       std::atomic<std::uint32_t>& changed,
                       std::atomic<std::uint32_t>& finished) {
    std::uint32_t local_changes = 0u;
    std::uint64_t local_units = 0u;
    for (std::uint32_t shard = owner; shard < kShardCount; shard += stride) {
        Shard* block = shards + static_cast<std::size_t>(shard) *
                                    kClusterCapacity;
        std::memset(block, 0, kClusterCapacity * sizeof(Shard));
        const std::uint32_t begin = shard * kShardPoints;
        if (begin >= point_count) {
            continue;
        }
        const std::uint32_t end = begin + kShardPoints < point_count
                                      ? begin + kShardPoints
                                      : point_count;
        double objective = 0.0;
        for (std::uint32_t i = begin; i < end; ++i) {
            std::uint32_t best = 0u;
            double best_distance =
                distance_to(points[i], scratch.centre_r[0],
                            scratch.centre_g[0], scratch.centre_b[0]);
            for (std::uint32_t c = 1u; c < cluster_count; ++c) {
                const double candidate =
                    distance_to(points[i], scratch.centre_r[c],
                                scratch.centre_g[c], scratch.centre_b[c]);
                if (candidate < best_distance) {
                    best_distance = candidate;
                    best = c;
                }
            }
            if (points[i].nearest != best) {
                ++local_changes;
            }
            points[i].nearest = best;
            Shard& accumulator = block[best];
            const double weight = points[i].w;
            accumulator.r += points[i].r * weight;
            accumulator.g += points[i].g * weight;
            accumulator.b += points[i].b * weight;
            accumulator.w += weight;
            objective += weight * best_distance;
            ++local_units;
            if ((local_units & 1023u) == 0u) {
                units_done.fetch_add(1024u, std::memory_order_relaxed);
                if (progress.cancelled()) {
                    break;
                }
            }
        }
        // A shard writes only its own objective slot, so no two threads race.
        scratch.shard_objective[shard] = objective;
        if (progress.cancelled()) {
            break;
        }
    }
    if (local_changes != 0u) {
        changed.fetch_add(local_changes, std::memory_order_relaxed);
    }
    if (local_units % 1024u != 0u) {
        units_done.fetch_add(local_units % 1024u, std::memory_order_relaxed);
    }
    finished.fetch_add(1u, std::memory_order_relaxed);
}

std::uint64_t fingerprint(const Scratch& scratch,
                          std::uint32_t cluster_count) {
    std::uint64_t hash = 14695981039346656037ull;
    for (std::uint32_t c = 0u; c < cluster_count; ++c) {
        const double values[4] = {scratch.total_r[c], scratch.total_g[c],
                                  scratch.total_b[c], scratch.total_w[c]};
        const std::uint8_t* bytes =
            reinterpret_cast<const std::uint8_t*>(values);
        for (std::size_t b = 0u; b < sizeof(values); ++b) {
            hash = (hash ^ bytes[b]) * 1099511628211ull;
        }
    }
    return hash;
}

}  // namespace

void dedup_palette(Palette& palette, Point* points,
                   std::uint32_t point_count, std::uint32_t index_base) {
    const std::uint32_t first =
        palette.transparent_index == SQ_NO_TRANSPARENT_INDEX ? 0u : 1u;
    std::uint32_t remap[kMaxPaletteEntries]{};
    std::uint32_t write = 0u;
    if (first == 1u) {
        remap[0] = 0u;
        write = 1u;
    }
    for (std::uint32_t i = first; i < palette.size; ++i) {
        std::uint32_t target = write;
        for (std::uint32_t j = first; j < write; ++j) {
            if (palette.rgba[j] == palette.rgba[i]) {
                target = j;
                break;
            }
        }
        remap[i] = target;
        if (target == write) {
            if (write != i) {
                palette.rgba[write] = palette.rgba[i];
                palette.r[write] = palette.r[i];
                palette.g[write] = palette.g[i];
                palette.b[write] = palette.b[i];
            }
            ++write;
        }
    }
    palette.size = static_cast<std::uint16_t>(write);
    if (points == nullptr) {
        return;
    }
    // Stored assignments may be zero-based cluster indices while the palette
    // reserves index zero for transparency. The offset keeps the lookup in
    // palette-index space, so a transparent source cannot remap a real cluster.
    for (std::uint32_t i = 0u; i < point_count; ++i) {
        const std::uint64_t key =
            static_cast<std::uint64_t>(points[i].nearest) + index_base;
        if (key < kMaxPaletteEntries) {
            points[i].nearest = remap[static_cast<std::size_t>(key)];
        }
    }
}

bool refine_palette(Point* points, std::uint32_t point_count,
                    const Palette& initial, std::uint32_t max_iter,
                    float tolerance, std::uint32_t threads, Shard* shards,
                    Progress& progress, Palette& output, RefineInfo& info,
                    double* objective_trace, std::uint32_t trace_capacity,
                    bool oklab) {
    const Palette starting = initial;
    const std::uint32_t first =
        starting.transparent_index == SQ_NO_TRANSPARENT_INDEX ? 0u : 1u;
    const std::uint32_t cluster_count =
        starting.size > first ? starting.size - first : 0u;

    info.iterations = 0u;
    info.objective_first = 0.0;
    info.objective_last = 0.0;
    info.centroid_fingerprint = 0u;

    if (max_iter == 0u || point_count == 0u || cluster_count == 0u) {
        output = starting;
        // No assignment was computed, so there is nothing to remap.
        dedup_palette(output, nullptr, 0u, 0u);
        return true;
    }

    Scratch scratch;
    const std::uint32_t active =
        threads < 1u ? 1u : (threads > kShardCount ? kShardCount : threads);
    for (std::uint32_t c = 0u; c < cluster_count; ++c) {
        if (oklab) {
            const sq_color::Oklab colour = sq_color::srgb8_to_oklab(
                starting.r[first + c], starting.g[first + c],
                starting.b[first + c]);
            scratch.centre_r[c] = colour.L;
            scratch.centre_g[c] = colour.a;
            scratch.centre_b[c] = colour.b;
        } else {
            scratch.centre_r[c] = static_cast<double>(starting.r[first + c]);
            scratch.centre_g[c] = static_cast<double>(starting.g[first + c]);
            scratch.centre_b[c] = static_cast<double>(starting.b[first + c]);
        }
    }
    for (std::uint32_t i = 0u; i < point_count; ++i) {
        points[i].nearest = 0xFFFFFFFFu;
    }

    const std::uint64_t total_units =
        static_cast<std::uint64_t>(max_iter) * point_count;
    double previous_objective = 0.0;
    bool stop = false;

    for (std::uint32_t iteration = 0u; iteration < max_iter && !stop;
         ++iteration) {
        std::memset(scratch.total_r, 0, sizeof(scratch.total_r));
        std::memset(scratch.total_g, 0, sizeof(scratch.total_g));
        std::memset(scratch.total_b, 0, sizeof(scratch.total_b));
        std::memset(scratch.total_w, 0, sizeof(scratch.total_w));
        for (std::uint32_t s = 0u; s < kShardCount; ++s) {
            scratch.shard_objective[s] = 0.0;
        }

        std::atomic<std::uint64_t> units_done{0u};
        std::atomic<std::uint32_t> changed{0u};
        std::atomic<std::uint32_t> finished{0u};
        std::vector<std::thread> workers;
        workers.reserve(active);
        try {
            for (std::uint32_t t = 0u; t < active; ++t) {
                workers.emplace_back([&, t]() {
                    accumulate_shards(points, point_count, cluster_count, t,
                                      active, scratch, shards, progress,
                                      units_done, changed, finished);
                });
            }
        } catch (...) {
            progress.cancel.store(true, std::memory_order_relaxed);
            for (std::thread& worker : workers) {
                worker.join();
            }
            return false;
        }

        if (progress.fn != nullptr) {
            while (finished.load(std::memory_order_relaxed) < active) {
                const std::uint64_t done =
                    units_done.load(std::memory_order_relaxed);
                const std::uint64_t bounded =
                    done < point_count ? done : point_count;
                const std::uint64_t units =
                    static_cast<std::uint64_t>(iteration) * point_count +
                    bounded;
                const std::uint32_t percent =
                    43u + static_cast<std::uint32_t>(22u * units /
                                                     total_units);
                progress.publish(percent);
                if (progress.cancelled()) {
                    std::this_thread::yield();
                } else {
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                }
            }
        }
        for (std::thread& worker : workers) {
            worker.join();
        }
        if (progress.cancelled()) {
            return false;
        }

        double objective = 0.0;
        for (std::uint32_t s = 0u; s < kShardCount; ++s) {
            const Shard* block =
                shards + static_cast<std::size_t>(s) * kClusterCapacity;
            for (std::uint32_t c = 0u; c < cluster_count; ++c) {
                scratch.total_r[c] += block[c].r;
                scratch.total_g[c] += block[c].g;
                scratch.total_b[c] += block[c].b;
                scratch.total_w[c] += block[c].w;
            }
            objective += scratch.shard_objective[s];
        }
        info.centroid_fingerprint = fingerprint(scratch, cluster_count);
        if (objective_trace != nullptr && iteration < trace_capacity) {
            objective_trace[iteration] = objective;
        }
        if (iteration == 0u) {
            info.objective_first = objective;
        }
        info.objective_last = objective;
        info.iterations = iteration + 1u;

        const double relative =
            iteration == 0u
                ? 1.0
                : (previous_objective - objective) /
                      (previous_objective > 1e-12 ? previous_objective : 1e-12);
        const bool unchanged =
            iteration != 0u && changed.load(std::memory_order_relaxed) == 0u;
        stop = unchanged || relative < static_cast<double>(tolerance) ||
               iteration + 1u >= max_iter;

        for (std::uint32_t c = 0u; c < cluster_count; ++c) {
            if (scratch.total_w[c] > 0.0) {
                scratch.centre_r[c] =
                    scratch.total_r[c] / scratch.total_w[c];
                scratch.centre_g[c] =
                    scratch.total_g[c] / scratch.total_w[c];
                scratch.centre_b[c] =
                    scratch.total_b[c] / scratch.total_w[c];
            }
        }
        previous_objective = objective;
    }

    std::uint64_t sum_w[kClusterCapacity]{};
    std::uint64_t sum_n[kClusterCapacity]{};
    for (std::uint32_t i = 0u; i < point_count; ++i) {
        const std::uint32_t c = points[i].nearest;
        if (c >= cluster_count) {
            continue;
        }
        sum_w[c] += static_cast<std::uint64_t>(points[i].w);
        sum_n[c] += points[i].n;
    }

    std::memset(&output, 0, sizeof(output));
    output.transparent_index = starting.transparent_index;
    std::uint32_t index = 0u;
    if (first == 1u) {
        output.rgba[0] = 0u;
        index = 1u;
    }
    for (std::uint32_t c = 0u; c < cluster_count; ++c) {
        std::uint8_t r = 0u;
        std::uint8_t g = 0u;
        std::uint8_t b = 0u;
        if (oklab) {
            sq_color::oklab_to_srgb8(
                {scratch.centre_r[c], scratch.centre_g[c],
                 scratch.centre_b[c]}, r, g, b);
        } else {
            r = emission_channel(scratch.centre_r[c]);
            g = emission_channel(scratch.centre_g[c]);
            b = emission_channel(scratch.centre_b[c]);
        }
        // An empty cluster keeps its centre but has no pixels to average
        // an alpha from. Its starting entry still carries a real alpha from the
        // data, whereas a forced 1 would make every pixel that selects this
        // entry nearly transparent.
        const std::uint8_t a =
            sum_n[c] != 0u
                ? emission_channel(static_cast<double>(sum_w[c]) /
                                   static_cast<double>(sum_n[c]))
                : static_cast<std::uint8_t>(starting.rgba[first + c] & 0xFFu);
        output.rgba[index] = pack_key(r, g, b, a);
        output.r[index] = r;
        output.g[index] = g;
        output.b[index] = b;
        ++index;
    }
    if (index == first) {
        output.rgba[0] = pack_key(0u, 0u, 0u, 255u);
        output.r[0] = 0u;
        output.g[0] = 0u;
        output.b[0] = 0u;
        output.transparent_index =
            static_cast<std::uint16_t>(SQ_NO_TRANSPARENT_INDEX);
        index = 1u;
    }
    output.size = static_cast<std::uint16_t>(index);
    dedup_palette(output, points, point_count, first);
    return true;
}

bool build_clustered_palette(Point* points, std::uint32_t point_count,
                             std::uint32_t max_colors, bool has_transparent,
                             std::uint32_t max_iter, float tolerance,
                             std::uint32_t threads, Shard* shards,
                             Progress& progress, Palette& palette,
                             RefineInfo& info, bool oklab) {
    Palette initial;
    std::memset(&initial, 0, sizeof(initial));
    std::uint32_t index = 0u;
    if (has_transparent) {
        initial.rgba[0] = 0u;
        initial.transparent_index = 0u;
        index = 1u;
    } else {
        initial.transparent_index =
            static_cast<std::uint16_t>(SQ_NO_TRANSPARENT_INDEX);
    }
    const std::uint32_t budget =
        max_colors - (has_transparent ? 1u : 0u);

    if (point_count == 0u || budget == 0u) {
        if (!has_transparent) {
            initial.rgba[0] = pack_key(0u, 0u, 0u, 255u);
            initial.size = 1u;
        } else {
            initial.size = static_cast<std::uint16_t>(index);
        }
        palette = initial;
        info.iterations = 0u;
        return true;
    }

    // The shard block holds exactly 32768 doubles, enough to keep one
    // minimum-distance value per point during seeding. Refinement clears the
    // same block before using it as cluster accumulators.
    double* minimum = reinterpret_cast<double*>(shards);
    std::uint32_t seeds[kMaxPaletteEntries]{};
    std::uint32_t seed_count = 0u;
    std::uint32_t first = 0u;
    for (std::uint32_t i = 1u; i < point_count; ++i) {
        if (points[i].w > points[first].w) {
            first = i;
        }
    }
    seeds[seed_count++] = first;
    for (std::uint32_t i = 0u; i < point_count; ++i) {
        minimum[i] = distance_to(points[i], points[first].r, points[first].g,
                                 points[first].b);
    }
    minimum[first] = -1.0;
    constexpr std::uint32_t kNoPoint = 0xFFFFFFFFu;
    while (seed_count < budget && seed_count < point_count) {
        // Entries that share RGB but differ in alpha have distance zero from
        // each other, so a "largest minimum distance" scan alone can stop
        // early with a nearly empty palette. A negative minimum marks an entry
        // that is already a seed, so the scan always picks a distinct one;
        // ties fall to the lowest index, the packed-key order on this path.
        std::uint32_t best = kNoPoint;
        double best_distance = -1.0;
        for (std::uint32_t i = 0u; i < point_count; ++i) {
            if (minimum[i] > best_distance) {
                best_distance = minimum[i];
                best = i;
            }
        }
        if (best == kNoPoint) {
            break;
        }
        seeds[seed_count++] = best;
        minimum[best] = -1.0;
        for (std::uint32_t i = 0u; i < point_count; ++i) {
            if (minimum[i] < 0.0) {
                continue;
            }
            const double candidate = distance_to(
                points[i], points[best].r, points[best].g, points[best].b);
            if (candidate < minimum[i]) {
                minimum[i] = candidate;
            }
        }
    }

    double sum_r[kClusterCapacity]{};
    double sum_g[kClusterCapacity]{};
    double sum_b[kClusterCapacity]{};
    std::uint64_t sum_w[kClusterCapacity]{};
    std::uint64_t sum_n[kClusterCapacity]{};
    for (std::uint32_t i = 0u; i < point_count; ++i) {
        std::uint32_t best = 0u;
        double best_distance = distance_to(points[i], points[seeds[0]].r,
                                           points[seeds[0]].g,
                                           points[seeds[0]].b);
        for (std::uint32_t s = 1u; s < seed_count; ++s) {
            const double candidate = distance_to(points[i], points[seeds[s]].r,
                                                 points[seeds[s]].g,
                                                 points[seeds[s]].b);
            if (candidate < best_distance) {
                best_distance = candidate;
                best = s;
            }
        }
        const double weight = points[i].w;
        sum_r[best] += points[i].r * weight;
        sum_g[best] += points[i].g * weight;
        sum_b[best] += points[i].b * weight;
        sum_w[best] += static_cast<std::uint64_t>(weight);
        sum_n[best] += points[i].n;
    }

    for (std::uint32_t s = 0u; s < seed_count; ++s) {
        if (index >= kMaxPaletteEntries) {
            break;
        }
        std::uint8_t r = 0u;
        std::uint8_t g = 0u;
        std::uint8_t b = 0u;
        std::uint8_t a = 0u;
        if (sum_w[s] != 0u) {
            if (oklab) {
                sq_color::oklab_to_srgb8(
                    {sum_r[s] / static_cast<double>(sum_w[s]),
                     sum_g[s] / static_cast<double>(sum_w[s]),
                     sum_b[s] / static_cast<double>(sum_w[s])},
                    r, g, b);
            } else {
                r = emission_channel(sum_r[s] /
                                     static_cast<double>(sum_w[s]));
                g = emission_channel(sum_g[s] /
                                     static_cast<double>(sum_w[s]));
                b = emission_channel(sum_b[s] /
                                     static_cast<double>(sum_w[s]));
            }
            a = emission_channel(static_cast<double>(sum_w[s]) /
                                 static_cast<double>(sum_n[s]));
        } else {
            if (oklab) {
                sq_color::oklab_to_srgb8(
                    {points[seeds[s]].r, points[seeds[s]].g,
                     points[seeds[s]].b}, r, g, b);
            } else {
                r = emission_channel(points[seeds[s]].r);
                g = emission_channel(points[seeds[s]].g);
                b = emission_channel(points[seeds[s]].b);
            }
            a = emission_channel(points[seeds[s]].w /
                                 static_cast<double>(points[seeds[s]].n));
        }
        if (a == 0u) {
            a = 1u;
        }
        initial.rgba[index] = pack_key(r, g, b, a);
        initial.r[index] = r;
        initial.g[index] = g;
        initial.b[index] = b;
        ++index;
    }
    initial.size = static_cast<std::uint16_t>(index);

    return refine_palette(points, point_count, initial, max_iter, tolerance,
                          threads, shards, progress, palette, info, nullptr,
                          0u, oklab);
}

}  // namespace sq
