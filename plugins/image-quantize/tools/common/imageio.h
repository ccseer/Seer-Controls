#ifndef SQ_TOOLS_IMAGEIO_H
#define SQ_TOOLS_IMAGEIO_H

#include <cstdint>
#include <string>
#include <vector>

namespace sqtools {

struct Image {
    std::uint32_t width = 0u;
    std::uint32_t height = 0u;
    std::vector<std::uint8_t> rgba;  // straight RGBA8, tightly packed, top-down
};

bool read_file(const std::string& path, std::vector<std::uint8_t>& bytes, std::string* error);
bool write_file(const std::string& path, const std::vector<std::uint8_t>& bytes, std::string* error);

// Decodes PNG/JPEG/GIF/BMP/TIFF with the vendored stb_image decoder. The
// baseline output is decoded with the same decoder so decoder differences
// cannot leak into a metric (6.4).
bool load_rgba(const std::string& path, Image& image, std::string* error);

// Encodes an indexed PNG-8 (PLTE + tRNS when any palette alpha is below 255)
// with the vendored lodepng encoder. Used for the bench output-size column and
// as the independent encoder reference for the WIC PoC.
bool encode_indexed_png(const std::vector<std::uint8_t>& indices,
                        std::uint32_t width,
                        std::uint32_t height,
                        const std::uint8_t* palette_rgba,
                        std::uint16_t palette_size,
                        std::vector<std::uint8_t>& png_bytes,
                        std::string* error);

}  // namespace sqtools

#endif  // SQ_TOOLS_IMAGEIO_H
