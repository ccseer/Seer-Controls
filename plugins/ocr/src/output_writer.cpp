#include "output_writer.h"
#include "paths.h"

#include <iomanip>
#include <sstream>
#include <fstream>

namespace seer_ocr {

namespace {

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

}  // namespace

std::string formatOcrJson(
    const OcrJsonMeta &meta,
    const std::vector<OcrLineResult> &lines)
{
    std::ostringstream ss;
    ss << "{\n";
    ss << "  \"schema\": \"seer.ocr/1\",\n";
    ss << "  \"source_image\": \"" << escapeJson(meta.source_image) << "\",\n";
    ss << "  \"image\": { \"width\": " << meta.img_width << ", \"height\": " << meta.img_height << " },\n";
    ss << "  \"engine\": {\n";
    ss << "    \"det\": \"" << escapeJson(meta.det_engine) << "\",\n";
    ss << "    \"rec\": \"" << escapeJson(meta.rec_engine) << "\",\n";
    ss << "    \"cls\": \"" << escapeJson(meta.cls_engine) << "\",\n";
    ss << "    \"ort\": \"" << escapeJson(meta.ort_version) << "\",\n";
    ss << "    \"ep\": \"" << escapeJson(meta.ep_used) << "\",\n";
    ss << "    \"fallback\": " << (meta.fallback ? "true" : "false") << "\n";
    ss << "  },\n";
    ss << "  \"lines\": [";

    for (size_t i = 0; i < lines.size(); ++i) {
        const auto &ln = lines[i];
        if (i > 0) ss << ",";
        ss << "\n    {\n";
        ss << "      \"index\": " << ln.index << ",\n";
        ss << "      \"text\": \"" << escapeJson(ln.text) << "\",\n";
        ss << std::fixed << std::setprecision(4);
        ss << "      \"score\": " << ln.score << ",\n";
        ss << "      \"orientation\": " << ln.orientation << ",\n";
        ss << "      \"box\": [";
        for (int k = 0; k < 4; ++k) {
            if (k > 0) ss << ",";
            ss << "[" << ln.box[k][0] << "," << ln.box[k][1] << "]";
        }
        ss << "]\n    }";
    }

    if (!lines.empty()) {
        ss << "\n  ";
    }
    ss << "],\n";
    ss << "  \"elapsed_ms\": " << meta.elapsed_ms << "\n";
    ss << "}\n";

    return ss.str();
}

std::string formatOcrText(const std::vector<OcrLineResult> &lines)
{
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

bool writeOcrJsonAtomically(
    const std::wstring &json_path,
    const OcrJsonMeta &meta,
    const std::vector<OcrLineResult> &lines)
{
    std::string content = formatOcrJson(meta, lines);
    return atomicWriteFile(json_path, content);
}

bool writeOcrTextAtomically(
    const std::wstring &txt_path,
    const std::vector<OcrLineResult> &lines)
{
    std::string content = formatOcrText(lines);
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
