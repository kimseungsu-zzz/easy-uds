#include "easy_uds/easy_uds.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <iostream>
#include <new>
#include <stdexcept>
#include <string>
#include <thread>

#include <unistd.h>

namespace {

std::atomic<bool> track_allocations{false};
std::atomic<std::size_t> allocation_count{0};

void* allocate(std::size_t size) {
    if (track_allocations.load(std::memory_order_relaxed)) {
        allocation_count.fetch_add(1, std::memory_order_relaxed);
    }
    if (void* pointer = std::malloc(size == 0 ? 1 : size)) {
        return pointer;
    }
    throw std::bad_alloc{};
}

} // namespace

void* operator new(std::size_t size) { return allocate(size); }
void* operator new[](std::size_t size) { return allocate(size); }
void operator delete(void* pointer) noexcept { std::free(pointer); }
void operator delete[](void* pointer) noexcept { std::free(pointer); }
void operator delete(void* pointer, std::size_t) noexcept { std::free(pointer); }
void operator delete[](void* pointer, std::size_t) noexcept { std::free(pointer); }

int main(int argc, char** argv) {
    const std::size_t iterations =
        argc > 1 ? static_cast<std::size_t>(std::strtoull(argv[1], nullptr, 10)) : 20000U;
    const std::string mode = argc > 2 ? argv[2] : "";
    const std::size_t payload_bytes =
        argc > 3 ? static_cast<std::size_t>(std::strtoull(argv[3], nullptr, 10)) : 1024U * 1024U;
    const bool stream_mode = mode == "stream";
    const bool serialized_mode = mode == "serialized";
    const bool domain_mode = mode == "domain";
    if (iterations == 0 ||
        (!mode.empty() && !stream_mode && !serialized_mode && !domain_mode) ||
        argc > 4) {
        std::cerr << "usage: easy_uds_allocation_benchmark "
                     "[iterations] [stream|serialized|domain] [payload_bytes]\n";
        return 2;
    }

    const std::string path = "/tmp/easy-uds-allocation-benchmark-" +
                             std::to_string(static_cast<long long>(::getpid())) + ".sock";
    easy_uds::ServerOptions options;
    options.worker_threads = 1;
    easy_uds::Server server(path, options);
    const std::array<char, 256> handler_state{};
    server.on("ping", [handler_state](const easy_uds::Request&) {
        return easy_uds::Response{handler_state[0] == 0 ? 200 : 500, "pong"};
    });
    server.on_serialized("serial", [](const easy_uds::Request&) {
        return easy_uds::Response{200, "ok"};
    });
    server.on(
        "domain",
        easy_uds::RouteOptions{[](const easy_uds::Request&) {
            return easy_uds::Response{200, "ok"};
        }}.serialize_in("robot.drivetrain.primary-command-domain"));
    server.on_stream("upload", [](const easy_uds::StreamReader& body, const easy_uds::Request&) {
        std::array<char, 64U * 1024U> buffer{};
        while (body(buffer.data(), buffer.size())) {
        }
        return easy_uds::StreamResponse{200, {}};
    });

    std::exception_ptr server_error;
    std::thread server_thread([&] {
        try {
            server.run();
        } catch (...) {
            server_error = std::current_exception();
        }
    });
    while (!server.is_running()) {
        std::this_thread::yield();
    }

    easy_uds::Client client(path);
    easy_uds::Session session = client.session();
    const std::string request_body(payload_bytes, 'x');

    // Warm allocator bins, route lookup, and server state so the counted
    // region measures only steady-state allocation.
    if (stream_mode) {
        for (std::size_t index = 0; index < 10; ++index) {
            std::size_t offset = 0;
            easy_uds::StreamReader upload =
                [&request_body, &offset](char* buffer, std::size_t capacity) -> std::size_t {
                const std::size_t take = std::min(capacity, request_body.size() - offset);
                if (take != 0) {
                    std::memcpy(buffer, request_body.data() + offset, take);
                    offset += take;
                }
                return take;
            };
            (void)client.request_stream("upload", upload, [](std::string_view) {});
        }
    } else {
        const std::string route =
            serialized_mode ? "serial" : domain_mode ? "domain" : "ping";
        for (std::size_t index = 0; index < 2000; ++index) {
            (void)session.request(route);
        }
    }

    allocation_count.store(0, std::memory_order_relaxed);
    track_allocations.store(true, std::memory_order_relaxed);
    bool response_mismatch = false;
    if (stream_mode) {
        for (std::size_t index = 0; index < iterations; ++index) {
            std::size_t offset = 0;
            easy_uds::StreamReader upload =
                [&request_body, &offset](char* buffer, std::size_t capacity) -> std::size_t {
                const std::size_t take = std::min(capacity, request_body.size() - offset);
                if (take != 0) {
                    std::memcpy(buffer, request_body.data() + offset, take);
                    offset += take;
                }
                return take;
            };
            const int status = client.request_stream("upload", upload, [](std::string_view) {});
            if (status != 200) {
                response_mismatch = true;
                break;
            }
        }
    } else {
        const std::string route =
            serialized_mode ? "serial" : domain_mode ? "domain" : "ping";
        for (std::size_t index = 0; index < iterations; ++index) {
            const auto response = session.request(route);
            if (response.status != 200) {
                response_mismatch = true;
                break;
            }
        }
    }
    track_allocations.store(false, std::memory_order_relaxed);

    server.stop();
    server_thread.join();
    (void)::unlink((path + ".lock").c_str());
    if (server_error) {
        std::rethrow_exception(server_error);
    }
    if (response_mismatch) {
        throw std::runtime_error("response mismatch");
    }

    const std::size_t allocations = allocation_count.load(std::memory_order_relaxed);
    const double per_exchange = static_cast<double>(allocations) / static_cast<double>(iterations);
    std::cout << "mode="
              << (stream_mode       ? "stream"
                  : serialized_mode ? "serialized"
                  : domain_mode     ? "domain"
                                    : "session")
              << ", ";
    if (stream_mode) {
        const double exchange_mib = static_cast<double>(payload_bytes) / (1024.0 * 1024.0);
        std::cout << "exchanges=" << iterations << ", payload=" << payload_bytes
                  << ", allocations=" << allocations
                  << ", allocations_per_exchange=" << per_exchange
                  << ", allocations_per_MiB="
                  << allocations / (static_cast<double>(iterations) * exchange_mib) << '\n';
    } else {
        std::cout << "requests=" << iterations << ", allocations=" << allocations
                  << ", allocations_per_request=" << per_exchange << '\n';
    }
}
