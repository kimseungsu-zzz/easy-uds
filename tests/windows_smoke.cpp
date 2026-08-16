#include <easy_uds/easy_uds.hpp>
#include <easy_uds/simple.hpp>
#include "platform/windows/socket_common.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cerrno>
#include <cstring>
#include <exception>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <type_traits>
#include <vector>

static_assert(std::is_move_constructible_v<easy_uds::Request>);
static_assert(!std::is_copy_constructible_v<easy_uds::Request>);

namespace {

void require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

void remove_endpoint(const std::filesystem::path& path) {
    std::error_code ignored;
    std::filesystem::remove(path, ignored);
    std::filesystem::remove(path.string() + ".lock", ignored);
}

template <typename Server>
bool wait_until_running(Server& server) {
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::seconds(5);
    while (!server.is_running() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return server.is_running();
}

} // namespace

int main() {
    try {
        // Exercise the errno boundary directly for representative setup,
        // connection, and data-path failures.  The table is intentionally
        // tested separately from the RPC smoke so a new Winsock error cannot
        // silently leak a raw 100xx value into the common transport.
        require(easy_uds::detail::platform_windows::errno_from_wsa(WSAEFAULT) == EFAULT,
                "WSAEFAULT mapping failed");
        require(easy_uds::detail::platform_windows::errno_from_wsa(WSAEADDRNOTAVAIL) ==
                    EADDRNOTAVAIL,
                "WSAEADDRNOTAVAIL mapping failed");
        require(easy_uds::detail::platform_windows::errno_from_wsa(WSAENETUNREACH) ==
                    ENETUNREACH,
                "WSAENETUNREACH mapping failed");
        require(easy_uds::detail::platform_windows::errno_from_wsa(WSAESHUTDOWN) ==
                    EPIPE,
                "WSAESHUTDOWN mapping failed");
        require(easy_uds::detail::platform_windows::errno_from_wsa(WSAEMSGSIZE) == EMSGSIZE,
                "WSAEMSGSIZE mapping failed");
        require(easy_uds::detail::platform_windows::errno_from_wsa(0x7fffffff) == EIO,
                "unknown Winsock mapping failed");

        easy_uds::Request request;
        request.route = "/portable";
        request.body = "header-only";
        require(request.route == "/portable", "portable Request construction failed");

        const auto base = std::filesystem::temp_directory_path();
        const auto core_path = base / "easy-uds-windows-core.sock";
        remove_endpoint(core_path);

        easy_uds::Server server(core_path.string());
        server.on("/ping", [](const easy_uds::Request&) {
            return easy_uds::Response::ok("pong");
        });
        server.on("/echo", [](const easy_uds::Request& request) {
            return easy_uds::Response::ok(request.body);
        });
        server.on_stream("/stream", [](const easy_uds::StreamReader& body,
                                        const easy_uds::Request&) {
            std::array<char, 256> buffer{};
            std::size_t total = 0;
            while (true) {
                const std::size_t size = body(buffer.data(), buffer.size());
                if (size == 0) {
                    break;
                }
                total += size;
            }
            if (total != 8192) {
                return easy_uds::StreamResponse{400, {}};
            }
            return easy_uds::StreamResponse{
                206,
                [text = std::string("stream-ok"), offset = std::size_t{0}](
                    char* output, std::size_t capacity) mutable {
                    const std::size_t size = std::min(capacity, text.size() - offset);
                    if (size == 0) {
                        return std::size_t{0};
                    }
                    std::memcpy(output, text.data() + offset, size);
                    offset += size;
                    return size;
                }};
        });

        std::exception_ptr server_error;
        std::thread server_thread([&] {
            try {
                server.run();
            } catch (...) {
                server_error = std::current_exception();
            }
        });
        if (!wait_until_running(server)) {
            server.stop();
            server_thread.join();
            if (server_error) {
                std::rethrow_exception(server_error);
            }
            throw std::runtime_error("Windows server did not become ready");
        }

        easy_uds::Client client(core_path.string());
        require(client.request("/ping").body == "pong", "Windows one-shot RPC failed");
        require(client.request("/missing").status == easy_uds::status_not_found,
                "Windows 404 response failed");

        auto session = client.session();
        require(session.request("/ping").body == "pong", "Windows Session request failed");
        constexpr std::size_t concurrent_requests = 16;
        std::vector<std::thread> request_threads;
        std::vector<std::string> replies(concurrent_requests);
        std::vector<std::exception_ptr> request_errors(concurrent_requests);
        request_threads.reserve(concurrent_requests);
        for (std::size_t index = 0; index < concurrent_requests; ++index) {
            request_threads.emplace_back([&, index] {
                try {
                    const std::string body = "body-" + std::to_string(index);
                    replies[index] = session.request("/echo", body).body;
                } catch (...) {
                    request_errors[index] = std::current_exception();
                }
            });
        }
        for (auto& thread : request_threads) {
            thread.join();
        }
        for (std::size_t index = 0; index < concurrent_requests; ++index) {
            require(!request_errors[index], "Windows concurrent Session request threw");
            require(replies[index] == "body-" + std::to_string(index),
                    "Windows concurrent Session reply mismatch");
        }
        require(session.status() == easy_uds::SessionStatus::active,
                "Windows Session did not remain active");

        std::size_t upload_remaining = 8192;
        easy_uds::StreamReader upload = [&upload_remaining](char* output,
                                                            std::size_t capacity) {
            const std::size_t size = std::min(capacity, upload_remaining);
            std::memset(output, 'x', size);
            upload_remaining -= size;
            return size;
        };
        std::string stream_reply;
        const auto stream_status = client.request_stream(
            "/stream", upload,
            [&stream_reply](std::string_view chunk) { stream_reply += chunk; });
        require(stream_status == 206 && stream_reply == "stream-ok",
                "Windows streaming RPC failed");

        server.stop();
        server_thread.join();
        if (server_error) {
            std::rethrow_exception(server_error);
        }
        remove_endpoint(core_path);

        const auto simple_path = base / "easy-uds-windows-simple.sock";
        remove_endpoint(simple_path);
        easy_uds::simple::Server simple_server(simple_path.string());
        simple_server.on("/ping") = "pong";
        std::exception_ptr simple_error;
        std::thread simple_thread([&] {
            try {
                simple_server.run();
            } catch (...) {
                simple_error = std::current_exception();
            }
        });
        if (!wait_until_running(simple_server)) {
            simple_server.stop();
            simple_thread.join();
            if (simple_error) {
                std::rethrow_exception(simple_error);
            }
            throw std::runtime_error("Windows Simple server did not become ready");
        }
        easy_uds::simple::Client simple_client(simple_path.string());
        require(simple_client.request("/ping") == "pong",
                "Windows Simple API request failed");
        bool saw_response_error = false;
        try {
            (void)simple_client.request("/missing");
        } catch (const easy_uds::simple::ResponseError& error) {
            saw_response_error = true;
            require(error.status() == easy_uds::status_not_found,
                    "Windows Simple ResponseError status mismatch");
        }
        require(saw_response_error, "Windows Simple API did not report ResponseError");
        simple_server.stop();
        simple_thread.join();
        if (simple_error) {
            std::rethrow_exception(simple_error);
        }
        remove_endpoint(simple_path);

        // Repeated bind/run/stop exercises Windows pathname and wakeup
        // cleanup, including registry removal between server instances.
        const auto lifecycle_path = base / "easy-uds-windows-lifecycle.sock";
        for (int iteration = 0; iteration < 3; ++iteration) {
            remove_endpoint(lifecycle_path);
            easy_uds::Server lifecycle_server(lifecycle_path.string());
            lifecycle_server.on("/ping", [](const easy_uds::Request&) {
                return easy_uds::Response::ok("pong");
            });
            std::exception_ptr lifecycle_error;
            std::thread lifecycle_thread([&] {
                try {
                    lifecycle_server.run();
                } catch (...) {
                    lifecycle_error = std::current_exception();
                }
            });
            if (!wait_until_running(lifecycle_server)) {
                lifecycle_server.stop();
                lifecycle_thread.join();
                if (lifecycle_error) {
                    std::rethrow_exception(lifecycle_error);
                }
                throw std::runtime_error("Windows lifecycle server did not become ready");
            }
            require(easy_uds::Client(lifecycle_path.string()).request("/ping").body == "pong",
                    "Windows lifecycle request failed");
            lifecycle_server.stop();
            lifecycle_thread.join();
            if (lifecycle_error) {
                std::rethrow_exception(lifecycle_error);
            }
        }
        remove_endpoint(lifecycle_path);
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "windows_smoke: " << error.what() << '\\n';
        return 1;
    }
}
