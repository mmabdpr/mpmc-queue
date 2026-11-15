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

add_executable(mpmc_basic_example basic.cpp)
target_link_libraries(mpmc_basic_example PRIVATE mpmc::mpmc_queue)

add_executable(mpmc_producer_consumer_example producer_consumer.cpp)
target_link_libraries(mpmc_producer_consumer_example PRIVATE
    mpmc::mpmc_queue Threads::Threads)

if(UNIX)
    add_executable(mpmc_shared_memory_example shared_memory.cpp)
    target_link_libraries(mpmc_shared_memory_example PRIVATE mpmc::mpmc_queue)
endif()

# unique_ptr_queue stores a pointer plus a sequence marker, so it needs a
# genuine 16-byte lock-free cell.
if(MPMC_HAS_LOCK_FREE_16_BYTE_CELL)
    add_executable(mpmc_unique_ptr_example unique_ptr.cpp)
    target_link_libraries(mpmc_unique_ptr_example PRIVATE mpmc::mpmc_queue)
endif()

set(_mpmc_example_targets
    mpmc_basic_example
    mpmc_producer_consumer_example)
if(UNIX)
    list(APPEND _mpmc_example_targets mpmc_shared_memory_example)
endif()
if(TARGET mpmc_unique_ptr_example)
    list(APPEND _mpmc_example_targets mpmc_unique_ptr_example)
endif()

foreach(example IN LISTS _mpmc_example_targets)
    target_compile_features(${example} PRIVATE cxx_std_20)
    target_compile_options(${example} PRIVATE ${MPMC_WARNING_FLAGS})
    set_target_properties(${example} PROPERTIES CXX_EXTENSIONS OFF)
endforeach()
