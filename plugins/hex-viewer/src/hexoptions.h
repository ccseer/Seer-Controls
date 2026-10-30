#pragma once

#include <string>
#include <vector>

namespace hexviewer {

struct HexOptions {
    std::wstring inputPath;
    std::wstring viewerPath;
    long long maxKb = 64;             // clamped into [1, 1048576]
    bool upper = true;                // hex digit case, default upper
    long long waitTimeoutMs = 30000;  // clamped into [5000, 600000]

    bool valid = false;
    std::wstring error;
};

// Parses the full command line including argv[0], which is skipped. Both the
// "--key=value" and "--key value" spellings are accepted, options may appear
// before or after the two positional arguments (<input> <viewer>), and a bare
// "--" ends the option list so a positional that itself starts with '-' still
// parses. Option names must start with '-' (the '/' spelling is not an option
// prefix, so a relative path such as "/dir/file" stays a positional). Any
// conflict (--case with --uppercase/--lowercase, --uppercase with
// --lowercase), duplicate option, unknown name, empty or non-numeric value,
// or a positional count other than two marks the whole parse invalid.
HexOptions parseCommandLine(const std::vector<std::wstring> &arguments);

}  // namespace hexviewer
