#include "image_loader.h"

#include <windows.h>
#include <wincodec.h>

#pragma comment(lib, "windowscodecs.lib")

namespace seer_ocr {

namespace {

// A screenshot never comes close to this, but a malformed or hostile image can
// declare huge dimensions and make the RGB buffer allocation throw.
constexpr UINT kMaxImageSide = 16384;
constexpr size_t kMaxImagePixels = 64ull * 1024ull * 1024ull;

struct ComInitGuard {
    HRESULT hr;
    ComInitGuard() {
        hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    }
    ~ComInitGuard() {
        if (SUCCEEDED(hr)) {
            CoUninitialize();
        }
    }
};

template <typename T>
struct ComPtr {
    T *p = nullptr;
    ComPtr() = default;
    ~ComPtr() {
        if (p) p->Release();
    }
    T **operator&() { return &p; }
    T *operator->() const { return p; }
    T *get() const { return p; }
    explicit operator bool() const { return p != nullptr; }
};

}  // namespace

bool loadImageRgb(const std::wstring &path, ImageRgb &out_image, std::wstring &error_msg)
{
    ComInitGuard com_guard;

    ComPtr<IWICImagingFactory> factory;
    HRESULT hr = CoCreateInstance(
        CLSID_WICImagingFactory,
        nullptr,
        CLSCTX_INPROC_SERVER,
        IID_IWICImagingFactory,
        reinterpret_cast<void **>(&factory));

    if (FAILED(hr)) {
        error_msg = L"Failed to create WICImagingFactory (hr=" + std::to_wstring(hr) + L")";
        return false;
    }

    ComPtr<IWICBitmapDecoder> decoder;
    hr = factory->CreateDecoderFromFilename(
        path.c_str(),
        nullptr,
        GENERIC_READ,
        WICDecodeMetadataCacheOnDemand,
        &decoder);

    if (FAILED(hr)) {
        error_msg = L"Failed to open image: " + path;
        return false;
    }

    ComPtr<IWICBitmapFrameDecode> frame;
    hr = decoder->GetFrame(0, &frame);
    if (FAILED(hr)) {
        error_msg = L"Failed to decode image frame from " + path;
        return false;
    }

    UINT width = 0;
    UINT height = 0;
    hr = frame->GetSize(&width, &height);
    if (FAILED(hr) || width == 0 || height == 0) {
        error_msg = L"Invalid image dimensions: " + path;
        return false;
    }

    if (width > kMaxImageSide || height > kMaxImageSide
        || static_cast<size_t>(width) * height > kMaxImagePixels) {
        error_msg = L"Image dimensions out of range (" + std::to_wstring(width) + L"x"
                    + std::to_wstring(height) + L"): " + path;
        return false;
    }

    ComPtr<IWICFormatConverter> converter;
    hr = factory->CreateFormatConverter(&converter);
    if (FAILED(hr)) {
        error_msg = L"Failed to create WICFormatConverter";
        return false;
    }

    // Try converting to 24bpp RGB first
    hr = converter->Initialize(
        frame.get(),
        GUID_WICPixelFormat24bppRGB,
        WICBitmapDitherTypeNone,
        nullptr,
        0.0,
        WICBitmapPaletteTypeCustom);

    bool need_bgr_swap = false;
    if (FAILED(hr)) {
        // Fallback: convert to 24bpp BGR and swap to RGB
        hr = converter->Initialize(
            frame.get(),
            GUID_WICPixelFormat24bppBGR,
            WICBitmapDitherTypeNone,
            nullptr,
            0.0,
            WICBitmapPaletteTypeCustom);
        need_bgr_swap = true;
    }

    if (FAILED(hr)) {
        // Fallback: convert to 32bpp RGBA and convert to 24bpp RGB
        hr = converter->Initialize(
            frame.get(),
            GUID_WICPixelFormat32bppRGBA,
            WICBitmapDitherTypeNone,
            nullptr,
            0.0,
            WICBitmapPaletteTypeCustom);

        if (FAILED(hr)) {
            error_msg = L"Failed to convert image pixel format for " + path;
            return false;
        }

        std::vector<uint8_t> rgba(static_cast<size_t>(width) * height * 4);
        UINT stride32 = width * 4;
        hr = converter->CopyPixels(nullptr, stride32, static_cast<UINT>(rgba.size()), rgba.data());
        if (FAILED(hr)) {
            error_msg = L"Failed to copy pixels from " + path;
            return false;
        }

        out_image.width  = static_cast<int>(width);
        out_image.height = static_cast<int>(height);
        out_image.data.resize(width * height * 3);

        const uint8_t *src = rgba.data();
        uint8_t *dst       = out_image.data.data();
        size_t num_pixels  = static_cast<size_t>(width) * height;
        for (size_t i = 0; i < num_pixels; ++i) {
            dst[0] = src[0];
            dst[1] = src[1];
            dst[2] = src[2];
            src += 4;
            dst += 3;
        }
        return true;
    }

    UINT stride = width * 3;
    out_image.width  = static_cast<int>(width);
    out_image.height = static_cast<int>(height);
    out_image.data.resize(static_cast<size_t>(stride) * height);

    hr = converter->CopyPixels(
        nullptr,
        stride,
        static_cast<UINT>(out_image.data.size()),
        out_image.data.data());

    if (FAILED(hr)) {
        error_msg = L"Failed to read pixels from " + path;
        return false;
    }

    if (need_bgr_swap) {
        uint8_t *buf      = out_image.data.data();
        size_t num_pixels = static_cast<size_t>(width) * height;
        for (size_t i = 0; i < num_pixels; ++i) {
            std::swap(buf[0], buf[2]);
            buf += 3;
        }
    }

    return true;
}

}  // namespace seer_ocr
