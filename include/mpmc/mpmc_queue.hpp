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
// Owning, bounded, lock-free MPMC queue.

#pragma once

#include "mpmc/concepts.hpp"
#include "mpmc/concurrency.hpp"
#include "mpmc/detail/queue_engine.hpp"
#include "mpmc/error.hpp"
#include "mpmc/expected.hpp"

#include <array>
#include <atomic>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <new>
#include <system_error>
#include <type_traits>
#include <utility>

namespace mpmc {

template<class T, std::size_t Capacity, class Index, bool LazyPush,
         bool LazyPop>
    requires unique_ptr_payload<T> && queue_payload_for<T*, Index> &&
             (Capacity == 0 || valid_static_capacity<Capacity, Index>)
class unique_ptr_queue;

namespace detail {

template<class Index>
[[nodiscard]] constexpr bool valid_capacity(std::size_t capacity) noexcept {
    return capacity > 0 && std::has_single_bit(capacity) &&
           capacity <=
               static_cast<std::size_t>(std::numeric_limits<Index>::max() / 4);
}

template<class Index>
[[nodiscard]] std::error_code capacity_error(std::size_t capacity) noexcept {
    if (!valid_capacity<Index>(capacity)) {
        return make_error_code(errc::invalid_capacity);
    }
    return {};
}

template<class Slot, std::size_t Capacity> class slot_storage;

template<class Slot> class slot_storage<Slot, 0> {
public:
    explicit slot_storage(std::size_t capacity, std::error_code& error) noexcept
        : slots_(new (std::nothrow) Slot[capacity]), capacity_(capacity) {
        if (slots_ == nullptr) {
            error = make_error_code(errc::allocation_failed);
        }
    }

    slot_storage(const slot_storage&) = delete;
    slot_storage& operator=(const slot_storage&) = delete;
    slot_storage(slot_storage&&) = delete;
    slot_storage& operator=(slot_storage&&) = delete;
    ~slot_storage() = default;

    [[nodiscard]] Slot* cells() noexcept {
        return slots_.get();
    }
    [[nodiscard]] const Slot* cells() const noexcept {
        return slots_.get();
    }
    [[nodiscard]] std::size_t capacity() const noexcept {
        return capacity_;
    }

private:
    std::unique_ptr<Slot[]> slots_;
    std::size_t capacity_;
};

template<class Slot, std::size_t Capacity> class slot_storage {
    static_assert(Capacity > 0);

public:
    slot_storage() noexcept = default;
    explicit slot_storage(std::size_t, std::error_code&) noexcept {}

    slot_storage(const slot_storage&) = delete;
    slot_storage& operator=(const slot_storage&) = delete;
    slot_storage(slot_storage&&) = delete;
    slot_storage& operator=(slot_storage&&) = delete;
    ~slot_storage() = default;

    [[nodiscard]] Slot* cells() noexcept {
        return slots_.data();
    }
    [[nodiscard]] const Slot* cells() const noexcept {
        return slots_.data();
    }
    [[nodiscard]] static constexpr std::size_t capacity() noexcept {
        return Capacity;
    }

private:
    alignas(detail::slot_stride) std::array<Slot, Capacity> slots_{};
};

} // namespace detail

/// A bounded, lock-free, multi-producer/multi-consumer queue.
///
/// \tparam T        Trivially copyable payload.  sizeof(T) + sizeof(Index)
///                  must fit in 8 or 16 bytes so that the payload and its
///                  sequence marker can be published with one atomic
///                  compare-exchange.
/// \tparam Capacity Compile-time capacity (power of two), or 0 to select the
///                  capacity at run time through create(capacity, error).
/// \tparam Index    Unsigned type of the sequence marker and indices.  64-bit
///                  is recommended for long-running producers; 32-bit leaves 12
///                  payload bytes in a 16-byte cell.
/// \tparam LazyPush Leave the write index to the next producer instead of
///                  advancing it in the successful push.
/// \tparam LazyPop  Leave the read index to the next consumer instead of
///                  advancing it in the successful pop.
///
/// The queue is lock-free as a whole: a stalled thread cannot prevent other
/// threads from making progress.  It is not wait-free for an individual
/// thread: a compare-exchange can fail repeatedly under contention, and no
/// thread is guaranteed to win one.  All storage is allocated during
/// construction; push and pop perform no allocation and no system calls.
template<class T, std::size_t Capacity = 0, class Index = std::uint64_t,
         bool LazyPush = false, bool LazyPop = false,
         concurrency Mode = concurrency::mpmc>
    requires queue_payload_for<T, Index> &&
             (Capacity == 0 || valid_static_capacity<Capacity, Index>)
class mpmc_queue
    : private detail::queue_engine<T, Index, LazyPush, LazyPop, Mode> {
public:
    using value_type = T;
    using index_type = Index;
    using engine_type = detail::queue_engine<T, Index, LazyPush, LazyPop, Mode>;
    using slot_type = typename engine_type::slot_type;

    static constexpr std::size_t static_capacity = Capacity;
    static constexpr std::size_t entry_size = engine_type::cell_bytes;
    static constexpr std::size_t payload_size = engine_type::payload_bytes;
    /// Bytes of slot storage embedded in the object for a compile-time
    /// capacity.  Zero for a runtime-sized queue.
    static constexpr std::size_t storage_bytes = Capacity * sizeof(slot_type);
    static constexpr bool is_always_lock_free =
        engine_type::is_always_lock_free;
    static constexpr bool lazy_push = LazyPush;
    static constexpr bool lazy_pop = LazyPop;
    static constexpr concurrency mode = Mode;
    static constexpr bool single_producer = engine_type::single_producer;
    static constexpr bool single_consumer = engine_type::single_consumer;
    static constexpr index_type marker_horizon = engine_type::marker_horizon;

    static_assert(Capacity == 0 ||
                      (Capacity <= std::numeric_limits<std::size_t>::max() /
                                       sizeof(slot_type) &&
                       Capacity <=
                           static_cast<std::size_t>(
                               std::numeric_limits<std::ptrdiff_t>::max()) /
                               sizeof(slot_type)),
                  "static capacity is too large for the address space");

    /// Compile-time-sized queue.  Construction cannot fail.
    mpmc_queue() noexcept
        requires(Capacity != 0)
        : storage_(), write_index_(0), read_index_(0) {
        initialize(static_cast<index_type>(Capacity));
    }

    /// Allocate a compile-time-sized queue.  Returns null and sets `error`
    /// if allocation fails.
    [[nodiscard]] static std::unique_ptr<mpmc_queue>
    create(std::error_code& error) noexcept
        requires(Capacity != 0)
    {
        error.clear();
        auto* queue = new (std::nothrow) mpmc_queue();
        if (queue == nullptr) {
            error = make_error_code(errc::allocation_failed);
        }
        return std::unique_ptr<mpmc_queue>{queue};
    }

    [[nodiscard]] static std::unique_ptr<mpmc_queue> create() noexcept
        requires(Capacity != 0)
    {
        std::error_code ignored;
        return create(ignored);
    }

    /// Create a runtime-sized queue.  Returns null and sets `error` on
    /// invalid capacity or allocation failure.
    [[nodiscard]] static std::unique_ptr<mpmc_queue>
    create(std::size_t capacity, std::error_code& error) noexcept
        requires(Capacity == 0)
    {
        error = detail::capacity_error<index_type>(capacity);
        if (error) {
            return nullptr;
        }
        if (capacity >
                std::numeric_limits<std::size_t>::max() / sizeof(slot_type) ||
            capacity > static_cast<std::size_t>(
                           std::numeric_limits<std::ptrdiff_t>::max()) /
                           sizeof(slot_type)) {
            error = make_error_code(errc::invalid_capacity);
            return nullptr;
        }

        auto* queue =
            new (std::nothrow) mpmc_queue(runtime_tag{}, capacity, error);
        if (queue == nullptr) {
            error = make_error_code(errc::allocation_failed);
            return nullptr;
        }
        if (error) {
            delete queue;
            return nullptr;
        }
        return std::unique_ptr<mpmc_queue>{queue};
    }

    [[nodiscard]] static std::unique_ptr<mpmc_queue>
    create(std::size_t capacity) noexcept
        requires(Capacity == 0)
    {
        std::error_code ignored;
        return create(capacity, ignored);
    }

#if MPMC_HAS_EXPECTED
    /// C++23 opt-in factory returning the queue or the setup error.
    [[nodiscard]] static expected<std::unique_ptr<mpmc_queue>, std::error_code>
    create_expected() noexcept
        requires(Capacity != 0)
    {
        std::error_code error;
        auto queue = create(error);
        if (!queue) {
            return std::unexpected(error);
        }
        return expected<std::unique_ptr<mpmc_queue>, std::error_code>{
            std::move(queue)};
    }

    [[nodiscard]] static expected<std::unique_ptr<mpmc_queue>, std::error_code>
    create_expected(std::size_t capacity) noexcept
        requires(Capacity == 0)
    {
        std::error_code error;
        auto queue = create(capacity, error);
        if (!queue) {
            return std::unexpected(error);
        }
        return expected<std::unique_ptr<mpmc_queue>, std::error_code>{
            std::move(queue)};
    }
#endif

    mpmc_queue(const mpmc_queue&) = delete;
    mpmc_queue& operator=(const mpmc_queue&) = delete;
    mpmc_queue(mpmc_queue&&) = delete;
    mpmc_queue& operator=(mpmc_queue&&) = delete;
    ~mpmc_queue() = default;

    using engine_type::capacity;
    using engine_type::clear;
    using engine_type::consume;
    using engine_type::empty;
    using engine_type::is_lock_free;
    using engine_type::peek;
    using engine_type::pop;
    using engine_type::publication_limit;
    using engine_type::publications_remaining;
    using engine_type::push;
    using engine_type::push_keep_n;
    using engine_type::sequence_exhausted;
    using engine_type::size;
    using engine_type::try_peek;
    using engine_type::try_pop;
    using engine_type::try_pop_if;
    using engine_type::try_push;
    using engine_type::try_push_or_replace;
    using engine_type::try_replace;
    using engine_type::try_replace_last;

private:
    struct runtime_tag {};

    mpmc_queue(runtime_tag, std::size_t capacity,
               std::error_code& error) noexcept
        requires(Capacity == 0)
        : storage_(capacity, error), write_index_(0), read_index_(0) {
        if (!error) {
            initialize(static_cast<index_type>(capacity));
        }
    }

    void initialize(index_type capacity) noexcept {
        this->reset(typename engine_type::state{
            storage_.cells(), &write_index_, &read_index_, capacity,
            static_cast<index_type>(capacity - index_type{1})});
        engine_type::initialize();
    }

    detail::slot_storage<slot_type, Capacity> storage_;
    alignas(detail::slot_stride) std::atomic<index_type> write_index_;
    alignas(detail::slot_stride) std::atomic<index_type> read_index_;
};

template<class T, std::size_t Capacity = 0, class Index = std::uint64_t,
         bool LazyPush = false, bool LazyPop = false>
using bounded_queue = mpmc_queue<T, Capacity, Index, LazyPush, LazyPop>;

/// Single-producer/multiple-consumer contract.  Enqueue calls must come from
/// one producer thread; dequeue calls may come from several consumers.
template<class T, std::size_t Capacity = 0, class Index = std::uint64_t,
         bool LazyPush = false, bool LazyPop = false>
using spmc_queue =
    mpmc_queue<T, Capacity, Index, LazyPush, LazyPop, concurrency::spmc>;

/// Multiple-producer/single-consumer contract.  Enqueue calls may come from
/// several producers; dequeue and observation calls must come from one
/// consumer thread.  `push_keep_n` is not available because it removes entries
/// from the producer side.
template<class T, std::size_t Capacity = 0, class Index = std::uint64_t,
         bool LazyPush = false, bool LazyPop = false>
using mpsc_queue =
    mpmc_queue<T, Capacity, Index, LazyPush, LazyPop, concurrency::mpsc>;

/// Heap-allocate a queue object without throwing.  This is the recommended
/// construction path for a compile-time capacity large enough that an inline
/// object would exceed the stack (slots occupy 128 bytes each).  The
/// allocation happens once, before the queue is used; push/pop remain
/// allocation-free.
///
/// The selected constructor must be non-throwing.  For runtime-sized queues
/// use `mpmc_queue<T>::create(capacity, error)` instead.
template<class Queue, class... Args>
    requires std::is_nothrow_constructible_v<Queue, Args...>
[[nodiscard]] std::unique_ptr<Queue> make_queue(std::error_code& error,
                                                Args&&... args) noexcept {
    error.clear();
    auto* queue = new (std::nothrow) Queue(std::forward<Args>(args)...);
    if (queue == nullptr) {
        error = make_error_code(errc::allocation_failed);
        return nullptr;
    }
    return std::unique_ptr<Queue>{queue};
}

#if MPMC_HAS_EXPECTED
/// C++23 opt-in variant of make_queue.
template<class Queue, class... Args>
    requires std::is_nothrow_constructible_v<Queue, Args...>
[[nodiscard]] expected<std::unique_ptr<Queue>, std::error_code>
make_queue_expected(Args&&... args) noexcept {
    std::error_code error;
    auto queue = make_queue<Queue>(error, std::forward<Args>(args)...);
    if (!queue) {
        return std::unexpected(error);
    }
    return expected<std::unique_ptr<Queue>, std::error_code>{std::move(queue)};
}
#endif

} // namespace mpmc
