#include "easy_uds/easy_uds.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <limits>
#include <string>
#include <thread>
#include <vector>

#include <unistd.h>

namespace {

using Clock = std::chrono::steady_clock;

void wait_until_running(const easy_uds::Server& server) {
    while (!server.is_running()) {
        std::this_thread::yield();
    }
}

double percentile(const std::vector<double>& sorted_samples, double fraction) {
    const auto index = static_cast<std::size_t>(fraction * static_cast<double>(sorted_samples.size() - 1));
    return sorted_samples[index];
}

} // namespace

int main(int argc, char** argv) {
    const std::size_t iterations =
        argc > 1 ? static_cast<std::size_t>(std::strtoull(argv[1], nullptr, 10)) : 100000U;
    const std::size_t concurrency =
        argc > 2 ? static_cast<std::size_t>(std::strtoull(argv[2], nullptr, 10)) : 1U;
    const std::size_t payload =
        argc > 3 ? static_cast<std::size_t>(std::strtoull(argv[3], nullptr, 10)) : 0U;
    if (iterations == 0 || concurrency == 0) {
        std::cerr << "iterations and concurrency must be greater than zero\n";
        return 2;
    }
    if (argc > 4) {
        std::cerr << "usage: easy_uds_rpc_benchmark [iterations] [concurrency] [payload_bytes]\n";
        return 2;
    }
    const std::string path =
        "/tmp/easy-uds-rpc-benchmark-" + std::to_string(static_cast<long long>(::getpid())) + ".sock";

    // The route + body must fit max_message_size, so a large payload raises
    // the cap above the default 1 MiB. The handler echoes the payload so both
    // request and response frames exercise large-body parsing.
    const std::size_t message_limit =
        std::max<std::size_t>(easy_uds::default_max_message_size, payload + 8);
    easy_uds::ServerOptions server_options;
    if (concurrency > std::numeric_limits<std::size_t>::max() / 2) {
        std::cerr << "concurrency is too large\n";
        return 2;
    }
    server_options.max_connections = std::max<std::size_t>(64, concurrency * 2);
    server_options.worker_threads = std::max<std::size_t>(1, concurrency);
    server_options.listen_backlog = static_cast<int>(
        std::min<std::size_t>(server_options.max_connections, static_cast<std::size_t>(std::numeric_limits<int>::max())));
    server_options.stale_socket_grace_period = std::chrono::milliseconds{0};
    server_options.max_message_size = message_limit;
    easy_uds::Server server(path, server_options);
    server.on("echo", [payload](const easy_uds::Request&) {
        return easy_uds::Response{200, std::string(payload, 'y')};
    });

    std::exception_ptr server_error;
    std::thread server_thread([&] {
        try {
            server.run();
        } catch (...) {
            server_error = std::current_exception();
        }
    });
    wait_until_running(server);

    easy_uds::ClientOptions client_options;
    client_options.max_message_size = message_limit;
    easy_uds::Client client(path, client_options);
    const std::string request_body(payload, 'x');
    const std::size_t warmup = payload == 0 ? 1000U : 10U;
    for (std::size_t index = 0; index < warmup; ++index) {
        (void)client.request("echo", request_body);
    }

    std::atomic<std::size_t> ready{0};
    std::atomic<bool> start_workers{false};
    std::atomic<bool> failed{false};
    std::vector<std::vector<double>> latency_samples(concurrency);
    std::vector<std::thread> workers;
    workers.reserve(concurrency);
    for (std::size_t worker = 0; worker < concurrency; ++worker) {
        const std::size_t worker_iterations = iterations / concurrency + (worker < iterations % concurrency ? 1 : 0);
        workers.emplace_back([&, worker, worker_iterations] {
            auto& samples = latency_samples[worker];
            samples.reserve(worker_iterations);
            ready.fetch_add(1, std::memory_order_relaxed);
            while (!start_workers.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            try {
                for (std::size_t index = 0; index < worker_iterations; ++index) {
                    const auto request_started = Clock::now();
                    const auto response = client.request("echo", request_body);
                    samples.push_back(
                        std::chrono::duration<double, std::micro>(Clock::now() - request_started).count());
                    if (response.status != 200 || response.body.size() != payload) {
                        failed.store(true, std::memory_order_relaxed);
                        return;
                    }
                }
            } catch (...) {
                failed.store(true, std::memory_order_relaxed);
            }
        });
    }
    while (ready.load(std::memory_order_relaxed) != concurrency) {
        std::this_thread::yield();
    }
    const auto start = Clock::now();
    start_workers.store(true, std::memory_order_release);
    for (auto& worker : workers) {
        worker.join();
    }
    const auto elapsed = Clock::now() - start;

    server.stop();
    server_thread.join();
    (void)::unlink((path + ".lock").c_str());
    if (server_error) {
        std::rethrow_exception(server_error);
    }
    if (failed.load(std::memory_order_relaxed)) {
        std::cerr << "request failed\n";
        return 1;
    }

    const double seconds = std::chrono::duration<double>(elapsed).count();
    const double requests_per_second = static_cast<double>(iterations) / seconds;
    std::vector<double> samples;
    samples.reserve(iterations);
    for (auto& worker_samples : latency_samples) {
        samples.insert(samples.end(), worker_samples.begin(), worker_samples.end());
    }
    std::sort(samples.begin(), samples.end());
    double latency_sum = 0.0;
    for (const double sample : samples) {
        latency_sum += sample;
    }
    std::cout << "requests=" << iterations << ", concurrency=" << concurrency << " (one-shot)\n"
              << "throughput: " << requests_per_second << " requests/s\n"
              << "latency:    avg=" << latency_sum / static_cast<double>(samples.size())
              << " us, p50=" << percentile(samples, 0.50) << " us, p95=" << percentile(samples, 0.95)
              << " us, p99=" << percentile(samples, 0.99) << " us\n";
    return 0;
}
