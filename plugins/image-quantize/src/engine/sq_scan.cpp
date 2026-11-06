#include "sq_internal.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <thread>
#include <vector>

namespace sq {
namespace {

constexpr std::uint32_t kProbeMask = kProbeSlots - 1u;

inline std::uint32_t hash_key(std::uint32_t key) {
    key ^= key >> 16;
    key *= 0x7feb352du;
    key ^= key >> 15;
    return key;
}

inline std::uint32_t cell_of(std::uint8_t r, std::uint8_t g, std::uint8_t b) {
    return (static_cast<std::uint32_t>(r) >> 3) * 1024u
         + (static_cast<std::uint32_t>(g) >> 3) * 32u
         + (static_cast<std::uint32_t>(b) >> 3);
}

bool probe_insert(Entry* slots, std::uint32_t& used, bool& overflow,
                  std::uint32_t key) {
    if (overflow) {
        return false;
    }
    std::uint32_t index = hash_key(key) & kProbeMask;
    for (std::uint32_t step = 0; step < kProbeSlots; ++step) {
        Entry& entry = slots[index];
        if (entry.key == 0u) {
            if (used >= kProbeCapacity) {
                overflow = true;
                return false;
            }
            entry.key = key;
            entry.count = 1u;
            ++used;
            return true;
        }
        if (entry.key == key) {
            ++entry.count;
            return true;
        }
        index = (index + 1u) & kProbeMask;
    }
    overflow = true;
    return false;
}

std::uint32_t compact_and_sort(Entry* slots, std::uint32_t slot_count) {
    std::uint32_t out = 0u;
    for (std::uint32_t i = 0u; i < slot_count; ++i) {
        if (slots[i].key != 0u) {
            if (out != i) {
                slots[out] = slots[i];
            }
            ++out;
        }
    }
    std::sort(slots, slots + out, [](const Entry& a, const Entry& b) {
        return a.key < b.key;
    });
    return out;
}

void build_cumulative(const std::uint64_t* hist, std::uint64_t* cumulative) {
    for (std::uint32_t channel = 0u; channel < 8u; ++channel) {
        const std::uint64_t* source = hist + channel * kCellCount;
        std::uint64_t* layer = cumulative + channel * kCumulativeCount;
        std::memset(layer, 0, kCumulativeCount * sizeof(std::uint64_t));
        for (std::uint32_t x = 1u; x < kCumulativeSide; ++x) {
            for (std::uint32_t y = 1u; y < kCumulativeSide; ++y) {
                for (std::uint32_t z = 1u; z < kCumulativeSide; ++z) {
                    const std::uint64_t value =
                        source[(x - 1u) * 1024u + (y - 1u) * 32u + (z - 1u)];
                    const std::size_t index =
                        static_cast<std::size_t>(x) * 1089u
                        + static_cast<std::size_t>(y) * 33u + z;
                    const std::int64_t sum =
                        static_cast<std::int64_t>(value)
                        + static_cast<std::int64_t>(layer[index - 1089u])
                        + static_cast<std::int64_t>(layer[index - 33u])
                        + static_cast<std::int64_t>(layer[index - 1u])
                        - static_cast<std::int64_t>(layer[index - 1089u - 33u])
                        - static_cast<std::int64_t>(layer[index - 1089u - 1u])
                        - static_cast<std::int64_t>(layer[index - 33u - 1u])
                        + static_cast<std::int64_t>(
                              layer[index - 1089u - 33u - 1u]);
                    layer[index] = static_cast<std::uint64_t>(sum);
                }
            }
        }
    }
}

struct WorkerState {
    std::uint32_t used = 0u;
    bool overflow = false;
    bool transparent = false;
    bool opaque = false;
    std::uint64_t pixels = 0u;
};

void scan_band(const sq_image_in& input, const sq_params& params,
               Workspace& workspace, Progress& progress,
               std::uint32_t worker, std::uint32_t begin,
               std::uint32_t end, std::atomic<std::uint64_t>& pixels_done,
               WorkerState& state) {
    std::uint64_t* hist = workspace.histogram(worker);
    Entry* probe = workspace.probe(worker);
    std::uint64_t local = 0u;
    std::uint64_t unflushed = 0u;

    const auto flush = [&](std::uint64_t amount) {
        if (amount != 0u) {
            pixels_done.fetch_add(amount, std::memory_order_relaxed);
        }
    };

    for (std::uint32_t row = begin; row < end; ++row) {
        if (progress.cancelled()) {
            flush(unflushed);
            return;
        }
        const std::uint8_t* source = input.pixels + row * input.stride;
        for (std::uint32_t column = 0u; column < input.width; ++column) {
            std::uint8_t r = 0u;
            std::uint8_t g = 0u;
            std::uint8_t b = 0u;
            std::uint8_t a = 0u;
            canonical_pixel(source + static_cast<std::size_t>(column) * 4u,
                            params.alpha_mode, r, g, b, a);
            if (a == 0u) {
                state.transparent = true;
            } else {
                if (a == 255u) {
                    state.opaque = true;
                }
                const std::uint32_t cell = cell_of(r, g, b);
                const std::uint32_t weight = a;
                hist[kHistN * kCellCount + cell] += 1u;
                hist[kHistW * kCellCount + cell] += weight;
                hist[kHistR * kCellCount + cell] +=
                    static_cast<std::uint64_t>(r) * weight;
                hist[kHistG * kCellCount + cell] +=
                    static_cast<std::uint64_t>(g) * weight;
                hist[kHistB * kCellCount + cell] +=
                    static_cast<std::uint64_t>(b) * weight;
                hist[kHistRR * kCellCount + cell] +=
                    static_cast<std::uint64_t>(r) * r * weight;
                hist[kHistGG * kCellCount + cell] +=
                    static_cast<std::uint64_t>(g) * g * weight;
                hist[kHistBB * kCellCount + cell] +=
                    static_cast<std::uint64_t>(b) * b * weight;
                if (!state.overflow) {
                    probe_insert(probe, state.used, state.overflow,
                                 pack_key(r, g, b, a));
                }
            }
            ++local;
            ++unflushed;
            if ((local & 4095u) == 0u) {
                flush(unflushed);
                unflushed = 0u;
                if (progress.cancelled()) {
                    return;
                }
            }
        }
    }
    flush(unflushed);
    state.pixels = local;
}

}  // namespace

bool scan_image(const sq_image_in& input, const sq_params& params,
                Workspace& workspace, Progress& progress,
                ScanResult& result) {
    const std::uint32_t workers = workspace.scan_workers();
    std::array<WorkerState, 4> states{};
    std::atomic<std::uint64_t> pixels_done{0u};
    std::atomic<std::uint32_t> finished{0u};

    const std::uint64_t total_pixels =
        static_cast<std::uint64_t>(input.width) * input.height;

    std::vector<std::thread> threads;
    threads.reserve(workers);
    try {
        for (std::uint32_t worker = 0u; worker < workers; ++worker) {
            const std::uint32_t begin =
                static_cast<std::uint32_t>(
                    static_cast<std::uint64_t>(input.height) * worker / workers);
            const std::uint32_t end =
                static_cast<std::uint32_t>(
                    static_cast<std::uint64_t>(input.height) * (worker + 1u) /
                    workers);
            threads.emplace_back([&, worker, begin, end]() {
                scan_band(input, params, workspace, progress, worker, begin,
                          end, pixels_done, states[worker]);
                finished.fetch_add(1u, std::memory_order_relaxed);
            });
        }
    } catch (...) {
        progress.cancel.store(true, std::memory_order_relaxed);
        for (std::thread& thread : threads) {
            thread.join();
        }
        return false;
    }

    if (progress.fn != nullptr) {
        while (finished.load(std::memory_order_relaxed) < workers) {
            const std::uint64_t done =
                pixels_done.load(std::memory_order_relaxed);
            const std::uint64_t bounded =
                done < total_pixels ? done : total_pixels;
            const std::uint32_t percent = total_pixels == 0u
                ? 40u
                : static_cast<std::uint32_t>(40u * bounded / total_pixels);
            progress.publish(percent);
            if (progress.cancelled()) {
                // Workers observe the flag at their next checkpoint. Yielding
                // instead of sleeping keeps the caller from adding a whole
                // timer tick to the measured cancellation latency.
                std::this_thread::yield();
            } else {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        }
    }

    for (std::thread& thread : threads) {
        thread.join();
    }
    if (progress.cancelled()) {
        return false;
    }
    progress.publish(40u);

    bool any_overflow = false;
    for (const WorkerState& state : states) {
        any_overflow = any_overflow || state.overflow;
        result.has_transparent = result.has_transparent || state.transparent;
        result.has_opaque = result.has_opaque || state.opaque;
    }

    // Sum the per-worker integer histograms into worker zero's block, then
    // build the single cumulative table.
    std::uint64_t* merged = workspace.histogram(0u);
    for (std::uint32_t worker = 1u; worker < workers; ++worker) {
        const std::uint64_t* source = workspace.histogram(worker);
        for (std::size_t i = 0u; i < kHistogramWords; ++i) {
            merged[i] += source[i];
        }
    }
    build_cumulative(merged, workspace.cumulative());

    result.occupied_cells = 0u;
    for (std::size_t cell = 0u; cell < kCellCount; ++cell) {
        if (merged[kHistN * kCellCount + cell] != 0u) {
            ++result.occupied_cells;
        }
    }

    if (any_overflow) {
        result.probe_state = 1u;
        result.distinct_colors = kProbeCapacity + 1u;
        result.exact_used = 0u;
        result.exact = workspace.exact_table();
        return true;
    }

    std::array<Entry*, 4> probes{};
    std::array<std::uint32_t, 4> counts{};
    for (std::uint32_t worker = 0u; worker < workers; ++worker) {
        probes[worker] = workspace.probe(worker);
        counts[worker] = compact_and_sort(probes[worker], kProbeSlots);
    }

    Entry* exact = workspace.exact_table();
    std::uint32_t exact_used = 0u;
    std::array<std::uint32_t, 4> cursor{};
    bool overflow = false;
    while (true) {
        // White opaque packs to 0xFFFFFFFF, so it cannot double as the
        // "no candidate" sentinel: the selected worker index carries that
        // meaning instead.
        std::uint32_t best = 0u;
        int chosen = -1;
        for (std::uint32_t worker = 0u; worker < workers; ++worker) {
            if (cursor[worker] >= counts[worker]) {
                continue;
            }
            const std::uint32_t key = probes[worker][cursor[worker]].key;
            if (chosen < 0 || key < best) {
                best = key;
                chosen = static_cast<int>(worker);
            }
        }
        if (chosen < 0) {
            break;
        }
        std::uint32_t total = 0u;
        for (std::uint32_t worker = 0u; worker < workers; ++worker) {
            while (cursor[worker] < counts[worker] &&
                   probes[worker][cursor[worker]].key == best) {
                total += probes[worker][cursor[worker]].count;
                ++cursor[worker];
            }
        }
        if (exact_used >= kProbeCapacity) {
            overflow = true;
            break;
        }
        exact[exact_used].key = best;
        exact[exact_used].count = total;
        ++exact_used;
    }

    result.exact = exact;
    result.exact_used = exact_used;
    if (overflow) {
        result.probe_state = 1u;
        result.distinct_colors = kProbeCapacity + 1u;
    } else {
        result.probe_state = 0u;
        result.distinct_colors = exact_used;
    }
    return true;
}

bool escalate_probe(const sq_image_in& input, const sq_params& params,
                    Workspace& workspace, Progress& progress,
                    ScanResult& result) {
    if (!workspace.grow_exact_table(kEscalationCapacity)) {
        return false;
    }
    Entry* exact = workspace.exact_table();
    std::memset(exact, 0, static_cast<std::size_t>(kEscalationCapacity) *
                             sizeof(Entry));

    std::uint32_t used = 0u;
    bool overflow = false;
    std::uint64_t local = 0u;
    for (std::uint32_t row = 0u; row < input.height; ++row) {
        if (progress.cancelled()) {
            return false;
        }
        const std::uint8_t* source = input.pixels + row * input.stride;
        for (std::uint32_t column = 0u; column < input.width; ++column) {
            std::uint8_t r = 0u;
            std::uint8_t g = 0u;
            std::uint8_t b = 0u;
            std::uint8_t a = 0u;
            canonical_pixel(source + static_cast<std::size_t>(column) * 4u,
                            params.alpha_mode, r, g, b, a);
            if (a != 0u && !overflow) {
                const std::uint32_t key = pack_key(r, g, b, a);
                std::uint32_t index = hash_key(key) &
                    (kEscalationCapacity - 1u);
                bool inserted = false;
                for (std::uint32_t step = 0u; step < kEscalationCapacity;
                     ++step) {
                    Entry& entry = exact[index];
                    if (entry.key == 0u) {
                        entry.key = key;
                        entry.count = 1u;
                        ++used;
                        inserted = true;
                        break;
                    }
                    if (entry.key == key) {
                        ++entry.count;
                        inserted = true;
                        break;
                    }
                    index = (index + 1u) & (kEscalationCapacity - 1u);
                }
                if (!inserted) {
                    overflow = true;
                }
            }
            ++local;
            if ((local & 4095u) == 0u) {
                // The scan band is already at 40; keep the callback alive so
                // cancellation can still observe the extra pass.
                progress.publish(40u);
                if (progress.cancelled()) {
                    return false;
                }
            }
        }
    }

    if (overflow) {
        result.probe_state = 1u;
        result.distinct_colors = kEscalationCapacity + 1u;
        result.exact_used = 0u;
        return true;
    }

    const std::uint32_t sorted = compact_and_sort(exact, kEscalationCapacity);
    result.probe_state = 0u;
    result.distinct_colors = sorted;
    result.exact_used = sorted;
    result.exact = exact;
    return true;
}

}  // namespace sq
