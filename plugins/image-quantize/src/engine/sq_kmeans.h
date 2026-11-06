#ifndef SQ_KMEANS_H
#define SQ_KMEANS_H

#include "sq_internal.h"

namespace sq {

struct RefineInfo {
    std::uint32_t iterations = 0u;
    double objective_first = 0.0;
    double objective_last = 0.0;
    std::uint64_t centroid_fingerprint = 0u;
};

bool refine_palette(Point* points, std::uint32_t point_count,
                    const Palette& initial, std::uint32_t max_iter,
                    float tolerance, std::uint32_t threads, Shard* shards,
                    Progress& progress, Palette& output, RefineInfo& info,
                    double* objective_trace, std::uint32_t trace_capacity,
                    bool oklab = false);

bool build_clustered_palette(Point* points, std::uint32_t point_count,
                             std::uint32_t max_colors, bool has_transparent,
                             std::uint32_t max_iter, float tolerance,
                             std::uint32_t threads, Shard* shards,
                             Progress& progress, Palette& palette,
                             RefineInfo& info, bool oklab = false);

// index_base converts a stored assignment into a palette index. It is zero
// when the assignments are already palette indices, and one when they are
// zero-based cluster indices and index zero is the reserved transparent entry.
void dedup_palette(Palette& palette, Point* points,
                   std::uint32_t point_count,
                   std::uint32_t index_base = 0u);

}  // namespace sq

#endif  // SQ_KMEANS_H
