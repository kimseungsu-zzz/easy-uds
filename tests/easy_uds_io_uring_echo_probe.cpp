#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

// Exact 8-byte request/response transport A/B. Both backends use the same
// blocking client harness, correctness checks, barrier, latency collection,
// and process CPU accounting. The server side is either a level-triggered
// epoll state machine or a basic io_uring ACCEPT -> RECV -> SEND state machine.
// This measures a transport ceiling; it does not replace the production epoll
// reactor or include easy-uds parsing, dispatch, backpressure, and handlers.

#include <liburing.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <iostream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include <arpa/inet.h>
#include <fcntl.h>
#include <sys/epoll.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

namespace {

using Clock = std::chrono::steady_clock;

constexpr std::size_t payload_size = sizeof(std::uint64_t);
constexpr unsigned ring_entries = 128;
constexpr std::size_t max_connections = 64;
constexpr std::size_t max_total_requests = 10000000;

[[noreturn]] void fail(const char* operation) {
    throw std::runtime_error(std::string(operation) + ": " + std::strerror(errno));
}

double percentile(std::vector<double> samples, double fraction) {
    std::sort(samples.begin(), samples.end());
    const auto index = static_cast<std::size_t>(fraction * static_cast<double>(samples.size() - 1));
    return samples[index];
}

class Fd {
  public:
    explicit Fd(int value = -1) noexcept : value_(value) {}
    ~Fd() {
        if (value_ >= 0) {
            (void)::close(value_);
        }
    }
    Fd(const Fd&) = delete;
    Fd& operator=(const Fd&) = delete;
    Fd(Fd&& other) noexcept : value_(std::exchange(other.value_, -1)) {}
    Fd& operator=(Fd&& other) noexcept {
        if (this != &other) {
            if (value_ >= 0) {
                (void)::close(value_);
            }
            value_ = std::exchange(other.value_, -1);
        }
        return *this;
    }
    [[nodiscard]] int get() const noexcept { return value_; }

  private:
    int value_;
};

class SocketPath {
  public:
    explicit SocketPath(std::string value) : value_(std::move(value)) {
        (void)::unlink(value_.c_str());
    }
    ~SocketPath() { (void)::unlink(value_.c_str()); }
    [[nodiscard]] const std::string& get() const noexcept { return value_; }

  private:
    std::string value_;
};

void set_nonblocking(int fd) {
    const int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags < 0 || ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) != 0) {
        fail("fcntl O_NONBLOCK");
    }
}

void set_client_timeout(int fd) {
    timeval timeout{};
    timeout.tv_sec = 5;
    if (::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) != 0 ||
        ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout)) != 0) {
        fail("setsockopt timeout");
    }
}

sockaddr_un make_address(const std::string& path) {
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    if (path.size() >= sizeof(address.sun_path)) {
        throw std::invalid_argument("probe socket path is too long");
    }
    std::memcpy(address.sun_path, path.c_str(), path.size() + 1);
    return address;
}

Fd make_listener(const std::string& path) {
    Fd listener(::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0));
    if (listener.get() < 0) {
        fail("socket");
    }
    set_nonblocking(listener.get());
    const sockaddr_un address = make_address(path);
    if (::bind(listener.get(), reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0 ||
        ::listen(listener.get(), static_cast<int>(max_connections)) != 0) {
        fail("bind/listen");
    }
    return listener;
}

void send_exact(int fd, const void* data, std::size_t size) {
    const auto* bytes = static_cast<const unsigned char*>(data);
    while (size != 0) {
        const ssize_t result = ::send(fd, bytes, size, MSG_NOSIGNAL);
        if (result > 0) {
            bytes += result;
            size -= static_cast<std::size_t>(result);
        } else if (result < 0 && errno == EINTR) {
            continue;
        } else {
            fail("client send");
        }
    }
}

void receive_exact(int fd, void* data, std::size_t size) {
    auto* bytes = static_cast<unsigned char*>(data);
    while (size != 0) {
        const ssize_t result = ::recv(fd, bytes, size, 0);
        if (result > 0) {
            bytes += result;
            size -= static_cast<std::size_t>(result);
        } else if (result < 0 && errno == EINTR) {
            continue;
        } else if (result == 0) {
            throw std::runtime_error("client received unexpected EOF");
        } else {
            fail("client recv");
        }
    }
}

struct Usage {
    std::uint64_t cpu_us = 0;
    std::uint64_t voluntary_cs = 0;
    std::uint64_t involuntary_cs = 0;
};

Usage usage_snapshot() {
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

Usage usage_delta(const Usage& before, const Usage& after) {
    return {after.cpu_us - before.cpu_us, after.voluntary_cs - before.voluntary_cs,
            after.involuntary_cs - before.involuntary_cs};
}

struct Endpoint {
    explicit Endpoint(int accepted_fd) : fd(accepted_fd) {}
    ~Endpoint() {
        if (fd >= 0) {
            (void)::close(fd);
        }
    }
    Endpoint(const Endpoint&) = delete;
    Endpoint& operator=(const Endpoint&) = delete;

    int fd = -1;
    std::array<unsigned char, payload_size> buffer{};
    std::size_t offset = 0;
    bool sending = false;
    bool write_interest = false;
};

struct Counters {
    const char* first_name = "";
    std::uint64_t first = 0;
    const char* second_name = "";
    std::uint64_t second = 0;
};

class EpollEchoServer {
  public:
    explicit EpollEchoServer(int listener) : listener_(listener), epoll_(::epoll_create1(EPOLL_CLOEXEC)) {
        if (epoll_.get() < 0) {
            fail("epoll_create1");
        }
        epoll_event event{};
        event.events = EPOLLIN;
        event.data.fd = listener_;
        if (::epoll_ctl(epoll_.get(), EPOLL_CTL_ADD, listener_, &event) != 0) {
            fail("epoll_ctl listener");
        }
        ++ctl_count_;
    }

    void operator()() {
        std::array<epoll_event, 128> events{};
        while (running_.load(std::memory_order_relaxed)) {
            ++wait_count_;
            const int ready = ::epoll_wait(epoll_.get(), events.data(),
                                           static_cast<int>(events.size()), 20);
            if (ready < 0) {
                if (errno == EINTR) {
                    continue;
                }
                fail("epoll_wait");
            }
            for (int index = 0; index < ready; ++index) {
                const int fd = events[static_cast<std::size_t>(index)].data.fd;
                if (fd == listener_) {
                    accept_ready();
                    continue;
                }
                const auto found = endpoints_.find(fd);
                if (found == endpoints_.end()) {
                    continue;
                }
                const std::uint32_t flags = events[static_cast<std::size_t>(index)].events;
                if ((flags & (EPOLLERR | EPOLLHUP | EPOLLRDHUP)) != 0 ||
                    !service(*found->second, flags)) {
                    remove(fd);
                }
            }
        }
    }

    void request_stop() noexcept { running_.store(false, std::memory_order_relaxed); }

    [[nodiscard]] Counters counters() const noexcept {
        return {"epoll_wait", wait_count_, "epoll_ctl", ctl_count_};
    }

  private:
    void accept_ready() {
        for (;;) {
            const int accepted =
                ::accept4(listener_, nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);
            if (accepted < 0) {
                if (errno == EINTR) {
                    continue;
                }
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    return;
                }
                fail("accept4");
            }
            auto endpoint = std::make_unique<Endpoint>(accepted);
            epoll_event event{};
            event.events = EPOLLIN | EPOLLRDHUP;
            event.data.fd = accepted;
            if (::epoll_ctl(epoll_.get(), EPOLL_CTL_ADD, accepted, &event) != 0) {
                fail("epoll_ctl add");
            }
            ++ctl_count_;
            endpoints_.emplace(accepted, std::move(endpoint));
        }
    }

    bool service(Endpoint& endpoint, std::uint32_t flags) {
        if (endpoint.sending) {
            return (flags & EPOLLOUT) == 0 || flush(endpoint);
        }
        if ((flags & EPOLLIN) == 0) {
            return true;
        }
        for (;;) {
            const ssize_t result = ::recv(endpoint.fd, endpoint.buffer.data() + endpoint.offset,
                                          payload_size - endpoint.offset, 0);
            if (result > 0) {
                endpoint.offset += static_cast<std::size_t>(result);
                if (endpoint.offset == payload_size) {
                    endpoint.offset = 0;
                    endpoint.sending = true;
                    return flush(endpoint);
                }
                continue;
            }
            if (result == 0) {
                return false;
            }
            if (errno == EINTR) {
                continue;
            }
            return errno == EAGAIN || errno == EWOULDBLOCK;
        }
    }

    bool flush(Endpoint& endpoint) {
        while (endpoint.offset != payload_size) {
            const ssize_t result =
                ::send(endpoint.fd, endpoint.buffer.data() + endpoint.offset,
                       payload_size - endpoint.offset, MSG_NOSIGNAL);
            if (result > 0) {
                endpoint.offset += static_cast<std::size_t>(result);
                continue;
            }
            if (result < 0 && errno == EINTR) {
                continue;
            }
            if (result < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                if (!endpoint.write_interest) {
                    modify(endpoint, EPOLLOUT | EPOLLRDHUP);
                    endpoint.write_interest = true;
                }
                return true;
            }
            return false;
        }
        endpoint.offset = 0;
        endpoint.sending = false;
        if (endpoint.write_interest) {
            modify(endpoint, EPOLLIN | EPOLLRDHUP);
            endpoint.write_interest = false;
        }
        return true;
    }

    void modify(const Endpoint& endpoint, std::uint32_t flags) {
        epoll_event event{};
        event.events = flags;
        event.data.fd = endpoint.fd;
        if (::epoll_ctl(epoll_.get(), EPOLL_CTL_MOD, endpoint.fd, &event) != 0) {
            fail("epoll_ctl mod");
        }
        ++ctl_count_;
    }

    void remove(int fd) noexcept {
        (void)::epoll_ctl(epoll_.get(), EPOLL_CTL_DEL, fd, nullptr);
        ++ctl_count_;
        endpoints_.erase(fd);
    }

    int listener_;
    Fd epoll_;
    std::atomic<bool> running_{true};
    std::unordered_map<int, std::unique_ptr<Endpoint>> endpoints_;
    std::uint64_t wait_count_ = 0;
    std::uint64_t ctl_count_ = 0;
};

class IouringEchoServer {
  public:
    explicit IouringEchoServer(int listener) : listener_(listener) {
        const int result = ::io_uring_queue_init(ring_entries, &ring_, 0);
        if (result != 0) {
            errno = -result;
            fail("io_uring_queue_init");
        }
        ring_initialized_ = true;
    }

    ~IouringEchoServer() {
        // Pending operations may still reference Endpoint buffers. Tear down
        // the ring before member destruction releases those buffers.
        if (ring_initialized_) {
            ::io_uring_queue_exit(&ring_);
        }
    }

    void operator()() {
        if (!submit_accept()) {
            return;
        }
        bool timeout_pending = false;
        while (running_.load(std::memory_order_relaxed)) {
            if (!timeout_pending) {
                if (!submit_timeout()) {
                    break;
                }
                timeout_pending = true;
            }
            ++enter_count_;
            const int wait_result = ::io_uring_submit_and_wait(&ring_, 1);
            if (wait_result < 0 && wait_result != -EINTR) {
                errno = -wait_result;
                fail("io_uring_submit_and_wait");
            }

            unsigned head = 0;
            unsigned count = 0;
            io_uring_cqe* cqe = nullptr;
            io_uring_for_each_cqe(&ring_, head, cqe) {
                const void* tag = ::io_uring_cqe_get_data(cqe);
                const int result = cqe->res;
                if (tag == accept_tag()) {
                    (void)submit_accept();
                    if (result >= 0) {
                        auto endpoint = std::make_unique<Endpoint>(result);
                        Endpoint* const raw = endpoint.get();
                        endpoints_.emplace(result, std::move(endpoint));
                        if (!submit_recv(raw)) {
                            endpoints_.erase(result);
                        }
                    }
                } else if (tag == timeout_tag()) {
                    timeout_pending = false;
                } else {
                    auto* const endpoint = static_cast<Endpoint*>(const_cast<void*>(tag));
                    if (!complete_io(endpoint, result)) {
                        endpoints_.erase(endpoint->fd);
                    }
                }
                ++count;
            }
            ::io_uring_cq_advance(&ring_, count);
        }
    }

    void request_stop() noexcept { running_.store(false, std::memory_order_relaxed); }

    [[nodiscard]] Counters counters() const noexcept {
        return {"io_uring_submit_and_wait", enter_count_, "", 0};
    }

  private:
    static void* accept_tag() noexcept {
        return reinterpret_cast<void*>(static_cast<std::uintptr_t>(1));
    }
    static void* timeout_tag() noexcept {
        return reinterpret_cast<void*>(static_cast<std::uintptr_t>(2));
    }

    bool complete_io(Endpoint* endpoint, int result) {
        if (result <= 0 || static_cast<std::size_t>(result) > payload_size - endpoint->offset) {
            return false;
        }
        endpoint->offset += static_cast<std::size_t>(result);
        if (endpoint->offset != payload_size) {
            return endpoint->sending ? submit_send(endpoint) : submit_recv(endpoint);
        }
        endpoint->offset = 0;
        endpoint->sending = !endpoint->sending;
        return endpoint->sending ? submit_send(endpoint) : submit_recv(endpoint);
    }

    bool submit_accept() noexcept {
        io_uring_sqe* const sqe = ::io_uring_get_sqe(&ring_);
        if (sqe == nullptr) {
            running_.store(false, std::memory_order_relaxed);
            return false;
        }
        ::io_uring_prep_accept(sqe, listener_, nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);
        ::io_uring_sqe_set_data(sqe, accept_tag());
        return true;
    }

    bool submit_recv(Endpoint* endpoint) noexcept {
        io_uring_sqe* const sqe = ::io_uring_get_sqe(&ring_);
        if (sqe == nullptr) {
            running_.store(false, std::memory_order_relaxed);
            return false;
        }
        ::io_uring_prep_recv(sqe, endpoint->fd, endpoint->buffer.data() + endpoint->offset,
                             payload_size - endpoint->offset, 0);
        ::io_uring_sqe_set_data(sqe, endpoint);
        endpoint->sending = false;
        return true;
    }

    bool submit_send(Endpoint* endpoint) noexcept {
        io_uring_sqe* const sqe = ::io_uring_get_sqe(&ring_);
        if (sqe == nullptr) {
            running_.store(false, std::memory_order_relaxed);
            return false;
        }
        ::io_uring_prep_send(sqe, endpoint->fd, endpoint->buffer.data() + endpoint->offset,
                             payload_size - endpoint->offset, MSG_NOSIGNAL);
        ::io_uring_sqe_set_data(sqe, endpoint);
        endpoint->sending = true;
        return true;
    }

    bool submit_timeout() noexcept {
        io_uring_sqe* const sqe = ::io_uring_get_sqe(&ring_);
        if (sqe == nullptr) {
            running_.store(false, std::memory_order_relaxed);
            return false;
        }
        timeout_.tv_sec = 0;
        timeout_.tv_nsec = 20000000;
        ::io_uring_prep_timeout(sqe, &timeout_, 0, 0);
        ::io_uring_sqe_set_data(sqe, timeout_tag());
        return true;
    }

    int listener_;
    std::atomic<bool> running_{true};
    std::unordered_map<int, std::unique_ptr<Endpoint>> endpoints_;
    io_uring ring_{};
    __kernel_timespec timeout_{};
    bool ring_initialized_ = false;
    std::uint64_t enter_count_ = 0;
};

struct Report {
    const char* backend = "";
    double wall_seconds = 0.0;
    std::vector<double> latency;
    Usage usage;
    Counters counters;
};

template <typename Server>
Report run_probe(const char* backend, std::size_t connections, std::size_t rounds) {
    SocketPath socket_path("/tmp/easy-uds-" + std::string(backend) + "-echo-" +
                           std::to_string(static_cast<long long>(::getpid())) + ".sock");
    Fd listener = make_listener(socket_path.get());
    Server server(listener.get());
    std::exception_ptr server_error;
    std::thread server_thread([&] {
        try {
            server();
        } catch (...) {
            server_error = std::current_exception();
        }
    });

    std::vector<std::vector<double>> latency_samples(connections);
    std::atomic<std::size_t> ready{0};
    std::atomic<bool> start{false};
    std::atomic<bool> failed{false};
    std::mutex error_mutex;
    std::exception_ptr client_error;
    std::vector<std::thread> clients;
    clients.reserve(connections);
    for (std::size_t index = 0; index < connections; ++index) {
        clients.emplace_back([&, index] {
            bool announced = false;
            try {
                Fd client(::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0));
                if (client.get() < 0) {
                    fail("client socket");
                }
                set_client_timeout(client.get());
                const sockaddr_un address = make_address(socket_path.get());
                if (::connect(client.get(), reinterpret_cast<const sockaddr*>(&address),
                              sizeof(address)) != 0) {
                    fail("connect");
                }
                auto& samples = latency_samples[index];
                samples.reserve(rounds);
                ready.fetch_add(1, std::memory_order_relaxed);
                announced = true;
                while (!start.load(std::memory_order_acquire) &&
                       !failed.load(std::memory_order_acquire)) {
                    std::this_thread::yield();
                }
                for (std::size_t round = 0;
                     round < rounds && !failed.load(std::memory_order_acquire); ++round) {
                    const std::uint64_t expected =
                        static_cast<std::uint64_t>(index * rounds + round + 1);
                    std::uint64_t payload = expected;
                    const auto begun = Clock::now();
                    send_exact(client.get(), &payload, sizeof(payload));
                    receive_exact(client.get(), &payload, sizeof(payload));
                    if (payload != expected) {
                        throw std::runtime_error("echo payload mismatch");
                    }
                    samples.push_back(
                        std::chrono::duration<double, std::micro>(Clock::now() - begun).count());
                }
            } catch (...) {
                failed.store(true, std::memory_order_release);
                {
                    std::lock_guard<std::mutex> lock(error_mutex);
                    if (!client_error) {
                        client_error = std::current_exception();
                    }
                }
                if (!announced) {
                    ready.fetch_add(1, std::memory_order_relaxed);
                }
            }
        });
    }

    while (ready.load(std::memory_order_relaxed) != connections) {
        std::this_thread::yield();
    }
    const Usage before = usage_snapshot();
    const auto started = Clock::now();
    start.store(true, std::memory_order_release);
    for (auto& client : clients) {
        client.join();
    }
    const double wall_seconds = std::chrono::duration<double>(Clock::now() - started).count();
    const Usage usage = usage_delta(before, usage_snapshot());

    server.request_stop();
    server_thread.join();
    if (server_error) {
        std::rethrow_exception(server_error);
    }
    if (client_error) {
        std::rethrow_exception(client_error);
    }

    Report report;
    report.backend = backend;
    report.wall_seconds = wall_seconds;
    report.usage = usage;
    report.counters = server.counters();
    report.latency.reserve(connections * rounds);
    for (const auto& samples : latency_samples) {
        report.latency.insert(report.latency.end(), samples.begin(), samples.end());
    }
    return report;
}

void print_report(const Report& report) {
    const double total = static_cast<double>(report.latency.size());
    std::cout << report.backend << ": throughput=" << total / report.wall_seconds
              << " req/s, p50=" << percentile(report.latency, 0.50)
              << " us, p95=" << percentile(report.latency, 0.95)
              << " us, p99=" << percentile(report.latency, 0.99)
              << " us, p99.9=" << percentile(report.latency, 0.999)
              << " us, CPU=" << static_cast<double>(report.usage.cpu_us) / total
              << " CPU-s/1M, vcs=" << report.usage.voluntary_cs
              << ", ivcs=" << report.usage.involuntary_cs << '\n';
    std::cout << "  " << report.counters.first_name << '=' << report.counters.first << " (~"
              << static_cast<double>(report.counters.first) / total << "/req)";
    if (report.counters.second_name[0] != '\0') {
        std::cout << ", " << report.counters.second_name << '=' << report.counters.second << " (~"
                  << static_cast<double>(report.counters.second) / total << "/req)";
    }
    std::cout << '\n';
}

} // namespace

int main(int argc, char** argv) {
    const std::size_t connections =
        argc > 1 ? static_cast<std::size_t>(std::strtoull(argv[1], nullptr, 10)) : 8U;
    const std::size_t rounds =
        argc > 2 ? static_cast<std::size_t>(std::strtoull(argv[2], nullptr, 10)) : 100000U;
    const std::string backend = argc > 3 ? argv[3] : "both";
    if (connections == 0 || connections > max_connections || rounds == 0 || argc > 4 ||
        rounds > max_total_requests / connections ||
        (backend != "both" && backend != "epoll" && backend != "io_uring")) {
        std::cerr << "usage: easy_uds_io_uring_echo_probe [connections<=64] [rounds] "
                     "[both|epoll|io_uring]\n";
        return 2;
    }

    std::cout << "connections=" << connections << ", rounds/client=" << rounds
              << ", total/backend=" << connections * rounds << '\n';
    if (backend == "both" || backend == "epoll") {
        print_report(run_probe<EpollEchoServer>("epoll", connections, rounds));
    }
    if (backend == "both" || backend == "io_uring") {
        print_report(run_probe<IouringEchoServer>("io_uring", connections, rounds));
    }
    return 0;
}
