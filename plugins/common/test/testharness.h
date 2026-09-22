#pragma once

#include <iostream>
#include <string>

// Minimal check/summarize harness shared by the native Control test targets.
// Function-local statics give every test executable its own counter without
// requiring a separate object file.
namespace TestHarness {

inline int &failureCount()
{
    static int failures = 0;
    return failures;
}

inline void check(const bool condition, const std::string &message)
{
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failureCount();
    }
}

inline void section(const std::string &name)
{
    std::cout << "[section] " << name << '\n';
}

inline int summarize(const char *name)
{
    if (failureCount() == 0) {
        std::cout << name << ": all checks passed\n";
        return 0;
    }
    std::cerr << name << ": " << failureCount() << " check(s) failed\n";
    return 1;
}

}  // namespace TestHarness
