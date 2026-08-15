#ifdef EASY_UDS_SIMPLE_BENCHMARK
#include "easy_uds/simple.hpp"
#else
#include "easy_uds/easy_uds.hpp"
#endif

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <unistd.h>
#include <sys/resource.h>

namespace {

using Clock = std::chrono::steady_clock;

double percentile(const std::vector<double>& samples, double fraction) {
    return samples[static_cast<std::size_t>(
        fraction * static_cast<double>(samples.size() - 1))];
}

#ifdef EASY_UDS_SIMPLE_BENCHMARK
using Server = easy_uds::simple::Server;
using Client = easy_uds::simple::Client;
#else
using Server = easy_uds::Server;
using Client = easy_uds::Client;
#endif

void wait_until_running(const Server& server) {
    while (!server.is_running()) {
        std::this_thread::yield();
    }
}

}  // namespace

int main(int argc, char** argv) {
    const std::size_t iterations =
        argc > 1 ? std::strtoull(argv[1], nullptr, 10) : 30000U;
    const std::size_t concurrency =
        argc > 2 ? std::strtoull(argv[2], nullptr, 10) : 1U;
    if (iterations == 0 || concurrency == 0 || argc > 3) {
        std::cerr << "usage: simple/core_api_benchmark [iterations] [concurrency]\n";
        return 2;
    }

    const std::string mode =
#ifdef EASY_UDS_SIMPLE_BENCHMARK
        "simple";
#else
        "core";
#endif
    const std::string path = "/tmp/easy-uds-" + mode + "-api-benchmark-" +
                             std::to_string(static_cast<long long>(::getpid())) +
                             ".sock";
    easy_uds::ServerOptions options;
    options.worker_threads = std::max<std::size_t>(1, concurrency);
    options.max_connections = std::max<std::size_t>(64, concurrency * 2);
    options.stale_socket_grace_period = std::chrono::milliseconds{0};
    Server server(path, options);
#ifdef EASY_UDS_SIMPLE_BENCHMARK
    server.on("/echo") = [](std::string_view body) {
        return std::string(body);
    };
#else
    server.on("/echo", [](const easy_uds::Request& request) {
        return easy_uds::Response::ok(request.body);
    });
#endif

    std::exception_ptr server_error;
    std::thread server_thread([&] {
        try {
            server.run();
        } catch (...) {
            server_error = std::current_exception();
        }
    });
    wait_until_running(server);
    Client client(path);
    for (std::size_t index = 0; index < 1000; ++index) {
#ifdef EASY_UDS_SIMPLE_BENCHMARK
        (void)client.request("/echo", "warmup");
#else
        (void)client.request("/echo", "warmup");
#endif
    }

    std::atomic<std::size_t> ready{0};
    std::atomic<bool> start{false};
    std::atomic<bool> failed{false};
    std::vector<std::vector<double>> local_samples(concurrency);
    std::vector<std::thread> workers;
    workers.reserve(concurrency);
    for (std::size_t worker = 0; worker < concurrency; ++worker) {
        const std::size_t count =
            iterations / concurrency + (worker < iterations % concurrency ? 1 : 0);
        workers.emplace_back([&, worker, count] {
            auto& samples = local_samples[worker];
            samples.reserve(count);
            ready.fetch_add(1, std::memory_order_release);
            while (!start.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            try {
                for (std::size_t index = 0; index < count; ++index) {
                    const auto begin = Clock::now();
                    const auto response = client.request("/echo", "hello");
                    samples.push_back(std::chrono::duration<double, std::micro>(
                                          Clock::now() - begin)
                                          .count());
#ifdef EASY_UDS_SIMPLE_BENCHMARK
                    if (response != "hello") {
#else
                    if (response.status != easy_uds::status_ok ||
                        response.body != "hello") {
#endif
                        failed.store(true, std::memory_order_relaxed);
                        return;
                    }
                }
            } catch (...) {
                failed.store(true, std::memory_order_relaxed);
            }
        });
    }
    while (ready.load(std::memory_order_acquire) != concurrency) {
        std::this_thread::yield();
    }
    rusage usage_before{};
    if (::getrusage(RUSAGE_SELF, &usage_before) != 0) {
        return 1;
    }
    const auto begin = Clock::now();
    start.store(true, std::memory_order_release);
    for (auto& worker : workers) {
        worker.join();
    }
    const auto elapsed = std::chrono::duration<double>(Clock::now() - begin).count();
    rusage usage_after{};
    if (::getrusage(RUSAGE_SELF, &usage_after) != 0) {
        return 1;
    }
    server.stop();
    server_thread.join();
    (void)::unlink(path.c_str());
    (void)::unlink((path + ".lock").c_str());
    if (server_error) {
        std::rethrow_exception(server_error);
    }
    if (failed.load(std::memory_order_relaxed)) {
        std::cerr << "request failed\n";
        return 1;
    }

    std::vector<double> samples;
    samples.reserve(iterations);
    for (auto& local : local_samples) {
        samples.insert(samples.end(), local.begin(), local.end());
    }
    std::sort(samples.begin(), samples.end());
    double total = 0.0;
    for (double sample : samples) {
        total += sample;
    }
    const auto user_us =
        (usage_after.ru_utime.tv_sec - usage_before.ru_utime.tv_sec) * 1000000LL +
        usage_after.ru_utime.tv_usec - usage_before.ru_utime.tv_usec;
    const auto system_us =
        (usage_after.ru_stime.tv_sec - usage_before.ru_stime.tv_sec) * 1000000LL +
        usage_after.ru_stime.tv_usec - usage_before.ru_stime.tv_usec;
    const auto cpu_us = user_us + system_us;
    std::cout << "api=" << mode << ", requests=" << iterations
              << ", concurrency=" << concurrency << '\n'
              << "throughput: " << static_cast<double>(iterations) / elapsed
              << " requests/s\n"
              << "latency:    avg=" << total / samples.size()
              << " us, p50=" << percentile(samples, 0.50)
              << " us, p99=" << percentile(samples, 0.99) << " us\n"
              << "cpu:        "
              << static_cast<double>(cpu_us) / static_cast<double>(iterations)
              << " CPU-s/1M requests\n";
    return 0;
}
