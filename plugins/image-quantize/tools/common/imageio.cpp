#include "imageio.h"

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

#include "lodepng.h"

#include <fstream>
#include <iterator>

namespace sqtools {

bool read_file(const std::string& path, std::vector<std::uint8_t>& bytes, std::string* error) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        if (error != nullptr) {
            *error = "cannot open " + path;
        }
        return false;
    }
    bytes.assign(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
    return true;
}

bool write_file(const std::string& path, const std::vector<std::uint8_t>& bytes,
                std::string* error) {
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    if (!stream) {
        if (error != nullptr) {
            *error = "cannot open " + path;
        }
        return false;
    }
    if (!bytes.empty()) {
        stream.write(reinterpret_cast<const char*>(bytes.data()),
                     static_cast<std::streamsize>(bytes.size()));
    }
    if (!stream) {
        if (error != nullptr) {
            *error = "write failed for " + path;
        }
        return false;
    }
    return true;
}

bool load_rgba(const std::string& path, Image& image, std::string* error) {
    int width = 0;
    int height = 0;
    int channels = 0;
    unsigned char* decoded = stbi_load(path.c_str(), &width, &height, &channels, 4);
    if (decoded == nullptr) {
        if (error != nullptr) {
            *error = std::string("decode failed for ") + path + ": " + stbi_failure_reason();
        }
        return false;
    }
    if (width <= 0 || height <= 0) {
        stbi_image_free(decoded);
        if (error != nullptr) {
            *error = "invalid decoded dimensions for " + path;
        }
        return false;
    }
    image.width = static_cast<std::uint32_t>(width);
    image.height = static_cast<std::uint32_t>(height);
    const std::size_t size = static_cast<std::size_t>(width)
                           * static_cast<std::size_t>(height) * 4u;
    image.rgba.assign(decoded, decoded + size);
    stbi_image_free(decoded);
    return true;
}

bool encode_indexed_png(const std::vector<std::uint8_t>& indices,
                        std::uint32_t width,
                        std::uint32_t height,
                        const std::uint8_t* palette_rgba,
                        std::uint16_t palette_size,
                        std::vector<std::uint8_t>& png_bytes,
                        std::string* error) {
    if (palette_rgba == nullptr || palette_size == 0u || palette_size > 256u) {
        if (error != nullptr) {
            *error = "invalid palette";
        }
        return false;
    }
    if (indices.size() < static_cast<std::size_t>(width) * height) {
        if (error != nullptr) {
            *error = "index buffer shorter than the image";
        }
        return false;
    }
    for (std::uint8_t index : indices) {
        if (index >= palette_size) {
            if (error != nullptr) {
                *error = "index outside the emitted palette";
            }
            return false;
        }
    }

    lodepng::State state;
    state.info_raw.colortype = LCT_PALETTE;
    state.info_raw.bitdepth = 8u;
    state.info_png.color.colortype = LCT_PALETTE;
    state.info_png.color.bitdepth = 8u;
    state.encoder.auto_convert = 0;

    for (std::uint32_t i = 0; i < palette_size; ++i) {
        const unsigned char r = palette_rgba[i * 4u + 0u];
        const unsigned char g = palette_rgba[i * 4u + 1u];
        const unsigned char b = palette_rgba[i * 4u + 2u];
        const unsigned char a = palette_rgba[i * 4u + 3u];
        unsigned code = lodepng_palette_add(&state.info_raw, r, g, b, a);
        if (code == 0u) {
            code = lodepng_palette_add(&state.info_png.color, r, g, b, a);
        }
        if (code != 0u) {
            if (error != nullptr) {
                *error = std::string("palette add failed: ") + lodepng_error_text(code);
            }
            return false;
        }
    }

    const unsigned code = lodepng::encode(png_bytes, indices.data(), width, height, state);
    if (code != 0u) {
        if (error != nullptr) {
            *error = std::string("PNG encode failed: ") + lodepng_error_text(code);
        }
        return false;
    }
    return true;
}

}  // namespace sqtools
