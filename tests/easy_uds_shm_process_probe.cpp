#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

// Process-to-process shared-memory experiment. The parent transfers two memfd
// rings and two eventfds to a forked child through SCM_RIGHTS. Three framed
// request/response paths isolate payload copies and wakeups:
//   1. copy through private buffers + eventfd on every publish
//   2. application writes/reads mapped slots directly + eventfd every publish
//   3. direct slots + eventfd only when the consumer advertises sleep
// A framed SOCK_STREAM socketpair is the process baseline. This is deliberately
// a standalone experiment, not a public transport API.

#include "../src/protocol.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <new>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#if defined(__linux__)
#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <linux/memfd.h>
#include <sys/eventfd.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace {

using Clock = std::chrono::steady_clock;
using easy_uds::detail::protocol::DecodedHeader;
using easy_uds::detail::protocol::HeaderBytes;
using easy_uds::detail::protocol::WireType;

double percentile(std::vector<double> samples, double fraction) {
    std::sort(samples.begin(), samples.end());
    const auto index = static_cast<std::size_t>(fraction * static_cast<double>(samples.size() - 1));
    return samples[index];
}

#if defined(__linux__)

static_assert(std::atomic<std::uint64_t>::is_always_lock_free,
              "probe requires lock-free 64-bit inter-process atomics");
static_assert(std::atomic<std::uint32_t>::is_always_lock_free,
              "probe requires lock-free 32-bit inter-process atomics");

constexpr std::size_t ring_slots = 8;
constexpr std::size_t route_size = 4;
constexpr std::array<unsigned char, route_size> route = {'/', 'e', 'c', 'h'};
constexpr unsigned char body_byte = 0x5A;
constexpr std::size_t default_conditional_spin_iterations = 256;

[[noreturn]] void fail(const char* operation) {
    throw std::runtime_error(std::string(operation) + ": " + std::strerror(errno));
}

inline void cpu_relax() noexcept {
#if defined(__x86_64__) || defined(__i386__)
    __builtin_ia32_pause();
#elif defined(__aarch64__) || defined(__arm__)
    __asm__ __volatile__("yield" ::: "memory");
#else
    __asm__ __volatile__("" ::: "memory");
#endif
}

class Fd {
  public:
    explicit Fd(int fd = -1) noexcept : fd_(fd) {}
    ~Fd() {
        if (fd_ >= 0) {
            (void)::close(fd_);
        }
    }
    Fd(const Fd&) = delete;
    Fd& operator=(const Fd&) = delete;
    Fd(Fd&& other) noexcept : fd_(std::exchange(other.fd_, -1)) {}
    Fd& operator=(Fd&& other) noexcept {
        if (this != &other) {
            if (fd_ >= 0) {
                (void)::close(fd_);
            }
            fd_ = std::exchange(other.fd_, -1);
        }
        return *this;
    }
    [[nodiscard]] int get() const noexcept { return fd_; }
    [[nodiscard]] int release() noexcept { return std::exchange(fd_, -1); }

  private:
    int fd_;
};

class Mapping {
  public:
    Mapping(void* address, std::size_t size) noexcept : address_(address), size_(size) {}
    ~Mapping() {
        if (address_ != MAP_FAILED) {
            (void)::munmap(address_, size_);
        }
    }
    Mapping(const Mapping&) = delete;
    Mapping& operator=(const Mapping&) = delete;
    [[nodiscard]] void* get() const noexcept { return address_; }

  private:
    void* address_;
    std::size_t size_;
};

struct ChildStats {
    std::uint64_t cpu_us = 0;
    std::uint64_t voluntary_cs = 0;
    std::uint64_t involuntary_cs = 0;
};

struct SharedMeta {
    SharedMeta(std::size_t slot_size, std::size_t slots)
        : write_sequence(0), read_sequence(0), sleeping(0), wake_writes(0),
          slot_size(slot_size), slots(slots) {}

    alignas(64) std::atomic<std::uint64_t> write_sequence;
    alignas(64) std::atomic<std::uint64_t> read_sequence;
    alignas(64) std::atomic<std::uint32_t> sleeping;
    std::atomic<std::uint64_t> wake_writes;
    std::size_t slot_size;
    std::size_t slots;
    ChildStats child_stats;
};

struct UsageSnapshot {
    std::uint64_t cpu_us = 0;
    std::uint64_t voluntary_cs = 0;
    std::uint64_t involuntary_cs = 0;
};

struct Result {
    double seconds = 0.0;
    std::vector<double> latency;
    std::uint64_t cpu_us = 0;
    std::uint64_t voluntary_cs = 0;
    std::uint64_t involuntary_cs = 0;
    std::uint64_t wake_writes = 0;
};

enum class SharedMode { copy_always, direct_always, direct_conditional };

UsageSnapshot usage_snapshot() {
    rusage usage{};
    if (::getrusage(RUSAGE_SELF, &usage) != 0) {
        fail("getrusage");
    }
    return {
        static_cast<std::uint64_t>(usage.ru_utime.tv_sec) * 1000000U +
            static_cast<std::uint64_t>(usage.ru_utime.tv_usec) +
            static_cast<std::uint64_t>(usage.ru_stime.tv_sec) * 1000000U +
            static_cast<std::uint64_t>(usage.ru_stime.tv_usec),
        static_cast<std::uint64_t>(usage.ru_nvcsw),
        static_cast<std::uint64_t>(usage.ru_nivcsw),
    };
}

ChildStats usage_delta(const UsageSnapshot& before, const UsageSnapshot& after) {
    return {after.cpu_us - before.cpu_us, after.voluntary_cs - before.voluntary_cs,
            after.involuntary_cs - before.involuntary_cs};
}

int create_memfd(const char* name) {
    const int fd = static_cast<int>(::syscall(SYS_memfd_create, name, MFD_CLOEXEC));
    if (fd < 0) {
        fail("memfd_create");
    }
    return fd;
}

Mapping map_region(int fd, std::size_t size) {
    void* const address = ::mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (address == MAP_FAILED) {
        fail("mmap");
    }
    return Mapping(address, size);
}

void write_exact(int fd, const void* data, std::size_t size) {
    const auto* bytes = static_cast<const unsigned char*>(data);
    while (size != 0) {
        const ssize_t result = ::write(fd, bytes, size);
        if (result > 0) {
            bytes += result;
            size -= static_cast<std::size_t>(result);
        } else if (result < 0 && errno == EINTR) {
            continue;
        } else {
            fail("write");
        }
    }
}

void read_exact(int fd, void* data, std::size_t size) {
    auto* bytes = static_cast<unsigned char*>(data);
    while (size != 0) {
        const ssize_t result = ::read(fd, bytes, size);
        if (result > 0) {
            bytes += result;
            size -= static_cast<std::size_t>(result);
        } else if (result < 0 && errno == EINTR) {
            continue;
        } else if (result == 0) {
            throw std::runtime_error("unexpected control/socket EOF");
        } else {
            fail("read");
        }
    }
}

void send_fds(int socket, const int* descriptors, std::size_t count) {
    char byte = 'F';
    iovec vector{&byte, sizeof(byte)};
    msghdr message{};
    message.msg_iov = &vector;
    message.msg_iovlen = 1;
    std::vector<char> control(CMSG_SPACE(count * sizeof(int)));
    message.msg_control = control.data();
    message.msg_controllen = control.size();
    cmsghdr* header = CMSG_FIRSTHDR(&message);
    header->cmsg_level = SOL_SOCKET;
    header->cmsg_type = SCM_RIGHTS;
    header->cmsg_len = CMSG_LEN(count * sizeof(int));
    std::memcpy(CMSG_DATA(header), descriptors, count * sizeof(int));
    if (::sendmsg(socket, &message, 0) != 1) {
        fail("sendmsg SCM_RIGHTS");
    }
}

std::vector<Fd> receive_fds(int socket, std::size_t expected_count) {
    char byte = 0;
    iovec vector{&byte, sizeof(byte)};
    msghdr message{};
    message.msg_iov = &vector;
    message.msg_iovlen = 1;
    std::vector<char> control(CMSG_SPACE(expected_count * sizeof(int)));
    message.msg_control = control.data();
    message.msg_controllen = control.size();
    const ssize_t result = ::recvmsg(socket, &message, MSG_CMSG_CLOEXEC);
    if (result != 1 || byte != 'F' || (message.msg_flags & MSG_CTRUNC) != 0) {
        throw std::runtime_error("invalid process SHM control message");
    }
    std::vector<Fd> descriptors;
    for (cmsghdr* header = CMSG_FIRSTHDR(&message); header != nullptr;
         header = CMSG_NXTHDR(&message, header)) {
        if (header->cmsg_level != SOL_SOCKET || header->cmsg_type != SCM_RIGHTS ||
            header->cmsg_len != CMSG_LEN(expected_count * sizeof(int))) {
            continue;
        }
        const auto* values = reinterpret_cast<const int*>(CMSG_DATA(header));
        for (std::size_t index = 0; index < expected_count; ++index) {
            descriptors.emplace_back(values[index]);
        }
        break;
    }
    if (descriptors.size() != expected_count) {
        throw std::runtime_error("process SHM control message omitted descriptors");
    }
    return descriptors;
}

void wait_for_event(int event_fd) {
    std::uint64_t value = 0;
    read_exact(event_fd, &value, sizeof(value));
}

void signal_event(SharedMeta* meta, int event_fd) {
    const std::uint64_t one = 1;
    write_exact(event_fd, &one, sizeof(one));
    meta->wake_writes.fetch_add(1, std::memory_order_relaxed);
}

unsigned char* write_slot(SharedMeta* meta, void* mapping) {
    const std::uint64_t sequence = meta->write_sequence.load(std::memory_order_relaxed);
    while (sequence - meta->read_sequence.load(std::memory_order_acquire) >= meta->slots) {
        cpu_relax();
    }
    return static_cast<unsigned char*>(mapping) + sizeof(SharedMeta) +
           (sequence % meta->slots) * meta->slot_size;
}

void publish_slot(SharedMeta* meta, int event_fd, bool conditional) {
    const auto sequence = meta->write_sequence.load(std::memory_order_relaxed);
    meta->write_sequence.store(sequence + 1, std::memory_order_release);
    if (!conditional || meta->sleeping.exchange(0, std::memory_order_acq_rel) != 0) {
        signal_event(meta, event_fd);
    }
}

const unsigned char* read_slot(SharedMeta* meta, const void* mapping, int event_fd,
                               bool conditional, std::size_t spin_iterations) {
    for (;;) {
        const auto sequence = meta->read_sequence.load(std::memory_order_relaxed);
        if (sequence < meta->write_sequence.load(std::memory_order_acquire)) {
            return static_cast<const unsigned char*>(mapping) + sizeof(SharedMeta) +
                   (sequence % meta->slots) * meta->slot_size;
        }
        if (conditional) {
            for (std::size_t spin = 0; spin < spin_iterations; ++spin) {
                if (sequence < meta->write_sequence.load(std::memory_order_acquire)) {
                    return static_cast<const unsigned char*>(mapping) + sizeof(SharedMeta) +
                           (sequence % meta->slots) * meta->slot_size;
                }
                cpu_relax();
            }
            // An RMW pairs with the producer's exchange. If the producer wins
            // the race, this acquire observes its release before the condition
            // is checked again. If the consumer wins, the producer observes 1
            // and must signal. A plain store permits the store-buffering
            // outcome where both sides miss each other at a zero-spin window.
            (void)meta->sleeping.exchange(1, std::memory_order_acq_rel);
            if (sequence < meta->write_sequence.load(std::memory_order_acquire)) {
                meta->sleeping.store(0, std::memory_order_release);
                continue;
            }
        }
        wait_for_event(event_fd);
        meta->sleeping.store(0, std::memory_order_release);
    }
}

void release_slot(SharedMeta* meta) {
    const auto sequence = meta->read_sequence.load(std::memory_order_relaxed);
    meta->read_sequence.store(sequence + 1, std::memory_order_release);
}

void write_frame(unsigned char* destination, WireType type, std::uint32_t request_id,
                 std::size_t payload_size) {
    const HeaderBytes header = type == WireType::request
                                   ? easy_uds::detail::protocol::encode_header(
                                         type, request_id, static_cast<std::uint32_t>(route_size),
                                         static_cast<std::uint32_t>(payload_size))
                                   : easy_uds::detail::protocol::encode_header(
                                         type, request_id, 200U,
                                         static_cast<std::uint32_t>(payload_size));
    std::copy(header.begin(), header.end(), destination);
    auto* body = destination + header.size();
    if (type == WireType::request) {
        std::copy(route.begin(), route.end(), body);
        body += route.size();
    }
    std::fill(body, body + payload_size, body_byte);
}

void validate_frame(const unsigned char* frame, WireType expected_type,
                    std::uint32_t expected_id, std::size_t payload_size) {
    HeaderBytes header{};
    std::copy_n(frame, header.size(), header.begin());
    const DecodedHeader decoded = easy_uds::detail::protocol::decode_header(header, expected_type);
    const std::uint32_t expected_arg1 = expected_type == WireType::request ? route_size : 200U;
    if (decoded.request_id != expected_id || decoded.arg1 != expected_arg1 ||
        decoded.arg2 != payload_size || decoded.flags != 0) {
        throw std::runtime_error("process probe frame header mismatch");
    }
    const auto* body = frame + header.size();
    if (expected_type == WireType::request) {
        if (!std::equal(route.begin(), route.end(), body)) {
            throw std::runtime_error("process probe frame route mismatch");
        }
        body += route.size();
    }
    if (!std::all_of(body, body + payload_size,
                     [](unsigned char value) { return value == body_byte; })) {
        throw std::runtime_error("process probe frame body mismatch");
    }
}

void child_shared(int control_fd, std::size_t request_size, std::size_t response_size,
                  std::size_t payload_size, std::size_t rounds, SharedMode mode,
                  std::size_t response_delay_us, std::size_t spin_iterations) {
    auto received = receive_fds(control_fd, 4);
    const std::size_t request_region_size = sizeof(SharedMeta) + request_size * ring_slots;
    const std::size_t response_region_size = sizeof(SharedMeta) + response_size * ring_slots;
    Mapping request_map = map_region(received[0].get(), request_region_size);
    Mapping response_map = map_region(received[2].get(), response_region_size);
    auto* const request_meta = static_cast<SharedMeta*>(request_map.get());
    auto* const response_meta = static_cast<SharedMeta*>(response_map.get());
    const bool direct = mode != SharedMode::copy_always;
    const bool conditional = mode == SharedMode::direct_conditional;
    std::vector<unsigned char> request_buffer(request_size);
    std::vector<unsigned char> response_buffer(response_size);
    const char ready = 'R';
    write_exact(control_fd, &ready, sizeof(ready));
    const UsageSnapshot before = usage_snapshot();
    for (std::size_t round = 0; round < rounds; ++round) {
        const auto request_id = static_cast<std::uint32_t>(round + 1);
        const auto* request_slot = read_slot(request_meta, request_map.get(), received[1].get(),
                                             conditional, spin_iterations);
        if (direct) {
            validate_frame(request_slot, WireType::request, request_id, payload_size);
        } else {
            std::memcpy(request_buffer.data(), request_slot, request_size);
            validate_frame(request_buffer.data(), WireType::request, request_id, payload_size);
        }
        release_slot(request_meta);
        if (response_delay_us != 0) {
            std::this_thread::sleep_for(std::chrono::microseconds(response_delay_us));
        }
        auto* const response_slot = write_slot(response_meta, response_map.get());
        if (direct) {
            write_frame(response_slot, WireType::response, request_id, payload_size);
        } else {
            write_frame(response_buffer.data(), WireType::response, request_id, payload_size);
            std::memcpy(response_slot, response_buffer.data(), response_size);
        }
        publish_slot(response_meta, received[3].get(), conditional);
    }
    request_meta->child_stats = usage_delta(before, usage_snapshot());
    const char complete = 'K';
    write_exact(control_fd, &complete, sizeof(complete));
}

void wait_child(pid_t child) {
    int status = 0;
    while (::waitpid(child, &status, 0) < 0) {
        if (errno == EINTR) {
            continue;
        }
        fail("waitpid");
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        throw std::runtime_error("process probe child failed");
    }
}

Result run_shared_process(std::size_t payload_size, std::size_t rounds, SharedMode mode,
                          std::size_t response_delay_us, std::size_t spin_iterations) {
    const std::size_t request_size =
        easy_uds::detail::protocol::header_size + route_size + payload_size;
    const std::size_t response_size = easy_uds::detail::protocol::header_size + payload_size;
    const std::size_t request_region_size = sizeof(SharedMeta) + request_size * ring_slots;
    const std::size_t response_region_size = sizeof(SharedMeta) + response_size * ring_slots;
    int controls[2] = {-1, -1};
    if (::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, controls) != 0) {
        fail("socketpair SHM control");
    }
    Fd parent_control(controls[0]);
    Fd child_control(controls[1]);
    const pid_t child = ::fork();
    if (child < 0) {
        fail("fork SHM child");
    }
    if (child == 0) {
        parent_control = Fd{};
        try {
            child_shared(child_control.get(), request_size, response_size, payload_size, rounds,
                         mode, response_delay_us, spin_iterations);
            _exit(0);
        } catch (const std::exception& error) {
            std::cerr << "SHM child: " << error.what() << '\n';
            _exit(2);
        }
    }
    child_control = Fd{};

    Fd request_memfd(create_memfd("easy-uds-process-request"));
    Fd response_memfd(create_memfd("easy-uds-process-response"));
    if (::ftruncate(request_memfd.get(), static_cast<off_t>(request_region_size)) != 0 ||
        ::ftruncate(response_memfd.get(), static_cast<off_t>(response_region_size)) != 0) {
        fail("ftruncate process rings");
    }
    Mapping request_map = map_region(request_memfd.get(), request_region_size);
    Mapping response_map = map_region(response_memfd.get(), response_region_size);
    auto* const request_meta = new (request_map.get()) SharedMeta(request_size, ring_slots);
    auto* const response_meta = new (response_map.get()) SharedMeta(response_size, ring_slots);
    Fd request_event(::eventfd(0, EFD_CLOEXEC));
    Fd response_event(::eventfd(0, EFD_CLOEXEC));
    if (request_event.get() < 0 || response_event.get() < 0) {
        fail("eventfd process rings");
    }
    const int passed[4] = {request_memfd.get(), request_event.get(), response_memfd.get(),
                           response_event.get()};
    send_fds(parent_control.get(), passed, 4);
    char ready = 0;
    read_exact(parent_control.get(), &ready, sizeof(ready));
    if (ready != 'R') {
        throw std::runtime_error("SHM child did not become ready");
    }

    const bool direct = mode != SharedMode::copy_always;
    const bool conditional = mode == SharedMode::direct_conditional;
    std::vector<unsigned char> request_buffer(request_size);
    std::vector<unsigned char> response_buffer(response_size);
    Result result;
    result.latency.reserve(rounds);
    const UsageSnapshot usage_before = usage_snapshot();
    const auto started = Clock::now();
    for (std::size_t round = 0; round < rounds; ++round) {
        const auto request_id = static_cast<std::uint32_t>(round + 1);
        auto* const request_slot = write_slot(request_meta, request_map.get());
        if (direct) {
            write_frame(request_slot, WireType::request, request_id, payload_size);
        } else {
            write_frame(request_buffer.data(), WireType::request, request_id, payload_size);
            std::memcpy(request_slot, request_buffer.data(), request_size);
        }
        const auto begun = Clock::now();
        publish_slot(request_meta, request_event.get(), conditional);
        const auto* response_slot = read_slot(response_meta, response_map.get(), response_event.get(),
                                              conditional, spin_iterations);
        if (direct) {
            validate_frame(response_slot, WireType::response, request_id, payload_size);
        } else {
            std::memcpy(response_buffer.data(), response_slot, response_size);
            validate_frame(response_buffer.data(), WireType::response, request_id, payload_size);
        }
        release_slot(response_meta);
        result.latency.push_back(
            std::chrono::duration<double, std::micro>(Clock::now() - begun).count());
    }
    result.seconds = std::chrono::duration<double>(Clock::now() - started).count();
    const ChildStats parent_delta = usage_delta(usage_before, usage_snapshot());
    char complete = 0;
    read_exact(parent_control.get(), &complete, sizeof(complete));
    wait_child(child);
    if (complete != 'K') {
        throw std::runtime_error("SHM child did not complete");
    }
    result.cpu_us = parent_delta.cpu_us + request_meta->child_stats.cpu_us;
    result.voluntary_cs = parent_delta.voluntary_cs + request_meta->child_stats.voluntary_cs;
    result.involuntary_cs = parent_delta.involuntary_cs + request_meta->child_stats.involuntary_cs;
    result.wake_writes = request_meta->wake_writes.load(std::memory_order_relaxed) +
                         response_meta->wake_writes.load(std::memory_order_relaxed);
    return result;
}

void child_socket(int socket_fd, std::size_t payload_size, std::size_t rounds,
                  std::size_t response_delay_us) {
    const std::size_t request_size =
        easy_uds::detail::protocol::header_size + route_size + payload_size;
    const std::size_t response_size = easy_uds::detail::protocol::header_size + payload_size;
    std::vector<unsigned char> request(request_size);
    std::vector<unsigned char> response(response_size);
    const char ready = 'R';
    write_exact(socket_fd, &ready, sizeof(ready));
    const UsageSnapshot before = usage_snapshot();
    for (std::size_t round = 0; round < rounds; ++round) {
        const auto request_id = static_cast<std::uint32_t>(round + 1);
        read_exact(socket_fd, request.data(), request.size());
        validate_frame(request.data(), WireType::request, request_id, payload_size);
        if (response_delay_us != 0) {
            std::this_thread::sleep_for(std::chrono::microseconds(response_delay_us));
        }
        write_frame(response.data(), WireType::response, request_id, payload_size);
        write_exact(socket_fd, response.data(), response.size());
    }
    const ChildStats stats = usage_delta(before, usage_snapshot());
    write_exact(socket_fd, &stats, sizeof(stats));
}

Result run_socket_process(std::size_t payload_size, std::size_t rounds,
                          std::size_t response_delay_us) {
    int sockets[2] = {-1, -1};
    if (::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sockets) != 0) {
        fail("socketpair process baseline");
    }
    Fd parent_socket(sockets[0]);
    Fd child_socket_fd(sockets[1]);
    const pid_t child = ::fork();
    if (child < 0) {
        fail("fork socket child");
    }
    if (child == 0) {
        parent_socket = Fd{};
        try {
            child_socket(child_socket_fd.get(), payload_size, rounds, response_delay_us);
            _exit(0);
        } catch (const std::exception& error) {
            std::cerr << "socket child: " << error.what() << '\n';
            _exit(2);
        }
    }
    child_socket_fd = Fd{};
    char ready = 0;
    read_exact(parent_socket.get(), &ready, sizeof(ready));
    if (ready != 'R') {
        throw std::runtime_error("socket child did not become ready");
    }
    const std::size_t request_size =
        easy_uds::detail::protocol::header_size + route_size + payload_size;
    const std::size_t response_size = easy_uds::detail::protocol::header_size + payload_size;
    std::vector<unsigned char> request(request_size);
    std::vector<unsigned char> response(response_size);
    Result result;
    result.latency.reserve(rounds);
    const UsageSnapshot usage_before = usage_snapshot();
    const auto started = Clock::now();
    for (std::size_t round = 0; round < rounds; ++round) {
        const auto request_id = static_cast<std::uint32_t>(round + 1);
        write_frame(request.data(), WireType::request, request_id, payload_size);
        const auto begun = Clock::now();
        write_exact(parent_socket.get(), request.data(), request.size());
        read_exact(parent_socket.get(), response.data(), response.size());
        validate_frame(response.data(), WireType::response, request_id, payload_size);
        result.latency.push_back(
            std::chrono::duration<double, std::micro>(Clock::now() - begun).count());
    }
    result.seconds = std::chrono::duration<double>(Clock::now() - started).count();
    const ChildStats parent_delta = usage_delta(usage_before, usage_snapshot());
    ChildStats child_stats{};
    read_exact(parent_socket.get(), &child_stats, sizeof(child_stats));
    wait_child(child);
    result.cpu_us = parent_delta.cpu_us + child_stats.cpu_us;
    result.voluntary_cs = parent_delta.voluntary_cs + child_stats.voluntary_cs;
    result.involuntary_cs = parent_delta.involuntary_cs + child_stats.involuntary_cs;
    return result;
}

void print_result(const char* name, const Result& result, std::size_t exchange_size,
                  std::size_t rounds) {
    const double wire_bytes =
        static_cast<double>(exchange_size) * static_cast<double>(rounds);
    const double cpu_seconds_per_million = static_cast<double>(result.cpu_us) /
                                           static_cast<double>(rounds);
    std::cout << name << ": " << wire_bytes / result.seconds / (1024.0 * 1024.0 * 1024.0)
              << " GiB/s, p50=" << percentile(result.latency, 0.50) << " us, p99="
              << percentile(result.latency, 0.99) << " us, p99.9="
              << percentile(result.latency, 0.999) << " us, CPU="
              << cpu_seconds_per_million << " CPU-s/1M, vcs=" << result.voluntary_cs
              << ", ivcs=" << result.involuntary_cs;
    if (result.wake_writes != 0) {
        std::cout << ", eventfd=" << result.wake_writes << " ("
                  << static_cast<double>(result.wake_writes) / static_cast<double>(rounds)
                  << "/exchange)";
    }
    std::cout << '\n';
}

#endif

} // namespace

int main(int argc, char** argv) {
#if !defined(__linux__)
    (void)argc;
    (void)argv;
    std::cout << "process shared-memory probe unavailable on this platform\n";
    return 0;
#else
    const std::size_t payload_size =
        argc > 1 ? static_cast<std::size_t>(std::strtoull(argv[1], nullptr, 10)) : 4096U;
    const std::size_t rounds =
        argc > 2 ? static_cast<std::size_t>(std::strtoull(argv[2], nullptr, 10)) : 1000U;
    const std::size_t response_delay_us =
        argc > 3 ? static_cast<std::size_t>(std::strtoull(argv[3], nullptr, 10)) : 0U;
    const std::size_t spin_iterations =
        argc > 4 ? static_cast<std::size_t>(std::strtoull(argv[4], nullptr, 10))
                 : default_conditional_spin_iterations;
    if (payload_size == 0 || payload_size > 4U * 1024U * 1024U || rounds == 0 || argc > 5 ||
        response_delay_us > 1000000U || spin_iterations > 1000000U ||
        rounds > static_cast<std::size_t>(UINT32_MAX - 1U)) {
        std::cerr << "usage: easy_uds_shm_process_probe [payload_bytes<=4194304] [rounds] "
                     "[response_delay_us<=1000000] [conditional_spin<=1000000]\n";
        return 2;
    }
    const Result copy_always =
        run_shared_process(payload_size, rounds, SharedMode::copy_always, response_delay_us,
                           spin_iterations);
    const Result direct_always =
        run_shared_process(payload_size, rounds, SharedMode::direct_always, response_delay_us,
                           spin_iterations);
    const Result direct_conditional =
        run_shared_process(payload_size, rounds, SharedMode::direct_conditional, response_delay_us,
                           spin_iterations);
    const Result socket = run_socket_process(payload_size, rounds, response_delay_us);
    const std::size_t exchange_size =
        2U * easy_uds::detail::protocol::header_size + route_size + 2U * payload_size;
    std::cout << "process framed payload=" << payload_size << ", rounds=" << rounds
              << ", conditional-spin=" << spin_iterations
              << ", response-delay=" << response_delay_us << " us\n";
    print_result("shm copy + always wake", copy_always, exchange_size, rounds);
    print_result("shm direct + always wake", direct_always, exchange_size, rounds);
    print_result("shm direct + conditional", direct_conditional, exchange_size, rounds);
    print_result("socketpair", socket, exchange_size, rounds);
    return 0;
#endif
}
