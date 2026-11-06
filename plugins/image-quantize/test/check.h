#ifndef SQ_TEST_CHECK_H
#define SQ_TEST_CHECK_H

#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace sqtest {

inline int& failure_count() {
    static int count = 0;
    return count;
}

inline int& check_count() {
    static int count = 0;
    return count;
}

inline void report_failure(const char* expression, const char* file, int line) {
    ++failure_count();
    std::fprintf(stderr, "FAIL %s:%d: %s\n", file, line, expression);
}

inline bool check_true(bool value, const char* expression, const char* file, int line) {
    ++check_count();
    if (!value) {
        report_failure(expression, file, line);
    }
    return value;
}

inline bool check_near(double actual, double expected, double tolerance,
                       const char* expression, const char* file, int line) {
    ++check_count();
    const bool ok = std::fabs(actual - expected) <= tolerance;
    if (!ok) {
        ++failure_count();
        std::fprintf(stderr, "FAIL %s:%d: %s (actual %.10f expected %.10f tol %g)\n",
                     file, line, expression, actual, expected, tolerance);
    }
    return ok;
}

inline int finish() {
    std::printf("%d checks, %d failures\n", check_count(), failure_count());
    return failure_count() == 0 ? 0 : 1;
}

}  // namespace sqtest

#define SQ_CHECK(expr) sqtest::check_true((expr), #expr, __FILE__, __LINE__)
#define SQ_CHECK_EQ(actual, expected) \
    sqtest::check_true((actual) == (expected), #actual " == " #expected, __FILE__, __LINE__)
#define SQ_CHECK_NEAR(actual, expected, tolerance) \
    sqtest::check_near((actual), (expected), (tolerance), #actual, __FILE__, __LINE__)

#endif  // SQ_TEST_CHECK_H
