#pragma once

#include <string>
#include "paths.h"

namespace seer_ocr {

// Removes previous outputs for this specific base name to prevent old results from triggering
void cleanupPreviousRun(const OcrOutputPaths &paths);

// Sweeps old outputs (>7 days old) in the base directory (best-effort)
void sweepOldOutputs(const std::wstring &base_dir);

}  // namespace seer_ocr
