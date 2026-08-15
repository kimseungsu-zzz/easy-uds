#ifdef EASY_UDS_SIMPLE_BENCHMARK
#include "easy_uds/simple.hpp"
#else
#include "easy_uds/easy_uds.hpp"
#endif

#include <atomic>
#include <cstddef>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <new>
#include <string>
#include <string_view>
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

}  // namespace

void* operator new(std::size_t size) { return allocate(size); }
void* operator new[](std::size_t size) { return allocate(size); }
void operator delete(void* pointer) noexcept { std::free(pointer); }
void operator delete[](void* pointer) noexcept { std::free(pointer); }
void operator delete(void* pointer, std::size_t) noexcept { std::free(pointer); }
void operator delete[](void* pointer, std::size_t) noexcept { std::free(pointer); }

int main(int argc, char** argv) {
    const std::size_t iterations =
        argc > 1 ? std::strtoull(argv[1], nullptr, 10) : 5000U;
    if (iterations == 0 || argc > 2) {
        std::cerr << "usage: simple/core_api_allocation_benchmark [iterations]\n";
        return 2;
    }
#ifdef EASY_UDS_SIMPLE_BENCHMARK
    const std::string mode = "simple";
#else
    const std::string mode = "core";
#endif
    const std::string path = "/tmp/easy-uds-" + mode + "-api-allocation-" +
                             std::to_string(static_cast<long long>(::getpid())) +
                             ".sock";
    easy_uds::ServerOptions options;
    options.worker_threads = 1;
#ifdef EASY_UDS_SIMPLE_BENCHMARK
    easy_uds::simple::Server server(path, options);
    server.on("echo") = [](std::string_view body) {
        return std::string(body);
    };
    easy_uds::simple::Client client(path);
#else
    easy_uds::Server server(path, options);
    server.on("echo", [](const easy_uds::Request& request) {
        return easy_uds::Response::ok(request.body);
    });
    easy_uds::Client client(path);
#endif
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
    for (std::size_t index = 0; index < 500; ++index) {
        (void)client.request("echo", "warmup");
    }
    allocation_count.store(0, std::memory_order_relaxed);
    track_allocations.store(true, std::memory_order_release);
    for (std::size_t index = 0; index < iterations; ++index) {
        (void)client.request("echo", "hello");
    }
    track_allocations.store(false, std::memory_order_release);
    server.stop();
    server_thread.join();
    (void)::unlink(path.c_str());
    (void)::unlink((path + ".lock").c_str());
    if (server_error) {
        std::rethrow_exception(server_error);
    }
    const std::size_t allocations =
        allocation_count.load(std::memory_order_relaxed);
    std::cout << "api=" << mode << ", requests=" << iterations
              << ", allocations=" << allocations
              << ", allocations_per_request="
              << static_cast<double>(allocations) /
                     static_cast<double>(iterations)
              << '\n';
    return 0;
}
