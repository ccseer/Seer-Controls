// Manual full-enumeration round trip required by 5.6.1: every one of the
// 16.7M eight-bit RGB triples must return within one LSB on every channel.
// This tool is kept out of the unit suite so the CTest preset stays fast.

#include <chrono>
#include <cstdint>
#include <cstdio>

#include "sq_color.h"

namespace {

std::uint32_t channel_delta(std::uint8_t a, std::uint8_t b) {
    return a > b ? static_cast<std::uint32_t>(a - b)
                 : static_cast<std::uint32_t>(b - a);
}

}  // namespace

int main() {
    const auto start = std::chrono::steady_clock::now();
    std::uint64_t failures = 0u;
    std::uint32_t worst = 0u;
    std::uint32_t worst_rgb = 0u;
    constexpr std::uint32_t kTriples = 16777216u;  // 256 * 256 * 256

    for (std::uint32_t packed = 0u; packed < kTriples; ++packed) {
        const std::uint8_t r = static_cast<std::uint8_t>((packed >> 16) & 0xFFu);
        const std::uint8_t g = static_cast<std::uint8_t>((packed >> 8) & 0xFFu);
        const std::uint8_t b = static_cast<std::uint8_t>(packed & 0xFFu);
        const sq_color::Oklab colour = sq_color::srgb8_to_oklab(r, g, b);
        std::uint8_t rr = 0u;
        std::uint8_t gg = 0u;
        std::uint8_t bb = 0u;
        sq_color::oklab_to_srgb8(colour, rr, gg, bb);
        const std::uint32_t dr = channel_delta(rr, r);
        const std::uint32_t dg = channel_delta(gg, g);
        const std::uint32_t db = channel_delta(bb, b);
        const std::uint32_t delta = dr > dg ? (dr > db ? dr : db)
                                            : (dg > db ? dg : db);
        if (delta > 1u) {
            ++failures;
        }
        if (delta > worst) {
            worst = delta;
            worst_rgb = packed;
        }
    }

    const double elapsed =
        std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - start).count();
    std::printf(
        "OKLAB_ROUNDTRIP triples=%u failures=%llu worst_lsb=%u "
        "worst_rgb=0x%06X elapsed_ms=%.1f clamp_events=%llu\n",
        kTriples, static_cast<unsigned long long>(failures), worst, worst_rgb,
        elapsed, static_cast<unsigned long long>(
                     sq_color::oklab_clamp_events().load(
                         std::memory_order_relaxed)));
    return failures == 0u ? 0 : 1;
}
