#pragma once

// Exit codes shared by the native Control helpers.
//
// Every manifest lists success_exit_codes [0] only, so any nonzero value is
// reported by the host as a failed invocation and never triggers Seer's
// close-after-success behavior.
namespace WinExit {

inline constexpr int kOk = 0;
inline constexpr int kFailure = 1;
inline constexpr int kUsage = 2;
inline constexpr int kMissingDependency = 3;
inline constexpr int kCancelled = 4;
inline constexpr int kUnsupported = 5;
inline constexpr int kNotFound = 6;
inline constexpr int kTimeout = 7;
inline constexpr int kBusy = 8;

}  // namespace WinExit
