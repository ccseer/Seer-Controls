#ifndef SQ_TOOLS_METRICS_H
#define SQ_TOOLS_METRICS_H

#include <cstdint>
#include <string>

#include "imageio.h"

namespace sqtools {

struct Quality {
    double de00_mean = 0.0;
    double de00_opaque_mean = 0.0;
    double de00_p95 = 0.0;
    double de00_p99 = 0.0;
    double cie76_mean = 0.0;
    double psnr_db = 0.0;
    double ssim = 0.0;
    bool ssim_defined = false;
    double alpha_mae = 0.0;
    double alpha_p95 = 0.0;
    double alpha_max = 0.0;
    double composite_white = 0.0;
    double composite_black = 0.0;
    double composite_checker = 0.0;
    double source_bytes = 0.0;
    double output_bytes = 0.0;
    double size_ratio = 0.0;
    double baseline_bytes = 0.0;
    double size_vs_baseline = 0.0;
    double banding_step = 0.0;
    bool banding_defined = false;
    std::uint32_t sample_count = 0u;
};

// All definitions are exactly those of spec section 6. Undefined metrics are
// reported as undefined rather than replaced with a plausible number (7.1).
Quality compute_quality(const Image& source, const Image& output);

// 6.7: mean gamma luma profile, 16-sample box filter, max adjacent L* delta.
// Returns false when the image is not a valid one-dimensional gradient fixture.
bool banding_step(const Image& image, double& step);

// Hand-checked 4x4 reference plus a 16x16 identical pair. Returns false and
// fills report with the failing assertion on any mismatch.
bool metrics_selfcheck(std::string& report);

std::string quality_json_row(const std::string& fixture, const Quality& quality);
std::string quality_markdown_row(const std::string& fixture, const Quality& quality);

}  // namespace sqtools

#endif  // SQ_TOOLS_METRICS_H
