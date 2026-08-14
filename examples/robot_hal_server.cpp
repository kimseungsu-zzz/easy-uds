#include "easy_uds/easy_uds.hpp"

#include <chrono>
#include <iostream>
#include <mutex>
#include <string>
#include <string_view>

namespace {

using namespace std::chrono_literals;

class RobotHal {
  public:
    easy_uds::Response set_velocity(std::string_view command) {
        if (command.empty()) {
            return {easy_uds::status_bad_request,
                    "velocity command must not be empty"};
        }
        std::lock_guard<std::mutex> lock(mutex_);
        velocity_ = std::string(command);
        return {easy_uds::status_ok, velocity_};
    }

    easy_uds::Response move_arm(std::string_view position) {
        if (position.empty()) {
            return {easy_uds::status_bad_request,
                    "arm position must not be empty"};
        }
        std::lock_guard<std::mutex> lock(mutex_);
        arm_position_ = std::string(position);
        return {easy_uds::status_ok, arm_position_};
    }

    easy_uds::Response calibrate() {
        std::lock_guard<std::mutex> lock(mutex_);
        calibrated_ = true;
        return {easy_uds::status_ok, "calibrated"};
    }

    std::string state() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return "velocity=" + velocity_ + " arm=" + arm_position_ +
               " calibrated=" + (calibrated_ ? "true" : "false");
    }

  private:
    mutable std::mutex mutex_;
    std::string velocity_ = "0.0";
    std::string arm_position_ = "home";
    bool calibrated_ = false;
};

std::string diagnostics(const easy_uds::ServerStats& stats) {
    std::string body =
        "running=" + std::string(stats.running ? "true" : "false") +
        " active_connections=" + std::to_string(stats.active_connections) +
        " inflight_requests=" + std::to_string(stats.inflight_requests) +
        " worker_queue_depth=" + std::to_string(stats.worker_queue_depth) +
        " serialized_queue_depth=" +
        std::to_string(stats.serialized_queue_depth) +
        " active_serialized_domains=" +
        std::to_string(stats.active_serialized_domains) +
        " queued_output_bytes=" + std::to_string(stats.queued_output_bytes);
    if (stats.counters) {
        body += " fixed_requests=" +
                std::to_string(stats.counters->fixed_requests_dispatched) +
                " superseded=" +
                std::to_string(stats.counters->serialized_requests_superseded) +
                " busy_rejected=" + std::to_string(
                    stats.counters->serialized_requests_rejected_busy);
    }
    return body;
}

}  // namespace

int main(int argc, char** argv) {
    const std::string socket_path =
        argc > 1 ? argv[1] : "/tmp/easy-uds-robot.sock";

    easy_uds::ServerOptions options;
    options.worker_threads = 4;
    options.max_concurrent_serialized_domains = 2;
    options.request_timeout = 200ms;
    options.stats = easy_uds::StatsMode::basic;

    easy_uds::Server server(socket_path, options);
    RobotHal hal;

    // Keep health short and cheap enough for a watchdog or readiness probe.
    server.on("health", [](const easy_uds::Request&) {
        return easy_uds::Response{easy_uds::status_ok, "ok"};
    });

    // Diagnostics are intentionally an application-owned RPC. Stats snapshots
    // are best-effort and should be sampled, not treated as one transaction.
    server.on("diagnostics", [&server, &hal](const easy_uds::Request&) {
        return easy_uds::Response{easy_uds::status_ok,
                                  diagnostics(server.stats()) +
                                      " " + hal.state()};
    });

    // Velocity commands are latest-value data: an older queued command is not
    // useful after a newer command for the same concrete route arrives.
    server.on(
        "drive/velocity",
        easy_uds::RouteOptions{[&hal](const easy_uds::Request& request,
                                      const easy_uds::RequestContext& context) {
            if (context.stop_requested()) {
                return easy_uds::Response{easy_uds::status_request_timeout,
                                          "velocity command cancelled"};
            }
            return hal.set_velocity(request.body);
        }}.serialize_in("drivetrain", easy_uds::QueuePolicy::latest_wins));

    // Arm movements are ordered and may use the contextual deadline while a
    // real HAL implementation performs a cooperative, interruptible move.
    server.on(
        "arm/position",
        easy_uds::RouteOptions{[&hal](const easy_uds::Request& request,
                                      const easy_uds::RequestContext& context) {
            if (context.stop_requested()) {
                return easy_uds::Response{easy_uds::status_request_timeout,
                                          "arm move cancelled"};
            }
            return hal.move_arm(request.body);
        }}.serialize_in("arm"));

    // Calibration must not overlap another drivetrain operation. A busy
    // domain produces 409 and leaves the connection/Session usable.
    server.on(
        "drive/calibrate",
        easy_uds::RouteOptions{[&hal](const easy_uds::Request&) {
            return hal.calibrate();
        }}.serialize_in("drivetrain", easy_uds::QueuePolicy::reject_if_busy));

    std::cout << "robot HAL server listening on " << socket_path << '\n'
              << "  health       -> watchdog/readiness response\n"
              << "  diagnostics  -> stats + simulated HAL state\n"
              << "  drive/velocity (LatestWins, drivetrain)\n"
              << "  arm/position   (FIFO, arm)\n"
              << "  drive/calibrate (RejectIfBusy, drivetrain)\n";
    server.run();
    return 0;
}
