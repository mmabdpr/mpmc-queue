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
// Single-producer/single-consumer queue with separate publication sequence
// and payload storage.

#pragma once

#include "mpmc/concepts.hpp"
#include "mpmc/concurrency.hpp"
#include "mpmc/detail/cell.hpp"
#include "mpmc/error.hpp"
#include "mpmc/expected.hpp"
#include "mpmc/mpmc_queue.hpp"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <new>
#include <type_traits>
#include <utility>

namespace mpmc {

namespace detail {

struct runtime_capacity_tag {};

inline constexpr std::size_t spsc_slot_stride = slot_stride;

template<class T, class Index> struct spsc_slot {
    static_assert(supported_index<Index>);
    static_assert(std::atomic<Index>::is_always_lock_free,
                  "the sequence index must be lock-free");

    T value{};
    std::atomic<Index> sequence{};
};

template<class T, class Index>
struct alignas(spsc_slot_stride) spsc_storage_slot {
    spsc_slot<T, Index> value;
};

template<class Slot, std::size_t Capacity> struct spsc_storage {
    alignas(spsc_slot_stride) std::array<Slot, Capacity> slots{};
};

template<class Slot> struct spsc_storage<Slot, 0> {
    spsc_storage() = delete;
    explicit spsc_storage(std::size_t capacity)
        : slots(new (std::nothrow) Slot[capacity]()), count(capacity) {}
    std::unique_ptr<Slot[]> slots;
    std::size_t count{};
};

} // namespace detail

/// Single-producer/single-consumer bounded queue.
///
/// This is a restricted fast path: exactly one thread may call enqueue
/// operations and exactly one thread may call dequeue/observation operations.
/// Payload and sequence are separate fields; a slot is published by a release
/// store of its sequence after the payload has been written.  There is no
/// whole-cell compare-exchange and no shared index compare-exchange on the
/// hot path.
///
/// The queue is FIFO and does not support concurrent replacement.  `clear()`
/// requires external quiescence.
template<class T, std::size_t Capacity = 0, class Index = std::uint64_t>
    requires queue_payload<T> && std::is_nothrow_default_constructible_v<T> &&
             supported_index<Index> &&
             (Capacity == 0 || valid_static_capacity<Capacity, Index>)
class spsc_queue {
public:
    using value_type = T;
    using index_type = Index;
    using slot_type = detail::spsc_storage_slot<T, Index>;
    using storage_type = detail::spsc_storage<slot_type, Capacity>;

    static constexpr concurrency mode = concurrency::spsc;
    static constexpr bool single_producer = true;
    static constexpr bool single_consumer = true;
    static constexpr std::size_t static_capacity = Capacity;
    static constexpr std::size_t slot_stride = detail::spsc_slot_stride;
    static constexpr index_type marker_horizon =
        static_cast<index_type>(std::numeric_limits<index_type>::max() / 2 + 1);
    static constexpr std::size_t storage_bytes =
        Capacity == 0 ? 0 : Capacity * sizeof(slot_type);
    static constexpr bool is_always_lock_free =
        std::atomic<index_type>::is_always_lock_free;

    spsc_queue() noexcept
        requires(Capacity != 0)
        : storage_() {
        initialize();
    }

private:
    explicit spsc_queue(std::size_t capacity,
                        detail::runtime_capacity_tag) noexcept
        requires(Capacity == 0)
        : storage_(capacity) {
        if (!storage_valid()) {
            capacity_ = 0;
            mask_ = 0;
            return;
        }
        capacity_ = capacity;
        mask_ = static_cast<index_type>(capacity - 1);
        initialize();
    }

public:
    spsc_queue(const spsc_queue&) = delete;
    spsc_queue& operator=(const spsc_queue&) = delete;
    spsc_queue(spsc_queue&&) = delete;
    spsc_queue& operator=(spsc_queue&&) = delete;
    ~spsc_queue() = default;
    static std::unique_ptr<spsc_queue> create(std::size_t capacity,
                                              std::error_code& error) noexcept
        requires(Capacity == 0)
    {
        const auto code = detail::capacity_error<Index>(capacity);
        if (code) {
            error = code;
            return nullptr;
        }
        if (capacity >
            std::numeric_limits<std::size_t>::max() / sizeof(slot_type)) {
            error = make_error_code(errc::invalid_capacity);
            return nullptr;
        }
        std::unique_ptr<spsc_queue> queue(new (std::nothrow) spsc_queue(
            capacity, detail::runtime_capacity_tag{}));
        if (!queue || !queue->storage_valid()) {
            error = make_error_code(errc::allocation_failed);
            return nullptr;
        }
        error.clear();
        return queue;
    }

    static std::unique_ptr<spsc_queue> create(std::size_t capacity) noexcept
        requires(Capacity == 0)
    {
        std::error_code error;
        return create(capacity, error);
    }

#if MPMC_HAS_EXPECTED
    static mpmc::expected<std::unique_ptr<spsc_queue>, std::error_code>
    create_expected(std::size_t capacity) noexcept
        requires(Capacity == 0)
    {
        std::error_code error;
        auto queue = create(capacity, error);
        if (!queue) {
            return std::unexpected(error);
        }
        return queue;
    }

    static mpmc::expected<std::unique_ptr<spsc_queue>, std::error_code>
    create_expected() noexcept
        requires(Capacity != 0)
    {
        std::error_code error;
        auto queue = create(error);
        if (!queue) {
            return std::unexpected(error);
        }
        return queue;
    }

    static mpmc::expected<std::unique_ptr<spsc_queue>, std::error_code>
    create_expected(std::error_code& error) noexcept
        requires(Capacity != 0)
    {
        auto queue = create(error);
        if (!queue) {
            return std::unexpected(error);
        }
        return queue;
    }
#endif

    static std::unique_ptr<spsc_queue> create(std::error_code& error) noexcept
        requires(Capacity != 0)
    {
        std::unique_ptr<spsc_queue> queue(new (std::nothrow) spsc_queue());
        if (!queue) {
            error = make_error_code(errc::allocation_failed);
            return nullptr;
        }
        error.clear();
        return queue;
    }

    [[nodiscard]] bool try_push(const value_type& value) noexcept {
        index_type sequence = 0;
        return try_push(value, sequence);
    }

    [[nodiscard]] bool try_push(const value_type& value,
                                index_type& sequence) noexcept {
        const index_type write = write_;
        if (write >= publication_limit()) {
            return false;
        }
        slot_type& slot = slot_for(write);
        if (slot.value.sequence.load(std::memory_order_acquire) !=
            empty_marker(write)) {
            return false;
        }
        copy_value(value, slot.value.value);
        slot.value.sequence.store(full_marker(write),
                                  std::memory_order_release);
        write_ = static_cast<index_type>(write + index_type{1});
        write_mirror_.store(write_, std::memory_order_relaxed);
        sequence = write;
        return true;
    }

    [[nodiscard]] bool push(const value_type& value) noexcept {
        return try_push(value);
    }

    [[nodiscard]] bool push(const value_type& value,
                            index_type& sequence) noexcept {
        return try_push(value, sequence);
    }

    [[nodiscard]] bool try_pop(value_type& value) noexcept {
        index_type sequence = 0;
        return try_pop(value, sequence);
    }

    [[nodiscard]] bool try_pop(value_type& value,
                               index_type& sequence) noexcept {
        const index_type read = read_;
        if (read >= publication_limit()) {
            return false;
        }
        slot_type& slot = slot_for(read);
        if (slot.value.sequence.load(std::memory_order_acquire) !=
            full_marker(read)) {
            return false;
        }
        copy_value(slot.value.value, value);
        slot.value.sequence.store(empty_marker(read + capacity_),
                                  std::memory_order_release);
        read_ = static_cast<index_type>(read + index_type{1});
        read_mirror_.store(read_, std::memory_order_relaxed);
        sequence = read;
        return true;
    }

    [[nodiscard]] bool pop(value_type& value) noexcept {
        return try_pop(value);
    }

    [[nodiscard]] bool pop(value_type& value, index_type& sequence) noexcept {
        return try_pop(value, sequence);
    }

    [[nodiscard]] bool try_peek(value_type& value) noexcept {
        const index_type read = read_;
        if (read >= publication_limit()) {
            return false;
        }
        const slot_type& slot = slot_for(read);
        if (slot.value.sequence.load(std::memory_order_acquire) !=
            full_marker(read)) {
            return false;
        }
        copy_value(slot.value.value, value);
        return true;
    }

    [[nodiscard]] bool try_peek(value_type& value,
                                index_type& sequence) noexcept {
        if (!try_peek(value)) {
            return false;
        }
        sequence = read_;
        return true;
    }

    [[nodiscard]] bool peek(value_type& value) noexcept {
        return try_peek(value);
    }

    [[nodiscard]] bool empty() const noexcept {
        const index_type read = read_mirror_.load(std::memory_order_acquire);
        if (read >= publication_limit()) {
            return true;
        }
        if (slot_for(read).value.sequence.load(std::memory_order_acquire) ==
            full_marker(read)) {
            return false;
        }
        return write_mirror_.load(std::memory_order_relaxed) == read;
    }

    [[nodiscard]] std::size_t size() const noexcept {
        const index_type write = write_mirror_.load(std::memory_order_relaxed);
        const index_type read = read_mirror_.load(std::memory_order_relaxed);
        const index_type delta = static_cast<index_type>(write - read);
        return delta > capacity_ ? 0 : static_cast<std::size_t>(delta);
    }

    [[nodiscard]] std::size_t capacity() const noexcept {
        return static_cast<std::size_t>(capacity_);
    }

    [[nodiscard]] static bool is_lock_free() noexcept {
        return std::atomic<index_type>::is_always_lock_free;
    }

    [[nodiscard]] index_type publication_limit() const noexcept {
        return static_cast<index_type>(marker_horizon -
                                       static_cast<index_type>(capacity_));
    }

    [[nodiscard]] bool sequence_exhausted() const noexcept {
        const index_type limit = publication_limit();
        return write_mirror_.load(std::memory_order_relaxed) >= limit ||
               read_mirror_.load(std::memory_order_relaxed) >= limit;
    }

    [[nodiscard]] index_type publications_remaining() const noexcept {
        const index_type write = write_mirror_.load(std::memory_order_relaxed);
        return static_cast<index_type>(publication_limit() - write);
    }

    /// Resets an empty or quiescent queue.  Concurrent producers, consumers,
    /// or observers are not supported.
    void clear() noexcept {
        initialize();
    }

private:
    [[nodiscard]] bool storage_valid() const noexcept {
        if constexpr (Capacity == 0) {
            return static_cast<bool>(storage_.slots);
        } else {
            return true;
        }
    }

    [[nodiscard]] slot_type& slot_for(index_type position) noexcept {
        return storage_.slots[static_cast<std::size_t>(position & mask_)];
    }

    [[nodiscard]] const slot_type&
    slot_for(index_type position) const noexcept {
        return storage_.slots[static_cast<std::size_t>(position & mask_)];
    }

    static constexpr index_type empty_marker(index_type position) noexcept {
        return static_cast<index_type>(static_cast<std::uint64_t>(position)
                                       << 1U);
    }

    static constexpr index_type full_marker(index_type position) noexcept {
        return static_cast<index_type>(
            (static_cast<std::uint64_t>(position) << 1U) | 1U);
    }

    static void copy_value(const value_type& source,
                           value_type& destination) noexcept {
        std::memcpy(static_cast<void*>(std::addressof(destination)),
                    static_cast<const void*>(std::addressof(source)),
                    sizeof(value_type));
    }

    void initialize() noexcept {
        if constexpr (Capacity == 0) {
            capacity_ = static_cast<index_type>(storage_.count);
        } else {
            capacity_ = static_cast<index_type>(Capacity);
        }
        mask_ = static_cast<index_type>(capacity_ - 1);
        for (index_type i = 0; i < capacity_; ++i) {
            slot_for(i).value.sequence.store(empty_marker(i),
                                             std::memory_order_relaxed);
        }
        write_ = 0;
        read_ = 0;
        write_mirror_.store(0, std::memory_order_relaxed);
        read_mirror_.store(0, std::memory_order_relaxed);
    }

    storage_type storage_;
    index_type capacity_{};
    index_type mask_{};
    alignas(detail::spsc_slot_stride) index_type write_{};
    alignas(detail::spsc_slot_stride) index_type read_{};
    alignas(detail::spsc_slot_stride) std::atomic<index_type> write_mirror_{};
    alignas(detail::spsc_slot_stride) std::atomic<index_type> read_mirror_{};
};

template<class T, std::size_t Capacity = 0, class Index = std::uint64_t>
using single_producer_single_consumer_queue = spsc_queue<T, Capacity, Index>;

} // namespace mpmc
