#include "easy_uds/easy_uds.hpp"

#include <algorithm>
#include <chrono>
#include <exception>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;

void remove_endpoint(const std::filesystem::path& path) {
    std::error_code ignored;
    std::filesystem::remove(path, ignored);
    std::filesystem::remove(path.string() + ".lock", ignored);
}

void wait_until_running(const easy_uds::Server& server) {
    const auto deadline = Clock::now() + std::chrono::seconds(5);
    while (!server.is_running() && Clock::now() < deadline) {
        std::this_thread::yield();
    }
    if (!server.is_running()) {
        throw std::runtime_error("Windows benchmark server did not become ready");
    }
}

double percentile(std::vector<double> samples, double fraction) {
    std::sort(samples.begin(), samples.end());
    const auto index = static_cast<std::size_t>(
        fraction * static_cast<double>(samples.size() - 1));
    return samples[index];
}

void print_result(const char* workload, const std::vector<double>& samples,
                  std::chrono::steady_clock::duration elapsed) {
    const double seconds = std::chrono::duration<double>(elapsed).count();
    const double throughput = static_cast<double>(samples.size()) / seconds;
    std::cout << workload << ": requests=" << samples.size()
              << ", throughput=" << std::fixed << std::setprecision(1)
              << throughput << " req/s, p50=" << percentile(samples, 0.50)
              << " us, p99=" << percentile(samples, 0.99) << " us\n";
}

} // namespace

int main() {
    const auto path = std::filesystem::temp_directory_path() /
                      "easy-uds-windows-performance.sock";
    remove_endpoint(path);

    std::exception_ptr server_error;
    std::unique_ptr<easy_uds::Server> server;
    std::thread server_thread;
    try {
        easy_uds::ServerOptions options;
        options.worker_threads = 4;
        options.max_connections = 32;
        options.stale_socket_grace_period = std::chrono::milliseconds{0};
        server = std::make_unique<easy_uds::Server>(path.string(), options);
        server->on("/ping", [](const easy_uds::Request&) {
            return easy_uds::Response::ok("pong");
        });
        server_thread = std::thread([&] {
            try {
                server->run();
            } catch (...) {
                server_error = std::current_exception();
            }
        });
        wait_until_running(*server);

        easy_uds::Client client(path.string());
        for (int index = 0; index < 100; ++index) {
            (void)client.request("/ping");
        }
        std::vector<double> one_shot;
        one_shot.reserve(1000);
        const auto one_shot_start = Clock::now();
        for (int index = 0; index < 1000; ++index) {
            const auto started = Clock::now();
            const auto response = client.request("/ping");
            if (response.status != 200 || response.body != "pong") {
                throw std::runtime_error("Windows one-shot benchmark response mismatch");
            }
            one_shot.push_back(
                std::chrono::duration<double, std::micro>(Clock::now() - started).count());
        }
        print_result("one-shot c1", one_shot, Clock::now() - one_shot_start);

        auto session = client.session();
        for (int index = 0; index < 100; ++index) {
            (void)session.request("/ping");
        }
        constexpr std::size_t concurrency = 8;
        constexpr std::size_t requests_per_worker = 250;
        std::vector<std::vector<double>> session_samples(concurrency);
        std::vector<std::exception_ptr> worker_errors(concurrency);
        std::vector<std::thread> workers;
        workers.reserve(concurrency);
        const auto session_start = Clock::now();
        for (std::size_t worker = 0; worker < concurrency; ++worker) {
            workers.emplace_back([&, worker] {
                try {
                    auto& samples = session_samples[worker];
                    samples.reserve(requests_per_worker);
                    for (std::size_t index = 0; index < requests_per_worker; ++index) {
                        const auto started = Clock::now();
                        const auto response = session.request("/ping");
                        if (response.status != 200 || response.body != "pong") {
                            throw std::runtime_error(
                                "Windows Session benchmark response mismatch");
                        }
                        samples.push_back(std::chrono::duration<double, std::micro>(
                                              Clock::now() - started)
                                              .count());
                    }
                } catch (...) {
                    worker_errors[worker] = std::current_exception();
                }
            });
        }
        for (auto& worker : workers) {
            worker.join();
        }
        for (const auto& error : worker_errors) {
            if (error) {
                std::rethrow_exception(error);
            }
        }
        std::vector<double> shared_session;
        shared_session.reserve(concurrency * requests_per_worker);
        for (auto& samples : session_samples) {
            shared_session.insert(shared_session.end(), samples.begin(), samples.end());
        }
        print_result("shared Session c8", shared_session, Clock::now() - session_start);

        server->stop();
        server_thread.join();
        if (server_error) {
            std::rethrow_exception(server_error);
        }
        remove_endpoint(path);
        return 0;
    } catch (const std::exception& error) {
        if (server) {
            server->stop();
        }
        if (server_thread.joinable()) {
            server_thread.join();
        }
        remove_endpoint(path);
        std::cerr << "windows_benchmark: " << error.what() << '\n';
        return 1;
    }
}
