#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

// Experimental request/response comparison.  Both transports carry the
// protocol-v2 20-byte header and an echo body; the shared-memory path keeps
// UDS only for SCM_RIGHTS control-plane setup.  This is a benchmark probe,
// not a transport implementation or a public API promise.

#include "../src/protocol/codec.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <cstdlib>
#include <iostream>
#include <new>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#if defined(__linux__)
#include <cerrno>
#include <fcntl.h>
#include <linux/memfd.h>
#include <sys/eventfd.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/syscall.h>
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

constexpr std::size_t ring_slots = 8;
constexpr std::size_t route_size = 4;
constexpr unsigned char body_byte = 0x5A;

[[noreturn]] void fail(const char* operation) {
    throw std::runtime_error(std::string(operation) + ": " + std::strerror(errno));
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

struct SharedMeta {
    SharedMeta(std::size_t slot_size, std::size_t slots)
        : write_sequence(0), read_sequence(0), slot_size(slot_size), slots(slots) {}

    alignas(64) std::atomic<std::uint64_t> write_sequence;
    alignas(64) std::atomic<std::uint64_t> read_sequence;
    std::size_t slot_size;
    std::size_t slots;
};

struct Result {
    double seconds = 0.0;
    std::vector<double> latency;
};

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
        throw std::runtime_error("invalid framed shared-memory control message");
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
        throw std::runtime_error("framed control message omitted descriptors");
    }
    return descriptors;
}

void wait_for_event(int event_fd) {
    std::uint64_t count = 0;
    while (::read(event_fd, &count, sizeof(count)) < 0) {
        if (errno == EINTR) {
            continue;
        }
        fail("read eventfd");
    }
}

void signal_event(int event_fd) {
    const std::uint64_t one = 1;
    while (::write(event_fd, &one, sizeof(one)) < 0) {
        if (errno == EINTR) {
            continue;
        }
        fail("write eventfd");
    }
}

void ring_write(SharedMeta* meta, void* mapping, int event_fd,
                const std::vector<unsigned char>& frame) {
    std::uint64_t sequence = meta->write_sequence.load(std::memory_order_relaxed);
    while (sequence - meta->read_sequence.load(std::memory_order_acquire) >= meta->slots) {
        std::this_thread::yield();
    }
    auto* destination = static_cast<unsigned char*>(mapping) + sizeof(SharedMeta) +
                        (sequence % meta->slots) * meta->slot_size;
    std::memcpy(destination, frame.data(), frame.size());
    meta->write_sequence.store(sequence + 1, std::memory_order_release);
    signal_event(event_fd);
}

void ring_read(SharedMeta* meta, void* mapping, int event_fd, std::vector<unsigned char>& frame) {
    for (;;) {
        const auto sequence = meta->read_sequence.load(std::memory_order_relaxed);
        if (sequence < meta->write_sequence.load(std::memory_order_acquire)) {
            const auto* source = static_cast<const unsigned char*>(mapping) + sizeof(SharedMeta) +
                                 (sequence % meta->slots) * meta->slot_size;
            std::memcpy(frame.data(), source, frame.size());
            meta->read_sequence.store(sequence + 1, std::memory_order_release);
            return;
        }
        wait_for_event(event_fd);
    }
}

std::vector<unsigned char> make_frame(WireType type, std::uint32_t request_id,
                                      std::size_t payload_size) {
    HeaderBytes header = type == WireType::request
                             ? easy_uds::detail::protocol::encode_header(
                                   type, request_id, static_cast<std::uint32_t>(route_size),
                                   static_cast<std::uint32_t>(payload_size))
                             : easy_uds::detail::protocol::encode_header(
                                   type, request_id, 200U, static_cast<std::uint32_t>(payload_size));
    std::vector<unsigned char> frame(header.size() + payload_size, body_byte);
    std::copy(header.begin(), header.end(), frame.begin());
    return frame;
}

void validate_frame(const std::vector<unsigned char>& frame, WireType expected_type,
                    std::uint32_t expected_id, std::size_t payload_size) {
    if (frame.size() != easy_uds::detail::protocol::header_size + payload_size) {
        throw std::runtime_error("framed payload size mismatch");
    }
    HeaderBytes header{};
    std::copy_n(frame.begin(), header.size(), header.begin());
    const DecodedHeader decoded = easy_uds::detail::protocol::decode_header(header, expected_type);
    const std::uint32_t expected_arg1 = expected_type == WireType::request ? route_size : 200U;
    if (decoded.request_id != expected_id || decoded.arg1 != expected_arg1 ||
        decoded.arg2 != payload_size || decoded.flags != 0) {
        throw std::runtime_error("framed header mismatch");
    }
    if (!std::all_of(frame.begin() + static_cast<std::ptrdiff_t>(header.size()), frame.end(),
                     [](unsigned char value) { return value == body_byte; })) {
        throw std::runtime_error("framed body mismatch");
    }
}

void write_exact(int fd, const unsigned char* data, std::size_t size) {
    while (size != 0) {
        const ssize_t result = ::write(fd, data, size);
        if (result > 0) {
            data += result;
            size -= static_cast<std::size_t>(result);
        } else if (result < 0 && errno == EINTR) {
            continue;
        } else {
            fail("write framed socketpair");
        }
    }
}

void read_exact(int fd, unsigned char* data, std::size_t size) {
    while (size != 0) {
        const ssize_t result = ::read(fd, data, size);
        if (result > 0) {
            data += result;
            size -= static_cast<std::size_t>(result);
        } else if (result < 0 && errno == EINTR) {
            continue;
        } else {
            fail("read framed socketpair");
        }
    }
}

Result run_shared_memory(std::size_t payload_size, std::size_t rounds) {
    const std::size_t frame_size = easy_uds::detail::protocol::header_size + payload_size;
    const std::size_t region_size = sizeof(SharedMeta) + frame_size * ring_slots;
    Fd request_memfd(create_memfd("easy-uds-framed-req"));
    Fd response_memfd(create_memfd("easy-uds-framed-resp"));
    if (::ftruncate(request_memfd.get(), static_cast<off_t>(region_size)) != 0 ||
        ::ftruncate(response_memfd.get(), static_cast<off_t>(region_size)) != 0) {
        fail("ftruncate framed ring");
    }
    Mapping client_request_map = map_region(request_memfd.get(), region_size);
    Mapping client_response_map = map_region(response_memfd.get(), region_size);
    auto* const client_request_meta = new (client_request_map.get()) SharedMeta(frame_size, ring_slots);
    auto* const client_response_meta = new (client_response_map.get()) SharedMeta(frame_size, ring_slots);
    Fd request_event_fd(::eventfd(0, EFD_CLOEXEC));
    Fd response_event_fd(::eventfd(0, EFD_CLOEXEC));
    if (request_event_fd.get() < 0 || response_event_fd.get() < 0) {
        fail("eventfd framed ring");
    }
    int controls[2] = {-1, -1};
    if (::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, controls) != 0) {
        fail("socketpair framed control");
    }
    Fd control_sender(controls[0]);
    Fd control_receiver(controls[1]);
    const int passed[4] = {request_memfd.get(), request_event_fd.get(), response_memfd.get(),
                           response_event_fd.get()};
    send_fds(control_sender.get(), passed, 4);
    auto received = receive_fds(control_receiver.get(), 4);
    Fd server_request_memfd(received[0].release());
    Fd server_request_event(received[1].release());
    Fd server_response_memfd(received[2].release());
    Fd server_response_event(received[3].release());
    Mapping server_request_map = map_region(server_request_memfd.get(), region_size);
    Mapping server_response_map = map_region(server_response_memfd.get(), region_size);
    auto* const server_request_meta = static_cast<SharedMeta*>(server_request_map.get());
    auto* const server_response_meta = static_cast<SharedMeta*>(server_response_map.get());
    const auto request = make_frame(WireType::request, 1, payload_size);
    std::vector<unsigned char> request_frame(request);
    std::vector<unsigned char> response_frame(frame_size);
    Result result;
    result.latency.reserve(rounds);
    std::exception_ptr server_error;
    std::thread server([&] {
        try {
            std::vector<unsigned char> server_request_frame(frame_size);
            std::vector<unsigned char> server_response_frame(frame_size);
            for (std::size_t round = 0; round < rounds; ++round) {
                ring_read(server_request_meta, server_request_map.get(), server_request_event.get(),
                          server_request_frame);
                const auto request_id = static_cast<std::uint32_t>(round + 1);
                validate_frame(server_request_frame, WireType::request, request_id, payload_size);
                server_response_frame = make_frame(WireType::response, request_id, payload_size);
                ring_write(server_response_meta, server_response_map.get(), server_response_event.get(),
                           server_response_frame);
            }
        } catch (...) {
            server_error = std::current_exception();
        }
    });
    const auto started = Clock::now();
    for (std::size_t round = 0; round < rounds; ++round) {
        const auto request_id = static_cast<std::uint32_t>(round + 1);
        request_frame = make_frame(WireType::request, request_id, payload_size);
        const auto begun = Clock::now();
        ring_write(client_request_meta, client_request_map.get(), request_event_fd.get(), request_frame);
        ring_read(client_response_meta, client_response_map.get(), response_event_fd.get(), response_frame);
        validate_frame(response_frame, WireType::response, request_id, payload_size);
        result.latency.push_back(
            std::chrono::duration<double, std::micro>(Clock::now() - begun).count());
    }
    server.join();
    result.seconds = std::chrono::duration<double>(Clock::now() - started).count();
    if (server_error) {
        std::rethrow_exception(server_error);
    }
    return result;
}

Result run_socketpair(std::size_t payload_size, std::size_t rounds) {
    int sockets[2] = {-1, -1};
    if (::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sockets) != 0) {
        fail("socketpair framed baseline");
    }
    Fd client_socket(sockets[0]);
    Fd server_socket(sockets[1]);
    const std::size_t frame_size = easy_uds::detail::protocol::header_size + payload_size;
    std::vector<unsigned char> request_frame(frame_size);
    std::vector<unsigned char> response_frame(frame_size);
    Result result;
    result.latency.reserve(rounds);
    std::exception_ptr server_error;
    std::thread server([&] {
        try {
            std::vector<unsigned char> server_request_frame(frame_size);
            std::vector<unsigned char> server_response_frame(frame_size);
            for (std::size_t round = 0; round < rounds; ++round) {
                read_exact(server_socket.get(), server_request_frame.data(), server_request_frame.size());
                const auto request_id = static_cast<std::uint32_t>(round + 1);
                validate_frame(server_request_frame, WireType::request, request_id, payload_size);
                server_response_frame = make_frame(WireType::response, request_id, payload_size);
                write_exact(server_socket.get(), server_response_frame.data(), server_response_frame.size());
            }
        } catch (...) {
            server_error = std::current_exception();
        }
    });
    const auto started = Clock::now();
    for (std::size_t round = 0; round < rounds; ++round) {
        const auto request_id = static_cast<std::uint32_t>(round + 1);
        request_frame = make_frame(WireType::request, request_id, payload_size);
        const auto begun = Clock::now();
        write_exact(client_socket.get(), request_frame.data(), request_frame.size());
        read_exact(client_socket.get(), response_frame.data(), response_frame.size());
        validate_frame(response_frame, WireType::response, request_id, payload_size);
        result.latency.push_back(
            std::chrono::duration<double, std::micro>(Clock::now() - begun).count());
    }
    server.join();
    result.seconds = std::chrono::duration<double>(Clock::now() - started).count();
    if (server_error) {
        std::rethrow_exception(server_error);
    }
    return result;
}

#endif

} // namespace

int main(int argc, char** argv) {
#if !defined(__linux__)
    (void)argc;
    (void)argv;
    std::cout << "framed shared-memory transport probe unavailable on this platform\n";
    return 0;
#else
    const std::size_t payload_size =
        argc > 1 ? static_cast<std::size_t>(std::strtoull(argv[1], nullptr, 10)) : 4096U;
    const std::size_t rounds =
        argc > 2 ? static_cast<std::size_t>(std::strtoull(argv[2], nullptr, 10)) : 1000U;
    if (payload_size == 0 || payload_size > 1024U * 1024U || rounds == 0 || argc > 3) {
        std::cerr << "usage: easy_uds_shm_framed_probe [payload_bytes] [rounds]\n";
        return 2;
    }
    const Result shared = run_shared_memory(payload_size, rounds);
    const Result socket = run_socketpair(payload_size, rounds);
    const double wire_bytes = 2.0 *
                              static_cast<double>(easy_uds::detail::protocol::header_size + payload_size) *
                              static_cast<double>(rounds);
    std::cout << "framed payload=" << payload_size << ", rounds=" << rounds << "\n"
              << "shared-memory: " << wire_bytes / shared.seconds / (1024.0 * 1024.0 * 1024.0)
              << " GiB/s, p50-rtt=" << percentile(shared.latency, 0.50) << " us, p99-rtt="
              << percentile(shared.latency, 0.99) << " us\n"
              << "socketpair:    " << wire_bytes / socket.seconds / (1024.0 * 1024.0 * 1024.0)
              << " GiB/s, p50-rtt=" << percentile(socket.latency, 0.50) << " us, p99-rtt="
              << percentile(socket.latency, 0.99) << " us\n";
    return 0;
#endif
}
