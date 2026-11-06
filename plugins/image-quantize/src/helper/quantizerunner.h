#pragma once

#include <cstdint>
#include <string>

namespace iq {

// Lossless Deflate optimisation tiers, cheapest to strongest.
enum class DeflateTier : std::uint32_t {
    kFast = 0u,
    kBalanced = 1u,
    kMax = 2u,
};

// Shipping defaults.
struct Options {
    std::wstring input;
    std::wstring output;
    std::wstring log_path;
    std::uint32_t colors = 256u;
    bool dither = true;
    float strength = 0.5f;
    bool oklab = true;
    std::uint32_t pipeline = 0u;
    std::uint32_t kmeans_iter = 20u;
    std::uint32_t threads = 0u;
    // Shipping default: the button exists to shrink the file, and the
    // manifest does not pass a tier, so the helper default is the product
    // default.
    DeflateTier deflate = DeflateTier::kMax;
    bool overwrite = false;
    // Viewer mode: when the host passes the viewer executable, the helper
    // opens it in its waiting state on the final output path before any heavy
    // work and lets the output rename complete the wait.
    std::wstring seerviewer_exe;
    std::uint32_t wait_timeout_ms = 300000u;
};

// Writes exactly one UTF-8 line to stderr. CR and LF are stripped first, so a
// path containing them cannot break the single-line stderr contract.
void write_error(const std::wstring& text);

int run(int argc, wchar_t** argv);

}  // namespace iq
