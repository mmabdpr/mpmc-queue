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
// The queue algorithm.  It owns no storage: it operates on a state block made
// of two indices and a set of 128-byte slots.  This lets the same algorithm
// back an owning in-process queue and a shared-memory queue without pointers
// being stored inside the shared region.

#pragma once

#include "mpmc/concurrency.hpp"
#include "mpmc/detail/cell.hpp"

#include <algorithm>
#include <atomic>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <type_traits>
#include <utility>

namespace mpmc::detail {

/// Requirements for the value-returning try_pop() overload.  The returned
/// optional must be constructible without any potentially-throwing operation:
/// a nothrow default construction for the temporary and a nothrow move or copy
/// construction into the optional.  Payloads without such a path (for example,
/// payloads with deleted copy and move constructors) are served only by the
/// reference overloads.
template<class T>
concept value_pop_payload = std::default_initializable<T> &&
                            std::is_nothrow_default_constructible_v<T> &&
                            std::is_nothrow_destructible_v<T> &&
                            (std::is_nothrow_move_constructible_v<T> ||
                             std::is_nothrow_copy_constructible_v<T>);

/// Conditions for a try_pop_if predicate: it must be invocable as
/// `predicate(const T&, Index)`, where the sequence number is passed as an
/// `Index` prvalue, return exactly bool (a proxy type with an implicit
/// conversion to bool is rejected), and not throw.  An overload set must
/// therefore accept an `Index` rvalue: by value, `const Index&`, or `Index&&`.
/// A mutable `Index&`-only overload is not supported.
template<class Predicate, class Value, class Index>
concept pop_if_predicate =
    std::invocable<Predicate&, const Value&, Index> &&
    std::same_as<std::invoke_result_t<Predicate&, const Value&, Index>, bool> &&
    std::is_nothrow_invocable_v<Predicate&, const Value&, Index>;

/// Conditions for a consume callback: it must be invocable as
/// `callback(const T&, Index)`, where the sequence number is passed as an
/// `Index` prvalue, return exactly void or bool, and not throw.  An overload
/// set must therefore accept an `Index` rvalue: by value, `const Index&`, or
/// `Index&&`.  A mutable `Index&`-only overload is not supported.
template<class Callback, class Value, class Index>
concept consume_callback =
    std::invocable<Callback&, const Value&, Index> &&
    (std::is_void_v<std::invoke_result_t<Callback&, const Value&, Index>> ||
     std::same_as<std::invoke_result_t<Callback&, const Value&, Index>,
                  bool>) &&
    std::is_nothrow_invocable_v<Callback&, const Value&, Index>;

template<class T, class Index, bool LazyPush, bool LazyPop,
         concurrency Mode = concurrency::mpmc>
    requires queue_payload_for<T, Index>
class alignas(slot_stride) queue_engine {
public:
    using value_type = T;
    using index_type = Index;
    using cell_type = cell<Index, cell_bytes_v<T, Index>>;
    using slot_type = slot<cell_type>;

    static_assert(
        std::atomic<cell_type>::is_always_lock_free,
        "the selected cell is not lock-free on this target.  Some "
        "x86-64 compilers report 16-byte cells as not lock-free even "
        "with -mcx16; the exported CMake target requests -mcx16 only "
        "where it helps.  This assertion, not compiler flags, decides "
        "whether the cell size is supported.  Reduce sizeof(value_type) "
        "+ sizeof(index_type) to 8 bytes for a portable configuration");
    static_assert(cell_layout_valid_v<Index, cell_bytes_v<T, Index>>,
                  "the atomic cell must have no implicit padding");
    static_assert(
        slot_layout_valid_v<cell_type>,
        "slot must occupy exactly the configured slot stride (128 bytes)");

    static constexpr std::size_t cell_bytes = cell_bytes_v<T, Index>;
    static constexpr std::size_t payload_bytes =
        cell_payload_bytes_v<Index, cell_bytes>;
    static constexpr bool is_always_lock_free =
        std::atomic<cell_type>::is_always_lock_free;
    static constexpr bool lazy_push = LazyPush;
    static constexpr bool lazy_pop = LazyPop;
    static constexpr concurrency mode = Mode;
    static constexpr bool single_producer = detail::single_producer_v<Mode>;
    static constexpr bool single_consumer = detail::single_consumer_v<Mode>;

    /// First position that cannot be encoded as `position << 1` with one
    /// ready bit.  The actual publication limit is lower: it reserves
    /// `capacity` positions so that the empty marker written by the last
    /// allowed pop also fits.  With the default 64-bit index the limit is
    /// 2^63 - capacity.
    static constexpr index_type marker_horizon = static_cast<index_type>(
        std::numeric_limits<index_type>::max() / 2 + index_type{1});

    /// Exclusive upper bound on publishable positions.  At capacity 1 this is
    /// `marker_horizon - 1`; reaching it leaves published entries drainable
    /// but stops further pushes until `clear()` starts a new generation.
    [[nodiscard]] index_type publication_limit() const noexcept {
        return static_cast<index_type>(marker_horizon - state_.capacity);
    }

    /// Pointers to the externally owned atomics and slot array.
    struct state {
        slot_type* slots;
        std::atomic<index_type>* write_index;
        std::atomic<index_type>* read_index;
        index_type capacity;
        index_type mask;
    };

    queue_engine() noexcept = default;

    queue_engine(const queue_engine&) = delete;
    queue_engine& operator=(const queue_engine&) = delete;
    queue_engine(queue_engine&&) = delete;
    queue_engine& operator=(queue_engine&&) = delete;
    ~queue_engine() = default;

    // ---------------------------------------------------------------- push

    /// Try to publish `value`.  On success stores the queue position in
    /// `sequence`; the position is unique and strictly increasing.
    [[nodiscard]] bool try_push(const value_type& value,
                                index_type& sequence) noexcept {
        cell_type desired = make_cell(value, index_type{0});

        for (;;) {
            // Cursors are monotonic hints.  The cell CAS is the publication
            // point, so the hint load needs no acquire fence.
            const index_type write = write_cursor();
            if (write >= publication_limit()) {
                return false;
            }
            slot_type& slot = slot_for(write);

            cell_type expected = slot.value.load(std::memory_order_relaxed);
            const index_type position = marker_position(expected.seq);
            if (position == write) {
                if (!marker_is_full(expected.seq)) {
                    desired.seq = full_marker(write);
                    if (slot.value.compare_exchange_strong(
                            expected, desired, std::memory_order_release,
                            std::memory_order_relaxed)) {
                        sequence = write;
                        if constexpr (!LazyPush) {
                            publish_write(write);
                        }
                        return true;
                    }
                    continue;
                }

                // Another producer published this position but has not moved
                // the write index yet.  Help it; the attempted value is not
                // published.
                advance_write(write);
                continue;
            }

            const index_type behind = static_cast<index_type>(write - position);
            if (behind == state_.capacity) {
                // The slot still carries the previous lap's full marker.
                return false;
            }

            if (behind > half_range) {
                // The slot has moved past `write`: a consumer already took
                // this position.  Help advance the write index and retry.
                advance_write(write);
                continue;
            }

            // Stale or inconsistent snapshot.  Reload and retry.
        }
    }

    [[nodiscard]] bool try_push(const value_type& value) noexcept {
        index_type unused{};
        return try_push(value, unused);
    }

    [[nodiscard]] bool push(const value_type& value) noexcept {
        return try_push(value);
    }

    [[nodiscard]] bool push(const value_type& value,
                            index_type& sequence) noexcept {
        return try_push(value, sequence);
    }

    // ----------------------------------------------------------------- pop

    /// Try to consume one value.  `sequence` receives the position that was
    /// consumed.
    [[nodiscard]] bool try_pop(value_type& value,
                               index_type& sequence) noexcept {
        for (;;) {
            const index_type read = read_cursor();
            if (read >= publication_limit()) {
                return false;
            }
            slot_type& slot = slot_for(read);
            // The successful removal CAS is the acquiring operation.  A
            // relaxed snapshot is enough to classify the cell before that.
            cell_type expected = slot.value.load(std::memory_order_relaxed);
            const index_type position = marker_position(expected.seq);

            if (position == read) {
                if (!marker_is_full(expected.seq)) {
                    return false;
                }

                cell_type empty = make_empty_cell(empty_marker(
                    static_cast<index_type>(read + state_.capacity)));
                if (slot.value.compare_exchange_strong(
                        expected, empty, std::memory_order_acquire,
                        std::memory_order_relaxed)) {
                    copy_value(expected, value);
                    sequence = read;
                    if constexpr (!LazyPop) {
                        publish_read(read);
                    }
                    return true;
                }
                continue;
            }

            const index_type ahead = static_cast<index_type>(position - read);
            if (ahead < half_range) {
                // Another consumer consumed this position (and possibly a
                // producer has already reused the slot).  Help advance the
                // read index.
                advance_read(read);
                continue;
            }

            // Stale or inconsistent snapshot.  Reload and retry.
        }
    }

    [[nodiscard]] bool try_pop(value_type& value) noexcept {
        index_type unused{};
        return try_pop(value, unused);
    }

    [[nodiscard]] bool pop(value_type& value) noexcept {
        return try_pop(value);
    }

    [[nodiscard]] bool pop(value_type& value, index_type& sequence) noexcept {
        return try_pop(value, sequence);
    }

    /// Convenience value-returning pop.  Participates only when the payload is
    /// nothrow default-constructible and nothrow move- or copy-constructible,
    /// so the returned optional can be built without throwing.  The overload
    /// is therefore `noexcept`.  Payloads that do not satisfy these
    /// constraints (for example, payloads with deleted copy and move
    /// constructors) must use the reference overload above, which has no such
    /// requirement.
    [[nodiscard]] std::optional<value_type> try_pop() noexcept
        requires value_pop_payload<value_type>
    {
        value_type value{};
        if (!try_pop(value)) {
            return std::nullopt;
        }
        // Construct the optional in place from the exact argument type each
        // trait checks.  Using the const-qualified lvalue avoids selecting a
        // payload's templated forwarding constructor when falling back to the
        // copy path.
        if constexpr (std::is_nothrow_move_constructible_v<value_type>) {
            return std::optional<value_type>{std::in_place, std::move(value)};
        } else {
            return std::optional<value_type>{std::in_place,
                                             std::as_const(value)};
        }
    }

    // ----------------------------------------------------------------- peek

    [[nodiscard]] bool try_peek(value_type& value,
                                index_type& sequence) noexcept {
        for (;;) {
            const index_type read = read_cursor();
            if (read >= publication_limit()) {
                return false;
            }
            slot_type& slot = slot_for(read);
            const cell_type current =
                slot.value.load(std::memory_order_acquire);
            const index_type position = marker_position(current.seq);

            if (position == read) {
                if (!marker_is_full(current.seq)) {
                    return false;
                }
                copy_value(current, value);
                sequence = read;
                return true;
            }

            const index_type ahead = static_cast<index_type>(position - read);
            if (ahead < half_range) {
                advance_read(read);
                continue;
            }
            return false;
        }
    }

    [[nodiscard]] bool try_peek(value_type& value) noexcept {
        index_type unused{};
        return try_peek(value, unused);
    }

    [[nodiscard]] bool peek(value_type& value) noexcept {
        return try_peek(value);
    }

    [[nodiscard]] bool peek(value_type& value, index_type& sequence) noexcept {
        return try_peek(value, sequence);
    }

    // ------------------------------------------------------------ pop_if

    /// Pop the head only when `predicate(value, sequence)` returns exactly
    /// `bool`.  The sequence number is passed to the predicate as an
    /// `index_type` prvalue.  Both overloads require a nothrow
    /// default-constructible payload (they build a candidate) and a nothrow
    /// predicate; a proxy type with an implicit conversion to bool is rejected
    /// so the operation cannot throw.  The predicate may be called more than
    /// once if the entry is concurrently replaced; it must be free of side
    /// effects.
    template<class Predicate>
        requires std::default_initializable<value_type> &&
                 std::is_nothrow_default_constructible_v<value_type> &&
                 pop_if_predicate<Predicate, value_type, index_type>
    [[nodiscard]] bool try_pop_if(Predicate&& predicate, value_type& value,
                                  index_type& sequence) noexcept {
        for (;;) {
            const index_type read = read_cursor();
            if (read >= publication_limit()) {
                return false;
            }
            slot_type& slot = slot_for(read);
            cell_type expected = slot.value.load(std::memory_order_acquire);
            const index_type position = marker_position(expected.seq);

            if (position == read) {
                if (!marker_is_full(expected.seq)) {
                    return false;
                }

                value_type candidate{};
                copy_value(expected, candidate);
                if (!std::invoke(predicate, std::as_const(candidate),
                                 index_type{read})) {
                    return false;
                }

                cell_type empty = make_empty_cell(empty_marker(
                    static_cast<index_type>(read + state_.capacity)));
                if (slot.value.compare_exchange_strong(
                        expected, empty, std::memory_order_acq_rel,
                        std::memory_order_acquire)) {
                    copy_value(expected, value);
                    sequence = read;
                    if constexpr (!LazyPop) {
                        advance_read(read);
                    }
                    return true;
                }
                continue;
            }

            const index_type ahead = static_cast<index_type>(position - read);
            if (ahead < half_range) {
                advance_read(read);
                continue;
            }
            return false;
        }
    }

    template<class Predicate>
        requires std::default_initializable<value_type> &&
                 std::is_nothrow_default_constructible_v<value_type> &&
                 pop_if_predicate<Predicate, value_type, index_type>
    [[nodiscard]] bool try_pop_if(Predicate&& predicate,
                                  value_type& value) noexcept {
        index_type unused{};
        return try_pop_if(std::forward<Predicate>(predicate), value, unused);
    }

    // ------------------------------------------------------------- consume

    /// Pop values until the queue is empty or the callback returns true.
    /// Returns the number of consumed values.  A callback may take
    /// `(const value_type&, index_type)` and must return exactly void or bool
    /// (a proxy type with an implicit conversion to bool is rejected) without
    /// throwing.  The sequence number is passed as an `index_type` prvalue, so
    /// an `index_type&&`, by-value, or `const index_type&` parameter works but
    /// an `index_type&` parameter does not.  The payload must be nothrow
    /// default-constructible.
    template<class F>
        requires std::default_initializable<value_type> &&
                 std::is_nothrow_default_constructible_v<value_type> &&
                 consume_callback<F, value_type, index_type>
    std::size_t consume(F&& callback) noexcept {
        using result_type =
            std::invoke_result_t<F&, const value_type&, index_type>;

        std::size_t count = 0;
        value_type value{};
        index_type sequence{};
        while (try_pop(value, sequence)) {
            ++count;
            if constexpr (std::is_void_v<result_type>) {
                std::invoke(callback, std::as_const(value),
                            index_type{sequence});
            } else {
                if (std::invoke(callback, std::as_const(value),
                                index_type{sequence})) {
                    break;
                }
            }
        }
        return count;
    }

    // ------------------------------------------------------------- replace

    /// Replace the value at `sequence` only if it still carries
    /// `expected_value`.  This is the atomic exchange/conflation primitive.
    [[nodiscard]] bool try_replace(index_type sequence,
                                   const value_type& expected_value,
                                   const value_type& desired_value) noexcept {
        if (sequence >= publication_limit()) {
            return false;
        }
        cell_type expected = make_cell(expected_value, full_marker(sequence));
        cell_type desired = make_cell(desired_value, full_marker(sequence));
        return slot_for(sequence).value.compare_exchange_strong(
            expected, desired, std::memory_order_acq_rel,
            std::memory_order_acquire);
    }

    /// Replace the value at `sequence` if that position is still unconsumed.
    /// Does not require knowledge of the previous value.  Returns false if
    /// the entry was consumed or changed concurrently.
    [[nodiscard]] bool try_replace(index_type sequence,
                                   const value_type& desired_value) noexcept {
        if (sequence >= publication_limit()) {
            return false;
        }
        slot_type& slot = slot_for(sequence);
        cell_type expected = slot.value.load(std::memory_order_acquire);
        if (marker_position(expected.seq) != sequence ||
            !marker_is_full(expected.seq)) {
            return false;
        }

        cell_type desired = expected;
        write_value(desired, desired_value);
        return slot.value.compare_exchange_strong(expected, desired,
                                                  std::memory_order_acq_rel,
                                                  std::memory_order_acquire);
    }

    /// Best-effort replacement of the most recently published entry.  If the
    /// entry has already been consumed, returns false.
    ///
    /// In lazy-push mode the write index may still point at the newest entry,
    /// so both `write` and `write - 1` are considered.
    [[nodiscard]] bool
    try_replace_last(const value_type& desired_value,
                     index_type* sequence = nullptr) noexcept {
        const index_type write =
            state_.write_index->load(std::memory_order_acquire);
        if (try_replace(write, desired_value)) {
            if (sequence != nullptr) {
                *sequence = write;
            }
            return true;
        }
        if (write != index_type{0}) {
            const index_type last =
                static_cast<index_type>(write - index_type{1});
            if (try_replace(last, desired_value)) {
                if (sequence != nullptr) {
                    *sequence = last;
                }
                return true;
            }
        }
        return false;
    }

    /// Replace the newest still-unconsumed entry if possible, otherwise push
    /// a new entry.  Useful for conflation of market-data style updates.
    [[nodiscard]] bool
    try_push_or_replace(const value_type& value,
                        index_type* sequence = nullptr) noexcept {
        index_type seq{};
        if (try_replace_last(value, &seq)) {
            if (sequence != nullptr) {
                *sequence = seq;
            }
            return true;
        }
        if (try_push(value, seq)) {
            if (sequence != nullptr) {
                *sequence = seq;
            }
            return true;
        }
        return false;
    }

    /// Push, dropping the oldest entry when the queue is full.  Under
    /// concurrent consumption this is a best-effort "keep the newest"
    /// operation: any consumer may remove the entry that made room.  Requires
    /// a nothrow default-constructible payload and is `noexcept`.
    [[nodiscard]] bool push_keep_n(const value_type& value) noexcept
        requires std::default_initializable<value_type> &&
                 std::is_nothrow_default_constructible_v<value_type> &&
                 (!single_consumer)
    {
        index_type seq{};
        for (;;) {
            if (try_push(value, seq)) {
                return true;
            }
            if (sequence_exhausted()) {
                // Never discard the remaining backlog merely because no new
                // publication position is available.
                return false;
            }
            value_type discarded{};
            if (try_pop(discarded, seq)) {
                continue;
            }
            // The queue is empty but the push still failed: the sequence
            // horizon has been reached (or another thread consumed the entry
            // that made room).  Do not spin forever at the horizon.
            return false;
        }
    }

    // --------------------------------------------------------------- state

    [[nodiscard]] bool empty() const noexcept {
        for (;;) {
            // Observers must not read an owner-local cursor: for single-
            // consumer modes that field is written only by the consumer.
            const index_type read =
                state_.read_index->load(std::memory_order_acquire);
            if (read >= publication_limit()) {
                return true;
            }
            const cell_type current =
                slot_for(read).value.load(std::memory_order_acquire);
            const index_type position = marker_position(current.seq);

            if (position == read) {
                return !marker_is_full(current.seq);
            }

            const index_type ahead = static_cast<index_type>(position - read);
            if (ahead < half_range) {
                // Help the shared hint without touching the consumer's owner
                // cursor.  The consumer reads the maximum of its owner value
                // and this mirror, so the two cannot move backwards.
                advance_read_shared(read);
                continue;
            }
            return false;
        }
    }

    /// Approximate number of entries.  The indices are advanced cooperatively,
    /// so this is a snapshot rather than a synchronization primitive.
    [[nodiscard]] std::size_t size() const noexcept {
        const index_type write =
            state_.write_index->load(std::memory_order_acquire);
        const index_type read =
            state_.read_index->load(std::memory_order_acquire);
        const index_type delta = static_cast<index_type>(write - read);
        // With lazy push, a consumer may advance the read index before the
        // next producer advances the write index.  In that transient the
        // unsigned difference is huge; report an empty snapshot rather than
        // clamping it to capacity.
        if (delta > state_.capacity) {
            return 0;
        }
        return static_cast<std::size_t>(delta);
    }

    [[nodiscard]] std::size_t capacity() const noexcept {
        return static_cast<std::size_t>(state_.capacity);
    }

    /// True once an index has reached the publication limit.  Entries already
    /// published can still be popped; further pushes fail instead of wrapping a
    /// marker onto an old position.  `clear()` resets the indices and must not
    /// run concurrently with any other operation.
    ///
    /// In lazy modes an index can lag the physical slot marker until another
    /// operation helps advance it.  This diagnostic can therefore report
    /// false shortly after the final publication or pop; a following push or
    /// pop attempt observes the limit and catches the index up.
    [[nodiscard]] bool sequence_exhausted() const noexcept {
        return state_.write_index->load(std::memory_order_acquire) >=
                   publication_limit() ||
               state_.read_index->load(std::memory_order_acquire) >=
                   publication_limit();
    }

    /// Number of publications left before the publication limit.  This is a
    /// snapshot and is intended for monitoring, not synchronization.  In lazy
    /// modes it can overstate the remaining positions until a later operation
    /// advances the write index.
    [[nodiscard]] index_type publications_remaining() const noexcept {
        const index_type write =
            state_.write_index->load(std::memory_order_acquire);
        return write < publication_limit()
                   ? static_cast<index_type>(publication_limit() - write)
                   : index_type{0};
    }

    [[nodiscard]] static constexpr bool is_lock_free() noexcept {
        return is_always_lock_free;
    }

    /// Reset the queue to empty, starting a new sequence generation at
    /// position zero.  This invalidates every previously returned sequence
    /// token: a token from the old generation can compare equal to a position
    /// in the new generation and then name an unrelated entry, so tokens must
    /// not be retained across clear().  For a shared-memory queue the
    /// instance_id() is unchanged; all local threads and remote processes must
    /// be quiescent before clear() is called.  Not safe concurrently with any
    /// other operation; intended for construction and single-threaded reuse.
    void clear() noexcept {
        initialize();
    }

protected:
    void reset(const state& new_state) noexcept {
        state_ = new_state;
    }

    /// Detach from externally owned storage (used when moving an owning
    /// adapter such as shared_memory_queue).
    void reset() noexcept {
        state_ = state{};
    }

    [[nodiscard]] const state& current_state() const noexcept {
        return state_;
    }

    void initialize() noexcept {
        owner_write_ = index_type{0};
        owner_read_ = index_type{0};
        for (std::size_t i = 0; i < static_cast<std::size_t>(state_.capacity);
             ++i) {
            state_.slots[i].value.store(
                make_empty_cell(empty_marker(static_cast<index_type>(i))),
                std::memory_order_relaxed);
        }
        state_.write_index->store(index_type{0}, std::memory_order_relaxed);
        state_.read_index->store(index_type{0}, std::memory_order_relaxed);
    }

private:
    static constexpr index_type half_range =
        static_cast<index_type>(std::numeric_limits<index_type>::max() / 2);

    static constexpr index_type empty_marker(index_type position) noexcept {
        return static_cast<index_type>(static_cast<std::uint64_t>(position)
                                       << 1U);
    }

    static constexpr index_type full_marker(index_type position) noexcept {
        return static_cast<index_type>(
            (static_cast<std::uint64_t>(position) << 1U) | std::uint64_t{1});
    }

    static constexpr index_type marker_position(index_type marker) noexcept {
        return static_cast<index_type>(static_cast<std::uint64_t>(marker) >>
                                       1U);
    }

    static constexpr bool marker_is_full(index_type marker) noexcept {
        return (static_cast<std::uint64_t>(marker) & std::uint64_t{1}) != 0;
    }

    [[nodiscard]] slot_type& slot_for(index_type position) noexcept {
        return state_.slots[static_cast<std::size_t>(position & state_.mask)];
    }

    [[nodiscard]] const slot_type&
    slot_for(index_type position) const noexcept {
        return state_.slots[static_cast<std::size_t>(position & state_.mask)];
    }

    static cell_type make_cell(const value_type& value,
                               index_type sequence) noexcept {
        cell_type result{};
        result.seq = sequence;
        write_value(result, value);
        return result;
    }

    static cell_type make_empty_cell(index_type sequence) noexcept {
        cell_type result{};
        result.seq = sequence;
        return result;
    }

    static void write_value(cell_type& target,
                            const value_type& value) noexcept {
        static_assert(sizeof(value_type) <= payload_bytes);
        std::memcpy(target.data.data(),
                    static_cast<const void*>(std::addressof(value)),
                    sizeof(value_type));
    }

    static void copy_value(const cell_type& source,
                           value_type& value) noexcept {
        // The destination is a trivially copyable payload by contract; the
        // void* cast also keeps -Wclass-memaccess quiet for payload types that
        // are trivially copyable but have a deleted assignment operator.
        std::memcpy(static_cast<void*>(std::addressof(value)),
                    source.data.data(), sizeof(value_type));
    }

    // The cursor is a monotonic hint.  The whole-cell CAS is the publication
    // point.  A single-role owner can publish the next cursor with a relaxed
    // store; concurrent producers or consumers use a compare-exchange so a
    // delayed participant cannot move the hint backwards.  The helping CAS
    // below advances a lagging hint one position at a time.
    [[nodiscard]] index_type write_cursor() const noexcept {
        if constexpr (single_producer) {
            return owner_write_;
        } else {
            return state_.write_index->load(std::memory_order_relaxed);
        }
    }

    [[nodiscard]] index_type read_cursor() const noexcept {
        if constexpr (single_consumer) {
            // An observer may have advanced the shared mirror.  The consumer
            // must not step behind it when it next wakes up.
            const index_type mirror =
                state_.read_index->load(std::memory_order_relaxed);
            return owner_read_ > mirror ? owner_read_ : mirror;
        } else {
            return state_.read_index->load(std::memory_order_relaxed);
        }
    }

    void publish_write(index_type position) const noexcept {
        const index_type next =
            static_cast<index_type>(position + index_type{1});
        if constexpr (single_producer) {
            owner_write_ = next;
            state_.write_index->store(next, std::memory_order_relaxed);
        } else {
            // Multiple producers publish concurrently.  Only move the index
            // when it still names `position`; a stale producer must not move
            // the hint backwards after another producer has advanced it.
            state_.write_index->compare_exchange_strong(
                position, next, std::memory_order_relaxed,
                std::memory_order_relaxed);
        }
    }

    void publish_read(index_type position) const noexcept {
        const index_type next =
            static_cast<index_type>(position + index_type{1});
        if constexpr (single_consumer) {
            if (next > owner_read_) {
                owner_read_ = next;
            }
            // An observer may already have advanced the shared mirror.
            index_type mirror =
                state_.read_index->load(std::memory_order_relaxed);
            while (next > mirror && !state_.read_index->compare_exchange_weak(
                                        mirror, next, std::memory_order_relaxed,
                                        std::memory_order_relaxed)) {
            }
        } else {
            // Multiple consumers publish concurrently; keep the hint
            // monotonic for the same reason as publish_write().
            state_.read_index->compare_exchange_strong(
                position, next, std::memory_order_relaxed,
                std::memory_order_relaxed);
        }
    }

    void advance_write(index_type expected) const noexcept {
        if constexpr (single_producer) {
            owner_write_ = static_cast<index_type>(expected + index_type{1});
            state_.write_index->store(owner_write_, std::memory_order_relaxed);
        } else {
            state_.write_index->compare_exchange_strong(
                expected, static_cast<index_type>(expected + index_type{1}),
                std::memory_order_relaxed, std::memory_order_relaxed);
        }
    }

    void advance_read(index_type expected) const noexcept {
        const index_type next =
            static_cast<index_type>(expected + index_type{1});
        if constexpr (single_consumer) {
            if (next > owner_read_) {
                owner_read_ = next;
            }
            advance_read_shared(expected);
        } else {
            state_.read_index->compare_exchange_strong(
                expected, next, std::memory_order_relaxed,
                std::memory_order_relaxed);
        }
    }

    /// Advance the shared read mirror only.  Safe for an observer in a
    /// single-consumer queue because it never touches the owner cursor.
    void advance_read_shared(index_type expected) const noexcept {
        const index_type next =
            static_cast<index_type>(expected + index_type{1});
        index_type mirror = state_.read_index->load(std::memory_order_relaxed);
        while (next > mirror && !state_.read_index->compare_exchange_weak(
                                    mirror, next, std::memory_order_relaxed,
                                    std::memory_order_relaxed)) {
        }
    }

    alignas(slot_stride) mutable index_type owner_write_{};
    alignas(slot_stride) mutable index_type owner_read_{};
    alignas(slot_stride) state state_{};
};

} // namespace mpmc::detail
