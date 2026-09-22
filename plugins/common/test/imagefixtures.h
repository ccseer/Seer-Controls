#pragma once

#include <windows.h>

#include <string>
#include <vector>

#include "fileprobe.h"
#include "winpath.h"

// Minimal structural image writers used by the tests.
//
// They exist so the validator is exercised against bytes that were produced
// deliberately, including malformed ones, instead of against whatever happens to
// be on the test machine.
namespace imagefixtures {

inline unsigned long pngCrc(const unsigned char *data, const size_t count)
{
    unsigned long crc = 0xFFFFFFFFUL;
    for (size_t index = 0; index < count; ++index) {
        crc ^= data[index];
        for (int bit = 0; bit < 8; ++bit) {
            crc = (crc & 1) ? (0xEDB88320UL ^ (crc >> 1)) : (crc >> 1);
        }
    }
    return crc ^ 0xFFFFFFFFUL;
}

inline void appendBigEndian(std::string *sink,
                            const unsigned long long value,
                            const size_t count)
{
    for (size_t index = 0; index < count; ++index) {
        const size_t shift = (count - 1 - index) * 8;
        sink->push_back(static_cast<char>((value >> shift) & 0xFF));
    }
}

inline void appendLittleEndian(std::string *sink,
                               const unsigned long long value,
                               const size_t count)
{
    for (size_t index = 0; index < count; ++index) {
        sink->push_back(static_cast<char>((value >> (8 * index)) & 0xFF));
    }
}

inline void appendChunk(std::string *sink,
                        const char *type,
                        const std::string &data)
{
    appendBigEndian(sink, data.size(), 4);
    const size_t start = sink->size();
    sink->append(type, 4);
    sink->append(data);
    const auto crc = pngCrc(
        reinterpret_cast<const unsigned char *>(sink->data() + start),
        sink->size() - start);
    appendBigEndian(sink, crc, 4);
}

// A PNG with the requested dimensions, alpha and payload size. The extra filler
// goes into a text chunk so the file size can be varied without breaking it.
inline std::string makePng(const unsigned width,
                           const unsigned height,
                           const bool alpha,
                           const size_t filler = 0,
                           const bool withIcc = false,
                           const bool withExif = false,
                           const bool animated = false)
{
    std::string bytes;
    bytes.append("\x89PNG\r\n\x1a\n", 8);

    std::string header;
    appendBigEndian(&header, width, 4);
    appendBigEndian(&header, height, 4);
    header.push_back(8);                                 // bit depth
    header.push_back(static_cast<char>(alpha ? 6 : 2));  // colour type
    header.push_back(0);
    header.push_back(0);
    header.push_back(0);
    appendChunk(&bytes, "IHDR", header);

    if (withIcc) {
        std::string profile;
        profile.push_back(0);
        profile.push_back(0);
        profile.push_back(0);
        profile.push_back(0);
        appendChunk(&bytes, "iCCP", profile);
    }
    if (withExif) {
        appendChunk(&bytes, "eXIf", std::string("MM\0*\0\0\0\10", 10));
    }
    if (animated) {
        std::string control;
        appendBigEndian(&control, 2, 4);
        appendBigEndian(&control, 0, 4);
        appendChunk(&bytes, "acTL", control);
    }
    if (filler > 0) {
        appendChunk(&bytes, "tEXt", std::string(filler, 'x'));
    }
    appendChunk(&bytes, "IDAT", std::string(filler / 2 + 4, 'i'));
    appendChunk(&bytes, "IEND", std::string());
    return bytes;
}

inline std::string makeJpeg(const unsigned width,
                            const unsigned height,
                            const unsigned orientation = 0,
                            const bool withIcc = false,
                            const size_t filler = 0)
{
    std::string bytes;
    bytes.push_back(static_cast<char>(0xFF));
    bytes.push_back(static_cast<char>(0xD8));   // SOI

    const auto segment = [&bytes](const unsigned char marker,
                                  const std::string &payload) {
        bytes.push_back(static_cast<char>(0xFF));
        bytes.push_back(static_cast<char>(marker));
        const auto length = static_cast<unsigned short>(payload.size() + 2);
        bytes.push_back(static_cast<char>((length >> 8) & 0xFF));
        bytes.push_back(static_cast<char>(length & 0xFF));
        bytes.append(payload);
    };

    segment(0xE0, std::string("JFIF\0\x01\x01\0\0\x01\0\x01\0\0", 14));

    if (orientation != 0) {
        std::string exif("Exif\0\0", 6);
        exif.append("MM\0*", 4);
        exif.append("\0\0\0\x08", 4);          // IFD offset
        exif.append("\0\x01", 2);              // one entry
        exif.append("\x01\x12", 2);            // orientation tag
        exif.append("\0\x03", 2);              // type SHORT
        exif.append("\0\0\0\x01", 4);          // count
        exif.push_back(static_cast<char>((orientation >> 8) & 0xFF));
        exif.push_back(static_cast<char>(orientation & 0xFF));
        exif.append("\0\0", 2);
        exif.append("\0\0\0\0", 4);            // next IFD
        segment(0xE1, exif);
    }
    if (withIcc) {
        std::string icc("ICC_PROFILE\0", 12);
        icc.append("\x01\x01", 2);
        icc.append(16, 'p');
        segment(0xE2, icc);
    }
    if (filler > 0) {
        segment(0xFE, std::string(filler, 'c'));
    }

    std::string frame;
    frame.push_back(8);                        // precision
    frame.push_back(static_cast<char>((height >> 8) & 0xFF));
    frame.push_back(static_cast<char>(height & 0xFF));
    frame.push_back(static_cast<char>((width >> 8) & 0xFF));
    frame.push_back(static_cast<char>(width & 0xFF));
    frame.push_back(3);                        // components
    for (int component = 0; component < 3; ++component) {
        frame.push_back(static_cast<char>(component + 1));
        frame.push_back(0x11);
        frame.push_back(0);
    }
    segment(0xC0, frame);

    std::string scan;
    scan.push_back(3);
    for (int component = 0; component < 3; ++component) {
        scan.push_back(static_cast<char>(component + 1));
        scan.push_back(0);
    }
    scan.push_back(0);
    scan.push_back(63);
    scan.push_back(0);
    segment(0xDA, scan);

    bytes.append(static_cast<size_t>(filler) * 2 + 16, '\x5A');
    bytes.push_back(static_cast<char>(0xFF));
    bytes.push_back(static_cast<char>(0xD9));   // EOI
    return bytes;
}

inline std::string makeWebp(const unsigned width,
                            const unsigned height,
                            const bool lossless = true,
                            const bool alpha = false,
                            const size_t filler = 0)
{
    std::string chunkType = lossless ? "VP8L" : "VP8 ";
    std::string payload;
    if (lossless) {
        payload.push_back(0x2F);
        unsigned long long bits = (width - 1) & 0x3FFFULL;
        bits |= static_cast<unsigned long long>((height - 1) & 0x3FFF) << 14;
        if (alpha) {
            bits |= 1ULL << 28;
        }
        appendLittleEndian(&payload, bits, 4);
        payload.append(filler, 'l');
    }
    else {
        payload.append(3, '\0');
        payload.push_back(static_cast<char>(0x9D));
        payload.push_back(0x01);
        payload.push_back(0x2A);
        appendLittleEndian(&payload, width & 0x3FFF, 2);
        appendLittleEndian(&payload, height & 0x3FFF, 2);
        payload.append(filler, 'v');
    }

    std::string chunks;
    chunks.append(chunkType);
    appendLittleEndian(&chunks, payload.size(), 4);
    chunks.append(payload);
    if (payload.size() % 2 != 0) {
        chunks.push_back('\0');
    }

    std::string bytes("RIFF", 4);
    appendLittleEndian(&bytes, chunks.size() + 4, 4);
    bytes.append("WEBP", 4);
    bytes.append(chunks);
    return bytes;
}

inline bool writeFixture(const std::wstring &path, const std::string &bytes)
{
    FileProbe::makeDirectoryTree(WinPath::parentDirectory(path));
    return FileProbe::writeTextFile(path, bytes);
}

}  // namespace imagefixtures
