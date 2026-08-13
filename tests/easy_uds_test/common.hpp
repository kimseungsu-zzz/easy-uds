#pragma once

#include "easy_uds/easy_uds.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <exception>
#include <fstream>
#include <iostream>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <poll.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

namespace easy_uds::test {

using namespace std::chrono_literals;

void expect(bool condition, const std::string& message);

template <typename Exception, typename Function>
void expect_throws(Function&& function, const std::string& message) {
    try {
        function();
    } catch (const Exception&) {
        return;
    }
    throw std::runtime_error("test failed: " + message);
}

template <typename Function>
void expect_easy_error(Function&& function, easy_uds::ErrorCode expected,
                       const std::string& message) {
    try {
        function();
    } catch (const easy_uds::Error& error) {
        if (error.kind() == expected) {
            return;
        }
        throw std::runtime_error("test failed: " + message +
                                 " (wrong easy-uds error code)");
    }
    throw std::runtime_error("test failed: " + message);
}

std::string socket_path(const char* suffix);
void wait_until_running(const easy_uds::Server& server);
int connect_raw(const std::string& path);
ssize_t send_no_signal(int fd, const void* data, std::size_t size);
void send_exact(int fd, const void* data, std::size_t size);
void recv_exact(int fd, void* data, std::size_t size);
void expect_peer_closed(int fd, std::chrono::milliseconds timeout,
                        const std::string& message);
std::uint32_t get_u32(const unsigned char* bytes);
void cleanup_socket_artifacts(const std::string& path);
void make_stale_socket(const std::string& path);
std::vector<unsigned char> frame(std::uint8_t type, std::uint32_t request_id,
                                 std::uint32_t arg1, std::uint32_t arg2,
                                 const void* payload, std::size_t payload_size);
std::vector<unsigned char> fixed_request(std::uint32_t request_id,
                                         const char* route,
                                         const char* body = "");

void test_option_validation();
void test_socket_path_safety();
void test_stop_preserves_replaced_socket_path();
void test_stale_socket_cleanup();
void test_basic_server();
void test_peer_credentials();
void test_tiny_message_limit_404();
void test_multiplexed_session();
void test_fragmented_fast_path_header();
void test_stream_connection_reuse();
void test_client_stream_response_limit();
void test_client_rejects_mismatched_response_ids();
void test_run_setup_failure_state();
void test_session_broken_after_shutdown();
void test_session_broken_after_timeout();
void test_idle_session_survives_io_timeout();
void test_session_move();
void test_error_model();
void test_fd_passing();
void test_reactor_request_timeouts();
void test_streams();
void test_stream_limit_reserves_worker();
void test_stream_timeout_is_independent();
void test_back_to_back_streams();
void test_serialized_handlers();
void test_serialized_queue_expiry();
void test_enqueue_maintenance();
void test_client_request_deadline();
void test_server_request_timeout_response();
void test_connection_limit();
void test_concurrent_clients();
void test_concurrent_handler_registration();
void test_stalled_response_does_not_block_workers();
void test_fixed_output_queue_is_bounded();
void test_pipelined_input_applies_backpressure();
void test_pipelined_input_resumes_below_low_watermark();
void test_disconnected_handler_fd_isolation();
void test_closing_connection_counts_toward_limit();
void test_stop_interrupts_blocked_workers();
void test_handler_error_opt_out();
void test_global_memory_budgets();
void test_partial_request_uses_global_budget();

} // namespace easy_uds::test
