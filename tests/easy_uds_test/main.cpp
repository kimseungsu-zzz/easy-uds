#include "common.hpp"

int main() {
    using namespace easy_uds::test;

#define RUN(name)                   \
    std::cerr << "RUN " #name "\n"; \
    name()
    try {
        RUN(test_option_validation);
        RUN(test_socket_path_safety);
        RUN(test_stop_preserves_replaced_socket_path);
        RUN(test_stale_socket_cleanup);
        RUN(test_basic_server);
        RUN(test_peer_credentials);
        RUN(test_tiny_message_limit_404);
        RUN(test_multiplexed_session);
        RUN(test_fragmented_fast_path_header);
        RUN(test_stream_connection_reuse);
        RUN(test_client_stream_response_limit);
        RUN(test_client_rejects_mismatched_response_ids);
        RUN(test_session_broken_after_shutdown);
        RUN(test_session_broken_after_timeout);
        RUN(test_idle_session_survives_io_timeout);
        RUN(test_session_move);
        RUN(test_error_model);
        RUN(test_request_context);
        RUN(test_stats_snapshots);
        RUN(test_fd_passing);
        RUN(test_reactor_request_timeouts);
        RUN(test_streams);
        RUN(test_stream_limit_reserves_worker);
        RUN(test_stream_timeout_is_independent);
        RUN(test_back_to_back_streams);
        RUN(test_serialized_handlers);
        RUN(test_serialized_queue_expiry);
        RUN(test_enqueue_maintenance);
        RUN(test_client_request_deadline);
        RUN(test_server_request_timeout_response);
        RUN(test_connection_limit);
        RUN(test_concurrent_clients);
        RUN(test_concurrent_handler_registration);
        RUN(test_stalled_response_does_not_block_workers);
        RUN(test_fixed_output_queue_is_bounded);
        RUN(test_pipelined_input_applies_backpressure);
        RUN(test_pipelined_input_resumes_below_low_watermark);
        RUN(test_disconnected_handler_fd_isolation);
        RUN(test_closing_connection_counts_toward_limit);
        RUN(test_stop_interrupts_blocked_workers);
        RUN(test_handler_error_opt_out);
        RUN(test_run_setup_failure_state);
        RUN(test_global_memory_budgets);
        RUN(test_partial_request_uses_global_budget);
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
#undef RUN

    std::cout << "All tests passed.\n";
    return 0;
}
