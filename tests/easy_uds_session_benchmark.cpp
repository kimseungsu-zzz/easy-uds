#include "easy_uds/easy_uds.hpp"

#ifdef EASY_UDS_TRACE_SESSION_CONTENTION
#include "../src/session_trace.hpp"
#endif

#include <atomic>

#ifdef EASY_UDS_TRACE_SPIN_MISS
// Defined by the easy_uds library when built with EASY_UDS_TRACE_SPIN_MISS.
namespace easy_uds::detail {
extern std::atomic<std::size_t> session_spin_miss_count;
}
#endif

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <unistd.h>
#include <sys/resource.h>

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
    const bool shared_session = argc > 3 && std::string_view(argv[3]) == "shared";
    // Optional server continuation grace in milliseconds (0 disables the
    // worker-lease continuation fast path; an A/B for hot-path attribution).
    const long long grace_ms = argc > 4 ? std::strtoll(argv[4], nullptr, 10) : 1;
    if (iterations == 0 || concurrency == 0 || argc > 5 || (argc == 4 && !shared_session) ||
        grace_ms < 0) {
        std::cerr << "usage: easy_uds_session_benchmark [iterations] [concurrency] [shared] [grace_ms]\n";
        return 2;
    }
    const std::string path =
        "/tmp/easy-uds-session-benchmark-" + std::to_string(static_cast<long long>(::getpid())) + ".sock";

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
    server_options.session_idle_grace = std::chrono::milliseconds{grace_ms};
    easy_uds::Server server(path, server_options);
    server.on("ping", [](const easy_uds::Request&) { return easy_uds::Response{200, "pong"}; });

    std::exception_ptr server_error;
    std::thread server_thread([&] {
        try {
            server.run();
        } catch (...) {
            server_error = std::current_exception();
        }
    });
    wait_until_running(server);

    easy_uds::Client client(path);
    std::unique_ptr<easy_uds::Session> shared;
    if (shared_session) {
        shared = std::make_unique<easy_uds::Session>(client.session());
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
            // Connection setup happens before the start barrier. By default
            // each worker owns a session; `shared` benchmarks multiplexing
            // contention on one session instead.
            std::unique_ptr<easy_uds::Session> local;
            if (!shared_session) {
                local = std::make_unique<easy_uds::Session>(client.session());
            }
            easy_uds::Session& session = shared_session ? *shared : *local;
            ready.fetch_add(1, std::memory_order_relaxed);
            while (!start_workers.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            try {
                for (std::size_t index = 0; index < worker_iterations; ++index) {
                    const auto request_started = Clock::now();
                    const auto response = session.request("ping");
                    samples.push_back(
                        std::chrono::duration<double, std::micro>(Clock::now() - request_started).count());
                    if (response.status != 200 || response.body != "pong") {
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
#ifdef EASY_UDS_TRACE_SESSION_CONTENTION
    easy_uds::detail::session_trace_counters.reset();
#endif
    rusage usage_before{};
    if (::getrusage(RUSAGE_SELF, &usage_before) != 0) {
        std::cerr << "getrusage failed\n";
        return 1;
    }
    const auto start = Clock::now();
    start_workers.store(true, std::memory_order_release);
    for (auto& worker : workers) {
        worker.join();
    }
    const auto elapsed = Clock::now() - start;
    rusage usage_after{};
    if (::getrusage(RUSAGE_SELF, &usage_after) != 0) {
        std::cerr << "getrusage failed\n";
        return 1;
    }

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
    const auto user_microseconds =
        (usage_after.ru_utime.tv_sec - usage_before.ru_utime.tv_sec) * 1000000LL +
        usage_after.ru_utime.tv_usec - usage_before.ru_utime.tv_usec;
    const auto system_microseconds =
        (usage_after.ru_stime.tv_sec - usage_before.ru_stime.tv_sec) * 1000000LL +
        usage_after.ru_stime.tv_usec - usage_before.ru_stime.tv_usec;
    const auto cpu_microseconds = user_microseconds + system_microseconds;
    const double cpu_seconds_per_million_requests =
        static_cast<double>(cpu_microseconds) / static_cast<double>(iterations);
    const double requests_per_cpu_second =
        cpu_microseconds == 0
            ? 0.0
            : static_cast<double>(iterations) * 1000000.0 / static_cast<double>(cpu_microseconds);
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
    std::cout << "requests=" << iterations << ", concurrency=" << concurrency
              << (shared_session ? " (one shared session)\n" : " (independent persistent sessions)\n")
              << "throughput: " << requests_per_second << " requests/s\n"
              << "latency:    avg=" << latency_sum / static_cast<double>(samples.size())
              << " us, p50=" << percentile(samples, 0.50) << " us, p90=" << percentile(samples, 0.90)
              << " us, p95=" << percentile(samples, 0.95) << " us, p99=" << percentile(samples, 0.99)
              << " us, p99.9=" << percentile(samples, 0.999) << " us, max=" << samples.back() << " us\n"
              << "resources:  user=" << user_microseconds << " us, system=" << system_microseconds
              << " us, voluntary_cs=" << usage_after.ru_nvcsw - usage_before.ru_nvcsw
              << ", involuntary_cs=" << usage_after.ru_nivcsw - usage_before.ru_nivcsw << '\n'
              << "efficiency: " << cpu_seconds_per_million_requests << " CPU-s/1M requests, "
              << requests_per_cpu_second << " requests/CPU-s\n";
#ifdef EASY_UDS_TRACE_SPIN_MISS
    const std::size_t spin_misses =
        easy_uds::detail::session_spin_miss_count.load(std::memory_order_relaxed);
    std::cout << "spin:       misses=" << spin_misses << "/" << iterations << " ("
              << 100.0 * static_cast<double>(spin_misses) / static_cast<double>(iterations)
              << "% condvar fallback)\n";
#endif
#ifdef EASY_UDS_TRACE_SESSION_CONTENTION
    const auto& trace = easy_uds::detail::session_trace_counters;
    const auto print_lock = [](const char* name, const std::atomic<std::uint64_t>& wait_ns,
                               const std::atomic<std::uint64_t>& acquisitions) {
        const auto total = wait_ns.load(std::memory_order_relaxed);
        const auto count = acquisitions.load(std::memory_order_relaxed);
        const double average = count == 0 ? 0.0 : static_cast<double>(total) / static_cast<double>(count);
        std::cout << "  " << name << ": total=" << static_cast<double>(total) / 1000.0
                  << " us, acquisitions=" << count << ", avg=" << average << " ns\n";
    };
    std::cout << "contention:\n";
    print_lock("send lock", trace.send_lock_wait_ns, trace.send_lock_acquisitions);
    print_lock("caller table lock", trace.caller_table_lock_wait_ns,
               trace.caller_table_lock_acquisitions);
    print_lock("reader table lock", trace.reader_table_lock_wait_ns,
               trace.reader_table_lock_acquisitions);
    print_lock("reader slot lock", trace.reader_slot_lock_wait_ns,
               trace.reader_slot_lock_acquisitions);
    print_lock("waiter slot lock", trace.waiter_slot_lock_wait_ns,
               trace.waiter_slot_lock_acquisitions);
    std::cout << "  request-id probes=" << trace.request_id_probes.load(std::memory_order_relaxed)
              << ", response lookups=" << trace.response_lookups.load(std::memory_order_relaxed)
              << ", notifications=" << trace.waiter_notifications.load(std::memory_order_relaxed)
              << ", condvar waits=" << trace.condition_waits.load(std::memory_order_relaxed) << '\n';
#endif
    return 0;
}
