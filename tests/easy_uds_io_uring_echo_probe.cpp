#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

// io_uring request/response transport probe. A re-armed ACCEPT + per-connection
// RECV -> SEND echo on one ring, completions reaped in batches so the server
// submits once per batch. The client runs `connections` threads doing symmetric
// tiny ping-pongs. Reported req/s and p50/p99 are the transport ceiling for the
// UDS workload; compare syscalls/req (strace -c) with the epoll easy-uds stack.

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
#include <iostream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include <arpa/inet.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

namespace {

using Clock = std::chrono::steady_clock;

constexpr std::size_t payload_size = 8;
constexpr std::size_t buffer_size = 4096;
constexpr unsigned ring_entries = 128;
constexpr int kAcceptTag = 1;

double percentile(const std::vector<double>& samples, double fraction) {
    std::vector<double> sorted(samples);
    std::sort(sorted.begin(), sorted.end());
    const auto index = static_cast<std::size_t>(fraction * static_cast<double>(sorted.size() - 1));
    return sorted[index];
}

void set_nonblocking(int fd) {
    const int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags < 0 || ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) != 0) {
        throw std::runtime_error("fcntl O_NONBLOCK failed");
    }
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
    std::array<char, buffer_size> buffer{};
    enum class Op { recv, send } op = Op::recv;
    std::size_t send_len = 0;
};

// One io_uring_enter coverage counter for a rough internal report.
std::atomic<std::uint64_t> enter_count{0};

class IouringEchoServer {
  public:
    explicit IouringEchoServer(int listener) : listener_(listener) {
        if (::io_uring_queue_init(ring_entries, &ring_, 0) != 0) {
            throw std::runtime_error("io_uring_queue_init failed");
        }
    }

    ~IouringEchoServer() { ::io_uring_queue_exit(&ring_); }

    void operator()() {
        if (!submit_accept()) {
            return;
        }
        std::unordered_map<int, std::unique_ptr<Endpoint>> endpoints;
        bool timeout_pending = false;

        while (running_.load(std::memory_order_relaxed)) {
            // A short in-ring timeout is the cross-thread stop wakeup.  The
            // ring itself is only touched by this server thread; calling
            // io_uring_get_sqe/submit from stop() races with this wait.
            if (!timeout_pending) {
                if (!submit_timeout()) {
                    break;
                }
                timeout_pending = true;
            }
            ++enter_count;
            const int wait_result = ::io_uring_submit_and_wait(&ring_, 1);
            if (wait_result < 0 && wait_result != -EINTR) {
                throw std::runtime_error("io_uring_submit_and_wait failed");
            }

            unsigned head = 0;
            unsigned count = 0;
            struct io_uring_cqe* cqe = nullptr;
            io_uring_for_each_cqe(&ring_, head, cqe) {
                const void* tag = ::io_uring_cqe_get_data(cqe);
                const int result = cqe->res;
                if (tag == reinterpret_cast<void*>(kAcceptTag)) {
                    (void)submit_accept();
                    if (result >= 0) {
                        auto endpoint = std::make_unique<Endpoint>(result);
                        Endpoint* const raw_endpoint = endpoint.get();
                        endpoints.emplace(result, std::move(endpoint));
                        if (!submit_recv(raw_endpoint)) {
                            endpoints.erase(result);
                        }
                    }
                } else if (tag == reinterpret_cast<void*>(kTimeoutTag)) {
                    timeout_pending = false;
                } else {
                    auto* const endpoint = static_cast<Endpoint*>(const_cast<void*>(tag));
                    const bool close_connection =
                        (endpoint->op == Endpoint::Op::recv && result <= 0) ||
                        (endpoint->op == Endpoint::Op::send && result < 0);
                    if (close_connection) {
                        endpoints.erase(endpoint->fd);
                    } else if (endpoint->op == Endpoint::Op::recv) {
                        endpoint->send_len = static_cast<std::size_t>(result);
                        if (!submit_send(endpoint)) {
                            endpoints.erase(endpoint->fd);
                        }
                    } else {
                        if (!submit_recv(endpoint)) {
                            endpoints.erase(endpoint->fd);
                        }
                    }
                }
                ++count;
            }
            ::io_uring_cq_advance(&ring_, count);
            if (!running_.load(std::memory_order_relaxed)) {
                break;
            }
            // Publish the newly prepared SQEs in one syscall.
            ++enter_count;
            const int submit_result = ::io_uring_submit(&ring_);
            if (submit_result < 0 && submit_result != -EINTR) {
                throw std::runtime_error("io_uring_submit failed");
            }
        }

        endpoints.clear();
    }

    // Safe to call from another thread. The server thread notices this flag
    // on the next short timeout completion and performs all ring cleanup.
    void request_stop() noexcept {
        running_.store(false, std::memory_order_relaxed);
    }

  private:
    bool submit_accept() noexcept {
        struct io_uring_sqe* sqe = ::io_uring_get_sqe(&ring_);
        if (sqe == nullptr) {
            running_.store(false, std::memory_order_relaxed);
            return false;
        }
        ::io_uring_prep_accept(sqe, listener_, nullptr, nullptr, 0);
        ::io_uring_sqe_set_data(sqe, reinterpret_cast<void*>(kAcceptTag));
        return true;
    }

    bool submit_recv(Endpoint* endpoint) noexcept {
        struct io_uring_sqe* sqe = ::io_uring_get_sqe(&ring_);
        if (sqe == nullptr) {
            running_.store(false, std::memory_order_relaxed);
            return false;
        }
        ::io_uring_prep_recv(sqe, endpoint->fd, endpoint->buffer.data(), buffer_size, 0);
        ::io_uring_sqe_set_data(sqe, endpoint);
        endpoint->op = Endpoint::Op::recv;
        return true;
    }

    bool submit_send(Endpoint* endpoint) noexcept {
        struct io_uring_sqe* sqe = ::io_uring_get_sqe(&ring_);
        if (sqe == nullptr) {
            running_.store(false, std::memory_order_relaxed);
            return false;
        }
        ::io_uring_prep_send(sqe, endpoint->fd, endpoint->buffer.data(), endpoint->send_len, 0);
        ::io_uring_sqe_set_data(sqe, endpoint);
        endpoint->op = Endpoint::Op::send;
        return true;
    }

    bool submit_timeout() noexcept {
        struct io_uring_sqe* sqe = ::io_uring_get_sqe(&ring_);
        if (sqe == nullptr) {
            running_.store(false, std::memory_order_relaxed);
            return false;
        }
        timeout_.tv_sec = 0;
        timeout_.tv_nsec = 100000000;
        ::io_uring_prep_timeout(sqe, &timeout_, 0, 0);
        ::io_uring_sqe_set_data(sqe, reinterpret_cast<void*>(kTimeoutTag));
        return true;
    }

    int listener_ = -1;
    std::atomic<bool> running_{true};
    struct io_uring ring_ {};
    struct __kernel_timespec timeout_ {};
    static constexpr int kTimeoutTag = 2;
};

} // namespace

int main(int argc, char** argv) {
    const std::size_t connections =
        argc > 1 ? static_cast<std::size_t>(std::strtoull(argv[1], nullptr, 10)) : 8U;
    const std::size_t rounds =
        argc > 2 ? static_cast<std::size_t>(std::strtoull(argv[2], nullptr, 10)) : 100000U;
    if (connections == 0 || rounds == 0 || argc > 3) {
        std::cerr << "usage: easy_uds_io_uring_echo_probe [connections] [rounds]\n";
        return 2;
    }

    const std::string path = "/tmp/easy-uds-io-uring-echo-" +
                             std::to_string(static_cast<long long>(::getpid())) + ".sock";
    const int listener = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (listener < 0) {
        throw std::runtime_error("socket failed");
    }
    set_nonblocking(listener);
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    std::strncpy(address.sun_path, path.c_str(), sizeof(address.sun_path) - 1);
    ::unlink(path.c_str());
    if (::bind(listener, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0 ||
        ::listen(listener, 128) != 0) {
        throw std::runtime_error("bind/listen failed");
    }

    IouringEchoServer server(listener);
    std::exception_ptr server_error;
    std::thread server_thread([&] {
        try {
            server();
        } catch (...) {
            server_error = std::current_exception();
        }
    });
    // Give the server's ring a moment; clients connect and the accept re-arms.

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
            int client = -1;
            bool announced = false;
            try {
                client = ::socket(AF_UNIX, SOCK_STREAM, 0);
                if (client < 0) {
                    throw std::runtime_error("client socket failed");
                }
                sockaddr_un client_address{};
                client_address.sun_family = AF_UNIX;
                std::strncpy(client_address.sun_path, path.c_str(), sizeof(client_address.sun_path) - 1);
                if (::connect(client, reinterpret_cast<const sockaddr*>(&client_address),
                              sizeof(client_address)) != 0) {
                    throw std::runtime_error("connect failed");
                }
                auto& samples = latency_samples[index];
                samples.reserve(rounds);
                ready.fetch_add(1, std::memory_order_relaxed);
                announced = true;
                std::array<char, payload_size> payload{};
                while (!start.load(std::memory_order_acquire) &&
                       !failed.load(std::memory_order_acquire)) {
                    std::this_thread::yield();
                }
                for (std::size_t round = 0;
                     round < rounds && !failed.load(std::memory_order_acquire); ++round) {
                    const auto begun = Clock::now();
                    if (::send(client, payload.data(), payload.size(), 0) !=
                            static_cast<ssize_t>(payload.size()) ||
                        ::recv(client, payload.data(), payload.size(), 0) !=
                            static_cast<ssize_t>(payload.size())) {
                        throw std::runtime_error("client exchange failed");
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
            if (client >= 0) {
                (void)::close(client);
            }
        });
    }
    while (ready.load(std::memory_order_relaxed) != connections) {
        std::this_thread::yield();
    }
    const auto wall_started = Clock::now();
    start.store(true, std::memory_order_release);
    for (auto& client : clients) {
        client.join();
    }
    const double wall_seconds = std::chrono::duration<double>(Clock::now() - wall_started).count();

    server.request_stop();
    server_thread.join();
    (void)::close(listener);
    ::unlink(path.c_str());
    if (server_error) {
        std::rethrow_exception(server_error);
    }
    if (client_error) {
        std::rethrow_exception(client_error);
    }

    std::vector<double> all;
    all.reserve(connections * rounds);
    for (const auto& samples : latency_samples) {
        all.insert(all.end(), samples.begin(), samples.end());
    }
    const std::size_t total = connections * rounds;
    std::cout << "connections=" << connections << ", rounds=" << rounds << ", total=" << total << '\n'
              << "throughput: " << static_cast<double>(total) / wall_seconds << " req/s\n"
              << "latency:    p50=" << percentile(all, 0.50) << " us, p95=" << percentile(all, 0.95)
              << " us, p99=" << percentile(all, 0.99) << " us\n"
              << "io_uring_enter=" << enter_count.load() << " (~"
              << static_cast<double>(enter_count.load()) / static_cast<double>(total) << "/req)\n";
    return 0;
}
