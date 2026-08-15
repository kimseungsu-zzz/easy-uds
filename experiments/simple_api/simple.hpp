#pragma once

// Experimental only: this header is intentionally outside include/easy_uds/
// and is not installed. It probes a beginner syntax layer without changing
// the production Server/Client API or the protocol.

#include "easy_uds/easy_uds.hpp"

#include <functional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

namespace easy_uds::simple {

namespace detail {

template <typename T>
struct is_supported_result
    : std::bool_constant<
          std::is_same_v<std::decay_t<T>, std::string> ||
          std::is_same_v<std::decay_t<T>, std::string_view> ||
          std::is_same_v<std::decay_t<T>, const char*> ||
          std::is_same_v<std::decay_t<T>, char*>> {};

template <typename T>
[[nodiscard]] Response to_response(T&& value) {
    using Result = std::decay_t<T>;
    static_assert(is_supported_result<Result>::value,
                  "easy-uds simple handler: unsupported return type; "
                  "expected std::string, std::string_view, or const char*");
    if constexpr (std::is_same_v<Result, std::string>) {
        return Response::ok(std::forward<T>(value));
    } else {
        return Response::ok(std::string(value));
    }
}

template <typename Handler>
[[nodiscard]] Response invoke_handler(Handler& handler,
                                      std::string_view body) {
    if constexpr (std::is_invocable_v<Handler&, std::string_view>) {
        return to_response(std::invoke(handler, body));
    } else if constexpr (std::is_invocable_v<Handler&>) {
        return to_response(std::invoke(handler));
    } else {
        static_assert(std::is_invocable_v<Handler&, std::string_view> ||
                          std::is_invocable_v<Handler&>,
                      "easy-uds simple handler: expected () or "
                      "(std::string_view) signature");
    }
}

}  // namespace detail

class Server {
  public:
    class Route {
      public:
        Route(const Route&) = delete;
        Route& operator=(const Route&) = delete;
        Route(Route&&) noexcept = default;
        Route& operator=(Route&&) = delete;

        Route& operator=(std::string value) {
            register_constant(std::move(value));
            return *this;
        }

        Route& operator=(std::string_view value) {
            register_constant(std::string(value));
            return *this;
        }

        Route& operator=(const char* value) {
            if (value == nullptr) {
                throw std::invalid_argument(
                    "easy-uds simple route value must not be null");
            }
            register_constant(std::string(value));
            return *this;
        }

        template <typename Handler,
                  std::enable_if_t<
                      !std::is_convertible_v<std::decay_t<Handler>,
                                              std::string_view>,
                      int> = 0>
        Route& operator=(Handler&& handler) {
            using Callable = std::decay_t<Handler>;
            Callable callable(std::forward<Handler>(handler));
            owner_->core_.on(
                std::move(route_),
                [callable = std::move(callable)](const Request& request) mutable {
                    return detail::invoke_handler(callable, request.body);
                });
            return *this;
        }

      private:
        friend class Server;

        Route(Server& owner, std::string route)
            : owner_(&owner), route_(std::move(route)) {}

        void register_constant(std::string value) {
            owner_->core_.on(
                std::move(route_),
                [value = std::move(value)](const Request&) {
                    return Response::ok(value);
                });
        }

        Server* owner_;
        std::string route_;
    };

    explicit Server(std::string socket_path, ServerOptions options = {})
        : core_(std::move(socket_path), std::move(options)) {}

    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;
    Server(Server&&) = delete;
    Server& operator=(Server&&) = delete;

    [[nodiscard]] Route on(std::string route) {
        return Route(*this, std::move(route));
    }

    void run() { core_.run(); }
    void stop() noexcept { core_.stop(); }
    [[nodiscard]] bool is_running() const noexcept { return core_.is_running(); }

    // Explicit prototype escape hatch. Production promotion must decide
    // whether exposing this preserves a useful lifetime/ownership contract.
    [[nodiscard]] easy_uds::Server& core() noexcept { return core_; }
    [[nodiscard]] const easy_uds::Server& core() const noexcept { return core_; }

  private:
    easy_uds::Server core_;
};

class Client {
  public:
    explicit Client(std::string socket_path, ClientOptions options = {})
        : core_(std::move(socket_path), std::move(options)) {}

    [[nodiscard]] std::string request(std::string_view route,
                                      std::string_view body = {}) const {
        const Response response = core_.request(route, body);
        if (response.status != status_ok) {
            throw std::runtime_error(
                "easy-uds simple request returned status " +
                std::to_string(response.status));
        }
        return response.body;
    }

    [[nodiscard]] easy_uds::Client& core() noexcept { return core_; }
    [[nodiscard]] const easy_uds::Client& core() const noexcept { return core_; }

  private:
    easy_uds::Client core_;
};

}  // namespace easy_uds::simple
