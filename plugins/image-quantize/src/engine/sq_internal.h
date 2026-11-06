#ifndef SQ_INTERNAL_H
#define SQ_INTERNAL_H

#include "include/seer_quantize.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <thread>

namespace sq {

constexpr std::uint32_t kGridSide = 32u;
constexpr std::uint32_t kCumulativeSide = 33u;
constexpr std::size_t kCellCount = 32768u;
constexpr std::size_t kCumulativeCount = 35937u;
constexpr std::size_t kHistogramWords = 8u * kCellCount;
constexpr std::size_t kCumulativeWords = 8u * kCumulativeCount;
constexpr std::uint32_t kProbeSlots = 8192u;
constexpr std::uint32_t kProbeCapacity = 4096u;
constexpr std::uint32_t kEscalationCapacity = 32768u;
constexpr std::uint32_t kMaxPaletteEntries = 256u;

// Histogram channel order, matching Appendix A.
enum HistChannel : std::uint32_t {
    kHistN = 0u,
    kHistW = 1u,
    kHistR = 2u,
    kHistG = 3u,
    kHistB = 4u,
    kHistRR = 5u,
    kHistGG = 6u,
    kHistBB = 7u
};

struct Entry {
    std::uint32_t key;
    std::uint32_t count;
};

struct Point {
    double r;
    double g;
    double b;
    double w;
    std::uint32_t n;
    std::uint32_t nearest;
};

struct Shard {
    double r;
    double g;
    double b;
    double w;
};

struct Palette {
    std::uint32_t rgba[kMaxPaletteEntries];
    std::uint8_t r[kMaxPaletteEntries];
    std::uint8_t g[kMaxPaletteEntries];
    std::uint8_t b[kMaxPaletteEntries];
    std::uint16_t size;
    std::uint16_t transparent_index;
};

static_assert(sizeof(Entry) == 8u, "the exact table entry must stay 8 bytes");
static_assert(sizeof(Point) == 40u, "the point array entry must stay 40 bytes");
static_assert(sizeof(Shard) == 32u, "the shard accumulator must stay 32 bytes");
static_assert(sizeof(Palette) <= 16384u, "the palette must fit the misc block");

// AUTO derives the working space from color_space. The B pipeline enum is a
// valid ABI value but the optional Oklab-grid variant is not shipped, so it
// resolves to the A path; the phase report states that matrix row as absent.
inline bool use_oklab(const sq_params& params) {
    if (params.pipeline == SQ_PIPELINE_SRGB) {
        return false;
    }
    if (params.pipeline == SQ_PIPELINE_OKLAB_A ||
        params.pipeline == SQ_PIPELINE_OKLAB_B) {
        return true;
    }
    return params.color_space == SQ_SPACE_OKLAB;
}

inline std::uint32_t resolve_thread_count(std::uint32_t requested) {
    if (requested != SQ_AUTO_THREADS) {
        return requested;
    }
    unsigned detected = std::thread::hardware_concurrency();
    if (detected == 0u) {
        detected = 1u;
    }
    if (detected > 8u) {
        detected = 8u;
    }
    return static_cast<std::uint32_t>(detected);
}

inline std::uint32_t scan_worker_count(std::uint32_t resolved) {
    return resolved < 4u ? resolved : 4u;
}

inline std::size_t core_bytes(std::uint32_t width, std::uint32_t scan_workers,
                              std::uint32_t table_entries) {
    return 2299968u
         + 2097152u * static_cast<std::size_t>(scan_workers)
         + 48u * static_cast<std::size_t>(width)
         + 1310720u
         + 262144u
         + 8u * static_cast<std::size_t>(table_entries)
         + 65536u * static_cast<std::size_t>(scan_workers)
         + 16384u;
}

inline std::uint32_t pack_key(std::uint8_t r, std::uint8_t g, std::uint8_t b,
                              std::uint8_t a) {
    return (static_cast<std::uint32_t>(r) << 24)
         | (static_cast<std::uint32_t>(g) << 16)
         | (static_cast<std::uint32_t>(b) << 8)
         | static_cast<std::uint32_t>(a);
}

inline void canonical_pixel(const std::uint8_t* source,
                            std::uint32_t alpha_mode, std::uint8_t& r,
                            std::uint8_t& g, std::uint8_t& b,
                            std::uint8_t& a) {
    r = source[0];
    g = source[1];
    b = source[2];
    a = source[3];
    if (a == 0u) {
        r = 0u;
        g = 0u;
        b = 0u;
        return;
    }
    if (alpha_mode == SQ_ALPHA_PREMULTIPLIED) {
        const std::uint32_t scale = a;
        const auto recover = [scale](std::uint8_t channel) {
            const std::uint32_t value =
                (static_cast<std::uint32_t>(channel) * 255u + scale / 2u) / scale;
            return static_cast<std::uint8_t>(value > 255u ? 255u : value);
        };
        r = recover(r);
        g = recover(g);
        b = recover(b);
    }
}

// Progress is driven from the calling thread only. Workers read the cancel
// flag; they never invoke the callback.
struct Progress {
    sq_progress_fn fn = nullptr;
    void* user = nullptr;
    std::uint32_t last_percent = 0u;
    std::atomic<bool> cancel{false};

    bool publish(std::uint32_t percent) {
        if (percent < last_percent) {
            percent = last_percent;
        }
        last_percent = percent;
        if (fn != nullptr && fn(percent, user) != 0) {
            cancel.store(true, std::memory_order_relaxed);
        }
        return !cancel.load(std::memory_order_relaxed);
    }

    bool cancelled() const {
        return cancel.load(std::memory_order_relaxed);
    }
};

class Workspace {
public:
    Workspace() = default;
    ~Workspace();
    Workspace(const Workspace&) = delete;
    Workspace& operator=(const Workspace&) = delete;

    bool allocate(const sq_params& params, std::uint32_t width,
                  std::uint32_t height);
    bool grow_exact_table(std::uint32_t entries);

    std::uint64_t* histogram(std::uint32_t worker) {
        return hist_ + static_cast<std::size_t>(worker) * kHistogramWords;
    }
    Entry* probe(std::uint32_t worker) {
        return probe_ + static_cast<std::size_t>(worker) * kProbeSlots;
    }
    std::uint64_t* cumulative() { return cum_; }
    double* dither_rows() { return dither_; }
    Point* points() { return points_; }
    Shard* shards() { return shards_; }
    Entry* exact_table() { return exact_; }
    std::uint32_t exact_capacity() const { return exact_capacity_; }
    std::byte* misc() { return misc_; }
    std::size_t peak_bytes() const { return peak_; }
    std::size_t live_bytes() const { return live_; }
    std::uint32_t scan_workers() const { return scan_workers_; }
    std::uint32_t threads() const { return threads_; }

private:
    void release();

    std::uint64_t* hist_ = nullptr;
    Entry* probe_ = nullptr;
    std::uint64_t* cum_ = nullptr;
    double* dither_ = nullptr;
    Point* points_ = nullptr;
    Shard* shards_ = nullptr;
    Entry* exact_ = nullptr;
    std::byte* misc_ = nullptr;
    std::size_t live_ = 0u;
    std::size_t peak_ = 0u;
    std::uint32_t exact_capacity_ = 0u;
    std::uint32_t scan_workers_ = 0u;
    std::uint32_t threads_ = 0u;
};

struct ScanResult {
    std::uint32_t probe_state = 0u;  // 0 complete, 1 overflow
    std::uint32_t distinct_colors = 0u;
    std::uint32_t occupied_cells = 0u;
    bool has_transparent = false;
    bool has_opaque = false;
    std::uint32_t exact_used = 0u;
    Entry* exact = nullptr;
};

bool scan_image(const sq_image_in& input, const sq_params& params,
                Workspace& workspace, Progress& progress,
                ScanResult& result);

bool escalate_probe(const sq_image_in& input, const sq_params& params,
                    Workspace& workspace, Progress& progress,
                    ScanResult& result);

// The histogram source is used by the grid path; the exact-colour table is
// used by the degenerate path. Both emit points in ascending identity order so
// a tie-break by point index matches a tie-break by packed RGBA key.
std::uint32_t collect_points(const ScanResult& scan, Workspace& workspace,
                             Point* points, bool from_histogram);

// Replaces each point's sRGB coordinates with Oklab coordinates in place.
// Cell means are fractional, so they use the transfer formula; emitted palette
// entries and pixels use the shared 256-entry table.
void convert_points_to_oklab(Point* points, std::uint32_t point_count);

bool build_wu_palette(const std::uint64_t* histogram,
                      const std::uint64_t* cumulative,
                      std::uint32_t max_colors, bool has_transparent,
                      std::byte* misc, Palette& palette,
                      Progress* progress = nullptr);

bool build_exact_palette(const ScanResult& scan, Palette& palette);

// sRGB nearest neighbour over the opaque entries of the palette. Ties resolve
// to the lowest index, so the mapping is stable for the determinism matrix.
std::uint8_t nearest_index(const Palette& palette, std::uint8_t r,
                           std::uint8_t g, std::uint8_t b);

// One Wu box, exposed so the unit suite can assert the partition invariants
// (single cell inclusion and weight conservation) without reaching into the
// splitter's translation unit.
struct WuBox {
    std::uint16_t r0;
    std::uint16_t r1;
    std::uint16_t g0;
    std::uint16_t g1;
    std::uint16_t b0;
    std::uint16_t b1;
};

std::uint32_t run_wu_boxes(const std::uint64_t* histogram,
                           const std::uint64_t* cumulative, std::uint32_t k,
                           WuBox* boxes);

bool map_exact(const sq_image_in& input, const sq_params& params,
               const ScanResult& scan, const Palette& palette,
               std::uint8_t* indices, Workspace& workspace, Progress& progress,
               std::uint32_t band_start, std::uint32_t band_width);

bool map_nearest(const sq_image_in& input, const sq_params& params,
                 const Palette& palette, std::uint8_t* indices,
                 Workspace& workspace, Progress& progress,
                 std::uint32_t band_start, std::uint32_t band_width,
                 bool oklab);

}  // namespace sq

#endif  // SQ_INTERNAL_H
