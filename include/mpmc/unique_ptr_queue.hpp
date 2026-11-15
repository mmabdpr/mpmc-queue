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
// std::unique_ptr ownership wrapper around mpmc_queue.
//
// The queue itself only moves trivially copyable values.  This wrapper moves
// the raw pointer through the queue and transfers ownership at the boundaries.
// It is intended for threads in one address space, not for shared memory.

#pragma once

#include "mpmc/mpmc_queue.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <new>
#include <optional>
#include <system_error>
#include <type_traits>
#include <utility>

namespace mpmc {

template<class T, std::size_t Capacity = 0, class Index = std::uint64_t,
         bool LazyPush = false, bool LazyPop = false>
    requires unique_ptr_payload<T> && queue_payload_for<T*, Index> &&
             (Capacity == 0 || valid_static_capacity<Capacity, Index>)
class unique_ptr_queue {
public:
    using value_type = T;
    using index_type = Index;
    using pointer_type = std::unique_ptr<T>;
    using queue_type = mpmc_queue<T*, Capacity, Index, LazyPush, LazyPop>;

    static constexpr std::size_t static_capacity = Capacity;
    static constexpr std::size_t entry_size = queue_type::entry_size;
    static constexpr std::size_t payload_size = queue_type::payload_size;
    static constexpr bool is_always_lock_free = queue_type::is_always_lock_free;
    static constexpr bool lazy_push = LazyPush;
    static constexpr bool lazy_pop = LazyPop;
    /// First position that cannot be encoded with a ready bit.  See
    /// mpmc_queue::marker_horizon.
    static constexpr index_type marker_horizon = queue_type::marker_horizon;

    unique_ptr_queue() noexcept
        requires(Capacity != 0)
        : queue_{} {}

    [[nodiscard]] static std::unique_ptr<unique_ptr_queue>
    create(std::size_t capacity, std::error_code& error) noexcept
        requires(Capacity == 0)
    {
        error.clear();
        auto* queue =
            new (std::nothrow) unique_ptr_queue(runtime_tag{}, capacity, error);
        if (queue == nullptr) {
            error = make_error_code(errc::allocation_failed);
            return nullptr;
        }
        if (error) {
            delete queue;
            return nullptr;
        }
        return std::unique_ptr<unique_ptr_queue>{queue};
    }

    [[nodiscard]] static std::unique_ptr<unique_ptr_queue>
    create(std::error_code& error) noexcept
        requires(Capacity != 0)
    {
        error.clear();
        auto* queue = new (std::nothrow) unique_ptr_queue();
        if (queue == nullptr) {
            error = make_error_code(errc::allocation_failed);
        }
        return std::unique_ptr<unique_ptr_queue>{queue};
    }

    [[nodiscard]] static std::unique_ptr<unique_ptr_queue> create() noexcept
        requires(Capacity != 0)
    {
        std::error_code ignored;
        return create(ignored);
    }

    [[nodiscard]] static std::unique_ptr<unique_ptr_queue>
    create(std::size_t capacity) noexcept
        requires(Capacity == 0)
    {
        std::error_code ignored;
        return create(capacity, ignored);
    }

#if MPMC_HAS_EXPECTED
    [[nodiscard]] static expected<std::unique_ptr<unique_ptr_queue>,
                                  std::error_code> create_expected() noexcept
        requires(Capacity != 0)
    {
        std::error_code error;
        auto queue = create(error);
        if (!queue) {
            return std::unexpected(error);
        }
        return expected<std::unique_ptr<unique_ptr_queue>, std::error_code>{
            std::move(queue)};
    }

    [[nodiscard]] static expected<std::unique_ptr<unique_ptr_queue>,
                                  std::error_code>
    create_expected(std::size_t capacity) noexcept
        requires(Capacity == 0)
    {
        std::error_code error;
        auto queue = create(capacity, error);
        if (!queue) {
            return std::unexpected(error);
        }
        return expected<std::unique_ptr<unique_ptr_queue>, std::error_code>{
            std::move(queue)};
    }
#endif

    unique_ptr_queue(const unique_ptr_queue&) = delete;
    unique_ptr_queue& operator=(const unique_ptr_queue&) = delete;
    unique_ptr_queue(unique_ptr_queue&&) = delete;
    unique_ptr_queue& operator=(unique_ptr_queue&&) = delete;

    ~unique_ptr_queue() {
        clear();
    }

    /// Publish ownership of `value`.  Returns false for a null pointer or a
    /// full queue; on failure the caller keeps ownership.
    [[nodiscard]] bool try_push(pointer_type&& value) noexcept {
        if (!value) {
            return false;
        }
        if (queue().try_push(value.get())) {
            value.release();
            return true;
        }
        return false;
    }

    [[nodiscard]] bool try_push(pointer_type&& value,
                                index_type& sequence) noexcept {
        if (!value) {
            return false;
        }
        if (queue().try_push(value.get(), sequence)) {
            value.release();
            return true;
        }
        return false;
    }

    [[nodiscard]] bool push(pointer_type&& value) noexcept {
        return try_push(std::move(value));
    }

    [[nodiscard]] bool push(pointer_type&& value,
                            index_type& sequence) noexcept {
        return try_push(std::move(value), sequence);
    }

    /// Receive ownership of one queued pointer.
    [[nodiscard]] bool try_pop(pointer_type& value) noexcept {
        T* raw = nullptr;
        if (queue().try_pop(raw)) {
            value.reset(raw);
            return true;
        }
        return false;
    }

    [[nodiscard]] bool try_pop(pointer_type& value,
                               index_type& sequence) noexcept {
        T* raw = nullptr;
        if (queue().try_pop(raw, sequence)) {
            value.reset(raw);
            return true;
        }
        return false;
    }

    [[nodiscard]] std::optional<pointer_type> try_pop() noexcept {
        std::optional<T*> raw = queue().try_pop();
        if (!raw.has_value()) {
            return std::nullopt;
        }
        return std::optional<pointer_type>{std::in_place, *raw};
    }

    [[nodiscard]] bool pop(pointer_type& value) noexcept {
        return try_pop(value);
    }

    [[nodiscard]] bool pop(pointer_type& value, index_type& sequence) noexcept {
        return try_pop(value, sequence);
    }

    [[nodiscard]] bool empty() const noexcept {
        return queue().empty();
    }
    [[nodiscard]] std::size_t size() const noexcept {
        return queue().size();
    }
    [[nodiscard]] std::size_t capacity() const noexcept {
        return queue().capacity();
    }

    /// Last exclusive publication position for this queue.  Reaching it makes
    /// try_push() fail permanently (until clear()) while leaving queued
    /// pointers drainable; see mpmc_queue::publication_limit().
    [[nodiscard]] index_type publication_limit() const noexcept {
        return queue().publication_limit();
    }

    /// True once an index has reached the publication limit.  A failed
    /// try_push() then means permanent sequence exhaustion, not a full queue.
    /// Lazy modes can lag until a later push/pop attempt, as in
    /// mpmc_queue; see mpmc_queue::sequence_exhausted().
    [[nodiscard]] bool sequence_exhausted() const noexcept {
        return queue().sequence_exhausted();
    }

    /// Snapshot of the publications left before the limit.  Intended for
    /// monitoring, not synchronization; may overstate the remaining count in
    /// lazy modes until the write index catches up.
    [[nodiscard]] index_type publications_remaining() const noexcept {
        return queue().publications_remaining();
    }

    [[nodiscard]] static constexpr bool is_lock_free() noexcept {
        return is_always_lock_free;
    }

    /// Delete every queued object and reset the queue to empty.  This releases
    /// the backlog retained after sequence exhaustion and starts a new
    /// sequence generation, so previously returned tokens are invalidated.
    /// Must not be called concurrently with producers or consumers.
    void clear() noexcept {
        if constexpr (Capacity == 0) {
            if (!queue_) {
                return;
            }
        }
        T* raw = nullptr;
        while (queue().try_pop(raw)) {
            delete raw;
        }
        queue().clear();
    }

private:
    struct runtime_tag {};

    unique_ptr_queue(runtime_tag, std::size_t capacity,
                     std::error_code& error) noexcept
        requires(Capacity == 0)
        : queue_(queue_type::create(capacity, error)) {}

    using storage_type =
        std::conditional_t<Capacity == 0, std::unique_ptr<queue_type>,
                           queue_type>;

    [[nodiscard]] queue_type& queue() noexcept {
        if constexpr (Capacity == 0) {
            return *queue_;
        } else {
            return queue_;
        }
    }

    [[nodiscard]] const queue_type& queue() const noexcept {
        if constexpr (Capacity == 0) {
            return *queue_;
        } else {
            return queue_;
        }
    }

    storage_type queue_;
};

} // namespace mpmc
