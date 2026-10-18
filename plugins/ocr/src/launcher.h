#pragma once

#include <string>

namespace seer_ocr {

// Launches SeerViewer.exe with --wait-file and target txt path
// Returns true if the process was launched successfully
bool launchSeerViewerWaiting(
    const std::wstring &seerviewer_exe,
    const std::wstring &txt_path,
    int wait_timeout_ms);

}  // namespace seer_ocr
