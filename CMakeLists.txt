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
cmake_minimum_required(VERSION 3.21)

project(mpmc_queue
    VERSION 1.0.0
    DESCRIPTION "Bounded lock-free multi-producer/multi-consumer queue"
    LANGUAGES CXX)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)
include(CheckCXXSourceCompiles)

# Keep a compilation database available for clang-tidy and other tooling.
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# The library only publishes a minimum language requirement through
# target_compile_features(cxx_std_20).  Do not set CMAKE_CXX_STANDARD at
# directory scope: that would override the language mode of a parent project
# that consumes this repository with add_subdirectory.  Project executables
# request their own standard below.
if(CMAKE_CXX_COMPILER_ID MATCHES "^(GNU|Clang|AppleClang)$")
    set(MPMC_WARNING_FLAGS -Wall -Wextra -Wpedantic)
else()
    set(MPMC_WARNING_FLAGS "")
endif()

# The shared-memory adapter uses std::mutex for its bootstrap lock, so the
# threading dependency is part of the public interface of this header-only
# library and must be propagated to consumers.
set(THREADS_PREFER_PTHREAD_FLAG ON)
find_package(Threads REQUIRED)

# POSIX is the supported platform.  Tests and examples are enabled only when
# this project is the top-level build; the header library itself remains
# usable from add_subdirectory consumers.
set(MPMC_TESTS_DEFAULT ${PROJECT_IS_TOP_LEVEL})
set(MPMC_EXAMPLES_DEFAULT ${PROJECT_IS_TOP_LEVEL})

option(MPMC_BUILD_TESTS "Build the test suite" ${MPMC_TESTS_DEFAULT})
option(MPMC_BUILD_EXAMPLES "Build examples" ${MPMC_EXAMPLES_DEFAULT})
option(MPMC_BUILD_BENCHMARKS "Build benchmarks" OFF)
option(MPMC_BUILD_COMPARISON_BENCHMARKS
       "Build the opt-in cross-library benchmark comparison" OFF)
option(MPMC_ENABLE_MCX16
       "Request -mcx16 on x86-64; 16-byte lock-free support is still probed" ON)

set(MPMC_USE_MCX16 OFF)
if(MPMC_ENABLE_MCX16
    AND NOT APPLE
    AND CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|amd64)$"
    AND CMAKE_CXX_COMPILER_ID MATCHES "^(GNU|Clang)$"
    AND NOT MSVC)
    set(MPMC_USE_MCX16 ON)
endif()

option(MPMC_FORCE_8_BYTE_CELLS
       "Build tests/examples/benchmarks with 8-byte cells even when 16-byte cells are lock-free"
       OFF)

# Probe the actual cell types, with the same compile options consumers receive.
# GCC x86-64 in particular does not reliably make a 16-byte std::atomic
# lock-free merely because -mcx16 is present, so a compiler/target query decides
# which cell sizes the build uses.  Keep the static assertion in queue_engine;
# the test and example suites follow the probe result.
# The cached probe result must follow a change to MPMC_ENABLE_MCX16 or the
# compiler, so re-run it on every configure (one compilation).
unset(MPMC_COMPILER_HAS_LOCK_FREE_8_BYTE_CELL CACHE)
unset(MPMC_COMPILER_HAS_LOCK_FREE_16_BYTE_CELL CACHE)
set(_mpmc_saved_cxx_standard "${CMAKE_CXX_STANDARD}")
set(_mpmc_saved_cxx_standard_required "${CMAKE_CXX_STANDARD_REQUIRED}")
set(_mpmc_saved_cxx_extensions "${CMAKE_CXX_EXTENSIONS}")
set(_mpmc_saved_required_flags "${CMAKE_REQUIRED_FLAGS}")
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(MPMC_USE_MCX16)
    set(CMAKE_REQUIRED_FLAGS "${_mpmc_saved_required_flags} -mcx16")
else()
    set(CMAKE_REQUIRED_FLAGS "${_mpmc_saved_required_flags}")
endif()
check_cxx_source_compiles([[
#include <atomic>
#include <cstdint>
#include <cstddef>
struct alignas(8) probe8 {
    std::uint32_t seq;
    std::byte data[4];
};
static_assert(sizeof(probe8) == 8, "probe8 size");
static_assert(std::atomic<probe8>::is_always_lock_free,
              "8-byte atomic cells are not lock-free");
int main() {}
]] MPMC_COMPILER_HAS_LOCK_FREE_8_BYTE_CELL)
check_cxx_source_compiles([[
#include <atomic>
#include <cstdint>
#include <cstddef>
struct alignas(16) probe16 {
    std::uint64_t seq;
    std::byte data[8];
};
static_assert(sizeof(probe16) == 16, "probe16 size");
static_assert(std::atomic<probe16>::is_always_lock_free,
              "16-byte atomic cells are not lock-free");
int main() {}
]] MPMC_COMPILER_HAS_LOCK_FREE_16_BYTE_CELL)
set(CMAKE_REQUIRED_FLAGS "${_mpmc_saved_required_flags}")
if(_mpmc_saved_cxx_standard)
    set(CMAKE_CXX_STANDARD "${_mpmc_saved_cxx_standard}")
else()
    unset(CMAKE_CXX_STANDARD)
endif()
if(_mpmc_saved_cxx_standard_required)
    set(CMAKE_CXX_STANDARD_REQUIRED "${_mpmc_saved_cxx_standard_required}")
else()
    unset(CMAKE_CXX_STANDARD_REQUIRED)
endif()
if(_mpmc_saved_cxx_extensions)
    set(CMAKE_CXX_EXTENSIONS "${_mpmc_saved_cxx_extensions}")
else()
    unset(CMAKE_CXX_EXTENSIONS)
endif()

set(MPMC_HAS_LOCK_FREE_8_BYTE_CELL OFF)
if(MPMC_COMPILER_HAS_LOCK_FREE_8_BYTE_CELL)
    set(MPMC_HAS_LOCK_FREE_8_BYTE_CELL ON)
endif()
set(MPMC_HAS_LOCK_FREE_16_BYTE_CELL OFF)
if(MPMC_COMPILER_HAS_LOCK_FREE_16_BYTE_CELL AND NOT MPMC_FORCE_8_BYTE_CELLS)
    set(MPMC_HAS_LOCK_FREE_16_BYTE_CELL ON)
endif()

if(NOT MPMC_HAS_LOCK_FREE_8_BYTE_CELL)
    message(WARNING
        "8-byte atomic cells are not lock-free on this compiler/target; "
        "mpmc_queue cannot be used here")
endif()
message(STATUS
    "mpmc: lock-free cells: 8-byte=${MPMC_HAS_LOCK_FREE_8_BYTE_CELL}, "
    "16-byte=${MPMC_HAS_LOCK_FREE_16_BYTE_CELL} (mcx16=${MPMC_USE_MCX16})")

add_library(mpmc_queue INTERFACE)
add_library(mpmc::mpmc_queue ALIAS mpmc_queue)

target_compile_features(mpmc_queue INTERFACE cxx_std_20)
target_include_directories(mpmc_queue INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
target_link_libraries(mpmc_queue INTERFACE Threads::Threads)

if(MPMC_USE_MCX16 AND MPMC_HAS_LOCK_FREE_16_BYTE_CELL)
    # Only propagate -mcx16 when the probe confirmed that the flag makes the
    # 16-byte cell lock-free on this compiler.
    target_compile_options(mpmc_queue INTERFACE -mcx16)
endif()

if(UNIX AND NOT APPLE)
    # glibc before 2.34 requires librt for shm_open/shm_unlink.
    find_library(MPMC_RT_LIBRARY rt)
    if(MPMC_RT_LIBRARY)
        target_link_libraries(mpmc_queue INTERFACE rt)
    endif()
endif()

if(MPMC_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

if(MPMC_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()

if(MPMC_BUILD_BENCHMARKS)
    add_subdirectory(benchmarks)
endif()

# The comparison is independent of the internal benchmark and requires
# explicitly fetched third-party headers plus Boost.Lockfree.
if(MPMC_BUILD_COMPARISON_BENCHMARKS)
    add_subdirectory(benchmarks/comparison)
endif()

install(TARGETS mpmc_queue EXPORT mpmc_queueTargets)
install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(FILES LICENSE NOTICE README.md CHANGELOG.md SECURITY.md
        DESTINATION ${CMAKE_INSTALL_DOCDIR})
install(DIRECTORY LICENSES DESTINATION ${CMAKE_INSTALL_DOCDIR})
# README.md links to these files with relative paths; install them in the
# matching layout so the installed documentation does not have dead links.
install(FILES docs/design.md DESTINATION ${CMAKE_INSTALL_DOCDIR}/docs)
# The README points at the cross-library comparison methodology and measured
# results; install them under the matching relative path. RESULTS.md exists
# only after a validated reference run, so it is optional for a source install.
install(FILES benchmarks/comparison/README.md
        DESTINATION ${CMAKE_INSTALL_DOCDIR}/benchmarks/comparison)
install(FILES benchmarks/comparison/RESULTS.md
        benchmarks/comparison/SPSC.md
        DESTINATION ${CMAKE_INSTALL_DOCDIR}/benchmarks/comparison
        OPTIONAL)

# The README also points readers at the verification directory and its script.
# Install the small model/script set alongside the verification README so the
# links and the documented command remain usable from an installed copy.
install(PROGRAMS verification/run-tlc.sh
    DESTINATION ${CMAKE_INSTALL_DOCDIR}/verification)
install(FILES
    verification/README.md
    verification/MpmcQueue.tla
    verification/MpmcQueue.cfg
    verification/MpmcQueue-cap1.cfg
    verification/MpmcQueue-cap3.cfg
    DESTINATION ${CMAKE_INSTALL_DOCDIR}/verification)
install(EXPORT mpmc_queueTargets
    FILE mpmc_queueTargets.cmake
    NAMESPACE mpmc::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/mpmc_queue)

configure_package_config_file(
    cmake/mpmc_queueConfig.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/mpmc_queueConfig.cmake
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/mpmc_queue)

write_basic_package_version_file(
    ${CMAKE_CURRENT_BINARY_DIR}/mpmc_queueConfigVersion.cmake
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion)

install(FILES
    ${CMAKE_CURRENT_BINARY_DIR}/mpmc_queueConfig.cmake
    ${CMAKE_CURRENT_BINARY_DIR}/mpmc_queueConfigVersion.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/mpmc_queue)

# pkg-config metadata for non-CMake consumers.  Ordinary link requirements
# belong in Libs (not Libs.private) because the shared-memory adapter is part
# of this header-only package and needs them even for a normal link.
# Do not force a language standard here: a consumer's -std=c++23 (or newer)
# must win over the package metadata.  The library headers themselves are
# C++20, and CMake consumers get that through target_compile_features.
set(MPMC_PKGCONFIG_CFLAGS "-I\${includedir}")
if(MPMC_USE_MCX16 AND MPMC_HAS_LOCK_FREE_16_BYTE_CELL)
    string(APPEND MPMC_PKGCONFIG_CFLAGS " -mcx16")
endif()

set(MPMC_PKGCONFIG_LIBS "")
if(UNIX AND NOT APPLE AND MPMC_RT_LIBRARY)
    # glibc before 2.34 requires librt for shm_open/shm_unlink.
    set(MPMC_PKGCONFIG_LIBS "-lrt")
endif()
if(CMAKE_USE_PTHREADS_INIT AND NOT APPLE)
    # Threading is a link-time requirement; keeping it out of Cflags avoids
    # changing the consumer's compile command unnecessarily.
    string(APPEND MPMC_PKGCONFIG_LIBS " -pthread")
endif()

# Resolve the installation prefix relative to the installed .pc file.  This
# also works for multiarch layouts such as lib/x86_64-linux-gnu/pkgconfig,
# unlike a hard-coded ${pcfiledir}/../.. .
if(IS_ABSOLUTE "${CMAKE_INSTALL_LIBDIR}")
    # An absolute libdir cannot be relocated; use the configured prefix.
    set(MPMC_PKGCONFIG_PREFIX "${CMAKE_INSTALL_PREFIX}")
else()
    set(MPMC_PKGCONFIG_PREFIX "\${pcfiledir}/..")
    string(REPLACE "/" ";" _mpmc_libdir_parts "${CMAKE_INSTALL_LIBDIR}")
    foreach(_mpmc_libdir_part IN LISTS _mpmc_libdir_parts)
        if(NOT _mpmc_libdir_part STREQUAL "")
            string(APPEND MPMC_PKGCONFIG_PREFIX "/..")
        endif()
    endforeach()
endif()

if(IS_ABSOLUTE "${CMAKE_INSTALL_INCLUDEDIR}")
    set(MPMC_PKGCONFIG_INCLUDEDIR "${CMAKE_INSTALL_INCLUDEDIR}")
else()
    set(MPMC_PKGCONFIG_INCLUDEDIR
        "\${prefix}/${CMAKE_INSTALL_INCLUDEDIR}")
endif()

configure_file(cmake/mpmc_queue.pc.in
    ${CMAKE_CURRENT_BINARY_DIR}/mpmc_queue.pc @ONLY)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/mpmc_queue.pc
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/pkgconfig)

# Optional formatting check.  Only changed files are checked by default, so
# enabling this target never forces a repository-wide reformat.
find_program(MPMC_CLANG_FORMAT_EXECUTABLE NAMES clang-format)
if(NOT MPMC_CLANG_FORMAT_EXECUTABLE AND APPLE)
    execute_process(
        COMMAND xcrun --find clang-format
        OUTPUT_VARIABLE _mpmc_xcrun_clang_format
        RESULT_VARIABLE _mpmc_xcrun_result
        ERROR_QUIET
        OUTPUT_STRIP_TRAILING_WHITESPACE)
    if(_mpmc_xcrun_result EQUAL 0 AND _mpmc_xcrun_clang_format)
        set(MPMC_CLANG_FORMAT_EXECUTABLE "${_mpmc_xcrun_clang_format}")
    endif()
endif()
if(MPMC_CLANG_FORMAT_EXECUTABLE)
    set(MPMC_FORMAT_BASE "HEAD^1" CACHE STRING
        "Git revision used by the format-check target")
    add_custom_target(format-check
        COMMAND ${CMAKE_COMMAND}
            -DMPMC_CLANG_FORMAT=${MPMC_CLANG_FORMAT_EXECUTABLE}
            -DMPMC_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}
            -DMPMC_FORMAT_BASE=${MPMC_FORMAT_BASE}
            -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/format-check.cmake
        COMMENT "Checking formatting of changed C++ files"
        VERBATIM)
else()
    add_custom_target(format-check
        COMMAND ${CMAKE_COMMAND} -E echo
            "clang-format not found; format-check is unavailable"
        VERBATIM)
endif()

# Optional clang-tidy run over every translation unit in the compilation
# database.  The repository .clang-tidy file selects the checks.
find_program(MPMC_CLANG_TIDY_EXECUTABLE NAMES clang-tidy
    HINTS /opt/homebrew/opt/llvm/bin /usr/local/opt/llvm/bin)
if(MPMC_CLANG_TIDY_EXECUTABLE)
    add_custom_target(clang-tidy
        COMMAND ${CMAKE_COMMAND} -E env
            CLANG_TIDY=${MPMC_CLANG_TIDY_EXECUTABLE}
            MPMC_BUILD_DIR=${CMAKE_BINARY_DIR}
            ${CMAKE_CURRENT_SOURCE_DIR}/tools/run-clang-tidy.sh
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        COMMENT "Running clang-tidy over the compilation database"
        USES_TERMINAL
        VERBATIM)
else()
    add_custom_target(clang-tidy
        COMMAND ${CMAKE_COMMAND} -E echo
            "clang-tidy not found; clang-tidy target is unavailable"
        VERBATIM)
endif()
