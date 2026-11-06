#include "sq_deflate.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>

namespace sq_deflate {
namespace {

constexpr int kWindowSize = 32768;
constexpr int kMinMatch = 3;
constexpr int kMaxMatch = 258;
constexpr int kHashBits = 16;
constexpr int kHashSize = 1 << kHashBits;
constexpr int kChainLimit = 32768;
constexpr int kMaxBits = 15;
constexpr int kMaxCodeLengthBits = 7;

struct Range {
    std::uint16_t base;
    std::uint8_t extra;
};

// RFC 1951 3.2.5, length codes 257..285.
constexpr Range kLengthTable[29] = {
    {3, 0},   {4, 0},   {5, 0},   {6, 0},   {7, 0},   {8, 0},   {9, 0},
    {10, 0},  {11, 1},  {13, 1},  {15, 1},  {17, 1},  {19, 2},  {23, 2},
    {27, 2},  {31, 2},  {35, 3},  {43, 3},  {51, 3},  {59, 3},  {67, 4},
    {83, 4},  {99, 4},  {115, 4}, {131, 5}, {163, 5}, {195, 5}, {227, 5},
    {258, 0}};

// RFC 1951 3.2.5, distance codes 0..29.
constexpr Range kDistanceTable[30] = {
    {1, 0},      {2, 0},      {3, 0},      {4, 0},      {5, 1},
    {7, 1},      {9, 2},      {13, 2},     {17, 3},     {25, 3},
    {33, 4},     {49, 4},     {65, 5},     {97, 5},     {129, 6},
    {193, 6},    {257, 7},    {385, 7},    {513, 8},    {769, 8},
    {1025, 9},   {1537, 9},   {2049, 10},  {3073, 10},  {4097, 11},
    {6145, 11},  {8193, 12},  {12289, 12}, {16385, 13}, {24577, 13}};

constexpr std::uint8_t kCodeLengthOrder[19] = {16, 17, 18, 0, 8,  7, 9,  6, 10, 5,
                                               11, 4,  12, 3, 13, 2, 14, 1, 15};

struct Tables {
    std::uint8_t length_code[kMaxMatch + 1]{};
    std::uint8_t length_extra[kMaxMatch + 1]{};
    std::uint16_t length_base[kMaxMatch + 1]{};
    std::uint16_t length_code_max[29]{};
    std::uint8_t distance_code[kWindowSize + 1]{};
    std::uint8_t distance_extra[kWindowSize + 1]{};
    std::uint16_t distance_base[kWindowSize + 1]{};

    Tables() {
        for (int code = 0; code < 29; ++code) {
            const int next = code + 1 < 29 ? kLengthTable[code + 1].base - 1
                                           : kMaxMatch;
            length_code_max[code] = static_cast<std::uint16_t>(next);
            for (int length = kLengthTable[code].base; length <= next; ++length) {
                length_code[length] = static_cast<std::uint8_t>(code);
                length_extra[length] = kLengthTable[code].extra;
                length_base[length] = kLengthTable[code].base;
            }
        }
        for (int code = 0; code < 30; ++code) {
            const int next = code + 1 < 30 ? kDistanceTable[code + 1].base - 1
                                           : kWindowSize;
            for (int distance = kDistanceTable[code].base; distance <= next;
                 ++distance) {
                distance_code[distance] = static_cast<std::uint8_t>(code);
                distance_extra[distance] = kDistanceTable[code].extra;
                distance_base[distance] = kDistanceTable[code].base;
            }
        }
    }
};

const Tables& tables() {
    static const Tables value;
    return value;
}

struct BitWriter {
    std::vector<std::uint8_t>& out;
    std::uint32_t buffer = 0u;
    int bits = 0;

    explicit BitWriter(std::vector<std::uint8_t>& target) : out(target) {}

    void write(std::uint32_t value, int count) {
        buffer |= (value & ((1u << count) - 1u)) << bits;
        bits += count;
        while (bits >= 8) {
            out.push_back(static_cast<std::uint8_t>(buffer & 0xFFu));
            buffer >>= 8;
            bits -= 8;
        }
    }

    void flush() {
        if (bits != 0) {
            out.push_back(static_cast<std::uint8_t>(buffer & 0xFFu));
            buffer = 0u;
            bits = 0;
        }
    }
};

std::uint16_t reverse_bits(std::uint16_t value, int count) {
    std::uint16_t result = 0u;
    for (int i = 0; i < count; ++i) {
        result = static_cast<std::uint16_t>((result << 1u) | (value & 1u));
        value = static_cast<std::uint16_t>(value >> 1u);
    }
    return result;
}

struct Huffman {
    std::uint8_t length[288]{};
    std::uint16_t code[288]{};

    void build_codes(int count) {
        int max_length = 0;
        int bl_count[16]{};
        for (int i = 0; i < count; ++i) {
            if (length[i] != 0u) {
                max_length = std::max<int>(max_length, length[i]);
                ++bl_count[length[i]];
            }
        }
        std::uint16_t next[16]{};
        std::uint16_t value = 0u;
        for (int bits = 1; bits <= max_length; ++bits) {
            value = static_cast<std::uint16_t>((value + bl_count[bits - 1]) << 1u);
            next[bits] = value;
        }
        for (int i = 0; i < count; ++i) {
            const int len = length[i];
            code[i] = len == 0 ? 0u : reverse_bits(static_cast<std::uint16_t>(next[len]++), len);
        }
    }
};

// Length-limited Huffman lengths from symbol frequencies. The overflow repair
// moves leaves one level down until the tree fits in max_bits.
void build_lengths(const std::uint32_t* frequency, int count, int max_bits,
                   std::uint8_t* lengths) {
    std::memset(lengths, 0, static_cast<std::size_t>(count));
    std::array<int, 288> used{};
    int used_count = 0;
    for (int i = 0; i < count; ++i) {
        if (frequency[i] != 0u) {
            used[static_cast<std::size_t>(used_count++)] = i;
        }
    }
    if (used_count == 0) {
        return;
    }
    if (used_count == 1) {
        lengths[used[0]] = 1u;
        return;
    }

    std::array<std::uint64_t, 576> combined{};
    std::array<int, 576> parent{};
    std::array<bool, 576> taken{};
    for (int i = 0; i < used_count; ++i) {
        combined[static_cast<std::size_t>(i)] = frequency[used[static_cast<std::size_t>(i)]];
        parent[static_cast<std::size_t>(i)] = -1;
    }
    int node_count = used_count;
    int active = used_count;
    while (active > 1) {
        int first = -1;
        int second = -1;
        for (int i = 0; i < node_count; ++i) {
            if (taken[static_cast<std::size_t>(i)]) {
                continue;
            }
            if (first < 0 ||
                combined[static_cast<std::size_t>(i)] < combined[static_cast<std::size_t>(first)]) {
                second = first;
                first = i;
            } else if (second < 0 ||
                       combined[static_cast<std::size_t>(i)] <
                           combined[static_cast<std::size_t>(second)]) {
                second = i;
            }
        }
        if (second < 0) {
            break;
        }
        taken[static_cast<std::size_t>(first)] = true;
        taken[static_cast<std::size_t>(second)] = true;
        combined[static_cast<std::size_t>(node_count)] =
            combined[static_cast<std::size_t>(first)] +
            combined[static_cast<std::size_t>(second)];
        parent[static_cast<std::size_t>(first)] = node_count;
        parent[static_cast<std::size_t>(second)] = node_count;
        parent[static_cast<std::size_t>(node_count)] = -1;
        taken[static_cast<std::size_t>(node_count)] = false;
        ++node_count;
        --active;
    }

    int bl_count[16]{};
    int overflow = 0;
    for (int i = 0; i < used_count; ++i) {
        int depth = 0;
        for (int node = i; node >= 0; node = parent[static_cast<std::size_t>(node)]) {
            ++depth;
        }
        int len = std::max(1, depth - 1);
        if (len > max_bits) {
            len = max_bits;
            ++overflow;
        }
        lengths[used[static_cast<std::size_t>(i)]] = static_cast<std::uint8_t>(len);
        ++bl_count[len];
    }
    while (overflow > 0) {
        int bits = max_bits - 1;
        while (bits > 0 && bl_count[bits] == 0) {
            --bits;
        }
        if (bits == 0) {
            break;
        }
        --bl_count[bits];
        bl_count[bits + 1] += 2;
        --bl_count[max_bits];
        overflow -= 2;
    }

    // The repair above can still oversubscribe a pathologically skewed tree.
    // A length-limited code is only legal when the Kraft sum stays at or below
    // one, so verify the repaired histogram and fall back to a balanced code
    // otherwise. The check must run on the repaired counts, not on the clamped
    // depths of the original tree.
    std::uint64_t kraft = 0u;
    for (int bits = 1; bits <= max_bits; ++bits) {
        kraft += static_cast<std::uint64_t>(bl_count[bits]) << (max_bits - bits);
    }
    const bool oversubscribed = kraft > (static_cast<std::uint64_t>(1u) << max_bits);

    std::array<int, 288> order{};
    std::memcpy(order.data(), used.data(), sizeof(int) * static_cast<std::size_t>(used_count));
    std::sort(order.begin(), order.begin() + used_count, [&](int left, int right) {
        if (frequency[left] != frequency[right]) {
            return frequency[left] < frequency[right];
        }
        return left < right;
    });

    if (oversubscribed) {
        int longer = 0;
        while ((1 << longer) < used_count) {
            ++longer;
        }
        longer = std::min(longer, max_bits);
        const int total = 1 << longer;
        const int short_codes = std::max(0, total - used_count);
        const int long_codes = used_count - short_codes;
        // Most frequent symbols get the shorter length.
        int cursor = used_count - 1;
        for (int i = 0; i < short_codes; ++i) {
            lengths[order[static_cast<std::size_t>(cursor--)]] =
                static_cast<std::uint8_t>(std::max(1, longer - 1));
        }
        for (int i = 0; i < long_codes; ++i) {
            lengths[order[static_cast<std::size_t>(cursor--)]] =
                static_cast<std::uint8_t>(longer);
        }
        return;
    }

    int index = 0;
    for (int bits = max_bits; bits >= 1; --bits) {
        for (int n = 0; n < bl_count[bits] && index < used_count; ++n) {
            lengths[order[static_cast<std::size_t>(index++)]] =
                static_cast<std::uint8_t>(bits);
        }
    }
    while (index < used_count) {
        lengths[order[static_cast<std::size_t>(index++)]] = 1u;
    }
}

class Matcher {
  public:
    Matcher(const std::uint8_t* data, std::size_t size)
        : data_(data), size_(size), head_(kHashSize, -1), prev_(kWindowSize, 0u) {}

    void insert(std::size_t position) {
        if (position + kMinMatch > size_) {
            return;
        }
        const std::int32_t previous = head_[hash(position)];
        const std::int32_t distance =
            previous < 0 ? 0 : static_cast<std::int32_t>(position) - previous;
        prev_[position & (kWindowSize - 1)] =
            distance > 0 && distance <= kWindowSize
                ? static_cast<std::uint16_t>(distance)
                : static_cast<std::uint16_t>(0u);
        head_[hash(position)] = static_cast<std::int32_t>(position);
    }

    // Fills buckets[0..28] with the nearest distance that still reaches the
    // length this code bucket can express. Walking the chain nearest-first
    // means the first candidate that covers a bucket is the cheapest one for
    // that length, which the parse relies on: a short match is usually far
    // cheaper through a short distance code than through the distance of the
    // longest match.
    void find(std::size_t position, int& best_length,
              std::uint16_t* buckets) const {
        best_length = 0;
        if (position + kMinMatch > size_) {
            return;
        }
        int improvement_length[kMaxMatch + 1]{};
        std::uint16_t improvement_distance[kMaxMatch + 1]{};
        int improvements = 0;
        std::int32_t candidate = head_[hash(position)];
        const std::size_t limit = std::min(size_, position + kMaxMatch);
        int chain = kChainLimit;
        while (candidate >= 0 && chain-- > 0) {
            const std::size_t distance = position - static_cast<std::size_t>(candidate);
            if (distance > kWindowSize || distance == 0u) {
                break;
            }
            // A candidate can only win when it matches at the current best
            // length, so reject it with one byte compare before the full scan.
            // When the current best already reaches the search limit no later
            // candidate can beat it, and the probe byte would sit one past the
            // buffer.
            if (best_length > 0 &&
                position + static_cast<std::size_t>(best_length) >= limit) {
                break;
            }
            if (best_length > 0 &&
                data_[static_cast<std::size_t>(candidate) +
                      static_cast<std::size_t>(best_length)] !=
                    data_[position + static_cast<std::size_t>(best_length)]) {
                const std::uint16_t step =
                    prev_[static_cast<std::size_t>(candidate) & (kWindowSize - 1)];
                if (step == 0u) {
                    break;
                }
                candidate -= static_cast<std::int32_t>(step);
                continue;
            }
            std::size_t length = 0u;
            while (position + length < limit &&
                   data_[static_cast<std::size_t>(candidate) + length] ==
                       data_[position + length]) {
                ++length;
            }
            if (static_cast<int>(length) > best_length) {
                best_length = static_cast<int>(length);
                improvement_length[improvements] = best_length;
                improvement_distance[improvements] =
                    static_cast<std::uint16_t>(distance);
                ++improvements;
                if (best_length >= kMaxMatch) {
                    break;
                }
            }
            const std::uint16_t step =
                prev_[static_cast<std::size_t>(candidate) & (kWindowSize - 1)];
            if (step == 0u) {
                break;
            }
            candidate -= static_cast<std::int32_t>(step);
        }
        if (best_length < kMinMatch) {
            return;
        }
        int cursor = 0;
        for (int code = 0; code < 29; ++code) {
            const int target = std::min<int>(best_length, tables().length_code_max[code]);
            while (cursor + 1 < improvements &&
                   improvement_length[cursor] < target) {
                ++cursor;
            }
            buckets[code] = improvement_distance[cursor];
        }
    }

  private:
    std::int32_t hash(std::size_t position) const {
        const std::uint32_t value =
            (static_cast<std::uint32_t>(data_[position]) << 10u) ^
            (static_cast<std::uint32_t>(data_[position + 1u]) << 5u) ^
            static_cast<std::uint32_t>(data_[position + 2u]);
        return static_cast<std::int32_t>(value & (kHashSize - 1));
    }

    const std::uint8_t* data_;
    std::size_t size_;
    std::vector<std::int32_t> head_;
    std::vector<std::uint16_t> prev_;
};

struct Token {
    std::uint16_t symbol;    // literal byte, or 257 + length code
    std::uint16_t distance;  // 0 for a literal
    std::uint16_t extra;     // length offset within its code
};

void build_greedy(const std::uint8_t* data, std::size_t block_start,
                  std::size_t block_length, const std::uint16_t* match_length,
                  const std::uint16_t* bucket_distance,
                  std::vector<Token>& tokens) {
    tokens.clear();
    tokens.reserve(block_length / 2u + 1u);
    std::size_t i = 0u;
    while (i < block_length) {
        const int available = static_cast<int>(block_length - i);
        int length = std::min<int>(match_length[i], available);
        if (length >= kMinMatch && i + 1u < block_length) {
            const int next = std::min<int>(match_length[i + 1u], available - 1);
            if (next > length) {
                length = 0;  // lazy: the next position matches better
            }
        }
        if (length >= kMinMatch) {
            Token token{};
            token.symbol =
                static_cast<std::uint16_t>(257 + tables().length_code[length]);
            token.distance = bucket_distance[i * 29u + tables().length_code[length]];
            token.extra = static_cast<std::uint16_t>(length - tables().length_base[length]);
            tokens.push_back(token);
            i += static_cast<std::size_t>(length);
        } else {
            Token token{};
            token.symbol = data[block_start + i];
            token.distance = 0u;
            token.extra = 0u;
            tokens.push_back(token);
            ++i;
        }
    }
}

void write_tokens(const std::vector<Token>& tokens, const Huffman& litlen,
                  const Huffman& distance, BitWriter& writer) {
    for (const Token& token : tokens) {
        if (token.distance == 0u) {
            writer.write(litlen.code[token.symbol], litlen.length[token.symbol]);
            continue;
        }
        writer.write(litlen.code[token.symbol], litlen.length[token.symbol]);
        const int length_code = token.symbol - 257;
        if (kLengthTable[length_code].extra != 0u) {
            writer.write(token.extra, kLengthTable[length_code].extra);
        }
        const int distance_code = tables().distance_code[token.distance];
        writer.write(distance.code[distance_code], distance.length[distance_code]);
        if (kDistanceTable[distance_code].extra != 0u) {
            writer.write(static_cast<std::uint32_t>(
                             token.distance - kDistanceTable[distance_code].base),
                         kDistanceTable[distance_code].extra);
        }
    }
    writer.write(litlen.code[256], litlen.length[256]);
}

void encode_dynamic(const std::vector<Token>& tokens, const Huffman& litlen,
                    const Huffman& distance, bool final, BitWriter& writer) {
    std::uint32_t litlen_frequency[288]{};
    std::uint32_t distance_frequency[30]{};
    for (const Token& token : tokens) {
        ++litlen_frequency[token.symbol];
        if (token.distance != 0u) {
            ++distance_frequency[tables().distance_code[token.distance]];
        }
    }
    ++litlen_frequency[256];
    int hlit = 286;
    while (hlit > 257 && litlen_frequency[hlit - 1] == 0u) {
        --hlit;
    }
    int hdist = 30;
    while (hdist > 1 && distance_frequency[hdist - 1] == 0u) {
        --hdist;
    }

    std::uint8_t combined[288 + 30]{};
    std::memcpy(combined, litlen.length, static_cast<std::size_t>(hlit));
    std::memcpy(combined + hlit, distance.length, static_cast<std::size_t>(hdist));
    const int total = hlit + hdist;

    std::uint8_t symbols[288 + 30]{};
    std::uint8_t extra_counts[288 + 30]{};
    std::uint8_t extra_values[288 + 30]{};
    int symbol_count = 0;
    int index = 0;
    while (index < total) {
        const std::uint8_t value = combined[index];
        int run = 1;
        while (index + run < total && combined[index + run] == value) {
            ++run;
        }
        if (value == 0u) {
            while (run >= 11) {
                const int take = std::min(run, 138);
                symbols[symbol_count] = 18u;
                extra_counts[symbol_count] = 7u;
                extra_values[symbol_count] = static_cast<std::uint8_t>(take - 11);
                ++symbol_count;
                run -= take;
                index += take;
            }
            while (run >= 3) {
                const int take = std::min(run, 10);
                symbols[symbol_count] = 17u;
                extra_counts[symbol_count] = 3u;
                extra_values[symbol_count] = static_cast<std::uint8_t>(take - 3);
                ++symbol_count;
                run -= take;
                index += take;
            }
            while (run > 0) {
                symbols[symbol_count++] = 0u;
                --run;
                ++index;
            }
        } else {
            symbols[symbol_count++] = value;
            --run;
            ++index;
            while (run >= 3) {
                const int take = std::min(run, 6);
                symbols[symbol_count] = 16u;
                extra_counts[symbol_count] = 2u;
                extra_values[symbol_count] = static_cast<std::uint8_t>(take - 3);
                ++symbol_count;
                run -= take;
                index += take;
            }
            while (run > 0) {
                symbols[symbol_count++] = value;
                --run;
                ++index;
            }
        }
    }

    std::uint32_t code_length_frequency[19]{};
    for (int i = 0; i < symbol_count; ++i) {
        ++code_length_frequency[symbols[i]];
    }
    std::uint8_t code_length_lengths[19]{};
    build_lengths(code_length_frequency, 19, kMaxCodeLengthBits, code_length_lengths);
    Huffman code_length_code;
    std::memcpy(code_length_code.length, code_length_lengths,
                sizeof(code_length_lengths));
    code_length_code.build_codes(19);

    int hclen = 19;
    while (hclen > 4 && code_length_lengths[kCodeLengthOrder[hclen - 1]] == 0u) {
        --hclen;
    }

    writer.write(final ? 1u : 0u, 1);
    writer.write(2u, 2);  // dynamic Huffman
    writer.write(static_cast<std::uint32_t>(hlit - 257), 5);
    writer.write(static_cast<std::uint32_t>(hdist - 1), 5);
    writer.write(static_cast<std::uint32_t>(hclen - 4), 4);
    for (int i = 0; i < hclen; ++i) {
        writer.write(code_length_lengths[kCodeLengthOrder[i]], 3);
    }
    for (int i = 0; i < symbol_count; ++i) {
        const int symbol = symbols[i];
        writer.write(code_length_code.code[symbol], code_length_code.length[symbol]);
        if (extra_counts[i] != 0u) {
            writer.write(extra_values[i], extra_counts[i]);
        }
    }
    write_tokens(tokens, litlen, distance, writer);
}

void encode_stored_block(const std::uint8_t* data, std::size_t size, bool final,
                         BitWriter& writer) {
    std::size_t offset = 0u;
    do {
        const std::size_t chunk = std::min<std::size_t>(65535u, size - offset);
        const bool last = final && offset + chunk == size;
        writer.write(last ? 1u : 0u, 1);
        writer.write(0u, 2);
        writer.flush();
        writer.out.push_back(static_cast<std::uint8_t>(chunk & 0xFFu));
        writer.out.push_back(static_cast<std::uint8_t>((chunk >> 8u) & 0xFFu));
        writer.out.push_back(static_cast<std::uint8_t>((~chunk) & 0xFFu));
        writer.out.push_back(static_cast<std::uint8_t>((~chunk >> 8u) & 0xFFu));
        writer.out.insert(writer.out.end(), data + offset, data + offset + chunk);
        offset += chunk;
    } while (offset < size);
}

}  // namespace

bool compress(const std::uint8_t* data, std::size_t size,
              std::vector<std::uint8_t>& out, unsigned iterations) {
    out.clear();
    const unsigned passes = std::max(1u, iterations);
    if (size == 0u) {
        BitWriter writer(out);
        encode_stored_block(data, 0u, true, writer);
        writer.flush();
        return true;
    }
    std::size_t block_size = size / 8u + 8u;
    block_size = std::max<std::size_t>(block_size, 65536u);
    block_size = std::min<std::size_t>(block_size, 262144u);

    Matcher matcher(data, size);
    std::vector<std::uint16_t> match_length(block_size);
    std::vector<std::uint16_t> bucket_distance(block_size * 29u);
    std::vector<Token> tokens;
    std::vector<std::uint8_t> candidate_output;

    BitWriter writer(out);
    std::size_t block_start = 0u;
    while (block_start < size) {
        const std::size_t block_end = std::min(size, block_start + block_size);
        const std::size_t block_length = block_end - block_start;
        const bool last_block = block_end == size;

        for (std::size_t i = 0u; i < block_length; ++i) {
            int length = 0;
            matcher.find(block_start + i, length, &bucket_distance[i * 29u]);
            matcher.insert(block_start + i);
            if (static_cast<std::size_t>(length) > block_length - i) {
                length = static_cast<int>(block_length - i);
            }
            match_length[i] = static_cast<std::uint16_t>(length);
        }

        std::size_t best_size = 0u;
        std::vector<Token> best_tokens;
        Huffman best_litlen;
        Huffman best_distance;

        std::uint8_t litlen_bits[288]{};
        std::uint8_t distance_bits[30]{};

        std::vector<Token> greedy_tokens;
        build_greedy(data, block_start, block_length, match_length.data(),
                     bucket_distance.data(), greedy_tokens);
        Huffman seed_litlen;
        Huffman seed_distance;
        {
            std::uint32_t litlen_frequency[288]{};
            std::uint32_t distance_frequency[30]{};
            for (const Token& token : greedy_tokens) {
                ++litlen_frequency[token.symbol];
                if (token.distance != 0u) {
                    ++distance_frequency[tables().distance_code[token.distance]];
                }
            }
            ++litlen_frequency[256];
            build_lengths(litlen_frequency, 288, kMaxBits, seed_litlen.length);
            build_lengths(distance_frequency, 30, kMaxBits, seed_distance.length);
            seed_litlen.build_codes(288);
            seed_distance.build_codes(30);
        }
        candidate_output.clear();
        {
            BitWriter probe(candidate_output);
            encode_dynamic(greedy_tokens, seed_litlen, seed_distance, false, probe);
            probe.flush();
            best_size = candidate_output.size();
            best_tokens = greedy_tokens;
            best_litlen = seed_litlen;
            best_distance = seed_distance;
        }
        for (int i = 0; i < 288; ++i) {
            litlen_bits[i] = seed_litlen.length[i] != 0u ? seed_litlen.length[i] : kMaxBits;
        }
        for (int i = 0; i < 30; ++i) {
            distance_bits[i] =
                seed_distance.length[i] != 0u ? seed_distance.length[i] : kMaxBits;
        }

        std::vector<double> cost(block_length + 1u);
        std::vector<std::uint32_t> previous(block_length + 1u);
        std::vector<std::uint16_t> previous_length(block_length + 1u);

        for (unsigned pass = 0; pass < passes; ++pass) {
            const double infinity = std::numeric_limits<double>::max() / 4.0;
            std::fill(cost.begin(), cost.end(), infinity);
            cost[0] = 0.0;
            for (std::size_t i = 0u; i < block_length; ++i) {
                const double base = cost[i];
                if (base >= infinity) {
                    continue;
                }
                const double literal_cost =
                    base + static_cast<double>(litlen_bits[data[block_start + i]]);
                if (literal_cost < cost[i + 1u]) {
                    cost[i + 1u] = literal_cost;
                    previous[i + 1u] = static_cast<std::uint32_t>(i);
                    previous_length[i + 1u] = 1u;
                }
                int maximum = match_length[i];
                if (static_cast<std::size_t>(maximum) > block_length - i) {
                    maximum = static_cast<int>(block_length - i);
                }
                if (maximum < kMinMatch) {
                    continue;
                }
                const std::uint16_t* buckets = &bucket_distance[i * 29u];
                for (int code = 0; code < 29; ++code) {
                    const int code_base = kLengthTable[code].base;
                    if (code_base > maximum) {
                        break;
                    }
                    const int length = std::min<int>(maximum, tables().length_code_max[code]);
                    if (length < kMinMatch) {
                        continue;
                    }
                    const int distance = buckets[code];
                    if (distance <= 0) {
                        continue;
                    }
                    const int distance_code = tables().distance_code[distance];
                    const double distance_cost =
                        static_cast<double>(distance_bits[distance_code]) +
                        kDistanceTable[distance_code].extra;
                    const double total = base +
                                         static_cast<double>(litlen_bits[257 + code]) +
                                         kLengthTable[code].extra + distance_cost;
                    if (total < cost[i + static_cast<std::size_t>(length)]) {
                        cost[i + static_cast<std::size_t>(length)] = total;
                        previous[i + static_cast<std::size_t>(length)] =
                            static_cast<std::uint32_t>(i);
                        previous_length[i + static_cast<std::size_t>(length)] =
                            static_cast<std::uint16_t>(length);
                    }
                }
            }

            tokens.clear();
            tokens.reserve(block_length / 2u + 1u);
            std::size_t position = block_length;
            while (position > 0u) {
                const std::size_t start = previous[position];
                const int length = previous_length[position];
                Token token{};
                if (length == 1) {
                    token.symbol = data[block_start + start];
                    token.distance = 0u;
                    token.extra = 0u;
                } else {
                    token.symbol = static_cast<std::uint16_t>(
                        257 + tables().length_code[length]);
                    token.distance = bucket_distance[start * 29u +
                                                     tables().length_code[length]];
                    token.extra = static_cast<std::uint16_t>(
                        length - tables().length_base[length]);
                }
                tokens.push_back(token);
                position = start;
            }
            std::reverse(tokens.begin(), tokens.end());

            std::uint32_t litlen_frequency[288]{};
            std::uint32_t distance_frequency[30]{};
            for (const Token& token : tokens) {
                ++litlen_frequency[token.symbol];
                if (token.distance != 0u) {
                    ++distance_frequency[tables().distance_code[token.distance]];
                }
            }
            ++litlen_frequency[256];
            Huffman litlen;
            Huffman distance;
            build_lengths(litlen_frequency, 288, kMaxBits, litlen.length);
            build_lengths(distance_frequency, 30, kMaxBits, distance.length);
            litlen.build_codes(288);
            distance.build_codes(30);

            candidate_output.clear();
            BitWriter probe(candidate_output);
            encode_dynamic(tokens, litlen, distance, false, probe);
            probe.flush();
            if (best_size == 0u || candidate_output.size() < best_size) {
                best_size = candidate_output.size();
                best_tokens = tokens;
                best_litlen = litlen;
                best_distance = distance;
            }

            for (int i = 0; i < 288; ++i) {
                litlen_bits[i] = litlen.length[i] != 0u ? litlen.length[i] : kMaxBits;
            }
            for (int i = 0; i < 30; ++i) {
                distance_bits[i] =
                    distance.length[i] != 0u ? distance.length[i] : kMaxBits;
            }
        }

        const std::size_t stored_size =
            block_length + 5u * ((block_length + 65534u) / 65535u);
        if (best_size == 0u || best_size >= stored_size) {
            encode_stored_block(data + block_start, block_length, last_block, writer);
        } else {
            encode_dynamic(best_tokens, best_litlen, best_distance, last_block, writer);
        }
        block_start = block_end;
    }
    writer.flush();
    return true;
}

}  // namespace sq_deflate
