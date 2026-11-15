# SPDX-FileCopyrightText: 2026 Mohammad Mahdi Abdollahpour <git@mohammadmahdi.com>
# SPDX-License-Identifier: CPAL-1.0
#
# The contents of this file are subject to the Common Public Attribution License Version 1.0 (the "License"); you may not use this file except in compliance with the License. You may obtain a copy of the License at https://opensource.org/license/cpal-1-0. The License is based on the Mozilla Public License Version 1.1 but Sections 14 and 15 have been added to cover use of software over a computer network and provide for limited attribution for the Original Developer. In addition, Exhibit A has been modified to be consistent with Exhibit B.
# Software distributed under the License is distributed on an "AS IS" basis, WITHOUT WARRANTY OF ANY KIND, either express or implied. See the License for the specific language governing rights and limitations under the License.
# The Original Code is mpmc_queue, a header-only C++20 bounded queue library.
# The Original Developer is not separately identified and is the Initial Developer. If left blank, the Original Developer is the Initial Developer.
# The Initial Developer of the Original Code is Mohammad Mahdi Abdollahpour. All portions of the code written by Mohammad Mahdi Abdollahpour are Copyright (c) 2026. All Rights Reserved.
# Contributor: Mohammad Mahdi Abdollahpour.
#
# CMake 3.30+ removed the bundled FindBoost module.  Selecting the NEW policy
# makes find_package use Boost's config package instead of emitting a warning.
if(POLICY CMP0167)
    cmake_policy(SET CMP0167 NEW)
endif()

if(NOT MPMC_HAS_LOCK_FREE_16_BYTE_CELL)
    message(FATAL_ERROR
        "The comparison benchmark uses mpmc::mpmc_queue<uint64_t, ..., "
        "uint32_t>, which requires lock-free 16-byte cells. Configure without "
        "MPMC_FORCE_8_BYTE_CELLS on a target/toolchain that supports them.")
endif()

find_package(Boost 1.83 REQUIRED)
find_package(Threads REQUIRED)

set(MPMC_COMPARISON_DEPS_DIR
    "${CMAKE_CURRENT_SOURCE_DIR}/deps"
    CACHE PATH "Directory containing the pinned comparison dependencies")

set(_mpmc_rigtorp_header
    "${MPMC_COMPARISON_DEPS_DIR}/MPMCQueue/include/rigtorp/MPMCQueue.h")
if(NOT EXISTS "${_mpmc_rigtorp_header}")
    message(FATAL_ERROR
        "Comparison dependency is missing. Run "
        "${CMAKE_CURRENT_SOURCE_DIR}/fetch_deps.sh first.")
endif()

add_executable(mpmc_comparison compare.cpp)
target_link_libraries(mpmc_comparison PRIVATE
    mpmc::mpmc_queue
    Threads::Threads)
target_include_directories(mpmc_comparison PRIVATE
    ${Boost_INCLUDE_DIRS}
    "${MPMC_COMPARISON_DEPS_DIR}/MPMCQueue/include")
target_compile_features(mpmc_comparison PRIVATE cxx_std_20)
if(MPMC_WARNING_FLAGS)
    target_compile_options(mpmc_comparison PRIVATE ${MPMC_WARNING_FLAGS})
endif()
set_target_properties(mpmc_comparison PROPERTIES CXX_EXTENSIONS OFF)

add_executable(mpmc_spsc_comparison spsc_raw.cpp)
target_link_libraries(mpmc_spsc_comparison PRIVATE
    mpmc::mpmc_queue
    Threads::Threads)
target_include_directories(mpmc_spsc_comparison PRIVATE
    ${Boost_INCLUDE_DIRS}
    "${MPMC_COMPARISON_DEPS_DIR}/MPMCQueue/include")
target_compile_features(mpmc_spsc_comparison PRIVATE cxx_std_20)
if(MPMC_WARNING_FLAGS)
    target_compile_options(mpmc_spsc_comparison PRIVATE ${MPMC_WARNING_FLAGS})
endif()
set_target_properties(mpmc_spsc_comparison PROPERTIES CXX_EXTENSIONS OFF)
