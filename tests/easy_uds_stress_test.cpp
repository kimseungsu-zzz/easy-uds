#include "easy_uds/easy_uds.hpp"

#include <atomic>
#include <chrono>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

#include <unistd.h>

namespace {

using namespace std::chrono_literals;

std::string socket_path() {
    return "/tmp/easy-uds-stress-" + std::to_string(static_cast<long long>(::getpid())) + ".sock";
}

void wait_until_running(const easy_uds::Server& server) {
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while (!server.is_running()) {
        if (std::chrono::steady_clock::now() >= deadline) {
            throw std::runtime_error("stress test: server did not enter running state");
        }
        std::this_thread::yield();
    }
}

} // namespace

int main() {
    using namespace easy_uds;

    const std::string path = socket_path();
    const std::string lock_path = path + ".lock";

    try {
        for (int iteration = 0; iteration < 40; ++iteration) {
            ServerOptions server_options;
            server_options.worker_threads = 3;
            server_options.max_connections = 24;
            server_options.io_timeout = 250ms;
            server_options.request_timeout = 1000ms;
            server_options.stale_socket_grace_period = 0ms;

            Server server(path, server_options);
            server.on("ping", [](const Request&) { return Response{200, "pong"}; });

            std::exception_ptr run_error;
            std::thread run_thread([&] {
                try {
                    server.run();
                } catch (...) {
                    run_error = std::current_exception();
                }
            });
            wait_until_running(server);

            ClientOptions client_options;
            client_options.connect_timeout = 100ms;
            client_options.io_timeout = 100ms;
            client_options.request_timeout = 250ms;
            Client client(path, client_options);

            std::atomic<bool> keep_running{true};
            std::vector<std::thread> clients;
            for (int index = 0; index < 8; ++index) {
                clients.emplace_back([&] {
                    while (keep_running.load(std::memory_order_relaxed)) {
                        try {
                            const Response response = client.request("ping");
                            if (response.status_code != 200 || response.body != "pong") {
                                std::terminate();
                            }
                        } catch (const std::system_error&) {
                            // Expected while stop() races with connect/read/write.
                        }
                    }
                });
            }

            std::this_thread::sleep_for(3ms);
            const auto stop_started = std::chrono::steady_clock::now();
            std::vector<std::thread> stoppers;
            for (int index = 0; index < 4; ++index) {
                stoppers.emplace_back([&] { server.stop(); });
            }
            for (auto& stopper : stoppers) {
                stopper.join();
            }

            // Keep the client flood alive until run() has observed the wakeup.
            // This catches an unbounded accept-drain loop starving stop().
            run_thread.join();
            const auto stop_elapsed = std::chrono::steady_clock::now() - stop_started;

            keep_running.store(false, std::memory_order_relaxed);
            for (auto& client_thread : clients) {
                client_thread.join();
            }

            if (stop_elapsed > 2s) {
                throw std::runtime_error("stress test: stop latency exceeded two seconds during connection flood");
            }

            if (run_error) {
                std::rethrow_exception(run_error);
            }
            if (server.is_running()) {
                throw std::runtime_error("stress test: server remained running after stop");
            }
        }
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        (void)::unlink(path.c_str());
        (void)::unlink(lock_path.c_str());
        return 1;
    }

    (void)::unlink(path.c_str());
    (void)::unlink(lock_path.c_str());
    std::cout << "Stress test passed.\n";
    return 0;
}
