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

#include <cstdint>
#include <iostream>
#include <system_error>

int main() {
    // Compile-time capacity: no allocation occurs after construction.  An
    // 8-byte cell (4-byte value + 4-byte index) is lock-free on every
    // supported target; 16-byte cells depend on the target and toolchain.
    mpmc::mpmc_queue<std::uint32_t, 8, std::uint32_t> queue;

    for (std::uint32_t i = 1; i <= 8; ++i) {
        std::uint32_t sequence = 0;
        if (!queue.try_push(i, sequence)) {
            std::cerr << "push failed: queue is full\n";
            return 1;
        }
        std::cout << "pushed " << i << " at sequence " << sequence << '\n';
    }

    if (queue.try_push(std::uint32_t{9})) {
        std::cerr << "push to a full queue unexpectedly succeeded\n";
        return 1;
    }

    std::uint32_t value = 0;
    std::uint32_t sequence = 0;
    while (queue.try_pop(value, sequence)) {
        std::cout << "popped " << value << " from sequence " << sequence
                  << '\n';
    }

    // Run-time capacity: create() reports setup failures through an error code.
    std::error_code error;
    auto runtime_queue =
        mpmc::mpmc_queue<std::uint32_t, 0, std::uint32_t>::create(32, error);
    if (error || !runtime_queue) {
        std::cerr << "could not create runtime queue: " << error.message()
                  << '\n';
        return 1;
    }
    if (!runtime_queue->try_push(42)) {
        return 1;
    }
    return runtime_queue->try_pop(value) && value == 42 ? 0 : 1;
}
