#include "metrics.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
#include <sstream>
#include <vector>

#include "sq_color.h"

namespace sqtools {
namespace {

struct Sample {
    double value;
    double weight;
};

constexpr double kDefinitionVersion = 1.0;

double weighted_percentile(std::vector<Sample> samples, double fraction) {
    if (samples.empty()) {
        return 0.0;
    }
    std::sort(samples.begin(), samples.end(),
              [](const Sample& a, const Sample& b) { return a.value < b.value; });
    double total = 0.0;
    for (const Sample& sample : samples) {
        total += sample.weight;
    }
    double cumulative = 0.0;
    for (const Sample& sample : samples) {
        cumulative += sample.weight;
        if (cumulative >= fraction * total) {
            return sample.value;
        }
    }
    return samples.back().value;
}

std::uint8_t composite_channel(std::uint8_t foreground, std::uint8_t alpha,
                               std::uint8_t background) {
    const double a = static_cast<double>(alpha) / 255.0;
    const double value = static_cast<double>(foreground) * a
                       + static_cast<double>(background) * (1.0 - a);
    const double clamped = std::clamp(value, 0.0, 255.0);
    return static_cast<std::uint8_t>(std::lround(clamped));
}

std::uint8_t checker_background(std::uint32_t x, std::uint32_t y) {
    return ((x / 8u + y / 8u) % 2u) == 0u ? 0u : 255u;
}

double luma(const std::uint8_t* pixel) {
    return 0.2126 * static_cast<double>(pixel[0])
         + 0.7152 * static_cast<double>(pixel[1])
         + 0.0722 * static_cast<double>(pixel[2]);
}

// 11x11 Gaussian weighted SSIM over luma, excluding a < 16 pixels (6.3).
bool ssim_value(const Image& a, const Image& b, double& value) {
    if (a.width < 11u || a.height < 11u || a.width != b.width || a.height != b.height) {
        return false;
    }
    double kernel[11][11] = {};
    double kernel_sum = 0.0;
    for (int dy = -5; dy <= 5; ++dy) {
        for (int dx = -5; dx <= 5; ++dx) {
            const double weight = std::exp(-(static_cast<double>(dx * dx + dy * dy))
                                           / (2.0 * 1.5 * 1.5));
            kernel[dy + 5][dx + 5] = weight;
            kernel_sum += weight;
        }
    }
    for (int y = 0; y < 11; ++y) {
        for (int x = 0; x < 11; ++x) {
            kernel[y][x] /= kernel_sum;
        }
    }

    const double C1 = (0.01 * 255.0) * (0.01 * 255.0);
    const double C2 = (0.03 * 255.0) * (0.03 * 255.0);
    const std::size_t w = a.width;
    const std::size_t h = a.height;
    double total = 0.0;
    std::size_t windows = 0u;
    for (std::size_t cy = 5u; cy + 5u < h; ++cy) {
        for (std::size_t cx = 5u; cx + 5u < w; ++cx) {
            const std::size_t center = (cy * w + cx) * 4u;
            if (a.rgba[center + 3u] < 16u || b.rgba[center + 3u] < 16u) {
                continue;
            }
            double weight_sum = 0.0;
            double mu1 = 0.0, mu2 = 0.0;
            for (int dy = -5; dy <= 5; ++dy) {
                for (int dx = -5; dx <= 5; ++dx) {
                    const std::size_t offset =
                        ((cy + static_cast<std::size_t>(dy + 5) - 5u) * w
                         + cx + static_cast<std::size_t>(dx + 5) - 5u) * 4u;
                    if (a.rgba[offset + 3u] < 16u || b.rgba[offset + 3u] < 16u) {
                        continue;
                    }
                    const double weight = kernel[dy + 5][dx + 5];
                    weight_sum += weight;
                    mu1 += weight * luma(&a.rgba[offset]);
                    mu2 += weight * luma(&b.rgba[offset]);
                }
            }
            if (weight_sum <= 0.0) {
                continue;
            }
            mu1 /= weight_sum;
            mu2 /= weight_sum;
            double var1 = 0.0, var2 = 0.0, cov = 0.0;
            double renormalized = 0.0;
            for (int dy = -5; dy <= 5; ++dy) {
                for (int dx = -5; dx <= 5; ++dx) {
                    const std::size_t offset =
                        ((cy + static_cast<std::size_t>(dy + 5) - 5u) * w
                         + cx + static_cast<std::size_t>(dx + 5) - 5u) * 4u;
                    if (a.rgba[offset + 3u] < 16u || b.rgba[offset + 3u] < 16u) {
                        continue;
                    }
                    const double weight = kernel[dy + 5][dx + 5];
                    const double d1 = luma(&a.rgba[offset]) - mu1;
                    const double d2 = luma(&b.rgba[offset]) - mu2;
                    var1 += weight * d1 * d1;
                    var2 += weight * d2 * d2;
                    cov += weight * d1 * d2;
                    renormalized += weight;
                }
            }
            if (renormalized <= 0.0) {
                continue;
            }
            var1 /= renormalized;
            var2 /= renormalized;
            cov /= renormalized;
            const double luminance = (2.0 * mu1 * mu2 + C1) / (mu1 * mu1 + mu2 * mu2 + C1);
            const double contrast = (2.0 * cov + C2) / (var1 + var2 + C2);
            total += luminance * contrast;
            ++windows;
        }
    }
    if (windows == 0u) {
        return false;
    }
    value = total / static_cast<double>(windows);
    return true;
}

double composite_delta_e(const Image& source, const Image& output, int background_mode) {
    std::vector<Sample> samples;
    const std::size_t count = static_cast<std::size_t>(source.width) * source.height;
    for (std::size_t i = 0; i < count; ++i) {
        const std::uint8_t* s = &source.rgba[i * 4u];
        const std::uint8_t* o = &output.rgba[i * 4u];
        if (s[3] == 0u) {
            continue;
        }
        const std::uint32_t x = static_cast<std::uint32_t>(i % source.width);
        const std::uint32_t y = static_cast<std::uint32_t>(i / source.width);
        std::uint8_t bg = 0u;
        if (background_mode == 1) {
            bg = 255u;
        } else if (background_mode == 2) {
            bg = checker_background(x, y);
        }
        const sq_color::Lab a = sq_color::srgb8_to_lab(
            composite_channel(s[0], s[3], bg),
            composite_channel(s[1], s[3], bg),
            composite_channel(s[2], s[3], bg));
        const sq_color::Lab b = sq_color::srgb8_to_lab(
            composite_channel(o[0], o[3], bg),
            composite_channel(o[1], o[3], bg),
            composite_channel(o[2], o[3], bg));
        samples.push_back({sq_color::delta_e_2000(a, b),
                           static_cast<double>(s[3]) / 255.0});
    }
    double total = 0.0;
    double weights = 0.0;
    for (const Sample& sample : samples) {
        total += sample.value * sample.weight;
        weights += sample.weight;
    }
    return weights > 0.0 ? total / weights : 0.0;
}

// JSON has no infinity literal, and an exact-path run legitimately produces an
// infinite PSNR. Emit it as a string so the row stays parseable.
std::string format_double(double value) {
    if (std::isinf(value)) {
        return "inf";
    }
    if (std::isnan(value)) {
        return "n/a";
    }
    char buffer[32] = {};
    std::snprintf(buffer, sizeof(buffer), "%.6f", value);
    return std::string(buffer);
}

// Paths reach the row on Windows, so backslashes have to be escaped or the
// row is not valid JSON.
std::string json_escape(const std::string& text) {
    std::string out;
    out.reserve(text.size() + 8u);
    for (char ch : text) {
        if (ch == '\\') {
            out += "\\\\";
        } else if (ch == '"') {
            out += "\\\"";
        } else {
            out += ch;
        }
    }
    return out;
}

std::string json_double(double value) {
    if (std::isinf(value)) {
        return "\"inf\"";
    }
    if (std::isnan(value)) {
        return "\"n/a\"";
    }
    return format_double(value);
}

}  // namespace

Quality compute_quality(const Image& source, const Image& output) {
    Quality q;
    if (source.width != output.width || source.height != output.height
        || source.rgba.size() != output.rgba.size() || source.rgba.empty()) {
        return q;
    }
    const std::size_t count = static_cast<std::size_t>(source.width) * source.height;
    std::vector<Sample> de00_samples;
    std::vector<Sample> cie76_samples;
    std::vector<Sample> alpha_errors;
    double mse_sum = 0.0;
    std::size_t mse_terms = 0u;
    double opaque_sum = 0.0;
    std::size_t opaque_count = 0u;
    double alpha_sum = 0.0;
    double alpha_max = 0.0;

    for (std::size_t i = 0; i < count; ++i) {
        const std::uint8_t* s = &source.rgba[i * 4u];
        const std::uint8_t* o = &output.rgba[i * 4u];
        if (s[3] > 0u) {
            const sq_color::Lab a = sq_color::srgb8_to_lab(s[0], s[1], s[2]);
            const sq_color::Lab b = sq_color::srgb8_to_lab(o[0], o[1], o[2]);
            const double de = sq_color::delta_e_2000(a, b);
            const double weight = static_cast<double>(s[3]) / 255.0;
            de00_samples.push_back({de, weight});
            cie76_samples.push_back({sq_color::delta_e_76(a, b), weight});
            if (s[3] == 255u) {
                opaque_sum += de;
                ++opaque_count;
            }
            for (std::uint32_t channel = 0u; channel < 3u; ++channel) {
                const double delta = static_cast<double>(s[channel])
                                   - static_cast<double>(o[channel]);
                mse_sum += delta * delta;
            }
            mse_terms += 3u;
        }
        if (s[3] > 0u || o[3] > 0u) {
            const double error = std::fabs(static_cast<double>(s[3])
                                          - static_cast<double>(o[3]));
            alpha_errors.push_back({error, 1.0});
            alpha_sum += error;
            alpha_max = std::max(alpha_max, error);
        }
    }

    if (!de00_samples.empty()) {
        double total = 0.0, weights = 0.0, cie_total = 0.0;
        for (const Sample& sample : de00_samples) {
            total += sample.value * sample.weight;
            weights += sample.weight;
        }
        for (const Sample& sample : cie76_samples) {
            cie_total += sample.value * sample.weight;
        }
        q.de00_mean = total / weights;
        q.cie76_mean = cie_total / weights;
        q.de00_p95 = weighted_percentile(de00_samples, 0.95);
        q.de00_p99 = weighted_percentile(de00_samples, 0.99);
        q.sample_count = static_cast<std::uint32_t>(de00_samples.size());
    }
    q.de00_opaque_mean = opaque_count > 0u
                             ? opaque_sum / static_cast<double>(opaque_count) : 0.0;

    if (mse_terms > 0u) {
        const double mse = mse_sum / static_cast<double>(mse_terms);
        q.psnr_db = mse == 0.0 ? std::numeric_limits<double>::infinity()
                               : 10.0 * std::log10((255.0 * 255.0) / mse);
    }
    q.ssim_defined = ssim_value(source, output, q.ssim);

    if (!alpha_errors.empty()) {
        q.alpha_mae = alpha_sum / static_cast<double>(alpha_errors.size());
        q.alpha_p95 = weighted_percentile(alpha_errors, 0.95);
        q.alpha_max = alpha_max;
    }
    q.composite_black = composite_delta_e(source, output, 0);
    q.composite_white = composite_delta_e(source, output, 1);
    q.composite_checker = composite_delta_e(source, output, 2);
    return q;
}

bool banding_step(const Image& image, double& step) {
    step = 0.0;
    const bool horizontal = image.width >= image.height;
    const std::uint32_t length = horizontal ? image.width : image.height;
    const std::uint32_t cross = horizontal ? image.height : image.width;
    if (length < 17u || cross == 0u) {
        return false;
    }
    std::vector<double> profile(length, 0.0);
    for (std::uint32_t i = 0; i < length; ++i) {
        double sum = 0.0;
        std::uint32_t valid = 0u;
        for (std::uint32_t j = 0; j < cross; ++j) {
            const std::uint32_t x = horizontal ? i : j;
            const std::uint32_t y = horizontal ? j : i;
            const std::size_t offset = (static_cast<std::size_t>(y) * image.width + x) * 4u;
            if (image.rgba[offset + 3u] > 0u) {
                sum += luma(&image.rgba[offset]);
                ++valid;
            }
        }
        if (valid == 0u) {
            return false;
        }
        profile[i] = sum / static_cast<double>(valid);
    }
    std::vector<double> lightness(length, 0.0);
    for (std::uint32_t i = 0; i < length; ++i) {
        double sum = 0.0;
        std::uint32_t samples = 0u;
        for (int delta = -7; delta <= 8; ++delta) {
            const long index = static_cast<long>(i) + delta;
            if (index < 0 || index >= static_cast<long>(length)) {
                continue;
            }
            sum += profile[static_cast<std::size_t>(index)];
            ++samples;
        }
        const double smoothed = sum / static_cast<double>(samples);
        const std::uint8_t gray = static_cast<std::uint8_t>(
            std::lround(std::clamp(smoothed, 0.0, 255.0)));
        lightness[i] = sq_color::lstar_from_gray8(gray);
    }
    double maximum = 0.0;
    for (std::uint32_t i = 0; i + 1u < length; ++i) {
        maximum = std::max(maximum, std::fabs(lightness[i + 1u] - lightness[i]));
    }
    step = maximum;
    return true;
}

bool metrics_selfcheck(std::string& report) {
    std::ostringstream out;
    bool ok = true;
    auto expect = [&](bool condition, const char* label, double actual, double expected) {
        out << label << ": actual=" << format_double(actual)
            << " expected=" << format_double(expected)
            << (condition ? " PASS" : " FAIL") << "\n";
        if (!condition) {
            ok = false;
        }
    };

    Image black, white;
    black.width = black.height = 4u;
    black.rgba.assign(4u * 4u * 4u, 0u);
    white.rgba.assign(4u * 4u * 4u, 255u);
    white.width = white.height = 4u;
    for (std::size_t i = 0; i < 16u; ++i) {
        black.rgba[i * 4u + 3u] = 255u;
    }
    // The published D65 matrix is rounded to seven decimals, so white lands
    // at L* = 100.000004 rather than exactly 100. The hand-checked target is
    // 100 with a tolerance that accounts for that transcription precision.
    Quality q = compute_quality(black, white);
    expect(std::fabs(q.de00_mean - 100.0) < 1e-4, "4x4 black-to-white dE00",
           q.de00_mean, 100.0);
    expect(std::fabs(q.cie76_mean - 100.0) < 1e-4, "4x4 black-to-white CIE76",
           q.cie76_mean, 100.0);
    expect(std::fabs(q.psnr_db) < 1e-9, "4x4 black-to-white PSNR dB", q.psnr_db, 0.0);
    expect(q.alpha_mae == 0.0, "4x4 alpha MAE", q.alpha_mae, 0.0);
    out << "4x4 SSIM undefined (image smaller than 11x11): "
        << (q.ssim_defined ? "FAIL" : "PASS") << "\n";
    if (q.ssim_defined) {
        ok = false;
    }

    Image identical;
    identical.width = identical.height = 16u;
    identical.rgba.assign(16u * 16u * 4u, 0u);
    for (std::size_t i = 0; i < 16u * 16u; ++i) {
        identical.rgba[i * 4u + 0u] = 50u;
        identical.rgba[i * 4u + 1u] = 100u;
        identical.rgba[i * 4u + 2u] = 150u;
        identical.rgba[i * 4u + 3u] = 255u;
    }
    Quality r = compute_quality(identical, identical);
    expect(r.de00_mean == 0.0, "16x16 identical dE00", r.de00_mean, 0.0);
    expect(std::isinf(r.psnr_db), "16x16 identical PSNR dB", r.psnr_db, 0.0);
    expect(r.ssim_defined && std::fabs(r.ssim - 1.0) < 1e-9,
           "16x16 identical SSIM", r.ssim, 1.0);
    expect(r.alpha_max == 0.0, "16x16 identical alpha max", r.alpha_max, 0.0);

    std::vector<Sample> percentile{{1.0, 1.0}, {2.0, 1.0}, {3.0, 1.0}, {4.0, 1.0}};
    const double p95 = weighted_percentile(percentile, 0.95);
    expect(std::fabs(p95 - 4.0) < 1e-9, "weighted percentile p95", p95, 4.0);

    double step = 0.0;
    const bool banding_defined = banding_step(identical, step);
    out << "banding on non-gradient fixture: "
        << (banding_defined ? "unexpectedly defined" : "not defined (PASS)") << "\n";

    out << "definition version: spec-003-r6 sections 6.1-6.7 (v"
        << kDefinitionVersion << ")\n";
    report = out.str();
    return ok;
}

std::string quality_json_row(const std::string& fixture, const Quality& q) {
    std::ostringstream out;
    out << "{\"fixture\":\"" << json_escape(fixture)
        << "\",\"definitions\":\"spec-003-r6-6.1..6.7-v"
        << kDefinitionVersion << "\",\"de00_mean\":" << json_double(q.de00_mean)
        << ",\"de00_p95\":" << json_double(q.de00_p95)
        << ",\"de00_p99\":" << json_double(q.de00_p99)
        << ",\"cie76_mean\":" << json_double(q.cie76_mean)
        << ",\"psnr_db\":" << json_double(q.psnr_db)
        << ",\"ssim\":" << (q.ssim_defined ? json_double(q.ssim) : std::string("\"n/a\""))
        << ",\"alpha_mae\":" << json_double(q.alpha_mae)
        << ",\"alpha_p95\":" << json_double(q.alpha_p95)
        << ",\"alpha_max\":" << json_double(q.alpha_max)
        << ",\"composite_white\":" << json_double(q.composite_white)
        << ",\"composite_black\":" << json_double(q.composite_black)
        << ",\"composite_checker\":" << json_double(q.composite_checker)
        << ",\"banding_step\":" << (q.banding_defined ? json_double(q.banding_step)
                                                      : std::string("\"n/a\""))
        << ",\"sample_count\":" << q.sample_count << "}";
    return out.str();
}

std::string quality_markdown_row(const std::string& fixture, const Quality& q) {
    std::ostringstream out;
    out << "| " << fixture << " | " << format_double(q.de00_mean)
        << " | " << format_double(q.de00_p95)
        << " | " << format_double(q.cie76_mean)
        << " | " << format_double(q.psnr_db)
        << " | " << (q.ssim_defined ? format_double(q.ssim) : std::string("n/a")) << " |";
    return out.str();
}

}  // namespace sqtools
