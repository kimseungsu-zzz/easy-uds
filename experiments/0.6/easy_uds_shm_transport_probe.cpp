#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

// Experimental control-plane/data-plane comparison. A memfd-backed SPSC ring
// carries fixed-size payloads while an eventfd supplies wakeups; the memfd and
// eventfd are transferred over a Unix socketpair with SCM_RIGHTS. This is a
// measurement probe only and is deliberately not part of the public API.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <cstdlib>
#include <iostream>
#include <memory>
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
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>
#endif

namespace {

using Clock = std::chrono::steady_clock;

double percentile(std::vector<double> samples, double fraction) {
    std::sort(samples.begin(), samples.end());
    const auto index = static_cast<std::size_t>(fraction * static_cast<double>(samples.size() - 1));
    return samples[index];
}

#if defined(__linux__)

constexpr std::size_t ring_slots = 8;

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
    explicit SharedMeta(std::size_t payload_size, std::size_t slot_count)
        : write_sequence(0), read_sequence(0), payload_size(payload_size), slot_count(slot_count) {}

    alignas(64) std::atomic<std::uint64_t> write_sequence;
    alignas(64) std::atomic<std::uint64_t> read_sequence;
    std::size_t payload_size;
    std::size_t slot_count;
};

int create_memfd() {
    const int fd = static_cast<int>(::syscall(SYS_memfd_create, "easy-uds-shm-probe", MFD_CLOEXEC));
    if (fd < 0) {
        fail("memfd_create");
    }
    return fd;
}

Mapping map_region(int fd, std::size_t size, int protection) {
    void* const address = ::mmap(nullptr, size, protection, MAP_SHARED, fd, 0);
    if (address == MAP_FAILED) {
        fail("mmap");
    }
    return Mapping(address, size);
}

void send_fds(int socket, const int* descriptors, std::size_t count) {
    char byte = 'S';
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
    if (result != 1 || byte != 'S' || (message.msg_flags & MSG_CTRUNC) != 0) {
        throw std::runtime_error("invalid shared-memory control message");
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
        throw std::runtime_error("shared-memory control message omitted descriptors");
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

struct Result {
    double seconds = 0.0;
    std::vector<double> latency;
};

Result run_shared_memory(std::size_t payload_size, std::size_t rounds) {
    const std::size_t region_size = sizeof(SharedMeta) + payload_size * ring_slots;
    Fd memfd(create_memfd());
    if (::ftruncate(memfd.get(), static_cast<off_t>(region_size)) != 0) {
        fail("ftruncate");
    }
    Mapping producer_map = map_region(memfd.get(), region_size, PROT_READ | PROT_WRITE);
    auto* const producer_meta = new (producer_map.get()) SharedMeta(payload_size, ring_slots);
    Fd event_fd(::eventfd(0, EFD_CLOEXEC));
    if (event_fd.get() < 0) {
        fail("eventfd");
    }
    int controls[2] = {-1, -1};
    if (::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, controls) != 0) {
        fail("socketpair");
    }
    Fd control_sender(controls[0]);
    Fd control_receiver(controls[1]);
    const int passed[2] = {memfd.get(), event_fd.get()};
    send_fds(control_sender.get(), passed, 2);
    auto received = receive_fds(control_receiver.get(), 2);
    Fd consumer_memfd(received[0].release());
    Fd consumer_event_fd(received[1].release());
    Mapping consumer_map = map_region(consumer_memfd.get(), region_size, PROT_READ | PROT_WRITE);
    auto* const consumer_meta = static_cast<SharedMeta*>(consumer_map.get());
    std::vector<char> payload(payload_size, 'x');
    std::vector<char> received_payload(payload_size);
    Result result;
    result.latency.reserve(rounds);
    std::exception_ptr consumer_error;
    std::atomic<bool> start{false};
    std::thread consumer([&] {
        try {
            while (!start.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            for (std::size_t round = 0; round < rounds; ++round) {
                while (consumer_meta->read_sequence.load(std::memory_order_relaxed) >=
                       consumer_meta->write_sequence.load(std::memory_order_acquire)) {
                    wait_for_event(consumer_event_fd.get());
                }
                const auto sequence = consumer_meta->read_sequence.load(std::memory_order_relaxed);
                const auto* source = static_cast<const char*>(consumer_map.get()) + sizeof(SharedMeta) +
                                     (sequence % ring_slots) * payload_size;
                std::memcpy(received_payload.data(), source, payload_size);
                if (received_payload != payload) {
                    throw std::runtime_error("shared-memory payload mismatch");
                }
                consumer_meta->read_sequence.store(sequence + 1, std::memory_order_release);
            }
        } catch (...) {
            consumer_error = std::current_exception();
        }
    });
    const auto started = Clock::now();
    start.store(true, std::memory_order_release);
    for (std::size_t round = 0; round < rounds; ++round) {
        const auto begun = Clock::now();
        std::uint64_t sequence = producer_meta->write_sequence.load(std::memory_order_relaxed);
        while (sequence - producer_meta->read_sequence.load(std::memory_order_acquire) >= ring_slots) {
            std::this_thread::yield();
        }
        auto* destination = static_cast<char*>(producer_map.get()) + sizeof(SharedMeta) +
                            (sequence % ring_slots) * payload_size;
        std::memcpy(destination, payload.data(), payload_size);
        producer_meta->write_sequence.store(sequence + 1, std::memory_order_release);
        signal_event(event_fd.get());
        result.latency.push_back(
            std::chrono::duration<double, std::micro>(Clock::now() - begun).count());
    }
    consumer.join();
    result.seconds = std::chrono::duration<double>(Clock::now() - started).count();
    if (consumer_error) {
        std::rethrow_exception(consumer_error);
    }
    return result;
}

void write_exact(int fd, const char* data, std::size_t size) {
    while (size != 0) {
        const ssize_t result = ::write(fd, data, size);
        if (result > 0) {
            data += result;
            size -= static_cast<std::size_t>(result);
        } else if (result < 0 && errno == EINTR) {
            continue;
        } else {
            fail("write socketpair");
        }
    }
}

void read_exact(int fd, char* data, std::size_t size) {
    while (size != 0) {
        const ssize_t result = ::read(fd, data, size);
        if (result > 0) {
            data += result;
            size -= static_cast<std::size_t>(result);
        } else if (result < 0 && errno == EINTR) {
            continue;
        } else {
            fail("read socketpair");
        }
    }
}

Result run_socketpair(std::size_t payload_size, std::size_t rounds) {
    int sockets[2] = {-1, -1};
    if (::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sockets) != 0) {
        fail("socketpair baseline");
    }
    Fd producer_socket(sockets[0]);
    Fd consumer_socket(sockets[1]);
    std::vector<char> payload(payload_size, 'x');
    std::vector<char> received_payload(payload_size);
    Result result;
    result.latency.reserve(rounds);
    std::exception_ptr consumer_error;
    std::atomic<bool> start{false};
    std::thread consumer([&] {
        try {
            while (!start.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            for (std::size_t round = 0; round < rounds; ++round) {
                read_exact(consumer_socket.get(), received_payload.data(), payload_size);
                if (received_payload != payload) {
                    throw std::runtime_error("socketpair payload mismatch");
                }
            }
        } catch (...) {
            consumer_error = std::current_exception();
        }
    });
    const auto started = Clock::now();
    start.store(true, std::memory_order_release);
    for (std::size_t round = 0; round < rounds; ++round) {
        const auto begun = Clock::now();
        write_exact(producer_socket.get(), payload.data(), payload.size());
        result.latency.push_back(
            std::chrono::duration<double, std::micro>(Clock::now() - begun).count());
    }
    consumer.join();
    result.seconds = std::chrono::duration<double>(Clock::now() - started).count();
    if (consumer_error) {
        std::rethrow_exception(consumer_error);
    }
    return result;
}

#endif

} // namespace

int main(int argc, char** argv) {
#if !defined(__linux__)
    (void)argc;
    (void)argv;
    std::cout << "shared-memory transport probe unavailable on this platform\n";
    return 0;
#else
    const std::size_t payload_size =
        argc > 1 ? static_cast<std::size_t>(std::strtoull(argv[1], nullptr, 10)) : 4096U;
    const std::size_t rounds =
        argc > 2 ? static_cast<std::size_t>(std::strtoull(argv[2], nullptr, 10)) : 10000U;
    if (payload_size == 0 || payload_size > 1024U * 1024U || rounds == 0 || argc > 3) {
        std::cerr << "usage: easy_uds_shm_transport_probe [payload_bytes] [rounds]\n";
        return 2;
    }
    const Result shared = run_shared_memory(payload_size, rounds);
    const Result socket = run_socketpair(payload_size, rounds);
    const double total_bytes = static_cast<double>(payload_size) * static_cast<double>(rounds);
    std::cout << "payload=" << payload_size << ", rounds=" << rounds << '\n'
              << "shared-memory: " << total_bytes / shared.seconds / (1024.0 * 1024.0 * 1024.0)
              << " GiB/s, p50-send=" << percentile(shared.latency, 0.50) << " us, p99-send="
              << percentile(shared.latency, 0.99) << " us\n"
              << "socketpair:    " << total_bytes / socket.seconds / (1024.0 * 1024.0 * 1024.0)
              << " GiB/s, p50-send=" << percentile(socket.latency, 0.50) << " us, p99-send="
              << percentile(socket.latency, 0.99) << " us\n";
    return 0;
#endif
}
