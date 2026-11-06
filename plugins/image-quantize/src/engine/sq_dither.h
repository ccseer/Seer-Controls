#ifndef SQ_DITHER_H
#define SQ_DITHER_H

#include "sq_internal.h"

namespace sq {

// Splits one scaled error into the four Floyd-Steinberg shares. The 1/16
// neighbour absorbs the division remainder, so the four shares sum to
// e_scaled exactly.
void distribute_error(std::int32_t e_scaled, std::int32_t& p7,
                       std::int32_t& p3, std::int32_t& p5,
                       std::int32_t& p1);

bool map_dither(const sq_image_in& input, const sq_params& params,
                const Palette& palette, std::uint8_t* indices,
                Workspace& workspace, Progress& progress,
                std::uint32_t band_start, std::uint32_t band_width,
                bool oklab);

}  // namespace sq

#endif  // SQ_DITHER_H
