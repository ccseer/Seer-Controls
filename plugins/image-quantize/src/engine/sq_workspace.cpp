#include "sq_internal.h"

namespace sq {

Workspace::~Workspace() {
    release();
}

void Workspace::release() {
    std::free(hist_);
    std::free(probe_);
    std::free(cum_);
    std::free(dither_);
    std::free(points_);
    std::free(shards_);
    std::free(exact_);
    std::free(misc_);
    hist_ = nullptr;
    probe_ = nullptr;
    cum_ = nullptr;
    dither_ = nullptr;
    points_ = nullptr;
    shards_ = nullptr;
    exact_ = nullptr;
    misc_ = nullptr;
    live_ = 0u;
    peak_ = 0u;
    exact_capacity_ = 0u;
    scan_workers_ = 0u;
    threads_ = 0u;
}

namespace {

void* take(std::size_t bytes, std::size_t& live) {
    void* block = std::malloc(bytes);
    if (block != nullptr) {
        live += bytes;
    }
    return block;
}

}  // namespace

bool Workspace::allocate(const sq_params& params, std::uint32_t width,
                         std::uint32_t height) {
    (void)height;  // the formula is sized from width and the worker count only
    threads_ = resolve_thread_count(params.thread_count);
    scan_workers_ = scan_worker_count(threads_);
    if (scan_workers_ == 0u) {
        return false;
    }

    const std::size_t hist_bytes =
        kHistogramWords * sizeof(std::uint64_t) * scan_workers_;
    const std::size_t probe_bytes =
        static_cast<std::size_t>(kProbeSlots) * sizeof(Entry) * scan_workers_;
    const std::size_t cum_bytes = kCumulativeWords * sizeof(std::uint64_t);
    const std::size_t dither_bytes = 6u * width * sizeof(double);
    const std::size_t point_bytes = 32768u * sizeof(Point);
    const std::size_t shard_bytes = 32u * 256u * sizeof(Shard);
    const std::size_t exact_bytes = 4096u * sizeof(Entry);

    hist_ = static_cast<std::uint64_t*>(take(hist_bytes, live_));
    probe_ = static_cast<Entry*>(take(probe_bytes, live_));
    cum_ = static_cast<std::uint64_t*>(take(cum_bytes, live_));
    dither_ = static_cast<double*>(take(dither_bytes, live_));
    points_ = static_cast<Point*>(take(point_bytes, live_));
    shards_ = static_cast<Shard*>(take(shard_bytes, live_));
    exact_ = static_cast<Entry*>(take(exact_bytes, live_));
    misc_ = static_cast<std::byte*>(take(16384u, live_));

    if (hist_ == nullptr || probe_ == nullptr || cum_ == nullptr ||
        dither_ == nullptr || points_ == nullptr || shards_ == nullptr ||
        exact_ == nullptr || misc_ == nullptr) {
        release();
        return false;
    }

    std::memset(hist_, 0, hist_bytes);
    std::memset(probe_, 0, probe_bytes);
    exact_capacity_ = 4096u;
    peak_ = live_;

    // The caller-visible bytes are asserted against the same formula that
    // sq_workspace_bytes() publishes; a mismatch is an internal defect.
    if (live_ != core_bytes(width, scan_workers_, 4096u)) {
        release();
        return false;
    }
    return true;
}

bool Workspace::grow_exact_table(std::uint32_t entries) {
    if (entries <= exact_capacity_) {
        return entries == exact_capacity_;
    }
    const std::size_t old_bytes =
        static_cast<std::size_t>(exact_capacity_) * sizeof(Entry);
    const std::size_t new_bytes =
        static_cast<std::size_t>(entries) * sizeof(Entry);
    void* grown = std::realloc(exact_, new_bytes);
    if (grown == nullptr) {
        return false;
    }
    exact_ = static_cast<Entry*>(grown);
    exact_capacity_ = entries;
    live_ = live_ - old_bytes + new_bytes;
    if (live_ > peak_) {
        peak_ = live_;
    }
    return true;
}

}  // namespace sq
