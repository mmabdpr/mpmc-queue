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
#include "mpmc/mpmc.hpp"

#include "test_cell_config.hpp"
#include "test_framework.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace {

struct no_default {
    no_default() = delete;
    explicit no_default(std::uint32_t value_in) : value(value_in) {}
    std::uint32_t value;
};

struct twelve_byte_payload {
    std::uint32_t a;
    std::uint32_t b;
    std::uint32_t c;
};

/// Trivially copyable, but neither copyable nor movable.  It can travel
/// through the queue via raw bytes, so the reference overloads work while the
/// value-returning overload must not participate.
struct immovable_payload {
    std::uint32_t value;

    immovable_payload() = default;
    immovable_payload(const immovable_payload&) = delete;
    immovable_payload(immovable_payload&&) = delete;
    immovable_payload& operator=(const immovable_payload&) = delete;
    immovable_payload& operator=(immovable_payload&&) = delete;
};

/// Copy-only payload whose templated forwarding constructor throws.  The
/// value-returning pop must select the trivial copy constructor (which is
/// selected only for a const lvalue) rather than this template.
struct copy_only_forwarding_payload {
    std::uint32_t value = 0;

    copy_only_forwarding_payload() = default;
    copy_only_forwarding_payload(const copy_only_forwarding_payload&) = default;
    copy_only_forwarding_payload(copy_only_forwarding_payload&&) = delete;
    copy_only_forwarding_payload&
    operator=(const copy_only_forwarding_payload&) = default;
    copy_only_forwarding_payload&
    operator=(copy_only_forwarding_payload&&) = delete;

    // Throws if an unintended forwarding overload is selected.
    template<class U>
    // NOLINTNEXTLINE(bugprone-forwarding-reference-overload)
    copy_only_forwarding_payload(U&&) {
        throw std::runtime_error{"throwing forwarding constructor"};
    }
};

/// Trivially movable but not copyable.  The value-returning pop must use the
/// move path without instantiating a deleted copy constructor.
struct move_only_payload {
    std::uint32_t value = 0;

    move_only_payload() = default;
    move_only_payload(const move_only_payload&) = delete;
    move_only_payload(move_only_payload&&) = default;
    move_only_payload& operator=(const move_only_payload&) = delete;
    move_only_payload& operator=(move_only_payload&&) = default;
};

/// Trivially copyable payload with a throwing default constructor.  The
/// value-returning pop and the helpers that build a temporary must not
/// participate.
struct throwing_default_payload {
    std::uint32_t value = 0;

    throwing_default_payload() noexcept(false) {}
};

/// Trivially movable payload with an unrelated throwing forwarding
/// constructor.  The value-returning pop must use the non-throwing move.
struct throwing_forward_payload {
    std::uint32_t value = 0;

    throwing_forward_payload() = default;
    throwing_forward_payload(const throwing_forward_payload&) = default;
    throwing_forward_payload(throwing_forward_payload&&) = default;

    // Throws if an unintended forwarding overload is selected.
    template<class U>
    // NOLINTNEXTLINE(bugprone-forwarding-reference-overload)
    throwing_forward_payload(U&&) {
        throw std::runtime_error{"throwing forwarding constructor"};
    }
};

static_assert(std::is_trivially_copyable_v<no_default>);
static_assert(std::is_trivially_copyable_v<immovable_payload>);
static_assert(std::is_trivially_copyable_v<copy_only_forwarding_payload>);
static_assert(std::is_trivially_copyable_v<move_only_payload>);
static_assert(std::is_trivially_copyable_v<throwing_default_payload>);
static_assert(std::is_trivially_copyable_v<throwing_forward_payload>);
static_assert(
    !std::is_nothrow_move_constructible_v<copy_only_forwarding_payload>);
static_assert(
    std::is_nothrow_copy_constructible_v<copy_only_forwarding_payload>);
static_assert(std::is_nothrow_move_constructible_v<throwing_forward_payload>);
static_assert(sizeof(twelve_byte_payload) == 12);

using cell_value = mpmc_test::wide_value;
using cell_index = mpmc_test::wide_index;

template<class Payload>
inline constexpr bool value_pop_available_v = requires(
    mpmc::mpmc_queue<Payload, 4, std::uint32_t>& queue) { queue.try_pop(); };

template<class Payload>
inline constexpr bool reference_pop_available_v =
    requires(mpmc::mpmc_queue<Payload, 4, std::uint32_t>& queue,
             Payload& value) { queue.try_pop(value); };

template<class Payload>
inline constexpr bool push_keep_n_available_v =
    requires(mpmc::mpmc_queue<Payload, 4, std::uint32_t>& queue,
             const Payload& value) { queue.push_keep_n(value); };

struct bool_proxy {
    operator bool() const noexcept {
        return true;
    }
};

struct throwing_bool_proxy {
    operator bool() const {
        return true;
    }
};

struct bool_predicate {
    bool operator()(const cell_value&, cell_index) const noexcept {
        return true;
    }
};

struct void_predicate {
    void operator()(const cell_value&, cell_index) const noexcept {}
};

struct proxy_predicate {
    bool_proxy operator()(const cell_value&, cell_index) const noexcept {
        return {};
    }
};

struct throwing_proxy_predicate {
    throwing_bool_proxy operator()(const cell_value&,
                                   cell_index) const noexcept {
        return {};
    }
};

struct int_predicate {
    int operator()(const cell_value&, cell_index) const noexcept {
        return 1;
    }
};

struct throwing_predicate {
    bool operator()(const cell_value&, cell_index) const {
        return true;
    }
};

struct void_consume_callback {
    void operator()(const cell_value&, cell_index) const noexcept {}
};

struct bool_consume_callback {
    bool operator()(const cell_value&, cell_index) const noexcept {
        return false;
    }
};

struct int_consume_callback {
    int operator()(const cell_value&, cell_index) const noexcept {
        return 0;
    }
};

struct proxy_consume_callback {
    bool_proxy operator()(const cell_value&, cell_index) const noexcept {
        return {};
    }
};

struct throwing_proxy_consume_callback {
    throwing_bool_proxy operator()(const cell_value&,
                                   cell_index) const noexcept {
        return {};
    }
};

struct throwing_consume_callback {
    void operator()(const cell_value&, cell_index) const {}
};

// The queue passes the sequence number as an index_type prvalue.  Each
// overload set below makes that value category observable: the rvalue overload
// is the only noexcept one, while the lvalue overload records its selection
// and throws, so a mismatch terminates inside the queue's noexcept operation.
template<class Index> struct overloaded_predicate {
    static inline int selected = 0;

    bool operator()(const cell_value&, Index&&) const noexcept {
        selected = 1;
        return true;
    }

    bool operator()(const cell_value&, const Index&) const {
        selected = 2;
        throw std::runtime_error{"predicate received an lvalue index"};
    }
};

template<class Index> struct rvalue_only_predicate {
    bool operator()(const cell_value&, Index&&) const noexcept {
        return true;
    }
};

template<class Index> struct lvalue_ref_predicate {
    bool operator()(const cell_value&, Index&) const noexcept {
        return true;
    }
};

// The rvalue overload returns exactly bool while the lvalue overload returns a
// bool proxy.  The exact-result requirement must inspect the rvalue overload,
// and the actual invocation must select it too.
template<class Index> struct mixed_return_predicate {
    static inline int selected = 0;

    bool operator()(const cell_value&, Index&&) const noexcept {
        selected = 1;
        return true;
    }

    bool_proxy operator()(const cell_value&, const Index&) const noexcept {
        selected = 2;
        return {};
    }
};

// The mirror image: only the lvalue overload returns exactly bool, so the
// predicate must be rejected rather than accepting the lvalue result.
template<class Index> struct lvalue_bool_rvalue_proxy_predicate {
    bool_proxy operator()(const cell_value&, Index&&) const noexcept {
        return {};
    }

    bool operator()(const cell_value&, const Index&) const noexcept {
        return true;
    }
};

template<class Index> struct overloaded_void_consume_callback {
    static inline int selected = 0;

    void operator()(const cell_value&, Index&&) const noexcept {
        selected = 1;
    }

    void operator()(const cell_value&, const Index&) const {
        selected = 2;
        throw std::runtime_error{"callback received an lvalue index"};
    }
};

template<class Index> struct overloaded_bool_consume_callback {
    static inline int selected = 0;

    bool operator()(const cell_value&, Index&&) const noexcept {
        selected = 1;
        return true;
    }

    bool operator()(const cell_value&, const Index&) const {
        selected = 2;
        throw std::runtime_error{"callback received an lvalue index"};
    }
};

template<class Index> struct rvalue_only_void_consume_callback {
    void operator()(const cell_value&, Index&&) const noexcept {}
};

template<class Index> struct rvalue_only_bool_consume_callback {
    bool operator()(const cell_value&, Index&&) const noexcept {
        return false;
    }
};

template<class Index> struct lvalue_ref_void_consume_callback {
    void operator()(const cell_value&, Index&) const noexcept {}
};

template<class Index> struct lvalue_ref_bool_consume_callback {
    bool operator()(const cell_value&, Index&) const noexcept {
        return false;
    }
};

template<class Index> struct mixed_return_consume_callback {
    static inline int selected = 0;

    bool operator()(const cell_value&, Index&&) const noexcept {
        selected = 1;
        return false;
    }

    int operator()(const cell_value&, const Index&) const noexcept {
        selected = 2;
        return 0;
    }
};

template<class Index> struct lvalue_bool_rvalue_int_consume_callback {
    int operator()(const cell_value&, Index&&) const noexcept {
        return 0;
    }
    bool operator()(const cell_value&, const Index&) const noexcept {
        return false;
    }
};

template<class Index> struct mixed_void_bool_consume_callback {
    static inline int selected = 0;

    void operator()(const cell_value&, Index&&) const noexcept {
        selected = 1;
    }

    bool operator()(const cell_value&, const Index&) const noexcept {
        selected = 2;
        return true;
    }
};

template<class Predicate>
inline constexpr bool pop_if_available_v =
    requires(mpmc::mpmc_queue<cell_value, 4, cell_index>& queue,
             Predicate& predicate, cell_value& value, cell_index& sequence) {
        queue.try_pop_if(predicate, value, sequence);
    } &&
    requires(mpmc::mpmc_queue<cell_value, 4, cell_index>& queue,
             Predicate& predicate,
             cell_value& value) { queue.try_pop_if(predicate, value); };

template<class Callback>
inline constexpr bool consume_available_v =
    requires(mpmc::mpmc_queue<cell_value, 4, cell_index>& queue,
             Callback& callback) { queue.consume(callback); };

// The value-returning overload is only available when its actual return
// construction is possible and non-throwing.
static_assert(value_pop_available_v<cell_value>);
static_assert(value_pop_available_v<throwing_forward_payload>);
static_assert(value_pop_available_v<copy_only_forwarding_payload>);
static_assert(value_pop_available_v<move_only_payload>);
static_assert(!value_pop_available_v<no_default>);
static_assert(!value_pop_available_v<throwing_default_payload>);
static_assert(!value_pop_available_v<immovable_payload>);
static_assert(reference_pop_available_v<no_default>);
static_assert(reference_pop_available_v<immovable_payload>);

static_assert(push_keep_n_available_v<cell_value>);
static_assert(!push_keep_n_available_v<no_default>);
static_assert(!push_keep_n_available_v<throwing_default_payload>);

// Both try_pop_if overloads require exactly bool and reject proxy
// conversions; consume accepts exactly void or bool.
static_assert(pop_if_available_v<bool_predicate>);
static_assert(!pop_if_available_v<void_predicate>);
static_assert(!pop_if_available_v<proxy_predicate>);
static_assert(!pop_if_available_v<throwing_proxy_predicate>);
static_assert(!pop_if_available_v<int_predicate>);
static_assert(!pop_if_available_v<throwing_predicate>);

static_assert(consume_available_v<void_consume_callback>);
static_assert(consume_available_v<bool_consume_callback>);
static_assert(!consume_available_v<int_consume_callback>);
static_assert(!consume_available_v<proxy_consume_callback>);
static_assert(!consume_available_v<throwing_proxy_consume_callback>);
static_assert(!consume_available_v<throwing_consume_callback>);

// Public noexcept coverage for exception-free operations and callbacks.
using checked_queue = mpmc::mpmc_queue<cell_value, 4, cell_index>;
static_assert(noexcept(std::declval<checked_queue&>().try_pop()));
static_assert(noexcept(
    std::declval<checked_queue&>().try_pop(std::declval<cell_value&>())));
static_assert(noexcept(std::declval<checked_queue&>().try_pop_if(
    std::declval<bool_predicate&>(), std::declval<cell_value&>())));
static_assert(noexcept(std::declval<checked_queue&>().try_pop_if(
    std::declval<bool_predicate&>(), std::declval<cell_value&>(),
    std::declval<cell_index&>())));
static_assert(noexcept(std::declval<checked_queue&>().consume(
    std::declval<void_consume_callback&>())));
static_assert(noexcept(std::declval<checked_queue&>().consume(
    std::declval<bool_consume_callback&>())));
static_assert(noexcept(std::declval<checked_queue&>().push_keep_n(
    std::declval<const cell_value&>())));
static_assert(
    noexcept(std::declval<const checked_queue&>().sequence_exhausted()));
static_assert(
    noexcept(std::declval<const checked_queue&>().publications_remaining()));
static_assert(
    noexcept(std::declval<const checked_queue&>().publication_limit()));

// Focused value-category contract probes.  The sequence number is passed as an
// Index prvalue, so the same probes are instantiated for 32- and 64-bit
// indices.  A small payload keeps the 64-bit-index queue a 16-byte cell.
using contract_value = std::uint32_t;

template<class Index>
using contract_queue = mpmc::mpmc_queue<contract_value, 4, Index>;

template<class Index, class Predicate>
inline constexpr bool pop_if_available_for_v =
    requires(contract_queue<Index>& queue, Predicate& predicate,
             contract_value& value, Index& sequence) {
        queue.try_pop_if(predicate, value, sequence);
    } &&
    requires(contract_queue<Index>& queue, Predicate& predicate,
             contract_value& value) { queue.try_pop_if(predicate, value); };

template<class Index, class Callback>
inline constexpr bool consume_available_for_v =
    requires(contract_queue<Index>& queue, Callback& callback) {
        queue.consume(callback);
    };

// Accepted: an overload set with a noexcept Index&& overload, a predicate
// whose exact result exists only on the rvalue overload, and a callback that
// can only be called with an rvalue index.
// Rejected: a mutable Index&-only predicate or callback, and an overload set
// whose rvalue result is not exactly bool.
template<class Index>
inline constexpr bool predicate_value_category_contract_v =
    pop_if_available_for_v<Index, overloaded_predicate<Index>> &&
    pop_if_available_for_v<Index, rvalue_only_predicate<Index>> &&
    pop_if_available_for_v<Index, mixed_return_predicate<Index>> &&
    !pop_if_available_for_v<Index, lvalue_ref_predicate<Index>> &&
    !pop_if_available_for_v<Index, lvalue_bool_rvalue_proxy_predicate<Index>>;

template<class Index>
inline constexpr bool consume_value_category_contract_v =
    consume_available_for_v<Index, overloaded_void_consume_callback<Index>> &&
    consume_available_for_v<Index, overloaded_bool_consume_callback<Index>> &&
    consume_available_for_v<Index, rvalue_only_void_consume_callback<Index>> &&
    consume_available_for_v<Index, rvalue_only_bool_consume_callback<Index>> &&
    consume_available_for_v<Index, mixed_return_consume_callback<Index>> &&
    consume_available_for_v<Index, mixed_void_bool_consume_callback<Index>> &&
    !consume_available_for_v<Index, lvalue_ref_void_consume_callback<Index>> &&
    !consume_available_for_v<Index, lvalue_ref_bool_consume_callback<Index>> &&
    !consume_available_for_v<Index,
                             lvalue_bool_rvalue_int_consume_callback<Index>>;

static_assert(predicate_value_category_contract_v<std::uint32_t>);
static_assert(consume_value_category_contract_v<std::uint32_t>);
#if MPMC_TEST_HAS_16_BYTE_CELL
static_assert(predicate_value_category_contract_v<std::uint64_t>);
static_assert(consume_value_category_contract_v<std::uint64_t>);
#endif

// The runtime counterparts check that the selected overload is the rvalue one:
// each lvalue overload above throws if it is ever reached, and each rvalue
// overload records its selection.
template<class Index> void pop_if_uses_prvalue_index() {
    contract_queue<Index> queue;
    contract_value value = 0;

    using predicate = overloaded_predicate<Index>;
    predicate overloaded;
    predicate::selected = 0;
    REQUIRE(queue.try_push(10));
    CHECK(queue.try_pop_if(overloaded, value));
    CHECK_EQ(value, contract_value{10});
    CHECK_EQ(predicate::selected, 1);

    Index sequence = 999;
    predicate::selected = 0;
    REQUIRE(queue.try_push(11));
    CHECK(queue.try_pop_if(overloaded, value, sequence));
    CHECK_EQ(value, contract_value{11});
    CHECK_EQ(static_cast<std::uint64_t>(sequence), std::uint64_t{1});
    CHECK_EQ(predicate::selected, 1);

    rvalue_only_predicate<Index> rvalue_only;
    REQUIRE(queue.try_push(12));
    CHECK(queue.try_pop_if(rvalue_only, value));
    CHECK_EQ(value, contract_value{12});

    using mixed = mixed_return_predicate<Index>;
    mixed mixed_predicate;
    mixed::selected = 0;
    REQUIRE(queue.try_push(13));
    CHECK(queue.try_pop_if(mixed_predicate, value));
    CHECK_EQ(value, contract_value{13});
    CHECK_EQ(mixed::selected, 1);
    CHECK(queue.empty());
}

template<class Index> void consume_uses_prvalue_index() {
    contract_queue<Index> queue;

    using void_callback = overloaded_void_consume_callback<Index>;
    void_callback void_cb;
    void_callback::selected = 0;
    REQUIRE(queue.try_push(1));
    REQUIRE(queue.try_push(2));
    CHECK_EQ(queue.consume(void_cb), std::size_t{2});
    CHECK_EQ(void_callback::selected, 1);
    CHECK(queue.empty());

    using bool_callback = overloaded_bool_consume_callback<Index>;
    bool_callback bool_cb;
    bool_callback::selected = 0;
    REQUIRE(queue.try_push(3));
    REQUIRE(queue.try_push(4));
    CHECK_EQ(queue.consume(bool_cb), std::size_t{1});
    CHECK_EQ(bool_callback::selected, 1);
    CHECK_EQ(queue.size(), std::size_t{1});

    rvalue_only_void_consume_callback<Index> rvalue_only;
    CHECK_EQ(queue.consume(rvalue_only), std::size_t{1});
    CHECK(queue.empty());

    using mixed = mixed_return_consume_callback<Index>;
    mixed mixed_cb;
    mixed::selected = 0;
    REQUIRE(queue.try_push(5));
    REQUIRE(queue.try_push(6));
    CHECK_EQ(queue.consume(mixed_cb), std::size_t{2});
    CHECK_EQ(mixed::selected, 1);
    CHECK(queue.empty());

    using mixed_void = mixed_void_bool_consume_callback<Index>;
    mixed_void mixed_void_cb;
    mixed_void::selected = 0;
    REQUIRE(queue.try_push(7));
    REQUIRE(queue.try_push(8));
    CHECK_EQ(queue.consume(mixed_void_cb), std::size_t{2});
    CHECK_EQ(mixed_void::selected, 1);
    CHECK(queue.empty());

    rvalue_only_bool_consume_callback<Index> rvalue_only_bool;
    REQUIRE(queue.try_push(9));
    CHECK_EQ(queue.consume(rvalue_only_bool), std::size_t{1});
    CHECK(queue.empty());
}

} // namespace

MPMC_TEST(static_fifo) {
    using queue_type = mpmc::mpmc_queue<cell_value, 8, cell_index>;
    queue_type queue;

    static_assert(queue_type::static_capacity == 8);
    static_assert(queue_type::entry_size ==
                  sizeof(cell_value) + sizeof(cell_index));
    static_assert(queue_type::payload_size == sizeof(cell_value));
    static_assert(queue_type::is_always_lock_free);

    CHECK(queue.empty());
    CHECK_EQ(queue.capacity(), std::size_t{8});
    CHECK_EQ(queue.size(), std::size_t{0});

    for (cell_value i = 0; i < 8; ++i) {
        cell_value sequence = 1000;
        REQUIRE(queue.try_push(i + 10, sequence));
        CHECK_EQ(sequence, i);
    }

    CHECK(!queue.empty());
    CHECK_EQ(queue.size(), std::size_t{8});
    CHECK(!queue.try_push(cell_value{99}));

    for (cell_value i = 0; i < 8; ++i) {
        cell_value value = 0;
        cell_value sequence = 1000;
        REQUIRE(queue.try_pop(value, sequence));
        CHECK_EQ(value, i + 10);
        CHECK_EQ(sequence, i);
    }

    CHECK(queue.empty());
    cell_value value = 0;
    CHECK(!queue.try_pop(value));
    CHECK(!queue.peek(value));
    CHECK(queue.try_push(cell_value{42}));
    CHECK(queue.peek(value));
    CHECK_EQ(value, cell_value{42});
    CHECK_EQ(queue.size(), std::size_t{1});
    CHECK(queue.try_pop(value));
    CHECK_EQ(value, cell_value{42});
}

MPMC_TEST(runtime_fifo) {
    std::error_code error;
    auto queue_owner =
        mpmc::mpmc_queue<cell_value, 0, cell_index>::create(4, error);
    REQUIRE(!error);
    REQUIRE(queue_owner);
    auto& queue = *queue_owner;
    CHECK_EQ(queue.capacity(), std::size_t{4});

    cell_value sequence = 0;
    REQUIRE(queue.try_push(1, sequence));
    CHECK_EQ(sequence, cell_value{0});
    REQUIRE(queue.try_push(2, sequence));
    REQUIRE(queue.try_push(3, sequence));
    REQUIRE(queue.try_push(4, sequence));
    CHECK(!queue.try_push(5));

    cell_value value = 0;
    REQUIRE(queue.try_pop(value, sequence));
    CHECK_EQ(value, cell_value{1});
    CHECK_EQ(sequence, cell_value{0});
    REQUIRE(queue.try_pop(value, sequence));
    CHECK_EQ(value, cell_value{2});
    REQUIRE(queue.try_pop(value, sequence));
    CHECK_EQ(value, cell_value{3});
    REQUIRE(queue.try_push(5, sequence));
    CHECK_EQ(sequence, cell_value{4});
    REQUIRE(queue.try_pop(value, sequence));
    CHECK_EQ(value, cell_value{4});
    REQUIRE(queue.try_pop(value, sequence));
    CHECK_EQ(value, cell_value{5});
    CHECK(queue.empty());
}

MPMC_TEST(optional_pop) {
    mpmc::mpmc_queue<cell_value, 2, cell_index> queue;
    CHECK(!queue.try_pop().has_value());
    CHECK(queue.try_push(7));
    const std::optional<cell_value> value = queue.try_pop();
    REQUIRE(value.has_value());
    CHECK_EQ(*value, cell_value{7});
    CHECK(!queue.try_pop().has_value());
}

MPMC_TEST(value_pop_uses_nothrow_construction) {
    using forwarding_queue =
        mpmc::mpmc_queue<throwing_forward_payload, 4, std::uint32_t>;
    static_assert(noexcept(std::declval<forwarding_queue&>().try_pop()));
    forwarding_queue forwarding;
    throwing_forward_payload item;
    item.value = 41;
    REQUIRE(forwarding.try_push(item));
    const std::optional<throwing_forward_payload> moved = forwarding.try_pop();
    REQUIRE(moved.has_value());
    CHECK_EQ(moved->value, std::uint32_t{41});
    CHECK(!forwarding.try_pop().has_value());

    using copy_queue =
        mpmc::mpmc_queue<copy_only_forwarding_payload, 4, std::uint32_t>;
    static_assert(noexcept(std::declval<copy_queue&>().try_pop()));
    copy_queue copying;
    copy_only_forwarding_payload source;
    source.value = 42;
    REQUIRE(copying.try_push(source));
    const std::optional<copy_only_forwarding_payload> copied =
        copying.try_pop();
    REQUIRE(copied.has_value());
    CHECK_EQ(copied->value, std::uint32_t{42});
    CHECK(!copying.try_pop().has_value());

    using move_queue = mpmc::mpmc_queue<move_only_payload, 4, std::uint32_t>;
    static_assert(noexcept(std::declval<move_queue&>().try_pop()));
    move_queue moving;
    move_only_payload move_source;
    move_source.value = 43;
    REQUIRE(moving.try_push(move_source));
    const std::optional<move_only_payload> moved_value = moving.try_pop();
    REQUIRE(moved_value.has_value());
    CHECK_EQ(moved_value->value, std::uint32_t{43});
    CHECK(!moving.try_pop().has_value());
}

MPMC_TEST(reference_pop_for_immovable_payload) {
    using queue_type = mpmc::mpmc_queue<immovable_payload, 4, std::uint32_t>;
    static_assert(!value_pop_available_v<immovable_payload>);
    static_assert(reference_pop_available_v<immovable_payload>);

    queue_type queue;
    immovable_payload item;
    item.value = 7;
    REQUIRE(queue.try_push(item));
    immovable_payload result;
    REQUIRE(queue.try_pop(result));
    CHECK_EQ(result.value, std::uint32_t{7});
    CHECK(!queue.try_pop(result));
}

MPMC_TEST(predicate_and_callback_results_are_exact) {
    mpmc::mpmc_queue<cell_value, 4, cell_index> queue;
    REQUIRE(queue.try_push(11));

    cell_value value = 0;
    bool_predicate predicate;
    CHECK(queue.try_pop_if(predicate, value));
    CHECK_EQ(value, cell_value{11});

    REQUIRE(queue.try_push(12));
    void_consume_callback void_callback;
    CHECK_EQ(queue.consume(void_callback), std::size_t{1});
    CHECK(queue.empty());

    REQUIRE(queue.try_push(13));
    bool_consume_callback stop_callback;
    CHECK_EQ(queue.consume(stop_callback), std::size_t{1});
    CHECK(queue.empty());
}

MPMC_TEST(predicate_and_callback_use_prvalue_index) {
    pop_if_uses_prvalue_index<std::uint32_t>();
    consume_uses_prvalue_index<std::uint32_t>();
#if MPMC_TEST_HAS_16_BYTE_CELL
    pop_if_uses_prvalue_index<std::uint64_t>();
    consume_uses_prvalue_index<std::uint64_t>();
#endif
}

MPMC_TEST(pop_if_head_only) {
    mpmc::mpmc_queue<cell_value, 8, cell_index> queue;
    CHECK(queue.try_push(1));
    CHECK(queue.try_push(2));
    CHECK(queue.try_push(3));

    cell_value value = 0;
    auto is_two = [](const cell_value& candidate, cell_value) noexcept {
        return candidate == 2;
    };

    CHECK(!queue.try_pop_if(is_two, value));
    CHECK_EQ(queue.size(), std::size_t{3});

    REQUIRE(queue.try_pop(value));
    CHECK_EQ(value, cell_value{1});
    REQUIRE(queue.try_pop_if(is_two, value));
    CHECK_EQ(value, cell_value{2});
    REQUIRE(queue.try_pop(value));
    CHECK_EQ(value, cell_value{3});
}

MPMC_TEST(replace_exact) {
    mpmc::mpmc_queue<cell_value, 8, cell_index> queue;
    cell_value sequence = 0;
    REQUIRE(queue.try_push(10, sequence));

    CHECK(!queue.try_replace(sequence, cell_value{11}, cell_value{42}));
    CHECK(queue.try_replace(sequence, cell_value{10}, cell_value{42}));

    cell_value value = 0;
    REQUIRE(queue.try_pop(value, sequence));
    CHECK_EQ(value, cell_value{42});
    CHECK(!queue.try_replace(sequence, cell_value{42}, cell_value{7}));
    CHECK(!queue.try_replace(sequence, cell_value{7}));
}

MPMC_TEST(replace_without_expected_value) {
    mpmc::mpmc_queue<cell_value, 8, cell_index> queue;
    cell_value sequence = 0;
    REQUIRE(queue.try_push(1, sequence));
    CHECK(queue.try_replace(sequence, cell_value{9}));
    cell_value value = 0;
    REQUIRE(queue.try_pop(value));
    CHECK_EQ(value, cell_value{9});

    CHECK(queue.try_push(2, sequence));
    CHECK(queue.try_replace_last(cell_value{8}, &sequence));
    REQUIRE(queue.try_pop(value));
    CHECK_EQ(value, cell_value{8});
    CHECK(!queue.try_replace_last(cell_value{7}));
}

MPMC_TEST(push_or_replace) {
    mpmc::mpmc_queue<cell_value, 8, cell_index> queue;
    cell_value sequence = 0;
    CHECK(queue.try_push_or_replace(1, &sequence));
    CHECK_EQ(sequence, cell_value{0});
    CHECK_EQ(queue.size(), std::size_t{1});

    CHECK(queue.try_push_or_replace(2, &sequence));
    CHECK_EQ(sequence, cell_value{0});
    CHECK_EQ(queue.size(), std::size_t{1});

    cell_value value = 0;
    REQUIRE(queue.try_pop(value));
    CHECK_EQ(value, cell_value{2});

    CHECK(queue.try_push_or_replace(3, &sequence));
    CHECK_EQ(sequence, cell_value{1});
    REQUIRE(queue.try_pop(value));
    CHECK_EQ(value, cell_value{3});
}

MPMC_TEST(push_keep_n) {
    mpmc::mpmc_queue<cell_value, 4, cell_index> queue;
    for (cell_value i = 1; i <= 4; ++i) {
        REQUIRE(queue.try_push(i));
    }
    REQUIRE(queue.push_keep_n(5));

    cell_value value = 0;
    for (cell_value expected = 2; expected <= 5; ++expected) {
        REQUIRE(queue.try_pop(value));
        CHECK_EQ(value, expected);
    }
    CHECK(queue.empty());
}

MPMC_TEST(consume_and_stop) {
    mpmc::mpmc_queue<cell_value, 8, cell_index> queue;
    for (cell_value i = 1; i <= 5; ++i) {
        REQUIRE(queue.try_push(i));
    }

    cell_value sum = 0;
    const std::size_t consumed = queue.consume(
        [&sum](const cell_value& value, cell_value) noexcept { sum += value; });
    CHECK_EQ(consumed, std::size_t{5});
    CHECK_EQ(sum, cell_value{15});
    CHECK(queue.empty());

    for (cell_value i = 1; i <= 5; ++i) {
        REQUIRE(queue.try_push(i));
    }
    const std::size_t partial =
        queue.consume([](const cell_value& value, cell_value) noexcept {
            return value == 3;
        });
    CHECK_EQ(partial, std::size_t{3});
    CHECK_EQ(queue.size(), std::size_t{2});
}

MPMC_TEST(clear_resets_queue) {
    mpmc::mpmc_queue<cell_value, 4, cell_index> queue;
    for (cell_value i = 0; i < 4; ++i) {
        REQUIRE(queue.try_push(i));
    }
    queue.clear();
    CHECK(queue.empty());
    CHECK_EQ(queue.size(), std::size_t{0});

    cell_value sequence = 99;
    REQUIRE(queue.try_push(123, sequence));
    CHECK_EQ(sequence, cell_value{0});
    cell_value value = 0;
    REQUIRE(queue.try_pop(value, sequence));
    CHECK_EQ(value, cell_value{123});
    CHECK_EQ(sequence, cell_value{0});
}

MPMC_TEST(clear_invalidates_sequence_tokens) {
    mpmc::mpmc_queue<cell_value, 4, cell_index> queue;
    cell_value old_sequence = 123;
    REQUIRE(queue.try_push(1, old_sequence));
    CHECK_EQ(old_sequence, cell_value{0});

    queue.clear();
    cell_value new_sequence = 456;
    REQUIRE(queue.try_push(2, new_sequence));
    // clear() starts a new generation at position zero, so a token from the
    // old generation can name an unrelated new entry.
    CHECK_EQ(new_sequence, old_sequence);
    CHECK(queue.try_replace(old_sequence, cell_value{9}));

    cell_value value = 0;
    REQUIRE(queue.try_pop(value));
    CHECK_EQ(value, cell_value{9});
}

MPMC_TEST(no_default_constructor_payload) {
    mpmc::mpmc_queue<no_default, 4, std::uint32_t> queue;
    static_assert(sizeof(no_default) == 4);
    static_assert(queue.entry_size == 8);

    CHECK(queue.try_push(no_default{11}));
    CHECK(queue.try_push(no_default{12}));

    no_default value{0};
    REQUIRE(queue.try_pop(value));
    CHECK_EQ(value.value, std::uint32_t{11});
    REQUIRE(queue.try_pop(value));
    CHECK_EQ(value.value, std::uint32_t{12});
    CHECK(!queue.try_pop(value));
}

#if MPMC_TEST_HAS_16_BYTE_CELL
MPMC_TEST(twelve_byte_payload_with_32_bit_index) {
    mpmc::mpmc_queue<twelve_byte_payload, 4, std::uint32_t> queue;
    static_assert(queue.entry_size == 16);
    static_assert(queue.payload_size == 12);

    CHECK(queue.try_push(twelve_byte_payload{1, 2, 3}));
    twelve_byte_payload value{0, 0, 0};
    REQUIRE(queue.try_pop(value));
    CHECK_EQ(value.a, std::uint32_t{1});
    CHECK_EQ(value.b, std::uint32_t{2});
    CHECK_EQ(value.c, std::uint32_t{3});
}
#endif

MPMC_TEST(capacity_validation) {
    std::error_code error;
    for (std::size_t capacity :
         {std::size_t{0}, std::size_t{3}, std::size_t{6}}) {
        CHECK((!mpmc::mpmc_queue<cell_value, 0, cell_index>::create(capacity,
                                                                    error)));
        CHECK(error == mpmc::errc::invalid_capacity);
    }

    mpmc::mpmc_queue<cell_value, 4, cell_index> queue;
    CHECK_EQ(queue.capacity(), std::size_t{4});

    auto single_owner =
        mpmc::mpmc_queue<cell_value, 0, cell_index>::create(1, error);
    REQUIRE(!error);
    REQUIRE(single_owner);
    auto& single = *single_owner;
    CHECK_EQ(single.capacity(), std::size_t{1});
    CHECK(single.try_push(5));
    cell_value value = 0;
    CHECK(single.try_pop(value));
    CHECK_EQ(value, cell_value{5});
}
