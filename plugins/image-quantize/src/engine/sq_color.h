#ifndef SQ_COLOR_H
#define SQ_COLOR_H

// Header-only colour math shared by the engine and the harnesses. Everything
// lives in an anonymous namespace so the engine exports only its C ABI symbols.

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>

namespace {

namespace sq_color {

struct Lab {
    double L;
    double a;
    double b;
};

namespace detail {

constexpr double kPi = 3.14159265358979323846;

inline double sq(double x) { return x * x; }
inline double rad(double d) { return d * kPi / 180.0; }
inline double deg(double r) { return r * 180.0 / kPi; }

struct LinearTable {
    std::array<double, 256> v{};
    LinearTable() {
        for (int i = 0; i < 256; ++i) {
            const double c = static_cast<double>(i) / 255.0;
            v[static_cast<std::size_t>(i)] =
                c <= 0.04045 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4);
        }
    }
};

// Built once from the sRGB transfer function so the per-pixel path has no pow.
inline const std::array<double, 256>& srgb_to_linear() {
    static const LinearTable table;
    return table.v;
}

inline double lab_f(double t) {
    return t > 216.0 / 24389.0 ? std::cbrt(t) : (841.0 / 108.0) * t + 4.0 / 29.0;
}

inline double hue_degrees(double b, double a) {
    if (a == 0.0 && b == 0.0) {
        return 0.0;
    }
    double h = deg(std::atan2(b, a));
    if (h < 0.0) {
        h += 360.0;
    }
    return h;
}

}  // namespace detail

// D65 2 degree observer on the 0..1 XYZ scale produced by the matrix below.
inline Lab srgb8_to_lab(std::uint8_t r8, std::uint8_t g8, std::uint8_t b8) {
    const auto& t = detail::srgb_to_linear();
    const double r = t[r8];
    const double g = t[g8];
    const double b = t[b8];
    const double X = 0.4124564 * r + 0.3575761 * g + 0.1804375 * b;
    const double Y = 0.2126729 * r + 0.7151522 * g + 0.0721750 * b;
    const double Z = 0.0193339 * r + 0.1191920 * g + 0.9503041 * b;
    const double fx = detail::lab_f(X / 0.95047);
    const double fy = detail::lab_f(Y / 1.00000);
    const double fz = detail::lab_f(Z / 1.08883);
    return {116.0 * fy - 16.0, 500.0 * (fx - fy), 200.0 * (fy - fz)};
}

inline double lstar_from_gray8(std::uint8_t v) {
    return srgb8_to_lab(v, v, v).L;
}

inline double delta_e_76(const Lab& x, const Lab& y) {
    using detail::sq;
    return std::sqrt(sq(y.L - x.L) + sq(y.a - x.a) + sq(y.b - x.b));
}

// Sharma, Wu, Dalal (2005), with kL = kC = kH = 1.
inline double delta_e_2000(const Lab& x, const Lab& y) {
    using detail::sq;
    using detail::rad;
    using detail::deg;

    const double C1 = std::sqrt(sq(x.a) + sq(x.b));
    const double C2 = std::sqrt(sq(y.a) + sq(y.b));
    const double Cbar = (C1 + C2) * 0.5;
    const double Cbar7 = std::pow(Cbar, 7.0);
    const double pow25_7 = 6103515625.0;  // 25^7
    const double G = 0.5 * (1.0 - std::sqrt(Cbar7 / (Cbar7 + pow25_7)));

    const double a1p = (1.0 + G) * x.a;
    const double a2p = (1.0 + G) * y.a;
    const double C1p = std::sqrt(sq(a1p) + sq(x.b));
    const double C2p = std::sqrt(sq(a2p) + sq(y.b));
    const double h1p = C1p == 0.0 ? 0.0 : deg(std::atan2(x.b, a1p)) < 0.0
                                                  ? deg(std::atan2(x.b, a1p)) + 360.0
                                                  : deg(std::atan2(x.b, a1p));
    const double h2p = C2p == 0.0 ? 0.0 : deg(std::atan2(y.b, a2p)) < 0.0
                                                  ? deg(std::atan2(y.b, a2p)) + 360.0
                                                  : deg(std::atan2(y.b, a2p));

    const double dLp = y.L - x.L;
    const double dCp = C2p - C1p;
    double dhp = 0.0;
    const double dhRaw = h2p - h1p;
    if (C1p * C2p != 0.0) {
        if (dhRaw > 180.0) {
            dhp = dhRaw - 360.0;
        } else if (dhRaw < -180.0) {
            dhp = dhRaw + 360.0;
        } else {
            dhp = dhRaw;
        }
    }
    const double dHp = 2.0 * std::sqrt(C1p * C2p) * std::sin(rad(dhp) * 0.5);

    const double Lbarp = (x.L + y.L) * 0.5;
    const double Cbarp = (C1p + C2p) * 0.5;
    double hbarp = 0.0;
    if (C1p * C2p == 0.0) {
        hbarp = h1p + h2p;
    } else if (std::fabs(h1p - h2p) <= 180.0) {
        hbarp = (h1p + h2p) * 0.5;
    } else if (h1p + h2p < 360.0) {
        hbarp = (h1p + h2p + 360.0) * 0.5;
    } else {
        hbarp = (h1p + h2p - 360.0) * 0.5;
    }

    const double T = 1.0 - 0.17 * std::cos(rad(hbarp - 30.0))
                   + 0.24 * std::cos(rad(2.0 * hbarp))
                   + 0.32 * std::cos(rad(3.0 * hbarp + 6.0))
                   - 0.20 * std::cos(rad(4.0 * hbarp - 63.0));
    const double dTheta = 30.0 * std::exp(-sq((hbarp - 275.0) / 25.0));
    const double Cbarp7 = std::pow(Cbarp, 7.0);
    const double Rc = 2.0 * std::sqrt(Cbarp7 / (Cbarp7 + pow25_7));
    const double Lm50 = Lbarp - 50.0;
    const double Sl = 1.0 + (0.015 * sq(Lm50)) / std::sqrt(20.0 + sq(Lm50));
    const double Sc = 1.0 + 0.045 * Cbarp;
    const double Sh = 1.0 + 0.015 * Cbarp * T;
    const double Rt = -std::sin(rad(2.0 * dTheta)) * Rc;

    const double tL = dLp / Sl;
    const double tC = dCp / Sc;
    const double tH = dHp / Sh;
    const double sum = sq(tL) + sq(tC) + sq(tH) + Rt * tC * tH;
    return std::sqrt(std::max(0.0, sum));
}

struct Oklab {
    double L;
    double a;
    double b;
};

// Clamp events are diagnostics only; the counter is private to this header and
// is never copied into a boundary struct. Concurrent quantize calls may reach
// this function at the same time, so the increment must be atomic.
inline std::atomic<std::uint64_t>& oklab_clamp_events() {
    static std::atomic<std::uint64_t> count{0u};
    return count;
}

inline double srgb_channel_to_linear(double c) {
    return c <= 0.04045 ? c / 12.92
                        : std::pow((c + 0.055) / 1.055, 2.4);
}

inline double linear_to_srgb_channel(double c) {
    return c <= 0.0031308 ? 12.92 * c
                          : 1.055 * std::pow(c, 1.0 / 2.4) - 0.055;
}

inline Oklab linear_rgb_to_oklab(double r, double g, double b) {
    const double l = 0.4122214708 * r + 0.5363325363 * g + 0.0514459929 * b;
    const double m = 0.2119034982 * r + 0.6806995451 * g + 0.1073969566 * b;
    const double s = 0.0883024619 * r + 0.2817188376 * g + 0.6299787005 * b;
    const double l_ = std::cbrt(l);
    const double m_ = std::cbrt(m);
    const double s_ = std::cbrt(s);
    return {0.2104542553 * l_ + 0.7936177850 * m_ - 0.0040720468 * s_,
            1.9779984951 * l_ - 2.4285922050 * m_ + 0.4505937099 * s_,
            0.0259040371 * l_ + 0.7827717662 * m_ - 0.8086757660 * s_};
}

// Eight-bit inputs use the shared 256-entry table. Fractional cell means use
// the transfer formula; the table itself is still the only constant table.
inline Oklab srgb8_to_oklab(std::uint8_t r8, std::uint8_t g8,
                            std::uint8_t b8) {
    const auto& table = detail::srgb_to_linear();
    return linear_rgb_to_oklab(table[r8], table[g8], table[b8]);
}

inline Oklab srgb_value_to_oklab(double r, double g, double b) {
    return linear_rgb_to_oklab(srgb_channel_to_linear(r / 255.0),
                               srgb_channel_to_linear(g / 255.0),
                               srgb_channel_to_linear(b / 255.0));
}

inline void oklab_to_linear_rgb(const Oklab& colour, double& r, double& g,
                                double& b) {
    const double l_ = colour.L + 0.3963377774 * colour.a
                    + 0.2158037573 * colour.b;
    const double m_ = colour.L - 0.1055613458 * colour.a
                    - 0.0638541728 * colour.b;
    const double s_ = colour.L - 0.0894841775 * colour.a
                    - 1.2914855480 * colour.b;
    const double l = l_ * l_ * l_;
    const double m = m_ * m_ * m_;
    const double s = s_ * s_ * s_;
    r =  4.0767416621 * l - 3.3077115913 * m + 0.2309699292 * s;
    g = -1.2684380046 * l + 2.6097574011 * m - 0.3413193965 * s;
    b = -0.0041960863 * l - 0.7034186147 * m + 1.7076147010 * s;
    if (r < 0.0 || r > 1.0 || g < 0.0 || g > 1.0 || b < 0.0 || b > 1.0) {
        oklab_clamp_events().fetch_add(1u, std::memory_order_relaxed);
    }
    r = std::clamp(r, 0.0, 1.0);
    g = std::clamp(g, 0.0, 1.0);
    b = std::clamp(b, 0.0, 1.0);
}

inline std::uint8_t linear_to_srgb8(double linear) {
    const double rounded =
        std::floor(linear_to_srgb_channel(linear) * 255.0 + 0.5);
    if (rounded <= 0.0) {
        return 0u;
    }
    if (rounded >= 255.0) {
        return 255u;
    }
    return static_cast<std::uint8_t>(rounded);
}

inline void oklab_to_srgb8(const Oklab& colour, std::uint8_t& r8,
                           std::uint8_t& g8, std::uint8_t& b8) {
    double r = 0.0;
    double g = 0.0;
    double b = 0.0;
    oklab_to_linear_rgb(colour, r, g, b);
    r8 = linear_to_srgb8(r);
    g8 = linear_to_srgb8(g);
    b8 = linear_to_srgb8(b);
}

}  // namespace sq_color

}  // namespace

#endif  // SQ_COLOR_H
