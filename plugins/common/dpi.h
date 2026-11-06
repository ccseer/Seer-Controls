#pragma once

#include <windows.h>

// A Control helper is a standalone process with no application manifest, so
// Windows treats it as DPI-unaware and bitmap-stretches every dialog it shows.
// At 150% scaling that is what makes a message box look blurry. Declaring
// per-monitor v2 before the first window exists makes the text render at native
// resolution; these helpers position nothing themselves, so no layout depends on
// the previous virtualised metrics.
namespace WinDpi {

inline void enablePerMonitorV2()
{
    // Returns FALSE when awareness is already set, for example by a manifest.
    // There is nothing to recover from in that case: the process keeps the
    // awareness it already has, which is at worst the previous behaviour.
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
}

}  // namespace WinDpi
