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
#include <mpmc/mpmc.hpp>

#include <chrono>
#include <cstdint>
#include <iostream>
#include <string>
#include <system_error>

#if !defined(_WIN32)
#include <unistd.h>
#endif

// NOLINTNEXTLINE(bugprone-exception-escape)
int main() {
#if defined(_WIN32)
    std::cout << "POSIX shared memory is not available on Windows\n";
    return 0;
#else
    using queue_type = mpmc::shared_memory_queue<std::uint32_t, std::uint32_t>;

    // Every run uses a fresh name.  The creator is an unlink_on_destroy
    // owner, so a normal exit removes the name and concurrent runs cannot
    // interfere with each other.  The short numeric suffix keeps the name
    // within the macOS 31-byte POSIX shm limit.
    const auto unique =
        static_cast<unsigned long long>(
            std::chrono::steady_clock::now().time_since_epoch().count()) %
        100000000ULL;
    const std::string name =
        "/mpmc_example_" + std::to_string(static_cast<long long>(::getpid())) +
        '_' + std::to_string(unique);

    std::error_code error;
    {
        auto producer = queue_type::create(
            name, 128, mpmc::shared_memory_lifetime::unlink_on_destroy, error);
        if (error || !producer) {
            std::cerr << "could not create shared queue: " << error.message()
                      << '\n';
            return 1;
        }
        for (std::uint32_t i = 0; i < 10; ++i) {
            if (!producer->try_push(i)) {
                return 1;
            }
        }

        // A second handle in another thread or process attaches to the same
        // mapping by name.
        auto consumer = queue_type::attach(name, error);
        if (error || !consumer) {
            std::cerr << "could not attach shared queue: " << error.message()
                      << '\n';
            return 1;
        }
        std::uint32_t value = 0;
        while (consumer->try_pop(value)) {
            std::cout << "received " << value << '\n';
        }
    }

    // unlink_on_destroy removed the name when the owner was destroyed.  A
    // second remove therefore reports "not found" rather than success.
    const bool removed = queue_type::remove(name, error);
    if (removed || error) {
        std::cerr << "unlink_on_destroy did not remove the queue name\n";
        return 2;
    }
    return 0;
#endif
}
