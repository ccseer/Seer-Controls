#pragma once

#include <string>
#include <vector>

namespace seer_ocr {

enum class ExecutionProvider {
    Auto,
    Cpu,
    Dml
};

enum class ForceDmlFail {
    None,
    Init,
    Run
};

struct OcrCliOptions {
    std::wstring screenshot_path;
    std::wstring seerviewer_exe;
    std::wstring input_file;

    ExecutionProvider ep     = ExecutionProvider::Auto;
    bool enable_cls          = true;
    // ZXing barcode/QR detection on the screenshot, aggregated into the
    // .ocr.txt/.ocr.json outputs. On by default; --barcode=0 skips the scan
    // and omits the JSON "barcodes" key entirely.
    bool enable_barcode      = true;
    int max_side             = 4096;
    // Matches SeerViewer's pending-job default; the shipped manifest passes an
    // explicit 300000 because server-model cold start needs the headroom.
    int wait_timeout_ms      = 120000;
    bool console             = false;
    ForceDmlFail force_fail  = ForceDmlFail::None;

    bool valid               = false;
    std::wstring error_msg;
};

OcrCliOptions parseCommandLine(int argc, wchar_t *argv[]);
OcrCliOptions parseCommandLine(const std::vector<std::wstring> &args);

}  // namespace seer_ocr
