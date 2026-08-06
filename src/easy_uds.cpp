#include "easy_uds/easy_uds.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <limits>
#include <mutex>
#include <sstream>
#include <unordered_map>
#include <utility>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <afunix.h>
#else
#include <arpa/inet.h>
#include <cerrno>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>
#endif

namespace easy_uds {
namespace {

#ifdef _WIN32
using NativeSocket = SOCKET;
constexpr NativeSocket invalid_socket = INVALID_SOCKET;

class WinsockRuntime {
public:
    WinsockRuntime() {
        WSADATA data{};
        const int result = WSAStartup(MAKEWORD(2, 2), &data);
        if (result != 0) {
            throw Error("WSAStartup", result);
        }
    }

    ~WinsockRuntime() {
        WSACleanup();
    }
};

void ensure_socket_runtime() {
    static WinsockRuntime runtime;
    (void)runtime;
}

int last_socket_error() noexcept {
    return WSAGetLastError();
}

void close_socket(NativeSocket socket) noexcept {
    if (socket != invalid_socket) {
        closesocket(socket);
    }
}
#else
using NativeSocket = int;
constexpr NativeSocket invalid_socket = -1;

void ensure_socket_runtime() {}

int last_socket_error() noexcept {
    return errno;
}

void close_socket(NativeSocket socket) noexcept {
    if (socket != invalid_socket) {
        ::close(socket);
    }
}
#endif

class Socket final {
public:
    Socket() = default;
    explicit Socket(NativeSocket socket) : socket_(socket) {}
    ~Socket() { reset(); }

    Socket(const Socket&) = delete;
    Socket& operator=(const Socket&) = delete;

    Socket(Socket&& other) noexcept : socket_(other.release()) {}

    Socket& operator=(Socket&& other) noexcept {
        if (this != &other) {
            reset(other.release());
        }
        return *this;
    }

    [[nodiscard]] NativeSocket get() const noexcept { return socket_; }
    [[nodiscard]] bool valid() const noexcept { return socket_ != invalid_socket; }

    NativeSocket release() noexcept {
        return std::exchange(socket_, invalid_socket);
    }

    void reset(NativeSocket socket = invalid_socket) noexcept {
        close_socket(socket_);
        socket_ = socket;
    }

private:
    NativeSocket socket_{invalid_socket};
};

constexpr std::array<std::uint8_t, 4> magic{'E', 'U', 'D', 'S'};
constexpr std::uint8_t protocol_version = 1;
constexpr std::uint8_t request_type = 1;
constexpr std::uint8_t response_type = 2;
constexpr std::size_t wire_header_size = 16;

struct WireMessage {
    std::uint8_t type{};
    std::uint16_t status{};
    std::string name;
    std::string body;
};

std::string build_error_message(const std::string& operation, int native_code, const std::string& detail) {
    std::ostringstream stream;
    stream << operation << " failed";
    if (native_code != 0) {
        stream << " (native error " << native_code << ')';
    }
    if (!detail.empty()) {
        stream << ": " << detail;
    }
    return stream.str();
}

void validate_options(const Options& options) {
    if (options.max_route_bytes == 0 || options.max_route_bytes > std::numeric_limits<std::uint32_t>::max()) {
        throw std::invalid_argument("max_route_bytes must be between 1 and UINT32_MAX");
    }
    if (options.max_payload_bytes == 0 || options.max_payload_bytes > std::numeric_limits<std::uint32_t>::max()) {
        throw std::invalid_argument("max_payload_bytes must be between 1 and UINT32_MAX");
    }
    if (options.timeout.count() <= 0) {
        throw std::invalid_argument("timeout must be positive");
    }
    if (options.backlog <= 0) {
        throw std::invalid_argument("backlog must be positive");
    }
}

sockaddr_un make_address(const std::string& path) {
    if (path.empty()) {
        throw std::invalid_argument("socket path must not be empty");
    }

    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    if (path.size() >= sizeof(address.sun_path)) {
        throw std::invalid_argument("socket path is too long for sockaddr_un");
    }
    std::memcpy(address.sun_path, path.c_str(), path.size() + 1);
    return address;
}

Socket create_socket() {
    ensure_socket_runtime();
    const auto native = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (native == invalid_socket) {
        throw Error("socket", last_socket_error());
    }
    Socket socket(native);
#ifdef SO_NOSIGPIPE
    const int enabled = 1;
    if (::setsockopt(socket.get(), SOL_SOCKET, SO_NOSIGPIPE, &enabled, sizeof(enabled)) != 0) {
        throw Error("setsockopt(SO_NOSIGPIPE)", last_socket_error());
    }
#endif
    return socket;
}

void set_timeout(NativeSocket socket, std::chrono::milliseconds timeout) {
#ifdef _WIN32
    const DWORD value = static_cast<DWORD>(timeout.count());
    if (::setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&value), sizeof(value)) != 0 ||
        ::setsockopt(socket, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&value), sizeof(value)) != 0) {
        throw Error("setsockopt", last_socket_error());
    }
#else
    timeval value{};
    value.tv_sec = static_cast<decltype(value.tv_sec)>(timeout.count() / 1000);
    value.tv_usec = static_cast<decltype(value.tv_usec)>((timeout.count() % 1000) * 1000);
    if (::setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO, &value, sizeof(value)) != 0 ||
        ::setsockopt(socket, SOL_SOCKET, SO_SNDTIMEO, &value, sizeof(value)) != 0) {
        throw Error("setsockopt", last_socket_error());
    }
#endif
}

void send_all(NativeSocket socket, const void* data, std::size_t size) {
    const auto* cursor = static_cast<const char*>(data);
    while (size > 0) {
        const auto chunk_size = static_cast<int>((std::min)(size, static_cast<std::size_t>(std::numeric_limits<int>::max())));
#ifdef MSG_NOSIGNAL
        constexpr int send_flags = MSG_NOSIGNAL;
#else
        constexpr int send_flags = 0;
#endif
        const int sent = ::send(socket, cursor, chunk_size, send_flags);
        if (sent <= 0) {
            throw Error("send", last_socket_error(), sent == 0 ? "connection closed" : "");
        }
        cursor += sent;
        size -= static_cast<std::size_t>(sent);
    }
}

void receive_all(NativeSocket socket, void* data, std::size_t size) {
    auto* cursor = static_cast<char*>(data);
    while (size > 0) {
        const auto chunk_size = static_cast<int>((std::min)(size, static_cast<std::size_t>(std::numeric_limits<int>::max())));
        const int received = ::recv(socket, cursor, chunk_size, 0);
        if (received <= 0) {
            throw Error("recv", last_socket_error(), received == 0 ? "connection closed" : "");
        }
        cursor += received;
        size -= static_cast<std::size_t>(received);
    }
}

void store_u16(std::uint8_t* destination, std::uint16_t value) {
    value = htons(value);
    std::memcpy(destination, &value, sizeof(value));
}

void store_u32(std::uint8_t* destination, std::uint32_t value) {
    value = htonl(value);
    std::memcpy(destination, &value, sizeof(value));
}

std::uint16_t load_u16(const std::uint8_t* source) {
    std::uint16_t value{};
    std::memcpy(&value, source, sizeof(value));
    return ntohs(value);
}

std::uint32_t load_u32(const std::uint8_t* source) {
    std::uint32_t value{};
    std::memcpy(&value, source, sizeof(value));
    return ntohl(value);
}

void send_message(NativeSocket socket, const WireMessage& message, const Options& options) {
    if (message.name.size() > options.max_route_bytes || message.name.size() > std::numeric_limits<std::uint32_t>::max()) {
        throw std::length_error("route exceeds max_route_bytes");
    }
    if (message.body.size() > options.max_payload_bytes || message.body.size() > std::numeric_limits<std::uint32_t>::max()) {
        throw std::length_error("payload exceeds max_payload_bytes");
    }

    std::array<std::uint8_t, wire_header_size> header{};
    std::copy(magic.begin(), magic.end(), header.begin());
    header[4] = protocol_version;
    header[5] = message.type;
    store_u16(header.data() + 6, message.status);
    store_u32(header.data() + 8, static_cast<std::uint32_t>(message.name.size()));
    store_u32(header.data() + 12, static_cast<std::uint32_t>(message.body.size()));

    send_all(socket, header.data(), header.size());
    if (!message.name.empty()) {
        send_all(socket, message.name.data(), message.name.size());
    }
    if (!message.body.empty()) {
        send_all(socket, message.body.data(), message.body.size());
    }
}

WireMessage receive_message(NativeSocket socket, const Options& options) {
    std::array<std::uint8_t, wire_header_size> header{};
    receive_all(socket, header.data(), header.size());

    if (!std::equal(magic.begin(), magic.end(), header.begin())) {
        throw std::runtime_error("invalid easy-uds message magic");
    }
    if (header[4] != protocol_version) {
        throw std::runtime_error("unsupported easy-uds protocol version");
    }
    if (header[5] != request_type && header[5] != response_type) {
        throw std::runtime_error("invalid easy-uds message type");
    }

    const auto name_size = load_u32(header.data() + 8);
    const auto body_size = load_u32(header.data() + 12);
    if (name_size > options.max_route_bytes) {
        throw std::length_error("received route exceeds max_route_bytes");
    }
    if (body_size > options.max_payload_bytes) {
        throw std::length_error("received payload exceeds max_payload_bytes");
    }

    WireMessage message;
    message.type = header[5];
    message.status = load_u16(header.data() + 6);
    message.name.resize(name_size);
    message.body.resize(body_size);
    if (name_size > 0) {
        receive_all(socket, message.name.data(), message.name.size());
    }
    if (body_size > 0) {
        receive_all(socket, message.body.data(), message.body.size());
    }
    return message;
}

bool wait_for_connection(NativeSocket socket, std::chrono::milliseconds timeout) {
    fd_set read_set;
    FD_ZERO(&read_set);
    FD_SET(socket, &read_set);

    timeval value{};
    value.tv_sec = static_cast<decltype(value.tv_sec)>(timeout.count() / 1000);
    value.tv_usec = static_cast<decltype(value.tv_usec)>((timeout.count() % 1000) * 1000);

#ifdef _WIN32
    const int result = ::select(0, &read_set, nullptr, nullptr, &value);
#else
    const int result = ::select(socket + 1, &read_set, nullptr, nullptr, &value);
#endif
    if (result < 0) {
        throw Error("select", last_socket_error());
    }
    return result > 0;
}

void remove_socket_file(const std::string& path) noexcept {
    std::error_code ignored;
    std::filesystem::remove(path, ignored);
}

void remove_stale_socket_file(const std::string& path) {
    std::error_code error;
    const auto status = std::filesystem::symlink_status(path, error);
    if (error == std::errc::no_such_file_or_directory || status.type() == std::filesystem::file_type::not_found) {
        return;
    }
    if (error) {
        throw std::filesystem::filesystem_error("cannot inspect socket path", path, error);
    }
    if (std::filesystem::is_regular_file(status) || std::filesystem::is_directory(status)) {
        throw std::runtime_error("refusing to remove a non-socket path: " + path);
    }
    if (!std::filesystem::remove(path, error) || error) {
        throw std::filesystem::filesystem_error("cannot remove stale socket path", path, error);
    }
}

} // namespace

Error::Error(std::string operation, int native_code, std::string detail)
    : std::runtime_error(build_error_message(operation, native_code, detail)),
      operation_(std::move(operation)),
      native_code_(native_code) {}

int Error::native_code() const noexcept {
    return native_code_;
}

const std::string& Error::operation() const noexcept {
    return operation_;
}

class Server::Impl final {
public:
    Impl(std::string socket_path, Options options)
        : socket_path_(std::move(socket_path)), options_(options) {
        validate_options(options_);
        (void)make_address(socket_path_);
    }

    ~Impl() {
        stop();
    }

    Server& add_handler(Server& owner, std::string route, Handler handler) {
        if (route.empty()) {
            throw std::invalid_argument("route must not be empty");
        }
        if (route.size() > options_.max_route_bytes) {
            throw std::length_error("route exceeds max_route_bytes");
        }
        if (!handler) {
            throw std::invalid_argument("handler must not be empty");
        }
        if (running_) {
            throw std::logic_error("handlers cannot be changed while the server is running");
        }
        handlers_.insert_or_assign(std::move(route), std::move(handler));
        return owner;
    }

    void listen() {
        std::lock_guard lock(listener_mutex_);
        if (listener_.valid()) {
            return;
        }

        Socket listener = create_socket();
        const auto address = make_address(socket_path_);
        remove_stale_socket_file(socket_path_);
        if (::bind(listener.get(), reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0) {
            throw Error("bind", last_socket_error(), socket_path_);
        }
        if (::listen(listener.get(), options_.backlog) != 0) {
            remove_socket_file(socket_path_);
            throw Error("listen", last_socket_error());
        }
        listener_ = std::move(listener);
        owns_socket_path_ = true;
        running_ = true;
    }

    void run() {
        listen();
        while (running_) {
            NativeSocket listener = invalid_socket;
            {
                std::lock_guard lock(listener_mutex_);
                listener = listener_.get();
            }
            try {
                if (listener != invalid_socket && wait_for_connection(listener, std::chrono::milliseconds(100)) && running_) {
                    serve_once();
                }
            } catch (const Error&) {
                if (running_) {
                    throw;
                }
            }
        }
    }

    void serve_once() {
        NativeSocket listener = invalid_socket;
        {
            std::lock_guard lock(listener_mutex_);
            listener = listener_.get();
        }
        if (listener == invalid_socket || !running_) {
            return;
        }

        const auto accepted = ::accept(listener, nullptr, nullptr);
        if (accepted == invalid_socket) {
            if (!running_) {
                return;
            }
            throw Error("accept", last_socket_error());
        }
        Socket client(accepted);
        set_timeout(client.get(), options_.timeout);
        handle_client(client.get());
    }

    void stop() noexcept {
        {
            std::lock_guard lock(listener_mutex_);
            running_ = false;
            listener_.reset();
            if (owns_socket_path_) {
                remove_socket_file(socket_path_);
                owns_socket_path_ = false;
            }
        }
    }

    [[nodiscard]] bool running() const noexcept { return running_; }
    [[nodiscard]] const std::string& socket_path() const noexcept { return socket_path_; }

private:
    void handle_client(NativeSocket client) noexcept {
        try {
            const auto message = receive_message(client, options_);
            if (message.type != request_type || message.name.empty()) {
                send_response(client, 400, "invalid request");
                return;
            }

            const auto handler = handlers_.find(message.name);
            if (handler == handlers_.end()) {
                send_response(client, 404, "route not found: " + message.name);
                return;
            }

            try {
                const auto response = handler->second(Request{message.name, message.body});
                if (response.status < 0 || response.status > std::numeric_limits<std::uint16_t>::max()) {
                    send_response(client, 500, "handler returned an invalid status code");
                    return;
                }
                send_response(client, response.status, response.body);
            } catch (const std::exception& error) {
                send_response(client, 500, std::string("handler failed: ") + error.what());
            } catch (...) {
                send_response(client, 500, "handler failed with an unknown error");
            }
        } catch (const std::exception& error) {
            try {
                send_response(client, 400, error.what());
            } catch (...) {
            }
        }
    }

    void send_response(NativeSocket client, int status, std::string body) {
        send_message(client, WireMessage{response_type, static_cast<std::uint16_t>(status), {}, std::move(body)}, options_);
    }

    std::string socket_path_;
    Options options_;
    Socket listener_;
    mutable std::mutex listener_mutex_;
    bool owns_socket_path_{false};
    std::atomic_bool running_{false};
    std::unordered_map<std::string, Handler> handlers_;
};

Server::Server(std::string socket_path, Options options)
    : impl_(std::make_unique<Impl>(std::move(socket_path), options)) {}

Server::~Server() = default;
Server::Server(Server&&) noexcept = default;
Server& Server::operator=(Server&&) noexcept = default;

Server& Server::on(std::string route, Handler handler) {
    return impl_->add_handler(*this, std::move(route), std::move(handler));
}

void Server::listen() {
    impl_->listen();
}

void Server::run() {
    impl_->run();
}

void Server::serve_once() {
    impl_->serve_once();
}

void Server::stop() noexcept {
    if (impl_) {
        impl_->stop();
    }
}

bool Server::running() const noexcept {
    return impl_ && impl_->running();
}

const std::string& Server::socket_path() const noexcept {
    return impl_->socket_path();
}

Client::Client(std::string socket_path, Options options)
    : socket_path_(std::move(socket_path)), options_(options) {
    validate_options(options_);
    (void)make_address(socket_path_);
}

Response Client::request(std::string_view route, std::string_view body) const {
    if (route.empty()) {
        throw std::invalid_argument("route must not be empty");
    }
    if (route.size() > options_.max_route_bytes) {
        throw std::length_error("route exceeds max_route_bytes");
    }
    if (body.size() > options_.max_payload_bytes) {
        throw std::length_error("payload exceeds max_payload_bytes");
    }

    Socket socket = create_socket();
    set_timeout(socket.get(), options_.timeout);
    const auto address = make_address(socket_path_);
    if (::connect(socket.get(), reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0) {
        throw Error("connect", last_socket_error(), socket_path_);
    }

    send_message(socket.get(), WireMessage{request_type, 0, std::string(route), std::string(body)}, options_);
    const auto message = receive_message(socket.get(), options_);
    if (message.type != response_type) {
        throw std::runtime_error("server returned a non-response message");
    }
    return Response{static_cast<int>(message.status), message.body};
}

const std::string& Client::socket_path() const noexcept {
    return socket_path_;
}

} // namespace easy_uds
