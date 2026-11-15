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
#include "mpmc/mpmc.hpp"

#include "test_cell_config.hpp"
#include "test_framework.hpp"

#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <type_traits>
#include <utility>

#if !defined(_WIN32)
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

// A four-byte custom payload keeps the opt-in test usable with either an
// 8-byte or a 16-byte cell.
struct approved_payload {
    std::uint16_t first;
    std::uint16_t second;
};

namespace mpmc {
template<> struct is_shared_memory_safe<approved_payload> : std::true_type {};
} // namespace mpmc

namespace {

using queue_value = mpmc_test::wide_value;
using queue_index = mpmc_test::wide_index;
using shared_queue = mpmc::shared_memory_queue<queue_value, queue_index>;

#if !defined(_WIN32)

std::string unique_name(const char* tag) {
    static std::atomic<unsigned> counter{0};
    const unsigned id = counter.fetch_add(1, std::memory_order_relaxed);
    return std::string{"/mpmc_"} + tag + '_' +
           std::to_string(static_cast<long long>(::getpid())) + '_' +
           std::to_string(id);
}

void remove_quietly(const std::string& name) {
    std::error_code error;
    (void)shared_queue::remove(name, error);
}

using raw_header = mpmc::detail::shared_memory_header;

[[nodiscard]] std::size_t expected_mapping_size(std::size_t capacity) {
    std::size_t size = 0;
    REQUIRE(mpmc::detail::shared_memory_mapping_size(capacity, size));
    return size;
}

/// Raw POSIX shared-memory object used to build malformed or partially
/// initialized fixtures that bypass shared_memory_queue::create().
class raw_shared_memory {
public:
    raw_shared_memory(std::string name, std::size_t size)
        : name_(std::move(name)), size_(size) {
        fd_ = ::shm_open(name_.c_str(), O_RDWR | O_CREAT | O_EXCL, 0600);
        if (fd_ < 0) {
            return;
        }
        if (size_ == 0) {
            valid_ = true;
            return;
        }
        if (::ftruncate(fd_, static_cast<off_t>(size_)) != 0) {
            cleanup();
            return;
        }
        base_ =
            ::mmap(nullptr, size_, PROT_READ | PROT_WRITE, MAP_SHARED, fd_, 0);
        if (base_ == MAP_FAILED) {
            base_ = nullptr;
            cleanup();
            return;
        }
        std::memset(base_, 0, size_);
        valid_ = true;
    }

    raw_shared_memory(const raw_shared_memory&) = delete;
    raw_shared_memory& operator=(const raw_shared_memory&) = delete;

    ~raw_shared_memory() {
        cleanup();
    }

    [[nodiscard]] bool valid() const noexcept {
        return valid_;
    }

    [[nodiscard]] void* data() const noexcept {
        return base_;
    }

    [[nodiscard]] std::size_t size() const noexcept {
        return size_;
    }

    /// Size observed through fstat.  Some kernels round POSIX shm objects up
    /// to a page, so this can exceed the requested size.
    [[nodiscard]] std::size_t file_size() const noexcept {
        struct stat status{};
        if (fd_ >= 0 && ::fstat(fd_, &status) == 0 && status.st_size >= 0) {
            return static_cast<std::size_t>(status.st_size);
        }
        return size_;
    }

    /// False on platforms that report no usable device/inode for shm objects
    /// (for example macOS reports st_dev==0 and st_ino==0).
    [[nodiscard]] bool has_backing_identity() const noexcept {
        struct stat status{};
        if (fd_ >= 0 && ::fstat(fd_, &status) == 0) {
            return status.st_ino != 0;
        }
        return false;
    }

    [[nodiscard]] raw_header& header() const noexcept {
        return *static_cast<raw_header*>(base_);
    }

    [[nodiscard]] const std::string& name() const noexcept {
        return name_;
    }

private:
    void cleanup() noexcept {
        if (base_ != nullptr) {
            (void)::msync(base_, size_, MS_SYNC);
            ::munmap(base_, size_);
            base_ = nullptr;
        }
        if (fd_ >= 0) {
            ::close(fd_);
            fd_ = -1;
        }
        if (!name_.empty()) {
            (void)::shm_unlink(name_.c_str());
        }
        valid_ = false;
    }

    std::string name_;
    std::size_t size_ = 0;
    int fd_ = -1;
    void* base_ = nullptr;
    bool valid_ = false;
};

/// Fill in every field of a structurally valid header for the test cell.
void initialize_valid_header(raw_shared_memory& mapping,
                             std::size_t capacity = 4) {
    raw_header& header = mapping.header();
    std::memset(static_cast<void*>(&header), 0, sizeof header);
    header.magic = mpmc::detail::shared_memory_magic;
    header.version = mpmc::detail::shared_memory_version;
    header.cell_bytes = sizeof(queue_value) + sizeof(queue_index) <= 8 ? 8 : 16;
    header.value_size = sizeof(queue_value);
    header.value_align = alignof(queue_value);
    header.index_size = sizeof(queue_index);
    header.index_align = alignof(queue_index);
    header.flags = 0;
    header.endianness = mpmc::detail::shared_memory_native_endianness;
    header.capacity = capacity;
    header.slots_offset = mpmc::detail::shared_memory_slots_offset;
    header.mapping_size =
        static_cast<std::uint64_t>(expected_mapping_size(capacity));
    header.ready = mpmc::detail::shared_memory_ready_value;
    header.abi_tag = 0;
    header.instance_id = 0x1122334455667788ULL;
}

void expect_attach_error(const std::string& name, mpmc::errc expected) {
    std::error_code error;
    auto queue = shared_queue::attach(name, error);
    CHECK(!queue);
    CHECK_EQ(error, mpmc::make_error_code(expected));
}

[[nodiscard]] bool lock_path_for(std::string_view name,
                                 std::array<char, 80>& path) {
    std::array<char, mpmc::detail::shared_memory_name_buffer_size> canonical{};
    std::size_t canonical_size = 0;
    std::error_code error = mpmc::detail::normalize_shared_memory_name(
        name, canonical, canonical_size);
    if (error) {
        return false;
    }
    std::size_t path_size = 0;
    error = mpmc::detail::shared_memory_lock_path(
        std::string_view{canonical.data(), canonical_size}, path, path_size);
    return !error;
}

/// Owns a pair of pipe descriptors so a failed REQUIRE cannot leak them.
class pipe_pair {
public:
    pipe_pair() {
        if (::pipe(fds_.data()) != 0) {
            fds_[0] = -1;
            fds_[1] = -1;
        }
    }

    pipe_pair(const pipe_pair&) = delete;
    pipe_pair& operator=(const pipe_pair&) = delete;
    ~pipe_pair() {
        close_all();
    }

    [[nodiscard]] bool valid() const noexcept {
        return fds_[0] >= 0 && fds_[1] >= 0;
    }

    [[nodiscard]] int read_fd() const noexcept {
        return fds_[0];
    }
    [[nodiscard]] int write_fd() const noexcept {
        return fds_[1];
    }

    void close_read() noexcept {
        close_one(0);
    }
    void close_write() noexcept {
        close_one(1);
    }
    void close_all() noexcept {
        close_one(0);
        close_one(1);
    }

private:
    void close_one(std::size_t index) noexcept {
        if (fds_[index] >= 0) {
            (void)::close(fds_[index]);
            fds_[index] = -1;
        }
    }

    std::array<int, 2> fds_{-1, -1};
};

/// Wait until `fd` is readable or the deadline expires.  Returns false on
/// timeout or error; never blocks past the deadline.
[[nodiscard]] bool wait_readable(int fd, int timeout_ms) {
    struct pollfd descriptor{};
    descriptor.fd = fd;
    descriptor.events = POLLIN;
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(timeout_ms);
    for (;;) {
        const auto remaining =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                deadline - std::chrono::steady_clock::now())
                .count();
        if (remaining < 0) {
            return false;
        }
        const int result = ::poll(&descriptor, 1, static_cast<int>(remaining));
        if (result > 0) {
            return true;
        }
        if (result == 0) {
            return false;
        }
        if (errno != EINTR) {
            return false;
        }
    }
}

/// Bounded waitpid.  On timeout the child is killed and reaped so a broken
/// test can never hang CTest.
[[nodiscard]] bool wait_child(pid_t pid, int& status, int timeout_ms) {
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(timeout_ms);
    for (;;) {
        const pid_t result = ::waitpid(pid, &status, WNOHANG);
        if (result == pid) {
            return true;
        }
        if (result < 0 && errno != EINTR) {
            return false;
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            (void)::kill(pid, SIGKILL);
            (void)::waitpid(pid, &status, 0);
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

#if defined(MPMC_ENABLE_TEST_HOOKS)
int creator_ready_fd = -1;
int creator_release_fd = -1;

void pause_creator_hook(std::string_view, int stage) noexcept {
    if (stage != mpmc::detail::shared_memory_hook_initializing) {
        return;
    }
    const char ready = 'R';
    [[maybe_unused]] const auto ready_written =
        ::write(creator_ready_fd, &ready, 1);
    char token = 0;
    for (;;) {
        const ssize_t got = ::read(creator_release_fd, &token, 1);
        if (got == 1) {
            return;
        }
        if (got < 0 && errno == EINTR) {
            continue;
        }
        return;
    }
}
#endif

#endif

} // namespace

#if !defined(_WIN32)

MPMC_TEST(shared_memory_create_attach_and_remove) {
    using queue_type = shared_queue;
    const std::string name = unique_name("basic");
    remove_quietly(name);
    std::error_code error;

    CHECK(!queue_type::exists(name, error));
    CHECK(!error);
    {
        auto creator = queue_type::create(name, 8, error);
        REQUIRE(!error);
        REQUIRE(creator);
        CHECK_EQ(creator->capacity(), std::size_t{8});
        CHECK_EQ(creator->name(), std::string_view{name});
        CHECK(queue_type::exists(name, error));

        for (std::uint64_t i = 0; i < 8; ++i) {
            REQUIRE(creator->try_push(i + 100));
        }
        CHECK(!creator->try_push(999));

        {
            auto attacher = queue_type::attach(name, error);
            REQUIRE(!error);
            REQUIRE(attacher);
            CHECK_EQ(attacher->capacity(), std::size_t{8});
            queue_value value = 0;
            for (std::uint64_t i = 0; i < 4; ++i) {
                REQUIRE(attacher->try_pop(value));
                CHECK_EQ(value, i + 100);
            }
            REQUIRE(attacher->try_push(500));
        }

        queue_value value = 0;
        for (std::uint64_t i = 4; i < 8; ++i) {
            REQUIRE(creator->try_pop(value));
            CHECK_EQ(value, i + 100);
        }
        REQUIRE(creator->try_pop(value));
        CHECK_EQ(value, std::uint64_t{500});
        CHECK(creator->empty());
    }

    // The creator must not unlink the name.
    CHECK(queue_type::exists(name, error));
    {
        auto attacher = queue_type::attach(name, error);
        REQUIRE(!error);
        REQUIRE(attacher);
        CHECK(attacher->empty());
        CHECK(attacher->try_push(77));
        queue_value value = 0;
        REQUIRE(attacher->try_pop(value));
        CHECK_EQ(value, std::uint64_t{77});
    }

    CHECK(queue_type::remove(name, error));
    CHECK(!queue_type::exists(name, error));
    CHECK(!queue_type::remove(name, error));
    CHECK(error == mpmc::errc::not_found || !error);
}

MPMC_TEST(shared_memory_exclusive_create) {
    using queue_type = shared_queue;
    const std::string name = unique_name("exclusive");
    remove_quietly(name);

    std::error_code error;
    auto creator = queue_type::create(name, 4, error);
    REQUIRE(!error);
    REQUIRE(creator);
    CHECK(!queue_type::create(name, 4, error));
    CHECK(error == mpmc::errc::already_exists);

    CHECK(queue_type::remove(name, error));
}

MPMC_TEST(shared_memory_type_validation) {
    const std::string name = unique_name("types");
    remove_quietly(name);

    std::error_code error;
    auto creator = shared_queue::create(name, 4, error);
    REQUIRE(!error);
    REQUIRE(creator);
    REQUIRE(creator->try_push(1));

    // Different payload size, index size, and eager/lazy mode must all be
    // rejected before the mapping is exposed.
    using other_value = std::uint16_t;
    using other_index = std::conditional_t<sizeof(queue_index) == 4,
                                           std::uint16_t, std::uint32_t>;
    CHECK((!mpmc::shared_memory_queue<other_value, queue_index>::attach(
        name, error)));
    CHECK(error == mpmc::errc::incompatible_layout);
    CHECK((!mpmc::shared_memory_queue<queue_value, other_index>::attach(
        name, error)));
    CHECK(error == mpmc::errc::incompatible_layout);
    CHECK((!mpmc::shared_memory_queue<queue_value, queue_index, true>::attach(
        name, error)));
    CHECK(error == mpmc::errc::incompatible_layout);
    CHECK((!mpmc::shared_memory_queue<queue_value, queue_index, false,
                                      true>::attach(name, error)));
    CHECK(error == mpmc::errc::incompatible_layout);

    queue_value value = 0;
    REQUIRE(creator->try_pop(value));
    CHECK_EQ(value, queue_value{1});

    CHECK(shared_queue::remove(name, error));
}

MPMC_TEST(shared_memory_abi_tag_and_instance) {
    using queue_type = shared_queue;
    using tagged_type =
        mpmc::shared_memory_queue<queue_value, queue_index, false, false,
                                  0x0123456789abcdefULL>;
    const std::string name = unique_name("abi");
    remove_quietly(name);

    static_assert(mpmc::is_shared_memory_safe_v<std::uint64_t>);
    static_assert(!mpmc::is_shared_memory_safe_v<int*>);
    static_assert(tagged_type::abi_tag == 0x0123456789abcdefULL);

    std::error_code error;
    auto creator = tagged_type::create(name, 4, error);
    REQUIRE(!error);
    REQUIRE(creator);
    CHECK(creator->is_owner());
    CHECK(creator->instance_id() != std::uint64_t{0});

    CHECK(!queue_type::attach(name, error));
    CHECK(error == mpmc::errc::incompatible_layout);

    auto attacher = tagged_type::attach(name, error);
    REQUIRE(!error);
    REQUIRE(attacher);
    CHECK(!attacher->is_owner());
    CHECK_EQ(attacher->instance_id(), creator->instance_id());

    REQUIRE(creator->try_push(11));
    queue_value value = 0;
    REQUIRE(attacher->try_pop(value));
    CHECK_EQ(value, std::uint64_t{11});

    CHECK(queue_type::remove(name, error));
}

MPMC_TEST(shared_memory_custom_payload_opt_in) {
    using queue_type = mpmc::shared_memory_queue<approved_payload, queue_index>;
    static_assert(mpmc::is_shared_memory_safe_v<approved_payload>);
    const std::string name = unique_name("payload");
    remove_quietly(name);

    std::error_code error;
    auto creator = queue_type::create(name, 4, error);
    REQUIRE(!error);
    REQUIRE(creator);
    REQUIRE(creator->try_push(approved_payload{7, 9}));

    approved_payload value{};
    REQUIRE(creator->try_pop(value));
    CHECK_EQ(value.first, std::uint16_t{7});
    CHECK_EQ(value.second, std::uint16_t{9});

    CHECK(queue_type::remove(name, error));
}

MPMC_TEST(shared_memory_unlink_on_destroy) {
    using queue_type = shared_queue;
    const std::string name = unique_name("autounlink");
    remove_quietly(name);

    std::error_code error;
    std::unique_ptr<queue_type> attacher;
    {
        auto creator = queue_type::create(
            name, 4, mpmc::shared_memory_lifetime::unlink_on_destroy, error);
        REQUIRE(!error);
        REQUIRE(creator);
        CHECK(creator->is_owner());
        CHECK(queue_type::exists(name, error));

        attacher = queue_type::attach(name, error);
        REQUIRE(!error);
        REQUIRE(attacher);
        CHECK(!attacher->is_owner());
        CHECK_EQ(attacher->instance_id(), creator->instance_id());
        REQUIRE(creator->try_push(7));
    }

    // The creator removed the name, but the already-open mapping stays valid.
    CHECK(!queue_type::exists(name, error));
    queue_value value = 0;
    REQUIRE(attacher->try_pop(value));
    CHECK_EQ(value, std::uint64_t{7});
    CHECK(attacher->try_push(8));
    REQUIRE(attacher->try_pop(value));
    CHECK_EQ(value, std::uint64_t{8});

    attacher.reset();
    CHECK(!queue_type::remove(name, error));
}

MPMC_TEST(shared_memory_autounlink_spares_replacement) {
    using queue_type = shared_queue;
    const std::string name = unique_name("autoreplace");
    remove_quietly(name);

    std::error_code error;
    auto original = queue_type::create(
        name, 4, mpmc::shared_memory_lifetime::unlink_on_destroy, error);
    REQUIRE(!error);
    REQUIRE(original);
    const std::uint64_t original_id = original->instance_id();
    CHECK(queue_type::remove(name, error));

    auto replacement = queue_type::create(name, 4, error);
    REQUIRE(!error);
    REQUIRE(replacement);
    CHECK(replacement->instance_id() != original_id);

    // Destroying the original creator must not unlink the replacement that
    // now owns the same name.
    original.reset();
    CHECK(queue_type::exists(name, error));
    {
        auto attacher = queue_type::attach(name, error);
        REQUIRE(!error);
        REQUIRE(attacher);
        CHECK_EQ(attacher->instance_id(), replacement->instance_id());
    }

    CHECK(queue_type::remove(name, error));
}

MPMC_TEST(shared_memory_autounlink_checks_backing_identity) {
    using queue_type = shared_queue;
    const std::string name = unique_name("autoidentity");
    remove_quietly(name);

    std::error_code error;
    auto original = queue_type::create(
        name, 4, mpmc::shared_memory_lifetime::unlink_on_destroy, error);
    REQUIRE(!error);
    REQUIRE(original);
    const std::uint64_t original_id = original->instance_id();
    CHECK(queue_type::remove(name, error));

    // Replace the name with a different object while the original creator is
    // still alive, and copy the original instance id into the replacement's
    // header.  Destroying the original must compare the retained descriptor's
    // backing identity, not only the instance id, and leave the replacement
    // name alone.
    raw_shared_memory replacement{name, expected_mapping_size(4)};
    REQUIRE(replacement.valid());
    initialize_valid_header(replacement);
    replacement.header().instance_id = original_id;
    const bool identity_available = replacement.has_backing_identity();

    original.reset();
    if (identity_available) {
        // Linux and other kernels that expose a real shm inode: the backing
        // identity differs even though the instance id was forged, so the
        // replacement must survive.
        CHECK(queue_type::exists(name, error));
        CHECK(!error);
        CHECK(queue_type::remove(name, error));
    } else {
        // macOS reports st_dev==0/st_ino==0 for shm.  Auto-unlink
        // falls back to the instance id, so a forged replacement is
        // indistinguishable and is unlinked by the original creator.
        CHECK(!queue_type::exists(name, error));
    }
    CHECK(!queue_type::exists(name, error));
}

MPMC_TEST(shared_memory_fork_child_does_not_unlink_owner_name) {
    using queue_type = shared_queue;
    const std::string name = unique_name("forkowner");
    remove_quietly(name);
    std::error_code error;

    auto creator = queue_type::create(
        name, 4, mpmc::shared_memory_lifetime::unlink_on_destroy, error);
    REQUIRE(!error);
    REQUIRE(creator);

    const pid_t pid = ::fork();
    REQUIRE(pid >= 0);
    if (pid == 0) {
        creator.reset();
        ::_exit(0);
    }

    int status = 0;
    CHECK(wait_child(pid, status, 10000));
    CHECK(WIFEXITED(status));
    CHECK_EQ(WEXITSTATUS(status), 0);
    CHECK(queue_type::exists(name, error));
    CHECK(creator->try_push(9));

    creator.reset();
    CHECK(!queue_type::exists(name, error));
}

MPMC_TEST(shared_memory_name_validation) {
    using queue_type = shared_queue;
    std::error_code error;
    CHECK(!queue_type::exists("", error));
    CHECK(error == mpmc::errc::invalid_name);
    CHECK(!queue_type::exists("/", error));
    CHECK(error == mpmc::errc::invalid_name);
    CHECK(!queue_type::exists("a/b", error));
    CHECK(error == mpmc::errc::invalid_name);
    CHECK(!queue_type::exists(std::string{"/a\0b", 4}, error));
    CHECK(error == mpmc::errc::invalid_name);
}

MPMC_TEST(shared_memory_missing_name) {
    using queue_type = shared_queue;
    const std::string name = unique_name("missing");
    std::error_code error;
    CHECK(!queue_type::attach(name, error));
    CHECK(error == mpmc::errc::not_found);
}

MPMC_TEST(shared_memory_rejects_truncated_object) {
    using queue_type = shared_queue;
    for (const std::size_t size :
         {std::size_t{0}, std::size_t{64}, std::size_t{127}}) {
        const std::string name = unique_name("truncated");
        raw_shared_memory mapping{name, size};
        REQUIRE(mapping.valid());
        // Linux keeps a small requested size; some kernels (macOS) round the
        // object up to a page, in which case the header is readable but
        // entirely zero and ready==0 wins.
        const auto expected =
            mapping.file_size() < mpmc::detail::shared_memory_header_size
                ? mpmc::errc::mapping_too_small
                : mpmc::errc::not_ready;
        expect_attach_error(name, expected);
        std::error_code error;
        CHECK(queue_type::exists(name, error));
        CHECK(!error);
    }
}

MPMC_TEST(shared_memory_rejects_bad_magic_version_and_ready) {
    const std::string name = unique_name("badenvelope");
    raw_shared_memory mapping{name, expected_mapping_size(4)};
    REQUIRE(mapping.valid());
    initialize_valid_header(mapping);

    mapping.header().magic = 0;
    expect_attach_error(name, mpmc::errc::incompatible_layout);
    mapping.header().magic = mpmc::detail::shared_memory_magic;

    mapping.header().version = mpmc::detail::shared_memory_version + 1;
    expect_attach_error(name, mpmc::errc::incompatible_layout);
    mapping.header().version = mpmc::detail::shared_memory_version;

    mapping.header().ready = 0;
    expect_attach_error(name, mpmc::errc::not_ready);
    mapping.header().ready = mpmc::detail::shared_memory_ready_value;

    std::error_code error;
    auto queue = shared_queue::attach(name, error);
    REQUIRE(!error);
    REQUIRE(queue);
    CHECK_EQ(queue->capacity(), std::size_t{4});
}

MPMC_TEST(shared_memory_rejects_invalid_capacity) {
    const std::string name = unique_name("badcap");
    raw_shared_memory mapping{name, expected_mapping_size(4)};
    REQUIRE(mapping.valid());
    initialize_valid_header(mapping);

    for (const std::uint64_t capacity :
         {std::uint64_t{0}, std::uint64_t{3}, std::uint64_t{6},
          std::uint64_t{1} << 62U}) {
        mapping.header().capacity = capacity;
        expect_attach_error(name, mpmc::errc::incompatible_layout);
    }
}

MPMC_TEST(shared_memory_rejects_invalid_offset_and_size) {
    const std::string name = unique_name("badoffset");
    const std::size_t mapping_size = expected_mapping_size(4);
    raw_shared_memory mapping{name, mapping_size};
    REQUIRE(mapping.valid());
    initialize_valid_header(mapping);

    mapping.header().slots_offset = 0;
    expect_attach_error(name, mpmc::errc::incompatible_layout);
    mapping.header().slots_offset = 128;
    expect_attach_error(name, mpmc::errc::incompatible_layout);
    mapping.header().slots_offset = mpmc::detail::shared_memory_slots_offset;

    mapping.header().mapping_size = mapping_size - 128;
    expect_attach_error(name, mpmc::errc::incompatible_layout);
    mapping.header().mapping_size =
        static_cast<std::uint64_t>(mapping_size) +
        static_cast<std::uint64_t>(mapping.file_size());
    expect_attach_error(name, mpmc::errc::mapping_too_small);
    mapping.header().mapping_size = static_cast<std::uint64_t>(mapping_size);

    // A header that claims a valid mapping larger than the actual object.
    const std::string short_name = unique_name("shortmap");
    raw_shared_memory short_mapping{short_name,
                                    mpmc::detail::shared_memory_header_size};
    REQUIRE(short_mapping.valid());
    std::size_t short_capacity = 4;
    while (expected_mapping_size(short_capacity) <= short_mapping.file_size()) {
        short_capacity *= 2;
    }
    initialize_valid_header(short_mapping, short_capacity);
    expect_attach_error(short_name, mpmc::errc::mapping_too_small);
}

MPMC_TEST(shared_memory_rejects_bad_layout_fields) {
    const std::string name = unique_name("badfields");
    raw_shared_memory mapping{name, expected_mapping_size(4)};
    REQUIRE(mapping.valid());
    initialize_valid_header(mapping);

    mapping.header().endianness = 0;
    expect_attach_error(name, mpmc::errc::incompatible_layout);
    mapping.header().endianness =
        mpmc::detail::shared_memory_native_endianness ==
                mpmc::detail::shared_memory_endian_little
            ? mpmc::detail::shared_memory_endian_big
            : mpmc::detail::shared_memory_endian_little;
    expect_attach_error(name, mpmc::errc::incompatible_layout);
    mapping.header().endianness = mpmc::detail::shared_memory_native_endianness;

    mapping.header().instance_id = 0;
    expect_attach_error(name, mpmc::errc::incompatible_layout);
    mapping.header().instance_id = 0x1122334455667788ULL;

    mapping.header().abi_tag = 1;
    expect_attach_error(name, mpmc::errc::incompatible_layout);
    mapping.header().abi_tag = 0;

    mapping.header().flags = mpmc::detail::shared_memory_flag_lazy_push;
    expect_attach_error(name, mpmc::errc::incompatible_layout);
    mapping.header().flags = 0;

    const std::uint32_t correct_cell =
        sizeof(queue_value) + sizeof(queue_index) <= 8 ? 8 : 16;
    mapping.header().cell_bytes = correct_cell == 8 ? 16 : 8;
    expect_attach_error(name, mpmc::errc::incompatible_layout);
    mapping.header().cell_bytes = correct_cell;

    mapping.header().value_size = sizeof(queue_value) == 4 ? 8 : 4;
    expect_attach_error(name, mpmc::errc::incompatible_layout);
    mapping.header().value_size = sizeof(queue_value);

    mapping.header().value_align = 1;
    expect_attach_error(name, mpmc::errc::incompatible_layout);
    mapping.header().value_align = alignof(queue_value);

    mapping.header().index_size = sizeof(queue_index) == 4 ? 8 : 4;
    expect_attach_error(name, mpmc::errc::incompatible_layout);
    mapping.header().index_size = sizeof(queue_index);

    mapping.header().index_align = 1;
    expect_attach_error(name, mpmc::errc::incompatible_layout);
    mapping.header().index_align = alignof(queue_index);
}

MPMC_TEST(shared_memory_failed_setup_cleanup_and_recovery) {
    using queue_type = shared_queue;
    const std::string name = unique_name("cleanup");
    std::error_code error;

    CHECK(!queue_type::create(name, 3, error));
    CHECK(error == mpmc::errc::invalid_capacity);
    CHECK(!queue_type::exists(name, error));

    // An existing malformed object makes create fail without unlinking it.
    {
        raw_shared_memory stale{name, expected_mapping_size(4)};
        REQUIRE(stale.valid());
        initialize_valid_header(stale);

        CHECK(!queue_type::create(name, 4, error));
        CHECK(error == mpmc::errc::already_exists);
        CHECK(queue_type::exists(name, error));

        // A failed attach must also leave the named object in place.
        stale.header().ready = 0;
        expect_attach_error(name, mpmc::errc::not_ready);
        CHECK(queue_type::exists(name, error));

        CHECK(queue_type::remove(name, error));
        CHECK(!error);
        CHECK(!queue_type::exists(name, error));
    }

    // The stale name is gone, so a fresh create and attach succeeds.
    auto creator = queue_type::create(name, 4, error);
    REQUIRE(!error);
    REQUIRE(creator);
    REQUIRE(creator->try_push(41));
    auto attacher = queue_type::attach(name, error);
    REQUIRE(!error);
    REQUIRE(attacher);
    queue_value value = 0;
    REQUIRE(attacher->try_pop(value));
    CHECK_EQ(value, std::uint64_t{41});
    attacher.reset();
    creator.reset();
    CHECK(queue_type::remove(name, error));
    CHECK(!queue_type::exists(name, error));
}

MPMC_TEST(shared_memory_interrupted_creation_is_recovered) {
    using queue_type = shared_queue;
    const std::size_t mapping_size = expected_mapping_size(4);

    // Stage 0 dies before the header is initialized; stage 1 dies after a
    // complete header is written but before ready is published.  Both look
    // like an interrupted creator to the next process.
    for (int stage = 0; stage < 2; ++stage) {
        const std::string name = unique_name("interrupted");
        const pid_t pid = ::fork();
        REQUIRE(pid >= 0);
        if (pid == 0) {
            raw_shared_memory mapping{name, mapping_size};
            if (!mapping.valid()) {
                ::_exit(2);
            }
            if (stage == 1) {
                initialize_valid_header(mapping);
                mapping.header().ready = 0;
            }
            (void)::msync(mapping.data(), mapping.size(), MS_SYNC);
            ::_exit(0);
        }

        int status = 0;
        CHECK(wait_child(pid, status, 10000));
        CHECK(WIFEXITED(status));
        CHECK_EQ(WEXITSTATUS(status), 0);

        expect_attach_error(name, mpmc::errc::not_ready);
        std::error_code error;
        CHECK(queue_type::exists(name, error));
        CHECK(queue_type::remove(name, error));
        CHECK(!queue_type::exists(name, error));

        auto creator = queue_type::create(name, 4, error);
        REQUIRE(!error);
        REQUIRE(creator);
        REQUIRE(creator->try_push(9));
        queue_value value = 0;
        REQUIRE(creator->try_pop(value));
        CHECK_EQ(value, std::uint64_t{9});
        creator.reset();
        CHECK(queue_type::remove(name, error));
    }
}

MPMC_TEST(shared_memory_rejects_unsafe_bootstrap_lock) {
    using queue_type = shared_queue;
    const std::string name = unique_name("badlock");
    std::array<char, 80> path{};
    REQUIRE(lock_path_for(name, path));
    (void)::unlink(path.data());

    const int fd = ::open(path.data(), O_RDWR | O_CREAT | O_EXCL, 0600);
    REQUIRE(fd >= 0);
    REQUIRE(::fchmod(fd, 0666) == 0);
    ::close(fd);

    std::error_code error;
    CHECK(!queue_type::exists(name, error));
    CHECK(error == std::errc::permission_denied);
    (void)::unlink(path.data());

    // A non-regular lock path is rejected before flock.
    REQUIRE(::mkfifo(path.data(), 0600) == 0);
    CHECK(!queue_type::exists(name, error));
    CHECK(error == std::errc::permission_denied);
    (void)::unlink(path.data());
}

MPMC_TEST(shared_memory_move) {
    using queue_type = shared_queue;
    const std::string name = unique_name("move");
    remove_quietly(name);

    std::error_code error;
    auto first = queue_type::create(name, 4, error);
    REQUIRE(!error);
    REQUIRE(first);
    REQUIRE(first->try_push(10));
    queue_type second{std::move(*first)};
    CHECK(!first->valid());
    CHECK(second.valid());
    CHECK_EQ(second.name(), std::string_view{name});
    queue_value value = 0;
    REQUIRE(second.try_pop(value));
    CHECK_EQ(value, std::uint64_t{10});
    CHECK(second.try_push(11));

    auto third = queue_type::attach(name, error);
    REQUIRE(!error);
    REQUIRE(third);
    *third = std::move(second);
    // The moved-from handle is detached; only validity is queried.
    // NOLINTNEXTLINE(clang-analyzer-cplusplus.Move, bugprone-use-after-move)
    CHECK(!second.valid());
    REQUIRE(third->try_pop(value));
    CHECK_EQ(value, std::uint64_t{11});
    CHECK(third->empty());

    CHECK(queue_type::remove(name, error));
}

#if defined(MPMC_ENABLE_TEST_HOOKS)
// Attach must wait for the creator's initialization lock.  A pipe handshake
// pauses the creator after shm_open()/mmap() while it still holds the exclusive
// bootstrap lock, then a second process calls attach() *without* probing the
// name first.  The attacher must still be blocked until the creator publishes
// ready; the timeout proves it cannot hang.
MPMC_TEST(shared_memory_attach_waits_for_creator_initialization) {
    using queue_type = shared_queue;
    constexpr std::uint64_t count = 500;
    constexpr std::size_t capacity = 512;
    const std::string name = unique_name("initwait");
    remove_quietly(name);

    pipe_pair ready;
    pipe_pair release;
    pipe_pair result;
    REQUIRE(ready.valid());
    REQUIRE(release.valid());
    REQUIRE(result.valid());

    creator_ready_fd = ready.write_fd();
    creator_release_fd = release.read_fd();
    mpmc::detail::shared_memory_test_hook.store(&pause_creator_hook,
                                                std::memory_order_release);

    const pid_t creator = ::fork();
    REQUIRE(creator >= 0);
    if (creator == 0) {
        ready.close_read();
        release.close_write();
        result.close_all();
        std::error_code error;
        auto child = queue_type::create(name, capacity, error);
        if (error || !child) {
            ::_exit(2);
        }
        const auto deadline =
            std::chrono::steady_clock::now() + std::chrono::seconds(20);
        for (std::uint64_t i = 0; i < count; ++i) {
            while (!child->try_push(static_cast<queue_value>(i))) {
                if (std::chrono::steady_clock::now() > deadline) {
                    ::_exit(4);
                }
                std::this_thread::yield();
            }
        }
        ::_exit(0);
    }

    // The child inherited the hook and descriptors.  Clear the parent's copy
    // so no other operation in this process can enter the pause.
    mpmc::detail::shared_memory_test_hook.store(nullptr,
                                                std::memory_order_release);
    creator_ready_fd = -1;
    creator_release_fd = -1;
    ready.close_write();
    // Keep the parent's read end of the release pipe open until the test ends;
    // if the creator died early this prevents a write from raising SIGPIPE.

    // The creator is now parked inside create_mapping with the lock held.
    const bool creator_parked = wait_readable(ready.read_fd(), 10000);
    CHECK(creator_parked);

    pid_t attacher = -1;
    if (creator_parked) {
        attacher = ::fork();
        CHECK(attacher >= 0);
        if (attacher == 0) {
            ready.close_all();
            release.close_all();
            result.close_read();
            std::error_code error;
            auto attached = queue_type::attach(name, error);
            const char outcome = (attached && !error) ? 'A' : 'F';
            [[maybe_unused]] const auto outcome_written =
                ::write(result.write_fd(), &outcome, 1);
            ::_exit(0);
        }
        result.close_write();
    }

    if (attacher > 0) {
        // attach() must not return while the creator holds the exclusive
        // bootstrap lock.  A readable pipe here means it returned too early.
        CHECK(!wait_readable(result.read_fd(), 250));
    }

    // Let the creator finish initializing.
    const char go = 'G';
    const bool released = ::write(release.write_fd(), &go, 1) == 1;
    CHECK(released);

    int creator_status = 0;
    CHECK(wait_child(creator, creator_status, 10000));
    CHECK(WIFEXITED(creator_status));
    CHECK_EQ(WEXITSTATUS(creator_status), 0);

    if (attacher > 0) {
        int attacher_status = 0;
        CHECK(wait_child(attacher, attacher_status, 10000));
        CHECK(WIFEXITED(attacher_status));
        CHECK_EQ(WEXITSTATUS(attacher_status), 0);
        CHECK(wait_readable(result.read_fd(), 10000));
        char outcome = 0;
        CHECK_EQ(::read(result.read_fd(), &outcome, 1), ssize_t{1});
        CHECK_EQ(outcome, 'A');
    }

    // The queue created by the paused creator is fully usable.
    std::error_code error;
    auto attached = queue_type::attach(name, error);
    REQUIRE(!error);
    REQUIRE(attached);
    CHECK_EQ(attached->capacity(), capacity);
    for (std::uint64_t i = 0; i < count; ++i) {
        queue_value value = 0;
        REQUIRE(attached->try_pop(value));
        CHECK_EQ(value, static_cast<queue_value>(i));
    }
    CHECK(attached->empty());
    CHECK(queue_type::remove(name, error));
}
#endif

MPMC_TEST(shared_memory_child_producer) {
    using queue_type = shared_queue;
    constexpr std::uint64_t count = 2000;
    const std::string name = unique_name("childprod");
    remove_quietly(name);

    std::error_code error;
    auto parent = queue_type::create(name, 64, error);
    REQUIRE(!error);
    REQUIRE(parent);
    const pid_t pid = ::fork();
    REQUIRE(pid >= 0);

    if (pid == 0) {
        int status = 0;
        std::error_code child_error;
        auto child = queue_type::attach(name, child_error);
        if (child_error || !child) {
            status = 2;
        } else {
            const auto deadline =
                std::chrono::steady_clock::now() + std::chrono::seconds(20);
            for (std::uint64_t i = 0; i < count; ++i) {
                while (!child->try_push(static_cast<queue_value>(i))) {
                    if (std::chrono::steady_clock::now() > deadline) {
                        status = 4;
                        break;
                    }
                    std::this_thread::yield();
                }
                if (status != 0) {
                    break;
                }
            }
        }
        ::_exit(status);
    }

    std::uint64_t next = 0;
    bool timed_out = false;
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(20);
    while (next < count) {
        queue_value value = 0;
        if (parent->try_pop(value)) {
            if (value != next) {
                break;
            }
            ++next;
        } else if (std::chrono::steady_clock::now() > deadline) {
            timed_out = true;
            break;
        } else {
            std::this_thread::yield();
        }
    }

    int status = 0;
    CHECK(wait_child(pid, status, 20000));
    CHECK(!timed_out);
    CHECK(WIFEXITED(status));
    CHECK_EQ(WEXITSTATUS(status), 0);
    CHECK_EQ(next, count);
    CHECK(parent->empty());
    CHECK(queue_type::remove(name, error));
}

MPMC_TEST(shared_memory_child_consumer) {
    using queue_type = shared_queue;
    constexpr std::uint64_t count = 2000;
    const std::string name = unique_name("childcons");
    remove_quietly(name);

    std::error_code error;
    auto parent = queue_type::create(name, 64, error);
    REQUIRE(!error);
    REQUIRE(parent);

    const pid_t pid = ::fork();
    REQUIRE(pid >= 0);
    if (pid == 0) {
        int status = 0;
        std::error_code child_error;
        auto child = queue_type::attach(name, child_error);
        if (child_error || !child) {
            status = 2;
        } else {
            const auto deadline =
                std::chrono::steady_clock::now() + std::chrono::seconds(20);
            for (std::uint64_t i = 0; i < count; ++i) {
                queue_value value = 0;
                queue_index sequence = 0;
                while (!child->try_pop(value, sequence)) {
                    if (std::chrono::steady_clock::now() > deadline) {
                        status = 4;
                        break;
                    }
                    std::this_thread::yield();
                }
                if (status != 0) {
                    break;
                }
                if (value != static_cast<queue_value>(i) ||
                    static_cast<std::uint64_t>(sequence) != i) {
                    status = 3;
                    break;
                }
            }
        }
        ::_exit(status);
    }

    bool timed_out = false;
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(20);
    for (std::uint64_t i = 0; i < count; ++i) {
        while (!parent->try_push(static_cast<queue_value>(i))) {
            if (std::chrono::steady_clock::now() > deadline) {
                timed_out = true;
                break;
            }
            std::this_thread::yield();
        }
        if (timed_out) {
            break;
        }
    }

    int status = 0;
    CHECK(wait_child(pid, status, 20000));
    CHECK(!timed_out);
    CHECK(WIFEXITED(status));
    CHECK_EQ(WEXITSTATUS(status), 0);
    CHECK(parent->empty());
    CHECK(queue_type::remove(name, error));
}

MPMC_TEST(shared_memory_fork_availability) {
    CHECK(mpmc::shared_memory_available);
}

#else

MPMC_TEST(shared_memory_unavailable_on_windows) {
    CHECK(!mpmc::shared_memory_available);
}

#endif
