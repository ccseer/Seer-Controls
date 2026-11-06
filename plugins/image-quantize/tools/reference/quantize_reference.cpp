// Reference quantizer: a per-pixel Lloyd k-means that never bins the image,
// used to measure how far the histogram-based objective of the engine is from
// the true per-pixel local optimum. The reference is deterministic: k-means++
// seeding runs from a fixed seed and the assignment order is the sorted colour
// order.
//
// Alpha semantics mirror the engine's lossy path: fully transparent pixels map
// to a reserved palette entry with alpha zero, while semi-transparent pixels
// are flattened to opaque, because an indexed PNG carries alpha per palette
// entry, not per pixel.
//
// This is a measurement tool, not part of the engine and not a shipping path.

#include "imageio.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <random>
#include <string>
#include <vector>

namespace {

constexpr std::uint64_t kSeed = 0x5eed1234u;

struct Weighted {
    std::uint32_t key = 0u;   // r << 24 | g << 16 | b << 8
    std::uint64_t count = 0u;
};

struct Centroid {
    double r = 0.0;
    double g = 0.0;
    double b = 0.0;
};

unsigned channel_of(std::uint32_t key, unsigned index) {
    return (key >> (24u - 8u * index)) & 0xFFu;
}

double uniform_unit(std::mt19937_64& rng) {
    return static_cast<double>(rng() >> 11u) * (1.0 / 9007199254740992.0);
}

double squared_distance(const Weighted& point, const Centroid& centroid) {
    const double dr = static_cast<double>(channel_of(point.key, 0u)) - centroid.r;
    const double dg = static_cast<double>(channel_of(point.key, 1u)) - centroid.g;
    const double db = static_cast<double>(channel_of(point.key, 2u)) - centroid.b;
    return dr * dr + dg * dg + db * db;
}

std::vector<Centroid> seed_plus_plus(const std::vector<Weighted>& points,
                                     std::uint32_t k) {
    std::mt19937_64 rng(kSeed);
    std::vector<Centroid> centroids;
    centroids.reserve(k);

    std::uint64_t total = 0u;
    for (const Weighted& point : points) {
        total += point.count;
    }
    if (total == 0u) {
        return centroids;
    }
    double target = uniform_unit(rng) * static_cast<double>(total);
    std::uint64_t running = 0u;
    for (const Weighted& point : points) {
        running += point.count;
        if (static_cast<double>(running) >= target) {
            centroids.push_back({static_cast<double>(channel_of(point.key, 0u)),
                                 static_cast<double>(channel_of(point.key, 1u)),
                                 static_cast<double>(channel_of(point.key, 2u))});
            break;
        }
    }
    if (centroids.empty()) {
        const Weighted& last = points.back();
        centroids.push_back({static_cast<double>(channel_of(last.key, 0u)),
                             static_cast<double>(channel_of(last.key, 1u)),
                             static_cast<double>(channel_of(last.key, 2u))});
    }

    std::vector<double> nearest(points.size(), 0.0);
    for (std::size_t i = 0u; i < points.size(); ++i) {
        nearest[i] = squared_distance(points[i], centroids[0]);
    }
    while (centroids.size() < k) {
        double weight_total = 0.0;
        for (std::size_t i = 0u; i < points.size(); ++i) {
            weight_total += static_cast<double>(points[i].count) * nearest[i];
        }
        if (weight_total <= 0.0) {
            break;
        }
        double pick = uniform_unit(rng) * weight_total;
        std::size_t chosen = points.size() - 1u;
        double running_weight = 0.0;
        for (std::size_t i = 0u; i < points.size(); ++i) {
            running_weight += static_cast<double>(points[i].count) * nearest[i];
            if (running_weight >= pick) {
                chosen = i;
                break;
            }
        }
        const Weighted& point = points[chosen];
        centroids.push_back({static_cast<double>(channel_of(point.key, 0u)),
                             static_cast<double>(channel_of(point.key, 1u)),
                             static_cast<double>(channel_of(point.key, 2u))});
        for (std::size_t i = 0u; i < points.size(); ++i) {
            const double distance = squared_distance(points[i], centroids.back());
            if (distance < nearest[i]) {
                nearest[i] = distance;
            }
        }
    }
    return centroids;
}

std::uint32_t run_lloyd(std::vector<Centroid>& centroids,
                        const std::vector<Weighted>& points,
                        std::uint32_t max_iterations, double& objective) {
    std::vector<std::uint32_t> assignment(points.size(), 0u);
    std::uint32_t iterations = 0u;
    for (; iterations < max_iterations; ++iterations) {
        bool changed = iterations == 0u;
        for (std::size_t i = 0u; i < points.size(); ++i) {
            const Weighted& point = points[i];
            std::uint32_t best = 0u;
            double best_distance = squared_distance(point, centroids[0]);
            for (std::size_t c = 1u; c < centroids.size(); ++c) {
                const double distance = squared_distance(point, centroids[c]);
                if (distance < best_distance) {
                    best_distance = distance;
                    best = static_cast<std::uint32_t>(c);
                }
            }
            if (best != assignment[i]) {
                assignment[i] = best;
                changed = true;
            }
        }
        std::vector<double> sum_r(centroids.size(), 0.0);
        std::vector<double> sum_g(centroids.size(), 0.0);
        std::vector<double> sum_b(centroids.size(), 0.0);
        std::vector<double> sum_w(centroids.size(), 0.0);
        for (std::size_t i = 0u; i < points.size(); ++i) {
            const std::uint32_t c = assignment[i];
            const double weight = static_cast<double>(points[i].count);
            sum_r[c] += static_cast<double>(channel_of(points[i].key, 0u)) * weight;
            sum_g[c] += static_cast<double>(channel_of(points[i].key, 1u)) * weight;
            sum_b[c] += static_cast<double>(channel_of(points[i].key, 2u)) * weight;
            sum_w[c] += weight;
        }
        for (std::size_t c = 0u; c < centroids.size(); ++c) {
            if (sum_w[c] == 0.0) {
                continue;
            }
            centroids[c].r = sum_r[c] / sum_w[c];
            centroids[c].g = sum_g[c] / sum_w[c];
            centroids[c].b = sum_b[c] / sum_w[c];
        }
        if (!changed) {
            ++iterations;
            break;
        }
    }
    objective = 0.0;
    for (std::size_t i = 0u; i < points.size(); ++i) {
        objective += squared_distance(points[i], centroids[assignment[i]])
                   * static_cast<double>(points[i].count);
    }
    return iterations;
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

int main(int argc, char** argv) {
    std::string input;
    std::string output;
    std::string stats_path;
    std::uint32_t colors = 256u;
    std::uint32_t max_iterations = 100u;
    for (int i = 1; i < argc; ++i) {
        const std::string key = argv[i];
        auto next = [&](std::string& destination) -> bool {
            if (i + 1 >= argc) {
                return false;
            }
            destination = argv[++i];
            return true;
        };
        if (key == "--input") {
            if (!next(input)) return 2;
        } else if (key == "--output") {
            if (!next(output)) return 2;
        } else if (key == "--stats") {
            if (!next(stats_path)) return 2;
        } else if (key == "--colors") {
            std::string value;
            if (!next(value)) return 2;
            colors = static_cast<std::uint32_t>(std::stoul(value));
        } else if (key == "--iterations") {
            std::string value;
            if (!next(value)) return 2;
            max_iterations = static_cast<std::uint32_t>(std::stoul(value));
        } else {
            std::fprintf(stderr, "quantize_reference: unknown option %s\n", key.c_str());
            return 2;
        }
    }
    if (input.empty() || output.empty()) {
        std::fprintf(stderr,
                     "usage: quantize_reference --input <png> --output <png> "
                     "[--colors 256] [--iterations 100] [--stats <path>]\n");
        return 2;
    }

    sqtools::Image image;
    std::string error;
    if (!sqtools::load_rgba(input, image, &error)) {
        std::fprintf(stderr, "quantize_reference: %s\n", error.c_str());
        return 1;
    }

    // Inputs with fully transparent pixels reserve palette index 0 for them,
    // mirroring the engine's transparent slot, so the emitted PNG keeps the
    // original transparency instead of painting it in the first centroid.
    bool has_transparent = false;
    for (std::size_t i = 3u; i < image.rgba.size(); i += 4u) {
        if (image.rgba[i] == 0u) {
            has_transparent = true;
            break;
        }
    }
    const std::uint32_t opaque_colors =
        has_transparent ? colors - 1u : colors;
    if (opaque_colors == 0u) {
        std::fprintf(stderr, "quantize_reference: colour budget exhausted by "
                             "the transparent reservation\n");
        return 1;
    }

    std::vector<Weighted> points;
    points.reserve(image.rgba.size() / 4u);
    for (std::size_t i = 0u; i < image.rgba.size(); i += 4u) {
        if (image.rgba[i + 3u] == 0u) {
            continue;
        }
        const std::uint32_t packed =
            (static_cast<std::uint32_t>(image.rgba[i + 0u]) << 24)
          | (static_cast<std::uint32_t>(image.rgba[i + 1u]) << 16)
          | (static_cast<std::uint32_t>(image.rgba[i + 2u]) << 8);
        points.push_back({packed, 1u});
    }
    std::sort(points.begin(), points.end(),
              [](const Weighted& a, const Weighted& b) { return a.key < b.key; });
    std::vector<Weighted> unique;
    unique.reserve(points.size());
    for (const Weighted& point : points) {
        if (!unique.empty() && unique.back().key == point.key) {
            unique.back().count += 1u;
        } else {
            unique.push_back(point);
        }
    }

    std::vector<Centroid> centroids = seed_plus_plus(unique, opaque_colors);
    if (centroids.empty()) {
        std::fprintf(stderr, "quantize_reference: empty image\n");
        return 1;
    }
    double objective = 0.0;
    const std::uint32_t iterations =
        run_lloyd(centroids, unique, max_iterations, objective);

    const std::size_t base = has_transparent ? 1u : 0u;
    std::vector<std::uint8_t> palette(static_cast<std::size_t>(colors) * 4u, 0u);
    if (has_transparent) {
        palette[0u] = 0u;
        palette[1u] = 0u;
        palette[2u] = 0u;
        palette[3u] = 0u;
    }
    std::vector<std::uint32_t> keys;
    keys.reserve(centroids.size());
    for (const Centroid& centroid : centroids) {
        const std::uint8_t r = clamp_round(centroid.r);
        const std::uint8_t g = clamp_round(centroid.g);
        const std::uint8_t b = clamp_round(centroid.b);
        const std::uint32_t packed =
            (static_cast<std::uint32_t>(r) << 24)
          | (static_cast<std::uint32_t>(g) << 16)
          | (static_cast<std::uint32_t>(b) << 8) | 0xFFu;
        bool duplicate = false;
        for (std::uint32_t existing : keys) {
            if (existing == packed) {
                duplicate = true;
                break;
            }
        }
        if (duplicate) {
            continue;
        }
        const std::size_t index = base + keys.size();
        palette[index * 4u + 0u] = r;
        palette[index * 4u + 1u] = g;
        palette[index * 4u + 2u] = b;
        palette[index * 4u + 3u] = 255u;
        keys.push_back(packed);
    }

    std::vector<std::uint8_t> indices(static_cast<std::size_t>(image.width) * image.height, 0u);
    for (std::size_t i = 0u; i < indices.size(); ++i) {
        const std::uint8_t alpha = image.rgba[i * 4u + 3u];
        if (alpha == 0u) {
            indices[i] = 0u;
            continue;
        }
        const std::uint32_t packed =
            (static_cast<std::uint32_t>(image.rgba[i * 4u + 0u]) << 24)
          | (static_cast<std::uint32_t>(image.rgba[i * 4u + 1u]) << 16)
          | (static_cast<std::uint32_t>(image.rgba[i * 4u + 2u]) << 8) | 0xFFu;
        std::uint32_t best = 0u;
        double best_distance = 0.0;
        for (std::size_t c = 0u; c < keys.size(); ++c) {
            const double dr = static_cast<double>((packed >> 24) & 0xFFu)
                            - static_cast<double>((keys[c] >> 24) & 0xFFu);
            const double dg = static_cast<double>((packed >> 16) & 0xFFu)
                            - static_cast<double>((keys[c] >> 16) & 0xFFu);
            const double db = static_cast<double>((packed >> 8) & 0xFFu)
                            - static_cast<double>((keys[c] >> 8) & 0xFFu);
            const double distance = dr * dr + dg * dg + db * db;
            if (c == 0u || distance < best_distance) {
                best_distance = distance;
                best = static_cast<std::uint32_t>(c);
            }
        }
        indices[i] = static_cast<std::uint8_t>(base + best);
    }

    std::vector<std::uint8_t> png;
    if (!sqtools::encode_indexed_png(indices, image.width, image.height,
                                     palette.data(),
                                     static_cast<std::uint16_t>(base + keys.size()),
                                     png, &error)) {
        std::fprintf(stderr, "quantize_reference: %s\n", error.c_str());
        return 1;
    }
    if (!sqtools::write_file(output, png, &error)) {
        std::fprintf(stderr, "quantize_reference: %s\n", error.c_str());
        return 1;
    }

    std::printf("REFERENCE seed=%llu iterations=%u unique=%zu palette=%zu sse=%.6f\n",
                static_cast<unsigned long long>(kSeed), iterations, unique.size(),
                base + keys.size(), objective);
    if (!stats_path.empty()) {
        char buffer[256] = {};
        std::snprintf(buffer, sizeof(buffer),
                      "{\"seed\":%llu,\"iterations\":%u,\"unique\":%zu,"
                      "\"palette\":%zu,\"sse\":%.6f}",
                      static_cast<unsigned long long>(kSeed), iterations,
                      unique.size(), base + keys.size(), objective);
        std::vector<std::uint8_t> bytes(buffer, buffer + std::strlen(buffer));
        if (!sqtools::write_file(stats_path, bytes, &error)) {
            std::fprintf(stderr, "quantize_reference: %s\n", error.c_str());
            return 1;
        }
    }
    return 0;
}
