// Round-trip and determinism checks for the in-package optimal-parse deflate.
// Every stream is inflated with lodepng's decoder, which is independent of the
// encoder under test.
#include "check.h"
#include "lodepng.h"
#include "sq_deflate.h"

#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

bool roundtrip(const char* label, const std::vector<unsigned char>& input,
               unsigned iterations) {
    std::vector<std::uint8_t> packed;
    if (!sq_deflate::compress(input.data(), input.size(), packed, iterations)) {
        std::printf("deflate_test: %s could not compress\n", label);
        return false;
    }
    unsigned char* out = nullptr;
    std::size_t outsize = 0;
    const unsigned error =
        lodepng_inflate(&out, &outsize, packed.data(), packed.size(),
                        &lodepng_default_decompress_settings);
    const bool ok = error == 0u && outsize == input.size() &&
                    (input.empty() || std::memcmp(out, input.data(), input.size()) == 0);
    std::free(out);
    if (!ok) {
        std::printf("deflate_test: %s round trip failed (error %u, %zu of %zu bytes)\n",
                    label, error, outsize, input.size());
    }
    return ok;
}

std::vector<unsigned char> repeated_text(int repeats) {
    const char* phrase = "the quick brown fox jumps over the lazy dog. ";
    std::vector<unsigned char> data;
    for (int i = 0; i < repeats; ++i) {
        data.insert(data.end(), phrase, phrase + std::strlen(phrase));
    }
    return data;
}

}  // namespace

int main() {
    const std::vector<unsigned char> one{'a'};
    const std::vector<unsigned char> two{'a', 'b'};
    const std::vector<unsigned char> run(1000u, 'a');
    std::vector<unsigned char> ramp(70000u);
    for (std::size_t i = 0; i < ramp.size(); ++i) {
        ramp[i] = static_cast<unsigned char>(i * 7u + (i >> 8u));
    }
    std::vector<unsigned char> random_bytes(40000u);
    unsigned seed = 12345u;
    for (auto& byte : random_bytes) {
        seed = seed * 1103515245u + 12345u;
        byte = static_cast<unsigned char>(seed >> 16u);
    }
    std::vector<unsigned char> all_bytes;
    for (int repeat = 0; repeat < 300; ++repeat) {
        for (int value = 0; value < 256; ++value) {
            all_bytes.push_back(static_cast<unsigned char>(value));
        }
    }

    SQ_CHECK(roundtrip("single byte", one, 1));
    SQ_CHECK(roundtrip("two bytes", two, 1));
    SQ_CHECK(roundtrip("run of 1000", run, 2));
    SQ_CHECK(roundtrip("repeated text", repeated_text(200), 3));
    SQ_CHECK(roundtrip("ramp spanning blocks", ramp, 2));
    SQ_CHECK(roundtrip("incompressible", random_bytes, 1));
    SQ_CHECK(roundtrip("all byte values", all_bytes, 2));

    // A zero-length stream cannot be inflated by lodepng (it expects at least
    // one output byte), so the empty case is checked as the stored block it
    // must be: final, stored, LEN 0, NLEN 0xFFFF.
    {
        std::vector<std::uint8_t> packed;
        SQ_CHECK(sq_deflate::compress(nullptr, 0u, packed, 1));
        const std::vector<std::uint8_t> expected{0x01u, 0x00u, 0x00u, 0xFFu, 0xFFu};
        SQ_CHECK(packed == expected);
    }

    // Identical input must produce identical bytes, or the package would break
    // AC-11 for the max tier.
    {
        const std::vector<unsigned char> text = repeated_text(500);
        std::vector<std::uint8_t> first;
        std::vector<std::uint8_t> second;
        SQ_CHECK(sq_deflate::compress(text.data(), text.size(), first, 3));
        SQ_CHECK(sq_deflate::compress(text.data(), text.size(), second, 3));
        SQ_CHECK(first == second);
        std::vector<std::uint8_t> once;
        SQ_CHECK(sq_deflate::compress(text.data(), text.size(), once, 1));
        SQ_CHECK(!once.empty());
    }

    // Never worse than an uncompressed stream by more than the block headers.
    {
        const std::vector<unsigned char> text = repeated_text(50);
        std::vector<std::uint8_t> packed;
        SQ_CHECK(sq_deflate::compress(text.data(), text.size(), packed, 1));
        SQ_CHECK(packed.size() < text.size());
    }

    std::printf("deflate_test: %d checks, %d failures\n", sqtest::check_count(),
                sqtest::failure_count());
    return sqtest::failure_count() == 0 ? 0 : 1;
}
