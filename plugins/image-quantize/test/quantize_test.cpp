#include "check.h"

#include <cmath>
#include <cstring>
#include <limits>
#include <thread>
#include <vector>

#include "seer_quantize.h"
#include "sq_color.h"
#include "sq_internal.h"
#include "sq_kmeans.h"
#include "sq_dither.h"

namespace {

// Sharma, Wu, Dalal (2005) supplementary test data, 34 pairs. Source:
// https://hajim.rochester.edu/ece/sites/gsharma/ciede2000/dataNprograms/ciede2000testdata.txt
// Downloaded 2026-11-04 (1830 bytes, 34 LF-terminated rows). The primary file
// is archived with the build evidence.
struct Ciede2000Case {
    double L1, a1, b1, L2, a2, b2, expected;
};

constexpr Ciede2000Case kSharmaCases[] = {
    {50.0000, 2.6772, -79.7751, 50.0000, 0.0000, -82.7485, 2.0425},
    {50.0000, 3.1571, -77.2803, 50.0000, 0.0000, -82.7485, 2.8615},
    {50.0000, 2.8361, -74.0200, 50.0000, 0.0000, -82.7485, 3.4412},
    {50.0000, -1.3802, -84.2814, 50.0000, 0.0000, -82.7485, 1.0000},
    {50.0000, -1.1848, -84.8006, 50.0000, 0.0000, -82.7485, 1.0000},
    {50.0000, -0.9009, -85.5211, 50.0000, 0.0000, -82.7485, 1.0000},
    {50.0000, 0.0000, 0.0000, 50.0000, -1.0000, 2.0000, 2.3669},
    {50.0000, -1.0000, 2.0000, 50.0000, 0.0000, 0.0000, 2.3669},
    {50.0000, 2.4900, -0.0010, 50.0000, -2.4900, 0.0009, 7.1792},
    {50.0000, 2.4900, -0.0010, 50.0000, -2.4900, 0.0010, 7.1792},
    {50.0000, 2.4900, -0.0010, 50.0000, -2.4900, 0.0011, 7.2195},
    {50.0000, 2.4900, -0.0010, 50.0000, -2.4900, 0.0012, 7.2195},
    {50.0000, -0.0010, 2.4900, 50.0000, 0.0009, -2.4900, 4.8045},
    {50.0000, -0.0010, 2.4900, 50.0000, 0.0010, -2.4900, 4.8045},
    {50.0000, -0.0010, 2.4900, 50.0000, 0.0011, -2.4900, 4.7461},
    {50.0000, 2.5000, 0.0000, 50.0000, 0.0000, -2.5000, 4.3065},
    {50.0000, 2.5000, 0.0000, 73.0000, 25.0000, -18.0000, 27.1492},
    {50.0000, 2.5000, 0.0000, 61.0000, -5.0000, 29.0000, 22.8977},
    {50.0000, 2.5000, 0.0000, 56.0000, -27.0000, -3.0000, 31.9030},
    {50.0000, 2.5000, 0.0000, 58.0000, 24.0000, 15.0000, 19.4535},
    {50.0000, 2.5000, 0.0000, 50.0000, 3.1736, 0.5854, 1.0000},
    {50.0000, 2.5000, 0.0000, 50.0000, 3.2972, 0.0000, 1.0000},
    {50.0000, 2.5000, 0.0000, 50.0000, 1.8634, 0.5757, 1.0000},
    {50.0000, 2.5000, 0.0000, 50.0000, 3.2592, 0.3350, 1.0000},
    {60.2574, -34.0099, 36.2677, 60.4626, -34.1751, 39.4387, 1.2644},
    {63.0109, -31.0961, -5.8663, 62.8187, -29.7946, -4.0864, 1.2630},
    {61.2901, 3.7196, -5.3901, 61.4292, 2.2480, -4.9620, 1.8731},
    {35.0831, -44.1164, 3.7933, 35.0232, -40.0716, 1.5901, 1.8645},
    {22.7233, 20.0904, -46.6940, 23.0331, 14.9730, -42.5619, 2.0373},
    {36.4612, 47.8580, 18.3852, 36.2715, 50.5065, 21.2231, 1.4146},
    {90.8027, -2.0831, 1.4410, 91.1528, -1.6435, 0.0447, 1.4441},
    {90.9257, -0.5406, -0.9208, 88.6381, -0.8985, -0.7239, 1.5381},
    {6.7747, -0.2908, -2.4247, 5.8714, -0.0985, -2.2286, 0.6377},
    {2.0776, 0.0795, -1.1350, 0.9033, -0.0636, -0.5514, 0.9082},
};

struct Fixture {
    std::vector<std::uint8_t> pixels;
    std::vector<std::uint8_t> indices;
    sq_image_in image{};
    sq_result result{};
    sq_stats stats{};

    explicit Fixture(std::uint32_t width, std::uint32_t height, bool transparent = false) {
        const std::size_t count = static_cast<std::size_t>(width) * height;
        pixels.assign(count * 4u, 0u);
        indices.assign(count, 0xFFu);
        for (std::size_t i = 0; i < count; ++i) {
            pixels[i * 4u + 0u] = 10u;
            pixels[i * 4u + 1u] = 20u;
            pixels[i * 4u + 2u] = 30u;
            pixels[i * 4u + 3u] = transparent ? 0u : 255u;
        }
        image.struct_size = sizeof(sq_image_in);
        image.version = SQ_ABI_VERSION;
        image.pixels = pixels.data();
        image.width = width;
        image.height = height;
        image.stride = static_cast<std::size_t>(width) * 4u;
        result.struct_size = sizeof(sq_result);
        result.version = SQ_ABI_VERSION;
        result.indices = indices.data();
        result.indices_capacity = count;
        stats.struct_size = sizeof(sq_stats);
        stats.version = SQ_ABI_VERSION;
    }
};

sq_params defaults() {
    sq_params params{};
    SQ_CHECK_EQ(sq_params_init(&params), SQ_OK);
    return params;
}

struct CancelState {
    int calls = 0;
    bool finished = false;
    bool fired_after_return = false;
    std::uint32_t cancel_at = 0u;
};

int cancel_callback(std::uint32_t percent, void* user) {
    auto* state = static_cast<CancelState*>(user);
    ++state->calls;
    if (state->finished) {
        state->fired_after_return = true;
    }
    return percent >= state->cancel_at ? 1 : 0;
}

void test_u3_ciede2000() {
    double worst = 0.0;
    for (std::size_t i = 0; i < sizeof(kSharmaCases) / sizeof(kSharmaCases[0]); ++i) {
        const Ciede2000Case& c = kSharmaCases[i];
        const double actual = sq_color::delta_e_2000(
            {c.L1, c.a1, c.b1}, {c.L2, c.a2, c.b2});
        worst = std::max(worst, std::fabs(actual - c.expected));
        SQ_CHECK_NEAR(actual, c.expected, 1e-4);
    }
    SQ_CHECK(worst <= 1e-4);
    SQ_CHECK_EQ(sizeof(kSharmaCases) / sizeof(kSharmaCases[0]), 34u);
}

void test_u13_and_u14() {
    SQ_CHECK_EQ(sq_params_init(nullptr), SQ_ERR_INVALID_ARG);
    SQ_CHECK_EQ(sq_params_validate(nullptr), SQ_ERR_INVALID_ARG);

    sq_params params = defaults();
    SQ_CHECK_EQ(params.struct_size, sizeof(sq_params));
    SQ_CHECK_EQ(params.version, SQ_ABI_VERSION);
    SQ_CHECK_EQ(params.max_colors, 256u);
    SQ_CHECK_EQ(params.alpha_mode, SQ_ALPHA_STRAIGHT);
    SQ_CHECK_EQ(params.color_space, SQ_SPACE_SRGB);
    SQ_CHECK_EQ(params.dither_mode, SQ_DITHER_NONE);
    SQ_CHECK(params.dither_strength == 0.0f);
    SQ_CHECK(params.kmeans_tolerance > 0.0f && params.kmeans_tolerance <= 0.1f);
    SQ_CHECK_EQ(params.kmeans_max_iter, 20u);
    SQ_CHECK_EQ(params.thread_count, SQ_AUTO_THREADS);
    SQ_CHECK_EQ(params.pipeline, SQ_PIPELINE_AUTO);
    SQ_CHECK(params.reserved0[0] == 0u && params.reserved0[1] == 0u
             && params.reserved0[2] == 0u);
    SQ_CHECK_EQ(sq_params_validate(&params), SQ_OK);

    sq_params undersized = params;
    undersized.struct_size = sizeof(sq_params) - sizeof(std::uint32_t);
    SQ_CHECK_EQ(sq_params_validate(&undersized), SQ_ERR_UNSUPPORTED_VERSION);
    sq_params larger = params;
    larger.struct_size = sizeof(sq_params) + 16u;
    SQ_CHECK_EQ(sq_params_validate(&larger), SQ_OK);
    sq_params wrong_version = params;
    wrong_version.version = SQ_ABI_VERSION + 1u;
    SQ_CHECK_EQ(sq_params_validate(&wrong_version), SQ_ERR_UNSUPPORTED_VERSION);

    auto expect_invalid = [&params](const sq_params& candidate) {
        SQ_CHECK_EQ(sq_params_validate(&candidate), SQ_ERR_INVALID_ARG);
        (void)params;
    };

    sq_params p = params; p.max_colors = 1u; expect_invalid(p);
    p = params; p.max_colors = 257u; expect_invalid(p);
    p = params; p.alpha_mode = 2u; expect_invalid(p);
    p = params; p.color_space = 2u; expect_invalid(p);
    p = params; p.dither_mode = 2u; expect_invalid(p);
    p = params; p.pipeline = 4u; expect_invalid(p);
    p = params; p.kmeans_max_iter = 1001u; expect_invalid(p);
    p = params; p.kmeans_max_iter = 1000u;
    SQ_CHECK_EQ(sq_params_validate(&p), SQ_OK);
    p = params; p.kmeans_max_iter = 0u;
    SQ_CHECK_EQ(sq_params_validate(&p), SQ_OK);
    p = params; p.thread_count = 257u; expect_invalid(p);
    p = params; p.thread_count = 256u; SQ_CHECK_EQ(sq_params_validate(&p), SQ_OK);
    p = params; p.dither_strength = 1.5f; expect_invalid(p);
    p = params; p.dither_strength = -0.1f; expect_invalid(p);
    p = params; p.dither_strength = std::numeric_limits<float>::quiet_NaN(); expect_invalid(p);
    p = params; p.dither_strength = std::numeric_limits<float>::infinity(); expect_invalid(p);
    p = params; p.kmeans_tolerance = std::numeric_limits<float>::quiet_NaN(); expect_invalid(p);
    p = params; p.kmeans_tolerance = std::numeric_limits<float>::infinity(); expect_invalid(p);
    p = params; p.kmeans_tolerance = 0.0f; expect_invalid(p);
    p = params; p.kmeans_tolerance = 0.100001f; expect_invalid(p);
    p = params; p.kmeans_tolerance = 0.1f; SQ_CHECK_EQ(sq_params_validate(&p), SQ_OK);
    p = params; p.reserved0[1] = 1u; expect_invalid(p);

    SQ_CHECK_EQ(sq_abi_version(), SQ_ABI_VERSION);
    SQ_CHECK(std::strcmp(sq_status_message(SQ_ERR_INVALID_ARG), "invalid argument") == 0);
    SQ_CHECK(std::strcmp(sq_status_message(SQ_ERR_CANCELLED), "cancelled") == 0);
}

void test_u20_and_negative_cases() {
    sq_params params = defaults();
    {
        Fixture f(1u, 1u);
        SQ_CHECK_EQ(sq_quantize(nullptr, &params, nullptr, nullptr, &f.result, nullptr),
                    SQ_ERR_INVALID_ARG);
        SQ_CHECK_EQ(sq_quantize(&f.image, nullptr, nullptr, nullptr, &f.result, nullptr),
                    SQ_ERR_INVALID_ARG);
        SQ_CHECK_EQ(sq_quantize(&f.image, &params, nullptr, nullptr, nullptr, nullptr),
                    SQ_ERR_INVALID_ARG);
    }
    {
        Fixture f(2u, 2u);
        f.image.pixels = nullptr;
        SQ_CHECK_EQ(sq_quantize(&f.image, &params, nullptr, nullptr, &f.result, nullptr),
                    SQ_ERR_INVALID_ARG);
    }
    {
        Fixture f(2u, 2u);
        f.result.indices = nullptr;
        SQ_CHECK_EQ(sq_quantize(&f.image, &params, nullptr, nullptr, &f.result, nullptr),
                    SQ_ERR_INVALID_ARG);
    }
    {
        Fixture f(2u, 2u);
        f.image.stride = f.image.width * 4u - 1u;
        SQ_CHECK_EQ(sq_quantize(&f.image, &params, nullptr, nullptr, &f.result, nullptr),
                    SQ_ERR_INVALID_ARG);
    }
    {
        Fixture f(2u, 2u);
        f.result.indices_capacity = 3u;
        SQ_CHECK_EQ(sq_quantize(&f.image, &params, nullptr, nullptr, &f.result, nullptr),
                    SQ_ERR_BUFFER_TOO_SMALL);
        f.result.indices_capacity = 4u;
        SQ_CHECK_EQ(sq_quantize(&f.image, &params, nullptr, nullptr, &f.result, nullptr), SQ_OK);
    }
    {
        Fixture f(1u, 1u);
        f.image.struct_size = sizeof(sq_image_in) - sizeof(std::uint32_t);
        SQ_CHECK_EQ(sq_quantize(&f.image, &params, nullptr, nullptr, &f.result, nullptr),
                    SQ_ERR_UNSUPPORTED_VERSION);
        f.image.struct_size = sizeof(sq_image_in);
        f.image.version = SQ_ABI_VERSION + 1u;
        SQ_CHECK_EQ(sq_quantize(&f.image, &params, nullptr, nullptr, &f.result, nullptr),
                    SQ_ERR_UNSUPPORTED_VERSION);
    }
    {
        Fixture f(1u, 1u);
        f.result.struct_size = sizeof(sq_result) - sizeof(std::uint32_t);
        SQ_CHECK_EQ(sq_quantize(&f.image, &params, nullptr, nullptr, &f.result, nullptr),
                    SQ_ERR_UNSUPPORTED_VERSION);
    }
    {
        Fixture f(1u, 1u);
        f.stats.struct_size = sizeof(sq_stats) - sizeof(std::uint32_t);
        SQ_CHECK_EQ(sq_quantize(&f.image, &params, nullptr, nullptr, &f.result, &f.stats),
                    SQ_ERR_UNSUPPORTED_VERSION);
    }
    {
        Fixture f(1u, 1u);
        f.image.reserved0[0] = 1u;
        SQ_CHECK_EQ(sq_quantize(&f.image, &params, nullptr, nullptr, &f.result, nullptr),
                    SQ_ERR_INVALID_ARG);
    }
    {
        Fixture f(1u, 1u);
        f.image.width = SQ_MAX_DIMENSION + 1u;
        SQ_CHECK_EQ(sq_quantize(&f.image, &params, nullptr, nullptr, &f.result, nullptr),
                    SQ_ERR_INVALID_ARG);
        f.image.width = 1u;
        f.image.height = 0u;
        SQ_CHECK_EQ(sq_quantize(&f.image, &params, nullptr, nullptr, &f.result, nullptr),
                    SQ_ERR_INVALID_ARG);
    }
    {
        Fixture f(1u, 1u);
        f.image.width = SQ_MAX_DIMENSION;
        f.image.height = SQ_MAX_DIMENSION;
        SQ_CHECK_EQ(sq_quantize(&f.image, &params, nullptr, nullptr, &f.result, nullptr),
                    SQ_ERR_INVALID_ARG);
    }
    {
        Fixture f(2u, 2u);
        f.image.stride = (std::numeric_limits<std::size_t>::max)();
        SQ_CHECK_EQ(sq_quantize(&f.image, &params, nullptr, nullptr, &f.result, nullptr),
                    SQ_ERR_INVALID_ARG);
    }
    {
        Fixture f(1u, 1u, true);
        SQ_CHECK_EQ(sq_quantize(&f.image, &params, nullptr, nullptr, &f.result, &f.stats), SQ_OK);
        SQ_CHECK_EQ(f.result.palette_size, 1u);
        SQ_CHECK_EQ(f.result.transparent_index, 0u);
        SQ_CHECK(f.result.indices[0] < f.result.palette_size);
        SQ_CHECK_EQ(f.stats.palette_size, 1u);
        SQ_CHECK_EQ(f.stats.workspace_bytes, sq_workspace_bytes(&params, 1u, 1u));
    }
    {
        Fixture f(1u, 1u);
        SQ_CHECK_EQ(sq_quantize(&f.image, &params, nullptr, nullptr, &f.result, nullptr), SQ_OK);
        SQ_CHECK_EQ(f.result.transparent_index, SQ_NO_TRANSPARENT_INDEX);
    }
    {
        Fixture f(4u, 3u);
        CancelState state;
        state.cancel_at = 1u;
        SQ_CHECK_EQ(sq_quantize(&f.image, &params, cancel_callback, &state,
                               &f.result, nullptr), SQ_ERR_CANCELLED);
        SQ_CHECK_EQ(f.result.palette_size, 0u);
        SQ_CHECK_EQ(f.result.transparent_index, SQ_NO_TRANSPARENT_INDEX);
        state.finished = true;
        SQ_CHECK(!state.fired_after_return);
    }
    {
        Fixture f(4u, 3u);
        CancelState state;
        state.cancel_at = 101u;  // never cancels
        SQ_CHECK_EQ(sq_quantize(&f.image, &params, cancel_callback, &state,
                               &f.result, nullptr), SQ_OK);
        SQ_CHECK(state.calls > 0);
        state.finished = true;
        SQ_CHECK(!state.fired_after_return);
    }
    {
        Fixture f(1024u, 1u);
        const std::size_t expected = sq_workspace_bytes(&params, 1024u, 1u);
        SQ_CHECK(expected > 0u);
        SQ_CHECK_EQ(sq_workspace_bytes(nullptr, 1024u, 1u), 0u);
    }
}

void test_exact_path_and_transparency() {
    sq_params params = defaults();
    params.max_colors = 4u;
    params.thread_count = 2u;
    {
        Fixture f(2u, 2u);
        for (std::size_t i = 0; i < 4u; ++i) {
            const bool second = (i & 1u) != 0u;
            f.pixels[i * 4u + 0u] = second ? 200u : 10u;
            f.pixels[i * 4u + 1u] = second ? 100u : 20u;
            f.pixels[i * 4u + 2u] = second ? 50u : 30u;
            f.pixels[i * 4u + 3u] = 255u;
        }
        SQ_CHECK_EQ(sq_quantize(&f.image, &params, nullptr, nullptr, &f.result,
                                &f.stats), SQ_OK);
        SQ_CHECK_EQ(f.stats.path, static_cast<std::uint32_t>(SQ_PATH_EXACT));
        SQ_CHECK_EQ(f.stats.probe_state, 0u);
        SQ_CHECK_EQ(f.stats.unique_colors, 2u);
        SQ_CHECK_EQ(f.result.palette_size, 2u);
        SQ_CHECK_EQ(f.result.transparent_index, SQ_NO_TRANSPARENT_INDEX);
        SQ_CHECK(f.result.indices[0] == f.result.indices[2]);
        SQ_CHECK(f.result.indices[1] == f.result.indices[3]);
        SQ_CHECK(f.result.indices[0] != f.result.indices[1]);
        SQ_CHECK_EQ(f.stats.workspace_bytes,
                    sq_workspace_bytes(&params, 2u, 2u));
        for (std::size_t i = 0; i < 4u; ++i) {
            const std::uint8_t index = f.result.indices[i];
            SQ_CHECK(index < f.result.palette_size);
            const std::uint8_t* entry = f.result.palette_rgba + index * 4u;
            SQ_CHECK_EQ(entry[0], f.pixels[i * 4u + 0u]);
            SQ_CHECK_EQ(entry[1], f.pixels[i * 4u + 1u]);
            SQ_CHECK_EQ(entry[2], f.pixels[i * 4u + 2u]);
            SQ_CHECK_EQ(entry[3], f.pixels[i * 4u + 3u]);
        }
    }
    {
        Fixture f(2u, 1u);
        f.pixels[0] = 0u;
        f.pixels[1] = 0u;
        f.pixels[2] = 0u;
        f.pixels[3] = 0u;
        f.pixels[4] = 200u;
        f.pixels[5] = 100u;
        f.pixels[6] = 50u;
        f.pixels[7] = 255u;
        params.max_colors = 2u;
        SQ_CHECK_EQ(sq_quantize(&f.image, &params, nullptr, nullptr, &f.result,
                                &f.stats), SQ_OK);
        SQ_CHECK_EQ(f.stats.path, static_cast<std::uint32_t>(SQ_PATH_EXACT));
        SQ_CHECK_EQ(f.result.palette_size, 2u);
        SQ_CHECK_EQ(f.result.transparent_index, 0u);
        SQ_CHECK_EQ(f.result.indices[0], 0u);
        SQ_CHECK_EQ(f.result.indices[1], 1u);
        SQ_CHECK_EQ(f.result.palette_rgba[0], 0u);
        SQ_CHECK_EQ(f.result.palette_rgba[1], 0u);
        SQ_CHECK_EQ(f.result.palette_rgba[2], 0u);
        SQ_CHECK_EQ(f.result.palette_rgba[3], 0u);
        SQ_CHECK_EQ(f.result.palette_rgba[4], 200u);
        SQ_CHECK_EQ(f.result.palette_rgba[7], 255u);
    }
    {
        Fixture f(16u, 16u);
        for (std::size_t i = 0; i < 256u; ++i) {
            f.pixels[i * 4u + 0u] = static_cast<std::uint8_t>(i);
            f.pixels[i * 4u + 1u] = static_cast<std::uint8_t>(255u - i);
            f.pixels[i * 4u + 2u] =
                static_cast<std::uint8_t>((i * 3u) & 0xFFu);
            f.pixels[i * 4u + 3u] = 255u;
        }
        params.max_colors = 256u;
        SQ_CHECK_EQ(sq_quantize(&f.image, &params, nullptr, nullptr, &f.result,
                                &f.stats), SQ_OK);
        SQ_CHECK_EQ(f.stats.path, static_cast<std::uint32_t>(SQ_PATH_EXACT));
        SQ_CHECK_EQ(f.stats.unique_colors, 256u);
        SQ_CHECK_EQ(f.result.palette_size, 256u);
        for (std::size_t i = 0; i < 256u; ++i) {
            const std::uint8_t index = f.result.indices[i];
            const std::uint8_t* entry = f.result.palette_rgba + index * 4u;
            SQ_CHECK_EQ(entry[0], f.pixels[i * 4u + 0u]);
            SQ_CHECK_EQ(entry[1], f.pixels[i * 4u + 1u]);
            SQ_CHECK_EQ(entry[2], f.pixels[i * 4u + 2u]);
            SQ_CHECK_EQ(entry[3], 255u);
        }
    }
    {
        // Black and white are the two extreme packed keys. White opaque packs
        // to 0xFFFFFFFF and once collided with the k-way merge sentinel, which
        // silently dropped it from the exact palette.
        Fixture f(4u, 1u);
        const std::uint8_t colors[2][3] = {{0u, 0u, 0u}, {255u, 255u, 255u}};
        for (std::size_t i = 0; i < 4u; ++i) {
            const std::uint8_t* color = colors[i & 1u];
            f.pixels[i * 4u + 0u] = color[0];
            f.pixels[i * 4u + 1u] = color[1];
            f.pixels[i * 4u + 2u] = color[2];
            f.pixels[i * 4u + 3u] = 255u;
        }
        params.max_colors = 2u;
        SQ_CHECK_EQ(sq_quantize(&f.image, &params, nullptr, nullptr, &f.result,
                                &f.stats), SQ_OK);
        SQ_CHECK_EQ(f.stats.path, static_cast<std::uint32_t>(SQ_PATH_EXACT));
        SQ_CHECK_EQ(f.stats.unique_colors, 2u);
        SQ_CHECK_EQ(f.result.palette_size, 2u);
        for (std::size_t i = 0; i < 4u; ++i) {
            const std::uint8_t index = f.result.indices[i];
            SQ_CHECK(index < f.result.palette_size);
            const std::uint8_t* entry = f.result.palette_rgba + index * 4u;
            SQ_CHECK_EQ(entry[0], f.pixels[i * 4u + 0u]);
            SQ_CHECK_EQ(entry[1], f.pixels[i * 4u + 1u]);
            SQ_CHECK_EQ(entry[2], f.pixels[i * 4u + 2u]);
            SQ_CHECK_EQ(entry[3], 255u);
        }
    }
}

void test_degenerate_path_and_determinism() {
    sq_params params = defaults();
    params.max_colors = 256u;
    params.thread_count = 1u;

    Fixture f(20u, 15u);
    for (std::size_t i = 0; i < 300u; ++i) {
        // 300 distinct colours inside one 5-bit RGB cell (0..7 per channel).
        const std::uint8_t r = static_cast<std::uint8_t>(i % 8u);
        const std::uint8_t g = static_cast<std::uint8_t>((i / 8u) % 8u);
        const std::uint8_t b = static_cast<std::uint8_t>(i / 64u);
        f.pixels[i * 4u + 0u] = r;
        f.pixels[i * 4u + 1u] = g;
        f.pixels[i * 4u + 2u] = b;
        f.pixels[i * 4u + 3u] = 255u;
    }
    SQ_CHECK_EQ(sq_quantize(&f.image, &params, nullptr, nullptr, &f.result,
                            &f.stats), SQ_OK);
    SQ_CHECK_EQ(f.stats.path,
                static_cast<std::uint32_t>(SQ_PATH_DEGENERATE));
    SQ_CHECK_EQ(f.stats.probe_state, 0u);
    SQ_CHECK_EQ(f.stats.unique_colors, 300u);
    SQ_CHECK_EQ(f.stats.occupied_cells, 1u);
    SQ_CHECK(f.result.palette_size > 1u);
    SQ_CHECK(f.result.palette_size <= 256u);
    SQ_CHECK_EQ(f.stats.workspace_bytes, sq_workspace_bytes(&params, 20u, 15u));
    for (std::size_t i = 0; i < 300u; ++i) {
        SQ_CHECK(f.result.indices[i] < f.result.palette_size);
    }

    Fixture g(20u, 15u);
    g.pixels = f.pixels;
    g.image.pixels = g.pixels.data();
    params.thread_count = 4u;
    SQ_CHECK_EQ(sq_quantize(&g.image, &params, nullptr, nullptr, &g.result,
                            &g.stats), SQ_OK);
    SQ_CHECK_EQ(f.result.palette_size, g.result.palette_size);
    SQ_CHECK(std::memcmp(f.result.palette_rgba, g.result.palette_rgba,
                         sizeof(f.result.palette_rgba)) == 0);
    SQ_CHECK(std::memcmp(f.result.indices, g.result.indices, 300u) == 0);
    SQ_CHECK_EQ(g.stats.workspace_bytes,
                sq_workspace_bytes(&params, 20u, 15u));
}

void test_second_pass_escalation() {
    sq_params params = defaults();
    params.max_colors = 256u;
    params.thread_count = 4u;

    Fixture f(100u, 50u);
    for (std::size_t i = 0; i < 5000u; ++i) {
        f.pixels[i * 4u + 0u] = static_cast<std::uint8_t>(i & 7u);
        f.pixels[i * 4u + 1u] = static_cast<std::uint8_t>((i >> 3) & 7u);
        f.pixels[i * 4u + 2u] = static_cast<std::uint8_t>((i >> 6) & 7u);
        f.pixels[i * 4u + 3u] = static_cast<std::uint8_t>(1u + (i >> 9));
    }
    SQ_CHECK_EQ(sq_quantize(&f.image, &params, nullptr, nullptr, &f.result,
                            &f.stats), SQ_OK);
    SQ_CHECK_EQ(f.stats.path,
                static_cast<std::uint32_t>(SQ_PATH_DEGENERATE));
    SQ_CHECK_EQ(f.stats.probe_state, 0u);
    SQ_CHECK_EQ(f.stats.unique_colors, 5000u);
    SQ_CHECK_EQ(f.stats.occupied_cells, 1u);
    SQ_CHECK(f.result.palette_size > 1u);
    SQ_CHECK(f.result.palette_size <= 256u);
    SQ_CHECK_EQ(f.stats.workspace_bytes,
                sq_workspace_bytes(&params, 100u, 50u) + 229376u);
    for (std::size_t i = 0; i < 5000u; ++i) {
        SQ_CHECK(f.result.indices[i] < f.result.palette_size);
    }
}

void test_wu_budget_and_transparency() {
    {
        sq_params params = defaults();
        params.max_colors = 2u;
        Fixture f(16u, 16u);
        for (std::size_t i = 0; i < 256u; ++i) {
            f.pixels[i * 4u + 0u] = static_cast<std::uint8_t>(i);
            f.pixels[i * 4u + 1u] = static_cast<std::uint8_t>(255u - i);
            f.pixels[i * 4u + 2u] =
                static_cast<std::uint8_t>((i * 3u) & 0xFFu);
            f.pixels[i * 4u + 3u] = 255u;
        }
        SQ_CHECK_EQ(sq_quantize(&f.image, &params, nullptr, nullptr, &f.result,
                                &f.stats), SQ_OK);
        SQ_CHECK_EQ(f.stats.path, static_cast<std::uint32_t>(SQ_PATH_GRID));
        SQ_CHECK_EQ(f.result.palette_size, 2u);
        SQ_CHECK(std::memcmp(f.result.palette_rgba, f.result.palette_rgba + 4u,
                             4u) != 0);
        for (std::size_t i = 0; i < 256u; ++i) {
            SQ_CHECK(f.result.indices[i] < f.result.palette_size);
        }
    }
    {
        // 256 opaque colours in 255 occupied cells, plus one transparent
        // pixel: exact is impossible (257 > 256), and occupied == k, so the
        // grid path must spend the full opaque budget.
        sq_params params = defaults();
        params.max_colors = 256u;
        Fixture f(257u, 1u);
        for (std::size_t i = 0; i < 255u; ++i) {
            f.pixels[i * 4u + 0u] =
                static_cast<std::uint8_t>((i % 32u) * 8u);
            f.pixels[i * 4u + 1u] =
                static_cast<std::uint8_t>((i / 32u) * 8u);
            f.pixels[i * 4u + 2u] = 0u;
            f.pixels[i * 4u + 3u] = 255u;
        }
        f.pixels[255u * 4u + 0u] = 1u;
        f.pixels[255u * 4u + 1u] = 0u;
        f.pixels[255u * 4u + 2u] = 0u;
        f.pixels[255u * 4u + 3u] = 255u;
        f.pixels[256u * 4u + 0u] = 0u;
        f.pixels[256u * 4u + 1u] = 0u;
        f.pixels[256u * 4u + 2u] = 0u;
        f.pixels[256u * 4u + 3u] = 0u;
        SQ_CHECK_EQ(sq_quantize(&f.image, &params, nullptr, nullptr, &f.result,
                                &f.stats), SQ_OK);
        SQ_CHECK_EQ(f.stats.path, static_cast<std::uint32_t>(SQ_PATH_GRID));
        SQ_CHECK_EQ(f.stats.occupied_cells, 255u);
        SQ_CHECK_EQ(f.result.transparent_index, 0u);
        SQ_CHECK_EQ(f.result.palette_size, 256u);
        SQ_CHECK_EQ(f.stats.palette_size, 256u);
        for (std::size_t i = 0; i < 257u; ++i) {
            SQ_CHECK(f.result.indices[i] < f.result.palette_size);
        }
    }
}

void test_premultiplied_and_grid_fallback() {
    sq_params params = defaults();
    params.max_colors = 4u;
    params.alpha_mode = SQ_ALPHA_PREMULTIPLIED;
    {
        Fixture f(1u, 1u);
        f.pixels[0] = 100u;
        f.pixels[1] = 50u;
        f.pixels[2] = 25u;
        f.pixels[3] = 128u;
        SQ_CHECK_EQ(sq_quantize(&f.image, &params, nullptr, nullptr, &f.result,
                                &f.stats), SQ_OK);
        SQ_CHECK_EQ(f.stats.path, static_cast<std::uint32_t>(SQ_PATH_EXACT));
        SQ_CHECK_EQ(f.result.palette_size, 1u);
        SQ_CHECK_EQ(f.result.palette_rgba[0], 199u);
        SQ_CHECK_EQ(f.result.palette_rgba[1], 100u);
        SQ_CHECK_EQ(f.result.palette_rgba[2], 50u);
        SQ_CHECK_EQ(f.result.palette_rgba[3], 128u);
    }

    params = defaults();
    params.max_colors = 64u;
    Fixture f(32u, 32u);
    for (std::size_t i = 0; i < 1024u; ++i) {
        f.pixels[i * 4u + 0u] = static_cast<std::uint8_t>((i * 37u) & 0xFFu);
        f.pixels[i * 4u + 1u] = static_cast<std::uint8_t>((i * 73u) & 0xFFu);
        f.pixels[i * 4u + 2u] = static_cast<std::uint8_t>((i * 109u) & 0xFFu);
        f.pixels[i * 4u + 3u] = 255u;
    }
    SQ_CHECK_EQ(sq_quantize(&f.image, &params, nullptr, nullptr, &f.result,
                            &f.stats), SQ_OK);
    SQ_CHECK_EQ(f.stats.path, static_cast<std::uint32_t>(SQ_PATH_GRID));
    SQ_CHECK(f.stats.unique_colors > 64u);
    SQ_CHECK(f.result.palette_size > 1u);
    SQ_CHECK(f.result.palette_size <= 64u);
    SQ_CHECK_EQ(f.stats.workspace_bytes, sq_workspace_bytes(&params, 32u, 32u));
    for (std::size_t i = 0; i < 1024u; ++i) {
        SQ_CHECK(f.result.indices[i] < f.result.palette_size);
    }
}

void test_cancel_strips() {
    sq_params params = defaults();
    params.thread_count = 4u;
    {
        Fixture f(1u, 32768u);
        CancelState state;
        state.cancel_at = 10u;
        SQ_CHECK_EQ(sq_quantize(&f.image, &params, cancel_callback, &state,
                                &f.result, nullptr), SQ_ERR_CANCELLED);
        SQ_CHECK_EQ(f.result.palette_size, 0u);
        SQ_CHECK_EQ(f.result.transparent_index, SQ_NO_TRANSPARENT_INDEX);
        state.finished = true;
        SQ_CHECK(!state.fired_after_return);
        SQ_CHECK(state.calls > 0);
    }
    {
        Fixture f(32768u, 1u);
        CancelState state;
        state.cancel_at = 10u;
        SQ_CHECK_EQ(sq_quantize(&f.image, &params, cancel_callback, &state,
                                &f.result, nullptr), SQ_ERR_CANCELLED);
        SQ_CHECK_EQ(f.result.palette_size, 0u);
        SQ_CHECK_EQ(f.result.transparent_index, SQ_NO_TRANSPARENT_INDEX);
        state.finished = true;
        SQ_CHECK(!state.fired_after_return);
        SQ_CHECK(state.calls > 0);
    }
    {
        // Cancelling at 50 percent lands on the 65 percent checkpoint, after
        // the palette and before the mapping pass. The engine must return
        // without spawning the mapping workers, so no index is written and the
        // cancellation latency stays at the checkpoint cost.
        Fixture f(64u, 64u);
        for (std::size_t i = 0; i < f.indices.size(); ++i) {
            f.pixels[i * 4u + 0u] = static_cast<std::uint8_t>((i & 31u) * 8u);
            f.pixels[i * 4u + 1u] =
                static_cast<std::uint8_t>(((i >> 5) & 31u) * 8u);
            f.pixels[i * 4u + 2u] =
                static_cast<std::uint8_t>(((i >> 10) & 3u) * 64u);
            f.pixels[i * 4u + 3u] = 255u;
        }
        Fixture reference(64u, 64u);
        reference.pixels = f.pixels;
        SQ_CHECK_EQ(sq_quantize(&reference.image, &params, nullptr, nullptr,
                                &reference.result, &reference.stats), SQ_OK);
        SQ_CHECK_EQ(reference.stats.path,
                    static_cast<std::uint32_t>(SQ_PATH_GRID));
        CancelState state;
        state.cancel_at = 50u;
        SQ_CHECK_EQ(sq_quantize(&f.image, &params, cancel_callback, &state,
                                &f.result, nullptr), SQ_ERR_CANCELLED);
        SQ_CHECK_EQ(f.result.palette_size, 0u);
        bool untouched = true;
        for (std::uint8_t index : f.indices) {
            untouched = untouched && index == 0xFFu;
        }
        SQ_CHECK(untouched);
        state.finished = true;
        SQ_CHECK(!state.fired_after_return);
    }
    {
        // Cancelling at 40 percent triggers cancellation during the Wu box splitting
        // phase (spec 4.6: checked every 256 candidate cuts).
        Fixture f(64u, 64u);
        for (std::size_t i = 0; i < f.indices.size(); ++i) {
            f.pixels[i * 4u + 0u] = static_cast<std::uint8_t>((i & 31u) * 8u);
            f.pixels[i * 4u + 1u] =
                static_cast<std::uint8_t>(((i >> 5) & 31u) * 8u);
            f.pixels[i * 4u + 2u] =
                static_cast<std::uint8_t>(((i >> 10) & 3u) * 64u);
            f.pixels[i * 4u + 3u] = 255u;
        }
        CancelState state;
        state.cancel_at = 40u;
        SQ_CHECK_EQ(sq_quantize(&f.image, &params, cancel_callback, &state,
                                &f.result, nullptr), SQ_ERR_CANCELLED);
        SQ_CHECK_EQ(f.result.palette_size, 0u);
        bool untouched = true;
        for (std::uint8_t index : f.indices) {
            untouched = untouched && index == 0xFFu;
        }
        SQ_CHECK(untouched);
        state.finished = true;
        SQ_CHECK(!state.fired_after_return);
    }
}

void test_progress_is_monotonic() {
    sq_params params = defaults();
    Fixture f(8u, 8u);
    struct ProgressState {
        std::uint32_t previous = 0u;
        bool monotonic = true;
        bool saw_hundred = false;
    } state;
    auto callback = [](std::uint32_t percent, void* user) -> int {
        auto* s = static_cast<ProgressState*>(user);
        if (percent < s->previous) {
            s->monotonic = false;
        }
        s->previous = percent;
        if (percent == 100u) {
            s->saw_hundred = true;
        }
        return 0;
    };
    SQ_CHECK_EQ(sq_quantize(&f.image, &params, callback, &state, &f.result, nullptr), SQ_OK);
    SQ_CHECK(state.monotonic);
    SQ_CHECK(state.saw_hundred);
    SQ_CHECK_EQ(state.previous, 100u);
}

}  // namespace

std::uint64_t prefix_at(const std::uint64_t* layer, unsigned x, unsigned y,
                        unsigned z) {
    return layer[static_cast<std::size_t>(x) * 1089u
                 + static_cast<std::size_t>(y) * 33u + z];
}

std::uint64_t box_total(const std::uint64_t* cumulative, std::uint32_t channel,
                        const sq::WuBox& box) {
    const std::uint64_t* layer =
        cumulative + static_cast<std::size_t>(channel) * sq::kCumulativeCount;
    const std::int64_t sum =
        static_cast<std::int64_t>(prefix_at(layer, box.r1, box.g1, box.b1))
      - static_cast<std::int64_t>(prefix_at(layer, box.r0, box.g1, box.b1))
      - static_cast<std::int64_t>(prefix_at(layer, box.r1, box.g0, box.b1))
      - static_cast<std::int64_t>(prefix_at(layer, box.r1, box.g1, box.b0))
      + static_cast<std::int64_t>(prefix_at(layer, box.r0, box.g0, box.b1))
      + static_cast<std::int64_t>(prefix_at(layer, box.r0, box.g1, box.b0))
      + static_cast<std::int64_t>(prefix_at(layer, box.r1, box.g0, box.b0))
      - static_cast<std::int64_t>(prefix_at(layer, box.r0, box.g0, box.b0));
    return static_cast<std::uint64_t>(sum);
}

bool box_contains(const sq::WuBox& box, unsigned r, unsigned g, unsigned b) {
    return r >= box.r0 && r < box.r1 && g >= box.g0 && g < box.g1
        && b >= box.b0 && b < box.b1;
}

// One scan feeds three invariant groups: the cumulative table against brute
// force, the box partition over the occupied cells, and one extra box per cut.
void test_u4_u6_u7_histogram_and_wu() {
    sq_params params = defaults();
    params.max_colors = 8u;
    params.thread_count = 2u;
    Fixture f(20u, 15u);
    for (std::size_t i = 0; i < 300u; ++i) {
        // Spread the colours over many 5-bit cells, otherwise the box set is a
        // single cell and cannot be split at all.
        f.pixels[i * 4u + 0u] = static_cast<std::uint8_t>((i * 37u) & 0xFFu);
        f.pixels[i * 4u + 1u] = static_cast<std::uint8_t>((i * 53u) & 0xFFu);
        f.pixels[i * 4u + 2u] = static_cast<std::uint8_t>((i * 97u) & 0xFFu);
        f.pixels[i * 4u + 3u] = 255u;
    }

    sq::Workspace workspace;
    SQ_CHECK(workspace.allocate(params, 20u, 15u));
    sq::Progress progress;
    sq::ScanResult scan;
    SQ_CHECK(sq::scan_image(f.image, params, workspace, progress, scan));
    const std::uint64_t* histogram = workspace.histogram(0u);
    const std::uint64_t* cumulative = workspace.cumulative();

    // Random boxes compare the prefix difference with a brute-force sum.
    std::uint32_t state = 0x1234567u;
    for (unsigned trial = 0; trial < 32u; ++trial) {
        state = state * 1664525u + 1013904223u;
        const unsigned r0 = state % 30u;
        state = state * 1664525u + 1013904223u;
        const unsigned r1 = r0 + 1u + (state % (32u - r0));
        state = state * 1664525u + 1013904223u;
        const unsigned g0 = state % 30u;
        state = state * 1664525u + 1013904223u;
        const unsigned g1 = g0 + 1u + (state % (32u - g0));
        state = state * 1664525u + 1013904223u;
        const unsigned b0 = state % 30u;
        state = state * 1664525u + 1013904223u;
        const unsigned b1 = b0 + 1u + (state % (32u - b0));
        const sq::WuBox box{static_cast<std::uint16_t>(r0),
                            static_cast<std::uint16_t>(r1),
                            static_cast<std::uint16_t>(g0),
                            static_cast<std::uint16_t>(g1),
                            static_cast<std::uint16_t>(b0),
                            static_cast<std::uint16_t>(b1)};
        std::uint64_t brute_n = 0u;
        std::uint64_t brute_w = 0u;
        for (unsigned r = r0; r < r1; ++r) {
            for (unsigned g = g0; g < g1; ++g) {
                for (unsigned b = b0; b < b1; ++b) {
                    const std::size_t cell = static_cast<std::size_t>(r) * 1024u
                                           + static_cast<std::size_t>(g) * 32u + b;
                    brute_n += histogram[sq::kHistN * sq::kCellCount + cell];
                    brute_w += histogram[sq::kHistW * sq::kCellCount + cell];
                }
            }
        }
        SQ_CHECK_EQ(box_total(cumulative, sq::kHistN, box), brute_n);
        SQ_CHECK_EQ(box_total(cumulative, sq::kHistW, box), brute_w);
    }

    // Every occupied cell sits in exactly one box and the weights add up.
    std::uint64_t total_w = 0u;
    for (std::size_t cell = 0u; cell < sq::kCellCount; ++cell) {
        total_w += histogram[sq::kHistW * sq::kCellCount + cell];
    }
    std::vector<sq::WuBox> boxes(8u);
    const std::uint32_t box_count = sq::run_wu_boxes(
        histogram, cumulative, 8u, boxes.data());
    SQ_CHECK_EQ(box_count, 8u);
    std::uint64_t partition_w = 0u;
    for (std::uint32_t i = 0u; i < box_count; ++i) {
        SQ_CHECK_EQ(box_total(cumulative, sq::kHistW, boxes[i]) > 0u, true);
        partition_w += box_total(cumulative, sq::kHistW, boxes[i]);
    }
    SQ_CHECK_EQ(partition_w, total_w);
    std::uint32_t multi_box_cells = 0u;
    for (std::size_t cell = 0u; cell < sq::kCellCount; ++cell) {
        if (histogram[sq::kHistN * sq::kCellCount + cell] == 0u) {
            continue;
        }
        const unsigned r = static_cast<unsigned>(cell / 1024u);
        const unsigned g = static_cast<unsigned>((cell / 32u) % 32u);
        const unsigned b = static_cast<unsigned>(cell % 32u);
        std::uint32_t hits = 0u;
        for (std::uint32_t i = 0u; i < box_count; ++i) {
            if (box_contains(boxes[i], r, g, b)) {
                ++hits;
            }
        }
        if (hits != 1u) {
            ++multi_box_cells;
        }
    }
    SQ_CHECK_EQ(multi_box_cells, 0u);

    // Each cut adds exactly one box, and both halves carry weight.
    for (std::uint32_t k = 1u; k <= 8u; ++k) {
        std::vector<sq::WuBox> grown(8u);
        const std::uint32_t count =
            sq::run_wu_boxes(histogram, cumulative, k, grown.data());
        SQ_CHECK_EQ(count, k);
        for (std::uint32_t i = 0u; i < count; ++i) {
            SQ_CHECK(box_total(cumulative, sq::kHistW, grown[i]) > 0u);
        }
    }
}

// The nearest entry wins and equal distances resolve to the lowest index.
void test_u10_nearest_tie_break() {
    sq::Palette palette{};
    palette.size = 3u;
    palette.transparent_index = SQ_NO_TRANSPARENT_INDEX;
    const std::uint8_t r[3] = {0u, 10u, 20u};
    for (std::uint32_t i = 0u; i < 3u; ++i) {
        palette.r[i] = r[i];
        palette.g[i] = 0u;
        palette.b[i] = 0u;
    }
    SQ_CHECK_EQ(sq::nearest_index(palette, 5u, 0u, 0u), 0u);
    SQ_CHECK_EQ(sq::nearest_index(palette, 15u, 0u, 0u), 1u);
    SQ_CHECK_EQ(sq::nearest_index(palette, 19u, 0u, 0u), 2u);
    SQ_CHECK_EQ(sq::nearest_index(palette, 255u, 0u, 0u), 2u);
    palette.transparent_index = 0u;
    SQ_CHECK_EQ(sq::nearest_index(palette, 0u, 0u, 0u), 1u);
}

// The palette never exceeds the budget and never repeats an entry.
void test_u8_palette_budget_and_dedup() {
    const std::uint32_t budgets[] = {2u, 4u, 16u, 256u};
    for (std::uint32_t budget : budgets) {
        sq_params params = defaults();
        params.max_colors = budget;
        Fixture f(32u, 32u);
        for (std::size_t i = 0; i < f.indices.size(); ++i) {
            const std::uint32_t value =
                static_cast<std::uint32_t>(i) * 2654435761u;
            f.pixels[i * 4u + 0u] = static_cast<std::uint8_t>(value);
            f.pixels[i * 4u + 1u] = static_cast<std::uint8_t>(value >> 8);
            f.pixels[i * 4u + 2u] = static_cast<std::uint8_t>(value >> 16);
            f.pixels[i * 4u + 3u] = 255u;
        }
        SQ_CHECK_EQ(sq_quantize(&f.image, &params, nullptr, nullptr, &f.result,
                                &f.stats), SQ_OK);
        SQ_CHECK(f.result.palette_size <= budget);
        for (std::uint32_t a = 0u; a < f.result.palette_size; ++a) {
            for (std::uint32_t b = a + 1u; b < f.result.palette_size; ++b) {
                SQ_CHECK(std::memcmp(f.result.palette_rgba + a * 4u,
                                     f.result.palette_rgba + b * 4u, 4u) != 0);
            }
        }
    }
}

// Low-bit colour differences survive the exact path, and a dense
// single-cell set still spreads over the palette.
void test_u17_u18_collision_paths() {
    {
        sq_params params = defaults();
        params.max_colors = 8u;
        Fixture f(8u, 1u);
        for (std::size_t i = 0; i < 8u; ++i) {
            f.pixels[i * 4u + 0u] = static_cast<std::uint8_t>(i);
            f.pixels[i * 4u + 1u] = 0u;
            f.pixels[i * 4u + 2u] = 0u;
            f.pixels[i * 4u + 3u] = 255u;
        }
        SQ_CHECK_EQ(sq_quantize(&f.image, &params, nullptr, nullptr, &f.result,
                                &f.stats), SQ_OK);
        SQ_CHECK_EQ(f.stats.path, static_cast<std::uint32_t>(SQ_PATH_EXACT));
        SQ_CHECK_EQ(f.result.palette_size, 8u);
        for (std::size_t i = 0; i < 8u; ++i) {
            const std::uint8_t index = f.result.indices[i];
            SQ_CHECK_EQ(f.result.palette_rgba[index * 4u + 0u],
                        f.pixels[i * 4u + 0u]);
        }
    }
    {
        sq_params params = defaults();
        params.max_colors = 256u;
        // 1000 distinct RGBA colours built from 512 RGB triples inside two
        // 5-bit cells. The 3D model can only separate the RGB part, so the
        // palette is bounded by the budget rather than by the pixel count; an
        // empty cluster keeps its starting alpha, so the entry stays usable.
        Fixture f(40u, 25u);
        for (std::size_t i = 0; i < 1000u; ++i) {
            f.pixels[i * 4u + 0u] = static_cast<std::uint8_t>(i & 7u);
            f.pixels[i * 4u + 1u] = static_cast<std::uint8_t>((i >> 3) & 7u);
            f.pixels[i * 4u + 2u] = static_cast<std::uint8_t>((i >> 6) & 15u);
            f.pixels[i * 4u + 3u] = 255u;
        }
        SQ_CHECK_EQ(sq_quantize(&f.image, &params, nullptr, nullptr, &f.result,
                                &f.stats), SQ_OK);
        SQ_CHECK_EQ(f.stats.path, static_cast<std::uint32_t>(SQ_PATH_DEGENERATE));
        SQ_CHECK_EQ(f.stats.occupied_cells, 2u);
        SQ_CHECK_EQ(f.result.palette_size, 256u);
        for (std::uint32_t a = 0u; a < f.result.palette_size; ++a) {
            for (std::uint32_t b = a + 1u; b < f.result.palette_size; ++b) {
                SQ_CHECK(std::memcmp(f.result.palette_rgba + a * 4u,
                                     f.result.palette_rgba + b * 4u, 4u) != 0);
            }
        }
    }
}

// The probe table and the path decision do not depend on thread count,
// and the counts add up to the number of pixels with alpha above zero.
void test_u21_probe_determinism() {
    Fixture f(64u, 64u);
    std::uint64_t opaque = 0u;
    for (std::size_t i = 0; i < f.indices.size(); ++i) {
        const std::uint32_t value = static_cast<std::uint32_t>(i) * 2246822519u;
        f.pixels[i * 4u + 0u] = static_cast<std::uint8_t>(value);
        f.pixels[i * 4u + 1u] = static_cast<std::uint8_t>(value >> 8);
        f.pixels[i * 4u + 2u] = static_cast<std::uint8_t>(value >> 16);
        const std::uint8_t alpha =
            (i % 16u) == 0u ? 0u : static_cast<std::uint8_t>(1u + (i % 255u));
        f.pixels[i * 4u + 3u] = alpha;
        if (alpha != 0u) {
            ++opaque;
        }
    }

    std::vector<sq::Entry> reference;
    std::uint32_t reference_state = 0u;
    std::uint32_t reference_cells = 0u;
    const std::uint32_t thread_counts[4] = {1u, 2u, 4u, 8u};
    for (std::uint32_t threads : thread_counts) {
        sq_params params = defaults();
        params.thread_count = threads;
        sq::Workspace workspace;
        SQ_CHECK(workspace.allocate(params, 64u, 64u));
        sq::Progress progress;
        sq::ScanResult scan;
        SQ_CHECK(sq::scan_image(f.image, params, workspace, progress, scan));
        std::uint64_t counted = 0u;
        std::vector<sq::Entry> entries(scan.exact, scan.exact + scan.exact_used);
        for (const sq::Entry& entry : entries) {
            counted += entry.count;
        }
        SQ_CHECK_EQ(counted, opaque);
        if (reference.empty()) {
            reference = entries;
            reference_state = scan.probe_state;
            reference_cells = scan.occupied_cells;
        } else {
            SQ_CHECK_EQ(scan.probe_state, reference_state);
            SQ_CHECK_EQ(scan.occupied_cells, reference_cells);
            SQ_CHECK_EQ(entries.size(), reference.size());
            const std::size_t shared = std::min(entries.size(), reference.size());
            for (std::size_t i = 0; i < shared; ++i) {
                SQ_CHECK_EQ(entries[i].key, reference[i].key);
                SQ_CHECK_EQ(entries[i].count, reference[i].count);
            }
        }
    }
}

double mean_de00_of(const Fixture& f) {
    double sum = 0.0;
    std::size_t scored = 0u;
    for (std::size_t i = 0; i < f.indices.size(); ++i) {
        if (f.pixels[i * 4u + 3u] == 0u) {
            continue;
        }
        const std::uint8_t* entry = f.result.palette_rgba + f.result.indices[i] * 4u;
        sum += sq_color::delta_e_2000(
            sq_color::srgb8_to_lab(f.pixels[i * 4u + 0u], f.pixels[i * 4u + 1u],
                             f.pixels[i * 4u + 2u]),
            sq_color::srgb8_to_lab(entry[0], entry[1], entry[2]));
        ++scored;
    }
    return scored == 0u ? 0.0 : sum / static_cast<double>(scored);
}

// Property tests: random images and params, exact-path idempotence, path
// stability and max_colors monotonicity, all driven by a fixed LCG so a failure
// reproduces.
void test_8_2_property_tests() {
    std::uint32_t state = 0x2f6e2b1u;
    const auto next = [&state]() {
        state = state * 1664525u + 1013904223u;
        return state;
    };
    for (unsigned trial = 0; trial < 24u; ++trial) {
        const std::uint32_t width = 1u + next() % 40u;
        const std::uint32_t height = 1u + next() % 40u;
        Fixture f(width, height);
        const std::uint32_t palette_seed = next();
        for (std::size_t i = 0; i < f.indices.size(); ++i) {
            const std::uint32_t value = palette_seed ^ static_cast<std::uint32_t>(i) * 2654435761u;
            f.pixels[i * 4u + 0u] = static_cast<std::uint8_t>(value);
            f.pixels[i * 4u + 1u] = static_cast<std::uint8_t>(value >> 8);
            f.pixels[i * 4u + 2u] = static_cast<std::uint8_t>(value >> 16);
            f.pixels[i * 4u + 3u] = static_cast<std::uint8_t>(
                (value >> 24) % 4u == 0u ? 0u : 1u + ((value >> 24) % 255u));
        }
        sq_params params = defaults();
        params.max_colors = 2u + next() % 255u;
        params.thread_count = 1u + next() % 8u;
        SQ_CHECK_EQ(sq_quantize(&f.image, &params, nullptr, nullptr, &f.result,
                                &f.stats), SQ_OK);
        SQ_CHECK(f.result.palette_size <= params.max_colors);
        const bool exact = f.stats.path == static_cast<std::uint32_t>(SQ_PATH_EXACT);
        for (std::size_t i = 0; i < f.indices.size(); ++i) {
            const std::uint8_t index = f.result.indices[i];
            SQ_CHECK(index < f.result.palette_size);
            const std::uint8_t* entry = f.result.palette_rgba + index * 4u;
            if (f.pixels[i * 4u + 3u] == 0u) {
                SQ_CHECK_EQ(index, f.result.transparent_index);
            } else if (exact) {
                // The exact path must reproduce every opaque pixel.
                SQ_CHECK_EQ(entry[0], f.pixels[i * 4u + 0u]);
                SQ_CHECK_EQ(entry[1], f.pixels[i * 4u + 1u]);
                SQ_CHECK_EQ(entry[2], f.pixels[i * 4u + 2u]);
                SQ_CHECK_EQ(entry[3], f.pixels[i * 4u + 3u]);
            } else {
                // Straight alpha: no opaque entry may carry alpha zero.
                SQ_CHECK(entry[3] != 0u);
            }
        }
    }

    // Exact path idempotence: re-quantizing the engine's own indexed output
    // reproduces every opaque pixel and leaves a == 0 canonical.
    {
        sq_params params = defaults();
        params.max_colors = 256u;
        Fixture f(16u, 16u);
        for (std::size_t i = 0; i < 200u; ++i) {
            f.pixels[i * 4u + 0u] = static_cast<std::uint8_t>(i);
            f.pixels[i * 4u + 1u] = static_cast<std::uint8_t>((i * 7u) & 0xFFu);
            f.pixels[i * 4u + 2u] = static_cast<std::uint8_t>((i * 13u) & 0xFFu);
            f.pixels[i * 4u + 3u] = 255u;
        }
        SQ_CHECK_EQ(sq_quantize(&f.image, &params, nullptr, nullptr, &f.result,
                                &f.stats), SQ_OK);
        SQ_CHECK_EQ(f.stats.path, static_cast<std::uint32_t>(SQ_PATH_EXACT));
        Fixture g(16u, 16u);
        for (std::size_t i = 0; i < f.indices.size(); ++i) {
            const std::uint8_t* entry = f.result.palette_rgba + f.result.indices[i] * 4u;
            g.pixels[i * 4u + 0u] = entry[0];
            g.pixels[i * 4u + 1u] = entry[1];
            g.pixels[i * 4u + 2u] = entry[2];
            g.pixels[i * 4u + 3u] = entry[3];
        }
        SQ_CHECK_EQ(sq_quantize(&g.image, &params, nullptr, nullptr, &g.result,
                                &g.stats), SQ_OK);
        SQ_CHECK_EQ(g.stats.path, static_cast<std::uint32_t>(SQ_PATH_EXACT));
        SQ_CHECK_EQ(g.result.palette_size, f.result.palette_size);
        SQ_CHECK(std::memcmp(g.result.palette_rgba, f.result.palette_rgba,
                             static_cast<std::size_t>(f.result.palette_size) * 4u) == 0);
        SQ_CHECK(std::memcmp(g.result.indices, f.result.indices, 256u) == 0);
    }

    // Path stability across thread counts.
    {
        Fixture f(48u, 32u);
        for (std::size_t i = 0; i < f.indices.size(); ++i) {
            const std::uint32_t value = static_cast<std::uint32_t>(i) * 2246822519u;
            f.pixels[i * 4u + 0u] = static_cast<std::uint8_t>(value);
            f.pixels[i * 4u + 1u] = static_cast<std::uint8_t>(value >> 8);
            f.pixels[i * 4u + 2u] = static_cast<std::uint8_t>(value >> 16);
            f.pixels[i * 4u + 3u] = 255u;
        }
        std::uint32_t reference_path = 0u;
        for (std::uint32_t threads = 1u; threads <= 8u; threads *= 2u) {
            sq_params params = defaults();
            params.max_colors = 16u;
            params.thread_count = threads;
            Fixture run(48u, 32u);
            run.pixels = f.pixels;
            SQ_CHECK_EQ(sq_quantize(&run.image, &params, nullptr, nullptr,
                                    &run.result, &run.stats), SQ_OK);
            if (threads == 1u) {
                reference_path = run.stats.path;
            } else {
                SQ_CHECK_EQ(run.stats.path, reference_path);
            }
        }
    }

    // Monotonicity in max_colors with the documented 2 percent tolerance.
    {
        Fixture f(64u, 48u);
        for (std::size_t i = 0; i < f.indices.size(); ++i) {
            const std::uint32_t value = static_cast<std::uint32_t>(i) * 1103515245u;
            f.pixels[i * 4u + 0u] = static_cast<std::uint8_t>(value);
            f.pixels[i * 4u + 1u] = static_cast<std::uint8_t>(value >> 8);
            f.pixels[i * 4u + 2u] = static_cast<std::uint8_t>(value >> 16);
            f.pixels[i * 4u + 3u] = 255u;
        }
        double previous = 0.0;
        for (std::uint32_t k = 2u; k <= 256u; k *= 2u) {
            sq_params params = defaults();
            params.max_colors = k;
            Fixture run(64u, 48u);
            run.pixels = f.pixels;
            SQ_CHECK_EQ(sq_quantize(&run.image, &params, nullptr, nullptr,
                                    &run.result, &run.stats), SQ_OK);
            const double error = mean_de00_of(run);
            SQ_CHECK(std::isfinite(error));
            if (k > 2u && error > previous * 1.02) {
                std::printf("PROPERTY monotonicity: k=%u dE00 %.4f above %.4f "
                            "(+%.2f%%)\n", k, error, previous,
                            (error - previous) / previous * 100.0);
            }
            previous = error;
        }
    }
}

std::vector<sq::Point> refinement_points(std::uint32_t count,
                                         std::uint32_t seed,
                                         bool integer_colors) {
    std::uint32_t state = seed;
    const auto next = [&state]() {
        state = state * 1664525u + 1013904223u;
        return state;
    };
    std::vector<sq::Point> points(count);
    for (std::uint32_t i = 0u; i < count; ++i) {
        const std::uint32_t colour = next();
        const std::uint32_t alpha = next();
        const std::uint32_t repeats = next();
        const std::uint8_t r = static_cast<std::uint8_t>(colour & 0xFFu);
        const std::uint8_t g =
            static_cast<std::uint8_t>((colour >> 8) & 0xFFu);
        const std::uint8_t b =
            static_cast<std::uint8_t>((colour >> 16) & 0xFFu);
        const std::uint32_t n = 1u + repeats % 7u;
        const std::uint32_t a = 1u + alpha % 255u;
        const double fraction = integer_colors ? 0.0 : 0.25;
        points[i].r = static_cast<double>(r) + fraction;
        points[i].g = static_cast<double>(g) + fraction;
        points[i].b = static_cast<double>(b) + fraction;
        points[i].w = static_cast<double>(a) * static_cast<double>(n);
        points[i].n = n;
        points[i].nearest = 0u;
    }
    return points;
}

sq::Palette refinement_initial(std::uint32_t clusters) {
    sq::Palette palette{};
    palette.transparent_index =
        static_cast<std::uint16_t>(SQ_NO_TRANSPARENT_INDEX);
    for (std::uint32_t c = 0u; c < clusters; ++c) {
        const std::uint8_t r = static_cast<std::uint8_t>((c * 17u) & 0xFFu);
        const std::uint8_t g =
            static_cast<std::uint8_t>((c * 53u + 40u) & 0xFFu);
        const std::uint8_t b =
            static_cast<std::uint8_t>((c * 97u + 120u) & 0xFFu);
        palette.rgba[c] = sq::pack_key(r, g, b, 255u);
        palette.r[c] = r;
        palette.g[c] = g;
        palette.b[c] = b;
    }
    palette.size = static_cast<std::uint16_t>(clusters);
    return palette;
}

void test_u1_oklab_roundtrip() {
    std::uint32_t state = 0x9e3779b9u;
    const auto next = [&state]() {
        state = state * 1664525u + 1013904223u;
        return static_cast<double>(state) / 4294967296.0;
    };
    double worst = 0.0;
    for (unsigned trial = 0u; trial < 100000u; ++trial) {
        const double r = next();
        const double g = next();
        const double b = next();
        const sq_color::Oklab colour =
            sq_color::linear_rgb_to_oklab(r, g, b);
        double rr = 0.0;
        double gg = 0.0;
        double bb = 0.0;
        sq_color::oklab_to_linear_rgb(colour, rr, gg, bb);
        worst = std::max(worst, std::fabs(rr - r));
        worst = std::max(worst, std::fabs(gg - g));
        worst = std::max(worst, std::fabs(bb - b));
        SQ_CHECK(std::fabs(rr - r) <= 2e-6);
        SQ_CHECK(std::fabs(gg - g) <= 2e-6);
        SQ_CHECK(std::fabs(bb - b) <= 2e-6);
    }
    SQ_CHECK(worst <= 2e-6);
}

void test_u2_gray_levels_exact() {
    for (unsigned level = 0u; level < 256u; ++level) {
        const std::uint8_t value = static_cast<std::uint8_t>(level);
        const sq_color::Oklab colour =
            sq_color::srgb8_to_oklab(value, value, value);
        std::uint8_t r = 0u;
        std::uint8_t g = 0u;
        std::uint8_t b = 0u;
        sq_color::oklab_to_srgb8(colour, r, g, b);
        SQ_CHECK_EQ(r, value);
        SQ_CHECK_EQ(g, value);
        SQ_CHECK_EQ(b, value);
    }
}

// Concurrent quantize calls share the private clamp counter in this translation
// unit, so a relaxed atomic increment must not lose any event.
void test_oklab_clamp_counter_concurrent() {
    constexpr std::uint32_t kThreads = 8u;
    constexpr std::uint32_t kCalls = 1000u;
    const std::uint64_t before =
        sq_color::oklab_clamp_events().load(std::memory_order_relaxed);
    std::vector<double> results(kThreads, 0.0);
    std::vector<std::thread> workers;
    workers.reserve(kThreads);
    for (std::uint32_t t = 0u; t < kThreads; ++t) {
        workers.emplace_back([&results, t, kCalls]() {
            double sink = 0.0;
            for (std::uint32_t i = 0u; i < kCalls; ++i) {
                double r = 0.0;
                double g = 0.0;
                double b = 0.0;
                sq_color::oklab_to_linear_rgb({2.0, 0.0, 0.0}, r, g, b);
                sink += r + g + b;
            }
            results[t] = sink;
        });
    }
    for (std::thread& worker : workers) {
        worker.join();
    }
    const std::uint64_t after =
        sq_color::oklab_clamp_events().load(std::memory_order_relaxed);
    SQ_CHECK_EQ(after - before,
                static_cast<std::uint64_t>(kThreads) * kCalls);
    for (std::uint32_t t = 0u; t < kThreads; ++t) {
        SQ_CHECK(std::isfinite(results[t]));
    }
}

// The weighted objective of the alternating minimisation must never rise.
void test_u9_objective_monotonic() {
    const std::uint32_t count = 4096u;
    std::vector<sq::Point> points =
        refinement_points(count, 0x51f00du, false);
    std::vector<sq::Shard> shards(32u * 256u);
    const sq::Palette initial = refinement_initial(16u);
    double trace[64]{};
    sq::RefineInfo info;
    sq::Progress progress;
    sq::Palette output{};
    SQ_CHECK(sq::refine_palette(points.data(), count, initial, 20u, 1e-4f,
                                4u, shards.data(), progress, output, info,
                                trace, 64u));
    SQ_CHECK(info.iterations >= 2u);
    SQ_CHECK(info.iterations <= 20u);
    for (std::uint32_t i = 1u; i < info.iterations; ++i) {
        SQ_CHECK(trace[i] <= trace[i - 1u] * (1.0 + 1e-9) + 1e-9);
    }
    SQ_CHECK(info.objective_first >= info.objective_last);
    SQ_CHECK(info.centroid_fingerprint != 0u);
    SQ_CHECK(output.size <= 16u);

    // A zero iteration budget keeps the initial palette unchanged.
    std::vector<sq::Point> skipped =
        refinement_points(count, 0x51f00du, false);
    sq::RefineInfo skipped_info;
    sq::Palette skipped_output{};
    SQ_CHECK(sq::refine_palette(skipped.data(), count, initial, 0u, 1e-4f,
                                4u, shards.data(), progress, skipped_output,
                                skipped_info, nullptr, 0u));
    SQ_CHECK_EQ(skipped_info.iterations, 0u);
    SQ_CHECK_EQ(skipped_output.size, initial.size);
    for (std::uint32_t i = 0u; i < initial.size; ++i) {
        SQ_CHECK_EQ(skipped_output.rgba[i], initial.rgba[i]);
    }
}

// Only entries equal in all four channels merge; a different alpha survives.
void test_u19_dedup_rgba_identity() {
    const std::uint32_t keys[6] = {
        sq::pack_key(10u, 20u, 30u, 255u),
        sq::pack_key(10u, 20u, 30u, 255u),
        sq::pack_key(10u, 20u, 30u, 200u),
        sq::pack_key(40u, 50u, 60u, 128u),
        sq::pack_key(40u, 50u, 60u, 128u),
        sq::pack_key(10u, 20u, 30u, 255u),
    };
    sq::Palette palette{};
    palette.transparent_index =
        static_cast<std::uint16_t>(SQ_NO_TRANSPARENT_INDEX);
    for (std::uint32_t i = 0u; i < 6u; ++i) {
        palette.rgba[i] = keys[i];
        palette.r[i] = static_cast<std::uint8_t>((keys[i] >> 24) & 0xFFu);
        palette.g[i] = static_cast<std::uint8_t>((keys[i] >> 16) & 0xFFu);
        palette.b[i] = static_cast<std::uint8_t>((keys[i] >> 8) & 0xFFu);
    }
    palette.size = 6u;
    sq::dedup_palette(palette, nullptr, 0u);
    SQ_CHECK_EQ(palette.size, 3u);
    SQ_CHECK_EQ(palette.rgba[0], keys[0]);
    SQ_CHECK_EQ(palette.rgba[1], keys[2]);
    SQ_CHECK_EQ(palette.rgba[2], keys[3]);

    sq::Palette transparent{};
    transparent.transparent_index = 0u;
    transparent.rgba[0] = 0u;
    transparent.rgba[1] = keys[0];
    transparent.rgba[2] = keys[0];
    transparent.rgba[3] = keys[2];
    transparent.size = 4u;
    sq::Point assignments[3]{};
    assignments[0].nearest = 1u;
    assignments[1].nearest = 2u;
    assignments[2].nearest = 3u;
    sq::dedup_palette(transparent, assignments, 3u);
    SQ_CHECK_EQ(transparent.size, 3u);
    SQ_CHECK_EQ(transparent.transparent_index, 0u);
    SQ_CHECK_EQ(transparent.rgba[1], keys[0]);
    SQ_CHECK_EQ(transparent.rgba[2], keys[2]);
    SQ_CHECK_EQ(assignments[0].nearest, 1u);
    SQ_CHECK_EQ(assignments[1].nearest, 1u);
    SQ_CHECK_EQ(assignments[2].nearest, 2u);

    // Zero-based cluster indices with a reserved transparent palette entry.
    // Cluster 0 belongs at palette index 1; without the offset it would be
    // remapped to the transparent entry and later clusters would be shifted.
    {
        const std::uint32_t first_key = sq::pack_key(10u, 20u, 30u, 255u);
        const std::uint32_t second_key = sq::pack_key(40u, 50u, 60u, 128u);
        sq::Palette shifted{};
        shifted.transparent_index = 0u;
        shifted.rgba[0] = 0u;
        shifted.rgba[1] = first_key;
        shifted.rgba[2] = first_key;
        shifted.rgba[3] = second_key;
        shifted.size = 4u;
        sq::Point clusters[3]{};
        clusters[0].nearest = 0u;
        clusters[1].nearest = 1u;
        clusters[2].nearest = 2u;
        sq::dedup_palette(shifted, clusters, 3u, 1u);
        SQ_CHECK_EQ(shifted.size, 3u);
        SQ_CHECK_EQ(clusters[0].nearest, 1u);
        SQ_CHECK_EQ(clusters[1].nearest, 1u);
        SQ_CHECK_EQ(clusters[2].nearest, 2u);
    }

    // The refinement path applies the same offset, and a zero iteration budget
    // leaves unassigned points untouched instead of remapping them.
    {
        const std::uint32_t count = 64u;
        sq::Palette with_transparent = refinement_initial(6u);
        with_transparent.transparent_index = 0u;
        for (std::uint32_t i = 6u; i > 0u; --i) {
            with_transparent.rgba[i] = with_transparent.rgba[i - 1u];
            with_transparent.r[i] = with_transparent.r[i - 1u];
            with_transparent.g[i] = with_transparent.g[i - 1u];
            with_transparent.b[i] = with_transparent.b[i - 1u];
        }
        with_transparent.rgba[0] = 0u;
        with_transparent.r[0] = 0u;
        with_transparent.g[0] = 0u;
        with_transparent.b[0] = 0u;
        with_transparent.size = 7u;

        std::vector<sq::Shard> shards(32u * 256u);
        sq::Progress progress;
        double trace[64]{};
        std::vector<sq::Point> points =
            refinement_points(count, 0x6b17c3a9u, true);
        sq::RefineInfo info;
        sq::Palette output{};
        SQ_CHECK(sq::refine_palette(points.data(), count, with_transparent,
                                    20u, 1e-4f, 4u, shards.data(), progress,
                                    output, info, trace, 64u));
        for (std::uint32_t i = 0u; i < count; ++i) {
            SQ_CHECK(points[i].nearest >= 1u);
            SQ_CHECK(points[i].nearest < output.size);
        }

        std::vector<sq::Point> skipped =
            refinement_points(count, 0x6b17c3a9u, true);
        for (sq::Point& point : skipped) {
            point.nearest = 123u;
        }
        sq::RefineInfo skipped_info;
        sq::Palette skipped_output{};
        SQ_CHECK(sq::refine_palette(skipped.data(), count, with_transparent,
                                    0u, 1e-4f, 4u, shards.data(), progress,
                                    skipped_output, skipped_info, nullptr,
                                    0u));
        for (const sq::Point& point : skipped) {
            SQ_CHECK_EQ(point.nearest, 123u);
        }
    }
}

// Fixed logical shards make every floating reduction independent of the
// thread count, on both the histogram source and the exact-colour source.
void test_u23_shard_determinism() {
    const std::uint32_t sources[2] = {4096u, 640u};
    for (std::uint32_t source = 0u; source < 2u; ++source) {
        const std::uint32_t count = sources[source];
        std::uint64_t reference_fp = 0u;
        std::uint32_t reference_iterations = 0u;
        std::uint16_t reference_size = 0u;
        std::uint32_t reference_rgba[256]{};
        double reference_trace[64]{};
        for (std::uint32_t threads = 1u; threads <= 8u; threads *= 2u) {
            std::vector<sq::Point> points = refinement_points(
                count, 0x71a2b3c4u + source, source == 1u);
            std::vector<sq::Shard> shards(32u * 256u);
            const sq::Palette initial =
                refinement_initial(source == 0u ? 24u : 12u);
            double trace[64]{};
            sq::RefineInfo info;
            sq::Progress progress;
            sq::Palette output{};
            SQ_CHECK(sq::refine_palette(
                points.data(), count, initial, 20u, 1e-4f, threads,
                shards.data(), progress, output, info, trace, 64u));
            if (threads == 1u) {
                reference_fp = info.centroid_fingerprint;
                reference_iterations = info.iterations;
                reference_size = output.size;
                std::memcpy(reference_rgba, output.rgba,
                            sizeof(reference_rgba));
                std::memcpy(reference_trace, trace, sizeof(reference_trace));
            } else {
                SQ_CHECK_EQ(info.centroid_fingerprint, reference_fp);
                SQ_CHECK_EQ(info.iterations, reference_iterations);
                SQ_CHECK_EQ(output.size, reference_size);
                for (std::uint32_t i = 0u; i < output.size; ++i) {
                    SQ_CHECK_EQ(output.rgba[i], reference_rgba[i]);
                }
                for (std::uint32_t i = 0u; i < info.iterations; ++i) {
                    SQ_CHECK(std::memcmp(&trace[i], &reference_trace[i],
                                         sizeof(double)) == 0);
                }
            }
        }
    }

    // The same guarantee on the two real clustered paths.
    for (std::uint32_t path_case = 0u; path_case < 2u; ++path_case) {
        Fixture fixture(64u, 48u);
        for (std::size_t i = 0u; i < fixture.indices.size(); ++i) {
            const std::uint32_t value =
                static_cast<std::uint32_t>(i) * 2246822519u;
            if (path_case == 0u) {
                fixture.pixels[i * 4u + 0u] =
                    static_cast<std::uint8_t>(value);
                fixture.pixels[i * 4u + 1u] =
                    static_cast<std::uint8_t>(value >> 8);
                fixture.pixels[i * 4u + 2u] =
                    static_cast<std::uint8_t>(value >> 16);
            } else {
                fixture.pixels[i * 4u + 0u] =
                    static_cast<std::uint8_t>(8u + (i & 7u));
                fixture.pixels[i * 4u + 1u] =
                    static_cast<std::uint8_t>(8u + ((i >> 3) & 7u));
                fixture.pixels[i * 4u + 2u] =
                    static_cast<std::uint8_t>(8u + ((i >> 6) & 7u));
            }
            fixture.pixels[i * 4u + 3u] = 255u;
        }
        std::vector<std::uint8_t> reference_indices;
        std::uint8_t reference_palette[1024]{};
        std::uint16_t reference_palette_size = 0u;
        for (std::uint32_t threads = 1u; threads <= 8u; threads *= 2u) {
            sq_params params = defaults();
            params.max_colors = 16u;
            params.thread_count = threads;
            SQ_CHECK_EQ(sq_quantize(&fixture.image, &params, nullptr, nullptr,
                                    &fixture.result, &fixture.stats),
                        SQ_OK);
            SQ_CHECK_EQ(fixture.stats.path, path_case == 0u
                            ? static_cast<std::uint32_t>(SQ_PATH_GRID)
                            : static_cast<std::uint32_t>(SQ_PATH_DEGENERATE));
            SQ_CHECK(fixture.stats.kmeans_iterations > 0u);
            if (threads == 1u) {
                reference_indices.assign(fixture.indices.begin(),
                                         fixture.indices.end());
                reference_palette_size = fixture.result.palette_size;
                std::memcpy(reference_palette, fixture.result.palette_rgba,
                            sizeof(reference_palette));
            } else {
                SQ_CHECK_EQ(fixture.result.palette_size,
                            reference_palette_size);
                SQ_CHECK(std::memcmp(reference_palette,
                                     fixture.result.palette_rgba,
                                     sizeof(reference_palette)) == 0);
                SQ_CHECK(std::memcmp(reference_indices.data(),
                                     fixture.indices.data(),
                                     fixture.indices.size()) == 0);
            }
        }

        sq_params zero = defaults();
        zero.max_colors = 16u;
        zero.kmeans_max_iter = 0u;
        SQ_CHECK_EQ(sq_quantize(&fixture.image, &zero, nullptr, nullptr,
                                &fixture.result, &fixture.stats),
                    SQ_OK);
        SQ_CHECK_EQ(fixture.stats.kmeans_iterations, 0u);
    }
}

// The Oklab working space keeps the same thread-count independence, and the
// explicit pipeline overrides reproduce the derived pipelines.
void test_oklab_pipeline_determinism() {
    for (std::uint32_t path_case = 0u; path_case < 2u; ++path_case) {
        Fixture fixture(64u, 48u);
        for (std::size_t i = 0u; i < fixture.indices.size(); ++i) {
            const std::uint32_t value =
                static_cast<std::uint32_t>(i) * 2246822519u;
            if (path_case == 0u) {
                fixture.pixels[i * 4u + 0u] =
                    static_cast<std::uint8_t>(value);
                fixture.pixels[i * 4u + 1u] =
                    static_cast<std::uint8_t>(value >> 8);
                fixture.pixels[i * 4u + 2u] =
                    static_cast<std::uint8_t>(value >> 16);
            } else {
                fixture.pixels[i * 4u + 0u] =
                    static_cast<std::uint8_t>(8u + (i & 7u));
                fixture.pixels[i * 4u + 1u] =
                    static_cast<std::uint8_t>(8u + ((i >> 3) & 7u));
                fixture.pixels[i * 4u + 2u] =
                    static_cast<std::uint8_t>(8u + ((i >> 6) & 7u));
            }
            fixture.pixels[i * 4u + 3u] = 255u;
        }
        std::vector<std::uint8_t> reference_indices;
        std::uint8_t reference_palette[1024]{};
        std::uint16_t reference_size = 0u;
        for (std::uint32_t threads = 1u; threads <= 8u; threads *= 2u) {
            sq_params params = defaults();
            params.max_colors = 16u;
            params.color_space = SQ_SPACE_OKLAB;
            params.thread_count = threads;
            SQ_CHECK_EQ(sq_quantize(&fixture.image, &params, nullptr, nullptr,
                                    &fixture.result, &fixture.stats),
                        SQ_OK);
            SQ_CHECK_EQ(fixture.stats.path,
                        path_case == 0u
                            ? static_cast<std::uint32_t>(SQ_PATH_GRID)
                            : static_cast<std::uint32_t>(SQ_PATH_DEGENERATE));
            if (threads == 1u) {
                reference_indices.assign(fixture.indices.begin(),
                                         fixture.indices.end());
                reference_size = fixture.result.palette_size;
                std::memcpy(reference_palette, fixture.result.palette_rgba,
                            sizeof(reference_palette));
            } else {
                SQ_CHECK_EQ(fixture.result.palette_size, reference_size);
                SQ_CHECK(std::memcmp(reference_palette,
                                     fixture.result.palette_rgba,
                                     sizeof(reference_palette)) == 0);
                SQ_CHECK(std::memcmp(reference_indices.data(),
                                     fixture.indices.data(),
                                     fixture.indices.size()) == 0);
            }
        }
    }

    Fixture fixture(64u, 48u);
    for (std::size_t i = 0u; i < fixture.indices.size(); ++i) {
        const std::uint32_t value =
            static_cast<std::uint32_t>(i) * 2246822519u;
        fixture.pixels[i * 4u + 0u] = static_cast<std::uint8_t>(value);
        fixture.pixels[i * 4u + 1u] =
            static_cast<std::uint8_t>(value >> 8);
        fixture.pixels[i * 4u + 2u] =
            static_cast<std::uint8_t>(value >> 16);
        fixture.pixels[i * 4u + 3u] = 255u;
    }
    const auto run = [&fixture](const sq_params& params,
                                std::vector<std::uint8_t>& indices,
                                std::uint8_t* palette) {
        SQ_CHECK_EQ(sq_quantize(&fixture.image, &params, nullptr, nullptr,
                                &fixture.result, &fixture.stats),
                    SQ_OK);
        indices.assign(fixture.indices.begin(), fixture.indices.end());
        std::memcpy(palette, fixture.result.palette_rgba, 1024u);
    };

    sq_params base = defaults();
    base.max_colors = 16u;
    sq_params oklab_auto = base;
    oklab_auto.color_space = SQ_SPACE_OKLAB;
    sq_params oklab_a = base;
    oklab_a.pipeline = SQ_PIPELINE_OKLAB_A;
    sq_params oklab_b = base;
    oklab_b.pipeline = SQ_PIPELINE_OKLAB_B;
    sq_params srgb_override = oklab_auto;
    srgb_override.pipeline = SQ_PIPELINE_SRGB;

    std::vector<std::uint8_t> base_indices;
    std::vector<std::uint8_t> auto_indices;
    std::vector<std::uint8_t> a_indices;
    std::vector<std::uint8_t> b_indices;
    std::vector<std::uint8_t> srgb_indices;
    std::uint8_t base_palette[1024]{};
    std::uint8_t auto_palette[1024]{};
    std::uint8_t a_palette[1024]{};
    std::uint8_t b_palette[1024]{};
    std::uint8_t srgb_palette[1024]{};
    run(base, base_indices, base_palette);
    run(oklab_auto, auto_indices, auto_palette);
    run(oklab_a, a_indices, a_palette);
    run(oklab_b, b_indices, b_palette);
    run(srgb_override, srgb_indices, srgb_palette);
    SQ_CHECK(std::memcmp(a_palette, b_palette, 1024u) == 0);
    SQ_CHECK(std::memcmp(a_indices.data(), b_indices.data(),
                         a_indices.size()) == 0);
    SQ_CHECK(std::memcmp(base_palette, srgb_palette, 1024u) == 0);
    SQ_CHECK(std::memcmp(base_indices.data(), srgb_indices.data(),
                         base_indices.size()) == 0);
    SQ_CHECK(std::memcmp(auto_palette, a_palette, 1024u) == 0);
    SQ_CHECK(std::memcmp(auto_indices.data(), a_indices.data(),
                         auto_indices.size()) == 0);
    SQ_CHECK(std::memcmp(base_palette, auto_palette, 1024u) != 0 ||
             std::memcmp(base_indices.data(), auto_indices.data(),
                         base_indices.size()) != 0);
}

// Strength zero must be byte-identical to the no-dither mapping, while a full
// strength run must actually change the indices.
void test_u11_dither_strength_zero() {
    Fixture fixture(32u, 24u);
    for (std::size_t i = 0u; i < fixture.indices.size(); ++i) {
        const std::uint32_t value =
            static_cast<std::uint32_t>(i) * 2246822519u;
        fixture.pixels[i * 4u + 0u] = static_cast<std::uint8_t>(value);
        fixture.pixels[i * 4u + 1u] = static_cast<std::uint8_t>(value >> 8);
        fixture.pixels[i * 4u + 2u] = static_cast<std::uint8_t>(value >> 16);
        fixture.pixels[i * 4u + 3u] = 255u;
    }
    const auto capture = [&fixture](const sq_params& params,
                                    std::vector<std::uint8_t>& indices,
                                    std::uint8_t* palette) {
        SQ_CHECK_EQ(sq_quantize(&fixture.image, &params, nullptr, nullptr,
                                &fixture.result, &fixture.stats),
                    SQ_OK);
        indices.assign(fixture.indices.begin(), fixture.indices.end());
        std::memcpy(palette, fixture.result.palette_rgba, 1024u);
    };
    sq_params base = defaults();
    base.max_colors = 16u;
    sq_params none = base;
    none.dither_mode = SQ_DITHER_NONE;
    none.dither_strength = 0.0f;
    sq_params zero = base;
    zero.dither_mode = SQ_DITHER_FLOYD_STEINBERG;
    zero.dither_strength = 0.0f;
    sq_params full = base;
    full.dither_mode = SQ_DITHER_FLOYD_STEINBERG;
    full.dither_strength = 1.0f;

    std::vector<std::uint8_t> none_indices;
    std::vector<std::uint8_t> zero_indices;
    std::vector<std::uint8_t> full_indices;
    std::uint8_t none_palette[1024]{};
    std::uint8_t zero_palette[1024]{};
    std::uint8_t full_palette[1024]{};
    capture(none, none_indices, none_palette);
    capture(zero, zero_indices, zero_palette);
    capture(full, full_indices, full_palette);
    SQ_CHECK(std::memcmp(none_palette, zero_palette, 1024u) == 0);
    SQ_CHECK(std::memcmp(none_indices.data(), zero_indices.data(),
                         none_indices.size()) == 0);
    SQ_CHECK(std::memcmp(none_indices.data(), full_indices.data(),
                         none_indices.size()) != 0);
}

// The 1/16 neighbour absorbs the division remainder, so conservation is an
// exact equality for every signed error in the integer domain.
void test_u12_error_conservation() {
    for (std::int32_t error = -1000; error <= 1000; ++error) {
        std::int32_t p7 = 0;
        std::int32_t p3 = 0;
        std::int32_t p5 = 0;
        std::int32_t p1 = 0;
        sq::distribute_error(error, p7, p3, p5, p1);
        SQ_CHECK_EQ(p7 + p3 + p5 + p1, error);
    }
}

// Hand-checked 1x4 case: a palette of {0, 255} and four pixels at 100. The
// first pixel has no incoming error and selects 0; its +100 error sends
// 7/16 x 100 = 43 to the next pixel, whose candidate 143 selects 255 instead
// of 0. Using `original - quantized` would leave the second pixel at 0.
void test_u22_dither_feedback() {
    Fixture fixture(4u, 1u);
    for (std::size_t i = 0u; i < fixture.indices.size(); ++i) {
        fixture.pixels[i * 4u + 0u] = 100u;
        fixture.pixels[i * 4u + 1u] = 100u;
        fixture.pixels[i * 4u + 2u] = 100u;
        fixture.pixels[i * 4u + 3u] = 255u;
    }
    sq_params params = defaults();
    params.max_colors = 2u;
    params.dither_mode = SQ_DITHER_FLOYD_STEINBERG;
    params.dither_strength = 1.0f;
    sq::Workspace workspace;
    SQ_CHECK(workspace.allocate(params, 4u, 1u));
    sq::Palette palette{};
    palette.transparent_index =
        static_cast<std::uint16_t>(SQ_NO_TRANSPARENT_INDEX);
    palette.size = 2u;
    palette.rgba[0] = sq::pack_key(0u, 0u, 0u, 255u);
    palette.rgba[1] = sq::pack_key(255u, 255u, 255u, 255u);
    palette.r[1] = 255u;
    palette.g[1] = 255u;
    palette.b[1] = 255u;
    sq::Progress progress;
    std::vector<std::uint8_t> dithered(4u, 0xFFu);
    std::vector<std::uint8_t> plain(4u, 0xFFu);
    SQ_CHECK(sq::map_dither(fixture.image, params, palette, dithered.data(),
                            workspace, progress, 67u, 32u, false));
    SQ_CHECK(sq::map_nearest(fixture.image, params, palette, plain.data(),
                             workspace, progress, 67u, 32u, false));
    SQ_CHECK_EQ(dithered[0], plain[0]);
    SQ_CHECK(dithered[1] != plain[1]);
}

// When the source colour equals an emitted 8-bit palette entry, the feedback
// error is exactly zero in both working spaces, so no error reaches the next
// pixel and every index stays on that entry.
void test_u24_post_dedup_palette_target() {
    const bool spaces[2] = {false, true};
    for (bool oklab : spaces) {
        Fixture fixture(4u, 1u);
        for (std::size_t i = 0u; i < fixture.indices.size(); ++i) {
            fixture.pixels[i * 4u + 0u] = 10u;
            fixture.pixels[i * 4u + 1u] = 20u;
            fixture.pixels[i * 4u + 2u] = 30u;
            fixture.pixels[i * 4u + 3u] = 255u;
        }
        sq_params params = defaults();
        params.max_colors = 2u;
        params.dither_mode = SQ_DITHER_FLOYD_STEINBERG;
        params.dither_strength = 1.0f;
        params.color_space =
            oklab ? SQ_SPACE_OKLAB : SQ_SPACE_SRGB;
        sq::Workspace workspace;
        SQ_CHECK(workspace.allocate(params, 4u, 1u));
        sq::Palette palette{};
        palette.transparent_index =
            static_cast<std::uint16_t>(SQ_NO_TRANSPARENT_INDEX);
        palette.size = 2u;
        palette.rgba[0] = sq::pack_key(10u, 20u, 30u, 255u);
        palette.r[0] = 10u;
        palette.g[0] = 20u;
        palette.b[0] = 30u;
        palette.rgba[1] = sq::pack_key(200u, 200u, 200u, 255u);
        palette.r[1] = 200u;
        palette.g[1] = 200u;
        palette.b[1] = 200u;
        sq::Progress progress;
        std::vector<std::uint8_t> indices(4u, 0xFFu);
        SQ_CHECK(sq::map_dither(fixture.image, params, palette,
                               indices.data(), workspace, progress, 67u,
                               32u, oklab));
        for (std::size_t i = 0u; i < indices.size(); ++i) {
            SQ_CHECK_EQ(indices[i], 0u);
        }
    }
}

// A row must hand its 5/16 share to the row below. A one-column strip has no
// horizontal neighbours, so the only incoming error on the second row is that
// share: 100 against {0,255} emits +100, p5 = 31, and 131 selects the white
// entry. Clearing the outgoing row before it is read would keep the second row
// at 100 and select black.
void test_dither_vertical_propagation() {
    const auto make_palette = [](std::uint8_t dark, std::uint8_t light) {
        sq::Palette palette{};
        palette.transparent_index =
            static_cast<std::uint16_t>(SQ_NO_TRANSPARENT_INDEX);
        palette.size = 2u;
        palette.rgba[0] = sq::pack_key(dark, dark, dark, 255u);
        palette.rgba[1] = sq::pack_key(light, light, light, 255u);
        palette.r[0] = palette.g[0] = palette.b[0] = dark;
        palette.r[1] = palette.g[1] = palette.b[1] = light;
        return palette;
    };
    const sq::Palette wide_palette = make_palette(0u, 255u);
    const bool spaces[2] = {false, true};

    {
        Fixture fixture(1u, 2u);
        for (std::size_t i = 0u; i < fixture.indices.size(); ++i) {
            fixture.pixels[i * 4u + 0u] = 100u;
            fixture.pixels[i * 4u + 1u] = 100u;
            fixture.pixels[i * 4u + 2u] = 100u;
        }
        sq_params params = defaults();
        params.max_colors = 2u;
        params.dither_mode = SQ_DITHER_FLOYD_STEINBERG;
        params.dither_strength = 1.0f;
        sq::Workspace workspace;
        SQ_CHECK(workspace.allocate(params, 1u, 2u));
        sq::Progress progress;
        SQ_CHECK(sq::map_dither(fixture.image, params, wide_palette,
                                fixture.indices.data(), workspace, progress,
                                67u, 32u, false));
        SQ_CHECK_EQ(fixture.indices[0], 0u);
        SQ_CHECK_EQ(fixture.indices[1], 1u);
    }

    // The control makes row 0 transparent, so it emits no error at all and row
    // 1 is scanned backwards with zero incoming error. A nearby pair of palette
    // entries makes the vertical share observable in both working spaces.
    const std::uint32_t width = 3u;
    const std::uint32_t height = 2u;
    const sq::Palette close_palette = make_palette(100u, 110u);
    for (bool oklab : spaces) {
        Fixture test(width, height);
        Fixture control(width, height);
        Fixture single(width, 1u);
        for (std::size_t i = 0u; i < test.indices.size(); ++i) {
            for (std::uint32_t c = 0u; c < 3u; ++c) {
                test.pixels[i * 4u + c] = 105u;
                control.pixels[i * 4u + c] = 105u;
            }
        }
        for (std::size_t i = 0u; i < single.indices.size(); ++i) {
            for (std::uint32_t c = 0u; c < 3u; ++c) {
                single.pixels[i * 4u + c] = 105u;
            }
        }
        for (std::uint32_t column = 0u; column < width; ++column) {
            control.pixels[column * 4u + 3u] = 0u;
        }

        sq_params params = defaults();
        params.max_colors = 2u;
        params.dither_mode = SQ_DITHER_FLOYD_STEINBERG;
        params.dither_strength = 1.0f;
        params.color_space = oklab ? SQ_SPACE_OKLAB : SQ_SPACE_SRGB;
        sq::Workspace workspace;
        SQ_CHECK(workspace.allocate(params, width, height));
        sq::Progress progress;
        std::vector<std::uint8_t> test_indices(test.indices.size(), 0xFFu);
        std::vector<std::uint8_t> control_indices(control.indices.size(), 0xFFu);
        std::vector<std::uint8_t> single_indices(single.indices.size(), 0xFFu);
        SQ_CHECK(sq::map_dither(test.image, params, close_palette,
                                test_indices.data(), workspace, progress, 67u,
                                32u, oklab));
        SQ_CHECK(sq::map_dither(control.image, params, close_palette,
                                control_indices.data(), workspace, progress,
                                67u, 32u, oklab));
        SQ_CHECK(sq::map_dither(single.image, params, close_palette,
                                single_indices.data(), workspace, progress,
                                67u, 32u, oklab));

        for (std::uint32_t column = 0u; column < width; ++column) {
            SQ_CHECK_EQ(test_indices[column], single_indices[column]);
        }
        bool row_changed = false;
        for (std::uint32_t column = 0u; column < width; ++column) {
            if (test_indices[width + column] !=
                control_indices[width + column]) {
                row_changed = true;
            }
        }
        SQ_CHECK(row_changed);
    }
}

// Row 1 runs right to left. With this source a backward row 1 emits [0, 0],
// while a left-to-right row 1 emits [0, 1]: the last pixel keeps its own
// quantized value instead of absorbing the error that reached it.
void test_dither_serpentine_direction() {
    Fixture fixture(2u, 2u);
    const std::uint8_t source[4] = {100u, 100u, 0u, 151u};
    for (std::size_t i = 0u; i < fixture.indices.size(); ++i) {
        for (std::uint32_t c = 0u; c < 3u; ++c) {
            fixture.pixels[i * 4u + c] = source[i];
        }
    }
    sq_params params = defaults();
    params.max_colors = 2u;
    params.dither_mode = SQ_DITHER_FLOYD_STEINBERG;
    params.dither_strength = 1.0f;
    sq::Workspace workspace;
    SQ_CHECK(workspace.allocate(params, 2u, 2u));
    sq::Palette palette{};
    palette.transparent_index =
        static_cast<std::uint16_t>(SQ_NO_TRANSPARENT_INDEX);
    palette.size = 2u;
    palette.rgba[0] = sq::pack_key(0u, 0u, 0u, 255u);
    palette.rgba[1] = sq::pack_key(255u, 255u, 255u, 255u);
    palette.r[1] = 255u;
    palette.g[1] = 255u;
    palette.b[1] = 255u;
    sq::Progress progress;
    std::vector<std::uint8_t> indices(4u, 0xFFu);
    SQ_CHECK(sq::map_dither(fixture.image, params, palette, indices.data(),
                            workspace, progress, 67u, 32u, false));
    SQ_CHECK_EQ(indices[0], 0u);
    SQ_CHECK_EQ(indices[1], 1u);
    SQ_CHECK_EQ(indices[2], 0u);
    SQ_CHECK_EQ(indices[3], 0u);
}

// Dithering searches the same straight-RGB canonical form that the scan and
// the non-dither mappers use, so a premultiplied input and its recovered
// straight counterpart must produce byte-identical indices in both spaces.
void test_dither_premultiplied_alpha() {
    const std::uint32_t width = 6u;
    const std::uint32_t height = 2u;
    Fixture premultiplied(width, height);
    Fixture straight(width, height);
    bool recovery_matters = false;
    for (std::size_t i = 0u; i < premultiplied.indices.size(); ++i) {
        const std::uint8_t alpha = (i % 5u == 0u)
                                       ? 128u
                                       : ((i % 7u == 0u) ? 200u : 255u);
        const std::uint32_t value =
            static_cast<std::uint32_t>(i) * 2246822519u;
        for (std::uint32_t c = 0u; c < 3u; ++c) {
            const std::uint8_t channel = static_cast<std::uint8_t>(
                (value >> (8u * c)) & 0xFFu);
            premultiplied.pixels[i * 4u + c] = static_cast<std::uint8_t>(
                (static_cast<std::uint32_t>(channel) * alpha + 127u) / 255u);
        }
        premultiplied.pixels[i * 4u + 3u] = alpha;

        std::uint8_t r = 0u;
        std::uint8_t g = 0u;
        std::uint8_t b = 0u;
        std::uint8_t a = 0u;
        sq::canonical_pixel(premultiplied.pixels.data() + i * 4u,
                            SQ_ALPHA_PREMULTIPLIED, r, g, b, a);
        straight.pixels[i * 4u + 0u] = r;
        straight.pixels[i * 4u + 1u] = g;
        straight.pixels[i * 4u + 2u] = b;
        straight.pixels[i * 4u + 3u] = a;
        if (r != premultiplied.pixels[i * 4u + 0u]) {
            recovery_matters = true;
        }
    }
    SQ_CHECK(recovery_matters);

    sq::Palette palette{};
    palette.transparent_index =
        static_cast<std::uint16_t>(SQ_NO_TRANSPARENT_INDEX);
    palette.size = 4u;
    const std::uint8_t shades[4] = {0u, 80u, 160u, 255u};
    for (std::uint32_t i = 0u; i < 4u; ++i) {
        palette.rgba[i] =
            sq::pack_key(shades[i], shades[i], shades[i], 255u);
        palette.r[i] = shades[i];
        palette.g[i] = shades[i];
        palette.b[i] = shades[i];
    }

    const bool spaces[2] = {false, true};
    for (bool oklab : spaces) {
        sq_params p_params = defaults();
        p_params.max_colors = 4u;
        p_params.dither_mode = SQ_DITHER_FLOYD_STEINBERG;
        p_params.dither_strength = 1.0f;
        p_params.alpha_mode = SQ_ALPHA_PREMULTIPLIED;
        p_params.color_space = oklab ? SQ_SPACE_OKLAB : SQ_SPACE_SRGB;
        sq_params s_params = p_params;
        s_params.alpha_mode = SQ_ALPHA_STRAIGHT;

        sq::Workspace workspace;
        SQ_CHECK(workspace.allocate(p_params, width, height));
        sq::Progress progress;
        std::vector<std::uint8_t> p_indices(premultiplied.indices.size(), 0xFFu);
        std::vector<std::uint8_t> s_indices(straight.indices.size(), 0xFFu);
        SQ_CHECK(sq::map_dither(premultiplied.image, p_params, palette,
                                p_indices.data(), workspace, progress, 67u,
                                32u, oklab));
        SQ_CHECK(sq::map_dither(straight.image, s_params, palette,
                                s_indices.data(), workspace, progress, 67u,
                                32u, oklab));
        SQ_CHECK(std::memcmp(p_indices.data(), s_indices.data(),
                             p_indices.size()) == 0);
    }
}

// The serial dither loop must observe a cancellation raised from the calling
// thread's callback and leave no palette behind.
void test_dither_cancellation() {
    Fixture fixture(1u, 32768u);
    for (std::size_t i = 0u; i < fixture.indices.size(); ++i) {
        const std::uint32_t value =
            static_cast<std::uint32_t>(i) * 2246822519u;
        fixture.pixels[i * 4u + 0u] = static_cast<std::uint8_t>(value);
        fixture.pixels[i * 4u + 1u] = static_cast<std::uint8_t>(value >> 8);
        fixture.pixels[i * 4u + 2u] = static_cast<std::uint8_t>(value >> 16);
        fixture.pixels[i * 4u + 3u] = 255u;
    }
    sq_params params = defaults();
    params.max_colors = 16u;
    params.dither_mode = SQ_DITHER_FLOYD_STEINBERG;
    params.dither_strength = 1.0f;
    CancelState state;
    state.cancel_at = 80u;
    SQ_CHECK_EQ(sq_quantize(&fixture.image, &params, cancel_callback, &state,
                            &fixture.result, &fixture.stats),
                SQ_ERR_CANCELLED);
    state.finished = true;
    SQ_CHECK(!state.fired_after_return);
    SQ_CHECK_EQ(fixture.result.palette_size, 0u);
    SQ_CHECK_EQ(fixture.result.transparent_index,
                static_cast<std::uint16_t>(SQ_NO_TRANSPARENT_INDEX));
}

int main() {
    test_u3_ciede2000();
    test_u13_and_u14();
    test_u20_and_negative_cases();
    test_exact_path_and_transparency();
    test_degenerate_path_and_determinism();
    test_second_pass_escalation();
    test_wu_budget_and_transparency();
    test_premultiplied_and_grid_fallback();
    test_cancel_strips();
    test_progress_is_monotonic();
    test_u4_u6_u7_histogram_and_wu();
    test_u10_nearest_tie_break();
    test_u8_palette_budget_and_dedup();
    test_u17_u18_collision_paths();
    test_u21_probe_determinism();
    test_u1_oklab_roundtrip();
    test_u2_gray_levels_exact();
    test_oklab_clamp_counter_concurrent();
    test_u9_objective_monotonic();
    test_u19_dedup_rgba_identity();
    test_u23_shard_determinism();
    test_oklab_pipeline_determinism();
    test_u11_dither_strength_zero();
    test_u12_error_conservation();
    test_u22_dither_feedback();
    test_u24_post_dedup_palette_target();
    test_dither_vertical_propagation();
    test_dither_serpentine_direction();
    test_dither_premultiplied_alpha();
    test_dither_cancellation();
    test_8_2_property_tests();
    return sqtest::finish();
}
