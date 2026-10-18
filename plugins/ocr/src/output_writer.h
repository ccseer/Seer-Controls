#pragma once

#include <string>
#include <vector>
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
    int64_t elapsed_ms = 0;
};

// Generates RFC 8259 compliant JSON string according to seer.ocr/1 schema
std::string formatOcrJson(
    const OcrJsonMeta &meta,
    const std::vector<OcrLineResult> &lines);

// Generates plain text (UTF-8 without BOM + CRLF)
std::string formatOcrText(const std::vector<OcrLineResult> &lines);

// Writes JSON atomically via .tmp -> MoveFileEx
bool writeOcrJsonAtomically(
    const std::wstring &json_path,
    const OcrJsonMeta &meta,
    const std::vector<OcrLineResult> &lines);

// Writes text atomically via .tmp -> MoveFileEx
bool writeOcrTextAtomically(
    const std::wstring &txt_path,
    const std::vector<OcrLineResult> &lines);

// Writes error explanation text atomically to txt_path so waiting window displays it
bool writeOcrErrorAtomically(
    const std::wstring &txt_path,
    const std::string &error_message);

// Writes session diagnostic log
void writeOcrLog(
    const std::wstring &log_path,
    const std::string &content);

}  // namespace seer_ocr
