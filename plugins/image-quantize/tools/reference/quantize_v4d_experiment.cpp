// Recorded experiment: three-dimensional versus four-dimensional clustering
// over an exact RGBA colour table. The table is built directly from source
// pixels, so pixels that share RGB but differ in alpha stay separate. The
// engine's grid histogram merges those pixels into one cell before refinement;
// this experiment therefore measures the alpha failure mode on a data source
// that can express it, and its result is not transferable to the grid path
// without a new histogram structure. Nothing here is linked into the engine.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "imageio.h"
#include "metrics.h"

namespace {

constexpr std::uint32_t kBudget = 256u;
constexpr std::uint32_t kMaxIterations = 20u;

struct Sample {
    std::uint32_t key;
    std::uint32_t count;
    double r;
    double g;
    double b;
    double alpha_axis;
    double weight;
    std::uint32_t nearest;
};

struct Centre {
    double axis[4]{};
};

struct PaletteEntry {
    std::uint8_t rgba[4]{};
};

std::uint32_t pack_key(std::uint8_t r, std::uint8_t g, std::uint8_t b,
                       std::uint8_t a) {
    return (static_cast<std::uint32_t>(r) << 24) |
           (static_cast<std::uint32_t>(g) << 16) |
           (static_cast<std::uint32_t>(b) << 8) |
           static_cast<std::uint32_t>(a);
}

std::uint8_t emit_channel(double value) {
    const double rounded = std::floor(value + 0.5);
    if (rounded <= 0.0) {
        return 0u;
    }
    if (rounded >= 255.0) {
        return 255u;
    }
    return static_cast<std::uint8_t>(rounded);
}

double squared_distance(const Sample& sample, const Centre& centre,
                        bool four_dimensional, double alpha_weight) {
    const double dr = sample.r - centre.axis[0];
    const double dg = sample.g - centre.axis[1];
    const double db = sample.b - centre.axis[2];
    double total = (dr * dr + dg * dg) + db * db;
    if (four_dimensional) {
        const double da = sample.alpha_axis * alpha_weight - centre.axis[3];
        total += da * da;
    }
    return total;
}

std::vector<Sample> build_samples(const sqtools::Image& image) {
    std::map<std::uint32_t, std::uint32_t> counts;
    for (std::size_t i = 0u; i + 3u < image.rgba.size(); i += 4u) {
        const std::uint8_t a = image.rgba[i + 3u];
        const std::uint32_t key =
            a == 0u ? 0u
                    : pack_key(image.rgba[i], image.rgba[i + 1u],
                               image.rgba[i + 2u], a);
        if (a != 0u) {
            ++counts[key];
        }
    }
    std::vector<Sample> samples;
    for (const auto& item : counts) {
        const std::uint32_t key = item.first;
        const std::uint32_t alpha = key & 0xFFu;
        Sample sample{};
        sample.key = key;
        sample.count = item.second;
        sample.r = static_cast<double>((key >> 24) & 0xFFu);
        sample.g = static_cast<double>((key >> 16) & 0xFFu);
        sample.b = static_cast<double>((key >> 8) & 0xFFu);
        sample.alpha_axis = static_cast<double>(alpha) / 255.0;
        sample.weight = static_cast<double>(sample.count) * alpha;
        sample.nearest = 0u;
        samples.push_back(sample);
    }
    return samples;
}

std::vector<Centre> seed_centres(const std::vector<Sample>& samples,
                                 bool four_dimensional, double alpha_weight,
                                 std::uint32_t budget) {
    std::vector<Centre> centres;
    if (samples.empty()) {
        return centres;
    }
    std::vector<double> minimum(samples.size(), 0.0);
    std::uint32_t first = 0u;
    for (std::uint32_t i = 1u; i < samples.size(); ++i) {
        if (samples[i].weight > samples[first].weight) {
            first = i;
        }
    }
    const auto make_centre = [&](const Sample& sample) {
        Centre centre;
        centre.axis[0] = sample.r;
        centre.axis[1] = sample.g;
        centre.axis[2] = sample.b;
        centre.axis[3] = sample.alpha_axis * alpha_weight;
        return centre;
    };
    centres.push_back(make_centre(samples[first]));
    for (std::size_t i = 0u; i < samples.size(); ++i) {
        minimum[i] = squared_distance(samples[i], centres[0],
                                      four_dimensional, alpha_weight);
    }
    while (centres.size() < budget && centres.size() < samples.size()) {
        std::uint32_t best = 0u;
        double best_distance = minimum[0];
        for (std::uint32_t i = 1u; i < samples.size(); ++i) {
            if (minimum[i] > best_distance) {
                best_distance = minimum[i];
                best = i;
            }
        }
        if (best_distance == 0.0) {
            break;
        }
        const Centre added = make_centre(samples[best]);
        centres.push_back(added);
        for (std::size_t i = 0u; i < samples.size(); ++i) {
            const double candidate = squared_distance(
                samples[i], added, four_dimensional, alpha_weight);
            if (candidate < minimum[i]) {
                minimum[i] = candidate;
            }
        }
    }
    return centres;
}

struct Variant {
    std::vector<PaletteEntry> palette;
    bool has_transparent = false;
    std::uint32_t distinct = 0u;
};

Variant cluster(const std::vector<Sample>& source, bool four_dimensional,
                double alpha_weight, bool has_transparent) {
    std::vector<Sample> samples = source;
    Variant result;
    result.has_transparent = has_transparent;
    result.distinct = static_cast<std::uint32_t>(samples.size());
    const std::uint32_t budget = kBudget - (has_transparent ? 1u : 0u);
    if (samples.empty() || budget == 0u) {
        return result;
    }

    std::vector<Centre> centres =
        seed_centres(samples, four_dimensional, alpha_weight, budget);
    const std::uint32_t cluster_count =
        static_cast<std::uint32_t>(centres.size());
    std::vector<std::uint32_t> assign(samples.size(), 0u);
    double previous_objective = 0.0;

    for (std::uint32_t iteration = 0u; iteration < kMaxIterations;
         ++iteration) {
        std::vector<Centre> next = centres;
        std::vector<double> sums(cluster_count * 4u, 0.0);
        std::vector<double> weights(cluster_count, 0.0);
        double objective = 0.0;
        std::uint32_t changes = 0u;
        for (std::size_t i = 0u; i < samples.size(); ++i) {
            std::uint32_t best = 0u;
            double best_distance = squared_distance(
                samples[i], centres[0], four_dimensional, alpha_weight);
            for (std::uint32_t c = 1u; c < cluster_count; ++c) {
                const double candidate = squared_distance(
                    samples[i], centres[c], four_dimensional, alpha_weight);
                if (candidate < best_distance) {
                    best_distance = candidate;
                    best = c;
                }
            }
            if (assign[i] != best || iteration == 0u) {
                ++changes;
            }
            assign[i] = best;
            samples[i].nearest = best;
            const double weight = samples[i].weight;
            sums[best * 4u + 0u] += samples[i].r * weight;
            sums[best * 4u + 1u] += samples[i].g * weight;
            sums[best * 4u + 2u] += samples[i].b * weight;
            sums[best * 4u + 3u] += samples[i].alpha_axis * alpha_weight * weight;
            weights[best] += weight;
            objective += weight * best_distance;
        }
        for (std::uint32_t c = 0u; c < cluster_count; ++c) {
            if (weights[c] > 0.0) {
                for (std::uint32_t axis = 0u; axis < 4u; ++axis) {
                    next[c].axis[axis] =
                        sums[c * 4u + axis] / weights[c];
                }
            }
        }
        const double relative =
            iteration == 0u
                ? 1.0
                : (previous_objective - objective) /
                      (previous_objective > 1e-12 ? previous_objective : 1e-12);
        centres = next;
        previous_objective = objective;
        if ((iteration != 0u && changes == 0u) || relative < 1e-4) {
            break;
        }
    }

    std::vector<std::uint64_t> sum_w(cluster_count, 0u);
    std::vector<std::uint64_t> sum_n(cluster_count, 0u);
    for (std::size_t i = 0u; i < samples.size(); ++i) {
        const std::uint32_t c = samples[i].nearest;
        sum_w[c] += static_cast<std::uint64_t>(samples[i].weight);
        sum_n[c] += samples[i].count;
    }

    if (has_transparent) {
        PaletteEntry transparent{};
        result.palette.push_back(transparent);
    }
    for (std::uint32_t c = 0u; c < cluster_count; ++c) {
        PaletteEntry entry;
        entry.rgba[0] = emit_channel(centres[c].axis[0]);
        entry.rgba[1] = emit_channel(centres[c].axis[1]);
        entry.rgba[2] = emit_channel(centres[c].axis[2]);
        std::uint8_t alpha = 0u;
        if (sum_n[c] != 0u) {
            alpha = emit_channel(static_cast<double>(sum_w[c]) /
                                 static_cast<double>(sum_n[c]));
        }
        if (alpha == 0u) {
            alpha = 1u;
        }
        entry.rgba[3] = alpha;
        bool duplicate = false;
        const std::uint32_t first = has_transparent ? 1u : 0u;
        for (std::uint32_t j = first; j < result.palette.size(); ++j) {
            if (std::memcmp(result.palette[j].rgba, entry.rgba, 4u) == 0) {
                duplicate = true;
                break;
            }
        }
        if (!duplicate) {
            result.palette.push_back(entry);
        }
    }
    return result;
}

std::uint8_t nearest_entry(const Variant& variant, std::uint8_t r,
                           std::uint8_t g, std::uint8_t b, std::uint8_t a,
                           bool four_dimensional, double alpha_weight) {
    const std::uint32_t first = variant.has_transparent ? 1u : 0u;
    const double alpha_axis = static_cast<double>(a) / 255.0 * alpha_weight;
    std::uint32_t best = first;
    double best_distance = -1.0;
    for (std::uint32_t i = first; i < variant.palette.size(); ++i) {
        const double dr = static_cast<double>(r) - variant.palette[i].rgba[0];
        const double dg = static_cast<double>(g) - variant.palette[i].rgba[1];
        const double db = static_cast<double>(b) - variant.palette[i].rgba[2];
        double distance = (dr * dr + dg * dg) + db * db;
        if (four_dimensional) {
            const double da =
                alpha_axis - static_cast<double>(variant.palette[i].rgba[3]) /
                                 255.0 * alpha_weight;
            distance += da * da;
        }
        if (best_distance < 0.0 || distance < best_distance) {
            best_distance = distance;
            best = i;
        }
    }
    return static_cast<std::uint8_t>(best);
}

sqtools::Image reconstruct(const sqtools::Image& source,
                           const Variant& variant, bool four_dimensional,
                           double alpha_weight) {
    sqtools::Image output;
    output.width = source.width;
    output.height = source.height;
    output.rgba.resize(source.rgba.size());
    for (std::size_t i = 0u; i + 3u < source.rgba.size(); i += 4u) {
        const std::uint8_t a = source.rgba[i + 3u];
        std::uint8_t index = 0u;
        if (a != 0u || !variant.has_transparent) {
            index = nearest_entry(variant, source.rgba[i], source.rgba[i + 1u],
                                  source.rgba[i + 2u], a, four_dimensional,
                                  alpha_weight);
        }
        const PaletteEntry& entry = variant.palette[index];
        output.rgba[i + 0u] = entry.rgba[0];
        output.rgba[i + 1u] = entry.rgba[1];
        output.rgba[i + 2u] = entry.rgba[2];
        output.rgba[i + 3u] = entry.rgba[3];
    }
    return output;
}

void emit_row(const std::string& name, const char* variant, double alpha_weight,
              const Variant& result, const sqtools::Quality& quality) {
    const double psnr =
        std::isfinite(quality.psnr_db) ? quality.psnr_db : -1.0;
    std::printf(
        "{\"fixture\":\"%s\",\"variant\":\"%s\",\"w_a\":%.2f,"
        "\"distinct\":%u,\"palette_size\":%u,\"de00_mean\":%.6f,"
        "\"alpha_mae\":%.6f,\"alpha_p95\":%.6f,\"alpha_max\":%.6f,"
        "\"comp_white\":%.6f,\"comp_black\":%.6f,\"comp_checker\":%.6f,"
        "\"ssim\":%.6f,\"psnr_db\":%.6f}\n",
        name.c_str(), variant, alpha_weight, result.distinct,
        static_cast<std::uint32_t>(result.palette.size()), quality.de00_mean,
        quality.alpha_mae, quality.alpha_p95, quality.alpha_max,
        quality.composite_white, quality.composite_black,
        quality.composite_checker, quality.ssim, psnr);
}

void run_fixture(const std::string& name, const sqtools::Image& source) {
    const std::vector<Sample> samples = build_samples(source);
    bool has_transparent = false;
    for (std::size_t i = 3u; i < source.rgba.size(); i += 4u) {
        if (source.rgba[i] == 0u) {
            has_transparent = true;
            break;
        }
    }

    const Variant v3d = cluster(samples, false, 0.0, has_transparent);
    const sqtools::Quality three =
        sqtools::compute_quality(source, reconstruct(source, v3d, false, 0.0));
    emit_row(name, "v3d", 0.0, v3d, three);

    const double weights[3] = {0.5, 1.0, 2.0};
    for (double weight : weights) {
        const Variant v4d =
            cluster(samples, true, weight, has_transparent);
        const sqtools::Quality four = sqtools::compute_quality(
            source, reconstruct(source, v4d, true, weight));
        emit_row(name, "v4d", weight, v4d, four);
    }
}

sqtools::Image synthetic_soft_shadow() {
    sqtools::Image image;
    image.width = 128u;
    image.height = 128u;
    image.rgba.resize(static_cast<std::size_t>(image.width) * image.height *
                      4u);
    for (std::uint32_t y = 0u; y < image.height; ++y) {
        for (std::uint32_t x = 0u; x < image.width; ++x) {
            const std::size_t i =
                (static_cast<std::size_t>(y) * image.width + x) * 4u;
            if (x < 64u) {
                image.rgba[i + 0u] =
                    static_cast<std::uint8_t>((x * 4u + y) & 0xFFu);
                image.rgba[i + 1u] = 100u;
                image.rgba[i + 2u] = 150u;
                image.rgba[i + 3u] = 255u;
            } else {
                image.rgba[i + 0u] =
                    static_cast<std::uint8_t>(200u + y % 20u);
                image.rgba[i + 1u] = 30u;
                image.rgba[i + 2u] = 30u;
                image.rgba[i + 3u] = static_cast<std::uint8_t>(
                    1u + ((x - 64u) * 4u + y) % 255u);
            }
        }
    }
    return image;
}

}  // namespace

int main(int argc, char** argv) {
    std::vector<std::string> inputs;
    bool synthetic = false;
    for (int i = 1; i < argc; ++i) {
        const std::string argument = argv[i];
        if (argument == "--input" && i + 1 < argc) {
            inputs.push_back(argv[++i]);
        } else if (argument == "--synthetic") {
            synthetic = true;
        } else {
            std::fprintf(stderr, "usage: --input <png> ... [--synthetic]\n");
            return 2;
        }
    }
    if (inputs.empty() && !synthetic) {
        std::fprintf(stderr, "no fixture supplied\n");
        return 2;
    }
    if (synthetic) {
        run_fixture("synthetic_soft_shadow_128x128", synthetic_soft_shadow());
    }
    for (const std::string& path : inputs) {
        sqtools::Image image;
        std::string error;
        if (!sqtools::load_rgba(path, image, &error)) {
            std::fprintf(stderr, "cannot decode %s: %s\n", path.c_str(),
                         error.c_str());
            return 1;
        }
        const std::size_t slash = path.find_last_of("/\\");
        run_fixture(slash == std::string::npos ? path
                                               : path.substr(slash + 1u),
                    image);
    }
    return 0;
}
