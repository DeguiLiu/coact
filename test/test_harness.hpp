// coact minimal test harness.
// Host-only, heap allowed (never compiled into target firmware).
// No exceptions, no RTTI, no external framework (Catch2 unavailable on host).
#pragma once

#include <cstdio>
#include <cstdint>
#include <string>
#include <vector>

namespace coact_test {

struct Stats {
    int passed = 0;
    int failed = 0;
};

inline Stats& stats() noexcept {
    static Stats s;
    return s;
}

struct TestCase {
    const char* name;
    void (*fn)();
};

inline std::vector<TestCase>& registry() {
    static std::vector<TestCase> r;
    return r;
}

struct Register {
    Register(const char* name, void (*fn)()) {
        registry().push_back(TestCase{name, fn});
    }
};

inline void report_fail(const char* file, int line, const char* expr) {
    stats().failed++;
    std::printf("FAIL %s:%d: %s\n", file, line, expr);
}

inline void report_require(const char* file, int line, const char* expr) {
    stats().failed++;
    std::printf("FATAL %s:%d: REQUIRE(%s)\n", file, line, expr);
}

inline int run_all() {
    int total = 0;
    for (const TestCase& tc : registry()) {
        total++;
        const int before = stats().failed;
        tc.fn();
        if (stats().failed == before) {
            std::printf("[PASS] %s\n", tc.name);
        }
        else {
            std::printf("[FAIL] %s\n", tc.name);
        }
    }
    std::printf("\n%d test(s), %d failed, %d passed assertion(s)\n",
                total, stats().failed, stats().passed);
    return (stats().failed == 0) ? 0 : 1;
}

}  // namespace coact_test

#define COACT_TEST(name)                                          \
    static void name();                                           \
    static ::coact_test::Register coact_register_##name(#name, &name); \
    static void name()

#define CHECK(cond)                                               \
    do {                                                          \
        if (cond) {                                               \
            ::coact_test::stats().passed++;                       \
        }                                                         \
        else {                                                    \
            ::coact_test::report_fail(__FILE__, __LINE__, #cond); \
        }                                                         \
    } while (0)

#define CHECK_EQ(a, b)                                            \
    do {                                                          \
        const auto& coact_va = (a);                               \
        const auto& coact_vb = (b);                               \
        if (coact_va == coact_vb) {                               \
            ::coact_test::stats().passed++;                       \
        }                                                         \
        else {                                                    \
            ::coact_test::report_fail(__FILE__, __LINE__,         \
                #a " == " #b);                                    \
        }                                                         \
    } while (0)

#define CHECK_NE(a, b)                                            \
    do {                                                          \
        if ((a) != (b)) {                                         \
            ::coact_test::stats().passed++;                       \
        }                                                         \
        else {                                                    \
            ::coact_test::report_fail(__FILE__, __LINE__,         \
                #a " != " #b);                                    \
        }                                                         \
    } while (0)

// REQUIRE aborts the current test function on failure.
#define REQUIRE(cond)                                             \
    do {                                                          \
        if (!(cond)) {                                            \
            ::coact_test::report_require(__FILE__, __LINE__, #cond); \
            return;                                               \
        }                                                         \
    } while (0)

#define REQUIRE_EQ(a, b)                                          \
    do {                                                          \
        const auto& coact_va = (a);                               \
        const auto& coact_vb = (b);                               \
        if (coact_va != coact_vb) {                               \
            ::coact_test::report_require(__FILE__, __LINE__,      \
                #a " == " #b);                                    \
            return;                                               \
        }                                                         \
        ::coact_test::stats().passed++;                           \
    } while (0)

#define COACT_TEST_MAIN()                                         \
    int main() {                                                  \
        return ::coact_test::run_all();                           \
    }
