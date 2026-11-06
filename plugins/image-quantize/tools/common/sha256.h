#ifndef SQ_TOOLS_SHA256_H
#define SQ_TOOLS_SHA256_H

#include <cstdint>
#include <string>
#include <vector>

namespace sqtools {

std::string sha256_hex(const std::uint8_t* data, std::size_t size);

inline std::string sha256_hex(const std::vector<std::uint8_t>& data) {
    return sha256_hex(data.data(), data.size());
}

}  // namespace sqtools

#endif  // SQ_TOOLS_SHA256_H
