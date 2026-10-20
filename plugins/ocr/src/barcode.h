#pragma once

#include <string>
#include <vector>

namespace seer_ocr {

struct ImageRgb;

struct BarcodeResult {
    // ZXing symbology name, e.g. "QRCode", "DataMatrix", "Code128"
    std::string format;
    // Decoded UTF-8 payload
    std::string text;
    // [[x,y] x4] in screenshot px, order TL, TR, BR, BL (same as OcrLineResult)
    int box[4][2] = {{0,0},{0,0},{0,0},{0,0}};
};

// Scans the RGB image for 1D/2D barcodes and QR codes via the vendored
// ZXing-C++ library. Returns the detected symbols sorted into a top-to-bottom,
// left-to-right reading order. Returns false (with out_error set, no throw)
// only when the scan itself could not run; "nothing found" is success with an
// empty vector.
bool detectBarcodes(
    const ImageRgb &image,
    std::vector<BarcodeResult> &out_barcodes,
    std::string &out_error);

}  // namespace seer_ocr
