# SPDX-FileCopyrightText: 2026 Mohammad Mahdi Abdollahpour <git@mohammadmahdi.com>
# SPDX-License-Identifier: CPAL-1.0
#
# The contents of this file are subject to the Common Public Attribution
# License Version 1.0 (the "License"); you may not use this file except in
# compliance with the License. You may obtain a copy of the License at
# https://opensource.org/license/cpal-1-0.
# The License is based on the Mozilla Public License Version 1.1 but
# Sections 14 and 15 have been added to cover use of software over a
# computer network and provide for limited attribution for the Original
# Developer. In addition, Exhibit A has been modified to be consistent
# with Exhibit B.
#
# Software distributed under the License is distributed on an "AS IS"
# basis, WITHOUT WARRANTY OF ANY KIND, either express or implied. See the
# License for the specific language governing rights and limitations
# under the License.
#
# The Original Code is mpmc_queue, a header-only C++20 bounded queue
# library.
# The Original Developer is not separately identified and is the Initial
# Developer. If left blank, the Original Developer is the Initial
# Developer.
# The Initial Developer of the Original Code is Mohammad Mahdi
# Abdollahpour. All portions of the code written by Mohammad Mahdi
# Abdollahpour are Copyright (c) 2026. All Rights Reserved.
# Contributor: Mohammad Mahdi Abdollahpour.
#
find_package(Threads REQUIRED)
include(CheckCXXSourceCompiles)

set(MPMC_TEST_DEFINITIONS
    MPMC_HAS_LOCK_FREE_16_BYTE_CELL=$<BOOL:${MPMC_HAS_LOCK_FREE_16_BYTE_CELL}>
    MPMC_ENABLE_TEST_HOOKS)

# Core unit tests.  They use the widest lock-free cell the target supports
# (wide_value/wide_index in test_cell_config.hpp), so this executable still
# builds and runs on 8-byte-only targets.
add_executable(mpmc_tests
    test_main.cpp
    test_basic.cpp
    test_edge_cases.cpp)
target_link_libraries(mpmc_tests PRIVATE mpmc::mpmc_queue Threads::Threads)
target_compile_features(mpmc_tests PRIVATE cxx_std_20)
target_compile_definitions(mpmc_tests PRIVATE ${MPMC_TEST_DEFINITIONS})
target_compile_options(mpmc_tests PRIVATE ${MPMC_WARNING_FLAGS})
set_target_properties(mpmc_tests PROPERTIES CXX_EXTENSIONS OFF)
add_test(NAME mpmc_tests COMMAND mpmc_tests)
set_tests_properties(mpmc_tests PROPERTIES TIMEOUT 120 LABELS unit)

# Stress and concurrency-matrix tests run in their own executable/CTest group
# so a timeout in one group cannot mask or hang the unit suite.
add_executable(mpmc_stress_tests
    test_main.cpp
    test_stress.cpp
    test_concurrency_matrix.cpp
    test_concurrency_modes.cpp)
target_link_libraries(mpmc_stress_tests PRIVATE
    mpmc::mpmc_queue Threads::Threads)
target_compile_features(mpmc_stress_tests PRIVATE cxx_std_20)
target_compile_definitions(mpmc_stress_tests PRIVATE ${MPMC_TEST_DEFINITIONS})
target_compile_options(mpmc_stress_tests PRIVATE ${MPMC_WARNING_FLAGS})
set_target_properties(mpmc_stress_tests PROPERTIES CXX_EXTENSIONS OFF)
add_test(NAME mpmc_stress_tests COMMAND mpmc_stress_tests)
set_tests_properties(mpmc_stress_tests PROPERTIES TIMEOUT 180 LABELS stress)

# Shared-memory and fork/IPC tests are isolated for the same reason.
add_executable(mpmc_shared_memory_tests
    test_main.cpp
    test_shared_memory.cpp)
target_link_libraries(mpmc_shared_memory_tests PRIVATE
    mpmc::mpmc_queue Threads::Threads)
target_compile_features(mpmc_shared_memory_tests PRIVATE cxx_std_20)
target_compile_definitions(mpmc_shared_memory_tests PRIVATE
    ${MPMC_TEST_DEFINITIONS})
target_compile_options(mpmc_shared_memory_tests PRIVATE ${MPMC_WARNING_FLAGS})
set_target_properties(mpmc_shared_memory_tests PROPERTIES CXX_EXTENSIONS OFF)
add_test(NAME mpmc_shared_memory_tests COMMAND mpmc_shared_memory_tests)
set_tests_properties(mpmc_shared_memory_tests PROPERTIES
    TIMEOUT 180 LABELS ipc)

# unique_ptr_queue stores a pointer plus a sequence marker, so it needs a
# 16-byte cell.  Build its tests only where such a cell is lock-free.
if(MPMC_HAS_LOCK_FREE_16_BYTE_CELL)
    add_executable(mpmc_unique_ptr_tests
        test_main.cpp
        test_unique_ptr.cpp)
    target_link_libraries(mpmc_unique_ptr_tests PRIVATE
        mpmc::mpmc_queue Threads::Threads)
    target_compile_features(mpmc_unique_ptr_tests PRIVATE cxx_std_20)
    target_compile_definitions(mpmc_unique_ptr_tests PRIVATE
        ${MPMC_TEST_DEFINITIONS})
    target_compile_options(mpmc_unique_ptr_tests PRIVATE ${MPMC_WARNING_FLAGS})
    set_target_properties(mpmc_unique_ptr_tests PROPERTIES CXX_EXTENSIONS OFF)
    add_test(NAME mpmc_unique_ptr_tests COMMAND mpmc_unique_ptr_tests)
    set_tests_properties(mpmc_unique_ptr_tests PROPERTIES
        TIMEOUT 120 LABELS unit)
endif()

if(CMAKE_CXX_COMPILER_ID MATCHES "^(GNU|Clang|AppleClang)$")
    # The allocation-failure test replaces global operator new/delete. TSan's
    # runtime also provides those symbols, so the target cannot link with
    # -fsanitize=thread. ASan covers this test in the sanitizer job.
    set(MPMC_NO_EXCEPTIONS_ENABLED ON)
    if("${CMAKE_CXX_FLAGS} ${CMAKE_CXX_FLAGS_DEBUG}" MATCHES
       "-fsanitize=thread")
        set(MPMC_NO_EXCEPTIONS_ENABLED OFF)
    endif()
    if(MPMC_NO_EXCEPTIONS_ENABLED)
        add_executable(mpmc_no_exceptions no_exceptions.cpp)
        target_link_libraries(mpmc_no_exceptions PRIVATE
            mpmc::mpmc_queue Threads::Threads)
        target_compile_features(mpmc_no_exceptions PRIVATE cxx_std_20)
        target_compile_definitions(mpmc_no_exceptions PRIVATE
            ${MPMC_TEST_DEFINITIONS})
        target_compile_options(mpmc_no_exceptions PRIVATE
            ${MPMC_WARNING_FLAGS} -fno-exceptions)
        set_target_properties(mpmc_no_exceptions PROPERTIES CXX_EXTENSIONS OFF)
        add_test(NAME mpmc_no_exceptions COMMAND mpmc_no_exceptions)
        set_tests_properties(mpmc_no_exceptions PROPERTIES
            TIMEOUT 60 LABELS unit)
    endif()
endif()

# Probe C++23 <expected> support without changing the directory language mode.
set(MPMC_SAVED_CXX_STANDARD "${CMAKE_CXX_STANDARD}")
set(MPMC_SAVED_CXX_STANDARD_REQUIRED "${CMAKE_CXX_STANDARD_REQUIRED}")
set(MPMC_SAVED_CXX_EXTENSIONS "${CMAKE_CXX_EXTENSIONS}")
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
check_cxx_source_compiles(
    "#include <expected>\n#include <system_error>\nint main() { std::expected<int, std::error_code> v{1}; return *v == 1 ? 0 : 1; }"
    MPMC_HAS_CXX23_EXPECTED)
if(MPMC_SAVED_CXX_STANDARD)
    set(CMAKE_CXX_STANDARD "${MPMC_SAVED_CXX_STANDARD}")
else()
    unset(CMAKE_CXX_STANDARD)
endif()
if(MPMC_SAVED_CXX_STANDARD_REQUIRED)
    set(CMAKE_CXX_STANDARD_REQUIRED "${MPMC_SAVED_CXX_STANDARD_REQUIRED}")
else()
    unset(CMAKE_CXX_STANDARD_REQUIRED)
endif()
if(MPMC_SAVED_CXX_EXTENSIONS)
    set(CMAKE_CXX_EXTENSIONS "${MPMC_SAVED_CXX_EXTENSIONS}")
else()
    unset(CMAKE_CXX_EXTENSIONS)
endif()

if(MPMC_HAS_CXX23_EXPECTED)
    add_executable(mpmc_expected_tests expected_api.cpp)
    target_link_libraries(mpmc_expected_tests PRIVATE mpmc::mpmc_queue)
    target_compile_features(mpmc_expected_tests PRIVATE cxx_std_23)
    target_compile_definitions(mpmc_expected_tests PRIVATE
        ${MPMC_TEST_DEFINITIONS})
    target_compile_options(mpmc_expected_tests PRIVATE ${MPMC_WARNING_FLAGS})
    set_target_properties(mpmc_expected_tests PROPERTIES CXX_EXTENSIONS OFF)
    add_test(NAME mpmc_expected_tests COMMAND mpmc_expected_tests)
    set_tests_properties(mpmc_expected_tests PROPERTIES
        TIMEOUT 60 LABELS unit)
endif()
