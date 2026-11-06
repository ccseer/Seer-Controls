#include "quantizerunner.h"

#include <windows.h>
#include <propvarutil.h>
#include <shellapi.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

#include "lodepng.h"
#include "seer_quantize.h"
#include "sq_deflate.h"
#include "winexit.h"
#include "winpath.h"

namespace iq {
namespace {

using Microsoft::WRL::ComPtr;

constexpr wchar_t kPrefix[] = L"image-quantize: ";
constexpr std::size_t kMaxMessageChars = 900u;
constexpr unsigned kAutoNameAttempts = 1000u;
// The viewer clamps --wait-timeout into the same window, so a value outside it
// would silently mean something else on the viewer side.
constexpr std::uint32_t kMinViewerWaitMs = 5000u;
constexpr std::uint32_t kMaxViewerWaitMs = 600000u;
constexpr std::uint32_t kDefaultViewerWaitMs = 300000u;

struct RgbaImage {
    std::uint32_t width = 0u;
    std::uint32_t height = 0u;
    std::vector<std::uint8_t> pixels;
};

bool same_path(const std::wstring& left, const std::wstring& right) {
    return _wcsicmp(left.c_str(), right.c_str()) == 0;
}

std::wstring extended_path(const std::wstring& path) {
    const std::wstring native = WinPath::nativeSeparators(path);
    if (native.rfind(L"\\\\?\\", 0u) == 0u || native.size() < 248u) {
        return native;
    }
    return WinPath::addExtendedPrefix(native);
}

std::wstring directory_of(const std::wstring& path) {
    const std::size_t slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? std::wstring() : path.substr(0u, slash);
}

std::wstring stem_of(const std::wstring& path) {
    const std::size_t slash = path.find_last_of(L"\\/");
    const std::size_t begin = slash == std::wstring::npos ? 0u : slash + 1u;
    const std::size_t dot = path.find_last_of(L'.');
    if (dot == std::wstring::npos || dot < begin) {
        return path.substr(begin);
    }
    return path.substr(begin, dot - begin);
}

unsigned short tiff_orientation(const std::vector<unsigned char>& data) {
    if (data.size() < 8u) {
        return 0u;
    }
    const bool little = data[0] == 'I' && data[1] == 'I';
    const bool big = data[0] == 'M' && data[1] == 'M';
    if (!little && !big) {
        return 0u;
    }
    const auto read16 = [&](std::size_t at) {
        return static_cast<unsigned>(little
            ? (data[at] | (data[at + 1u] << 8u))
            : ((data[at] << 8u) | data[at + 1u]));
    };
    const auto read32 = [&](std::size_t at) {
        return little
            ? (static_cast<unsigned>(data[at])
               | (static_cast<unsigned>(data[at + 1u]) << 8u)
               | (static_cast<unsigned>(data[at + 2u]) << 16u)
               | (static_cast<unsigned>(data[at + 3u]) << 24u))
            : ((static_cast<unsigned>(data[at]) << 24u)
               | (static_cast<unsigned>(data[at + 1u]) << 16u)
               | (static_cast<unsigned>(data[at + 2u]) << 8u)
               | static_cast<unsigned>(data[at + 3u]));
    };
    const unsigned offset = read32(4u);
    if (offset + 2u > data.size()) {
        return 0u;
    }
    const unsigned count = read16(offset);
    for (unsigned i = 0u; i < count; ++i) {
        const std::size_t entry = offset + 2u + i * 12u;
        if (entry + 12u > data.size()) {
            return 0u;
        }
        if (read16(entry) == 0x0112u) {
            if (read16(entry + 2u) != 3u || read32(entry + 4u) < 1u) {
                return 0u;
            }
            return static_cast<unsigned short>(read16(entry + 8u));
        }
    }
    return 0u;
}

// WIC does not surface System.Photo.Orientation for PNG eXIf in every build, so
// the chunk is parsed directly; other containers still use the WIC query.
unsigned short png_exif_orientation(const std::wstring& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        return 0u;
    }
    unsigned char signature[8]{};
    file.read(reinterpret_cast<char*>(signature), 8);
    static const unsigned char kPngSignature[8] = {0x89u, 'P', 'N', 'G',
                                                   0x0Du, 0x0Au, 0x1Au, 0x0Au};
    if (!file || std::memcmp(signature, kPngSignature, 8u) != 0) {
        return 0u;
    }
    for (;;) {
        unsigned char header[8]{};
        if (!file.read(reinterpret_cast<char*>(header), 8)) {
            return 0u;
        }
        const std::uint32_t length = (static_cast<std::uint32_t>(header[0]) << 24u)
                                     | (static_cast<std::uint32_t>(header[1]) << 16u)
                                     | (static_cast<std::uint32_t>(header[2]) << 8u)
                                     | static_cast<std::uint32_t>(header[3]);
        if (std::memcmp(header + 4u, "eXIf", 4u) == 0) {
            if (length > 65535u) {
                return 0u;
            }
            std::vector<unsigned char> data(length);
            if (length > 0u
                && !file.read(reinterpret_cast<char*>(data.data()), length)) {
                return 0u;
            }
            return tiff_orientation(data);
        }
        if (std::memcmp(header + 4u, "IEND", 4u) == 0) {
            return 0u;
        }
        file.seekg(static_cast<std::streamoff>(length) + 4, std::ios::cur);
    }
}

std::wstring hex_error(unsigned long value) {
    wchar_t buffer[16];
    std::swprintf(buffer, 16u, L"0x%08lX", value);
    return buffer;
}

std::wstring hr_error(HRESULT hr) {
    return hex_error(static_cast<unsigned long>(hr));
}

bool parse_uint(const wchar_t* text, std::uint32_t low, std::uint32_t high,
                std::uint32_t& value) {
    if (text == nullptr || *text == L'\0') {
        return false;
    }
    wchar_t* end = nullptr;
    const unsigned long parsed = std::wcstoul(text, &end, 10);
    if (end == text || *end != L'\0' || parsed < low || parsed > high) {
        return false;
    }
    value = static_cast<std::uint32_t>(parsed);
    return true;
}

bool parse_float(const wchar_t* text, float low, float high, float& value) {
    if (text == nullptr || *text == L'\0') {
        return false;
    }
    wchar_t* end = nullptr;
    const double parsed = std::wcstod(text, &end);
    if (end == text || *end != L'\0' || !std::isfinite(parsed)
        || parsed < low || parsed > high) {
        return false;
    }
    value = static_cast<float>(parsed);
    return true;
}

void assign(std::wstring& target, const wchar_t* value) {
    target.assign(value == nullptr ? L"" : value);
}

bool parse_options(int argc, wchar_t** argv, Options& options,
                   std::wstring& error) {
    for (int index = 1; index < argc; ++index) {
        const std::wstring_view name(argv[index]);
        const auto next = [&](const wchar_t*& value) {
            if (index + 1 >= argc) {
                return false;
            }
            value = argv[++index];
            return true;
        };
        const wchar_t* value = nullptr;
        if (name == L"--input") {
            if (!next(value)) {
                error = L"--input requires a path";
                return false;
            }
            assign(options.input, value);
        } else if (name == L"--output") {
            if (!next(value)) {
                error = L"--output requires a path";
                return false;
            }
            assign(options.output, value);
        } else if (name == L"--log") {
            if (!next(value)) {
                error = L"--log requires a path";
                return false;
            }
            assign(options.log_path, value);
        } else if (name == L"--overwrite") {
            options.overwrite = true;
        } else if (name == L"--colors") {
            if (!next(value) || !parse_uint(value, 2u, SQ_MAX_PALETTE_ENTRIES,
                                           options.colors)) {
                error = L"--colors must be an integer in [2, 256]";
                return false;
            }
        } else if (name == L"--dither") {
            if (!next(value)) {
                error = L"--dither requires none or fs";
                return false;
            }
            if (std::wcscmp(value, L"none") == 0) {
                options.dither = false;
            } else if (std::wcscmp(value, L"fs") == 0) {
                options.dither = true;
            } else {
                error = L"--dither must be none or fs";
                return false;
            }
        } else if (name == L"--dither-strength") {
            if (!next(value) || !parse_float(value, 0.0f, 1.0f,
                                             options.strength)) {
                error = L"--dither-strength must be a number in [0, 1]";
                return false;
            }
        } else if (name == L"--space") {
            if (!next(value)) {
                error = L"--space requires srgb or oklab";
                return false;
            }
            if (std::wcscmp(value, L"srgb") == 0) {
                options.oklab = false;
            } else if (std::wcscmp(value, L"oklab") == 0) {
                options.oklab = true;
            } else {
                error = L"--space must be srgb or oklab";
                return false;
            }
        } else if (name == L"--pipeline") {
            if (!next(value)) {
                error = L"--pipeline requires auto, srgb, a or b";
                return false;
            }
            if (std::wcscmp(value, L"auto") == 0) {
                options.pipeline = SQ_PIPELINE_AUTO;
            } else if (std::wcscmp(value, L"srgb") == 0) {
                options.pipeline = SQ_PIPELINE_SRGB;
            } else if (std::wcscmp(value, L"a") == 0) {
                options.pipeline = SQ_PIPELINE_OKLAB_A;
            } else if (std::wcscmp(value, L"b") == 0) {
                options.pipeline = SQ_PIPELINE_OKLAB_B;
            } else {
                error = L"--pipeline must be auto, srgb, a or b";
                return false;
            }
        } else if (name == L"--kmeans-iter") {
            if (!next(value) || !parse_uint(value, 0u, 1000u,
                                            options.kmeans_iter)) {
                error = L"--kmeans-iter must be an integer in [0, 1000]";
                return false;
            }
        } else if (name == L"--deflate") {
            if (!next(value)) {
                error = L"--deflate requires fast, balanced or max";
                return false;
            }
            if (std::wcscmp(value, L"fast") == 0) {
                options.deflate = DeflateTier::kFast;
            } else if (std::wcscmp(value, L"balanced") == 0) {
                options.deflate = DeflateTier::kBalanced;
            } else if (std::wcscmp(value, L"max") == 0) {
                options.deflate = DeflateTier::kMax;
            } else {
                error = L"--deflate must be fast, balanced or max";
                return false;
            }
        } else if (name == L"--optimize") {
            std::uint32_t level = 0u;
            if (!next(value) || !parse_uint(value, 0u, 2u, level)) {
                error = L"--optimize must be an integer in [0, 2]";
                return false;
            }
            options.deflate = static_cast<DeflateTier>(level);
        } else if (name == L"--seerviewer") {
            if (!next(value)) {
                error = L"--seerviewer requires a path";
                return false;
            }
            options.seerviewer_exe = value;
        } else if (name.rfind(L"--wait-timeout=", 0u) == 0u) {
            const std::wstring number(name.substr(15u));
            if (!parse_uint(number.c_str(), kMinViewerWaitMs, kMaxViewerWaitMs,
                            options.wait_timeout_ms)) {
                error = L"--wait-timeout must be an integer in [5000, 600000]";
                return false;
            }
        } else if (name == L"--threads") {
            if (!next(value) || !parse_uint(value, 0u, 256u,
                                            options.threads)) {
                error = L"--threads must be an integer in [0, 256]";
                return false;
            }
        } else {
            error = std::wstring(L"unknown option ") + std::wstring(name);
            return false;
        }
    }
    if (options.input.empty()) {
        error = L"--input is required";
        return false;
    }
    return true;
}

bool choose_frame(IWICBitmapDecoder* decoder, UINT& index, std::wstring& error) {
    GUID container{};
    HRESULT hr = decoder->GetContainerFormat(&container);
    if (FAILED(hr)) {
        error = L"GetContainerFormat failed " + hr_error(hr);
        return false;
    }
    UINT count = 0u;
    hr = decoder->GetFrameCount(&count);
    if (FAILED(hr) || count == 0u) {
        error = L"the image has no frames";
        return false;
    }
    index = 0u;
    if (!IsEqualGUID(container, GUID_ContainerFormatIco)) {
        return true;
    }
    std::uint64_t best = 0u;
    for (UINT candidate = 0u; candidate < count; ++candidate) {
        ComPtr<IWICBitmapFrameDecode> frame;
        if (FAILED(decoder->GetFrame(candidate, frame.GetAddressOf()))) {
            continue;
        }
        UINT width = 0u;
        UINT height = 0u;
        if (FAILED(frame->GetSize(&width, &height))) {
            continue;
        }
        const std::uint64_t area =
            static_cast<std::uint64_t>(width) * height;
        if (area > best) {
            best = area;
            index = candidate;
        }
    }
    return true;
}

bool make_source(IWICImagingFactory* factory, IWICBitmapFrameDecode* frame,
                 ComPtr<IWICBitmapSource>& source, bool& sixteen_bit,
                 bool& icc_converted, std::wstring& error);

bool decode(const std::wstring& path, RgbaImage& image, int& code,
            std::wstring& error, bool& icc_converted) {
    icc_converted = false;
    ComPtr<IWICImagingFactory> factory;
    HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory2, nullptr,
                                  CLSCTX_INPROC_SERVER,
                                  IID_PPV_ARGS(factory.GetAddressOf()));
    if (FAILED(hr)) {
        hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr,
                              CLSCTX_INPROC_SERVER,
                              IID_PPV_ARGS(factory.GetAddressOf()));
    }
    if (FAILED(hr)) {
        code = WinExit::kMissingDependency;
        error = L"the WIC decoder is unavailable";
        return false;
    }

    ComPtr<IWICBitmapDecoder> decoder;
    hr = factory->CreateDecoderFromFilename(
        extended_path(path).c_str(), nullptr, GENERIC_READ,
        WICDecodeMetadataCacheOnLoad, decoder.GetAddressOf());
    if (FAILED(hr)) {
        code = hr == HRESULT_FROM_WIN32(ERROR_ACCESS_DENIED)
                   ? WinExit::kFailure
                   : WinExit::kUnsupported;
        error = L"cannot decode " + path + L" (" + hr_error(hr) + L")";
        return false;
    }

    UINT index = 0u;
    if (!choose_frame(decoder.Get(), index, error)) {
        code = WinExit::kUnsupported;
        return false;
    }

    ComPtr<IWICBitmapFrameDecode> frame;
    hr = decoder->GetFrame(index, frame.GetAddressOf());
    if (FAILED(hr)) {
        code = WinExit::kUnsupported;
        error = L"cannot read frame " + std::to_wstring(index);
        return false;
    }

    ComPtr<IWICBitmapSource> source;
    bool sixteen_bit = false;
    if (!make_source(factory.Get(), frame.Get(), source, sixteen_bit,
                     icc_converted, error)) {
        code = WinExit::kUnsupported;
        return false;
    }

    UINT width = 0u;
    UINT height = 0u;
    hr = source->GetSize(&width, &height);
    if (FAILED(hr) || width == 0u || height == 0u) {
        code = WinExit::kUnsupported;
        error = L"the decoded image is empty";
        return false;
    }
    if (width > SQ_MAX_DIMENSION || height > SQ_MAX_DIMENSION
        || static_cast<std::uint64_t>(width) * height > SQ_MAX_PIXELS) {
        code = WinExit::kUnsupported;
        error = L"the image exceeds the engine limits";
        return false;
    }

    ComPtr<IWICBitmap> bitmap;
    hr = factory->CreateBitmapFromSource(source.Get(), WICBitmapCacheOnLoad,
                                         bitmap.GetAddressOf());
    if (FAILED(hr)) {
        code = WinExit::kFailure;
        error = L"cannot materialize the decoded image " + hr_error(hr);
        return false;
    }

    // EXIF orientation is applied after the image is cached and before any
    // pixel is read, so quantization sees display orientation.
    ComPtr<IWICBitmapSource> oriented;
    oriented = bitmap;
    WICBitmapTransformOptions transform = WICBitmapTransformRotate0;
    unsigned short tag = png_exif_orientation(path);
    if (tag == 0u) {
        ComPtr<IWICMetadataQueryReader> reader;
        if (SUCCEEDED(frame->GetMetadataQueryReader(reader.GetAddressOf()))
            && reader != nullptr) {
            PROPVARIANT value;
            PropVariantInit(&value);
            if (SUCCEEDED(reader->GetMetadataByName(L"System.Photo.Orientation",
                                                    &value))) {
                if (value.vt == VT_UI2) {
                    tag = value.uiVal;
                } else if (value.vt == VT_UI4) {
                    tag = static_cast<unsigned short>(value.ulVal);
                } else if (value.vt == VT_I2) {
                    tag = static_cast<unsigned short>(value.iVal);
                }
            }
            PropVariantClear(&value);
        }
    }
    switch (tag) {
        case 2u:
            transform = WICBitmapTransformFlipHorizontal;
            break;
        case 3u:
            transform = WICBitmapTransformRotate180;
            break;
        case 4u:
            transform = WICBitmapTransformFlipVertical;
            break;
        case 5u:
            transform = static_cast<WICBitmapTransformOptions>(
                WICBitmapTransformRotate270 | WICBitmapTransformFlipHorizontal);
            break;
        case 6u:
            transform = WICBitmapTransformRotate90;
            break;
        case 7u:
            transform = static_cast<WICBitmapTransformOptions>(
                WICBitmapTransformRotate90 | WICBitmapTransformFlipHorizontal);
            break;
        case 8u:
            transform = WICBitmapTransformRotate270;
            break;
        default:
            break;
    }
    if (transform != WICBitmapTransformRotate0) {
        ComPtr<IWICBitmapFlipRotator> rotator;
        hr = factory->CreateBitmapFlipRotator(rotator.GetAddressOf());
        if (FAILED(hr)
            || FAILED(rotator->Initialize(bitmap.Get(), transform))
            || FAILED(rotator.As(&oriented))) {
            code = WinExit::kUnsupported;
            error = L"cannot apply the EXIF orientation";
            return false;
        }
        hr = oriented->GetSize(&width, &height);
        if (FAILED(hr) || width == 0u || height == 0u) {
            code = WinExit::kUnsupported;
            error = L"the oriented image is empty";
            return false;
        }
    }

    const UINT stride = width * 4u;
    image.width = width;
    image.height = height;
    image.pixels.resize(static_cast<std::size_t>(stride) * height);
    hr = oriented->CopyPixels(nullptr, stride,
                              static_cast<UINT>(image.pixels.size()),
                              image.pixels.data());
    if (FAILED(hr)) {
        code = WinExit::kFailure;
        error = L"cannot read the decoded pixels " + hr_error(hr);
        return false;
    }
    return true;
}

inline bool is_sixteen_bit_format(const WICPixelFormatGUID& format) {
    return IsEqualGUID(format, GUID_WICPixelFormat16bppGray)
        || IsEqualGUID(format, GUID_WICPixelFormat64bppRGBA)
        || IsEqualGUID(format, GUID_WICPixelFormat64bppPRGBA)
        || IsEqualGUID(format, GUID_WICPixelFormat64bppBGRA)
        || IsEqualGUID(format, GUID_WICPixelFormat64bppPBGRA)
        || IsEqualGUID(format, GUID_WICPixelFormat64bppRGB)
        || IsEqualGUID(format, GUID_WICPixelFormat48bppRGB)
        || IsEqualGUID(format, GUID_WICPixelFormat48bppBGR);
}

inline std::uint8_t reduce_16_to_8(std::uint16_t sample) {
    return static_cast<std::uint8_t>((sample + 128u) / 257u);
}

bool make_source(IWICImagingFactory* factory, IWICBitmapFrameDecode* frame,
                 ComPtr<IWICBitmapSource>& source, bool& sixteen_bit,
                 bool& icc_converted, std::wstring& error) {
    icc_converted = false;
    WICPixelFormatGUID format{};
    HRESULT hr = frame->GetPixelFormat(&format);
    if (FAILED(hr)) {
        error = L"cannot read the pixel format";
        return false;
    }
    const bool is_gray16 = IsEqualGUID(format, GUID_WICPixelFormat16bppGray);
    sixteen_bit = is_sixteen_bit_format(format);

    ComPtr<IWICBitmapSource> current;
    current.Attach(frame);
    frame->AddRef();

    UINT contexts = 0u;
    hr = frame->GetColorContexts(0u, nullptr, &contexts);
    if (SUCCEEDED(hr) && contexts > 0u) {
        if (sixteen_bit) {
            // Applying an ICC transform would force an 8-bit destination and
            // bypass the 257 reduction required for 16-bit input.
            error = L"a 16-bit image with an embedded profile is not supported";
            return false;
        }
        std::vector<ComPtr<IWICColorContext>> list(contexts);
        std::vector<IWICColorContext*> raw(contexts, nullptr);
        for (UINT i = 0u; i < contexts; ++i) {
            if (FAILED(factory->CreateColorContext(list[i].GetAddressOf()))) {
                error = L"cannot read the color profile";
                return false;
            }
            raw[i] = list[i].Get();
        }
        UINT loaded = 0u;
        hr = frame->GetColorContexts(contexts, raw.data(), &loaded);
        ComPtr<IWICColorContext> destination;
        ComPtr<IWICColorTransform> transform;
        ComPtr<IWICBitmapSource> converted;
        if (FAILED(hr) || loaded == 0u
            || FAILED(factory->CreateColorContext(destination.GetAddressOf()))
            || FAILED(destination->InitializeFromExifColorSpace(1u))
            || FAILED(factory->CreateColorTransformer(transform.GetAddressOf()))
            || FAILED(transform->Initialize(current.Get(), raw[0],
                                            destination.Get(),
                                            GUID_WICPixelFormat32bppRGBA))
            || FAILED(transform.As(&converted))) {
            // A profile that cannot be converted is rejected rather than
            // silently treated as sRGB.
            error = L"the embedded color profile is not convertible to sRGB";
            return false;
        }
        current = converted;
        icc_converted = true;
    }

    if (is_gray16) {
        ComPtr<IWICBitmap> bitmap;
        hr = factory->CreateBitmapFromSource(current.Get(), WICBitmapCacheOnLoad,
                                             bitmap.GetAddressOf());
        if (FAILED(hr)) {
            error = L"cannot materialize the 16-bit gray image";
            return false;
        }
        UINT width = 0u;
        UINT height = 0u;
        bitmap->GetSize(&width, &height);
        const UINT stride = width * 2u;
        std::vector<std::uint8_t> wide(static_cast<std::size_t>(stride) * height);
        hr = bitmap->CopyPixels(nullptr, stride, static_cast<UINT>(wide.size()),
                                wide.data());
        if (FAILED(hr)) {
            error = L"cannot read the 16-bit gray pixels";
            return false;
        }
        std::vector<std::uint8_t> narrow(static_cast<std::size_t>(width) * 4u
                                         * height);
        for (std::size_t i = 0u; i < static_cast<std::size_t>(width) * height; ++i) {
            const std::uint16_t sample =
                static_cast<std::uint16_t>(wide[i * 2u])
                | (static_cast<std::uint16_t>(wide[i * 2u + 1u]) << 8u);
            const std::uint8_t g = reduce_16_to_8(sample);
            narrow[i * 4u + 0u] = g;
            narrow[i * 4u + 1u] = g;
            narrow[i * 4u + 2u] = g;
            narrow[i * 4u + 3u] = 255u;
        }
        ComPtr<IWICBitmap> reduced;
        hr = factory->CreateBitmapFromMemory(
            width, height, GUID_WICPixelFormat32bppRGBA, width * 4u,
            static_cast<UINT>(narrow.size()), narrow.data(),
            reduced.GetAddressOf());
        if (FAILED(hr)) {
            error = L"cannot build the reduced 8-bit image";
            return false;
        }
        source = reduced;
        return true;
    }

    ComPtr<IWICFormatConverter> converter;
    hr = factory->CreateFormatConverter(converter.GetAddressOf());
    if (FAILED(hr)) {
        error = L"cannot create the format converter";
        return false;
    }
    const WICPixelFormatGUID target_format =
        sixteen_bit ? GUID_WICPixelFormat64bppRGBA : GUID_WICPixelFormat32bppRGBA;
    hr = converter->Initialize(current.Get(), target_format,
                               WICBitmapDitherTypeNone, nullptr, 0.0,
                               WICBitmapPaletteTypeCustom);
    if (FAILED(hr)) {
        error = L"cannot convert the image to RGBA8";
        return false;
    }
    if (sixteen_bit) {
        // WIC truncates 16-bit samples, so the 257 round happens here instead:
        // the reduction happens in helper-owned code below.
        ComPtr<IWICBitmap> bitmap;
        hr = factory->CreateBitmapFromSource(converter.Get(), WICBitmapCacheOnLoad,
                                            bitmap.GetAddressOf());
        if (FAILED(hr)) {
            error = L"cannot materialize the 16-bit image";
            return false;
        }
        UINT width = 0u;
        UINT height = 0u;
        bitmap->GetSize(&width, &height);
        const UINT stride = width * 8u;
        std::vector<std::uint8_t> wide(static_cast<std::size_t>(stride) * height);
        hr = bitmap->CopyPixels(nullptr, stride, static_cast<UINT>(wide.size()),
                                wide.data());
        if (FAILED(hr)) {
            error = L"cannot read the 16-bit pixels";
            return false;
        }
        std::vector<std::uint8_t> narrow(static_cast<std::size_t>(width) * 4u
                                         * height);
        for (std::size_t i = 0u; i < narrow.size(); ++i) {
            const std::uint16_t sample =
                static_cast<std::uint16_t>(wide[i * 2u])
                | (static_cast<std::uint16_t>(wide[i * 2u + 1u]) << 8u);
            narrow[i] = reduce_16_to_8(sample);
        }
        ComPtr<IWICBitmap> reduced;
        hr = factory->CreateBitmapFromMemory(
            width, height, GUID_WICPixelFormat32bppRGBA, width * 4u,
            static_cast<UINT>(narrow.size()), narrow.data(),
            reduced.GetAddressOf());
        if (FAILED(hr)) {
            error = L"cannot build the reduced 8-bit image";
            return false;
        }
        source = reduced;
        return true;
    }

    source = converter;
    return true;
}

// The custom context must outlive the lodepng call that reads it, so the
// iteration count lives at namespace scope rather than in a nested block.
constexpr unsigned kOptimalIterations = 6u;

unsigned optimal_deflate(unsigned char** out, std::size_t* outsize,
                         const unsigned char* in, std::size_t insize,
                         const LodePNGCompressSettings* settings) {
    const unsigned iterations =
        settings != nullptr && settings->custom_context != nullptr
            ? *static_cast<const unsigned*>(settings->custom_context)
            : kOptimalIterations;
    std::vector<std::uint8_t> packed;
    if (!sq_deflate::compress(in, insize, packed, iterations)) {
        return 1u;
    }
    auto* buffer = static_cast<unsigned char*>(std::malloc(packed.size()));
    if (buffer == nullptr) {
        return 83u;
    }
    std::memcpy(buffer, packed.data(), packed.size());
    *out = buffer;
    *outsize = packed.size();
    return 0u;
}

bool palette_state(const std::uint8_t* palette, std::uint16_t size,
                   unsigned filter_variant, lodepng::State& state) {
    state = lodepng::State{};
    state.info_raw.colortype = LCT_PALETTE;
    state.info_raw.bitdepth = 8u;
    state.info_png.color.colortype = LCT_PALETTE;
    state.info_png.color.bitdepth = size <= 2u   ? 1u
                                    : size <= 4u ? 2u
                                    : size <= 16u ? 4u
                                                  : 8u;
    state.encoder.auto_convert = 0u;
    state.encoder.zlibsettings.windowsize = 32768u;
    for (std::uint16_t i = 0u; i < size; ++i) {
        const std::uint8_t* entry = palette + i * 4u;
        unsigned code = lodepng_palette_add(&state.info_raw, entry[0], entry[1],
                                            entry[2], entry[3]);
        if (code == 0u) {
            code = lodepng_palette_add(&state.info_png.color, entry[0], entry[1],
                                       entry[2], entry[3]);
        }
        if (code != 0u) {
            return false;
        }
    }
    if (filter_variant == 0u) {
        state.encoder.filter_palette_zero = 1u;
        state.encoder.filter_strategy = LFS_ZERO;
    } else if (filter_variant == 1u) {
        state.encoder.filter_palette_zero = 0u;
        state.encoder.filter_strategy = LFS_MINSUM;
    } else {
        state.encoder.filter_palette_zero = 0u;
        state.encoder.filter_strategy = LFS_ENTROPY;
    }
    return true;
}

// Rewrites the palette order and the index plane. Non-opaque entries always
// come first: the tRNS chunk only has to cover the leading run of translucency,
// so putting them first keeps it at one byte instead of up to 256. Optionally
// sorts inside each group by perceptual luminance, which makes neighbouring
// colours sit at neighbouring byte values and gives Deflate smaller deltas.
void palette_relabel(const sq_result& result, std::size_t pixels, bool by_luminance,
                     std::vector<std::uint8_t>& palette,
                     std::vector<std::uint8_t>& indices) {
    const std::uint16_t count = result.palette_size;
    palette.assign(result.palette_rgba, result.palette_rgba + count * 4u);
    std::vector<std::uint16_t> order(count);
    for (std::uint16_t i = 0u; i < count; ++i) {
        order[i] = i;
    }
    const auto alpha = [&](std::uint16_t index) {
        return result.palette_rgba[static_cast<std::size_t>(index) * 4u + 3u];
    };
    const auto luminance = [&](std::uint16_t index) {
        const std::uint8_t* entry = result.palette_rgba + index * 4u;
        return 2126u * entry[0] + 7152u * entry[1] + 722u * entry[2];
    };
    // Without the luminance pass the engine's own order is kept inside each
    // group, so the cheapest tier only pays for the translucent-first move.
    std::stable_sort(order.begin(), order.end(),
                     [&](std::uint16_t left, std::uint16_t right) {
        const bool left_opaque = alpha(left) == 255u;
        const bool right_opaque = alpha(right) == 255u;
        if (left_opaque != right_opaque) {
            return !left_opaque;
        }
        if (!by_luminance) {
            return false;
        }
        const std::uint32_t left_luma = luminance(left);
        const std::uint32_t right_luma = luminance(right);
        if (left_luma != right_luma) {
            return left_luma < right_luma;
        }
        const std::uint8_t* a = result.palette_rgba + left * 4u;
        const std::uint8_t* b = result.palette_rgba + right * 4u;
        for (int c = 0; c < 4; ++c) {
            if (a[c] != b[c]) {
                return a[c] < b[c];
            }
        }
        return left < right;
    });
    std::vector<std::uint8_t> remap(count);
    for (std::uint16_t position = 0u; position < count; ++position) {
        remap[order[position]] = static_cast<std::uint8_t>(position);
        std::memcpy(&palette[static_cast<std::size_t>(position) * 4u],
                    &result.palette_rgba[static_cast<std::size_t>(order[position]) * 4u],
                    4u);
    }
    indices.resize(pixels);
    for (std::size_t i = 0u; i < pixels; ++i) {
        indices[i] = remap[result.indices[i]];
    }
}

bool encode_png(const RgbaImage& image, const sq_result& result,
                DeflateTier tier, std::vector<std::uint8_t>& bytes,
                std::wstring& error) {
    if (result.palette_size == 0u || result.palette_size > 256u) {
        error = L"the engine returned an invalid palette";
        return false;
    }
    const std::size_t pixels = static_cast<std::size_t>(image.width) * image.height;

    std::vector<std::uint8_t> plain_palette;
    std::vector<std::uint8_t> plain_indices;
    palette_relabel(result, pixels, false, plain_palette, plain_indices);
    std::vector<std::uint8_t> sorted_palette;
    std::vector<std::uint8_t> sorted_indices;
    if (tier != DeflateTier::kFast) {
        palette_relabel(result, pixels, true, sorted_palette, sorted_indices);
    }

    struct Variant {
        const std::vector<std::uint8_t>* palette;
        const std::vector<std::uint8_t>* indices;
        unsigned filter;
    };
    std::vector<Variant> variants;
    const unsigned filters = tier == DeflateTier::kFast ? 2u : 3u;
    for (unsigned filter = 0u; filter < filters; ++filter) {
        variants.push_back({&plain_palette, &plain_indices, filter});
    }
    if (tier != DeflateTier::kFast) {
        for (unsigned filter = 0u; filter < filters; ++filter) {
            variants.push_back({&sorted_palette, &sorted_indices, filter});
        }
    }

    const auto run_variant = [&](const Variant& variant, bool optimal,
                                 std::vector<std::uint8_t>& out) {
        lodepng::State state;
        if (!palette_state(variant.palette->data(),
                           static_cast<std::uint16_t>(variant.palette->size() / 4u),
                           variant.filter, state)) {
            error = L"the PNG palette could not be built";
            return false;
        }
        if (optimal) {
            state.encoder.zlibsettings.custom_deflate = &optimal_deflate;
            state.encoder.zlibsettings.custom_context = &kOptimalIterations;
        }
        const unsigned code = lodepng::encode(out, *variant.indices, image.width,
                                              image.height, state);
        return code == 0u;
    };

    std::vector<std::uint8_t> best;
    std::size_t best_variant = 0u;
    for (std::size_t index = 0u; index < variants.size(); ++index) {
        std::vector<std::uint8_t> candidate;
        if (!run_variant(variants[index], false, candidate)) {
            error = L"the PNG encoder failed";
            return false;
        }
        if (best.empty() || candidate.size() < best.size()) {
            best.swap(candidate);
            best_variant = index;
        }
    }

    if (tier == DeflateTier::kMax) {
        // The strongest deflate changes which filter wins, so re-encode the
        // two most promising variants with the optimal parser and keep the
        // smaller result. The lodepng result stays as the floor.
        const Variant* first = &variants[best_variant];
        const Variant* second = nullptr;
        for (std::size_t index = 0u; index < variants.size(); ++index) {
            if (index != best_variant && variants[index].filter == 0u) {
                second = &variants[index];
                break;
            }
        }
        for (const Variant* chosen : {first, second}) {
            if (chosen == nullptr) {
                continue;
            }
            std::vector<std::uint8_t> candidate;
            if (!run_variant(*chosen, true, candidate)) {
                error = L"the PNG encoder failed";
                return false;
            }
            if (candidate.size() < best.size()) {
                best.swap(candidate);
            }
        }
    }

    bytes.swap(best);
    return true;
}

bool write_all(HANDLE handle, const std::vector<std::uint8_t>& bytes) {
    std::size_t offset = 0u;
    while (offset < bytes.size()) {
        const DWORD chunk = static_cast<DWORD>(
            (bytes.size() - offset) > 0x10000000u ? 0x10000000u
                                                  : (bytes.size() - offset));
        DWORD written = 0u;
        if (!WriteFile(handle, bytes.data() + offset, chunk, &written, nullptr)
            || written == 0u) {
            return false;
        }
        offset += written;
    }
    return true;
}

void reclaim_orphans(const std::wstring& directory) {
    const std::wstring prefix = L".seer-quantize-";
    const std::wstring folder =
        directory.empty() ? std::wstring() : directory + L"\\";
    WIN32_FIND_DATAW data{};
    HANDLE find = FindFirstFileW(extended_path(folder + prefix + L"*.tmp").c_str(),
                                &data);
    if (find == INVALID_HANDLE_VALUE) {
        return;
    }
    FILETIME current{};
    GetSystemTimeAsFileTime(&current);
    const ULARGE_INTEGER now{current.dwLowDateTime, current.dwHighDateTime};
    do {
        const std::wstring name = data.cFileName;
        if (name.rfind(prefix, 0u) != 0u) {
            continue;
        }
        const std::size_t dash = name.find(L'-', prefix.size());
        if (dash == std::wstring::npos) {
            continue;
        }
        const unsigned long pid = std::wcstoul(
            name.substr(prefix.size(), dash - prefix.size()).c_str(), nullptr, 10);
        if (pid == 0u || pid == GetCurrentProcessId()) {
            continue;
        }
        const ULARGE_INTEGER stamp{data.ftLastWriteTime.dwLowDateTime,
                                   data.ftLastWriteTime.dwHighDateTime};
        if (now.QuadPart <= stamp.QuadPart
            || (now.QuadPart - stamp.QuadPart) < 36000000000ull) {
            continue;
        }
        HANDLE process = OpenProcess(SYNCHRONIZE, FALSE, pid);
        if (process != nullptr) {
            CloseHandle(process);
            continue;
        }
        if (GetLastError() != ERROR_INVALID_PARAMETER) {
            continue;
        }
        DeleteFileW(extended_path(folder + name).c_str());
    } while (FindNextFileW(find, &data));
    FindClose(find);
}

bool create_temp(const std::wstring& directory, std::wstring& path,
                 HANDLE& handle, int& code, std::wstring& error) {
    for (unsigned counter = 0u; counter < 100u; ++counter) {
        wchar_t name[64];
        std::swprintf(name, 64u, L".seer-quantize-%lu-%u.tmp",
                      GetCurrentProcessId(), counter);
        path = directory.empty() ? std::wstring(name)
                                 : directory + L"\\" + name;
        handle = CreateFileW(extended_path(path).c_str(), GENERIC_WRITE, 0u,
                             nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL,
                             nullptr);
        if (handle != INVALID_HANDLE_VALUE) {
            return true;
        }
        const DWORD last = GetLastError();
        if (last != ERROR_FILE_EXISTS && last != ERROR_ALREADY_EXISTS) {
            break;
        }
    }
    code = WinExit::kFailure;
    error = L"cannot create a temporary file in " + directory;
    return false;
}

bool rename_into_place(const std::wstring& temp, const std::wstring& target,
                       DWORD flags, bool& collision, int& code,
                       std::wstring& error) {
    collision = false;
    if (MoveFileExW(extended_path(temp).c_str(), extended_path(target).c_str(),
                    flags)) {
        return true;
    }
    const DWORD last = GetLastError();
    collision = last == ERROR_ALREADY_EXISTS || last == ERROR_FILE_EXISTS;
    code = (collision || last == ERROR_ACCESS_DENIED
            || last == ERROR_SHARING_VIOLATION)
               ? WinExit::kBusy
               : WinExit::kFailure;
    error = L"cannot move the finished PNG into place at " + target;
    return false;
}

std::wstring auto_name_candidate(const std::wstring& directory,
                                 const std::wstring& stem, std::uint32_t colors,
                                 unsigned attempt) {
    wchar_t suffix[40];
    if (attempt == 0u) {
        std::swprintf(suffix, 40u, L"-%uc.png", colors);
    } else {
        std::swprintf(suffix, 40u, L"-%uc-%u.png", colors, attempt + 1u);
    }
    return (directory.empty() ? std::wstring() : directory + L"\\") + stem
           + suffix;
}

// Picks the auto-name the next output will use without creating anything. The
// waiting viewer needs the final path up front, so the rename later has to
// land exactly here; a name that appears in between is a failure, not a
// reason to advance.
bool resolve_auto_name(const std::wstring& directory, const std::wstring& stem,
                       std::uint32_t colors, std::wstring& target) {
    for (unsigned attempt = 0u; attempt < kAutoNameAttempts; ++attempt) {
        const std::wstring candidate =
            auto_name_candidate(directory, stem, colors, attempt);
        if (GetFileAttributesW(extended_path(candidate).c_str())
            == INVALID_FILE_ATTRIBUTES) {
            target = candidate;
            return true;
        }
    }
    return false;
}

// Opens SeerViewer in its waiting state on the future output path. A failed
// launch only costs the loading UI: the output is still produced headless.
bool launch_seerviewer_waiting(const std::wstring& exe,
                               const std::wstring& image_path,
                               std::uint32_t wait_timeout_ms) {
    if (exe.empty() || image_path.empty()) {
        return false;
    }
    const std::wstring params =
        L"--wait-file --wait-timeout=" + std::to_wstring(wait_timeout_ms)
        + L" -- \"" + image_path + L"\"";
    std::wstring cmd = L"\"" + exe + L"\" " + params;
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, 0u,
                       nullptr, nullptr, &startup, &process)) {
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
        return true;
    }
    SHELLEXECUTEINFOW shell{};
    shell.cbSize = sizeof(shell);
    shell.fMask = SEE_MASK_FLAG_NO_UI;
    shell.lpVerb = L"open";
    shell.lpFile = exe.c_str();
    shell.lpParameters = params.c_str();
    shell.nShow = SW_SHOWNORMAL;
    return ShellExecuteExW(&shell) != FALSE;
}

bool place_output(const std::wstring& directory, const std::wstring& stem,
                  std::uint32_t colors, const std::wstring& explicit_output,
                  bool overwrite, const std::vector<std::uint8_t>& bytes,
                  std::wstring& final_path, int& code, std::wstring& error,
                  const std::wstring& viewer_target) {
    // The temp file has to live in the target's own directory: the final step
    // is a no-copy rename, which is atomic only on one volume.
    const std::wstring scratch = explicit_output.empty()
                                     ? directory
                                     : directory_of(explicit_output);
    reclaim_orphans(scratch);

    std::wstring temp;
    HANDLE handle = INVALID_HANDLE_VALUE;
    if (!create_temp(scratch, temp, handle, code, error)) {
        return false;
    }
    bool written = write_all(handle, bytes);
    if (written) {
        FlushFileBuffers(handle);
    }
    CloseHandle(handle);
    if (!written) {
        DeleteFileW(extended_path(temp).c_str());
        code = WinExit::kFailure;
        error = L"cannot write the temporary file in " + scratch;
        return false;
    }

    if (!explicit_output.empty()) {
        bool collision = false;
        if (rename_into_place(temp, explicit_output,
                              overwrite ? MOVEFILE_REPLACE_EXISTING : 0u,
                              collision, code, error)) {
            final_path = explicit_output;
            return true;
        }
        DeleteFileW(extended_path(temp).c_str());
        return false;
    }

    if (!viewer_target.empty()) {
        bool collision = false;
        int attempt_code = WinExit::kFailure;
        std::wstring attempt_error;
        if (rename_into_place(temp, viewer_target, 0u, collision, attempt_code,
                              attempt_error)) {
            final_path = viewer_target;
            return true;
        }
        DeleteFileW(extended_path(temp).c_str());
        if (collision) {
            code = WinExit::kBusy;
            error = L"the output appeared while SeerViewer was waiting for it: "
                    + viewer_target;
        } else {
            code = attempt_code;
            error = attempt_error;
        }
        return false;
    }
    for (unsigned attempt = 0u; attempt < kAutoNameAttempts; ++attempt) {
        const std::wstring target =
            auto_name_candidate(directory, stem, colors, attempt);
        bool collision = false;
        int attempt_code = WinExit::kFailure;
        std::wstring attempt_error;
        if (rename_into_place(temp, target, 0u, collision, attempt_code,
                              attempt_error)) {
            final_path = target;
            return true;
        }
        if (collision) {
            continue;
        }
        DeleteFileW(extended_path(temp).c_str());
        code = attempt_code;
        error = attempt_error;
        return false;
    }
    DeleteFileW(extended_path(temp).c_str());
    code = WinExit::kBusy;
    error = L"auto-naming exhausted 1000 attempts in " + directory;
    return false;
}

void append_log(const std::wstring& path, const std::string& line) {
    HANDLE handle = CreateFileW(extended_path(path).c_str(), FILE_APPEND_DATA,
                                FILE_SHARE_READ, nullptr, OPEN_ALWAYS,
                                FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        return;
    }
    DWORD written = 0u;
    WriteFile(handle, line.data(), static_cast<DWORD>(line.size()), &written,
              nullptr);
    CloseHandle(handle);
}

std::wstring widen(const char* text) {
    if (text == nullptr) {
        return std::wstring();
    }
    const int size = MultiByteToWideChar(CP_UTF8, 0, text, -1, nullptr, 0);
    if (size <= 1) {
        return std::wstring();
    }
    std::wstring out(static_cast<std::size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text, -1, out.data(), size);
    out.resize(static_cast<std::size_t>(size - 1));
    return out;
}

bool file_exists(const std::wstring& path) {
    const DWORD attributes = GetFileAttributesW(extended_path(path).c_str());
    return attributes != INVALID_FILE_ATTRIBUTES
           && (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0u;
}

}  // namespace

void write_error(const std::wstring& text) {
    std::wstring message = text;
    for (wchar_t& ch : message) {
        if (ch == L'\r' || ch == L'\n') {
            ch = L' ';
        }
    }
    if (message.size() > kMaxMessageChars) {
        message.resize(kMaxMessageChars);
    }
    std::wstring line = kPrefix + message + L"\n";
    const int needed = WideCharToMultiByte(CP_UTF8, 0, line.c_str(),
                                           static_cast<int>(line.size()),
                                           nullptr, 0, nullptr, nullptr);
    if (needed <= 0) {
        return;
    }
    std::string bytes(static_cast<std::size_t>(needed), '\0');
    WideCharToMultiByte(CP_UTF8, 0, line.c_str(), static_cast<int>(line.size()),
                        bytes.data(), needed, nullptr, nullptr);
    HANDLE handle = GetStdHandle(STD_ERROR_HANDLE);
    if (handle == nullptr || handle == INVALID_HANDLE_VALUE) {
        return;
    }
    DWORD written = 0u;
    WriteFile(handle, bytes.data(), static_cast<DWORD>(bytes.size()), &written,
              nullptr);
}

int run(int argc, wchar_t** argv) {
    Options options;
    std::wstring error;
    if (!parse_options(argc, argv, options, error)) {
        write_error(error);
        return WinExit::kUsage;
    }
    if (!file_exists(options.input)) {
        write_error(L"the input is missing or not a regular file: "
                    + options.input);
        return WinExit::kNotFound;
    }

    const std::wstring directory = directory_of(options.input);
    const std::wstring stem = stem_of(options.input);
    if (!options.output.empty()) {
        std::wstring resolved_input;
        std::wstring resolved_target;
        bool fully_resolved = false;
        if (WinPath::finalPath(options.input, &resolved_input)
            && WinPath::finalPathOfDeepestExistingAncestor(
                options.output, &resolved_target, &fully_resolved)
            && same_path(resolved_input, resolved_target)) {
            write_error(L"--output resolves to the input file: "
                        + options.output);
            return WinExit::kUsage;
        }
    }
    // Viewer mode resolves the final name before anything heavy runs, so the
    // waiting window can open within milliseconds of the click; the rename at
    // the end is what completes the wait.
    std::wstring viewer_target;
    if (!options.seerviewer_exe.empty()) {
        if (!options.output.empty()) {
            viewer_target = options.output;
        } else if (!resolve_auto_name(directory, stem, options.colors,
                                      viewer_target)) {
            write_error(L"auto-naming exhausted 1000 attempts in " + directory);
            return WinExit::kBusy;
        }
        launch_seerviewer_waiting(options.seerviewer_exe, viewer_target,
                                  options.wait_timeout_ms);
    }

    int code = WinExit::kFailure;
    RgbaImage image;
    bool icc_converted = false;
    if (!decode(options.input, image, code, error, icc_converted)) {
        write_error(error);
        return code;
    }

    sq_params params{};
    sq_status status = sq_params_init(&params);
    if (status != SQ_OK) {
        write_error(L"the engine parameters could not be initialized");
        return WinExit::kFailure;
    }
    params.max_colors = options.colors;
    params.alpha_mode = SQ_ALPHA_STRAIGHT;
    params.color_space = options.oklab ? SQ_SPACE_OKLAB : SQ_SPACE_SRGB;
    params.dither_mode = options.dither ? SQ_DITHER_FLOYD_STEINBERG
                                        : SQ_DITHER_NONE;
    params.dither_strength = options.strength;
    params.kmeans_max_iter = options.kmeans_iter;
    params.thread_count = options.threads;
    params.pipeline = options.pipeline;

    sq_image_in input{};
    input.struct_size = sizeof(input);
    input.version = SQ_ABI_VERSION;
    input.pixels = image.pixels.data();
    input.width = image.width;
    input.height = image.height;
    input.stride = static_cast<std::size_t>(image.width) * 4u;

    std::vector<std::uint8_t> indices(
        static_cast<std::size_t>(image.width) * image.height, 0u);
    sq_result result{};
    result.struct_size = sizeof(result);
    result.version = SQ_ABI_VERSION;
    result.indices = indices.data();
    result.indices_capacity = indices.size();

    sq_stats stats{};
    stats.struct_size = sizeof(stats);
    stats.version = SQ_ABI_VERSION;
    status = sq_quantize(&input, &params, nullptr, nullptr, &result, &stats);
    if (status != SQ_OK) {
        write_error(L"the engine failed: "
                    + widen(sq_status_message(status)));
        return status == SQ_ERR_CANCELLED ? WinExit::kCancelled
                                          : WinExit::kFailure;
    }

    std::vector<std::uint8_t> bytes;
    if (!encode_png(image, result, options.deflate, bytes, error)) {
        write_error(error);
        return WinExit::kFailure;
    }

    std::wstring written_path;
    if (!place_output(directory, stem, options.colors, options.output,
                      options.overwrite, bytes, written_path, code, error,
                      viewer_target)) {
        write_error(error);
        return code;
    }
    (void)written_path;

    if (!options.log_path.empty()) {
        char line[512];
        std::snprintf(line, sizeof(line),
                      "{\"palette_size\":%u,\"path\":%u,\"workspace_bytes\":%zu,"
                      "\"engine_ms\":%.3f,\"threads\":%u,\"icc\":\"%s\","
                      "\"deflate\":\"%s\"}\n",
                      static_cast<unsigned>(result.palette_size),
                      stats.path, stats.workspace_bytes, stats.stage_ms[5],
                      stats.thread_count_used,
                      icc_converted ? "converted" : "none",
                      options.deflate == DeflateTier::kMax      ? "max"
                      : options.deflate == DeflateTier::kBalanced ? "balanced"
                                                                  : "fast");
        append_log(options.log_path, line);
    }
    return WinExit::kOk;
}

}  // namespace iq
