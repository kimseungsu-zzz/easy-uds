#include <easy_uds/client.hpp>
#include <easy_uds/error.hpp>
#include <easy_uds/posix.hpp>
#include <easy_uds/server.hpp>
#include <easy_uds/stats.hpp>
#include <easy_uds/version.hpp>

#include <chrono>
#include <string_view>
#include <type_traits>
#include <utility>

#include <unistd.h>

static_assert(easy_uds::version == std::string_view{"0.8.0-rc.1"});
static_assert(easy_uds::protocol_version == 2U);
static_assert(sizeof(easy_uds::BorrowedFd) == sizeof(int));
static_assert(sizeof(easy_uds::OwnedFd) == sizeof(int));
static_assert(std::is_aggregate_v<easy_uds::Response>);
static_assert(!std::is_copy_constructible_v<easy_uds::Request>);
static_assert(std::is_move_constructible_v<easy_uds::Request>);
static_assert(std::is_copy_constructible_v<easy_uds::posix::RequestCapabilities>);
static_assert(std::is_move_constructible_v<easy_uds::posix::RequestCapabilities>);
static_assert(sizeof(easy_uds::posix::RequestCapabilities) == sizeof(void*));
static_assert(!std::is_copy_constructible_v<easy_uds::OwnedFd>);
static_assert(std::is_same_v<decltype(std::declval<const easy_uds::Session&>().status()),
                             easy_uds::SessionStatus>);
static_assert(std::is_same_v<decltype(std::declval<const easy_uds::Session&>().valid()), bool>);
static_assert(noexcept(std::declval<const easy_uds::Session&>().status()));
static_assert(noexcept(std::declval<const easy_uds::Session&>().valid()));
static_assert(!std::is_copy_constructible_v<easy_uds::RequestContext>);
static_assert(!std::is_move_constructible_v<easy_uds::RequestContext>);
static_assert(std::is_constructible_v<
              easy_uds::RouteOptions, easy_uds::RouteOptions::Handler>);
static_assert(std::is_constructible_v<
              easy_uds::RouteOptions,
              easy_uds::RouteOptions::SimpleHandler>);
static_assert(std::is_same_v<decltype(std::declval<const easy_uds::Server&>().stats()),
                             easy_uds::ServerStats>);
static_assert(std::is_same_v<decltype(std::declval<const easy_uds::Session&>().stats()),
                             easy_uds::SessionStats>);
static_assert(easy_uds::ServerOptions{}.stats == easy_uds::StatsMode::disabled);
static_assert(
    easy_uds::ServerOptions{}.max_concurrent_serialized_domains == 0);
static_assert(easy_uds::ClientOptions{}.stats == easy_uds::StatsMode::disabled);

void register_context_routes(easy_uds::Server& server) {
    const auto handler = [](const easy_uds::Request&,
                            const easy_uds::RequestContext&) {
        return easy_uds::Response{200, "ok"};
    };
    server.on("context", easy_uds::RouteOptions{handler});
    server.on_prefix("context.prefix.", easy_uds::RouteOptions{handler});
    server.on_serialized("context.serialized",
                         easy_uds::RouteOptions{handler});
    server.on(
        "simple.latest",
        easy_uds::RouteOptions{[](const easy_uds::Request&) {
            return easy_uds::Response{easy_uds::status_ok, "ok"};
        }}.serialize_in("drivetrain",
                        easy_uds::QueuePolicy::latest_wins));
    server.on(
        "context.busy",
        easy_uds::RouteOptions{handler}.serialize_in(
            "arm", easy_uds::QueuePolicy::reject_if_busy));
}

void inspect_runtime_stats(const easy_uds::Server& server,
                           const easy_uds::Session& session) {
    (void)server.stats();
    (void)session.stats();
}

int main() {
    constexpr const char* socket_path =
        "/tmp/easy-uds-package-consumer-never-created.sock";
    (void)::unlink(socket_path);
    easy_uds::ClientOptions options;
    options.connect_timeout = std::chrono::milliseconds{50};
    options.request_timeout = std::chrono::milliseconds{100};
    const easy_uds::Client client(socket_path, options);
    const easy_uds::BorrowedFd empty;
    if (client.socket_path().empty() || empty.valid()) {
        return 1;
    }
    try {
        (void)client.request("ping");
    } catch (const easy_uds::Error& error) {
        return error.code() == easy_uds::ErrorCode::unavailable &&
                       error.system_code()
                   ? 0
                   : 1;
    }
    return 1;
}
