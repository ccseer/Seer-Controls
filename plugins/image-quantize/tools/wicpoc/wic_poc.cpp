// wic_poc.cpp
//
// SPEC-003 (Seer image quantization engine), section 13.5, Phase 0.
//
// Purpose: determine empirically whether the Windows Imaging Component (WIC)
// PNG encoder emits a complete tRNS array (one straight-alpha byte per PLTE
// entry) for a GUID_WICPixelFormat8bppIndexed frame whose IWICPalette entries
// carry several distinct alpha values. WICColor is 0xAARRGGBB.
//
// Six steps from 13.5:
//   1. Build a 256-entry IWICPalette with several distinct alpha values.
//   2. Create a GUID_WICPixelFormat8bppIndexed frame and write index pixels.
//   3. Encode with the WIC PNG encoder (GUID_ContainerFormatPng) to an
//      in-memory IWICStream backed by an HGLOBAL IStream.
//   4. Decode the encoded bytes with the independent lodepng decoder.
//   5. Assert IHDR colour type 3, PLTE entries, tRNS (one alpha byte per
//      entry), every decoded pixel index, and every decoded alpha.
//   6. Record the encoded size and compare with the pinned baseline tool.
//
// Step 6 needs two external inputs: the pinned baseline executable and one
// real fixture. Both are passed on the command line, so the default CTest run
// stays free of external files and reports step 6 as BLOCKED. When both are
// supplied, the baseline tool quantizes the fixture, its palette and indices
// are fed back through the WIC encoder, and the two file sizes are compared.
// Re-encoding the same indexed image isolates the encoder, which is what the
// size gate measures.
//
// Exit status:
//   0  steps 1-5 passed (with no external inputs), or steps 1-6 passed
//   1  an assertion failed
//   2  wrong or missing command line (usage)
//
// Every assertion is printed as a structured
// "ASSERT <name>: PASS|FAIL (<detail>)" line so the Phase 0 report can quote
// it. Run with --selfcheck (this is how CTest invokes the target).
//
// Build: C++17, MSVC /W4 /WX /utf-8, static CRT; links windowscodecs, ole32
// and quantize_lodepng (which exposes "lodepng.h"). English-only ASCII, LF.

#include "lodepng.h"

#include <windows.h>
#include <objbase.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <array>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

using Microsoft::WRL::ComPtr;

constexpr unsigned kWidth = 16;
constexpr unsigned kHeight = 16;
constexpr unsigned kPaletteSize = 256;
constexpr unsigned kMaxColors = 256;
constexpr const char* kDitherMode = "none";

struct Options {
    std::string baseline;
    std::string image;
    std::string work_dir;
};

// ---------------------------------------------------------------------------
// Error handling and COM lifetime
// ---------------------------------------------------------------------------

struct PocFailure {
    std::string message;
};

[[noreturn]] void Fail(const std::string& message) { throw PocFailure{message}; }

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

// CoInitializeEx returns S_FALSE when COM is already initialised on this
// thread; that value is a success code and still requires one CoUninitialize,
// so SUCCEEDED (rather than == S_OK) is the correct test here.
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

// ---------------------------------------------------------------------------
// Step 1 data: a 256-entry palette with distinct RGB and distinct alpha.
//
// WICColor is 0xAARRGGBB. The red channel equals the entry index, so every
// palette entry is uniquely recoverable from its colour. Alpha is
// (i * 37) mod 256; 37 is coprime with 256, so this is a bijection and all
// 256 alphas are distinct. alpha[255] is deliberately non-opaque (219), so an
// encoder that trims trailing opaque tRNS bytes still has to emit all 256.
// ---------------------------------------------------------------------------
using Palette = std::array<WICColor, kPaletteSize>;

Palette BuildPalette() {
    Palette palette{};
    for (unsigned i = 0; i < kPaletteSize; ++i) {
        const unsigned a = (i * 37u) & 0xFFu;
        const unsigned r = i;
        const unsigned g = 255u - i;
        const unsigned b = (i * 3u) & 0xFFu;
        palette[i] = (a << 24) | (r << 16) | (g << 8) | b;
    }
    return palette;
}

unsigned CountDistinctAlphas(const Palette& palette) {
    bool seen[256] = {};
    unsigned count = 0;
    for (WICColor color : palette) {
        const unsigned a = (color >> 24) & 0xFFu;
        if (!seen[a]) {
            seen[a] = true;
            ++count;
        }
    }
    return count;
}

unsigned AlphaOf(WICColor c) { return (c >> 24) & 0xFFu; }
unsigned RedOf(WICColor c) { return (c >> 16) & 0xFFu; }
unsigned GreenOf(WICColor c) { return (c >> 8) & 0xFFu; }
unsigned BlueOf(WICColor c) { return c & 0xFFu; }

const char* YesNo(bool value) { return value ? "yes" : "no"; }

// ---------------------------------------------------------------------------
// Step 3: WIC encode to an in-memory stream.
//
// All interfaces are held by WRL ComPtr, so each is released deterministically
// before CoUninitialize runs (the ComPtrs live inside this function and are
// destroyed when it returns, while ComScope lives in the caller).
// ---------------------------------------------------------------------------
struct IndexedImage {
    unsigned width = 0;
    unsigned height = 0;
    std::vector<WICColor> palette;
    std::vector<BYTE> indices;
};

std::vector<unsigned char> EncodeWithWic(const IndexedImage& image) {
    Require(image.width > 0u && image.height > 0u, "the indexed image is empty");
    Require(!image.palette.empty() && image.palette.size() <= kPaletteSize,
            "the indexed image palette must hold 1..256 entries");
    Require(image.indices.size() ==
                static_cast<std::size_t>(image.width) * image.height,
            "the indexed image index count does not match its dimensions");

    ComPtr<IWICImagingFactory> factory;
    RequireHr(CoCreateInstance(CLSID_WICImagingFactory, nullptr,
                               CLSCTX_INPROC_SERVER,
                               __uuidof(IWICImagingFactory),
                               reinterpret_cast<void**>(factory.GetAddressOf())),
              "CoCreateInstance(CLSID_WICImagingFactory)");

    ComPtr<IWICPalette> wicPalette;
    RequireHr(factory->CreatePalette(wicPalette.GetAddressOf()),
              "IWICImagingFactory::CreatePalette");
    // InitializeCustom takes a non-const WICColor* even though it only reads
    // the entries (an "in" parameter), so a const_cast is required here.
    RequireHr(wicPalette->InitializeCustom(
                  const_cast<WICColor*>(image.palette.data()),
                  static_cast<UINT>(image.palette.size())),
              "IWICPalette::InitializeCustom");

    ComPtr<IStream> memoryStream;
    RequireHr(CreateStreamOnHGlobal(nullptr, TRUE, memoryStream.GetAddressOf()),
              "CreateStreamOnHGlobal");
    ComPtr<IWICStream> wicStream;
    RequireHr(factory->CreateStream(wicStream.GetAddressOf()),
              "IWICImagingFactory::CreateStream");
    RequireHr(wicStream->InitializeFromIStream(memoryStream.Get()),
              "IWICStream::InitializeFromIStream");

    ComPtr<IWICBitmapEncoder> encoder;
    RequireHr(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr,
                                     encoder.GetAddressOf()),
              "IWICImagingFactory::CreateEncoder(GUID_ContainerFormatPng)");
    RequireHr(encoder->Initialize(wicStream.Get(), WICBitmapEncoderNoCache),
              "IWICBitmapEncoder::Initialize");

    ComPtr<IWICBitmapFrameEncode> frame;
    ComPtr<IPropertyBag2> options;
    RequireHr(encoder->CreateNewFrame(frame.GetAddressOf(),
                                      options.GetAddressOf()),
              "IWICBitmapEncoder::CreateNewFrame");
    RequireHr(frame->Initialize(options.Get()),
              "IWICBitmapFrameEncode::Initialize");
    RequireHr(frame->SetSize(image.width, image.height),
              "IWICBitmapFrameEncode::SetSize");

    WICPixelFormatGUID format = GUID_WICPixelFormat8bppIndexed;
    RequireHr(frame->SetPixelFormat(&format),
              "IWICBitmapFrameEncode::SetPixelFormat");
    Require(IsEqualGUID(format, GUID_WICPixelFormat8bppIndexed),
            "the WIC PNG encoder did not accept GUID_WICPixelFormat8bppIndexed");

    RequireHr(frame->SetPalette(wicPalette.Get()),
              "IWICBitmapFrameEncode::SetPalette");

    const UINT stride = image.width;  // one byte per palette index
    RequireHr(frame->WritePixels(image.height, stride, stride * image.height,
                                 const_cast<BYTE*>(image.indices.data())),
              "IWICBitmapFrameEncode::WritePixels");
    RequireHr(frame->Commit(), "IWICBitmapFrameEncode::Commit");
    RequireHr(encoder->Commit(), "IWICBitmapEncoder::Commit");

    HGLOBAL global = nullptr;
    RequireHr(GetHGlobalFromStream(memoryStream.Get(), &global),
              "GetHGlobalFromStream");
    const SIZE_T allocated = GlobalSize(global);
    Require(allocated > 0, "the encoded in-memory stream is empty");
    void* base = GlobalLock(global);
    Require(base != nullptr, "GlobalLock on the encoded stream failed");
    const unsigned char* bytes = static_cast<unsigned char*>(base);
    std::vector<unsigned char> raw(bytes, bytes + allocated);
    (void)GlobalUnlock(global);

    // The HGLOBAL stream grows geometrically, so GlobalSize may exceed the
    // real PNG byte count. ParsePng below finds the exact IEND end; we trim the
    // trailing slack before handing the bytes to lodepng.
    return raw;
}

// ---------------------------------------------------------------------------
// Step 6 helpers: path conversion, scratch directory, baseline process.
// ---------------------------------------------------------------------------
std::wstring Widen(const std::string& text) {
    if (text.empty()) return std::wstring();
    const int length = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1,
                                           nullptr, 0);
    if (length <= 0) return std::wstring();
    std::wstring wide(static_cast<std::size_t>(length), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, &wide[0], length);
    wide.resize(static_cast<std::size_t>(length - 1));  // drop the terminator
    return wide;
}

std::string Narrow(const std::wstring& text) {
    if (text.empty()) return std::string();
    const int length = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), -1,
                                           nullptr, 0, nullptr, nullptr);
    if (length <= 0) return std::string();
    std::string narrow(static_cast<std::size_t>(length), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.c_str(), -1, &narrow[0], length,
                        nullptr, nullptr);
    narrow.resize(static_cast<std::size_t>(length - 1));
    return narrow;
}

std::string JoinPath(const std::string& dir, const char* leaf) {
    if (dir.empty()) return std::string(leaf);
    const char last = dir.back();
    if (last == '\\' || last == '/') return dir + leaf;
    return dir + "\\" + leaf;
}

bool EnsureDirectory(const std::string& path, std::string& error) {
    if (path.empty()) {
        error = "the work directory is empty";
        return false;
    }
    if (CreateDirectoryW(Widen(path).c_str(), nullptr) != FALSE) return true;
    if (GetLastError() == ERROR_ALREADY_EXISTS) return true;
    error = "cannot create the work directory: " + path;
    return false;
}

bool RunBaselineTool(const std::string& executable, const std::string& input,
                     const std::string& output, std::string& error) {
    if (GetFileAttributesW(Widen(executable).c_str()) == INVALID_FILE_ATTRIBUTES) {
        error = "the baseline executable is missing: " + executable;
        return false;
    }
    // 6.4: the colour count is a positional argument, not an option.
    std::string command = "\"" + executable +
        "\" --force --quality=0-100 --speed 1 --nofs --output \"" + output +
        "\" 256 -- \"" + input + "\"";
    std::wstring wide = Widen(command);
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (CreateProcessW(nullptr, &wide[0], nullptr, nullptr, FALSE,
                       CREATE_NO_WINDOW, nullptr, nullptr, &startup,
                       &process) == FALSE) {
        error = "the baseline process could not be launched";
        return false;
    }
    WaitForSingleObject(process.hProcess, INFINITE);
    DWORD code = 1u;
    GetExitCodeProcess(process.hProcess, &code);
    CloseHandle(process.hProcess);
    CloseHandle(process.hThread);
    if (code != 0u) {
        error = "the baseline process exited with code " + std::to_string(code);
        return false;
    }
    return true;
}

bool LoadIndexedPng(const std::string& path, IndexedImage& image,
                    std::string& error) {
    std::vector<unsigned char> bytes;
    if (lodepng::load_file(bytes, path) != 0u) {
        error = "cannot read the baseline output: " + path;
        return false;
    }
    lodepng::State state;
    std::vector<unsigned char> rgba;
    unsigned width = 0;
    unsigned height = 0;
    unsigned err = lodepng::decode(rgba, width, height, state, bytes);
    if (err != 0u) {
        error = std::string("baseline output decode failed: ") +
                lodepng_error_text(err);
        return false;
    }
    if (state.info_png.color.colortype != LCT_PALETTE ||
        state.info_png.color.bitdepth != 8u) {
        error = "the baseline output is not an 8-bit palette PNG";
        return false;
    }
    const std::size_t entries = state.info_png.color.palettesize;
    if (entries == 0u || entries > kPaletteSize) {
        error = "the baseline output palette is outside 1..256 entries";
        return false;
    }
    const unsigned char* palette = state.info_png.color.palette;
    if (palette == nullptr) {
        error = "the baseline output has no PLTE entries";
        return false;
    }
    image.palette.resize(entries);
    for (std::size_t i = 0; i < entries; ++i) {
        const unsigned r = palette[i * 4u + 0u];
        const unsigned g = palette[i * 4u + 1u];
        const unsigned b = palette[i * 4u + 2u];
        const unsigned a = palette[i * 4u + 3u];
        image.palette[i] = (a << 24) | (r << 16) | (g << 8) | b;
    }

    std::vector<unsigned char> indices;
    unsigned index_width = 0;
    unsigned index_height = 0;
    err = lodepng::decode(indices, index_width, index_height, bytes,
                          LCT_PALETTE, 8);
    if (err != 0u) {
        error = std::string("baseline index decode failed: ") +
                lodepng_error_text(err);
        return false;
    }
    if (index_width != width || index_height != height ||
        indices.size() != static_cast<std::size_t>(width) * height) {
        error = "the baseline index plane does not match its dimensions";
        return false;
    }
    image.width = width;
    image.height = height;
    image.indices.assign(indices.begin(), indices.end());
    return true;
}

// ---------------------------------------------------------------------------
// Minimal, self-contained PNG chunk parser: IHDR / PLTE / tRNS / IDAT / IEND,
// each with length, type, data and a verified CRC-32. This deliberately does
// NOT depend on lodepng exposing raw chunks; it is an independent structural
// view used for the step-5 assertions.
// ---------------------------------------------------------------------------
unsigned ReadBe32(const unsigned char* p) {
    return (static_cast<unsigned>(p[0]) << 24) |
           (static_cast<unsigned>(p[1]) << 16) |
           (static_cast<unsigned>(p[2]) << 8) |
           static_cast<unsigned>(p[3]);
}

unsigned PngCrc32(const unsigned char* data, std::size_t n) {
    unsigned crc = 0xFFFFFFFFu;
    for (std::size_t i = 0; i < n; ++i) {
        crc ^= static_cast<unsigned>(data[i]);
        for (int bit = 0; bit < 8; ++bit) {
            const unsigned mask = 0u - (crc & 1u);
            crc = (crc >> 1) ^ (0xEDB88320u & mask);
        }
    }
    return crc ^ 0xFFFFFFFFu;
}

struct PngInfo {
    bool signature_ok = false;
    bool ihdr_ok = false;
    unsigned width = 0;
    unsigned height = 0;
    unsigned bit_depth = 0;
    unsigned color_type = 0;
    unsigned interlace = 0;
    bool plte_present = false;
    std::vector<unsigned char> plte;  // RGB triples
    bool trns_present = false;
    std::vector<unsigned char> trns;  // one alpha byte per entry
    bool idat_present = false;
    std::size_t idat_bytes = 0;
    bool iend_present = false;
    std::size_t iend_end = 0;  // byte offset just past the IEND CRC
    std::size_t chunk_count = 0;
    bool all_crc_ok = true;
    std::string error;
};

PngInfo ParsePng(const std::vector<unsigned char>& png) {
    PngInfo info;
    static const unsigned char kSignature[8] = {137, 80, 78, 71, 13, 10, 26, 10};
    if (png.size() < 8 || std::memcmp(png.data(), kSignature, 8) != 0) {
        info.error = "PNG signature missing or invalid";
        return info;
    }
    info.signature_ok = true;

    std::size_t pos = 8;
    while (pos + 8 <= png.size()) {
        const std::size_t remaining = png.size() - pos;
        if (remaining < 12) {
            info.error = "truncated chunk header";
            return info;
        }
        const unsigned length = ReadBe32(png.data() + pos);
        if (static_cast<std::size_t>(length) > remaining - 12) {
            info.error = "chunk length exceeds the remaining bytes";
            return info;
        }
        const char* type = reinterpret_cast<const char*>(png.data() + pos + 4);
        const unsigned char* data = png.data() + pos + 8;
        const unsigned stored_crc = ReadBe32(png.data() + pos + 8 + length);
        // CRC is computed over the 4 type bytes followed by the data bytes;
        // those two regions are contiguous in the file.
        const unsigned computed_crc =
            PngCrc32(png.data() + pos + 4, 4u + static_cast<std::size_t>(length));
        if (computed_crc != stored_crc) info.all_crc_ok = false;
        ++info.chunk_count;

        const std::string tag(type, 4);
        if (tag == "IHDR") {
            if (length != 13) {
                info.error = "IHDR length is not 13";
                return info;
            }
            info.width = ReadBe32(data + 0);
            info.height = ReadBe32(data + 4);
            info.bit_depth = data[8];
            info.color_type = data[9];
            info.interlace = data[12];
            info.ihdr_ok = true;
        } else if (tag == "PLTE") {
            info.plte.assign(data, data + length);
            info.plte_present = true;
        } else if (tag == "tRNS") {
            info.trns.assign(data, data + length);
            info.trns_present = true;
        } else if (tag == "IDAT") {
            info.idat_present = true;
            info.idat_bytes += length;
        } else if (tag == "IEND") {
            info.iend_present = true;
        }

        pos += static_cast<std::size_t>(length) + 12;  // len + type + data + crc
        if (info.iend_present) {
            info.iend_end = pos;
            break;
        }
    }
    if (!info.iend_present && info.error.empty()) {
        info.error = "IEND chunk not found";
    }
    return info;
}

// ---------------------------------------------------------------------------
// Step-5 comparison helpers
// ---------------------------------------------------------------------------
bool PlteRgbMatches(const PngInfo& info, const Palette& palette) {
    if (info.plte.size() != static_cast<std::size_t>(kPaletteSize) * 3u) return false;
    for (std::size_t i = 0; i < kPaletteSize; ++i) {
        if (static_cast<unsigned>(info.plte[i * 3 + 0]) != RedOf(palette[i]) ||
            static_cast<unsigned>(info.plte[i * 3 + 1]) != GreenOf(palette[i]) ||
            static_cast<unsigned>(info.plte[i * 3 + 2]) != BlueOf(palette[i])) {
            return false;
        }
    }
    return true;
}

bool TrnsMatches(const PngInfo& info, const Palette& palette) {
    if (!info.trns_present) return false;
    if (info.trns.size() != static_cast<std::size_t>(kPaletteSize)) return false;
    for (std::size_t i = 0; i < kPaletteSize; ++i) {
        if (static_cast<unsigned>(info.trns[i]) != AlphaOf(palette[i])) return false;
    }
    return true;
}

bool DecodedIndicesMatch(const std::vector<unsigned char>& decoded,
                         unsigned w, unsigned h,
                         const std::vector<BYTE>& expected) {
    if (w != kWidth || h != kHeight) return false;
    if (decoded.size() != expected.size()) return false;
    for (std::size_t i = 0; i < expected.size(); ++i) {
        if (decoded[i] != expected[i]) return false;
    }
    return true;
}

bool DecodedAlphaMatches(const std::vector<unsigned char>& rgba,
                         unsigned w, unsigned h, const Palette& palette,
                         const std::vector<BYTE>& expected) {
    if (w != kWidth || h != kHeight) return false;
    if (rgba.size() != expected.size() * 4u) return false;
    for (std::size_t i = 0; i < expected.size(); ++i) {
        if (static_cast<unsigned>(rgba[i * 4 + 3]) !=
            AlphaOf(palette[expected[i]])) {
            return false;
        }
    }
    return true;
}

bool DecodedRgbMatches(const std::vector<unsigned char>& rgba,
                       unsigned w, unsigned h, const Palette& palette,
                       const std::vector<BYTE>& expected) {
    if (w != kWidth || h != kHeight) return false;
    if (rgba.size() != expected.size() * 4u) return false;
    for (std::size_t i = 0; i < expected.size(); ++i) {
        const WICColor c = palette[expected[i]];
        if (static_cast<unsigned>(rgba[i * 4 + 0]) != RedOf(c) ||
            static_cast<unsigned>(rgba[i * 4 + 1]) != GreenOf(c) ||
            static_cast<unsigned>(rgba[i * 4 + 2]) != BlueOf(c)) {
            return false;
        }
    }
    return true;
}

// lodepng independently parses PLTE + tRNS into info_png.color.palette (RGBA,
// 4 bytes per entry). Cross-check that view too.
bool LodepngPaletteAlphaMatches(const lodepng::State& state,
                                const Palette& palette) {
    if (state.info_png.color.colortype != LCT_PALETTE) return false;
    if (state.info_png.color.palettesize !=
        static_cast<std::size_t>(kPaletteSize)) {
        return false;
    }
    const unsigned char* p = state.info_png.color.palette;
    if (p == nullptr) return false;
    for (std::size_t i = 0; i < kPaletteSize; ++i) {
        if (static_cast<unsigned>(p[i * 4 + 3]) != AlphaOf(palette[i])) {
            return false;
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
// Structured assertion reporter
// ---------------------------------------------------------------------------
class Reporter {
public:
    void Check(bool condition, const std::string& name,
               const std::string& detail) {
        std::printf("ASSERT %s: %s (%s)\n", name.c_str(),
                    condition ? "PASS" : "FAIL", detail.c_str());
        if (!condition) ok_ = false;
        ++count_;
    }
    bool ok() const { return ok_; }
    std::size_t count() const { return count_; }

private:
    bool ok_ = true;
    std::size_t count_ = 0;
};

// ---------------------------------------------------------------------------
// Step 6: the baseline tool quantizes a real fixture, then its palette and
// indices are re-encoded by WIC. Both encoders therefore write the same
// indexed image, so the byte counts compare encoder efficiency only.
// ---------------------------------------------------------------------------
void RunSizeStep(const Options& options, Reporter& rep) {
    Require(!options.baseline.empty() && !options.image.empty(),
            "step 6 needs both --baseline and --image");

    std::string error;
    Require(EnsureDirectory(options.work_dir, error), error);
    const std::string source_path =
        JoinPath(options.work_dir, "sq_wic_size_source.png");
    const std::string baseline_path =
        JoinPath(options.work_dir, "sq_wic_size_baseline.png");

    std::vector<unsigned char> image_bytes;
    Require(lodepng::load_file(image_bytes, options.image) == 0u,
            "cannot read the fixture: " + options.image);
    lodepng::State state;
    std::vector<unsigned char> rgba;
    unsigned width = 0;
    unsigned height = 0;
    unsigned err = lodepng::decode(rgba, width, height, state, image_bytes);
    Require(err == 0u, std::string("fixture decode failed: ") +
                           lodepng_error_text(err));

    std::vector<unsigned char> source_png;
    err = lodepng::encode(source_png, rgba, width, height);
    Require(err == 0u, std::string("fixture re-encode failed: ") +
                           lodepng_error_text(err));
    Require(lodepng::save_file(source_png, source_path) == 0u,
            "cannot write the step 6 source PNG: " + source_path);

    Require(RunBaselineTool(options.baseline, source_path, baseline_path,
                            error),
            error);

    IndexedImage baseline;
    Require(LoadIndexedPng(baseline_path, baseline, error), error);
    const std::vector<unsigned char> wic = EncodeWithWic(baseline);

    WIN32_FILE_ATTRIBUTE_DATA attributes{};
    Require(GetFileAttributesExW(Widen(baseline_path).c_str(),
                                 GetFileExInfoStandard,
                                 &attributes) != FALSE,
            "cannot read the baseline output size");
    const std::size_t baseline_bytes =
        (static_cast<std::size_t>(attributes.nFileSizeHigh) << 32) |
        static_cast<std::size_t>(attributes.nFileSizeLow);
    Require(baseline_bytes > 0u, "the baseline output is empty");

    const double ratio = static_cast<double>(wic.size()) /
                         static_cast<double>(baseline_bytes);
    std::printf("STEP6 SOURCE: path=%s dims=%ux%u palette=%zu "
                "source_bytes=%zu\n",
                options.image.c_str(), baseline.width, baseline.height,
                baseline.palette.size(), source_png.size());
    std::printf("STEP6 SIZE: baseline_bytes=%zu wic_bytes=%zu ratio=%.4f "
                "max_colors=%u dither=%s\n",
                baseline_bytes, wic.size(), ratio, kMaxColors, kDitherMode);
    rep.Check(wic.size() * 100u <= baseline_bytes * 115u,
              "WIC PNG size <= 1.15x baseline",
              "wic=" + std::to_string(wic.size()) +
                  " baseline=" + std::to_string(baseline_bytes) +
                  " ratio=" + std::to_string(ratio));
}

// ---------------------------------------------------------------------------
// The six-step proof of concept
// ---------------------------------------------------------------------------
int RunPoc(const Options& options) {
    ComScope com;
    if (!com.ok()) {
        std::printf("FATAL: CoInitializeEx failed (HRESULT 0x%08lX)\n",
                    static_cast<unsigned long>(com.hr()));
        std::printf("RESULT: FAILED (steps 1-5); PoC NOT fully passed\n");
        return 1;
    }

    try {
        const Palette palette = BuildPalette();
        std::vector<BYTE> indices(static_cast<std::size_t>(kWidth) * kHeight);
        for (std::size_t i = 0; i < indices.size(); ++i) {
            indices[i] = static_cast<BYTE>(i);  // use every one of the 256 entries
        }

        // --- Step 1 -------------------------------------------------------
        const unsigned distinct_alphas = CountDistinctAlphas(palette);
        std::printf("STEP1 PALETTE: entries=%u distinct_alpha=%u "
                    "alpha[0]=0x%02X alpha[255]=0x%02X\n",
                    kPaletteSize, distinct_alphas, AlphaOf(palette[0]),
                    AlphaOf(palette[kPaletteSize - 1]));

        // --- Step 2 -------------------------------------------------------
        std::printf("STEP2 FRAME: format=GUID_WICPixelFormat8bppIndexed "
                    "size=%ux%u index_bytes=%zu SetPalette=WritePixels\n",
                    kWidth, kHeight, indices.size());

        // --- Step 3 -------------------------------------------------------
        IndexedImage synthetic;
        synthetic.width = kWidth;
        synthetic.height = kHeight;
        synthetic.palette.assign(palette.begin(), palette.end());
        synthetic.indices = indices;
        const std::vector<unsigned char> raw = EncodeWithWic(synthetic);
        const PngInfo info = ParsePng(raw);
        Require(info.error.empty(), "PNG chunk parser: " + info.error);
        Require(info.iend_present && info.iend_end > 0u,
                "the encoded bytes do not contain a complete PNG stream");
        std::vector<unsigned char> encoded(
            raw.data(), raw.data() + info.iend_end);
        std::printf("STEP3 ENCODE: stream=in-memory IWICStream/HGLOBAL "
                    "container=GUID_ContainerFormatPng png_bytes=%zu\n",
                    encoded.size());

        // --- Step 4: independent lodepng decoder --------------------------
        // Pass A: decode to LCT_PALETTE/8, which preserves the raw indices.
        std::vector<unsigned char> decodedIndices;
        unsigned indexW = 0;
        unsigned indexH = 0;
        unsigned err = lodepng::decode(decodedIndices, indexW, indexH, encoded,
                                       LCT_PALETTE, 8);
        Require(err == 0u, std::string("lodepng index decode failed: ") +
                              lodepng_error_text(err));
        // Pass B: decode with a State (default output RGBA/8) so the decoded
        // alpha and lodepng's own palette view are both available.
        lodepng::State state;
        std::vector<unsigned char> rgba;
        unsigned rgbaW = 0;
        unsigned rgbaH = 0;
        err = lodepng::decode(rgba, rgbaW, rgbaH, state, encoded);
        Require(err == 0u, std::string("lodepng RGBA decode failed: ") +
                              lodepng_error_text(err));
        std::printf("STEP4 DECODE: decoder=lodepng(independent) "
                    "index=%ux%u rgba=%ux%u lodepng_color_type=%d\n",
                    indexW, indexH, rgbaW, rgbaH,
                    static_cast<int>(state.info_png.color.colortype));

        // --- Step 5: assertions ------------------------------------------
        Reporter rep;
        const std::size_t plte_entries = info.plte.size() / 3u;

        rep.Check(info.signature_ok && info.iend_present && info.all_crc_ok,
                  "PNG signature / IEND / chunk CRCs",
                  "chunks=" + std::to_string(info.chunk_count) +
                      ", crc_ok=" + YesNo(info.all_crc_ok));
        rep.Check(info.ihdr_ok && info.color_type == 3u,
                  "IHDR colour type 3",
                  "got " + std::to_string(info.color_type) + ", expected 3");
        rep.Check(info.ihdr_ok && info.bit_depth == 8u,
                  "IHDR bit depth 8",
                  "got " + std::to_string(info.bit_depth) + ", expected 8");
        rep.Check(info.ihdr_ok && info.width == kWidth &&
                      info.height == kHeight && info.interlace == 0u,
                  "IHDR dimensions 16x16 non-interlaced",
                  std::to_string(info.width) + "x" +
                      std::to_string(info.height) +
                      ", interlace=" + std::to_string(info.interlace));
        rep.Check(info.idat_present && info.idat_bytes > 0u,
                  "IDAT present",
                  "bytes=" + std::to_string(info.idat_bytes));
        rep.Check(info.plte_present &&
                      plte_entries == static_cast<std::size_t>(kPaletteSize),
                  "PLTE entry count 256",
                  "got " + std::to_string(plte_entries) +
                      ", expected 256");
        rep.Check(PlteRgbMatches(info, palette),
                  "PLTE RGB entries match WIC palette",
                  "checked " + std::to_string(kPaletteSize) + " entries");
        rep.Check(info.trns_present,
                  "tRNS present",
                  YesNo(info.trns_present));
        rep.Check(info.trns_present &&
                      info.trns.size() == static_cast<std::size_t>(kPaletteSize),
                  "tRNS one alpha byte per entry",
                  "got " + std::to_string(info.trns.size()) +
                      ", expected 256");
        rep.Check(TrnsMatches(info, palette),
                  "tRNS alpha bytes match WIC palette",
                  "checked " + std::to_string(kPaletteSize) + " bytes");
        rep.Check(DecodedIndicesMatch(decodedIndices, indexW, indexH, indices),
                  "decoded pixel indices match",
                  "checked " + std::to_string(indices.size()) + " pixels");
        rep.Check(DecodedAlphaMatches(rgba, rgbaW, rgbaH, palette, indices),
                  "decoded alpha matches",
                  "checked " + std::to_string(indices.size()) + " pixels");
        rep.Check(DecodedRgbMatches(rgba, rgbaW, rgbaH, palette, indices),
                  "decoded RGB matches palette",
                  "checked " + std::to_string(indices.size()) + " pixels");
        rep.Check(state.info_png.color.colortype == LCT_PALETTE,
                  "lodepng info_png colour type 3",
                  "got " + std::to_string(
                               static_cast<int>(state.info_png.color.colortype)));
        rep.Check(state.info_png.color.palettesize ==
                      static_cast<std::size_t>(kPaletteSize),
                  "lodepng info_png palette size 256",
                  "got " + std::to_string(state.info_png.color.palettesize));
        rep.Check(LodepngPaletteAlphaMatches(state, palette),
                  "lodepng info_png palette alpha matches",
                  "cross-check of the independent decoder view");

        if (!rep.ok()) {
            std::printf("ASSERT_COUNT: %zu\n", rep.count());
            std::printf("RESULT: FAILED at step 5 (assertion failure above)\n");
            std::printf("FULL_PASS: NO\n");
            return 1;
        }

        if (options.baseline.empty() && options.image.empty()) {
            std::printf("STEP6 SIZE: wic_png_bytes=%zu max_colors=%u "
                        "dither=%s baseline=absent\n",
                        encoded.size(), kMaxColors, kDitherMode);
            std::printf("SIZE_BASELINE_BLOCKED: no --baseline/--image "
                        "supplied\n");
            std::printf("ASSERT_COUNT: %zu\n", rep.count());
            std::printf("RESULT: STEPS_1_5_PASS (step 6 inputs absent); "
                        "PoC NOT fully passed\n");
            std::printf("FULL_PASS: NO\n");
            return 0;
        }

        // --- Step 6: size baseline ---------------------------------------
        RunSizeStep(options, rep);
        std::printf("ASSERT_COUNT: %zu\n", rep.count());
        if (!rep.ok()) {
            std::printf("RESULT: FAILED at step 6 (assertion failure above)\n");
            std::printf("FULL_PASS: NO\n");
            return 1;
        }
        std::printf("RESULT: PASS (steps 1-6)\n");
        std::printf("FULL_PASS: YES\n");
        return 0;
    } catch (const PocFailure& failure) {
        std::printf("FATAL: %s\n", failure.message.c_str());
        std::printf("RESULT: FAILED (steps 1-5); PoC NOT fully passed\n");
        std::printf("FULL_PASS: NO\n");
        return 1;
    }
}

}  // namespace

int main(int argc, char** argv) {
    Options options;
    for (int i = 1; i < argc; ++i) {
        const char* arg = argv[i];
        if (std::strcmp(arg, "--selfcheck") == 0) {
            continue;
        } else if (std::strcmp(arg, "--baseline") == 0 && i + 1 < argc) {
            options.baseline = argv[++i];
        } else if (std::strcmp(arg, "--image") == 0 && i + 1 < argc) {
            options.image = argv[++i];
        } else if (std::strcmp(arg, "--work-dir") == 0 && i + 1 < argc) {
            options.work_dir = argv[++i];
        } else {
            std::printf("Usage: quantize_wic_poc --selfcheck "
                        "[--baseline <exe> --image <png> "
                        "[--work-dir <dir>]]\n");
            return 2;
        }
    }
    if (options.work_dir.empty()) {
        wchar_t temp[MAX_PATH + 1] = {};
        const DWORD length = GetTempPathW(MAX_PATH, temp);
        if (length == 0u || length > MAX_PATH) {
            std::printf("FATAL: GetTempPathW failed\n");
            return 2;
        }
        options.work_dir =
            JoinPath(Narrow(std::wstring(temp, length)), "sq_wic_size");
    }
    return RunPoc(options);
}
