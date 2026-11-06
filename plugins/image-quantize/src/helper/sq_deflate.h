// Original deflate (RFC 1951) encoder used by the max tier of --deflate.
// It is written for this package: no third-party source, no external zlib.
// The encoder trades time for size: an iterative cost model drives a shortest
// path parse over every block instead of a greedy scan.
#ifndef SQ_DEFLATE_H
#define SQ_DEFLATE_H

#include <cstddef>
#include <cstdint>
#include <vector>

namespace sq_deflate {

// Compresses `size` bytes into a raw deflate stream (no zlib header or
// checksum, matching the custom_deflate contract of lodepng).
// `iterations` is the number of cost-model passes per block; 1 means a single
// parse with uniform costs.
bool compress(const std::uint8_t* data, std::size_t size,
              std::vector<std::uint8_t>& out, unsigned iterations);

}  // namespace sq_deflate

#endif  // SQ_DEFLATE_H
