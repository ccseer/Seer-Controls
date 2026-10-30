#pragma once

#include <string>

namespace hexviewer {

// Starts SeerViewer.exe in its pending state: --wait-file makes it show the
// loading UI until the dump file appears. CreateProcessW first, the shell
// fallback second; both are fire-and-forget, matching the detached host model.
bool launchSeerViewerWaiting(const std::wstring &viewerPath,
                             const std::wstring &txtPath,
                             long long waitTimeoutMs);

}  // namespace hexviewer
