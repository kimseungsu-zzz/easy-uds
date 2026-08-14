#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <cstdint>
#include <iostream>
#include <thread>

namespace {

struct AdjacentCounters {
    std::atomic<std::uint64_t> first{0};
    std::atomic<std::uint64_t> second{0};
};

struct alignas(64) PaddedCounter {
    std::atomic<std::uint64_t> value{0};
};

template <typename First, typename Second>
double run_pair(First& first, Second& second, std::size_t iterations) {
    const auto started = std::chrono::steady_clock::now();
    std::thread left([&] {
        for (std::size_t index = 0; index < iterations; ++index) {
            first.fetch_add(1, std::memory_order_relaxed);
        }
    });
    std::thread right([&] {
        for (std::size_t index = 0; index < iterations; ++index) {
            second.fetch_add(1, std::memory_order_relaxed);
        }
    });
    left.join();
    right.join();
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
}

} // namespace

int main(int argc, char** argv) {
    const std::size_t iterations =
        argc > 1 ? static_cast<std::size_t>(std::strtoull(argv[1], nullptr, 10)) : 100000000U;
    if (iterations == 0 || argc > 2) {
        std::cerr << "usage: easy_uds_false_sharing_probe [iterations]\n";
        return 2;
    }
    AdjacentCounters adjacent;
    PaddedCounter padded_first;
    PaddedCounter padded_second;
    const double adjacent_seconds = run_pair(adjacent.first, adjacent.second, iterations);
    const double padded_seconds = run_pair(padded_first.value, padded_second.value, iterations);
    std::cout << "iterations_per_counter=" << iterations
              << ", adjacent=" << adjacent_seconds << " s"
              << ", padded=" << padded_seconds << " s"
              << ", padded_speedup=" << adjacent_seconds / padded_seconds << "x\n";
    return 0;
}
