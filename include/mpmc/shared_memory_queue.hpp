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
// POSIX shared-memory adapter for the MPMC queue.
//
// The queue algorithm lives in queue_engine and stores no pointers.  This
// header places the two indices and the slot array into a named POSIX shared
// memory object, preceded by a small, versioned header.  Any process that can
// open the name can attach to the same queue.
//
// The payload type must be trivially copyable and must not contain pointers or
// other process-local state: every byte stored in the queue is interpreted by
// every participating process.  This adapter never throws; setup failures are
// reported through std::error_code.
//
// Bootstrap trust model
// ---------------------
// Creation and attachment are serialized by an advisory lock file.  Every
// process that opens the same queue name must run as the same effective user
// and must share both the POSIX shared-memory namespace (shm_open) and the
// lock-file namespace (/tmp; see detail::shared_memory_lock_path).  A private
// /tmp or a different mount namespace breaks bootstrap synchronization, so
// such processes must not share a queue name.
//
// The lock file is created with mode 0600.  It is accepted only while it is a
// regular file owned by the effective user with no group or other
// permissions.  This guards against accidental collisions and unprivileged
// other users; it is not a security boundary.  A malicious process running as
// the same user, or any process that already has the mapping open, can modify
// or truncate the shared memory while the queue is in use.  All participants
// are assumed to be trusted, cooperating processes.

#pragma once

#include "mpmc/error.hpp"
#include "mpmc/mpmc_queue.hpp"

#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <utility>

#if !defined(_WIN32)
#include <cerrno>
#include <cstdio>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace mpmc {

/// Lifetime policy for the process that creates a named queue.  A persistent
/// name survives the creator (the caller must remove it explicitly); an
/// auto-unlinking creator removes the name when its queue object is destroyed.
/// Already-attached mappings stay valid in either case.
enum class shared_memory_lifetime { persistent, unlink_on_destroy };

/// Opt-in payload safety trait for shared memory.  Arithmetic and enum types
/// are accepted by default.  A class payload must specialize this trait (and
/// must be trivially copyable, standard-layout, and free of process-local
/// pointers or handles).  This conservative policy is required because C++20
/// cannot introspect arbitrary aggregate members, so silently accepting every
/// trivially-copyable class would permit a `struct { char* p; }`.
template<class T>
struct is_shared_memory_safe
    : std::bool_constant<std::is_arithmetic_v<T> || std::is_enum_v<T>> {};

template<class T, std::size_t N>
struct is_shared_memory_safe<std::array<T, N>> : is_shared_memory_safe<T> {};

template<class T>
inline constexpr bool is_shared_memory_safe_v = is_shared_memory_safe<T>::value;

/// A payload that is valid for a queue and explicitly approved for placement
/// in shared memory.
template<class T>
concept shared_memory_payload =
    queue_payload<T> && std::is_standard_layout_v<T> &&
    is_shared_memory_safe_v<T>;

#if defined(_WIN32)

/// Named shared-memory queues are implemented with POSIX shm.  POSIX shared
/// memory is unavailable on Windows.
inline constexpr bool shared_memory_available = false;

namespace detail {
template<class> inline constexpr bool dependent_false_v = false;
} // namespace detail

template<class T, class Index = std::uint64_t, bool LazyPush = false,
         bool LazyPop = false, std::uint64_t AbiTag = 0>
    requires queue_payload_for<T, Index> && shared_memory_payload<T>
class shared_memory_queue {
    static_assert(detail::dependent_false_v<T>,
                  "mpmc::shared_memory_queue requires POSIX shared memory");
};

#else

inline constexpr bool shared_memory_available = true;

namespace detail {

inline constexpr std::uint64_t shared_memory_magic = 0x4d504d4351554555ULL;
inline constexpr std::uint32_t shared_memory_version = 2;
inline constexpr std::uint32_t shared_memory_ready_value = 1;
inline constexpr std::uint32_t shared_memory_flag_lazy_push =
    std::uint32_t{1} << std::uint32_t{0};
inline constexpr std::uint32_t shared_memory_flag_lazy_pop =
    std::uint32_t{1} << std::uint32_t{1};

inline constexpr std::uint32_t shared_memory_endian_little = 1;
inline constexpr std::uint32_t shared_memory_endian_big = 2;
inline constexpr std::uint32_t shared_memory_native_endianness =
    std::endian::native == std::endian::little ? shared_memory_endian_little
    : std::endian::native == std::endian::big  ? shared_memory_endian_big
                                               : 0;

inline constexpr std::size_t shared_memory_header_size = 128;
inline constexpr std::size_t shared_memory_write_offset = 128;
inline constexpr std::size_t shared_memory_read_offset = 256;
inline constexpr std::size_t shared_memory_slots_offset = 384;

/// Versioned header at offset zero.  It is written once by the creating
/// process under the bootstrap lock.  The remaining bytes are reserved for
/// compatible future extensions.
struct alignas(slot_stride) shared_memory_header {
    std::uint64_t magic{shared_memory_magic};
    std::uint32_t version{shared_memory_version};
    std::uint32_t cell_bytes{0};
    std::uint32_t value_size{0};
    std::uint32_t value_align{0};
    std::uint32_t index_size{0};
    std::uint32_t index_align{0};
    std::uint32_t flags{0};
    std::uint32_t endianness{shared_memory_native_endianness};
    std::uint64_t capacity{0};
    std::uint64_t slots_offset{shared_memory_slots_offset};
    std::uint64_t mapping_size{0};
    // Plain storage: an attacher that obtains the bootstrap lock
    // after a creator crash must be able to inspect this field without
    // invoking the lifetime or atomicity rules of a std::atomic object that
    // may never have been constructed.
    std::uint32_t ready{0};
    std::uint32_t reserved1{0};
    std::uint64_t abi_tag{0};
    std::uint64_t instance_id{0};
    std::array<std::byte, 40> reserved{};
};

static_assert(sizeof(shared_memory_header) == shared_memory_header_size);
static_assert(alignof(shared_memory_header) == slot_stride);
static_assert(std::is_standard_layout_v<shared_memory_header>);
static_assert(std::is_trivially_copyable_v<shared_memory_header>);
static_assert(offsetof(shared_memory_header, magic) == 0);
static_assert(offsetof(shared_memory_header, version) == 8);
static_assert(offsetof(shared_memory_header, cell_bytes) == 12);
static_assert(offsetof(shared_memory_header, value_size) == 16);
static_assert(offsetof(shared_memory_header, value_align) == 20);
static_assert(offsetof(shared_memory_header, index_size) == 24);
static_assert(offsetof(shared_memory_header, index_align) == 28);
static_assert(offsetof(shared_memory_header, flags) == 32);
static_assert(offsetof(shared_memory_header, endianness) == 36);
static_assert(offsetof(shared_memory_header, capacity) == 40);
static_assert(offsetof(shared_memory_header, slots_offset) == 48);
static_assert(offsetof(shared_memory_header, mapping_size) == 56);
static_assert(offsetof(shared_memory_header, ready) == 64);
static_assert(offsetof(shared_memory_header, abi_tag) == 72);
static_assert(offsetof(shared_memory_header, instance_id) == 80);

inline constexpr std::size_t shared_memory_name_buffer_size = 256;

/// Optional test hook.  It is compiled in only when the build defines
/// MPMC_ENABLE_TEST_HOOKS, which the project test targets do.  The hook runs
/// while the creator holds the bootstrap lock; tests use it to pause a creator
/// after shm_open()/mmap() and before the header becomes ready, so an attacher
/// can be observed waiting on the lock.  Non-test builds compile a no-op.
inline constexpr int shared_memory_hook_initializing = 1;

#if defined(MPMC_ENABLE_TEST_HOOKS)
using shared_memory_test_hook_fn = void (*)(std::string_view, int) noexcept;
inline std::atomic<shared_memory_test_hook_fn> shared_memory_test_hook{nullptr};

inline void invoke_shared_memory_test_hook(std::string_view name,
                                           int stage) noexcept {
    const shared_memory_test_hook_fn hook =
        shared_memory_test_hook.load(std::memory_order_acquire);
    if (hook != nullptr) {
        hook(name, stage);
    }
}
#else
inline void invoke_shared_memory_test_hook(std::string_view, int) noexcept {}
#endif

/// Canonicalize a POSIX shared-memory name into a fixed buffer.  No allocation
/// is performed, so this remains usable with exceptions disabled.
[[nodiscard]] inline std::error_code normalize_shared_memory_name(
    std::string_view name,
    std::array<char, shared_memory_name_buffer_size>& out,
    std::size_t& size) noexcept {
    if (name.empty()) {
        return make_error_code(errc::invalid_name);
    }

#if defined(__APPLE__)
    // macOS limits POSIX shm names, including the leading slash, to 31 bytes.
    constexpr std::size_t max_name_size = 31;
#else
    constexpr std::size_t max_name_size = 255;
#endif

    std::size_t length = 0;
    if (name.front() != '/') {
        out[length++] = '/';
    }
    if (name.size() > shared_memory_name_buffer_size - 1 - length) {
        return make_error_code(errc::name_too_long);
    }
    for (char ch : name) {
        if (ch == '\0') {
            return make_error_code(errc::invalid_name);
        }
        out[length++] = ch;
    }
    out[length] = '\0';

    if (length <= 1 || length > max_name_size) {
        return length > max_name_size ? make_error_code(errc::name_too_long)
                                      : make_error_code(errc::invalid_name);
    }
    for (std::size_t i = 1; i < length; ++i) {
        if (out[i] == '/') {
            return make_error_code(errc::invalid_name);
        }
    }
    size = length;
    return {};
}

[[nodiscard]] inline std::error_code
shared_memory_lock_path(std::string_view canonical_name,
                        std::array<char, 80>& out, std::size_t& size) noexcept {
    const unsigned long long uid = static_cast<unsigned long long>(::geteuid());
    const unsigned long long hash =
        static_cast<unsigned long long>([](std::string_view name) noexcept {
            std::uint64_t value = 14695981039346656037ULL;
            for (char ch : name) {
                value ^=
                    static_cast<std::uint64_t>(static_cast<unsigned char>(ch));
                value *= 1099511628211ULL;
            }
            return value;
        }(canonical_name));
    const int written = std::snprintf(out.data(), out.size(),
                                      "/tmp/mpmc-%llu-%016llx.lock", uid, hash);
    if (written <= 0 || static_cast<std::size_t>(written) >= out.size()) {
        return make_error_code(errc::invalid_name);
    }
    size = static_cast<std::size_t>(written);
    return {};
}

/// Advisory bootstrap lock at /tmp/mpmc-<uid>-<hash>.lock.  It is held only
/// while a creator initializes the mapping or an attacher validates it, never
/// on the queue fast path.  Cooperating participants must resolve the same
/// path; the lock is advisory and does not protect the mapping from writers
/// that run as the same effective user.
class shared_memory_bootstrap_lock {
public:
    shared_memory_bootstrap_lock(std::string_view name, int operation,
                                 std::error_code& error) noexcept {
        error.clear();
        std::array<char, 80> path{};
        std::size_t path_size = 0;
        error = shared_memory_lock_path(name, path, path_size);
        if (error) {
            return;
        }

        unsigned int flags = static_cast<unsigned int>(O_RDWR) |
                             static_cast<unsigned int>(O_CREAT);
#if defined(O_CLOEXEC)
        flags |= static_cast<unsigned int>(O_CLOEXEC);
#endif
#if defined(O_NOFOLLOW)
        flags |= static_cast<unsigned int>(O_NOFOLLOW);
#endif
        fd_ = ::open(path.data(), static_cast<int>(flags), 0600);
        if (fd_ < 0) {
            error = std::error_code(errno, std::system_category());
            return;
        }

        struct stat status{};
        if (::fstat(fd_, &status) != 0) {
            error = std::error_code(errno, std::system_category());
            close_fd();
            return;
        }
        const mode_t group_other_permissions =
            static_cast<mode_t>(S_IRWXG) | static_cast<mode_t>(S_IRWXO);
        if (status.st_uid != ::geteuid() || !S_ISREG(status.st_mode) ||
            (status.st_mode & group_other_permissions) != 0) {
            error = make_error_code(std::errc::permission_denied);
            close_fd();
            return;
        }

        for (;;) {
            if (::flock(fd_, operation) == 0) {
                break;
            }
            if (errno == EINTR) {
                continue;
            }
            error = std::error_code(errno, std::system_category());
            close_fd();
            return;
        }
        (void)path_size;
    }

    shared_memory_bootstrap_lock(const shared_memory_bootstrap_lock&) = delete;
    shared_memory_bootstrap_lock&
    operator=(const shared_memory_bootstrap_lock&) = delete;
    shared_memory_bootstrap_lock(shared_memory_bootstrap_lock&&) = delete;
    shared_memory_bootstrap_lock&
    operator=(shared_memory_bootstrap_lock&&) = delete;

    ~shared_memory_bootstrap_lock() {
        if (fd_ >= 0) {
            (void)::flock(fd_, LOCK_UN);
            ::close(fd_);
        }
    }

    [[nodiscard]] bool valid() const noexcept {
        return fd_ >= 0;
    }

private:
    void close_fd() noexcept {
        ::close(fd_);
        fd_ = -1;
    }

    int fd_ = -1;
};

[[nodiscard]] inline void* offset_pointer(void* base,
                                          std::size_t offset) noexcept {
    return static_cast<std::byte*>(base) + offset;
}

[[nodiscard]] inline std::mutex& shared_memory_bootstrap_mutex() noexcept {
    static std::mutex mutex;
    return mutex;
}

/// Nonzero instance identifier for a newly created object.  OS randomness is
/// mixed with the clock, PID and a per-process counter.  This is not a
/// mathematical uniqueness guarantee, but a collision between live queues is
/// negligible.  Zero is never returned: attach rejects zero explicitly so an
/// interrupted or stale header cannot be mistaken for a valid instance.
[[nodiscard]] inline std::uint64_t make_shared_memory_instance_id() noexcept {
    static std::atomic<std::uint64_t> counter{0};

    std::uint64_t random = 0;
    bool have_random = false;
    unsigned int flags = static_cast<unsigned int>(O_RDONLY);
#if defined(O_CLOEXEC)
    flags |= static_cast<unsigned int>(O_CLOEXEC);
#endif
    const int fd = ::open("/dev/urandom", static_cast<int>(flags));
    if (fd >= 0) {
        std::size_t done = 0;
        while (done < sizeof random) {
            // No application lock is held here; the analyzer conflates the
            // process-local atomic counter with a blocking critical section.
            const ssize_t got =
                // NOLINTNEXTLINE(clang-analyzer-unix.BlockInCriticalSection)
                ::read(fd, reinterpret_cast<char*>(&random) + done,
                       sizeof random - done);
            if (got > 0) {
                done += static_cast<std::size_t>(got);
                continue;
            }
            if (got < 0 && errno == EINTR) {
                continue;
            }
            break;
        }
        ::close(fd);
        have_random = done == sizeof random;
    }

    const auto now = static_cast<std::uint64_t>(
        std::chrono::system_clock::now().time_since_epoch().count());
    const std::uint64_t pid = static_cast<std::uint64_t>(::getpid());
    const std::uint64_t count = counter.fetch_add(1, std::memory_order_relaxed);
    std::uint64_t id =
        random ^ now ^ (pid << 32U) ^ (count * 0x9e3779b97f4a7c15ULL);
    if (!have_random) {
        // The clock/PID/counter mix is the only fallback when /dev/urandom is
        // unavailable.  It is still forced nonzero below.
        id ^= 0x6a09e667f3bcc909ULL;
    }
    if (id == 0) {
        id = 0x9e3779b97f4a7c15ULL;
    }
    return id;
}

[[nodiscard]] inline bool
shared_memory_mapping_size(std::size_t capacity, std::size_t& size) noexcept {
    constexpr std::size_t max_size = std::numeric_limits<std::size_t>::max();
    if (capacity > (max_size - shared_memory_slots_offset) / slot_stride) {
        return false;
    }
    size = shared_memory_slots_offset + capacity * slot_stride;
    return true;
}

[[nodiscard]] inline std::error_code
shared_memory_system_error(int error) noexcept {
    return std::error_code(error, std::system_category());
}

} // namespace detail

/// A named, fixed-capacity queue placed in POSIX shared memory.
///
/// Construct with `create()` or `attach()`; both return a `std::unique_ptr`
/// and report every failure through `std::error_code`.  The object is
/// move-only and its operations are the same as `mpmc_queue`.
template<class T, class Index = std::uint64_t, bool LazyPush = false,
         bool LazyPop = false, std::uint64_t AbiTag = 0>
    requires queue_payload_for<T, Index> && shared_memory_payload<T>
class shared_memory_queue
    : private detail::queue_engine<T, Index, LazyPush, LazyPop> {
public:
    using value_type = T;
    using index_type = Index;
    using engine_type = detail::queue_engine<T, Index, LazyPush, LazyPop>;
    using slot_type = typename engine_type::slot_type;

    static constexpr std::uint64_t abi_tag = AbiTag;
    static constexpr std::size_t entry_size = engine_type::cell_bytes;
    static constexpr std::size_t payload_size = engine_type::payload_bytes;
    static constexpr bool is_always_lock_free =
        engine_type::is_always_lock_free;
    static constexpr bool lazy_push = LazyPush;
    static constexpr bool lazy_pop = LazyPop;
    static constexpr index_type marker_horizon = engine_type::marker_horizon;

    static_assert(std::atomic<index_type>::is_always_lock_free,
                  "the queue index atomic must be lock-free for shared memory");
    static_assert(detail::shared_memory_native_endianness != 0,
                  "shared_memory_queue does not support mixed-endian targets");

    [[nodiscard]] static std::unique_ptr<shared_memory_queue>
    create(std::string_view name, std::size_t capacity,
           shared_memory_lifetime lifetime, std::error_code& error) noexcept {
        error.clear();
        std::array<char, detail::shared_memory_name_buffer_size> canonical{};
        std::size_t canonical_size = 0;
        error = detail::normalize_shared_memory_name(name, canonical,
                                                     canonical_size);
        if (error) {
            return nullptr;
        }

        auto* queue = new (std::nothrow) shared_memory_queue(uninitialized_t{});
        if (queue == nullptr) {
            error = make_error_code(errc::allocation_failed);
            return nullptr;
        }
        queue->name_ = canonical;
        queue->name_size_ = canonical_size;
        queue->lifetime_ = lifetime;
        error = queue->create_mapping(capacity);
        if (error) {
            delete queue;
            return nullptr;
        }
        return std::unique_ptr<shared_memory_queue>{queue};
    }

    [[nodiscard]] static std::unique_ptr<shared_memory_queue>
    create(std::string_view name, std::size_t capacity,
           std::error_code& error) noexcept {
        return create(name, capacity, shared_memory_lifetime::persistent,
                      error);
    }

#if MPMC_HAS_EXPECTED
    [[nodiscard]] static expected<std::unique_ptr<shared_memory_queue>,
                                  std::error_code>
    create_expected(std::string_view name, std::size_t capacity) noexcept {
        std::error_code error;
        auto queue = create(name, capacity, error);
        if (!queue) {
            return std::unexpected(error);
        }
        return expected<std::unique_ptr<shared_memory_queue>, std::error_code>{
            std::move(queue)};
    }

    [[nodiscard]] static expected<std::unique_ptr<shared_memory_queue>,
                                  std::error_code>
    create_expected(std::string_view name, std::size_t capacity,
                    shared_memory_lifetime lifetime) noexcept {
        std::error_code error;
        auto queue = create(name, capacity, lifetime, error);
        if (!queue) {
            return std::unexpected(error);
        }
        return expected<std::unique_ptr<shared_memory_queue>, std::error_code>{
            std::move(queue)};
    }

    [[nodiscard]] static expected<std::unique_ptr<shared_memory_queue>,
                                  std::error_code>
    attach_expected(std::string_view name) noexcept {
        std::error_code error;
        auto queue = attach(name, error);
        if (!queue) {
            return std::unexpected(error);
        }
        return expected<std::unique_ptr<shared_memory_queue>, std::error_code>{
            std::move(queue)};
    }
#endif

    [[nodiscard]] static std::unique_ptr<shared_memory_queue>
    attach(std::string_view name, std::error_code& error) noexcept {
        error.clear();
        std::array<char, detail::shared_memory_name_buffer_size> canonical{};
        std::size_t canonical_size = 0;
        error = detail::normalize_shared_memory_name(name, canonical,
                                                     canonical_size);
        if (error) {
            return nullptr;
        }

        auto* queue = new (std::nothrow) shared_memory_queue(uninitialized_t{});
        if (queue == nullptr) {
            error = make_error_code(errc::allocation_failed);
            return nullptr;
        }
        queue->name_ = canonical;
        queue->name_size_ = canonical_size;
        error = queue->open_mapping();
        if (error) {
            delete queue;
            return nullptr;
        }
        return std::unique_ptr<shared_memory_queue>{queue};
    }

    /// Remove a shared-memory name.  Returns false when the name does not
    /// exist.  Processes that already attached keep their mapping; future
    /// attaches to the name fail.
    [[nodiscard]] static bool remove(std::string_view name,
                                     std::error_code& error) noexcept {
        error.clear();
        std::array<char, detail::shared_memory_name_buffer_size> canonical{};
        std::size_t canonical_size = 0;
        error = detail::normalize_shared_memory_name(name, canonical,
                                                     canonical_size);
        if (error) {
            return false;
        }
        const std::string_view canonical_view{canonical.data(), canonical_size};

        std::lock_guard<std::mutex> local{
            detail::shared_memory_bootstrap_mutex()};
        detail::shared_memory_bootstrap_lock bootstrap{canonical_view, LOCK_EX,
                                                       error};
        if (error) {
            return false;
        }

        if (::shm_unlink(canonical.data()) == 0) {
            return true;
        }
        if (errno == ENOENT) {
            return false;
        }
        error = detail::shared_memory_system_error(errno);
        return false;
    }

    /// Test whether a shared-memory name currently exists.
    [[nodiscard]] static bool exists(std::string_view name,
                                     std::error_code& error) noexcept {
        error.clear();
        std::array<char, detail::shared_memory_name_buffer_size> canonical{};
        std::size_t canonical_size = 0;
        error = detail::normalize_shared_memory_name(name, canonical,
                                                     canonical_size);
        if (error) {
            return false;
        }
        const std::string_view canonical_view{canonical.data(), canonical_size};

        std::lock_guard<std::mutex> local{
            detail::shared_memory_bootstrap_mutex()};
        detail::shared_memory_bootstrap_lock bootstrap{canonical_view, LOCK_SH,
                                                       error};
        if (error) {
            return false;
        }

        const int fd = ::shm_open(canonical.data(), O_RDONLY, 0);
        if (fd >= 0) {
            ::close(fd);
            return true;
        }
        if (errno == ENOENT) {
            return false;
        }
        error = detail::shared_memory_system_error(errno);
        return false;
    }

#if MPMC_HAS_EXPECTED
    [[nodiscard]] static expected<bool, std::error_code>
    remove_expected(std::string_view name) noexcept {
        std::error_code error;
        const bool removed = remove(name, error);
        if (error) {
            return std::unexpected(error);
        }
        return removed;
    }

    [[nodiscard]] static expected<bool, std::error_code>
    exists_expected(std::string_view name) noexcept {
        std::error_code error;
        const bool present = exists(name, error);
        if (error) {
            return std::unexpected(error);
        }
        return present;
    }
#endif

    shared_memory_queue(const shared_memory_queue&) = delete;
    shared_memory_queue& operator=(const shared_memory_queue&) = delete;

    shared_memory_queue(shared_memory_queue&& other) noexcept
        : engine_type(), mapping_(other.mapping_),
          mapping_size_(other.mapping_size_), fd_(other.fd_),
          name_(other.name_), name_size_(other.name_size_),
          owner_(other.owner_), lifetime_(other.lifetime_),
          instance_id_(other.instance_id_), creator_pid_(other.creator_pid_) {
        this->reset(other.current_state());
        other.detach();
    }

    shared_memory_queue& operator=(shared_memory_queue&& other) noexcept {
        if (this != std::addressof(other)) {
            release();
            this->reset(other.current_state());
            mapping_ = other.mapping_;
            mapping_size_ = other.mapping_size_;
            fd_ = other.fd_;
            name_ = other.name_;
            name_size_ = other.name_size_;
            owner_ = other.owner_;
            lifetime_ = other.lifetime_;
            instance_id_ = other.instance_id_;
            creator_pid_ = other.creator_pid_;
            other.detach();
        }
        return *this;
    }

    ~shared_memory_queue() {
        release();
    }

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

    [[nodiscard]] std::string_view name() const noexcept {
        return {name_.data(), name_size_};
    }

    [[nodiscard]] bool valid() const noexcept {
        return mapping_ != nullptr;
    }

    [[nodiscard]] bool is_owner() const noexcept {
        return owner_;
    }

    [[nodiscard]] std::uint64_t instance_id() const noexcept {
        return instance_id_;
    }

    [[nodiscard]] std::size_t mapping_size() const noexcept {
        return mapping_size_;
    }

private:
    struct uninitialized_t {};

    explicit shared_memory_queue(uninitialized_t) noexcept : engine_type() {}

private:
    [[nodiscard]] std::string_view name_view() const noexcept {
        return {name_.data(), name_size_};
    }

    [[nodiscard]] static constexpr std::uint32_t mode_flags() noexcept {
        std::uint32_t flags = 0;
        if constexpr (LazyPush) {
            flags |= detail::shared_memory_flag_lazy_push;
        }
        if constexpr (LazyPop) {
            flags |= detail::shared_memory_flag_lazy_pop;
        }
        return flags;
    }

    [[nodiscard]] std::error_code
    create_mapping(std::size_t capacity) noexcept {
        if (capacity == 0 || !detail::valid_capacity<index_type>(capacity)) {
            return make_error_code(errc::invalid_capacity);
        }
        std::size_t mapping_size = 0;
        if (!detail::shared_memory_mapping_size(capacity, mapping_size)) {
            return make_error_code(errc::invalid_capacity);
        }
        if (mapping_size >
            static_cast<std::size_t>(std::numeric_limits<off_t>::max())) {
            return make_error_code(errc::invalid_capacity);
        }

        std::lock_guard<std::mutex> local{
            detail::shared_memory_bootstrap_mutex()};
        std::error_code lock_error;
        detail::shared_memory_bootstrap_lock bootstrap{name_view(), LOCK_EX,
                                                       lock_error};
        if (!bootstrap.valid()) {
            return lock_error;
        }

        const int fd =
            ::shm_open(name_.data(), O_RDWR | O_CREAT | O_EXCL, 0600);
        if (fd < 0) {
            return errno == EEXIST ? make_error_code(errc::already_exists)
                                   : detail::shared_memory_system_error(errno);
        }

        if (::ftruncate(fd, static_cast<off_t>(mapping_size)) != 0) {
            const auto error = detail::shared_memory_system_error(errno);
            ::close(fd);
            (void)::shm_unlink(name_.data());
            return error;
        }
#if defined(__linux__)
        // Reserve the backing pages before touching the mapping.  This turns
        // common tmpfs quota failures into an error code instead of SIGBUS.
        const int reserve =
            ::posix_fallocate(fd, 0, static_cast<off_t>(mapping_size));
        if (reserve != 0 && reserve != EOPNOTSUPP && reserve != ENOSYS) {
            const auto error = detail::shared_memory_system_error(reserve);
            ::close(fd);
            (void)::shm_unlink(name_.data());
            return error;
        }
#endif

        void* base = ::mmap(nullptr, mapping_size, PROT_READ | PROT_WRITE,
                            MAP_SHARED, fd, 0);
        if (base == MAP_FAILED) {
            const auto error = detail::shared_memory_system_error(errno);
            ::close(fd);
            (void)::shm_unlink(name_.data());
            return error;
        }

        std::memset(base, 0, detail::shared_memory_header_size);
        auto* header = ::new (base) detail::shared_memory_header{};
        header->cell_bytes =
            static_cast<std::uint32_t>(engine_type::cell_bytes);
        header->value_size = static_cast<std::uint32_t>(sizeof(value_type));
        header->value_align = static_cast<std::uint32_t>(alignof(value_type));
        header->index_size = static_cast<std::uint32_t>(sizeof(index_type));
        header->index_align = static_cast<std::uint32_t>(alignof(index_type));
        header->flags = mode_flags();
        header->endianness = detail::shared_memory_native_endianness;
        header->capacity = static_cast<std::uint64_t>(capacity);
        header->slots_offset = detail::shared_memory_slots_offset;
        header->mapping_size = static_cast<std::uint64_t>(mapping_size);
        header->abi_tag = AbiTag;
        header->instance_id = detail::make_shared_memory_instance_id();

        // The header is fully described but ready is still zero.  A test hook
        // may pause here while the bootstrap lock is held.
        detail::invoke_shared_memory_test_hook(
            name_view(), detail::shared_memory_hook_initializing);

        auto* write = ::new (
            detail::offset_pointer(base, detail::shared_memory_write_offset))
            std::atomic<index_type>{};
        auto* read = ::new (detail::offset_pointer(
            base, detail::shared_memory_read_offset)) std::atomic<index_type>{};
        auto* slots = reinterpret_cast<slot_type*>(
            detail::offset_pointer(base, detail::shared_memory_slots_offset));
        for (std::size_t i = 0; i < capacity; ++i) {
            ::new (slots + i) slot_type{};
        }

        this->reset(typename engine_type::state{
            slots, write, read, static_cast<index_type>(capacity),
            static_cast<index_type>(capacity - 1)});
        this->initialize();

        std::atomic_thread_fence(std::memory_order_release);
        header->ready = detail::shared_memory_ready_value;

        mapping_ = base;
        mapping_size_ = mapping_size;
        fd_ = fd;
        owner_ = true;
        instance_id_ = header->instance_id;
        creator_pid_ = static_cast<unsigned long long>(::getpid());
        return {};
    }

    [[nodiscard]] std::error_code open_mapping() noexcept {
        std::lock_guard<std::mutex> local{
            detail::shared_memory_bootstrap_mutex()};
        std::error_code lock_error;
        detail::shared_memory_bootstrap_lock bootstrap{name_view(), LOCK_SH,
                                                       lock_error};
        if (!bootstrap.valid()) {
            return lock_error;
        }

        const int fd = ::shm_open(name_.data(), O_RDWR, 0);
        if (fd < 0) {
            return errno == ENOENT ? make_error_code(errc::not_found)
                                   : detail::shared_memory_system_error(errno);
        }

        struct stat status{};
        if (::fstat(fd, &status) != 0) {
            const auto error = detail::shared_memory_system_error(errno);
            ::close(fd);
            return error;
        }
        if (status.st_size <
            static_cast<off_t>(detail::shared_memory_header_size)) {
            ::close(fd);
            return make_error_code(errc::mapping_too_small);
        }
        if (static_cast<std::uintmax_t>(status.st_size) >
            static_cast<std::uintmax_t>(
                std::numeric_limits<std::size_t>::max())) {
            ::close(fd);
            return make_error_code(errc::mapping_too_small);
        }

        const std::size_t file_size = static_cast<std::size_t>(status.st_size);
        void* base = ::mmap(nullptr, file_size, PROT_READ | PROT_WRITE,
                            MAP_SHARED, fd, 0);
        if (base == MAP_FAILED) {
            const auto error = detail::shared_memory_system_error(errno);
            ::close(fd);
            return error;
        }

        auto* header = static_cast<detail::shared_memory_header*>(base);
        std::atomic_thread_fence(std::memory_order_acquire);
        if (header->ready != detail::shared_memory_ready_value) {
            ::munmap(base, file_size);
            ::close(fd);
            return make_error_code(errc::not_ready);
        }

        const auto validation = validate_header(*header, file_size);
        if (validation) {
            ::munmap(base, file_size);
            ::close(fd);
            return validation;
        }

        auto* write = reinterpret_cast<std::atomic<index_type>*>(
            detail::offset_pointer(base, detail::shared_memory_write_offset));
        auto* read = reinterpret_cast<std::atomic<index_type>*>(
            detail::offset_pointer(base, detail::shared_memory_read_offset));
        auto* slots = reinterpret_cast<slot_type*>(
            detail::offset_pointer(base, detail::shared_memory_slots_offset));

        this->reset(typename engine_type::state{
            slots, write, read, static_cast<index_type>(header->capacity),
            static_cast<index_type>(header->capacity - 1)});

        mapping_ = base;
        mapping_size_ = file_size;
        fd_ = fd;
        owner_ = false;
        instance_id_ = header->instance_id;
        return {};
    }

    [[nodiscard]] static std::error_code
    validate_header(const detail::shared_memory_header& header,
                    std::size_t file_size) noexcept {
        if (header.magic != detail::shared_memory_magic ||
            header.version != detail::shared_memory_version) {
            return make_error_code(errc::incompatible_layout);
        }
        if (header.cell_bytes != engine_type::cell_bytes ||
            header.value_size != sizeof(value_type) ||
            header.value_align != alignof(value_type) ||
            header.index_size != sizeof(index_type) ||
            header.index_align != alignof(index_type) ||
            header.flags != mode_flags() ||
            header.endianness != detail::shared_memory_native_endianness ||
            header.abi_tag != AbiTag || header.instance_id == 0) {
            return make_error_code(errc::incompatible_layout);
        }
        if (header.capacity == 0 ||
            (header.capacity & (header.capacity - 1)) != 0 ||
            header.capacity > static_cast<std::uint64_t>(
                                  std::numeric_limits<index_type>::max() / 4) ||
            header.capacity > static_cast<std::uint64_t>(
                                  std::numeric_limits<std::size_t>::max())) {
            return make_error_code(errc::incompatible_layout);
        }
        if (header.slots_offset != detail::shared_memory_slots_offset) {
            return make_error_code(errc::incompatible_layout);
        }
        if (header.mapping_size > file_size) {
            return make_error_code(errc::mapping_too_small);
        }
        std::size_t expected_size = 0;
        if (!detail::shared_memory_mapping_size(
                static_cast<std::size_t>(header.capacity), expected_size) ||
            header.mapping_size != expected_size) {
            return make_error_code(errc::incompatible_layout);
        }
        if (header.ready != detail::shared_memory_ready_value) {
            return make_error_code(errc::not_ready);
        }
        return {};
    }

    /// Remove the name only if it still refers to the object this creator
    /// opened.  Where the platform exposes a usable shm device/inode, the
    /// retained creator descriptor is compared with the current named
    /// object's backing identity; the header instance id is checked as a
    /// second guard (and is the fallback where no identity is available).
    /// This keeps a late-destroyed creator from unlinking an unrelated
    /// replacement that reused the same name.
    void unlink_owned_name_if_current() noexcept {
        if (fd_ < 0) {
            return;
        }
        struct stat creator_status{};
        if (::fstat(fd_, &creator_status) != 0) {
            return;
        }

        std::error_code error;
        std::lock_guard<std::mutex> local{
            detail::shared_memory_bootstrap_mutex()};
        detail::shared_memory_bootstrap_lock bootstrap{name_view(), LOCK_EX,
                                                       error};
        if (!bootstrap.valid()) {
            return;
        }

        const int named_fd = ::shm_open(name_.data(), O_RDONLY, 0);
        if (named_fd < 0) {
            return;
        }
        struct stat status{};
        if (::fstat(named_fd, &status) != 0 ||
            status.st_size <
                static_cast<off_t>(detail::shared_memory_header_size)) {
            ::close(named_fd);
            return;
        }
        // Some POSIX implementations (notably macOS) report st_dev==0 and
        // st_ino==0 for shm objects.  Compare the backing identity where it is
        // available; otherwise fall back to the random instance id.
        const bool identity_available = creator_status.st_ino != 0;
        if (identity_available && (creator_status.st_dev != status.st_dev ||
                                   creator_status.st_ino != status.st_ino)) {
            ::close(named_fd);
            return;
        }
        void* base = ::mmap(nullptr, detail::shared_memory_header_size,
                            PROT_READ, MAP_SHARED, named_fd, 0);
        if (base == MAP_FAILED) {
            ::close(named_fd);
            return;
        }
        detail::shared_memory_header header{};
        std::memcpy(&header, base, sizeof header);
        ::munmap(base, detail::shared_memory_header_size);
        ::close(named_fd);

        if (header.ready == detail::shared_memory_ready_value &&
            header.magic == detail::shared_memory_magic &&
            header.version == detail::shared_memory_version &&
            header.instance_id == instance_id_ && instance_id_ != 0) {
            (void)::shm_unlink(name_.data());
        }
    }

    void release() noexcept {
        if (mapping_ != nullptr) {
            ::munmap(mapping_, mapping_size_);
            mapping_ = nullptr;
        }
        // The creator descriptor must stay open until the identity comparison;
        // it is closed below.
        if (owner_ && lifetime_ == shared_memory_lifetime::unlink_on_destroy &&
            name_size_ != 0 &&
            creator_pid_ == static_cast<unsigned long long>(::getpid())) {
            unlink_owned_name_if_current();
        }
        if (fd_ >= 0) {
            ::close(fd_);
            fd_ = -1;
        }
        mapping_size_ = 0;
        owner_ = false;
        instance_id_ = 0;
        creator_pid_ = 0;
        this->reset();
    }

    void detach() noexcept {
        mapping_ = nullptr;
        mapping_size_ = 0;
        fd_ = -1;
        owner_ = false;
        lifetime_ = shared_memory_lifetime::persistent;
        instance_id_ = 0;
        creator_pid_ = 0;
        this->reset();
    }

    void* mapping_ = nullptr;
    std::size_t mapping_size_ = 0;
    int fd_ = -1;
    std::array<char, detail::shared_memory_name_buffer_size> name_{};
    std::size_t name_size_ = 0;
    bool owner_ = false;
    shared_memory_lifetime lifetime_ = shared_memory_lifetime::persistent;
    std::uint64_t instance_id_ = 0;
    unsigned long long creator_pid_ = 0;
};

#endif // _WIN32

} // namespace mpmc
