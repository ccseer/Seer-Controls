#pragma once

#include <string>
#include <vector>
#include "barcode.h"
#include "geometry.h"

namespace seer_ocr {

struct OcrJsonMeta {
    std::string source_image;
    int img_width   = 0;
    int img_height  = 0;
    std::string det_engine = "PP-OCRv5_server_det";
    std::string rec_engine = "PP-OCRv5_server_rec";
    std::string cls_engine = "PP-LCNet_x1_0_textline_ori";
    std::string ort_version = "1.22.1";
    std::string ep_used = "dml";
    bool fallback   = false;
    // false (--barcode=0): the scan never ran, so the JSON omits "barcodes"
    // entirely; true: the key is present, possibly with an empty array.
    bool barcode_scan = true;
    int64_t elapsed_ms = 0;
};

// Generates RFC 8259 compliant JSON string according to seer.ocr/1 schema
std::string formatOcrJson(
    const OcrJsonMeta &meta,
    const std::vector<OcrLineResult> &lines,
    const std::vector<BarcodeResult> &barcodes);

// Generates plain text (UTF-8 without BOM + CRLF). When barcodes were found
// they are placed in a highlighted block on top of the OCR text; with no
// barcodes the output is identical to the pre-barcode format.
std::string formatOcrText(
    const std::vector<OcrLineResult> &lines,
    const std::vector<BarcodeResult> &barcodes);

// Writes JSON atomically via .tmp -> MoveFileEx
bool writeOcrJsonAtomically(
    const std::wstring &json_path,
    const OcrJsonMeta &meta,
    const std::vector<OcrLineResult> &lines,
    const std::vector<BarcodeResult> &barcodes);

// Writes text atomically via .tmp -> MoveFileEx
bool writeOcrTextAtomically(
    const std::wstring &txt_path,
    const std::vector<OcrLineResult> &lines,
    const std::vector<BarcodeResult> &barcodes);

// Writes error explanation text atomically to txt_path so waiting window displays it
bool writeOcrErrorAtomically(
    const std::wstring &txt_path,
    const std::string &error_message);

// Writes session diagnostic log
void writeOcrLog(
    const std::wstring &log_path,
    const std::string &content);

}  // namespace seer_ocr
