// SPDX-FileCopyrightText: 2026 Mohammad Mahdi Abdollahpour <git@mohammadmahdi.com>
// SPDX-License-Identifier: CPAL-1.0
//
// The contents of this file are subject to the Common Public Attribution
// License Version 1.0 (the "License"); you may not use this file except in
// compliance with the License. You may obtain a copy of the License at
// https://opensource.org/license/cpal-1-0.
// The License is based on the Mozilla Public License Version 1.1 but
// Sections 14 and 15 have been added to cover use of software over a
// computer network and provide for limited attribution for the Original
// Developer. In addition, Exhibit A has been modified to be consistent
// with Exhibit B.
//
// Software distributed under the License is distributed on an "AS IS"
// basis, WITHOUT WARRANTY OF ANY KIND, either express or implied. See the
// License for the specific language governing rights and limitations
// under the License.
//
// The Original Code is mpmc_queue, a header-only C++20 bounded queue
// library.
// The Original Developer is not separately identified and is the Initial
// Developer. If left blank, the Original Developer is the Initial
// Developer.
// The Initial Developer of the Original Code is Mohammad Mahdi
// Abdollahpour. All portions of the code written by Mohammad Mahdi
// Abdollahpour are Copyright (c) 2026. All Rights Reserved.
// Contributor: Mohammad Mahdi Abdollahpour.
//
// Exception-free setup test plus deterministic nothrow-allocation failure
// coverage.  The executable replaces the global allocation functions
// so it can make the next allocation fail at a chosen point.  This lets the
// test distinguish object allocation from the runtime slot-array allocation
// without adding a test hook to the library headers.

#include <mpmc/mpmc.hpp>

#include "test_cell_config.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <string>
#include <system_error>

#if !defined(_WIN32)
#include <unistd.h>
#endif

namespace {

std::atomic<long> live_allocations{0};
std::atomic<long> fail_after{-1}; // -1 disarmed; 0 fails the next allocation

[[nodiscard]] bool should_fail() noexcept {
    const long remaining = fail_after.load(std::memory_order_relaxed);
    if (remaining < 0) {
        return false;
    }
    if (remaining == 0) {
        return true;
    }
    fail_after.store(remaining - 1, std::memory_order_relaxed);
    return false;
}

[[nodiscard]] void* test_allocate(std::size_t size, std::size_t alignment,
                                  bool inject) noexcept {
    if (inject && should_fail()) {
        return nullptr;
    }
    if (size == 0) {
        size = 1;
    }
    void* pointer = nullptr;
    if (alignment <= alignof(std::max_align_t)) {
        pointer = std::malloc(size);
    } else if (::posix_memalign(&pointer, alignment, size) != 0) {
        pointer = nullptr;
    }
    if (pointer != nullptr) {
        live_allocations.fetch_add(1, std::memory_order_relaxed);
    }
    return pointer;
}

void test_deallocate(void* pointer) noexcept {
    if (pointer == nullptr) {
        return;
    }
    live_allocations.fetch_sub(1, std::memory_order_relaxed);
    std::free(pointer);
}

void arm_failure(long skip) noexcept {
    fail_after.store(skip, std::memory_order_relaxed);
}

void disarm_failure() noexcept {
    fail_after.store(-1, std::memory_order_relaxed);
}

[[nodiscard]] long live() noexcept {
    return live_allocations.load(std::memory_order_relaxed);
}

void* allocate_or_abort(std::size_t size, std::size_t alignment) {
    void* pointer = test_allocate(size, alignment, false);
    if (pointer == nullptr) {
        std::abort();
    }
    return pointer;
}

[[nodiscard]] bool expect_live(long baseline, const char* what) {
    const long now = live();
    if (now != baseline) {
        std::fprintf(stderr,
                     "    %s: allocation leak or partial init "
                     "(live %ld, expected %ld)\n",
                     what, now, baseline);
        return false;
    }
    return true;
}

[[nodiscard]] bool expect(bool condition, const char* what) {
    if (!condition) {
        std::fprintf(stderr, "    %s\n", what);
    }
    return condition;
}

using small_queue = mpmc::mpmc_queue<std::uint32_t, 8, std::uint32_t>;
using runtime_queue = mpmc::mpmc_queue<std::uint32_t, 0, std::uint32_t>;

[[nodiscard]] bool static_object_allocation_failure() {
    bool ok = true;
    const long baseline = live();
    std::error_code error = mpmc::errc::invalid_capacity;
    arm_failure(0);
    auto queue = small_queue::create(error);
    disarm_failure();
    ok &= expect(!queue, "static queue create returned an object");
    ok &= expect(error == mpmc::errc::allocation_failed,
                 "static queue create reported the wrong error");
    ok &= expect_live(baseline, "static object allocation");

    // A successful create must clear a stale error from an earlier failure.
    error = mpmc::errc::invalid_capacity;
    auto good = small_queue::create(error);
    ok &= expect(good != nullptr && !error,
                 "static queue create did not clear a stale error");
    if (good) {
        ok &= expect(good->try_push(7), "static queue push failed");
        std::uint32_t value = 0;
        ok &= expect(good->try_pop(value) && value == 7,
                     "static queue pop failed");
    }
    return ok;
}

[[nodiscard]] bool runtime_object_allocation_failure() {
    bool ok = true;
    const long baseline = live();
    std::error_code error;
    arm_failure(0);
    auto queue = runtime_queue::create(8, error);
    disarm_failure();
    ok &= expect(!queue, "runtime queue create returned an object");
    ok &= expect(error == mpmc::errc::allocation_failed,
                 "runtime object failure reported the wrong error");
    ok &= expect_live(baseline, "runtime object allocation");
    return ok;
}

[[nodiscard]] bool runtime_slot_array_allocation_failure() {
    bool ok = true;
    const long baseline = live();
    std::error_code error;
    arm_failure(1); // object succeeds, slot array fails
    auto queue = runtime_queue::create(8, error);
    disarm_failure();
    ok &= expect(!queue, "runtime queue with failed slots was returned");
    ok &= expect(error == mpmc::errc::allocation_failed,
                 "slot array failure reported the wrong error");
    ok &= expect_live(baseline, "runtime slot array allocation");

    // The failed attempt must not poison later setup.
    error = mpmc::errc::invalid_capacity;
    auto good = runtime_queue::create(8, error);
    ok &= expect(good != nullptr && !error,
                 "runtime queue create failed after a slot allocation error");
    if (good) {
        ok &= expect(good->try_push(3), "runtime queue push failed");
        std::uint32_t value = 0;
        ok &= expect(good->try_pop(value) && value == 3,
                     "runtime queue pop failed");
    }
    return ok;
}

[[nodiscard]] bool make_queue_allocation_failure() {
    bool ok = true;
    const long baseline = live();
    std::error_code error;
    arm_failure(0);
    auto queue = mpmc::make_queue<small_queue>(error);
    disarm_failure();
    ok &= expect(!queue, "make_queue returned an object after failure");
    ok &= expect(error == mpmc::errc::allocation_failed,
                 "make_queue reported the wrong error");
    ok &= expect_live(baseline, "make_queue object allocation");
    return ok;
}

#if MPMC_TEST_HAS_16_BYTE_CELL
[[nodiscard]] bool unique_ptr_static_object_failure() {
    bool ok = true;
    const long baseline = live();
    std::error_code error;
    arm_failure(0);
    auto queue = mpmc::unique_ptr_queue<int, 8>::create(error);
    disarm_failure();
    ok &= expect(!queue, "static unique_ptr_queue create returned an object");
    ok &= expect(error == mpmc::errc::allocation_failed,
                 "static unique_ptr_queue reported the wrong error");
    ok &= expect_live(baseline, "static unique_ptr_queue allocation");
    return ok;
}

[[nodiscard]] bool unique_ptr_runtime_object_failure() {
    bool ok = true;
    const long baseline = live();
    std::error_code error;
    arm_failure(0);
    auto queue = mpmc::unique_ptr_queue<int>::create(8, error);
    disarm_failure();
    ok &= expect(!queue, "runtime unique_ptr_queue create returned an object");
    ok &= expect(error == mpmc::errc::allocation_failed,
                 "runtime unique_ptr_queue object reported the wrong error");
    ok &= expect_live(baseline, "runtime unique_ptr_queue object");
    return ok;
}

[[nodiscard]] bool unique_ptr_runtime_queue_object_failure() {
    bool ok = true;
    const long baseline = live();
    std::error_code error;
    arm_failure(1); // wrapper succeeds, wrapped queue object fails
    auto queue = mpmc::unique_ptr_queue<int>::create(8, error);
    disarm_failure();
    ok &= expect(!queue, "unique_ptr_queue returned after queue object "
                         "allocation failure");
    ok &= expect(error == mpmc::errc::allocation_failed,
                 "unique_ptr_queue inner object reported the wrong error");
    ok &= expect_live(baseline, "unique_ptr_queue inner object");
    return ok;
}

[[nodiscard]] bool unique_ptr_runtime_slot_array_failure() {
    bool ok = true;
    const long baseline = live();
    std::error_code error;
    arm_failure(2); // wrapper and queue object succeed, slot array fails
    auto queue = mpmc::unique_ptr_queue<int>::create(8, error);
    disarm_failure();
    ok &= expect(!queue, "unique_ptr_queue returned after slot failure");
    ok &= expect(error == mpmc::errc::allocation_failed,
                 "unique_ptr_queue slot failure reported the wrong error");
    ok &= expect_live(baseline, "unique_ptr_queue slot array");
    return ok;
}

#endif // MPMC_TEST_HAS_16_BYTE_CELL

[[nodiscard]] bool spsc_setup_allocation_failure() {
    using queue_type = mpmc::spsc_queue<std::uint32_t, 0, std::uint32_t>;
    bool ok = true;
    const long baseline = live();
    std::error_code error;
    arm_failure(0);
    auto queue = queue_type::create(8, error);
    disarm_failure();
    ok &= expect(!queue, "spsc queue create returned an object");
    ok &= expect(error == mpmc::errc::allocation_failed,
                 "spsc object failure reported the wrong error");
    ok &= expect_live(baseline, "spsc object allocation");

    error = mpmc::errc::invalid_capacity;
    arm_failure(1);
    auto slots = queue_type::create(8, error);
    disarm_failure();
    ok &= expect(!slots, "spsc queue with failed slots was returned");
    ok &= expect(error == mpmc::errc::allocation_failed,
                 "spsc slot array failure reported the wrong error");
    ok &= expect_live(baseline, "spsc slot array allocation");

    error.clear();
    auto good = queue_type::create(8, error);
    ok &= expect(good != nullptr && !error,
                 "spsc queue create failed after a slot allocation error");
    if (good) {
        ok &= expect(good->try_push(3), "spsc queue push failed");
        std::uint32_t value = 0;
        ok &=
            expect(good->try_pop(value) && value == 3, "spsc queue pop failed");
    }
    return ok;
}

#if MPMC_TEST_HAS_16_BYTE_CELL
[[nodiscard]] bool unique_ptr_stale_error_cleared() {
    bool ok = true;
    std::error_code error = mpmc::errc::allocation_failed;
    auto queue = mpmc::unique_ptr_queue<int, 8>::create(error);
    ok &= expect(queue != nullptr && !error,
                 "unique_ptr_queue create did not clear a stale error");
    if (queue) {
        auto value = std::make_unique<int>(11);
        ok &= expect(queue->try_push(std::move(value)),
                     "unique_ptr_queue push failed");
        std::unique_ptr<int> received;
        ok &= expect(queue->try_pop(received) && received && *received == 11,
                     "unique_ptr_queue pop failed");
    }
    return ok;
}
#endif // MPMC_TEST_HAS_16_BYTE_CELL

#if !defined(_WIN32)
[[nodiscard]] std::string unique_name(const char* tag) {
    static std::atomic<unsigned> counter{0};
    return std::string{"/mpmc_noexc_"} + tag + '_' +
           std::to_string(static_cast<long long>(::getpid())) + '_' +
           std::to_string(counter.fetch_add(1, std::memory_order_relaxed));
}

[[nodiscard]] bool shared_memory_object_allocation_failure() {
    using queue_type = mpmc::shared_memory_queue<std::uint32_t, std::uint32_t>;
    bool ok = true;
    const std::string name = unique_name("alloc");
    const long baseline = live();
    std::error_code error;
    arm_failure(0);
    auto queue = queue_type::create(
        name, 4, mpmc::shared_memory_lifetime::unlink_on_destroy, error);
    disarm_failure();
    ok &= expect(!queue, "shared memory create returned an object");
    ok &= expect(error == mpmc::errc::allocation_failed,
                 "shared memory object failure reported the wrong error");
    ok &= expect_live(baseline, "shared memory object allocation");
    ok &= expect(!queue_type::exists(name, error),
                 "failed shared memory create left the name behind");
    return ok;
}

[[nodiscard]] bool shared_memory_setup() {
    using queue_type = mpmc::shared_memory_queue<std::uint32_t, std::uint32_t>;
    const std::string name = unique_name("setup");
    std::error_code error;
    auto invalid = queue_type::create(name, 3, error);
    if (!expect(!invalid && error == mpmc::errc::invalid_capacity,
                "invalid shared memory capacity was accepted")) {
        return false;
    }
    if (!expect(!queue_type::exists(name, error),
                "invalid capacity left a shared memory name")) {
        return false;
    }

    auto shared = queue_type::create(
        name, 4, mpmc::shared_memory_lifetime::unlink_on_destroy, error);
    if (!expect(shared != nullptr && !error, "shared memory create failed")) {
        return false;
    }
    if (!expect(queue_type::exists(name, error),
                "shared memory name does not exist after create")) {
        return false;
    }
    bool ok = expect(shared->try_push(1), "shared memory push failed");
    std::uint32_t value = 0;
    ok &= expect(shared->try_pop(value) && value == 1,
                 "shared memory pop failed");
    shared.reset();
    ok &= expect(!queue_type::exists(name, error),
                 "unlink_on_destroy left the shared memory name");
    return ok;
}
#endif

} // namespace

// The replaceable global allocation functions.  Throwing forms never inject
// failure; the library's setup paths all use the nothrow forms.
void* operator new(std::size_t size) {
    return allocate_or_abort(size, alignof(std::max_align_t));
}

void* operator new(std::size_t size, const std::nothrow_t&) noexcept {
    return test_allocate(size, alignof(std::max_align_t), true);
}

void* operator new[](std::size_t size) {
    return allocate_or_abort(size, alignof(std::max_align_t));
}

void* operator new[](std::size_t size, const std::nothrow_t&) noexcept {
    return test_allocate(size, alignof(std::max_align_t), true);
}

void* operator new(std::size_t size, std::align_val_t alignment) {
    return allocate_or_abort(size, static_cast<std::size_t>(alignment));
}

void* operator new(std::size_t size, std::align_val_t alignment,
                   const std::nothrow_t&) noexcept {
    return test_allocate(size, static_cast<std::size_t>(alignment), true);
}

void* operator new[](std::size_t size, std::align_val_t alignment) {
    return allocate_or_abort(size, static_cast<std::size_t>(alignment));
}

void* operator new[](std::size_t size, std::align_val_t alignment,
                     const std::nothrow_t&) noexcept {
    return test_allocate(size, static_cast<std::size_t>(alignment), true);
}

void operator delete(void* pointer) noexcept {
    test_deallocate(pointer);
}

void operator delete(void* pointer, std::size_t) noexcept {
    test_deallocate(pointer);
}

void operator delete(void* pointer, const std::nothrow_t&) noexcept {
    test_deallocate(pointer);
}

void operator delete(void* pointer, std::align_val_t) noexcept {
    test_deallocate(pointer);
}

void operator delete(void* pointer, std::size_t, std::align_val_t) noexcept {
    test_deallocate(pointer);
}

void operator delete[](void* pointer) noexcept {
    test_deallocate(pointer);
}

void operator delete[](void* pointer, std::size_t) noexcept {
    test_deallocate(pointer);
}

void operator delete[](void* pointer, const std::nothrow_t&) noexcept {
    test_deallocate(pointer);
}

void operator delete[](void* pointer, std::align_val_t) noexcept {
    test_deallocate(pointer);
}

void operator delete[](void* pointer, std::size_t, std::align_val_t) noexcept {
    test_deallocate(pointer);
}

int main() {
    int failures = 0;
    failures += static_object_allocation_failure() ? 0 : 1;
    failures += runtime_object_allocation_failure() ? 0 : 1;
    failures += runtime_slot_array_allocation_failure() ? 0 : 1;
    failures += spsc_setup_allocation_failure() ? 0 : 1;
    failures += make_queue_allocation_failure() ? 0 : 1;
#if MPMC_TEST_HAS_16_BYTE_CELL
    failures += unique_ptr_static_object_failure() ? 0 : 1;
    failures += unique_ptr_runtime_object_failure() ? 0 : 1;
    failures += unique_ptr_runtime_queue_object_failure() ? 0 : 1;
    failures += unique_ptr_runtime_slot_array_failure() ? 0 : 1;
    failures += unique_ptr_stale_error_cleared() ? 0 : 1;
#endif
#if !defined(_WIN32)
    failures += shared_memory_object_allocation_failure() ? 0 : 1;
    failures += shared_memory_setup() ? 0 : 1;
#endif
    if (failures != 0) {
        std::fprintf(stderr, "%d allocation/setup check(s) failed\n", failures);
        return 1;
    }
    return 0;
}
