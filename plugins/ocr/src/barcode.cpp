#include "barcode.h"

#include <algorithm>
#include <exception>

#include "image_loader.h"

#include <Barcode.h>
#include <ImageView.h>
#include <ReadBarcode.h>

namespace seer_ocr {

bool detectBarcodes(
    const ImageRgb &image,
    std::vector<BarcodeResult> &out_barcodes,
    std::string &out_error)
{
    out_barcodes.clear();
    out_error.clear();

    if (image.empty()) {
        out_error = "empty image buffer";
        return false;
    }

    try {
        // Zero-copy view over the WIC buffer: the 24bpp RGB layout is read
        // in place, so no intermediate conversion is allocated. The default
        // format filter is "all supported symbologies" (QRCode, DataMatrix,
        // Aztec, PDF417, Code128, EAN/UPC, ITF, ...).
        const ZXing::ImageView view(
            image.data.data(), image.width, image.height, ZXing::ImageFormat::RGB);

        ZXing::ReaderOptions opts;
        opts.setTryHarder(true)
            .setTryRotate(true)
            .setTryInvert(true)
            .setTryDownscale(true);

        const ZXing::Barcodes found = ZXing::ReadBarcodes(view, opts);

        out_barcodes.reserve(found.size());
        for (const auto &bc : found) {
            BarcodeResult item;
            item.format = ZXing::ToString(bc.format());
            item.text   = bc.text();
            const auto &pos = bc.position();
            const ZXing::PointI corners[4] = {
                pos.topLeft(), pos.topRight(), pos.bottomRight(), pos.bottomLeft()};
            for (int k = 0; k < 4; ++k) {
                item.box[k][0] = corners[k].x;
                item.box[k][1] = corners[k].y;
            }
            out_barcodes.push_back(std::move(item));
        }
    }
    catch (const std::exception &e) {
        out_error = e.what();
        out_barcodes.clear();
        return false;
    }
    catch (...) {
        out_error = "unknown exception in barcode scan";
        out_barcodes.clear();
        return false;
    }

    std::sort(out_barcodes.begin(), out_barcodes.end(),
              [](const BarcodeResult &a, const BarcodeResult &b) {
                  if (a.box[0][1] != b.box[0][1]) {
                      return a.box[0][1] < b.box[0][1];
                  }
                  return a.box[0][0] < b.box[0][0];
              });

    return true;
}

}  // namespace seer_ocr
