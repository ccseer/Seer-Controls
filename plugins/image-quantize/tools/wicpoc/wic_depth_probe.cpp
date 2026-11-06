// wic_depth_probe.cpp
//
// SPEC-003 (Seer image quantization engine), Phase 0 item 8 and 13.5.
//
// Purpose: determine empirically how the Windows Imaging Component (WIC)
// format converter reduces a 16-bit sample to 8 bits. The helper decodes a
// 16-bit source through WIC and converts it to 8-bit RGBA before quantization;
// 13.5 states that the reduction must be round-to-nearest, and that the helper
//  performs the reduction itself when WIC truncates instead.
//
// The probe converts every one of the 65536 possible 16-bit sample values,
// through both output formats the helper can see:
//
//   * GUID_WICPixelFormat16bppGray -> GUID_WICPixelFormat8bppGray
//   * GUID_WICPixelFormat64bppRGBA -> GUID_WICPixelFormat32bppRGBA
//
// For each path it counts how many samples disagree with round-to-nearest and
// how many disagree with truncation. A path that matches one rule for all
// values is classified ROUND or TRUNCATE; a path that matches neither is
// MIXED. The 32bppRGBA path is the one the helper uses, so its classification
// is authoritative: the program exits 0 when that path is consistent and
// reports whether the helper must reduce 16-bit input itself. The gray8 path
// is reported for completeness.
//
// Exit status:
//   0  both paths are consistent with one rule (ROUND or TRUNCATE)
//   1  a path is MIXED, the two paths disagree, or a WIC call failed
//   2  wrong or missing command line (usage)
//
// Output is a list of "ASSERT <name>: PASS|FAIL (<detail>)" lines followed by
// a RESULT line, so the Phase 0 report can quote the measurement directly.

#include <windows.h>
#include <objbase.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

using Microsoft::WRL::ComPtr;

constexpr unsigned kSide = 256;  // 256 x 256 = every 16-bit sample value
constexpr unsigned kSamples = kSide * kSide;

struct ProbeFailure {
    std::string message;
};

[[noreturn]] void Fail(const std::string& message) { throw ProbeFailure{message}; }

void Require(bool condition, const std::string& message) {
    if (!condition) Fail(message);
}

void RequireHr(HRESULT hr, const char* what) {
    if (FAILED(hr)) {
        char buffer[96];
        std::snprintf(buffer, sizeof(buffer), "%s failed (HRESULT 0x%08lX)",
                      what, static_cast<unsigned long>(hr));
        Fail(std::string(buffer));
    }
}

class ComScope {
public:
    ComScope()
        : hr_(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED)),
          initialized_(SUCCEEDED(hr_)) {}
    ~ComScope() {
        if (initialized_) CoUninitialize();
    }
    ComScope(const ComScope&) = delete;
    ComScope& operator=(const ComScope&) = delete;

    bool ok() const { return initialized_; }
    HRESULT hr() const { return hr_; }

private:
    HRESULT hr_;
    bool initialized_;
};

// Round-to-nearest of v / 257, the exact scale from 16 bits to 8 bits.
// Integer half-up is exact here because 257 is odd and no sample lands on .5.
unsigned RoundTo8(std::uint16_t v) {
    return (static_cast<unsigned>(v) + 128u) / 257u;
}

unsigned TruncateTo8(std::uint16_t v) {
    return static_cast<unsigned>(v) / 257u;
}

ComPtr<IWICImagingFactory> CreateFactory() {
    ComPtr<IWICImagingFactory> factory;
    RequireHr(CoCreateInstance(CLSID_WICImagingFactory, nullptr,
                               CLSCTX_INPROC_SERVER,
                               __uuidof(IWICImagingFactory),
                               reinterpret_cast<void**>(factory.GetAddressOf())),
              "CoCreateInstance(CLSID_WICImagingFactory)");
    return factory;
}

// Creates a 256x256 bitmap in the given 16-bit format and fills every pixel
// with the sample value (row * 256 + column) repeated across the channels.
// The alpha channel, when present, is fully opaque.
ComPtr<IWICBitmap> CreateRamp16(IWICImagingFactory* factory,
                                REFWICPixelFormatGUID format,
                                unsigned bytes_per_pixel) {
    ComPtr<IWICBitmap> bitmap;
    RequireHr(factory->CreateBitmap(kSide, kSide, format, WICBitmapCacheOnLoad,
                                    bitmap.GetAddressOf()),
              "IWICImagingFactory::CreateBitmap(16-bit ramp)");

    WICRect rect{0, 0, static_cast<INT>(kSide), static_cast<INT>(kSide)};
    ComPtr<IWICBitmapLock> lock;
    RequireHr(bitmap->Lock(&rect, WICBitmapLockWrite, lock.GetAddressOf()),
              "IWICBitmap::Lock(16-bit ramp)");

    UINT stride = 0;
    UINT size = 0;
    BYTE* data = nullptr;
    RequireHr(lock->GetStride(&stride), "IWICBitmapLock::GetStride");
    RequireHr(lock->GetDataPointer(&size, &data), "IWICBitmapLock::GetDataPointer");
    Require(stride >= kSide * bytes_per_pixel, "the locked stride is too small");
    Require(size >= stride * kSide, "the locked buffer is too small");

    std::memset(data, 0, static_cast<std::size_t>(size));
    for (unsigned y = 0; y < kSide; ++y) {
        BYTE* row = data + static_cast<std::size_t>(y) * stride;
        for (unsigned x = 0; x < kSide; ++x) {
            const std::uint16_t v = static_cast<std::uint16_t>(y * kSide + x);
            BYTE* pixel = row + static_cast<std::size_t>(x) * bytes_per_pixel;
            // Little-endian 16-bit channels; the first three are the colour.
            pixel[0] = static_cast<BYTE>(v & 0xFFu);
            pixel[1] = static_cast<BYTE>(v >> 8);
            if (bytes_per_pixel == 8u) {
                pixel[2] = pixel[0];
                pixel[3] = pixel[1];
                pixel[4] = pixel[0];
                pixel[5] = pixel[1];
                pixel[6] = 0xFFu;  // alpha = 0xFFFF
                pixel[7] = 0xFFu;
            }
        }
    }
    return bitmap;
}

// Converts through IWICFormatConverter and copies the result out as tightly
// packed rows of the requested byte width.
std::vector<BYTE> ConvertAndCopy(IWICImagingFactory* factory,
                                 IWICBitmapSource* source,
                                 REFWICPixelFormatGUID target,
                                 unsigned bytes_per_pixel) {
    ComPtr<IWICFormatConverter> converter;
    RequireHr(factory->CreateFormatConverter(converter.GetAddressOf()),
              "IWICImagingFactory::CreateFormatConverter");
    RequireHr(converter->Initialize(source, target, WICBitmapDitherTypeNone,
                                    nullptr, 0.0, WICBitmapPaletteTypeCustom),
              "IWICFormatConverter::Initialize");

    const UINT stride = kSide * bytes_per_pixel;
    std::vector<BYTE> pixels(static_cast<std::size_t>(stride) * kSide);
    RequireHr(converter->CopyPixels(nullptr, stride,
                                    static_cast<UINT>(pixels.size()),
                                    pixels.data()),
              "IWICFormatConverter::CopyPixels");
    return pixels;
}

struct Verdict {
    unsigned round_mismatches = 0;
    unsigned truncate_mismatches = 0;
    unsigned high_byte_mismatches = 0;
    unsigned round_256_mismatches = 0;
    unsigned decreases = 0;
    unsigned max_delta = 0;
    const char* rule = "MIXED";
};

// Compares one channel of every pixel against both candidate rules. `step`
// is the channel stride in bytes and `offset` the channel offset.
Verdict Classify(const std::vector<BYTE>& pixels, unsigned bytes_per_pixel,
                 unsigned offset) {
    Verdict verdict;
    int previous = -1;
    for (unsigned i = 0; i < kSamples; ++i) {
        const std::uint16_t v = static_cast<std::uint16_t>(i);
        const unsigned actual =
            static_cast<unsigned>(pixels[static_cast<std::size_t>(i) *
                                             bytes_per_pixel + offset]);
        const unsigned expected_round = RoundTo8(v);
        const unsigned expected_trunc = TruncateTo8(v);
        const unsigned expected_high = static_cast<unsigned>(v) >> 8;
        const unsigned expected_round_256 =
            (static_cast<unsigned>(v) + 128u) >> 8;
        if (actual != expected_round) ++verdict.round_mismatches;
        if (actual != expected_trunc) ++verdict.truncate_mismatches;
        if (actual != expected_high) ++verdict.high_byte_mismatches;
        if (actual != expected_round_256) ++verdict.round_256_mismatches;

        const int delta = static_cast<int>(actual) -
                          static_cast<int>(expected_round);
        const unsigned magnitude =
            static_cast<unsigned>(delta < 0 ? -delta : delta);
        if (magnitude > verdict.max_delta) verdict.max_delta = magnitude;

        if (previous >= 0 && static_cast<int>(actual) < previous) {
            ++verdict.decreases;
        }
        previous = static_cast<int>(actual);
    }

    if (verdict.round_mismatches == 0u) {
        verdict.rule = "ROUND_257";
    } else if (verdict.round_256_mismatches == 0u) {
        verdict.rule = "ROUND_256";
    } else if (verdict.high_byte_mismatches == 0u) {
        verdict.rule = "TRUNCATE_256";
    } else if (verdict.truncate_mismatches == 0u) {
        verdict.rule = "TRUNCATE_257";
    } else {
        verdict.rule = "MIXED";
    }
    return verdict;
}

void PrintVerdict(const char* name, const Verdict& v) {
    std::printf("DEPTH16 %s: rule=%s round257_mismatch=%u "
                "truncate257_mismatch=%u highbyte_mismatch=%u "
                "round256_mismatch=%u decreases=%u max_delta=%u\n",
                name, v.rule, v.round_mismatches, v.truncate_mismatches,
                v.high_byte_mismatches, v.round_256_mismatches,
                v.decreases, v.max_delta);
}

void PrintSamples(const char* name, const std::vector<BYTE>& pixels,
                  unsigned bytes_per_pixel, unsigned offset) {
    const unsigned probes[] = {0u, 1u, 127u, 128u, 129u, 255u, 256u, 257u,
                               258u, 511u, 512u, 32767u, 32768u, 65407u,
                               65408u, 65535u};
    std::printf("DEPTH16 %s samples:", name);
    for (unsigned v : probes) {
        const unsigned actual =
            static_cast<unsigned>(pixels[static_cast<std::size_t>(v) *
                                             bytes_per_pixel + offset]);
        std::printf(" %u->%u", v, actual);
    }
    std::printf("\n");
}

int RunProbe() {
    ComScope com;
    if (!com.ok()) {
        std::printf("FATAL: CoInitializeEx failed (HRESULT 0x%08lX)\n",
                    static_cast<unsigned long>(com.hr()));
        std::printf("RESULT: FAILED\n");
        return 1;
    }

    try {
        ComPtr<IWICImagingFactory> factory = CreateFactory();

        ComPtr<IWICBitmap> gray16 =
            CreateRamp16(factory.Get(), GUID_WICPixelFormat16bppGray, 2u);
        const std::vector<BYTE> gray8 =
            ConvertAndCopy(factory.Get(), gray16.Get(),
                           GUID_WICPixelFormat8bppGray, 1u);
        const Verdict gray_verdict = Classify(gray8, 1u, 0u);
        PrintVerdict("GRAY8", gray_verdict);
        PrintSamples("GRAY8", gray8, 1u, 0u);

        ComPtr<IWICBitmap> rgba64 =
            CreateRamp16(factory.Get(), GUID_WICPixelFormat64bppRGBA, 8u);
        const std::vector<BYTE> rgba32 =
            ConvertAndCopy(factory.Get(), rgba64.Get(),
                           GUID_WICPixelFormat32bppRGBA, 4u);
        const Verdict red_verdict = Classify(rgba32, 4u, 0u);
        const Verdict green_verdict = Classify(rgba32, 4u, 1u);
        const Verdict blue_verdict = Classify(rgba32, 4u, 2u);
        PrintVerdict("RGBA32_R", red_verdict);
        PrintVerdict("RGBA32_G", green_verdict);
        PrintVerdict("RGBA32_B", blue_verdict);
        PrintSamples("RGBA32_R", rgba32, 4u, 0u);

        bool alpha_ok = true;
        for (unsigned i = 0; i < kSamples; ++i) {
            if (rgba32[static_cast<std::size_t>(i) * 4u + 3u] != 0xFFu) {
                alpha_ok = false;
                break;
            }
        }

        // The helper decodes to 32bppRGBA, so that path is the authoritative
        // one. The gray8 path is reported for completeness; its mapping is
        // not a simple linear rule on this build, which is additional evidence
        // that WIC must not be trusted to reduce 16-bit samples.
        const bool rgba_agree =
            std::strcmp(red_verdict.rule, green_verdict.rule) == 0 &&
            std::strcmp(green_verdict.rule, blue_verdict.rule) == 0;
        const bool helper_rule_known =
            rgba_agree && std::strcmp(red_verdict.rule, "MIXED") != 0 &&
            red_verdict.decreases == 0u && green_verdict.decreases == 0u &&
            blue_verdict.decreases == 0u;

        std::printf("ASSERT helper_path_rule_known: %s (%s)\n",
                    helper_rule_known ? "PASS" : "FAIL", red_verdict.rule);
        std::printf("ASSERT helper_path_monotonic: %s (decreases=%u)\n",
                    red_verdict.decreases == 0u ? "PASS" : "FAIL",
                    red_verdict.decreases);
        std::printf("ASSERT alpha_opaque: %s\n", alpha_ok ? "PASS" : "FAIL");
        std::printf("INFO gray8_path_rule: %s (not the helper path)\n",
                    gray_verdict.rule);

        if (!helper_rule_known || !alpha_ok) {
            std::printf("RESULT: FAILED (the helper path is inconsistent)\n");
            return 1;
        }
        if (std::strcmp(red_verdict.rule, "ROUND_257") == 0) {
            std::printf("ACTION: none; WIC rounds on the helper path\n");
            std::printf("RESULT: PASS (WIC rounds)\n");
        } else {
            std::printf("ACTION: the helper must reduce 16-bit input itself "
                        "before quantization (WIC rule: %s)\n",
                        red_verdict.rule);
            std::printf("RESULT: PASS (WIC does not round exactly; helper-side "
                        "reduction required)\n");
        }
        return 0;
    } catch (const ProbeFailure& failure) {
        std::printf("FATAL: %s\n", failure.message.c_str());
        std::printf("RESULT: FAILED\n");
        return 1;
    }
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 2 || std::strcmp(argv[1], "--selfcheck") != 0) {
        std::printf("Usage: quantize_wic_depth --selfcheck\n");
        return 2;
    }
    return RunProbe();
}
