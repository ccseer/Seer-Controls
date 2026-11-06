#include "sq_internal.h"

#include <cmath>
#include <limits>

namespace sq {
namespace {

struct Box {
    std::uint16_t r0;
    std::uint16_t r1;
    std::uint16_t g0;
    std::uint16_t g1;
    std::uint16_t b0;
    std::uint16_t b1;
};

struct Aggregate {
    std::uint64_t v[8]{};
};

static_assert(sizeof(Palette) + 256u * sizeof(Box) <= 16384u,
              "the Wu boxes must fit beside the palette in the misc block");

std::uint64_t prefix(const std::uint64_t* layer, std::uint32_t x,
                     std::uint32_t y, std::uint32_t z) {
    return layer[static_cast<std::size_t>(x) * 1089u
               + static_cast<std::size_t>(y) * 33u + z];
}

Aggregate box_aggregate(const std::uint64_t* cumulative, const Box& box) {
    Aggregate result;
    for (std::uint32_t channel = 0u; channel < 8u; ++channel) {
        const std::uint64_t* layer = cumulative + channel * kCumulativeCount;
        const std::int64_t sum =
            static_cast<std::int64_t>(prefix(layer, box.r1, box.g1, box.b1))
            - static_cast<std::int64_t>(prefix(layer, box.r0, box.g1, box.b1))
            - static_cast<std::int64_t>(prefix(layer, box.r1, box.g0, box.b1))
            - static_cast<std::int64_t>(prefix(layer, box.r1, box.g1, box.b0))
            + static_cast<std::int64_t>(prefix(layer, box.r0, box.g0, box.b1))
            + static_cast<std::int64_t>(prefix(layer, box.r0, box.g1, box.b0))
            + static_cast<std::int64_t>(prefix(layer, box.r1, box.g0, box.b0))
            - static_cast<std::int64_t>(prefix(layer, box.r0, box.g0, box.b0));
        result.v[channel] = static_cast<std::uint64_t>(sum);
    }
    return result;
}

double box_sse(const Aggregate& aggregate) {
    const double w = static_cast<double>(aggregate.v[kHistW]);
    if (w == 0.0) {
        return 0.0;
    }
    const double r = static_cast<double>(aggregate.v[kHistR]);
    const double g = static_cast<double>(aggregate.v[kHistG]);
    const double b = static_cast<double>(aggregate.v[kHistB]);
    double sse =
        (static_cast<double>(aggregate.v[kHistRR]) - r * r / w)
      + (static_cast<double>(aggregate.v[kHistGG]) - g * g / w)
      + (static_cast<double>(aggregate.v[kHistBB]) - b * b / w);
    if (sse < 0.0) {
        sse = 0.0;
    }
    return sse;
}

// A box can be cut only when it spans at least two occupied cells. The scan
// stops as soon as the second one is found, so wide boxes cost two probes
// instead of their full volume.
bool box_is_splittable(const std::uint64_t* histogram, const Box& box) {
    std::uint32_t occupied = 0u;
    for (std::uint32_t r = box.r0; r < box.r1; ++r) {
        for (std::uint32_t g = box.g0; g < box.g1; ++g) {
            for (std::uint32_t b = box.b0; b < box.b1; ++b) {
                const std::size_t cell =
                    static_cast<std::size_t>(r) * 1024u
                    + static_cast<std::size_t>(g) * 32u + b;
                if (histogram[kHistN * kCellCount + cell] != 0u) {
                    if (++occupied >= 2u) {
                        return true;
                    }
                }
            }
        }
    }
    return false;
}

std::uint32_t axis_low(const Box& box, std::uint32_t axis) {
    return axis == 0u ? box.r0 : (axis == 1u ? box.g0 : box.b0);
}

std::uint32_t axis_high(const Box& box, std::uint32_t axis) {
    return axis == 0u ? box.r1 : (axis == 1u ? box.g1 : box.b1);
}

void set_low(Box& box, std::uint32_t axis, std::uint16_t cut) {
    if (axis == 0u) {
        box.r1 = cut;
    } else if (axis == 1u) {
        box.g1 = cut;
    } else {
        box.b1 = cut;
    }
}

void set_high(Box& box, std::uint32_t axis, std::uint16_t cut) {
    if (axis == 0u) {
        box.r0 = cut;
    } else if (axis == 1u) {
        box.g0 = cut;
    } else {
        box.b0 = cut;
    }
}

std::uint32_t run_wu(const std::uint64_t* histogram,
                     const std::uint64_t* cumulative, std::uint32_t k,
                     Box* boxes, Progress* progress = nullptr) {
    if (k == 0u) {
        return 0u;
    }
    boxes[0] = Box{0u, 32u, 0u, 32u, 0u, 32u};
    std::uint32_t box_count = 1u;
    std::uint32_t cut_count = 0u;

    while (box_count < k) {
        double best_sse = -std::numeric_limits<double>::infinity();
        std::int32_t best_box = -1;
        for (std::uint32_t i = 0u; i < box_count; ++i) {
            const Aggregate aggregate = box_aggregate(cumulative, boxes[i]);
            if (aggregate.v[kHistW] == 0u) {
                continue;
            }
            if (!box_is_splittable(histogram, boxes[i])) {
                continue;
            }
            const double sse = box_sse(aggregate);
            if (sse > best_sse) {
                best_sse = sse;
                best_box = static_cast<std::int32_t>(i);
            }
        }
        if (best_box < 0) {
            break;
        }

        const Box selected = boxes[static_cast<std::uint32_t>(best_box)];
        double best_gain = -std::numeric_limits<double>::infinity();
        std::int32_t best_axis = -1;
        std::uint16_t best_cut = 0u;
        for (std::uint32_t axis = 0u; axis < 3u; ++axis) {
            const std::uint32_t low_edge = axis_low(selected, axis);
            const std::uint32_t high_edge = axis_high(selected, axis);
            for (std::uint32_t cut = low_edge + 1u; cut < high_edge; ++cut) {
                if (((++cut_count) & 0xFFu) == 0u && progress != nullptr &&
                    progress->cancelled()) {
                    return box_count;
                }
                Box low = selected;
                Box high = selected;
                set_low(low, axis, static_cast<std::uint16_t>(cut));
                set_high(high, axis, static_cast<std::uint16_t>(cut));
                const Aggregate low_aggregate =
                    box_aggregate(cumulative, low);
                const Aggregate high_aggregate =
                    box_aggregate(cumulative, high);
                if (low_aggregate.v[kHistW] == 0u ||
                    high_aggregate.v[kHistW] == 0u) {
                    continue;
                }
                const double gain = best_sse - box_sse(low_aggregate) -
                                    box_sse(high_aggregate);
                if (gain > best_gain) {
                    best_gain = gain;
                    best_axis = static_cast<std::int32_t>(axis);
                    best_cut = static_cast<std::uint16_t>(cut);
                }
            }
        }
        if (best_axis < 0) {
            break;
        }

        Box low = selected;
        Box high = selected;
        set_low(low, static_cast<std::uint32_t>(best_axis), best_cut);
        set_high(high, static_cast<std::uint32_t>(best_axis), best_cut);
        boxes[static_cast<std::uint32_t>(best_box)] = low;
        boxes[box_count] = high;
        ++box_count;
    }
    return box_count;
}

std::uint8_t clamp_round(double value) {
    const double rounded = std::floor(value + 0.5);
    if (rounded <= 0.0) {
        return 0u;
    }
    if (rounded >= 255.0) {
        return 255u;
    }
    return static_cast<std::uint8_t>(rounded);
}

}  // namespace

// The splitter's private Box layout and the test-facing WuBox layout are the
// same six 16-bit fields; the assert keeps them from drifting apart.
static_assert(sizeof(WuBox) == sizeof(Box), "WuBox must mirror Box");

std::uint32_t run_wu_boxes(const std::uint64_t* histogram,
                           const std::uint64_t* cumulative, std::uint32_t k,
                           WuBox* boxes) {
    return run_wu(histogram, cumulative, k, reinterpret_cast<Box*>(boxes));
}

bool build_wu_palette(const std::uint64_t* histogram,
                      const std::uint64_t* cumulative,
                      std::uint32_t max_colors, bool has_transparent,
                      std::byte* misc, Palette& palette,
                      Progress* progress) {
    std::memset(&palette, 0, sizeof(palette));
    std::uint32_t index = 0u;
    if (has_transparent) {
        palette.transparent_index = 0u;
        index = 1u;
    } else {
        palette.transparent_index =
            static_cast<std::uint16_t>(SQ_NO_TRANSPARENT_INDEX);
    }

    const std::uint32_t budget =
        max_colors - (has_transparent ? 1u : 0u);
    Box* boxes = reinterpret_cast<Box*>(misc + sizeof(Palette));
    const std::uint32_t box_count =
        run_wu(histogram, cumulative, budget, boxes, progress);
    if (progress != nullptr && progress->cancelled()) {
        return false;
    }

    for (std::uint32_t i = 0u; i < box_count; ++i) {
        const Aggregate aggregate = box_aggregate(cumulative, boxes[i]);
        if (aggregate.v[kHistW] == 0u || aggregate.v[kHistN] == 0u) {
            continue;
        }
        const double w = static_cast<double>(aggregate.v[kHistW]);
        const std::uint8_t r = clamp_round(
            static_cast<double>(aggregate.v[kHistR]) / w);
        const std::uint8_t g = clamp_round(
            static_cast<double>(aggregate.v[kHistG]) / w);
        const std::uint8_t b = clamp_round(
            static_cast<double>(aggregate.v[kHistB]) / w);
        const std::uint8_t a = clamp_round(
            w / static_cast<double>(aggregate.v[kHistN]));
        const std::uint32_t packed = pack_key(r, g, b, a);
        bool duplicate = false;
        for (std::uint32_t existing = 0u; existing < index; ++existing) {
            if (palette.rgba[existing] == packed) {
                duplicate = true;
                break;
            }
        }
        if (duplicate) {
            continue;
        }
        if (index >= kMaxPaletteEntries) {
            break;
        }
        palette.rgba[index] = packed;
        palette.r[index] = r;
        palette.g[index] = g;
        palette.b[index] = b;
        ++index;
    }

    if (index == 0u) {
        palette.rgba[0] = pack_key(0u, 0u, 0u, 255u);
        palette.size = 1u;
        return true;
    }
    palette.size = static_cast<std::uint16_t>(index);
    return true;
}

}  // namespace sq
