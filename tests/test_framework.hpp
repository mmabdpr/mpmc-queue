// SPDX-FileCopyrightText: 2026 Mohammad Mahdi Abdollahpour <git@mohammadmahdi.com>
// SPDX-License-Identifier: CPAL-1.0
//
// The contents of this file are subject to the Common Public Attribution License Version 1.0 (the "License"); you may not use this file except in compliance with the License. You may obtain a copy of the License at https://opensource.org/license/cpal-1-0. The License is based on the Mozilla Public License Version 1.1 but Sections 14 and 15 have been added to cover use of software over a computer network and provide for limited attribution for the Original Developer. In addition, Exhibit A has been modified to be consistent with Exhibit B.
// Software distributed under the License is distributed on an "AS IS" basis, WITHOUT WARRANTY OF ANY KIND, either express or implied. See the License for the specific language governing rights and limitations under the License.
// The Original Code is mpmc_queue, a header-only C++20 bounded queue library.
// The Original Developer is not separately identified and is the Initial Developer. If left blank, the Original Developer is the Initial Developer.
// The Initial Developer of the Original Code is Mohammad Mahdi Abdollahpour. All portions of the code written by Mohammad Mahdi Abdollahpour are Copyright (c) 2026. All Rights Reserved.
// Contributor: Mohammad Mahdi Abdollahpour.
//
// A tiny self-contained test harness.  No network access and no third-party
// dependency: the project can be configured and tested offline.

#pragma once

#include <atomic>
#include <cstdio>
#include <exception>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace mpmc_test {

using test_function = void (*)();

struct test_case {
    const char* name;
    test_function function;
};

inline std::vector<test_case>& registry() {
    static std::vector<test_case> tests;
    return tests;
}

struct registrar {
    registrar(const char* name, test_function function) {
        registry().push_back(test_case{name, function});
    }
};

class failure : public std::runtime_error {
public:
    explicit failure(const std::string& message)
        : std::runtime_error(message) {}
};

inline std::atomic<int> failure_count{0};
inline std::mutex output_mutex;

inline void report_failure(const char* file, int line,
                           const std::string& expression) {
    failure_count.fetch_add(1, std::memory_order_relaxed);
    std::lock_guard<std::mutex> lock(output_mutex);
    std::fprintf(stderr, "    CHECK failed at %s:%d: %s\n", file, line,
                 expression.c_str());
}

inline void report_message(const char* file, int line,
                           const std::string& message) {
    failure_count.fetch_add(1, std::memory_order_relaxed);
    std::lock_guard<std::mutex> lock(output_mutex);
    std::fprintf(stderr, "    failure at %s:%d: %s\n", file, line,
                 message.c_str());
}

template<class A, class B>
void check_equal(const A& actual, const B& expected, const char* actual_text,
                 const char* expected_text, const char* file, int line) {
    if (!(actual == expected)) {
        std::ostringstream stream;
        stream << actual_text << " == " << expected_text
               << " (actual: " << actual << ", expected: " << expected << ')';
        report_failure(file, line, stream.str());
    }
}

inline int run_all(int argc, char** argv) {
    std::string_view filter;
    if (argc > 1) {
        filter = argv[1];
    }

    int ran = 0;
    for (const test_case& test : registry()) {
        if (!filter.empty() && std::string_view{test.name}.find(filter) ==
                                   std::string_view::npos) {
            continue;
        }
        ++ran;
        std::printf("[ RUN      ] %s\n", test.name);
        std::fflush(stdout);
        const int before = failure_count.load(std::memory_order_relaxed);
        try {
            test.function();
        } catch (const std::exception& error) {
            report_message("", 0,
                           std::string{"uncaught exception: "} + error.what());
        } catch (...) {
            report_message("", 0, "uncaught non-standard exception");
        }
        const int after = failure_count.load(std::memory_order_relaxed);
        if (after == before) {
            std::printf("[       OK ] %s\n", test.name);
        } else {
            std::printf("[   FAILED ] %s\n", test.name);
        }
        std::fflush(stdout);
    }

    if (ran == 0) {
        std::fprintf(stderr, "no tests matched the filter\n");
        return 1;
    }
    if (failure_count.load(std::memory_order_relaxed) != 0) {
        std::fprintf(stderr, "%d check(s) failed\n",
                     failure_count.load(std::memory_order_relaxed));
        return 1;
    }
    std::printf("all %d test(s) passed\n", ran);
    return 0;
}

} // namespace mpmc_test

#define MPMC_TEST_CONCAT_IMPL(a, b) a##b
#define MPMC_TEST_CONCAT(a, b) MPMC_TEST_CONCAT_IMPL(a, b)

#define MPMC_TEST(name)                                                        \
    static void MPMC_TEST_CONCAT(test_function_, name)();                      \
    static ::mpmc_test::registrar MPMC_TEST_CONCAT(test_registrar_, name){     \
        #name, &MPMC_TEST_CONCAT(test_function_, name)};                       \
    static void MPMC_TEST_CONCAT(test_function_, name)()

#define CHECK(condition)                                                       \
    do {                                                                       \
        if (!(condition)) {                                                    \
            ::mpmc_test::report_failure(__FILE__, __LINE__, #condition);       \
        }                                                                      \
    } while (false)

#define REQUIRE(condition)                                                     \
    do {                                                                       \
        if (!(condition)) {                                                    \
            throw ::mpmc_test::failure(std::string{__FILE__} + ':' +           \
                                       std::to_string(__LINE__) +              \
                                       ": required: " #condition);             \
        }                                                                      \
    } while (false)

#define CHECK_EQ(actual, expected)                                             \
    ::mpmc_test::check_equal((actual), (expected), #actual, #expected,         \
                             __FILE__, __LINE__)

#define REQUIRE_EQ(actual, expected)                                           \
    do {                                                                       \
        if (!((actual) == (expected))) {                                       \
            throw ::mpmc_test::failure(                                        \
                std::string{__FILE__} + ':' + std::to_string(__LINE__) +       \
                ": required equality: " #actual " == " #expected);             \
        }                                                                      \
    } while (false)

#define CHECK_THROWS_AS(expression, exception_type)                            \
    do {                                                                       \
        bool caught_expected_exception = false;                                \
        bool caught_other_exception = false;                                   \
        try {                                                                  \
            (void)(expression);                                                \
        } catch (const exception_type&) {                                      \
            caught_expected_exception = true;                                  \
        } catch (...) {                                                        \
            caught_other_exception = true;                                     \
        }                                                                      \
        if (caught_other_exception) {                                          \
            ::mpmc_test::report_failure(                                       \
                __FILE__, __LINE__,                                            \
                "unexpected exception type from: " #expression);               \
        } else if (!caught_expected_exception) {                               \
            ::mpmc_test::report_failure(__FILE__, __LINE__,                    \
                                        "expected " #exception_type            \
                                        " from: " #expression);                \
        }                                                                      \
    } while (false)
