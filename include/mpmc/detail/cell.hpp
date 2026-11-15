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
// mpmc_queue: a bounded, lock-free, multi-producer/multi-consumer queue.
//
// Cell layout and the 128-byte slot wrapper.  A cell is the single atomic
// object that contains both the user payload and the sequence number, so
// publishing a value is one compare-exchange on one object.

#pragma once

#include "mpmc/concepts.hpp"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace mpmc::detail {

/// Nominal cache-line size used to name the slot stride.  Actual line sizes
/// vary (64 bytes on x86-64, commonly 128 on Apple silicon) and do not change
/// the byte layout below.
inline constexpr std::size_t cache_line_size = 64;

/// Bytes between adjacent slot starts: a fixed 128-byte stride.  On a
/// 64-byte-line target each slot spans two full lines; on a 128-byte-line
/// target it spans one.  Adjacent slots therefore never share the line being
/// written, including with an adjacent-line prefetcher.  The stride is part of
/// the shared-memory ABI.
inline constexpr std::size_t slot_stride = 2 * cache_line_size;

/// Largest cell size in bytes that a hardware compare-exchange covers.
inline constexpr std::size_t max_cell_bytes = 16;

template<class Index>
inline constexpr bool is_supported_index_v = supported_index<Index>;

/// 8 bytes when value+index fit, otherwise 16 bytes.  0 means the combination
/// does not fit in the hardware atomic cell.
template<class T, class Index>
inline constexpr std::size_t cell_bytes_v =
    (sizeof(T) + sizeof(Index) <= 8)    ? 8
    : (sizeof(T) + sizeof(Index) <= 16) ? 16
                                        : 0;

/// The atomic cell.  It contains no padding: `seq` followed by
/// exactly `CellBytes - sizeof(Index)` payload bytes.  The whole object is
/// compared and exchanged by the hardware, including the payload, so a cell
/// cannot expose a torn value.
template<class Index, std::size_t CellBytes>
    requires supported_index<Index> && (CellBytes == 8 || CellBytes == 16) &&
             (sizeof(Index) < CellBytes)
struct alignas(CellBytes) cell {
    /// Sequence marker.  Even values mean empty for this position; odd values
    /// mean published for this position.  The marker is
    /// `(position << 1) | ready`.
    Index seq;
    std::array<std::byte, CellBytes - sizeof(Index)> data;

    friend bool operator==(const cell&, const cell&) = default;
};

template<class Index, std::size_t CellBytes>
inline constexpr std::size_t cell_payload_bytes_v = CellBytes - sizeof(Index);

template<class Index, std::size_t CellBytes>
inline constexpr bool cell_layout_valid_v =
    sizeof(cell<Index, CellBytes>) == CellBytes;

/// Capability probes for the two hardware cell sizes.  These mirror the
/// `std::atomic<cell_type>::is_always_lock_free` assertion in queue_engine so
/// tests, examples and build scripts can select the cell size that matches the
/// target instead of assuming every compiler/target makes 16-byte atomics
/// lock-free.
/// In particular, GCC on x86-64 does not necessarily report 16-byte cells as
/// lock-free solely because `-mcx16` was supplied.
inline constexpr bool has_lock_free_8_byte_cell =
    std::atomic<cell<std::uint32_t, 8>>::is_always_lock_free;
inline constexpr bool has_lock_free_16_byte_cell =
    std::atomic<cell<std::uint64_t, 16>>::is_always_lock_free;

/// The alignment wrapper that gives every entry its own 128-byte slot, so no
/// slot shares a cache line with a neighboring slot.
template<class Cell> struct alignas(slot_stride) slot {
    std::atomic<Cell> value;

    slot() noexcept : value(Cell{}) {}
    explicit slot(const Cell& initial) noexcept : value(initial) {}

    slot(const slot&) = delete;
    slot& operator=(const slot&) = delete;
    slot(slot&&) = delete;
    slot& operator=(slot&&) = delete;
    ~slot() = default;
};

template<class Cell>
inline constexpr bool slot_layout_valid_v =
    sizeof(slot<Cell>) == slot_stride && alignof(slot<Cell>) == slot_stride;

} // namespace mpmc::detail
