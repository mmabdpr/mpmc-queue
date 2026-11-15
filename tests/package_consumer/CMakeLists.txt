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
cmake_minimum_required(VERSION 3.21)
project(mpmc_package_consumer LANGUAGES CXX)

find_package(mpmc_queue CONFIG REQUIRED)
include(CheckCXXSourceCompiles)

add_executable(mpmc_package_consumer main.cpp)
target_link_libraries(mpmc_package_consumer PRIVATE mpmc::mpmc_queue)
target_compile_features(mpmc_package_consumer PRIVATE cxx_std_20)

# Optional 16-byte consumer.  The installed target carries -mcx16 where the
# package was configured for it, so this check reflects the consumer's actual
# compile command rather than a hard-coded compiler assumption.
set(_mpmc_saved_standard "${CMAKE_CXX_STANDARD}")
set(_mpmc_saved_required "${CMAKE_CXX_STANDARD_REQUIRED}")
set(_mpmc_saved_extensions "${CMAKE_CXX_EXTENSIONS}")
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_REQUIRED_LIBRARIES mpmc::mpmc_queue)
unset(MPMC_CONSUMER_HAS_LOCK_FREE_16_BYTE_CELL CACHE)
check_cxx_source_compiles([[
#include <mpmc/mpmc.hpp>
int main() {
    mpmc::mpmc_queue<unsigned long long, 4> queue;
    return queue.entry_size == 16 && queue.is_always_lock_free ? 0 : 1;
}
]] MPMC_CONSUMER_HAS_LOCK_FREE_16_BYTE_CELL)
unset(CMAKE_REQUIRED_LIBRARIES)
if(_mpmc_saved_standard)
    set(CMAKE_CXX_STANDARD "${_mpmc_saved_standard}")
else()
    unset(CMAKE_CXX_STANDARD)
endif()
if(_mpmc_saved_required)
    set(CMAKE_CXX_STANDARD_REQUIRED "${_mpmc_saved_required}")
else()
    unset(CMAKE_CXX_STANDARD_REQUIRED)
endif()
if(_mpmc_saved_extensions)
    set(CMAKE_CXX_EXTENSIONS "${_mpmc_saved_extensions}")
else()
    unset(CMAKE_CXX_EXTENSIONS)
endif()

if(MPMC_CONSUMER_HAS_LOCK_FREE_16_BYTE_CELL)
    add_executable(mpmc_package_consumer_16 main16.cpp)
    target_link_libraries(mpmc_package_consumer_16 PRIVATE mpmc::mpmc_queue)
    target_compile_features(mpmc_package_consumer_16 PRIVATE cxx_std_20)
endif()
