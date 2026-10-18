#pragma once

#include <string>

namespace seer_ocr {

struct OcrOutputPaths {
    std::wstring base_dir;
    std::wstring base_name;
    std::wstring txt_path;
    std::wstring json_path;
    std::wstring log_path;
    std::wstring txt_tmp_path;
    std::wstring json_tmp_path;
};

// Sanitizes input_file to a safe, valid Windows filename (max 80 chars, no reserved device names)
std::wstring sanitizeBaseName(const std::wstring &input_file);

// Output base name for a target file: an 8-hex-character fingerprint of its
// directory, then " - ", then sanitizeBaseName(). Two identically named files
// in different folders therefore get distinct output sets instead of
// overwriting each other's .ocr.txt / .ocr.json / .ocr.log.
std::wstring outputBaseName(const std::wstring &input_file);

// Resolves %TEMP%\SeerOCR and constructs all output paths
OcrOutputPaths resolveOutputPaths(const std::wstring &input_file);

// Gets the directory containing the currently running executable
std::wstring getModuleDirectory();

// Writes data to target_path atomically using tmp file and MoveFileEx
bool atomicWriteFile(const std::wstring &target_path, const std::string &data);

}  // namespace seer_ocr
