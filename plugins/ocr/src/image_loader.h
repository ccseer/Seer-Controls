#pragma once

#include <string>
#include <vector>
#include <cstdint>

namespace seer_ocr {

struct ImageRgb {
    int width  = 0;
    int height = 0;
    std::vector<uint8_t> data; // RGB format: R, G, B, R, G, B...

    bool empty() const {
        return width <= 0 || height <= 0 || data.empty();
    }
};

// Loads image from file path using Windows Imaging Component (WIC)
// Output data is formatted as RGB 24bpp (width * height * 3 bytes)
bool loadImageRgb(const std::wstring &path, ImageRgb &out_image, std::wstring &error_msg);

}  // namespace seer_ocr
