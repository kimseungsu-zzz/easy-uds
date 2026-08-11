#include "easy_uds/easy_uds.hpp"

#include <atomic>
#include <array>
#include <chrono>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <unistd.h>

namespace {

using namespace std::chrono_literals;

void expect(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

void wait_until_running(const easy_uds::Server& server) {
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while (!server.is_running()) {
        if (std::chrono::steady_clock::now() >= deadline) {
            throw std::runtime_error("server did not start");
        }
        std::this_thread::yield();
    }
}

std::string socket_path(const char* suffix) {
    return "/tmp/easy-uds-stress-" + std::to_string(static_cast<long long>(::getpid())) + "-" + suffix + ".sock";
}

void cleanup(const std::string& path) {
    (void)::unlink(path.c_str());
    (void)::unlink((path + ".lock").c_str());
}

// Concurrent one-shot and multiplexed-session load.
void run_concurrent_load() {
    using namespace easy_uds;

    const std::string path = socket_path("load");
    ServerOptions options;
    options.worker_threads = 8;
    options.max_connections = 512;
    options.io_timeout = 2s;
    options.request_timeout = 5s;
    Server server(path, options);
    server.on("echo", [](const Request& request) { return Response{200, request.body}; });
    server.on_stream("up", [](const StreamReader& body, const Request&) {
        std::array<char, 2048> buffer{};
        while (body(buffer.data(), buffer.size()) != 0) {
        }
        return StreamResponse{200, {}};
    });

    std::thread server_thread([&] { server.run(); });
    wait_until_running(server);

    Client client(path);

    std::atomic<std::size_t> successes{0};
    std::atomic<bool> failed{false};
    std::mutex failure_mutex;
    std::string first_failure;
    const auto record_failure = [&](std::string message) {
        failed.store(true);
        std::lock_guard<std::mutex> lock(failure_mutex);
        if (first_failure.empty()) {
            first_failure = std::move(message);
        }
    };

    // 8 one-shot workers.
    std::vector<std::thread> one_shot;
    for (int w = 0; w < 8; ++w) {
        one_shot.emplace_back([&, w] {
            int iteration = -1;
            try {
                for (iteration = 0; iteration < 500; ++iteration) {
                    const std::string body = "s" + std::to_string(w) + "-" + std::to_string(iteration);
                    const Response response = client.request("echo", body);
                    if (response.status != 200 || response.body != body) {
                        record_failure("one-shot response mismatch at worker " + std::to_string(w) +
                                       ", iteration " + std::to_string(iteration));
                        return;
                    }
                }
                successes.fetch_add(1);
            } catch (const std::exception& error) {
                record_failure(std::string{"one-shot error at worker "} + std::to_string(w) + ", iteration " +
                               std::to_string(iteration) + ": " + error.what());
            } catch (...) {
                record_failure("one-shot non-standard exception");
            }
        });
    }

    // 4 multiplexed session workers.
    Session session = client.session();
    std::vector<std::thread> sess;
    for (int w = 0; w < 4; ++w) {
        sess.emplace_back([&, w] {
            try {
                for (int i = 0; i < 500; ++i) {
                    const std::string body = "m" + std::to_string(w) + "-" + std::to_string(i);
                    const Response response = session.request("echo", body);
                    if (response.status != 200 || response.body != body) {
                        record_failure("session response mismatch");
                        return;
                    }
                }
                successes.fetch_add(1);
            } catch (const std::exception& error) {
                record_failure(std::string{"session error: "} + error.what());
            } catch (...) {
                record_failure("session non-standard exception");
            }
        });
    }

    for (auto& thread : one_shot) {
        thread.join();
    }
    for (auto& thread : sess) {
        thread.join();
    }

    // A couple of streams interleaved.
    for (int i = 0; i < 20; ++i) {
        StreamReader up = [r = std::size_t{256 * 1024}](char*, std::size_t c) mutable {
            const std::size_t take = std::min(c, r);
            if (take == 0) return std::size_t{0};
            r -= take;
            return take;
        };
        const Status status = client.request_stream("up", up, {});
        if (status != 200) {
            record_failure("stream status mismatch");
        }
    }

    server.stop();
    server_thread.join();
    cleanup(path);

    if (failed.load()) {
        throw std::runtime_error("no request may fail under concurrent load: " + first_failure);
    }
    expect(successes.load() >= 12, "every client worker should complete");
    std::cout << "  concurrent load: ok (12 workers, " << successes.load() << " completed)\n";
}

// Many threads race stop() with live traffic.
void run_concurrent_stop() {
    using namespace easy_uds;

    const std::string path = socket_path("stop");
    ServerOptions options;
    options.worker_threads = 4;
    options.max_connections = 256;
    options.io_timeout = 2s;
    Server server(path, options);
    server.on("ping", [](const Request&) { return Response{200, "pong"}; });

    std::thread server_thread([&] { server.run(); });
    wait_until_running(server);

    std::atomic<bool> stop_requested{false};
    std::vector<std::thread> clients;
    for (int i = 0; i < 8; ++i) {
        clients.emplace_back([&] {
            try {
                Client client(path);
                while (!stop_requested.load()) {
                    (void)client.request("ping");
                }
            } catch (...) {
                // Requests racing shutdown may legitimately fail.
            }
        });
    }

    std::this_thread::sleep_for(200ms);
    std::vector<std::thread> stoppers;
    for (int i = 0; i < 4; ++i) {
        stoppers.emplace_back([&] { server.stop(); });
    }
    stop_requested.store(true);
    for (auto& thread : stoppers) {
        thread.join();
    }
    for (auto& thread : clients) {
        thread.join();
    }
    server_thread.join();
    cleanup(path);
    std::cout << "  concurrent stop: ok (4 stoppers, 8 clients)\n";
}

} // namespace

int main() {
    try {
        run_concurrent_load();
        run_concurrent_stop();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    std::cout << "Stress test passed.\n";
    return 0;
}
