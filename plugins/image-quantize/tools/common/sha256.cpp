#include "sha256.h"

#include <array>
#include <cstdio>

namespace sqtools {
namespace {

constexpr std::array<std::uint32_t, 64> kRoundConstants = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u,
    0x923f82a4u, 0xab1c5ed5u, 0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u,
    0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u, 0xe49b69c1u, 0xefbe4786u,
    0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
    0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u,
    0x06ca6351u, 0x14292967u, 0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u,
    0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u, 0xa2bfe8a1u, 0xa81a664bu,
    0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
    0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au,
    0x5b9cca4fu, 0x682e6ff3u, 0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
    0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u};

std::uint32_t rotate_right(std::uint32_t value, unsigned bits) {
    return (value >> bits) | (value << (32u - bits));
}

void compress(const std::uint8_t block[64], std::uint32_t h[8]) {
    std::uint32_t w[64] = {};
    for (unsigned i = 0; i < 16u; ++i) {
        w[i] = (static_cast<std::uint32_t>(block[i * 4u]) << 24u)
             | (static_cast<std::uint32_t>(block[i * 4u + 1u]) << 16u)
             | (static_cast<std::uint32_t>(block[i * 4u + 2u]) << 8u)
             | static_cast<std::uint32_t>(block[i * 4u + 3u]);
    }
    for (unsigned i = 16u; i < 64u; ++i) {
        const std::uint32_t s0 = rotate_right(w[i - 15u], 7u)
                               ^ rotate_right(w[i - 15u], 18u)
                               ^ (w[i - 15u] >> 3u);
        const std::uint32_t s1 = rotate_right(w[i - 2u], 17u)
                               ^ rotate_right(w[i - 2u], 19u)
                               ^ (w[i - 2u] >> 10u);
        w[i] = w[i - 16u] + s0 + w[i - 7u] + s1;
    }
    std::uint32_t a = h[0], b = h[1], c = h[2], d = h[3];
    std::uint32_t e = h[4], f = h[5], g = h[6], hh = h[7];
    for (unsigned i = 0; i < 64u; ++i) {
        const std::uint32_t S1 = rotate_right(e, 6u) ^ rotate_right(e, 11u) ^ rotate_right(e, 25u);
        const std::uint32_t ch = (e & f) ^ ((~e) & g);
        const std::uint32_t t1 = hh + S1 + ch + kRoundConstants[i] + w[i];
        const std::uint32_t S0 = rotate_right(a, 2u) ^ rotate_right(a, 13u) ^ rotate_right(a, 22u);
        const std::uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        const std::uint32_t t2 = S0 + maj;
        hh = g; g = f; f = e; e = d + t1;
        d = c; c = b; b = a; a = t1 + t2;
    }
    h[0] += a; h[1] += b; h[2] += c; h[3] += d;
    h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
}

}  // namespace

std::string sha256_hex(const std::uint8_t* data, std::size_t size) {
    std::uint32_t h[8] = {0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
                          0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u};
    const std::uint64_t bit_length = static_cast<std::uint64_t>(size) * 8u;
    std::vector<std::uint8_t> buffer(data, data + size);
    buffer.push_back(0x80u);
    while ((buffer.size() % 64u) != 56u) {
        buffer.push_back(0u);
    }
    for (int shift = 56; shift >= 0; shift -= 8) {
        buffer.push_back(static_cast<std::uint8_t>((bit_length >> shift) & 0xFFu));
    }
    for (std::size_t offset = 0; offset < buffer.size(); offset += 64u) {
        compress(buffer.data() + offset, h);
    }
    std::string result;
    result.reserve(64u);
    for (std::uint32_t word : h) {
        char digits[9] = {};
        std::snprintf(digits, sizeof(digits), "%08x", word);
        result.append(digits);
    }
    return result;
}

}  // namespace sqtools
