#include "output_writer.h"
#include "paths.h"

#include <iomanip>
#include <sstream>
#include <fstream>

namespace seer_ocr {

namespace {

// Exactly 50 '=', the width of the highlighted barcode block in the txt view
constexpr const char *kBarcodeRule = "==================================================";

std::string escapeJson(const std::string &s)
{
    std::ostringstream o;
    for (char c : s) {
        switch (c) {
        case '"':  o << "\\\""; break;
        case '\\': o << "\\\\"; break;
        case '\b': o << "\\b";  break;
        case '\f': o << "\\f";  break;
        case '\n': o << "\\n";  break;
        case '\r': o << "\\r";  break;
        case '\t': o << "\\t";  break;
        default:
            if (static_cast<unsigned char>(c) < 32) {
                o << "\\u" << std::hex << std::setw(4) << std::setfill('0') << static_cast<int>(c)
                  << std::dec << std::setfill(' ');
            }
            else {
                o << c;
            }
            break;
        }
    }
    return o.str();
}

// Emits the [[x,y] x4] corner quad shared by OcrLineResult and BarcodeResult.
void appendBoxQuad(std::ostringstream &ss, const int box[4][2])
{
    ss << '[';
    for (int k = 0; k < 4; ++k) {
        if (k > 0) ss << ",";
        ss << "[" << box[k][0] << "," << box[k][1] << "]";
    }
    ss << ']';
}

// Emits one array of objects with the fixed seer.ocr/1 framing: comma-separated
// entries indented one level, closing bracket back at the member indentation.
template <typename Item, typename EmitItem>
void appendObjectArray(
    std::ostringstream &ss,
    const char *key,
    const std::vector<Item> &items,
    EmitItem emit_item)
{
    ss << "  \"" << key << "\": [";
    for (size_t i = 0; i < items.size(); ++i) {
        if (i > 0) {
            ss << ",";
        }
        ss << "\n    {\n";
        emit_item(ss, items[i]);
        ss << "\n    }";
    }
    if (!items.empty()) {
        ss << "\n  ";
    }
    ss << "],\n";
}

}  // namespace

std::string formatOcrJson(
    const OcrJsonMeta &meta,
    const std::vector<OcrLineResult> &lines,
    const std::vector<BarcodeResult> &barcodes)
{
    std::ostringstream ss;
    ss << "{\n";
    ss << "  \"schema\": \"seer.ocr/1\",\n";
    ss << "  \"source_image\": \"" << escapeJson(meta.source_image) << "\",\n";
    ss << "  \"image\": { \"width\": " << meta.img_width << ", \"height\": " << meta.img_height << " },\n";

    // The key is only meaningful when the scan actually ran; --barcode=0
    // omits it so consumers can tell "disabled" from "no barcodes found".
    if (meta.barcode_scan) {
        appendObjectArray(ss, "barcodes", barcodes,
                          [](std::ostringstream &out, const BarcodeResult &bc) {
                              out << "      \"format\": \"" << escapeJson(bc.format) << "\",\n";
                              out << "      \"text\": \"" << escapeJson(bc.text) << "\",\n";
                              out << "      \"box\": ";
                              appendBoxQuad(out, bc.box);
                          });
    }

    ss << "  \"engine\": {\n";
    ss << "    \"det\": \"" << escapeJson(meta.det_engine) << "\",\n";
    ss << "    \"rec\": \"" << escapeJson(meta.rec_engine) << "\",\n";
    ss << "    \"cls\": \"" << escapeJson(meta.cls_engine) << "\",\n";
    ss << "    \"ort\": \"" << escapeJson(meta.ort_version) << "\",\n";
    ss << "    \"ep\": \"" << escapeJson(meta.ep_used) << "\",\n";
    ss << "    \"fallback\": " << (meta.fallback ? "true" : "false") << "\n";
    ss << "  },\n";

    appendObjectArray(ss, "lines", lines,
                      [](std::ostringstream &out, const OcrLineResult &ln) {
                          out << "      \"index\": " << ln.index << ",\n";
                          out << "      \"text\": \"" << escapeJson(ln.text) << "\",\n";
                          out << "      \"score\": " << std::fixed << std::setprecision(4)
                              << ln.score << ",\n";
                          out << "      \"orientation\": " << ln.orientation << ",\n";
                          out << "      \"box\": ";
                          appendBoxQuad(out, ln.box);
                      });

    ss << "  \"elapsed_ms\": " << meta.elapsed_ms << "\n";
    ss << "}\n";

    return ss.str();
}

std::string formatOcrText(
    const std::vector<OcrLineResult> &lines,
    const std::vector<BarcodeResult> &barcodes)
{
    // No barcodes: keep the historical layout byte-for-byte so a plain-text
    // run looks exactly like it did before the barcode stage existed.
    if (barcodes.empty()) {
        if (lines.empty()) {
            return "No text detected in the current view.\r\n";
        }

        std::string text;
        for (const auto &ln : lines) {
            text += ln.text;
            text += "\r\n";
        }
        return text;
    }

    std::string text;
    for (const auto &bc : barcodes) {
        text += kBarcodeRule;
        text += "\r\n";
        text += "[QR Code / Barcode]\r\n";
        text += "Type: ";
        text += bc.format;
        text += "\r\n";
        text += "Content: ";
        text += bc.text;
        text += "\r\n";
        text += kBarcodeRule;
        text += "\r\n\r\n";
    }

    if (!lines.empty()) {
        text += "[OCR Text]\r\n";
        for (const auto &ln : lines) {
            text += ln.text;
            text += "\r\n";
        }
    }

    return text;
}

bool writeOcrJsonAtomically(
    const std::wstring &json_path,
    const OcrJsonMeta &meta,
    const std::vector<OcrLineResult> &lines,
    const std::vector<BarcodeResult> &barcodes)
{
    std::string content = formatOcrJson(meta, lines, barcodes);
    return atomicWriteFile(json_path, content);
}

bool writeOcrTextAtomically(
    const std::wstring &txt_path,
    const std::vector<OcrLineResult> &lines,
    const std::vector<BarcodeResult> &barcodes)
{
    std::string content = formatOcrText(lines, barcodes);
    return atomicWriteFile(txt_path, content);
}

bool writeOcrErrorAtomically(
    const std::wstring &txt_path,
    const std::string &error_message)
{
    std::string content = "OCR Failed:\r\n" + error_message + "\r\n";
    return atomicWriteFile(txt_path, content);
}

void writeOcrLog(
    const std::wstring &log_path,
    const std::string &content)
{
    std::ofstream ofs(log_path, std::ios::app | std::ios::binary);
    if (ofs.is_open()) {
        ofs << content << "\r\n";
    }
}

}  // namespace seer_ocr
