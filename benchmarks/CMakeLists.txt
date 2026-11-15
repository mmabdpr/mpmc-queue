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

add_executable(mpmc_benchmark benchmark.cpp)
target_link_libraries(mpmc_benchmark PRIVATE mpmc::mpmc_queue Threads::Threads)
target_compile_features(mpmc_benchmark PRIVATE cxx_std_20)
# This is the effective capability: MPMC_HAS_LOCK_FREE_16_BYTE_CELL already
# folds in MPMC_FORCE_8_BYTE_CELLS.  The benchmark must use this selection for
# every 16-byte row instead of re-deriving it from the compiler.
target_compile_definitions(mpmc_benchmark PRIVATE
    MPMC_BENCHMARK_HAS_16BYTE_CELL=$<BOOL:${MPMC_HAS_LOCK_FREE_16_BYTE_CELL}>)
target_compile_options(mpmc_benchmark PRIVATE
    ${MPMC_WARNING_FLAGS} $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-O3>)
set_target_properties(mpmc_benchmark PROPERTIES CXX_EXTENSIONS OFF)

# Cell-selection self-check: runs a short sweep and asserts that the effective
# cell selection audited the expected rows.  The failure expression catches
# any 16-byte measurement row (rows start with the cell label at column 0), so
# a forced-8 build cannot silently run or print 16-byte configurations.
if(MPMC_BUILD_TESTS)
    add_test(NAME mpmc_benchmark_cell_selection
        COMMAND mpmc_benchmark --check)
    set(_mpmc_benchmark_test_properties
        PASS_REGULAR_EXPRESSION "benchmark self-check: PASS"
        TIMEOUT 120)
    # Measurement rows start with the cell label at column 0.  Only forbid
    # them when CMake selected the 8-byte-only configuration.
    if(NOT MPMC_HAS_LOCK_FREE_16_BYTE_CELL)
        list(APPEND _mpmc_benchmark_test_properties
            FAIL_REGULAR_EXPRESSION "16B ")
    endif()
    set_tests_properties(mpmc_benchmark_cell_selection PROPERTIES
        ${_mpmc_benchmark_test_properties})
    unset(_mpmc_benchmark_test_properties)
endif()

add_custom_target(mpmc_benchmark_check
    COMMAND $<TARGET_FILE:mpmc_benchmark> --check
    DEPENDS mpmc_benchmark
    COMMENT "Checking benchmark cell selection (no 16-byte rows when disabled)")
