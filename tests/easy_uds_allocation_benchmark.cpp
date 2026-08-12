#include "easy_uds/easy_uds.hpp"

#include <atomic>
#include <cstddef>
#include <cstdlib>
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
    if (iterations == 0 || argc > 2) {
        std::cerr << "usage: easy_uds_allocation_benchmark [iterations]\n";
        return 2;
    }

    const std::string path = "/tmp/easy-uds-allocation-benchmark-" +
                             std::to_string(static_cast<long long>(::getpid())) + ".sock";
    easy_uds::ServerOptions options;
    options.worker_threads = 1;
    easy_uds::Server server(path, options);
    server.on("ping", [](const easy_uds::Request&) { return easy_uds::Response{200, "pong"}; });

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

    easy_uds::Session session = easy_uds::Client(path).session();
    for (std::size_t index = 0; index < 1000; ++index) {
        (void)session.request("ping");
    }

    allocation_count.store(0, std::memory_order_relaxed);
    track_allocations.store(true, std::memory_order_relaxed);
    bool response_mismatch = false;
    for (std::size_t index = 0; index < iterations; ++index) {
        const auto response = session.request("ping");
        if (response.status != 200 || response.body != "pong") {
            response_mismatch = true;
            break;
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
    std::cout << "requests=" << iterations << ", ordinary_heap_allocations=" << allocations
              << ", allocations_per_request="
              << static_cast<double>(allocations) / static_cast<double>(iterations) << '\n';
}
