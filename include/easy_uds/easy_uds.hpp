#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>

namespace easy_uds {

struct Options {
    std::size_t max_route_bytes{4 * 1024};
    std::size_t max_payload_bytes{16 * 1024 * 1024};
    std::chrono::milliseconds timeout{5000};
    int backlog{16};
};

struct Request {
    std::string route;
    std::string body;
};

struct Response {
    int status{200};
    std::string body;

    [[nodiscard]] bool ok() const noexcept {
        return status >= 200 && status < 300;
    }
};

class Error final : public std::runtime_error {
public:
    Error(std::string operation, int native_code, std::string detail = {});

    [[nodiscard]] int native_code() const noexcept;
    [[nodiscard]] const std::string& operation() const noexcept;

private:
    std::string operation_;
    int native_code_{};
};

class Server final {
public:
    using Handler = std::function<Response(const Request&)>;

    explicit Server(std::string socket_path, Options options = {});
    ~Server();

    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;
    Server(Server&&) noexcept;
    Server& operator=(Server&&) noexcept;

    Server& on(std::string route, Handler handler);
    void listen();
    void run();
    void serve_once();
    void stop() noexcept;

    [[nodiscard]] bool running() const noexcept;
    [[nodiscard]] const std::string& socket_path() const noexcept;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

class Client final {
public:
    explicit Client(std::string socket_path, Options options = {});

    [[nodiscard]] Response request(std::string_view route, std::string_view body = {}) const;
    [[nodiscard]] const std::string& socket_path() const noexcept;

private:
    std::string socket_path_;
    Options options_;
};

} // namespace easy_uds

