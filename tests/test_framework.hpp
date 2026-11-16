#ifndef METRICS_TEST_FRAMEWORK_HPP_
#define METRICS_TEST_FRAMEWORK_HPP_

// Minimal self-contained test runner: no third-party dependency.

#include <cmath>
#include <functional>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <type_traits>
#include <vector>

#include <string_utils.hpp>

struct TestFailure {
    std::string message;
};

struct TestCase {
    std::string name;
    std::function<void()> fn;
};

inline std::vector<TestCase>& testRegistry() {
    static std::vector<TestCase> registry;
    return registry;
}

struct TestRegistrar {
    TestRegistrar(const char* name, std::function<void()> fn) {
        testRegistry().push_back(TestCase{name, std::move(fn)});
    }
};

template <typename T>
std::string testToString(const T& value) {
    if constexpr (std::is_same_v<T, __uint128_t>) {
        return uint128_t_to_string(value);
    } else if constexpr (std::is_same_v<T, bool>) {
        return value ? "true" : "false";
    } else if constexpr (std::is_floating_point_v<T>) {
        std::ostringstream oss;
        oss << std::setprecision(17) << value;
        return oss.str();
    } else {
        return std::to_string(value);
    }
}

#define TEST_CASE(name)                                             \
    static void name();                                             \
    static TestRegistrar name##_registrar(#name, name);             \
    static void name()

#define FAIL_TEST(msg) \
    throw TestFailure{std::string(__FILE__) + ":" + std::to_string(__LINE__) + ": " + (msg)}

#define CHECK(cond)                                                 \
    do {                                                            \
        if (!(cond)) FAIL_TEST("CHECK(" #cond ") failed");          \
    } while (0)

#define CHECK_EQ(actual, expected)                                                        \
    do {                                                                                  \
        auto _actual = (actual);                                                          \
        auto _expected = (expected);                                                      \
        if (!(_actual == _expected))                                                      \
            FAIL_TEST(std::string("CHECK_EQ(" #actual ", " #expected ") failed: actual ") \
                      + testToString(_actual) + " != expected " + testToString(_expected)); \
    } while (0)

#define CHECK_NEAR(actual, expected, tolerance)                                              \
    do {                                                                                     \
        double _actual = (actual);                                                           \
        double _expected = (expected);                                                       \
        if (!(std::fabs(_actual - _expected) <= (tolerance)))                                \
            FAIL_TEST(std::string("CHECK_NEAR(" #actual ", " #expected ") failed: actual ")  \
                      + testToString(_actual) + " vs expected " + testToString(_expected));   \
    } while (0)

// Runs every registered test; returns the number of failures.
inline int runAllTests() {
    int failed = 0;
    for (const TestCase& test : testRegistry()) {
        try {
            test.fn();
            std::cout << "[PASS] " << test.name << "\n";
        } catch (const TestFailure& failure) {
            ++failed;
            std::cout << "[FAIL] " << test.name << "\n       " << failure.message << "\n";
        } catch (const std::exception& e) {
            ++failed;
            std::cout << "[FAIL] " << test.name << "\n       exception: " << e.what() << "\n";
        }
    }
    std::cout << testRegistry().size() - failed << " passed, " << failed << " failed, "
              << testRegistry().size() << " total\n";
    return failed;
}

#endif // METRICS_TEST_FRAMEWORK_HPP_
